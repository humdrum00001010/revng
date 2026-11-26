//
// This file is distributed under the MIT License. See LICENSE.md for details.
//

#include <fstream>
#include <iterator>
#include <memory>

#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/PostOrderIterator.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SetOperations.h"
#include "llvm/ADT/SmallSet.h"
#include "llvm/IR/GlobalVariable.h"
#include "llvm/IR/Instruction.h"
#include "llvm/IR/LegacyPassManager.h"
#include "llvm/IR/Module.h"
#include "llvm/Passes/PassBuilder.h"
#include "llvm/Support/GraphWriter.h"
#include "llvm/Transforms/Utils/Mem2Reg.h"

#include "revng/ABI/FunctionType/Conversion.h"
#include "revng/ABI/FunctionType/Layout.h"
#include "revng/ADT/Queue.h"
#include "revng/BasicAnalyses/CSVGlobals.h"
#include "revng/BasicAnalyses/RootFunction.h"
#include "revng/EarlyFunctionAnalysis/CFGAnalyzer.h"
#include "revng/EarlyFunctionAnalysis/CallEdge.h"
#include "revng/EarlyFunctionAnalysis/CallGraph.h"
#include "revng/EarlyFunctionAnalysis/ChainCSVWrites.h"
#include "revng/EarlyFunctionAnalysis/CollectFunctionsFromUnusedAddresses.h"
#include "revng/EarlyFunctionAnalysis/ControlFlowGraph.h"
#include "revng/EarlyFunctionAnalysis/ControlFlowGraphCache.h"
#include "revng/EarlyFunctionAnalysis/DetectABI.h"
#include "revng/EarlyFunctionAnalysis/FunctionEdgeBase.h"
#include "revng/EarlyFunctionAnalysis/FunctionSummaryOracle.h"
#include "revng/EarlyFunctionAnalysis/IgnorePreservedBits.h"
#include "revng/EarlyFunctionAnalysis/PromoteGlobalToLocalVars.h"
#include "revng/InlineHelpers/InlineHelpers.h"
#include "revng/InlineHelpers/LinkHelpersToInline.h"
#include "revng/Lift/JumpTargetReason.h"
#include "revng/Model/ABI/Definition.h"
#include "revng/Model/Binary.h"
#include "revng/Model/IRHelpers.h"
#include "revng/Model/Pass/DeduplicateCollidingNames.h"
#include "revng/Model/Register.h"
#include "revng/Support/BasicBlockID.h"
#include "revng/Support/CommonOptions.h"
#include "revng/Support/Debug.h"
#include "revng/Support/FunctionCallMarker.h"
#include "revng/Support/IRBuilder.h"
#include "revng/Support/IRHelpers.h"
#include "revng/Support/MetaAddress.h"
#include "revng/Support/NewPC.h"
#include "revng/Support/OpaqueRegisterUser.h"

using namespace llvm;
using namespace llvm::cl;

static opt<std::string> CallGraphOutputPath("cg-output",
                                            desc("Dump to disk the recovered "
                                                 "call graph."),
                                            value_desc("filename"));

enum ABIEnforcementOption {
  NoABIEnforcement = 0,
  SoftABIEnforcement,
  FullABIEnforcement,
};

using ABIOpt = ABIEnforcementOption;
static opt<ABIOpt> ABIEnforcement("abi-enforcement-level",
                                  desc("ABI refinement preferences."),
                                  values(clEnumValN(NoABIEnforcement,
                                                    "no",
                                                    "Do not enforce "
                                                    "ABI-specific "
                                                    "information."),
                                         clEnumValN(SoftABIEnforcement,
                                                    "soft",
                                                    "Enforce ABI-specific "
                                                    "information, but allows "
                                                    "for incompatibility, if "
                                                    "unsure about the ABI."),
                                         clEnumValN(FullABIEnforcement,
                                                    "full",
                                                    "Enforce ABI-specific "
                                                    "information, and do not "
                                                    "allow for any possible "
                                                    "violations of the ABI "
                                                    "found.")),
                                  init(ABIOpt::FullABIEnforcement));

static opt<std::string> DumpPostInline("detect-abi-dump-post-helpers-inlining",
                                       desc("Path of a file the whole "
                                            "module is written to once "
                                            "`analyzeABI` has prepared it, "
                                            "just before the register usage "
                                            "analysis reads it (for "
                                            "debugging and testing)"),
                                       init(""));

static Logger Log("detect-abi");

struct Changes {
  bool Function = false;
  std::set<MetaAddress> Callees;
};

namespace efa {

static model::Architecture::Values getCodeArchitecture(const MetaAddress &MA) {
  const auto Architecture = MetaAddressType::arch(MA.type());
  revng_assert(Architecture != model::Architecture::Invalid,
               "The architecture is available for code addresses");
  return Architecture;
}

static bool isWritingToMemory(llvm::Instruction &I) {
  if (auto *StoreInst = dyn_cast<llvm::StoreInst>(&I)) {
    auto *Destination = StoreInst->getPointerOperand();

    return isMemory(Destination);
  }

  return false;
}

using BasicBlockQueue = UniquedQueue<const BasicBlockNode *>;

class DetectABI {
private:
  using BasicBlockToNodeMap = llvm::DenseMap<llvm::BasicBlock *,
                                             BasicBlockNode *>;

private:
  llvm::Module &M;
  llvm::LLVMContext &Context;
  RootFunction &Root;
  const CSVGlobals &Globals;
  ControlFlowGraphCache &FMC;
  TupleTree<model::Binary> &Binary;
  FunctionSummaryOracle &Oracle;
  CFGAnalyzer &Analyzer;

  CallGraph ApproximateCallGraph;
  BasicBlockToNodeMap BasicBlockNodeMap;

  /// Whether each function that has been looked at saves its stack pointer
  /// in its frame, see CFGAnalyzer::savesStackPointer
  std::map<MetaAddress, bool> SavesStackPointer;

public:
  DetectABI(llvm::Module &M,
            RootFunction &Root,
            const CSVGlobals &Globals,
            ControlFlowGraphCache &FMC,
            TupleTree<model::Binary> &Binary,
            FunctionSummaryOracle &Oracle,
            CFGAnalyzer &Analyzer) :
    M(M),
    Context(M.getContext()),
    Root(Root),
    Globals(Globals),
    FMC(FMC),
    Binary(Binary),
    Oracle(Oracle),
    Analyzer(Analyzer) {}

public:
  void run() {
    llvm::Task Task(6, "DetectABI");
    Task.advance("computeApproximateCallGraph");
    computeApproximateCallGraph();

    // Perform a preliminary analysis of the function
    //
    // We're interested:
    //
    // 1. the function being noreturn or not;
    // 2. the list of callee-saved registers;
    // 3. the final stack offset;
    // 4. the CFG (specifically, which indirect jumps are returns);
    Task.advance("preliminaryFunctionAnalysis");
    preliminaryFunctionAnalysis();

    // Run the (fixed-point) analysis of the ABI of each function
    Task.advance("analyzeABI");
    analyzeABI();

    // Refine results with ABI-specific information
    Task.advance("applyABIDeductions");
    applyABIDeductions();

    // Commit the results onto the model. A non-const model is taken as
    // argument to be written.
    Task.advance("finalizeModel");
    finalizeModel();

    // Propagate prototypes
    Task.advance("propagatePrototypes");
    propagatePrototypes();
  }

  /// Code no function reaches, right after a block of a function, is most
  /// often where that function goes on after an exception is handled.
  /// If the function, analyzed as being resumed there, still makes sense, the
  /// code is its own; otherwise it is left to become a function.
  ///
  /// To be called on an object of its own, made after `run` has written the
  /// model: the functions are analyzed again with what the model says about
  /// them and their call sites, as every later step does.
  void attachResumeBlocks();

  /// Offer to the functions found before them the functions that were made
  /// since \p Before was taken and that do not leave as functions do. To be
  /// called as attachResumeBlocks.
  void reattachFunctions(const std::set<MetaAddress> &Before);

  /// Where a function has something left unexplained, see whether a caller
  /// that left out the code that removes the arguments of a call explains
  /// it, and say so at that call site. To be called as attachResumeBlocks.
  ///
  /// \return the functions that turn out to leave in another way than the
  ///         model says: those that return after all, or remove another
  ///         amount from the stack. What the model says about them, and
  ///         about the functions that call them, was found with that.
  std::vector<MetaAddress> inferElidedCleanups();

  /// Make the model forget what detect-abi found about \p Functions and
  /// about the functions that call them, directly or not: their prototype,
  /// their attributes and what it said about their call sites, except the
  /// clean-ups left out. They are analyzed again by the next \ref run.
  void forgetWithCallers(const std::vector<MetaAddress> &Functions);

private:
  void computeApproximateCallGraph();
  void preliminaryFunctionAnalysis();

  bool tryToAttach(model::Function &Function, const MetaAddress &Address);

  /// Whose the code is
  struct Ownership {
    /// The functions each block belongs to, by where the block starts
    std::map<MetaAddress, llvm::SmallVector<MetaAddress, 1>> Owners;

    /// Where the last block of each function starts
    std::map<MetaAddress, MetaAddress> LastBlock;

    /// Where each block ends, by where it starts
    std::map<MetaAddress, MetaAddress> End;

    /// How long the longest block is: no block that starts further before an
    /// address than this contains it
    uint64_t LongestBlock = 0;

    /// The code no function has: where a function that has not been found
    /// yet can be
    std::set<MetaAddress> Unowned;

    /// Whether what follows a function may be offered to more than the
    /// nearest function that can be resumed
    bool OnlyNearest = false;
  };
  Ownership
  ownersOfBlocks(llvm::function_ref<bool(const model::Function &)> IsOwner);
  void addBlocksOf(Ownership &Whose, const MetaAddress &Function);
  bool offer(const MetaAddress &Candidate, Ownership &Whose);
  void analyzeABI();
  Changes analyzeFunctionABI(const model::Function &Function,
                             OutlinedFunction &OutlinedFunction,
                             OpaqueRegisterUser &Clobberer);
  void applyABIDeductions();

  /// Finish the population of the model by building the prototype
  void finalizeModel();

  // Calls propagatePrototypesInFunction for all functions in Binary
  void propagatePrototypes();

  /// Propagate prototypes to callers
  /// \param NamesAreDeduplicated whether the names in the model have been
  ///        deduplicated and nothing has changed since
  void propagatePrototypesInFunction(model::Function &Function,
                                     bool &NamesAreDeduplicated);

private:
  /// Record in \p Inserter the registers whose lowest CSV is in \p CSVs.
  void recordRegisters(const efa::CSVSet &CSVs, auto Inserter);

  CSVSet computePreservedCSVs(const CSVSet &ClobberedRegisters) const;

  TrackingSortedVector<model::Register::Values>
  computePreservedRegisters(const CSVSet &ClobberedRegisters) const;

  Changes runAnalyses(MetaAddress EntryAddress,
                      OutlinedFunction &OutlinedFunction);

  CSVSet findWrittenRegisters(llvm::Function *F);

  model::UpcastableType
  buildPrototypeForIndirectCall(const FunctionSummary &CallerSummary,
                                const efa::BasicBlock &CallerBlock);

  /// \return the default prototype with \p FinalStackOffset as its final stack
  ///         offset, or an empty type if that is the one it has
  model::UpcastableType defaultPrototypeRemoving(int64_t FinalStackOffset);

  /// A callee that removes more from the stack than its return address
  /// removes its arguments: they are those bytes. Give \p Prototype, which
  /// removes \p FinalStackOffset where a callee that leaves its arguments
  /// removes \p DefaultOffset, the stack arguments that says it has.
  void setRemovedStackArguments(model::RawFunctionDefinition &Prototype,
                                int64_t FinalStackOffset,
                                int64_t DefaultOffset);

private:
  /// The copies of the default prototype \ref defaultPrototypeRemoving made,
  /// by their final stack offset
  std::map<int64_t, model::TypeDefinition::Key> DefaultPrototypeCopies;
};

void DetectABI::computeApproximateCallGraph() {
  using llvm::BasicBlock;

  // Temporary worklist to collect the function entrypoints
  llvm::SmallVector<BasicBlock *, 8> Worklist;

  // Create an over-approximated call graph
  for (const auto &Function : Binary->Functions()) {
    auto *Entry = Root.getBlockAt(Function.Entry());
    BasicBlockNode Node{ Function.Entry() };
    BasicBlockNode *GraphNode = ApproximateCallGraph.addNode(Node);
    BasicBlockNodeMap[Entry] = GraphNode;
  }

  for (const auto &Function : Binary->Functions()) {
    llvm::SmallSet<BasicBlock *, 8> Visited;
    auto *Entry = Root.getBlockAt(Function.Entry());
    revng_assert(Entry != nullptr);

    BasicBlockNode *StartNode = BasicBlockNodeMap[Entry];
    revng_assert(StartNode != nullptr);
    Worklist.emplace_back(Entry);

    while (!Worklist.empty()) {
      BasicBlock *Current = Worklist.pop_back_val();
      Visited.insert(Current);

      if (hasMarker(Current, FunctionCallMarker)) {
        // If not an indirect call, add the node to the CG
        if (BasicBlock *Callee = getOnlyCallee(Current, *Binary)) {
          BasicBlockNode *GraphNode = nullptr;
          auto It = BasicBlockNodeMap.find(Callee);
          if (It != BasicBlockNodeMap.end()) {
            auto IsCurrentSuccessor = [&It](auto &Successor) {
              return Successor == It->second;
            };
            if (not any_of(StartNode->successors(), IsCurrentSuccessor))
              StartNode->addSuccessor(It->second);
          }
        }
        BasicBlock *Next = getFallthrough(Current);
        revng_assert(Next != nullptr);

        if (!Visited.contains(Next))
          Worklist.push_back(Next);

      } else {

        for (BasicBlock *Successor : successors(Current)) {
          if (not isPartOfRootDispatcher(Successor)
              && !Visited.contains(Successor)) {
            revng_assert(Successor != nullptr);
            Worklist.push_back(Successor);
          }
        }
      }
    }
  }

  // Create a root entry node for the call-graph, connect all the nodes to it,
  // and perform a post-order traversal. Keep in mind that adding a root node
  // as a predecessor to all nodes does not affect POT of any node, except the
  // root node itself.
  BasicBlockNode *RootNode = ApproximateCallGraph.addNode(MetaAddress());
  ApproximateCallGraph.setEntryNode(RootNode);

  // The order of the successors of the root node is the order in which the
  // functions are analyzed: follow the model, not the addresses of the blocks
  for (const auto &Function : Binary->Functions()) {
    auto *Entry = Root.getBlockAt(Function.Entry());
    RootNode->addSuccessor(BasicBlockNodeMap[Entry]);
  }

  // Dump the call-graph, if requested
  if (CallGraphOutputPath.getNumOccurrences() == 1) {
    std::error_code EC;
    raw_fd_ostream OutputCG(CallGraphOutputPath, EC);
    revng_assert(!EC);
    llvm::WriteGraph(OutputCG, &ApproximateCallGraph);
  }
}

/// The code at \p Entry is what another function is resumed at: forget the
/// function that was made of it. (A function of its own: what is dropped can
/// be watched from a debugger.)
[[gnu::noinline]] static void
dropFunctionNowPartOfAnother(model::Binary &Binary, const MetaAddress &Entry) {
  Binary.Functions().erase(Entry);
}

void DetectABI::forgetWithCallers(const std::vector<MetaAddress> &Functions) {
  // Who calls whom, from the blocks of the functions
  std::map<MetaAddress, std::set<MetaAddress>> Callers;
  for (const model::Function &Function : Binary->Functions())
    for (const efa::BasicBlock &Block :
         FMC.getControlFlowGraph(Function.Entry()).Blocks())
      for (const auto &Edge : Block.Successors())
        if (const auto *Call = llvm::dyn_cast<efa::CallEdge>(Edge.get()))
          if (Call->Destination().isValid())
            Callers[Call->Destination().notInlinedAddress()]
              .insert(Function.Entry());

  OnceQueue<MetaAddress> Queue;
  for (const MetaAddress &Entry : Functions)
    Queue.insert(Entry);
  while (not Queue.empty()) {
    MetaAddress Entry = Queue.pop();
    auto It = Callers.find(Entry);
    if (It != Callers.end())
      for (const MetaAddress &Caller : It->second)
        Queue.insert(Caller);
  }

  for (const MetaAddress &Entry : Queue.visited()) {
    model::Function &Function = Binary->Functions().at(Entry);
    Function.Prototype() = model::UpcastableType();
    Function.Attributes().erase(model::FunctionAttribute::NoReturn);
    Function.Comments().clear();

    std::vector<MetaAddress> ToForget;
    for (const model::CallSitePrototype &CallSite :
         Function.CallSitePrototypes())
      if (CallSite.ElidedStackCleanup() == 0)
        ToForget.push_back(CallSite.CallerBlockAddress());
    for (const MetaAddress &Block : ToForget)
      Function.CallSitePrototypes().erase(Block);
  }
}

std::vector<MetaAddress> DetectABI::inferElidedCleanups() {
  std::vector<MetaAddress> Changed;
  for (model::Function &Function : Binary->Functions()) {
    if (Function.Prototype().isEmpty())
      continue;

    // Only the functions with something left to explain: a jump to nowhere,
    // or a block where the height of the stack is not known
    MetaAddress Entry = Function.Entry();
    bool HasLeftover = false;
    for (const efa::BasicBlock &Block :
         FMC.getControlFlowGraph(Entry).Blocks()) {
      if (not Block.ID().isInlined() and Block.ID().start() != Entry
          and not Block.HasStackDepth())
        HasLeftover = true;
      for (const auto &Edge : Block.Successors())
        if (Edge->Type() == efa::FunctionEdgeType::LongJmp)
          HasLeftover = true;
    }
    if (not HasLeftover)
      continue;

    std::map<MetaAddress, uint64_t> Found;
    auto New = Analyzer.analyzeElidedCleanups(Entry, Found);
    if (not New.has_value())
      continue;

    for (const efa::BasicBlock &Block : New->CFG) {
      if (Block.ID().isInlined())
        continue;
      MetaAddress BlockAddress = Block.ID().notInlinedAddress();
      auto FoundIt = Found.find(BlockAddress);
      if (FoundIt == Found.end())
        continue;

      for (const auto &Edge : Block.Successors()) {
        const auto *CE = llvm::dyn_cast<efa::CallEdge>(Edge.get());
        if (CE == nullptr)
          continue;

        // The call site is the callee's, with what the caller left out
        const model::TypeDefinition *CalleePrototype = nullptr;
        std::vector<model::FunctionAttribute::Values> Attributes;
        if (CE->Destination().isValid()) {
          MetaAddress Callee = CE->Destination().notInlinedAddress();
          const auto &Local = Binary->Functions().at(Callee);
          CalleePrototype = Local.prototype();
          for (auto Attribute : Local.Attributes())
            Attributes.push_back(Attribute);
        } else if (not CE->DynamicFunction().empty()) {
          const auto &Imported = Binary->ImportedDynamicFunctions()
                                   .at(CE->DynamicFunction());
          CalleePrototype = Imported.prototype();
          for (auto Attribute : Imported.Attributes())
            Attributes.push_back(Attribute);
        }

        if (CalleePrototype == nullptr
            or Function.CallSitePrototypes().contains(BlockAddress))
          continue;

        auto It = Function.CallSitePrototypes()
                    .emplace(BlockAddress,
                             Binary->makeType(CalleePrototype->key()),
                             false)
                    .first;
        It->ElidedStackCleanup() = FoundIt->second;
        for (auto Attribute : Attributes)
          It->Attributes().insert(Attribute);
      }
    }

    // Does the function leave as the model says it does?
    bool Returns = false;
    for (const efa::BasicBlock &Block : New->CFG)
      for (const auto &Edge : Block.Successors())
        if (Edge->Type() == efa::FunctionEdgeType::Return)
          Returns = true;
    using model::FunctionAttribute::NoReturn;
    uint64_t Removes = abi::FunctionType::finalStackOffset(*Function
                                                              .prototype());
    if ((Returns and Function.Attributes().contains(NoReturn))
        or (Returns and New->ElectedFSO.has_value()
            and uint64_t(*New->ElectedFSO) != Removes))
      Changed.push_back(Entry);

    TupleTree<efa::FunctionBundle> Bundle;
    efa::ControlFlowGraph &Main = Bundle->MainFunction();
    Main.Entry() = Entry;
    Main.Blocks() = New->CFG;
    Main.simplify(*Binary);
    FMC.set(std::move(Bundle));
  }

  return Changed;
}

void DetectABI::attachResumeBlocks() {
  std::vector<MetaAddress> Candidates = unusedCodeAddresses(M,
                                                            Root,
                                                            *Binary,
                                                            FMC);
  if (Candidates.empty())
    return;

  Ownership Whose = ownersOfBlocks([](const model::Function &) {
    return true;
  });
  Whose.Unowned.insert(Candidates.begin(), Candidates.end());
  for (const MetaAddress &Candidate : Candidates)
    offer(Candidate, Whose);
}

void DetectABI::addBlocksOf(Ownership &Whose, const MetaAddress &Function) {
  for (const efa::BasicBlock &Block :
       FMC.getControlFlowGraph(Function).Blocks()) {
    if (Block.ID().isInlined())
      continue;
    auto &List = Whose.Owners[Block.ID().start()];
    if (not llvm::is_contained(List, Function))
      List.push_back(Function);
    auto [EndIt, IsNewBlock] = Whose.End.try_emplace(Block.ID().start(),
                                                     Block.End());
    if (not IsNewBlock and EndIt->second < Block.End())
      EndIt->second = Block.End();
    if (std::optional<uint64_t> Length = Block.End() - Block.ID().start())
      Whose.LongestBlock = std::max(Whose.LongestBlock, *Length);
    auto [It, IsNew] = Whose.LastBlock.try_emplace(Function,
                                                   Block.ID().start());
    if (not IsNew and It->second < Block.ID().start())
      It->second = Block.ID().start();
  }
}

DetectABI::Ownership
DetectABI::ownersOfBlocks(llvm::function_ref<bool(const model::Function &)>
                            IsOwner) {
  Ownership Whose;
  for (const model::Function &Function : Binary->Functions())
    if (IsOwner(Function))
      addBlocksOf(Whose, Function.Entry());
  return Whose;
}

bool DetectABI::offer(const MetaAddress &Candidate, Ownership &Whose) {
  // Code nothing jumps to is offered to the functions it leads into, if
  // that is known, or else to the function it lies in (between two of its
  // blocks) or follows: whether it is its own is for tryToAttach to say.
  // Between a function and the code it goes on with there can be the
  // handlers the function is resumed from, which are functions of their own
  // and cannot be resumed: they are looked past, up to a point.
  //
  // tryToAttach cannot tell a function from another one with the same frame:
  // what it accepts is right only for the function the code belongs to. So
  // code that follows a function is not offered across code no function has
  // (the function it belongs to may not have been found yet), and where
  // Whose says so, not past a function that can be resumed and refused it.
  //
  // A block several functions reach (each of the functions that jump to a
  // function has its code as their own) is resumed in all of them or in
  // none: the code is offered to each, and stays attached only if every one
  // of them takes it.
  constexpr unsigned MostOwners = 4;
  constexpr unsigned MostSeen = 32;
  constexpr unsigned MostSharing = 8;

  // (What has become a block of a function since is that function's)
  if (Whose.Owners.contains(Candidate))
    return false;

  // Only a function that saves its stack pointer for whoever resumes it
  // can be resumed: looking for that is much cheaper than trying
  using OwnerList = llvm::SmallVector<MetaAddress, 1>;
  auto AllSave = [this](const OwnerList &Sharing) {
    bool Result = true;
    for (const MetaAddress &Owner : Sharing) {
      auto [SavesIt, IsNew] = SavesStackPointer.try_emplace(Owner, false);
      if (IsNew)
        SavesIt->second = Analyzer.savesStackPointer(Owner);
      Result = Result and SavesIt->second;
    }
    return Result;
  };

  // The code becomes that of all the functions of \p Sharing, or of none
  auto Attach = [this, &Candidate, &Whose](const OwnerList &Sharing) {
    // What each of them was, to go back to if one of them refuses
    using Blocks = SortedVector<efa::BasicBlock>;
    llvm::SmallVector<std::pair<MetaAddress, Blocks>, 1> Attached;
    bool AllTake = true;
    for (const MetaAddress &Owner : Sharing) {
      Blocks Old = FMC.getControlFlowGraph(Owner).Blocks();
      if (not tryToAttach(Binary->Functions().at(Owner), Candidate)) {
        AllTake = false;
        break;
      }
      Attached.emplace_back(Owner, std::move(Old));
    }

    if (AllTake) {
      // The code is theirs now
      Whose.Unowned.erase(Candidate);
      for (const MetaAddress &Owner : Sharing)
        addBlocksOf(Whose, Owner);
      return true;
    }

    for (auto &[Owner, Old] : Attached) {
      Binary->Functions().at(Owner).ResumeAddresses().erase(Candidate);
      TupleTree<efa::FunctionBundle> Bundle;
      efa::ControlFlowGraph &Main = Bundle->MainFunction();
      Main.Entry() = Owner;
      Main.Blocks() = std::move(Old);
      FMC.set(std::move(Bundle));
    }

    return false;
  };

  // Where the code goes says whose it is, and says it better than where it
  // lies: code that was put away from its function, as what is seldom run
  // is, leads back into it. A candidate that was made a function has blocks:
  // the first of them that the functions of Whose have too is where it joins
  // them, and they are asked first. (If they refuse, the functions around it
  // are still asked, as before: nothing that was taken stops being taken.)
  if (Binary->Functions().contains(Candidate)) {
    const auto &Blocks = FMC.getControlFlowGraph(Candidate).Blocks();
    OwnerList Into;
    OnceQueue<BasicBlockID> Queue;
    Queue.insert(BasicBlockID(Candidate));
    while (Into.empty() and not Queue.empty()) {
      auto It = Blocks.find(Queue.pop());
      if (It == Blocks.end())
        continue;

      // The blocks of theirs that start there, or that the address is in:
      // nothing in the function jumps to where it goes on, so that place is
      // most often in the middle of one of its blocks. Functions that share
      // code do not cut it into the same blocks: all the blocks the address
      // is in are looked at, and the code is offered to the functions of
      // all of them.
      MetaAddress Start = It->ID().start();
      auto Owned = Whose.Owners.upper_bound(Start);
      while (Start != Candidate and Owned != Whose.Owners.begin()) {
        --Owned;
        std::optional<uint64_t> Before = Start - Owned->first;
        if (not Before.has_value() or *Before > Whose.LongestBlock)
          break;

        auto EndIt = Whose.End.find(Owned->first);
        bool IsInside = Owned->first == Start
                        or (EndIt != Whose.End.end() and Start < EndIt->second);
        if (not IsInside)
          continue;

        for (const MetaAddress &Owner : Owned->second)
          if (Owner != Candidate and not llvm::is_contained(Into, Owner))
            Into.push_back(Owner);
      }
      if (not Into.empty())
        break;

      for (const auto &Edge : It->Successors())
        if (Edge->Type() == efa::FunctionEdgeType::DirectBranch
            and not Edge->Destination().isInlined())
          Queue.insert(Edge->Destination());
    }

    if (not Into.empty() and Into.size() <= MostSharing and AllSave(Into)
        and Attach(Into))
      return true;
  }

  MetaAddress Gap = MetaAddress::invalid();
  if (auto It = Whose.Unowned.lower_bound(Candidate);
      It != Whose.Unowned.begin())
    Gap = *std::prev(It);

  llvm::SmallVector<MetaAddress, MostSeen> Seen;
  unsigned Tried = 0;
  bool PastNearest = false;
  auto It = Whose.Owners.upper_bound(Candidate);
  while (It != Whose.Owners.begin() and Tried < MostOwners
         and Seen.size() < MostSeen) {
    --It;
    const llvm::SmallVector<MetaAddress, 1> Sharing = It->second;
    if (It->first == Candidate or Sharing.empty()
        or Sharing.size() > MostSharing
        or llvm::is_contained(Sharing, Candidate)
        or llvm::is_contained(Seen, Sharing.front()))
      continue;

    Seen.push_back(Sharing.front());

    // Does the code lie between two blocks of the function?
    bool IsInside = false;
    for (const MetaAddress &Owner : Sharing) {
      auto Last = Whose.LastBlock.find(Owner);
      IsInside = IsInside
                 or (Last != Whose.LastBlock.end()
                     and Candidate < Last->second);
    }

    bool IsBehindGap = Gap.isValid() and It->first < Gap;
    if (not IsInside and (IsBehindGap or PastNearest))
      continue;

    if (not AllSave(Sharing))
      continue;

    ++Tried;
    if (not IsInside and Whose.OnlyNearest)
      PastNearest = true;

    if (Attach(Sharing))
      return true;
  }

  return false;
}

void DetectABI::reattachFunctions(const std::set<MetaAddress> &Before) {
  // A function made from code nothing jumps to, that does not leave as a
  // function does, may be what a function that was found at the same time is
  // resumed at: the first time around its function was not there to be
  // offered it. If the function takes it now, it is not a function.
  auto LeavesProperly = [this](const model::Function &Function) {
    if (not Function.Attributes().empty())
      return false;

    bool Returns = false;
    for (const efa::BasicBlock &Block :
         FMC.getControlFlowGraph(Function.Entry()).Blocks()) {
      for (const auto &Edge : Block.Successors()) {
        using namespace efa::FunctionEdgeType;
        const auto *Call = llvm::dyn_cast<efa::CallEdge>(Edge.get());
        if (Edge->Type() == Return
            or (Call != nullptr and Edge->Type() == FunctionCall
                and Call->IsTailCall()))
          Returns = true;
        else if (Edge->Type() != DirectBranch and Edge->Type() != FunctionCall)
          return false;
      }
    }
    return Returns;
  };

  std::vector<MetaAddress> Candidates;
  for (const model::Function &Function : Binary->Functions())
    if (not Before.contains(Function.Entry()) and not LeavesProperly(Function))
      Candidates.push_back(Function.Entry());

  // (The functions are in the order of their addresses, so are these)
  Ownership Whose = ownersOfBlocks([&](const model::Function &Function) {
    return not std::binary_search(Candidates.begin(),
                                  Candidates.end(),
                                  Function.Entry());
  });

  // All the functions there are have been found: what is between a function
  // and the code it goes on with is its handlers, whatever they look like.
  // But the nearest function that can be resumed is the one, or none is.
  Whose.OnlyNearest = true;

  // One that starts where a block of a function found before it starts is
  // code of that function already: it only looked unused when the functions
  // were less well known
  for (const MetaAddress &Candidate : Candidates)
    if (Whose.Owners.contains(Candidate) or offer(Candidate, Whose))
      dropFunctionNowPartOfAnother(*Binary, Candidate);
}

bool DetectABI::tryToAttach(model::Function &Function,
                            const MetaAddress &Address) {
  MetaAddress Entry = Function.Entry();
  auto Old = FMC.getControlFlowGraph(Entry).Blocks();
  if (Function.Prototype().isEmpty() or not Function.Attributes().empty()
      or Old.contains(BasicBlockID(Address)))
    return false;

  Function.ResumeAddresses().insert(Address);
  FunctionSummary New = Analyzer.tryOut(Entry);

  // The function has to be what it was, plus code that leads back into it
  // or returns as it does: every block it had leaves it as it did (a return
  // that removes another amount from the stack would turn the returns of the
  // function into something else), and it removes what it removed
  auto ExitsOf = [](const efa::BasicBlock &Block) {
    llvm::SmallVector<efa::FunctionEdgeType::Values, 2> Result;
    for (const auto &Edge : Block.Successors())
      if (Edge->Type() != efa::FunctionEdgeType::DirectBranch)
        Result.push_back(Edge->Type());
    llvm::sort(Result);
    return Result;
  };

  // (Where the function goes on can be in the middle of one of its blocks,
  // which is then two: what counts is how the code of the old block leaves)
  auto ExitsWithin = [&New](const efa::BasicBlock &Old) {
    llvm::SmallVector<efa::FunctionEdgeType::Values, 2> Result;
    for (const efa::BasicBlock &Block : New.CFG) {
      if (Block.ID().isInlined() or Block.ID().start() < Old.ID().start()
          or not(Block.ID().start() < Old.End()))
        continue;
      for (const auto &Edge : Block.Successors())
        if (Edge->Type() != efa::FunctionEdgeType::DirectBranch)
          Result.push_back(Edge->Type());
    }
    llvm::sort(Result);
    return Result;
  };

  bool IsFine = New.CFG.contains(BasicBlockID(Address));
  for (const efa::BasicBlock &Block : Old) {
    auto It = New.CFG.find(Block.ID());
    if (It == New.CFG.end()) {
      IsFine = false;
    } else if (Block.ID().isInlined()) {
      IsFine = IsFine and ExitsOf(*It) == ExitsOf(Block);
    } else {
      IsFine = IsFine and ExitsWithin(Block) == ExitsOf(Block);
    }
  }

  uint64_t OldFSO = abi::FunctionType::finalStackOffset(*Function.prototype());
  IsFine = IsFine and New.ElectedFSO.has_value()
           and uint64_t(*New.ElectedFSO) == OldFSO;

  for (const efa::BasicBlock &Block : New.CFG) {
    if (Old.contains(Block.ID()))
      continue;

    for (const auto &Edge : Block.Successors()) {
      using namespace efa::FunctionEdgeType;
      const auto *Call = llvm::dyn_cast<efa::CallEdge>(Edge.get());
      bool IsPlainCall = Call != nullptr and not Call->IsTailCall();
      if (Edge->Type() != DirectBranch and Edge->Type() != Return
          and not IsPlainCall)
        IsFine = false;
    }
  }

  if (not IsFine) {
    Function.ResumeAddresses().erase(Address);
    return false;
  }

  {
    TupleTree<efa::FunctionBundle> Bundle;
    efa::ControlFlowGraph &Main = Bundle->MainFunction();
    Main.Entry() = Entry;
    Main.Blocks() = New.CFG;
    Main.simplify(*Binary);
    FMC.set(std::move(Bundle));
  }

  return true;
}

void DetectABI::preliminaryFunctionAnalysis() {
  revng_log(Log, "Running the preliminary function analysis");
  LoggerIndent LodIndent(Log);

  BasicBlockQueue EntrypointsQueue;

  //
  // Populate queue of entry points
  //

  // Create an over-approximated call graph of the program. A queue of all the
  // function entrypoints is maintained.
  for (auto *Node : llvm::post_order(&ApproximateCallGraph)) {
    // Ignore entry node
    if (Node == ApproximateCallGraph.getEntryNode())
      continue;

    // The intraprocedural analysis will be scheduled only for those functions
    // which have `Invalid` as type.
    auto &Function = Binary->Functions().at(Node->Address);
    EntrypointsQueue.insert(Node);
  }

  revng_assert(Binary->Functions().size() == EntrypointsQueue.size());

  //
  // Process the queue
  //

  while (!EntrypointsQueue.empty()) {
    const BasicBlockNode *EntryNode = EntrypointsQueue.pop();
    MetaAddress EntryPointAddress = EntryNode->Address;
    revng_log(Log, "Analyzing " << EntryPointAddress.toString());
    LoggerIndent Indent(Log);

    FunctionSummary AnalysisResult = Analyzer.analyze(EntryNode->Address);

    if (Log.isEnabled()) {
      AnalysisResult.dump(Log);
      Log << DoLog;
    }

    // Serialize CFG in the ControlFlowGraphCache
    {
      TupleTree<efa::FunctionBundle> New;
      efa::ControlFlowGraph &Main = New->MainFunction();
      Main.Entry() = EntryNode->Address;
      Main.Blocks() = AnalysisResult.CFG;
      Main.simplify(*Binary);
      FMC.set(std::move(New));
    }

    // Perform some early sanity checks once the CFG is ready
    revng_assert(AnalysisResult.CFG.size() > 0);

    if (not Binary->Functions()[EntryPointAddress].Prototype().isEmpty()) {
      Oracle.getLocalFunction(EntryPointAddress)
        .CFG = std::move(AnalysisResult.CFG);
      continue;
    }

    bool Changed = Oracle.registerLocalFunction(EntryPointAddress,
                                                std::move(AnalysisResult));

    revng_assert(Oracle.getLocalFunction(EntryPointAddress).CFG.size() > 0);

    // If we got improved results for a function, we need to recompute its
    // callers, and if a caller turns out to be an inline function, the
    // callers of the inline function too.
    if (Changed) {
      revng_log(Log,
                "Entry " << EntryPointAddress.toString() << " has changed");
      LoggerIndent Indent(Log);
      OnceQueue<const BasicBlockNode *> InlineFunctionWorklist;
      InlineFunctionWorklist.insert(EntryNode);

      while (!InlineFunctionWorklist.empty()) {
        const BasicBlockNode *Node = InlineFunctionWorklist.pop();
        MetaAddress NodeAddress = Node->Address;
        revng_log(Log,
                  "Re-enqueuing callers of " << NodeAddress.toString() << ":");
        LoggerIndent Indent(Log);
        for (auto *Caller : Node->predecessors()) {
          // Root node?
          if (Caller->Address.isInvalid())
            continue;

          // If it's inline, re-enqueue its callers too
          MetaAddress CallerPC = Caller->Address;
          const auto &CallerSummary = Oracle.getLocalFunction(CallerPC);
          using namespace model::FunctionAttribute;
          if (CallerSummary.Attributes.contains(HasOneBrokenReturn))
            InlineFunctionWorklist.insert(Caller);

          if (Binary->Functions().at(CallerPC).Prototype().isEmpty()) {
            revng_log(Log, CallerPC.toString());
            EntrypointsQueue.insert(Caller);
          }
        }
      }
    }
  }

  // Register all indirect call sites in all functions, we'll need to fill in
  // their prototypes
  for (auto &Function : Binary->Functions()) {
    auto &CFG = Oracle.getLocalFunction(Function.Entry()).CFG;
    revng_log(Log,
              "Registering indirect call sites of "
                << Function.Entry().toString());
    for (efa::BasicBlock &Block : CFG) {
      for (auto &Edge : Block.Successors()) {
        if (auto *Call = dyn_cast<CallEdge>(Edge.get())) {
          if (Call->isIndirect()) {
            revng_log(Log,
                      "Registering " << Function.Entry().toString() << " "
                                     << Block.ID().toString());
            Oracle.registerCallSite(Function.Entry(),
                                    Block.ID(),
                                    FunctionSummary(),
                                    Call->IsTailCall());
          }
        }
      }
    }
  }
}

namespace {

/// Run a function pipeline on the outlined stubs, and on nothing else.
///
/// The module still holds `root` and the helpers. The analysis never looks at
/// them and the rest of the pipeline needs them intact, so they are left alone.
/// `InlineHelpersPass` picks the functions it inlines into the same way, and by
/// this point the stubs are the only thing wearing the tag.
class IsolatedOnlyPass : public llvm::PassInfoMixin<IsolatedOnlyPass> {
private:
  llvm::FunctionPassManager FPM;

public:
  explicit IsolatedOnlyPass(llvm::FunctionPassManager &&FPM) :
    FPM(std::move(FPM)) {}

public:
  llvm::PreservedAnalyses run(llvm::Module &M,
                              llvm::ModuleAnalysisManager &MAM) {
    using namespace llvm;
    auto &FAM = MAM.getResult<FunctionAnalysisManagerModuleProxy>(M)
                  .getManager();

    for (Function &F : M)
      if (FunctionTags::Isolated.isTagOf(&F))
        FPM.run(F, FAM);

    return PreservedAnalyses::none();
  }
};

} // namespace

/// Inline the helpers, then say what the CSVs are for and let the standard
/// machinery simplify the ones the analysis ignores.
///
/// While a CSV is a global, no pass may drop a store to it: the caller or a
/// callee could be watching. As a local it is ordinary memory, and `mem2reg`
/// both deletes the stores nobody reads and turns the ones somebody does read
/// into plain SSA edges.
static void prepareFunctions(llvm::Module &M,
                             model::Architecture::Values Architecture) {
  using namespace llvm;

  LoopAnalysisManager LAM;
  FunctionAnalysisManager FAM;
  CGSCCAnalysisManager CGAM;
  ModuleAnalysisManager MAM;

  PassBuilder PB;
  PB.registerModuleAnalyses(MAM);
  PB.registerCGSCCAnalyses(CGAM);
  PB.registerFunctionAnalyses(FAM);
  PB.registerLoopAnalyses(LAM);
  PB.crossRegisterProxies(LAM, FAM, CGAM, MAM);

  FunctionPassManager FPM;

  // A sub-register write reads the register back in order to preserve what it
  // does not touch, which makes it look read-before-written. Mark that merge
  // before anything else moves the value it reads, or the register is reported
  // as an argument.
  FPM.addPass(IgnorePreservedBitsPass());

  // Give the copy of a value the CSV the original went to, while both stores
  // are still there to be seen.
  SmallVector<WeakVH> Loads;
  FPM.addPass(ChainCSVWritesPass(Loads));

  // `mem2reg` rather than SROA: the CSVs are scalars, so promotion is all that
  // is needed, and unlike SROA it cannot touch the CFG. The call sites the
  // analysis reports are keyed by basic block, and the CFG they are matched
  // against was recorded before this point.
  auto IsNotARegister = [Architecture](const llvm::GlobalVariable &CSV) {
    auto Register = model::Register::fromCSVName(CSV.getName(), Architecture);
    return Register == model::Register::Invalid;
  };
  FPM.addPass(PromoteGlobalToLocalPass(IsNotARegister));
  FPM.addPass(PromotePass());

  ModulePassManager MPM;

  // Inline `revng_inline`-tagged helper calls so the ABI dataflow analysis can
  // observe the helpers' register reads and writes directly. Helpers whose
  // per-call critical arguments are not LLVM constants here survive un-inlined
  // and are consumed as opaque calls by the analysis.
  MPM.addPass(InlineHelpersPass());

  MPM.addPass(IsolatedOnlyPass(std::move(FPM)));

  MPM.run(M, MAM);

  // A write nobody uses is gone by now, and the load it lent to the copy of its
  // value is left without a reader. Drop it, or the write it reads would look
  // read even where nothing ever looked at the copy.
  //
  // A plain `DCEPass` cannot do this: `precall_hook` and its siblings are
  // `memory(none)` calls returning nothing, so it would take the markers the
  // analysis matches its call sites against with it.
  for (WeakVH &Chained : Loads)
    if (auto *Load = cast_or_null<LoadInst>(Chained);
        Load != nullptr and Load->use_empty())
      Load->eraseFromParent();

  // The outlined stubs live in the module, so a single dump captures them all,
  // as the analysis is about to read them.
  if (not DumpPostInline.empty())
    dumpModule(&M, DumpPostInline.c_str());
}

void DetectABI::analyzeABI() {
  revng_log(Log, "Running ABI analyses");
  LoggerIndent Indent(Log);

  llvm::Task Task(3, "analyzeABI");
  std::map<MetaAddress, std::unique_ptr<OutlinedFunction>> Functions;

  // Create all temporary functions
  Task.advance("Create temporary functions");
  for (model::Function &Function : Binary->Functions()) {
    const MetaAddress &Entry = Function.Entry();
    auto NewFunction = std::make_unique<OutlinedFunction>(Analyzer
                                                            .outline(Entry));
    Functions[Function.Entry()] = std::move(NewFunction);
  }

  // `InlineHelpersPass` only inlines into `Isolated` functions, so we tag the
  // outlined stubs as such. These stubs are temporary and are discarded with
  // the cloned module once the analysis ends, so the tag never escapes. The
  // tag is also what keeps the rest of the pipeline below off `root` and the
  // helpers.
  for (auto &[Entry, OutlinedFn] : Functions)
    FunctionTags::Isolated.addTo(OutlinedFn->Function.get());

  // When `--debug-names` is set, rename each outlined stub from the
  // metaaddress-based identifier to its source-symbol name, using the same
  // `llvmName` scheme as the rest of the pipeline. Done before anything runs,
  // so that whatever a pass logs or dumps says the same names.
  if (DebugNames) {
    for (auto &[Entry, OutlinedFn] : Functions) {
      llvm::Function *F = OutlinedFn->Function.get();
      F->setName(llvmName(Binary->Functions().at(Entry)));
    }
  }

  Task.advance("Prepare the functions");
  prepareFunctions(M, Binary->Architecture());

  // Push this into analyzeFunction
  OpaqueRegisterUser RegisterUser(&M);

  // TODO: this really needs to become a monotone framework
  Task.advance("Run fixed-point analyses");
  llvm::Task FixedPointTask({}, "Fixed-point analysis");
  UniquedQueue<model::Function *> ToAnalyze;
  for (model::Function &Function : Binary->Functions())
    ToAnalyze.insert(&Function);

  // Change the oracle default prototype to have no arguments nor return values
  {
    auto NewDefault = Oracle.getDefault().clone();
    NewDefault.ABIResults.ArgumentsRegisters = {};
    NewDefault.ABIResults.ReturnValuesRegisters = {};
    Oracle.setDefault(std::move(NewDefault));
  }

  unsigned Runs = 0;
  while (not ToAnalyze.empty()) {
    model::Function &Function = *ToAnalyze.pop();
    revng_log(Log, "Analyzing " << Function.Entry().toString());
    FixedPointTask.advance(llvmName(Function));
    OutlinedFunction &OutlinedFunction = *Functions.at(Function.Entry());
    Changes Changes = analyzeFunctionABI(Function,
                                         OutlinedFunction,
                                         RegisterUser);

    if (Changes.Function) {
      revng_log(Log, "The function has changed, re-enqueing all callers:");
      LoggerIndent Indent(Log);
      // The prototype of the function we analyzed has changed, reanalyze
      // callers
      auto *EntryBlock = Root.getBlockAt(Function.Entry());
      auto &FunctionNode = BasicBlockNodeMap[EntryBlock];
      for (auto &CallerNode : FunctionNode->predecessors()) {
        if (CallerNode->Address.isValid()) {
          revng_log(Log, CallerNode->Address.toString());
          ToAnalyze.insert(&Binary->Functions().at(CallerNode->Address));
        }
      }
    }

    // Register for re-analysis all the callees for which we have new
    // information
    for (const MetaAddress &ToReanalyze : Changes.Callees) {
      revng_assert(ToReanalyze.isValid());
      revng_log(Log, "Re-enqueing callee " << ToReanalyze.toString());
      ToAnalyze.insert(&Binary->Functions().at(ToReanalyze));
    }
  }
}

Changes DetectABI::analyzeFunctionABI(const model::Function &Function,
                                      OutlinedFunction &OutlinedFunction,
                                      OpaqueRegisterUser &RegisterReader) {
  revng_log(Log, "Analyzing the ABI of " << Function.Entry().toString());

  //
  // Enrich all the call sites
  //

  // Collect all calls to precall_hook and postcall_hook
  SmallVector<std::pair<CallInst *, bool>> Hooks;
  for (Instruction &I : instructions(OutlinedFunction.Function.get())) {
    if (CallInst *CallToPreHook = getCallTo(&I, Analyzer.preCallHook())) {
      Hooks.emplace_back(CallToPreHook, true);
    } else if (CallInst *CallToPostHook = getCallTo(&I,
                                                    Analyzer.postCallHook())) {
      Hooks.emplace_back(CallToPostHook, false);
    }
  }

  revng::IRBuilder Builder(M.getContext());
  for (auto &[Call, IsPreHook] : Hooks) {
    auto CallerBlock = BasicBlockID::fromValue(Call->getArgOperand(0));
    auto CalleeAddress = MetaAddress::fromValue(Call->getArgOperand(1));
    auto ExtractString = extractFromConstantStringPtr;
    StringRef CalleeSymbol = ExtractString(Call->getArgOperand(2));
    auto &&[Summary, _] = Oracle.getCallSite(Function.Entry(),
                                             CallerBlock,
                                             CalleeAddress,
                                             CalleeSymbol);
    auto &ABIResults = Summary->ABIResults;

    auto *BB = Call->getParent();
    if (IsPreHook) {
      Builder.SetInsertPoint(Call);
      for (auto *CSV : ABIResults.ArgumentsRegisters) {
        // Inject a virtual read of the arguments of the callee
        // TODO: drop const_cast. Unfortunately it requires a significant
        //       refactoring.
        RegisterReader.read(Builder, const_cast<GlobalVariable *>(CSV));
      }

      // Ensure the precall_hook is the first instruction of the block
      BB->splitBasicBlockBefore(Call);
    } else {
      // Ensure the postcall_hook is the last instruction of the block
      BB->splitBasicBlockBefore(Call->getNextNode());

      Builder.SetInsertPoint(BB, BB->getFirstInsertionPt());
      for (auto *CSV : ABIResults.ReturnValuesRegisters) {
        // Inject a virtual write to the return values of the callee
        // TODO: drop const_cast. Unfortunately it requires a significant
        //       refactoring.
        RegisterReader.write(Builder, const_cast<GlobalVariable *>(CSV));
      }
    }
  }

  // Perform the analysis
  Changes Changes = runAnalyses(Function.Entry(), OutlinedFunction);

  RegisterReader.purgeCreated();

  return Changes;
}

// TODO: drop this.
// Since function type conversion is capable of handling the holes
// internally, there's not much reason to push such invasive changes
// this early in the pipeline.
void DetectABI::applyABIDeductions() {
  if (ABIEnforcement == NoABIEnforcement)
    return;

  auto ABI = abi::Definition::get(Binary->DefaultABI());
  for (const model::Function &Function : Binary->Functions()) {
    auto &Summary = Oracle.getLocalFunction(Function.Entry());

    abi::Definition::RegisterSet Arguments;
    abi::Definition::RegisterSet RValues;
    model::Architecture::Values Architecture = Binary->Architecture();
    for (const auto &Register : model::Architecture::registers(Architecture)) {
      for (const model::Register::CSV &RegisterCSV :
           model::Register::getCSVs(Register)) {
        // A register is an argument or a return value if its lowest CSV is
        if (RegisterCSV.StartOffset != 0)
          continue;

        llvm::GlobalVariable *CSV = M.getGlobalVariable(RegisterCSV.Name, true);
        if (CSV == nullptr)
          continue;

        if (Summary.ABIResults.ArgumentsRegisters.contains(CSV))
          Arguments.emplace(Register);

        if (Summary.ABIResults.ReturnValuesRegisters.contains(CSV))
          RValues.emplace(Register);
      }
    }

    if (ABIEnforcement == FullABIEnforcement) {
      // A register the function reads before writing it, where the default
      // ABI passes no argument in it, is not to be taken away from it if an
      // ABI of the same family passes one there: a member function takes
      // `this` in a register in 32-bit Windows code, whatever the default of
      // the binary is. Enforcing the default would leave the function
      // reading a value nobody gives it.
      const abi::Definition *Chosen = &ABI;
      using RegisterSet = abi::Definition::RegisterSet;
      auto Takes = [&Arguments](const abi::Definition &Candidate) {
        RegisterSet Copy = Arguments;
        return Candidate.tryDeducingArgumentRegisterState(std::move(Copy))
          .has_value();
      };
      if (not Arguments.empty() and not Takes(ABI)) {
        using abi::FunctionType::calleeCleanupCounterparts;
        for (model::ABI::Values Other : calleeCleanupCounterparts(ABI.ABI())) {
          const abi::Definition &Counterpart = abi::Definition::get(Other);
          if (Takes(Counterpart)) {
            Chosen = &Counterpart;
            break;
          }
        }
      }

      Arguments = Chosen->enforceArgumentRegisterState(std::move(Arguments));
      RValues = Chosen->enforceReturnValueRegisterState(std::move(RValues));
    } else {
      if (auto R = ABI.tryDeducingArgumentRegisterState(std::move(Arguments)))
        Arguments = *R;
      else
        continue; // Register deduction failed.

      if (auto R = ABI.tryDeducingReturnValueRegisterState(std::move(RValues)))
        RValues = *R;
      else
        continue; // Register deduction failed.
    }

    efa::CSVSet ResultingArguments;
    efa::CSVSet ResultingReturnValues;
    for (const auto &Register : model::Architecture::registers(Architecture)) {
      for (const model::Register::CSV &RegisterCSV :
           model::Register::getCSVs(Register)) {
        llvm::GlobalVariable *CSV = M.getGlobalVariable(RegisterCSV.Name, true);
        if (CSV == nullptr)
          continue;

        if (Arguments.contains(Register))
          ResultingArguments.insert(CSV);
        if (RValues.contains(Register))
          ResultingReturnValues.insert(CSV);
      }
    }

    for (auto &Block : Summary.CFG) {
      for (auto &Edge : Block.Successors()) {
        if (efa::FunctionEdgeType::isCall(Edge->Type())
            && Edge->Type() != efa::FunctionEdgeType::FunctionCall) {
          revng_assert(Block.ID().isValid());
          auto &CSSummary = Summary.ABIResults.CallSites.at(Block.ID());
          CSSummary.ArgumentsRegisters = ResultingArguments;
          CSSummary.ReturnValuesRegisters = ResultingReturnValues;
        }
      }
    }
    Summary.ABIResults.ArgumentsRegisters = std::move(ResultingArguments);
    Summary.ABIResults.ReturnValuesRegisters = std::move(ResultingReturnValues);

    if (Log.isEnabled()) {
      Log << "Summary for " << Function.Name() << ":\n";
      Summary.dump(Log);
      Log << DoLog;
    }
  }
}

/// \return whether \p CFG is a single basic block calling a dynamic function
///
/// That is what a PLT stub looks like: it exists only to reach the dynamic
/// function, so inlining it lets its callers call that function directly.
static bool isDynamicFunctionStub(const SortedVector<efa::BasicBlock> &CFG) {
  if (CFG.size() != 1)
    return false;

  for (const auto &Edge : CFG.begin()->Successors()) {
    const auto *Call = llvm::dyn_cast<efa::CallEdge>(Edge.get());
    if (Call != nullptr and not Call->DynamicFunction().empty())
      return true;
  }

  return false;
}

void DetectABI::recordRegisters(const efa::CSVSet &CSVs, auto Inserter) {
  // For each register, find its highest used CSV and limit the potential size
  // of the register based on it.
  //
  // For example, if only one CSV of a `zmm0` register is used, we can actually
  // type it as `generic64_t` instead of `generic512_t`, which is a lot nicer
  // to work with!
  std::map<model::Register::Values, uint64_t> ConfirmedByteCounts;
  std::set<model::Register::Values> HasLowestCSV;
  for (auto *CSV : CSVs) {
    model::Register::Portion Portion(CSV->getName(), Binary->Architecture());
    if (Portion.Register == model::Register::Invalid)
      continue;

    uint64_t &Bytes = ConfirmedByteCounts[Portion.Register];
    Bytes = std::max(Bytes, Portion.StartOffset + Portion.Size);

    if (Portion.StartOffset == 0)
      HasLowestCSV.insert(Portion.Register);
  }

  for (auto [Register, ByteCount] : ConfirmedByteCounts) {
    revng_assert(model::Register::getSize(Register) >= ByteCount);

    if (not HasLowestCSV.contains(Register))
      continue;

    namespace PK = model::PrimitiveKind;
    auto Type = model::PrimitiveType::makeNextPowerOfTwo(PK::Generic,
                                                         ByteCount);
    revng_assert(model::Register::getSize(Register) >= *Type->size());

    Inserter.emplace(Register).Type() = std::move(Type);
  }
}

void DetectABI::finalizeModel() {
  using namespace model;

  // Fill up the model and build its prototype for each function
  std::set<model::Function *> Functions;
  for (model::Function &Function : Binary->Functions()) {
    // Ignore if we already have a prototype
    if (not Function.Prototype().isEmpty())
      continue;

    MetaAddress EntryPC = Function.Entry();
    revng_assert(EntryPC.isValid());
    auto &Summary = Oracle.getLocalFunction(EntryPC);

    // Replace function attributes
    Function.Attributes() = Summary.Attributes;

    if (isDynamicFunctionStub(Summary.CFG))
      Function.Attributes().insert(FunctionAttribute::AlwaysInline);

    auto &&[Prototype, NewType] = Binary->makeRawFunctionDefinition();
    Prototype.Architecture() = getCodeArchitecture(EntryPC);

    // Record arguments and return values
    recordRegisters(Summary.ABIResults.ArgumentsRegisters,
                    Prototype.Arguments().batch_insert());
    recordRegisters(Summary.ABIResults.ReturnValuesRegisters,
                    Prototype.ReturnValues().batch_insert());

    // Preserved registers
    const auto &ClobberedRegisters = Summary.ClobberedRegisters;
    auto PreservedRegisters = computePreservedRegisters(ClobberedRegisters);
    Prototype.PreservedRegisters() = std::move(PreservedRegisters);

    // Final stack offset
    Prototype.FinalStackOffset() = Summary.ElectedFSO.value_or(0);

    Function.Prototype() = std::move(NewType);
    Functions.insert(&Function);
  }

  // Build prototype for indirect function calls
  for (auto &Function : Functions) {
    auto &Summary = Oracle.getLocalFunction(Function->Entry());
    for (auto &Block : Summary.CFG) {

      // TODO: we do not detect prototypes for inlined call sites
      if (Block.ID().isInlined())
        continue;
      MetaAddress BlockAddress = Block.ID().notInlinedAddress();

      for (auto &Edge : Block.Successors()) {
        if (auto *CE = llvm::dyn_cast<efa::CallEdge>(Edge.get())) {
          auto &CallSitePrototypes = Function->CallSitePrototypes();
          bool IsDirect = CE->Destination().isValid();
          bool IsDynamic = not CE->DynamicFunction().empty();
          bool HasInfoOnEdge = CallSitePrototypes.contains(BlockAddress);
          if (not IsDynamic and not IsDirect and not HasInfoOnEdge) {
            // It's an indirect call for which we have now call site information
            auto Prototype = buildPrototypeForIndirectCall(Summary, Block);

            // This analysis does not have the power to detect whether an
            // indirect call site is a tail call, noreturn or inline
            bool IsTailCall = false;

            // Register new prototype
            Function->CallSitePrototypes().emplace(BlockAddress,
                                                   std::move(Prototype),
                                                   IsTailCall);
          } else if (IsDynamic and not HasInfoOnEdge) {
            // A dynamic function without a prototype: if the analysis of the
            // caller found that it removes its arguments, say so here, since
            // the function is assumed not to
            auto FSO = Analyzer.callSiteFinalStackOffset(BlockAddress);
            auto Prototype = FSO.has_value() ? defaultPrototypeRemoving(*FSO) :
                                               model::UpcastableType();
            if (not Prototype.isEmpty())
              Function->CallSitePrototypes().emplace(BlockAddress,
                                                     std::move(Prototype),
                                                     false);
          }
        }
      }
    }
  }

  // Where the function says what a group of calls removes from the stack
  // together, but not how much each, what is assumed is one of the answers:
  // say so where the call is, instead of leaving it to look known
  for (auto &Function : Functions) {
    auto &Summary = Oracle.getLocalFunction(Function->Entry());
    for (auto &Block : Summary.CFG) {
      if (Block.ID().isInlined())
        continue;
      MetaAddress Call = Analyzer.ambiguousCall(Block.ID().notInlinedAddress());
      if (Call.isInvalid())
        continue;

      model::StatementComment Comment;
      Comment.Index() = Function->Comments().size();
      Comment.Location().insert(Call);
      Comment.Body() = "The stack arguments of this call may belong to a call "
                       "next to it: the function only says how many words "
                       "these calls remove from the stack together.";
      Function->Comments().insert(std::move(Comment));
    }
  }

  for (auto &Function : Functions) {
    auto &Summary = Oracle.getLocalFunction(Function->Entry());
    efa::ControlFlowGraph FM(Function->Entry(), "", Summary.CFG);
    FM.verify(*Binary, true);
  }

  revng_check(Binary->verify(true));
}

void DetectABI::propagatePrototypes() {
  bool NamesAreDeduplicated = false;
  for (model::Function &Function : Binary->Functions()) {
    propagatePrototypesInFunction(Function, NamesAreDeduplicated);
  }
}

/// Check if any of the CSVs holding \p Portions is in \p WrittenRegisters
static bool isAnyWritten(llvm::Module &M,
                         const CSVSet &WrittenRegisters,
                         llvm::ArrayRef<model::Register::Portion> Portions) {
  for (const model::Register::Portion &Portion : Portions) {
    uint64_t PortionEnd = Portion.StartOffset + Portion.Size;
    for (const model::Register::CSV &CSV :
         model::Register::getCSVs(Portion.Register)) {
      // Ignore the CSVs outside of the portion
      if (CSV.StartOffset >= PortionEnd
          or CSV.StartOffset + CSV.Size <= Portion.StartOffset)
        continue;

      auto *Variable = M.getGlobalVariable(CSV.Name, true);
      if (Variable != nullptr and WrittenRegisters.contains(Variable))
        return true;
    }
  }

  return false;
}

// TODO: is this still necessary after the new EFA?
void DetectABI::propagatePrototypesInFunction(model::Function &Function,
                                              bool &NamesAreDeduplicated) {
  const MetaAddress &Entry = Function.Entry();

  revng_log(Log, "Trying to propagate prototypes for " << Entry.toString());
  LoggerIndent Indent(Log);

  FunctionSummary &Summary = Oracle.getLocalFunction(Entry);
  RUAResults &ABI = Summary.ABIResults;
  SortedVector<efa::BasicBlock> &CFG = Summary.CFG;
  CSVSet &WrittenRegisters = Summary.WrittenRegisters;

  bool IsSingleNode = CFG.size() == 1;

  revng_log(Log, "IsSingleNode: " << IsSingleNode);

  if (not IsSingleNode)
    return;

  efa::BasicBlock &Block = *CFG.begin();
  bool HasSingleSuccessor = Block.Successors().size() == 1;
  revng_log(Log, "HasSingleSuccessor: " << HasSingleSuccessor);

  if (not HasSingleSuccessor)
    return;

  auto &Successor = *Block.Successors().begin();
  const auto *Call = dyn_cast<efa::CallEdge>(Successor.get());
  bool SuccessorIsCall = Call != nullptr;
  revng_log(Log, "SuccessorIsCall: " << SuccessorIsCall);

  // Select new prototype for wrapper function
  if (SuccessorIsCall) {
    auto Prototype = getPrototype(*Binary, Entry, Block, *Call);

    using abi::FunctionType::Layout;
    // Get layout of wrapped function
    Layout CalleeLayout = Layout::make(*Prototype);

    // Verify that wrapper function:
    //  - don't write stack pointer
    //  - don't write to callee arguments
    //  - every store instruction writes to registers (not memory)
    llvm::BasicBlock *BB = Root.getBlockAt(Block.ID().start());

    GlobalVariable *StackPointer = Globals.spReg();

    // Check if wrapper writes to Stack Pointer
    const bool WritesSP = WrittenRegisters.contains(StackPointer);

    using std::ranges::count_if;
    const auto &Arguments = CalleeLayout.argumentRegisters();
    bool WritesCalleeArgs = isAnyWritten(M, WrittenRegisters, Arguments);
    const auto &ReturnValues = CalleeLayout.returnValueRegisters();
    bool WritesCalleeReturnValues = isAnyWritten(M,
                                                 WrittenRegisters,
                                                 ReturnValues);

    bool WritesToMemory = count_if(*BB, isWritingToMemory) > 0;
    bool WritesOnlyRegisters = WritesToMemory == 0;

    revng_log(Log, "WritesSP: " << WritesSP);
    revng_log(Log, "WritesCalleeArgs: " << WritesCalleeArgs);
    revng_log(Log, "WritesOnlyRegisters: " << WritesOnlyRegisters);

    // When above conditions are met, overwrite wrappers prototype with
    // wrapped function prototype (`CABIFunctionDefinition` or
    // `RawFunctionDefinition`)
    if (not WritesSP and not WritesCalleeArgs and not WritesCalleeReturnValues
        and WritesOnlyRegisters) {

      if (Log.isEnabled()) {
        Log << "Overwriting " << Entry.toString() << " prototype ";
        if (!Function.Prototype().isEmpty())
          Log << "(" << toString(Function.Prototype()) << ") ";
        Log << "with wrapped function's prototype: "
            << toString(model::copyTypeDefinition(*Prototype)) << DoLog;
      }

      NamesAreDeduplicated = false;
      Function.Prototype() = Binary->makeType(Prototype->key());

      if (Function.Name().empty()) {
        if (not Call->DynamicFunction().empty()) {
          Function.Name() = Call->DynamicFunction();

        } else if (Call->Destination().isValid()) {
          const model::Function &Callee = Binary->Functions()
                                            .at(Call->Destination()
                                                  .notInlinedAddress());
          if (not Callee.Name().empty())
            Function.Name() = Callee.Name();
          else if (not Callee.Name().empty())
            Function.Name() = Callee.Name();
        }
      }
    }
  }

  // TODO: should this be done at a higher abstraction level?
  //
  // This visits the whole model: do it again only after a change, since the
  // previous time left no name to rename
  if (not NamesAreDeduplicated) {
    model::deduplicateCollidingNames(Binary);
    NamesAreDeduplicated = true;
  }
}

model::UpcastableType
DetectABI::buildPrototypeForIndirectCall(const FunctionSummary &CallerSummary,
                                         const efa::BasicBlock &CallerBlock) {
  using namespace model;

  auto &&[Prototype, NewType] = Binary->makeRawFunctionDefinition();
  Prototype.Architecture() = getCodeArchitecture(CallerBlock.ID().start());

  bool Found = false;
  for (const auto &[PC, CallSites] : CallerSummary.ABIResults.CallSites) {
    if (PC != CallerBlock.ID())
      continue;

    revng_assert(!Found);
    Found = true;

    recordRegisters(CallSites.ArgumentsRegisters,
                    Prototype.Arguments().batch_insert());
    recordRegisters(CallSites.ReturnValuesRegisters,
                    Prototype.ReturnValues().batch_insert());
  }
  revng_assert(Found);

  // Import FinalStackOffset and CalleeSavedRegisters from the default
  // prototype
  const FunctionSummary &DefaultSummary = Oracle.getDefault();
  const auto &Clobbered = DefaultSummary.ClobberedRegisters;
  Prototype.PreservedRegisters() = computePreservedRegisters(Clobbered);

  Prototype.FinalStackOffset() = DefaultSummary.ElectedFSO.value_or(0);

  // Unless the analysis of the caller found what this callee removes
  MetaAddress CallSite = CallerBlock.ID().notInlinedAddress();
  if (auto FSO = Analyzer.callSiteFinalStackOffset(CallSite))
    Prototype.FinalStackOffset() = *FSO;

  return std::move(NewType);
}

model::UpcastableType
DetectABI::defaultPrototypeRemoving(int64_t FinalStackOffset) {
  const model::TypeDefinition *Prototype = Binary->defaultPrototype();
  const auto *Default = dyn_cast<model::RawFunctionDefinition>(Prototype);
  if (Default == nullptr
      or int64_t(Default->FinalStackOffset()) == FinalStackOffset)
    return {};

  auto It = DefaultPrototypeCopies.find(FinalStackOffset);
  if (It != DefaultPrototypeCopies.end())
    return Binary->makeType(It->second);

  auto &&[Copy, NewType] = Binary->makeRawFunctionDefinition();
  Copy.Architecture() = Default->Architecture();
  Copy.Arguments() = Default->Arguments();
  Copy.ReturnValues() = Default->ReturnValues();
  Copy.PreservedRegisters() = Default->PreservedRegisters();
  Copy.FinalStackOffset() = FinalStackOffset;
  setRemovedStackArguments(Copy, FinalStackOffset, Default->FinalStackOffset());
  DefaultPrototypeCopies.emplace(FinalStackOffset, Copy.key());

  return std::move(NewType);
}

static void combineCrossCallSites(auto &CallSite, auto &Callee) {
  // TODO: why not return values?
  for (auto *CSV : CallSite.ArgumentsRegisters) {
    Callee.ArgumentsRegisters.insert(CSV);
  }
}

CSVSet DetectABI::findWrittenRegisters(llvm::Function *F) {
  using namespace llvm;

  CSVSet WrittenRegisters;
  for (auto &BB : *F) {
    for (auto &I : BB) {
      if (auto *SI = dyn_cast<StoreInst>(&I)) {
        Value *Ptr = skipCasts(SI->getPointerOperand());
        if (auto *GV = dyn_cast<GlobalVariable>(Ptr))
          WrittenRegisters.insert(GV);
      }
    }
  }

  return WrittenRegisters;
}

CSVSet DetectABI::computePreservedCSVs(const CSVSet &ClobberedRegisters) const {
  using llvm::GlobalVariable;
  using std::set;
  CSVSet PreservedRegisters(Analyzer.abiCSVs().begin(),
                            Analyzer.abiCSVs().end());

  for (GlobalVariable *ClobberedRegister : ClobberedRegisters)
    PreservedRegisters.erase(ClobberedRegister);

  return PreservedRegisters;
}

TrackingSortedVector<model::Register::Values>
DetectABI::computePreservedRegisters(const CSVSet &ClobberedRegisters) const {
  using namespace model;

  TrackingSortedVector<model::Register::Values> Result;
  CSVSet PreservedRegisters = computePreservedCSVs(ClobberedRegisters);

  // A register is preserved only if all the CSVs composing it are preserved,
  // so count how many of each register's CSVs survive.
  std::map<model::Register::Values, uint64_t> PreservedLaneCount;
  for (auto *CSV : PreservedRegisters) {
    auto RegisterID = model::Register::fromCSVName(CSV->getName(),
                                                   Binary->Architecture());
    if (RegisterID == Register::Invalid)
      continue;

    PreservedLaneCount[RegisterID] += 1;
  }

  auto PreservedRegistersInserter = Result.batch_insert();
  for (auto [RegisterID, Count] : PreservedLaneCount)
    if (Count == model::Register::getCSVCount(RegisterID))
      PreservedRegistersInserter.insert(RegisterID);

  return Result;
}

static Logger FunctionFromCalleesLog("functions-from-callees-collection");

/// \p Entry is where more than one function jumps to with nothing but its
/// return address on the stack: a function. (A function of its own: what is
/// made one can be watched from a debugger.)
[[gnu::noinline]] static void
makeFunctionAtSharedJumpTarget(model::Binary &Binary,
                               const MetaAddress &Entry) {
  Binary.Functions()[Entry];
}

/// Code that several functions jump to is part of each of them as long as it
/// is not a function: nobody calls it, so nothing said it was one. If each
/// of them gets there with the stack as it was when it was entered, they
/// leave through it as through a call that is not coming back, and it is the
/// function they all end with: the member functions adjusting thunks jump to
/// are found this way. As code of several functions it is decompiled once
/// for each, and what belongs to it (the code it is resumed at after an
/// exception) has no function to belong to.
///
/// \return the functions that jump to the new ones
///
/// \p Analyzed are the functions whose blocks are known.
static std::vector<MetaAddress>
collectFunctionsFromSharedJumpTargets(model::Binary &Binary,
                                      ControlFlowGraphCache &FMC,
                                      const std::set<MetaAddress> &Analyzed) {
  std::map<MetaAddress, unsigned> Owners;
  for (const MetaAddress &Entry : Analyzed)
    for (const efa::BasicBlock &Block : FMC.getControlFlowGraph(Entry).Blocks())
      if (not Block.ID().isInlined())
        ++Owners[Block.ID().start()];

  auto IsShared = [&Owners](const MetaAddress &Address) {
    auto It = Owners.find(Address);
    return It != Owners.end() and It->second > 1;
  };

  // The shared blocks a block of one function only leads to, and from which
  // functions
  std::map<MetaAddress, std::set<MetaAddress>> Jumpers;
  std::set<MetaAddress> NotAtEntryHeight;
  for (const MetaAddress &Entry : Analyzed) {
    const auto &Blocks = FMC.getControlFlowGraph(Entry).Blocks();
    for (const efa::BasicBlock &Block : Blocks) {
      if (Block.ID().isInlined() or IsShared(Block.ID().start()))
        continue;

      for (const auto &Edge : Block.Successors()) {
        if (Edge->Type() != efa::FunctionEdgeType::DirectBranch
            or Edge->Destination().isInlined())
          continue;

        MetaAddress Target = Edge->Destination().start();
        if (not IsShared(Target) or Binary.Functions().contains(Target))
          continue;

        Jumpers[Target].insert(Entry);
        auto It = Blocks.find(Edge->Destination());
        if (It == Blocks.end() or not It->HasStackDepth()
            or It->StackDepth() != 0)
          NotAtEntryHeight.insert(Target);
      }
    }
  }

  std::set<MetaAddress> Result;
  for (const auto &[Target, From] : Jumpers) {
    if (From.size() < 2 or NotAtEntryHeight.contains(Target))
      continue;
    makeFunctionAtSharedJumpTarget(Binary, Target);
    Result.insert(From.begin(), From.end());
  }

  return { Result.begin(), Result.end() };
}

static void collectFunctionsFromCallees(Module &M, model::Binary &Binary) {
  Function &Root = *M.getFunction("root");

  // Static symbols have already been registered during lifting phase. Now
  // register all the other candidate entry points.
  for (llvm::BasicBlock &BB : Root) {
    if (getType(&BB) != BlockType::JumpTargetBlock)
      continue;

    MetaAddress Entry = getBasicBlockAddress(getJumpTargetBlock(&BB));
    if (Binary.Functions().contains(Entry))
      continue;

    uint32_t Reasons = JumpTargetReason::getReasons(&BB);
    bool IsCallee = hasReason(Reasons, JumpTargetReason::Callee);

    if (IsCallee) {
      // Create the function
      Binary.Functions()[Entry];
      revng_log(FunctionFromCalleesLog,
                "Found function from callee: " << BB.getName().str());
    }
  }

  // The only place a call through a pointer can go to is called too
  if (auto *Marker = functionOrNull(FunctionCallMarker.get(M))) {
    for (CallBase *Call : callersIn(Marker, &Root)) {
      if (not isa<ConstantPointerNull>(Call->getArgOperand(0)))
        continue;

      llvm::BasicBlock *Callee = getOnlyCallee(Call->getParent(), Binary);
      if (Callee == nullptr)
        continue;

      MetaAddress Entry = getBasicBlockAddress(Callee);
      if (Entry.isValid() and not Binary.Functions().contains(Entry)) {
        Binary.Functions()[Entry];
        revng_log(FunctionFromCalleesLog,
                  "Found function from the only target of a call: "
                    << Callee->getName().str());
      }
    }
  }
}

static void suppressCalleeSaved(RUAResults &ABIResults,
                                const CSVSet &CalleeSavedRegs) {

  // Suppress from arguments
  for (const auto &Register : CalleeSavedRegs)
    ABIResults.ArgumentsRegisters.erase(Register);

  // Suppress from return values
  for (const auto &Register : CalleeSavedRegs)
    ABIResults.ReturnValuesRegisters.erase(Register);

  // Suppress from call-sites
  for (const auto &[K, _] : ABIResults.CallSites) {
    for (const auto &Register : CalleeSavedRegs) {
      ABIResults.CallSites[K].ArgumentsRegisters.erase(Register);
      ABIResults.CallSites[K].ReturnValuesRegisters.erase(Register);
    }
  }
}

template<typename T, typename F>
static auto zeroOrOne(const T &Range, const F &Predicate)
  -> decltype(&*Range.begin()) {
  decltype(&*Range.begin()) Result = nullptr;
  for (auto &E : Range) {
    if (Predicate(E)) {
      revng_assert(not Result);
      Result = &E;
    }
  }

  return Result;
}

Changes DetectABI::runAnalyses(MetaAddress EntryAddress,
                               OutlinedFunction &OutlinedFunction) {
  using namespace llvm;
  using llvm::BasicBlock;

  revng::IRBuilder Builder(M.getContext());
  RUAResults ABIResults;

  // Find registers that may be target of at least one store. This helps
  // refine the final results.
  auto WrittenRegisters = findWrittenRegisters(OutlinedFunction.Function.get());

  // Run ABI-independent data-flow analyses
  ABIResults = analyzeRegisterUsage(OutlinedFunction.Function.get(),
                                    Binary->Architecture(),
                                    Analyzer.preCallHook(),
                                    Analyzer.postCallHook(),
                                    Analyzer.retHook());

  // We say that a register is callee-saved when, besides being preserved by
  // the callee, there is at least a write onto this register.
  FunctionSummary &Summary = Oracle.getLocalFunction(EntryAddress);
  auto CalleeSavedRegs = computePreservedCSVs(Summary.ClobberedRegisters);
  auto ActualCalleeSavedRegs = llvm::set_intersection(CalleeSavedRegs,
                                                      WrittenRegisters);

  // Refine ABI analyses results by suppressing callee-saved and stack
  // pointer registers.
  suppressCalleeSaved(ABIResults, ActualCalleeSavedRegs);

  // Commit ABI analysis results to the oracle
  auto Old = Summary.ABIResults;
  Summary.ABIResults.combine(ABIResults);
  Summary.WrittenRegisters = WrittenRegisters;

  Changes Changes;
  Changes.Function = Old != Summary.ABIResults;

  for (auto &[BlockID, CallSite] : ABIResults.CallSites) {
    // TODO: why are we ignoring inlined call sites?
    if (BlockID.isInlined())
      continue;

    MetaAddress Callee = CallSite.CalleeAddress;

    // TODO: eventually we'll want to add arguments/return values to dynamic
    //       functions too
    FunctionSummary *Summary = nullptr;
    if (Callee.isValid()) {
      Summary = &Oracle.getLocalFunction(Callee);
      revng_assert(Summary != nullptr);
    } else {
      Summary = Oracle.getExactCallSite(EntryAddress, BlockID).first;
    }

    // TODO: are longjmps calls? Do we want to collect prototype for them?
    //       Right now, they are in the IR, but we do not register them as call
    //       sites in the Oracle.
    if (Summary != nullptr) {
      bool Changed = false;
      RUAResults &ToAdjust = Summary->ABIResults;

      // Note the order of the operands: `or` short circuits, so writing
      // `Changed or insert(...)` would stop inserting as soon as one register
      // turned out to be new, and which ones made it in would depend on the
      // order the set is iterated in.
      for (auto *CSV : CallSite.ArgumentsRegisters)
        Changed = ToAdjust.ArgumentsRegisters.insert(CSV).second or Changed;

      for (auto *CSV : CallSite.ReturnValuesRegisters)
        Changed = ToAdjust.ReturnValuesRegisters.insert(CSV).second or Changed;

      if (Changed and Callee.isValid())
        Changes.Callees.insert(Callee);
    }
  }

  return Changes;
}

static void reattachFunctions(Module &M,
                              RootFunction &Root,
                              const CSVGlobals &Globals,
                              ControlFlowGraphCache &FMC,
                              TupleTree<model::Binary> &Binary,
                              const std::set<MetaAddress> &Before) {
  using FSOracle = FunctionSummaryOracle;
  FSOracle Oracle = FSOracle::importFullPrototypes(M, Globals, *Binary);
  CFGAnalyzer Analyzer(M, Root, Globals, Binary, Oracle);
  DetectABI Attacher(M, Root, Globals, FMC, Binary, Oracle, Analyzer);
  Attacher.reattachFunctions(Before);
}

/// \p Entry is the address of data, not the entry of a function: forget the
/// function that was made of it. (A function of its own: what is dropped can
/// be watched from a debugger.)
[[gnu::noinline]] static void
dropFunctionAtDataAddress(model::Binary &Binary, const MetaAddress &Entry) {
  Binary.Functions().erase(Entry);
}

static void runDetectABI(Module &M,
                         RootFunction &Root,
                         const CSVGlobals &Globals,
                         ControlFlowGraphCache &FMC,
                         TupleTree<model::Binary> &Binary) {
  using FSOracle = FunctionSummaryOracle;
  {
    FSOracle Oracle = FSOracle::importFullPrototypes(M, Globals, *Binary);
    CFGAnalyzer Analyzer(M, Root, Globals, Binary, Oracle);
    DetectABI ABIDetector(M, Root, Globals, FMC, Binary, Oracle, Analyzer);
    ABIDetector.run();
  }

  // With what the model now says about the functions: find the clean-ups
  // their callers left out, then give them the code they are resumed at.
  //
  // A function that returns after all, once such a clean-up is counted, was
  // said never to return: its callers were cut at each call to it, without a
  // trace. Those functions and all that call them are found again, with the
  // clean-ups that were found; which can bring new ones to light.
  constexpr unsigned MostRounds = 3;
  for (unsigned Round = 0; Round < MostRounds; ++Round) {
    std::vector<MetaAddress> Changed;
    {
      FSOracle Oracle = FSOracle::importFullPrototypes(M, Globals, *Binary);
      CFGAnalyzer Analyzer(M, Root, Globals, Binary, Oracle);
      DetectABI Cleaner(M, Root, Globals, FMC, Binary, Oracle, Analyzer);
      Changed = Cleaner.inferElidedCleanups();
      if (Changed.empty())
        break;
      Cleaner.forgetWithCallers(Changed);
    }

    FSOracle Oracle = FSOracle::importFullPrototypes(M, Globals, *Binary);
    CFGAnalyzer Analyzer(M, Root, Globals, Binary, Oracle);
    DetectABI Again(M, Root, Globals, FMC, Binary, Oracle, Analyzer);
    Again.run();
  }

  FSOracle Oracle = FSOracle::importFullPrototypes(M, Globals, *Binary);
  CFGAnalyzer Analyzer(M, Root, Globals, Binary, Oracle);
  DetectABI Attacher(M, Root, Globals, FMC, Binary, Oracle, Analyzer);
  Attacher.attachResumeBlocks();
}

} // namespace efa

namespace revng::pypeline::analyses {

llvm::Error DetectABI::run(Model &Model,
                           const Request &Incoming,
                           llvm::StringRef Configuration,
                           LLVMRootContainer &ModuleContainer) {
  llvm::Module &Module = ModuleContainer.getModule();
  TupleTree<model::Binary> &TupleModel = Model.get();
  model::Binary &Binary = *TupleModel;

  RootFunction Root(Module);
  CSVGlobals Globals(Binary, Module);
  ControlFlowGraphCache FMC;

  // Link helper bodies once at the start of the analysis, to avoid relinking
  // for every `inline-helpers` call
  {
    llvm::legacy::PassManager PM;
    PM.add(new LinkHelpersToInlinePass());
    PM.run(Module);
  }

  efa::collectFunctionsFromCallees(Module, Binary);
  efa::runDetectABI(Module, Root, Globals, FMC, TupleModel);

  std::set<MetaAddress> Before;
  for (const model::Function &Function : Binary.Functions())
    Before.insert(Function.Entry());

  collectFunctionsFromUnusedAddresses(Module, Root, Binary, FMC);
  using efa::collectFunctionsFromSharedJumpTargets;
  auto Jumpers = collectFunctionsFromSharedJumpTargets(Binary, FMC, Before);
  efa::runDetectABI(Module, Root, Globals, FMC, TupleModel);

  // The functions that were just found jump to shared code too: the thunks
  // the tables of virtual functions refer to are found there, and several of
  // them end in the same function
  {
    std::set<MetaAddress> Analyzed;
    for (const model::Function &Function : Binary.Functions())
      Analyzed.insert(Function.Entry());
    auto More = collectFunctionsFromSharedJumpTargets(Binary, FMC, Analyzed);
    if (not More.empty()) {
      efa::runDetectABI(Module, Root, Globals, FMC, TupleModel);
      Jumpers.insert(Jumpers.end(), More.begin(), More.end());
    }
  }

  // What the functions that jump to a new function said about the calls in
  // its code is now said by that function
  for (const MetaAddress &Entry : Jumpers) {
    model::Function &Function = Binary.Functions().at(Entry);
    const auto &Blocks = FMC.getControlFlowGraph(Entry).Blocks();
    std::vector<MetaAddress> Stale;
    for (const model::CallSitePrototype &CallSite :
         Function.CallSitePrototypes())
      if (not Blocks.contains(BasicBlockID(CallSite.CallerBlockAddress())))
        Stale.push_back(CallSite.CallerBlockAddress());
    for (const MetaAddress &Block : Stale)
      Function.CallSitePrototypes().erase(Block);
  }

  // A function made from an address nothing jumps to is not one if code that
  // can be trusted only uses that address to say where memory is read: it is
  // a table. Code can be trusted if it is of a function found before those,
  // or of one that turned out to return and nothing else: what the bytes of
  // a table decode to does not.
  auto IsTrusted = [&](const model::Function &Function) {
    if (Before.contains(Function.Entry()))
      return true;
    if (not Function.Attributes().empty())
      return false;

    bool Returns = false;
    for (const efa::BasicBlock &Block :
         FMC.getControlFlowGraph(Function.Entry()).Blocks()) {
      for (const auto &Edge : Block.Successors()) {
        using namespace efa::FunctionEdgeType;
        const auto *Call = llvm::dyn_cast<efa::CallEdge>(Edge.get());
        if (Edge->Type() == Return
            or (Call != nullptr and Edge->Type() == FunctionCall
                and Call->IsTailCall()))
          Returns = true;
        else if (Edge->Type() != DirectBranch and Edge->Type() != FunctionCall)
          return false;
      }
    }
    return Returns;
  };
  std::set<uint64_t> Data = addressesUsedAsData(Module,
                                                Root,
                                                Binary,
                                                FMC,
                                                IsTrusted);
  std::vector<MetaAddress> NotFunctions;
  for (const model::Function &Function : Binary.Functions())
    if (not Before.contains(Function.Entry())
        and Data.contains(Function.Entry().address()))
      NotFunctions.push_back(Function.Entry());
  for (const MetaAddress &Entry : NotFunctions)
    efa::dropFunctionAtDataAddress(Binary, Entry);

  // What is left of them and does not leave as a function does may be code
  // that one of the others is resumed at
  efa::reattachFunctions(Module, Root, Globals, FMC, TupleModel, Before);

  return llvm::Error::success();
}

} // namespace revng::pypeline::analyses
