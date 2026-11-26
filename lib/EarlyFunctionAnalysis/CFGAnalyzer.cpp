//
// This file is distributed under the MIT License. See LICENSE.md for details.
//

#include <fstream>

#include <numeric>

#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/Analysis/AliasAnalysis.h"
#include "llvm/Analysis/BasicAliasAnalysis.h"
#include "llvm/Analysis/LazyValueInfo.h"
#include "llvm/Analysis/ScopedNoAliasAA.h"
#include "llvm/CodeGen/UnreachableBlockElim.h"
#include "llvm/IR/Dominators.h"
#include "llvm/IR/InstIterator.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/IR/Operator.h"
#include "llvm/IR/PatternMatch.h"
#include "llvm/Passes/PassBuilder.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Transforms/InstCombine/InstCombine.h"
#include "llvm/Transforms/Scalar/EarlyCSE.h"
#include "llvm/Transforms/Scalar/GVN.h"
#include "llvm/Transforms/Scalar/JumpThreading.h"
#include "llvm/Transforms/Scalar/MergedLoadStoreMotion.h"
#include "llvm/Transforms/Scalar/SROA.h"
#include "llvm/Transforms/Scalar/SimplifyCFG.h"

#include "revng/ADT/Queue.h"
#include "revng/BasicAnalyses/CSVGlobals.h"
#include "revng/BasicAnalyses/RemoveHelperCalls.h"
#include "revng/BasicAnalyses/RemoveNewPCCalls.h"
#include "revng/EarlyFunctionAnalysis/AAWriterPass.h"
#include "revng/EarlyFunctionAnalysis/BasicBlock.h"
#include "revng/EarlyFunctionAnalysis/CFGAnalyzer.h"
#include "revng/EarlyFunctionAnalysis/CallEdge.h"
#include "revng/EarlyFunctionAnalysis/FunctionEdgeBase.h"
#include "revng/EarlyFunctionAnalysis/FunctionEdgeType.h"
#include "revng/EarlyFunctionAnalysis/IndirectBranchInfo.h"
#include "revng/EarlyFunctionAnalysis/IndirectBranchInfoPrinterPass.h"
#include "revng/EarlyFunctionAnalysis/PromoteGlobalToLocalVars.h"
#include "revng/EarlyFunctionAnalysis/SegregateDirectStackAccesses.h"
#include "revng/Model/FunctionAttribute.h"
#include "revng/Model/FunctionTags.h"
#include "revng/Support/BasicBlockID.h"
#include "revng/Support/EmitAbort.h"
#include "revng/Support/Generator.h"
#include "revng/Support/IRBuilder.h"
#include "revng/Support/MetaAddress.h"
#include "revng/Support/NewPC.h"
#include "revng/Support/TemporaryLLVMOption.h"

// This name is not present after `lift`.

using namespace llvm;

using BranchArgument = efa::IndirectBranchInfoArgument;
constexpr unsigned CallerBlockIDIndex = index(BranchArgument::CallerBlockID);
constexpr unsigned CalledSymbolIndex = index(BranchArgument::CalledSymbol);
constexpr unsigned
  JumpsToReturnAddressIndex = index(BranchArgument::JumpsToReturnAddress);
constexpr unsigned
  StackPointerOffsetIndex = index(BranchArgument::StackPointerOffset);
constexpr unsigned
  ReturnValuePreservedIndex = index(BranchArgument::ReturnValuePreserved);
constexpr unsigned
  FirstPreservedRegisterIndex = index(BranchArgument::FirstPreservedRegister);
constexpr unsigned CallerBlockID = CallerBlockIDIndex;
constexpr unsigned CalledSymbol = CalledSymbolIndex;
constexpr unsigned JumpsToReturnAddress = JumpsToReturnAddressIndex;
constexpr unsigned StackPointerOffset = StackPointerOffsetIndex;
constexpr unsigned ReturnValuePreserved = ReturnValuePreservedIndex;
constexpr unsigned FirstPreservedRegister = FirstPreservedRegisterIndex;
using namespace llvm::cl;

static opt<std::string> AAWriterPath("aa-writer",
                                     desc("Dump to disk the outlined functions "
                                          "with annotated alias info."),
                                     value_desc("filename"));

static opt<std::string> IndirectBranchInfoSummaryPath("indirect-branch-info-"
                                                      "summary",
                                                      desc("Write the results "
                                                           "of SA2 on disk."),
                                                      value_desc("filename"));

static Logger Log("cfg-analyzer");

/// What a call to the stack height hook is about: its last argument
constexpr uint32_t StackHeightAtBlockStart = 0;
constexpr uint32_t StackHeightAtCall = 1;

static MetaAddress getFinalAddressOfBasicBlock(llvm::BasicBlock *BB) {
  auto &&[End, Size] = getPC(BB->getTerminator());
  return End + Size;
}

static UpcastablePointer<efa::FunctionEdgeBase>
makeCall(MetaAddress Destination) {
  using ReturnType = UpcastablePointer<efa::FunctionEdgeBase>;
  return ReturnType::make<efa::CallEdge>(BasicBlockID(Destination),
                                         efa::FunctionEdgeType::FunctionCall);
};

static UpcastablePointer<efa::FunctionEdgeBase>
makeEdge(BasicBlockID Destination, efa::FunctionEdgeType::Values Type) {
  revng_assert(Type != efa::FunctionEdgeType::FunctionCall);
  efa::FunctionEdge *Result = nullptr;
  using ReturnType = UpcastablePointer<efa::FunctionEdgeBase>;
  return ReturnType::make<efa::FunctionEdge>(Destination, Type);
};

static UpcastablePointer<efa::FunctionEdgeBase>
makeIndirectEdge(efa::FunctionEdgeType::Values Type) {
  return makeEdge(BasicBlockID::invalid(), Type);
};

namespace efa {

static efa::BasicBlock &
blockFromIndirectBranchInfo(CallBase *CI, SortedVector<efa::BasicBlock> &CFG) {
  auto *BlockIDArgument = CI->getArgOperand(CallerBlockIDIndex);
  return CFG.at(BasicBlockID::fromValue(BlockIDArgument));
}

static std::unique_ptr<llvm::raw_ostream>
streamFromOption(const opt<std::string> &Option) {
  if (Option.getNumOccurrences() == 1) {
    std::ifstream File(Option.c_str());
    if (File.is_open()) {
      int Status = std::remove(Option.c_str());
      revng_assert(Status == 0);
    }

    std::error_code EC;
    auto Result = std::make_unique<raw_fd_ostream>(Option,
                                                   EC,
                                                   llvm::sys::fs::OF_Append);
    revng_assert(!EC);
    return Result;
  } else {
    return std::make_unique<llvm::raw_null_ostream>();
  }
}

CFGAnalyzer::CFGAnalyzer(llvm::Module &M,
                         RootFunction &Root,
                         const CSVGlobals &Globals,
                         const TupleTree<model::Binary> &Binary,
                         FunctionSummaryOracle &Oracle) :
  M(M),
  Globals(Globals),
  PCH(ProgramCounterHandler::fromModule(Binary->Architecture(), &M)),
  Oracle(Oracle),
  Binary(Binary),
  PreCallHook(createCallMarkerType(M), "precall_hook", &M),
  PostCallHook(PreCallHook.get()->getFunctionType(), "postcall_hook", &M),
  RetHook(createRetMarkerType(M), "retcall_hook", &M),
  StackHeightHook(createStackHeightMarkerType(Globals),
                  "stack_height_hook",
                  &M),
  StackEffect(createStackEffectType(Globals), "stack_effect", &M),
  OutlinerState(M),
  Outliner(M, Root, Globals, Oracle, OutlinerState),
  OpaqueBranchConditionsPool(&M, false),
  OutputAAWriter(streamFromOption(AAWriterPath)),
  OutputIBI(streamFromOption(IndirectBranchInfoSummaryPath)) {

  // Unlike the other hooks, this one is read after the optimizations: its
  // calls have to survive them, without standing in the way of what they do
  // with the memory of the program
  StackHeightHook.get()->setMemoryEffects(MemoryEffects::inaccessibleMemOnly());
  StackHeightHook.get()->addFnAttr(Attribute::NoMerge);

  // An unknown is a value, not something that happens
  StackEffect.get()->setDoesNotAccessMemory();

  // Collect all ABI CSVs except for the stack pointer
  for (GlobalVariable *CSV : Globals.abiRegisters())
    if (CSV != nullptr and not Globals.isSPReg(CSV))
      ABICSVs.emplace_back(CSV);

  // Prepare header for debugging information about indirect branch infos
  *OutputIBI << "name,ra,fso,address";
  for (const auto &Register : ABICSVs)
    *OutputIBI << "," << Register->getName();
  *OutputIBI << "\n";
}

OutlinedFunction CFGAnalyzer::outline(const MetaAddress &Entry) {
  auto &CFG = Oracle.getLocalFunction(Entry).CFG;
  bool HasCFG = CFG.size() != 0;
  llvm::SmallSet<MetaAddress, 4> ReturnBlocks;

  if (HasCFG)
    for (const efa::BasicBlock &Block : CFG)
      for (const auto &Successor : Block.Successors())
        if (Successor->Type() == efa::FunctionEdgeType::Return)
          ReturnBlocks.insert(Block.ID().start());

  CallSummarizer Summarizer(&M,
                            PreCallHook.get(),
                            PostCallHook.get(),
                            RetHook.get(),
                            StackHeightHook.get(),
                            StackEffect.get(),
                            Globals.spReg(),
                            HasCFG ? &ReturnBlocks : nullptr,
                            &CallSiteFSO,
                            &SymbolicSites,
                            &UnknownCalls,
                            &KnownCalls,
                            &ElidedCleanups,
                            &CalleeClobbered,
                            &CalleeSaved,
                            Entry);

  UnknownCalls.clear();
  KnownCalls.clear();
  CalleeClobbered.clear();
  if (CalleeSaved.empty())
    for (llvm::GlobalVariable *CSV : ABICSVs)
      if (not Oracle.getDefault().ClobberedRegisters.contains(CSV))
        CalleeSaved.insert(CSV);
  OutlinedFunction Result = Outliner.outline(Entry, &Summarizer);

  // Make sure we start a new block before a PreCallHook and for each jump
  // target of the function that has just been outlined
  std::optional NewPC = NewPCHelper.get(M);
  revng_assert(NewPC.has_value());
  std::vector<llvm::Instruction *> BlockStarts;
  if (llvm::Function *F = Result.Function.get()) {
    for (llvm::Instruction &I : llvm::instructions(F)) {
      if (isCallTo(&I, PreCallHook.get())) {
        BlockStarts.push_back(&I);
      } else if (std::optional Call = NewPC->getCall(&I)) {
        if (startsBasicBlock(*Call))
          BlockStarts.push_back(&I);
      }
    }
  }

  for (llvm::Instruction *I : BlockStarts)
    if (I->getParent()->getFirstNonPHI() != I)
      I->getParent()->splitBasicBlock(I);

  return Result;
}

llvm::FunctionType *CFGAnalyzer::createCallMarkerType(llvm::Module &M) {
  auto &Context = M.getContext();
  Type *I8Ptr = Type::getInt8PtrTy(Context);
  Type *BoolType = Type::getInt1Ty(Context);
  Type *Void = Type::getVoidTy(Context);
  return llvm::FunctionType::get(Void,
                                 { I8Ptr, I8Ptr, I8Ptr, BoolType },
                                 false);
}

llvm::FunctionType *
CFGAnalyzer::createStackEffectType(const CSVGlobals &Globals) {
  Type *StackPointer = Globals.spReg()->getValueType();
  Type *Index = Type::getInt32Ty(StackPointer->getContext());
  return llvm::FunctionType::get(StackPointer, { Index }, false);
}

llvm::FunctionType *CFGAnalyzer::createRetMarkerType(llvm::Module &M) {
  auto &Context = M.getContext();
  Type *Void = Type::getVoidTy(Context);
  Type *I8Ptr = Type::getInt8PtrTy(Context);
  return llvm::FunctionType::get(Void, { I8Ptr }, false);
}

llvm::FunctionType *
CFGAnalyzer::createStackHeightMarkerType(const CSVGlobals &Globals) {
  Type *StackPointer = Globals.spReg()->getValueType();
  LLVMContext &Context = StackPointer->getContext();
  Type *Void = Type::getVoidTy(Context);
  Type *I8Ptr = Type::getInt8PtrTy(Context);
  Type *Kind = Type::getInt32Ty(Context);
  return llvm::FunctionType::get(Void, { I8Ptr, StackPointer, Kind }, false);
}

std::optional<UpcastablePointer<efa::FunctionEdgeBase>>
CFGAnalyzer::handleCall(llvm::CallInst *PreCallHookCall) {
  using namespace llvm;

  //
  // Extract parameters
  //

  // Is this a direct call to a function defined in the model?
  Value *CalleePC = PreCallHookCall->getArgOperand(1);
  MetaAddress CalleeAddress = MetaAddress::invalid();
  bool IsDirectCall = false;
  auto Address = BasicBlockID::fromValue(CalleePC).notInlinedAddress();
  if (Address.isValid()) {
    IsDirectCall = Binary->Functions().count(Address) != 0;
    if (IsDirectCall)
      CalleeAddress = Address;
  }

  // Symbol name
  StringRef SymbolName;
  auto *SymbolArgument = PreCallHookCall->getArgOperand(2);
  bool IsDynamicCall = not isa<ConstantPointerNull>(SymbolArgument);
  if (IsDynamicCall)
    SymbolName = extractFromConstantStringPtr(SymbolArgument);

  // Tail call?
  bool IsTailCall = (getLimitedValue(PreCallHookCall->getArgOperand(3)) == 1);

  // Here we handle only direct, non-tail calls
  if (not IsDirectCall and IsTailCall)
    return std::nullopt;

  if (IsDynamicCall)
    revng_assert(not IsDirectCall);

  //
  // Construct call edge
  //
  UpcastablePointer<efa::FunctionEdgeBase> Edge = makeCall(CalleeAddress);

  auto *CE = cast<efa::CallEdge>(Edge.get());
  CE->IsTailCall() = IsTailCall;
  if (IsDynamicCall)
    CE->DynamicFunction() = SymbolName.str();

  return Edge;
}

SortedVector<efa::BasicBlock>
CFGAnalyzer::collectDirectCFG(OutlinedFunction *OF) {
  using namespace llvm;
  using llvm::BasicBlock;
  revng_log(Log, "collectDirectCFG(" << OF->Function->getName().str() << ")");
  LoggerIndent Indent(Log);

  SortedVector<efa::BasicBlock> CFG;

  for (BasicBlock &BB : *OF->Function) {
    if (isJumpTarget(&BB)) {
      // Create a efa::BasicBlock for each jump target
      BasicBlockID ID = getBasicBlockID(&BB);
      MetaAddress End = getFinalAddressOfBasicBlock(&BB);
      revng_assert(End.isValid());

      efa::BasicBlock Block{ ID };
      Block.End() = End;
      Block.InlinedFrom() = OF->InlinedFunctionsByIndex.at(ID.inliningIndex());
      bool ReachesUnexpectedPC = false;

      // Initialize the end address of the basic block, we'll extend it later on
      revng_log(Log,
                "Creating block starting at " << ID.toString()
                                              << " (preliminary ending is "
                                              << Block.End().toString() << ")");
      LoggerIndent Indent(Log);

      OnceQueue<BasicBlock *> Queue;
      Queue.insert(&BB);

      bool ReachesUnreachable = false;

      while (!Queue.empty()) {
        BasicBlock *Current = Queue.pop();
        revng_log(Log, "Analyzing block " << getName(Current));

        // If this block belongs to a single `newpc`, record its address
        MetaAddress CurrentBlockEnd = getFinalAddressOfBasicBlock(Current);
        if (CurrentBlockEnd.isValid() and CurrentBlockEnd > Block.End()) {
          revng_log(Log,
                    "Extending block end to " << CurrentBlockEnd.toString());
          Block.End() = CurrentBlockEnd;
        }

        if (isa<UnreachableInst>(Current->getTerminator())) {
          revng_log(Log, "Reaches unreachable");
          ReachesUnreachable = true;
        } else {
          revng_assert(succ_size(Current) > 0);
        }

        revng_log(Log, "Considering successors");
        LoggerIndent Indent2(Log);

        for (BasicBlock *Succ : successors(Current)) {
          revng_log(Log, "Considering successor " << getName(Succ));
          if (isa<ReturnInst>(Succ->getTerminator())) {
            revng_log(Log, "It's a ret");
            // Did we meet the end of the cloned function? Do nothing
            revng_assert(Succ->size() == 1);
          } else if (auto *Call = getCallTo(&*Succ->begin(),
                                            PreCallHook.get())) {
            // Handle edge for regular function calls
            if (auto MaybeEdge = handleCall(Call); MaybeEdge) {
              revng_log(Log, "It's a direct call, emitting a CallEdge");
              Block.Successors().insert(*MaybeEdge);
            }
          } else if (isJumpTarget(Succ)) {
            // TODO: handle situation in which it's a *direct* tail call.
            //       We might need an IBI here to know if the stack position is
            //       compatible with a tail call.
            BasicBlockID Destination = getBasicBlockID(Succ);
            revng_log(Log,
                      "It's a jump target: emitting a DirectBranch to "
                        << Destination.toString());
            auto Edge = makeEdge(Destination,
                                 efa::FunctionEdgeType::DirectBranch);
            Block.Successors().insert(Edge);
          } else if (Succ == OF->UnexpectedPCCloned) {
            revng_log(Log, "Reaches UnexpectedPC");
            ReachesUnexpectedPC = true;
          } else {

            revng_log(Log, "Nothing special, enqueue");
            // Not one of the cases above? Enqueue the successor basic block.
            Queue.insert(Succ);
          }
        }
      }

      bool HasNoSuccessor = Block.Successors().size() == 0;
      if (HasNoSuccessor) {
        if (ReachesUnreachable) {
          // If we reach any unreachable instruction, add a single unreachable
          // edge
          revng_log(Log, "Reaches unreachable, add to successors");
          revng_assert(Block.Successors().empty());
          using namespace efa::FunctionEdgeType;
          auto NewEdge = makeEdge(BasicBlockID::invalid(), Unreachable);
          Block.Successors().insert(NewEdge);
        } else if (ReachesUnexpectedPC) {
          // successor of the current basic block.
          revng_log(Log,
                    "No other successors other than UnexpectedPC, emitting "
                    "Unexpected");
          auto Edge = makeEdge(BasicBlockID::invalid(),
                               efa::FunctionEdgeType::Unexpected);
          Block.Successors().insert(Edge);
        }
      }

      // Commit the newly created block to the CFG
      CFG.insert(Block);
    }
  }

  return CFG;
}

CFGAnalyzer::State CFGAnalyzer::loadState(revng::IRBuilder &Builder) const {
  using namespace llvm;
  LLVMContext &Context = M.getContext();

  // Load the stack pointer
  auto *SP0 = Builder.createLoad(Globals.spReg());

  // Load the return address
  Value *ReturnAddress = nullptr;
  if (GlobalVariable *Register = Globals.raReg()) {
    ReturnAddress = Builder.createLoad(Register);
  } else {
    auto *OpaquePointer = PointerType::get(Context, 0);
    auto *StackPointerPointer = Builder.CreateIntToPtr(SP0, OpaquePointer);
    ReturnAddress = Builder.CreateLoad(Globals.pcReg()->getValueType(),
                                       StackPointerPointer);
  }

  // Load the PC
  auto DissectedPC = PCH->dissectJumpablePC(Builder,
                                            ReturnAddress,
                                            Binary->Architecture());
  Value *IntegerPC = MetaAddress::composeIntegerPC(Builder,
                                                   DissectedPC[0],
                                                   DissectedPC[1],
                                                   DissectedPC[2],
                                                   DissectedPC[3]);

  // Load all CSVs
  SmallVector<Value *, 16> CSVs;
  Type *IsRetTy = Type::getInt128Ty(Context);
  for (auto *CSR : ABICSVs) {
    auto *V = Builder.createLoad(CSR);
    CSVs.emplace_back(V);
  }

  return { SP0, IntegerPC, CSVs };
}

static cppcoro::generator<llvm::Instruction *>
previousInstructions(llvm::BasicBlock *BB) {
  do {
    for (Instruction &I : make_range(BB->rbegin(), BB->rend()))
      co_yield &I;
  } while ((BB = BB->getSinglePredecessor()));
}

void CFGAnalyzer::createIBIMarker(OutlinedFunction *Outlined) {
  using namespace llvm;
  using llvm::BasicBlock;

  revng::IRBuilder Builder(&Outlined->Function->getEntryBlock().front());

  State Initial = loadState(Builder);

  // Leave a trace of where the stack pointer is at the start of each block,
  // right after the marker the block begins with
  for (BasicBlock &BB : *Outlined->Function) {
    BasicBlockID ID = getBasicBlockID(&BB);
    if (not isJumpTarget(&BB) or not ID.isValid())
      continue;

    revng::IRBuilder HookBuilder(BB.getFirstNonPHI()->getNextNode());
    Value *BlockID = ID.toValue(&M);
    HookBuilder.CreateCall(StackHeightHook.get(),
                           { BlockID,
                             HookBuilder.createLoad(Globals.spReg()),
                             HookBuilder.getInt32(StackHeightAtBlockStart) });
  }

  // Turn the stack pointers the hooks were met with into heights of the stack
  for (CallBase *Call : callers(StackHeightHook.get())) {
    if (Call->getFunction() != Outlined->Function.get())
      continue;

    auto *Height = BinaryOperator::CreateSub(Call->getArgOperand(1),
                                             Initial.StackPointer,
                                             "",
                                             Call);
    Call->setArgOperand(1, Height);
  }

  //
  // Create IBI for this function
  //
  LLVMContext &Context = M.getContext();
  auto *IntTy = Globals.spReg()->getValueType();
  Type *I8Ptr = Type::getInt8PtrTy(Context);
  SmallVector<Type *, 16> ArgTypes;
  ArgTypes.resize(FirstPreservedRegisterIndex);
  ArgTypes[CallerBlockIDIndex] = I8Ptr;
  ArgTypes[CalledSymbolIndex] = I8Ptr;
  ArgTypes[JumpsToReturnAddressIndex] = Initial.ReturnPC->getType();
  ArgTypes[StackPointerOffsetIndex] = Initial.StackPointer->getType();
  ArgTypes[ReturnValuePreservedIndex] = Initial.ReturnPC->getType();
  for (auto *CSV : ABICSVs)
    ArgTypes.emplace_back(CSV->getValueType());

  auto *FTy = llvm::FunctionType::get(IntTy, ArgTypes, false);
  auto *IBI = IndirectBranchInfo.create(M, FTy, GlobalValue::ExternalLinkage)
                .function();
  Outlined->IndirectBranchInfoMarker = UniqueValuePtr<Function>(IBI);
  Outlined->IndirectBranchInfoMarker->addFnAttr(Attribute::NoUnwind);
  Outlined->IndirectBranchInfoMarker->addFnAttr(Attribute::NoReturn);

  // When an indirect jump is encountered we load the state at that point and
  // compare it against the initial state

  // Initialize markers for ABI analyses and set up the branches on which
  // `indirect_branch_info` will be installed.
  SmallVector<Instruction *, 16> BranchesToAnyPC;
  if (Outlined->AnyPCCloned == nullptr)
    return;

  SmallVector<BasicBlock *, 16> Predecessors;
  for (BasicBlock *Predecessor : predecessors(Outlined->AnyPCCloned))
    Predecessors.push_back(Predecessor);

  for (BasicBlock *Predecessor : Predecessors) {
    Instruction *Term = Predecessor->getTerminator();

    // Create a block and jump there
    auto *IBIBlock = BasicBlock::Create(Context,
                                        Predecessor->getName()
                                          + Twine("_indirect_branch_info"),
                                        Outlined->Function.get(),
                                        nullptr);

    Term->replaceUsesOfWith(Outlined->AnyPCCloned, IBIBlock);

    Builder.SetInsertPoint(IBIBlock);

    // Load the state
    State Final = loadState(Builder);

    // Prepare the arguments for the indirect_branch_info probe call
    SmallVector<Value *, 16> ArgValues;
    ArgValues.resize(FirstPreservedRegisterIndex);

    // Record the MetaAddress of the caller
    auto NewPCID = getBasicBlockID(getJumpTargetBlock(Term->getParent()));
    revng_assert(NewPCID.isValid());
    ArgValues[CallerBlockIDIndex] = NewPCID.toValue(getModule(Term));

    // Is this a return instruction? It is if the last thing written to the
    // program counter was read from where the stack pointer points
    for (Instruction *I : previousInstructions(Term->getParent())) {
      auto *Store = dyn_cast<StoreInst>(I);
      if (Store == nullptr or not PCH->affectsPC(Store))
        continue;

      if (auto *Target = dyn_cast<LoadInst>(Store->getValueOperand())) {
        Value *Address = Target->getPointerOperand();
        if (auto *ToPointer = dyn_cast<IntToPtrInst>(Address))
          Address = ToPointer->getOperand(0);
        if (auto *Extended = dyn_cast<ZExtInst>(Address))
          Address = Extended->getOperand(0);
        auto *Top = dyn_cast<LoadInst>(Address);
        if (Top != nullptr and Top->getPointerOperand() == Globals.spReg())
          PopsItsTarget.insert(NewPCID);
      }
      break;
    }

    // Record the name of the symbol, if any
    using CPN = ConstantPointerNull;
    Value *SymbolName = CPN::get(Type::getInt8PtrTy(Context));
    BasicBlock *BB = Term->getParent();

    for (Instruction *I : previousInstructions(BB)) {
      if (auto *Call = getCallTo(I, PreCallHook.get())) {
        SymbolName = Call->getArgOperand(2);
        break;
      }
    }

    ArgValues[CalledSymbolIndex] = SymbolName;

    // Compute the difference between the final PC at this program point and
    // the expected return address
    auto *FinalPC = PCH->composeIntegerPC(Builder);
    ArgValues[JumpsToReturnAddressIndex] = Builder.CreateSub(FinalPC,
                                                             Initial.ReturnPC);

    // Compute the difference between the initial stack pointer and the stack
    // pointer at this program point
    Value *Difference = Builder.CreateSub(Final.StackPointer,
                                          Initial.StackPointer);
    ArgValues[StackPointerOffsetIndex] = Difference;

    // Check if the expected ReturnPC has been preserved
    ArgValues[ReturnValuePreservedIndex] = Builder.CreateSub(Final.ReturnPC,
                                                             Initial.ReturnPC);

    // Compute the difference between the initial and final values of the CSVs
    for (const auto &[Initial, End] : zip(Initial.CSVs, Final.CSVs)) {
      auto *ABIRegistersDifference = Builder.CreateSub(Initial, End);
      ArgValues.emplace_back(ABIRegistersDifference);
    }

    // Emit the `indirect_branch_info` call
    Builder.CreateCall(Outlined->IndirectBranchInfoMarker.get(), ArgValues);
    Builder.CreateUnreachable();
  }
}

void CFGAnalyzer::opaqueBranchConditions(llvm::Function *F,
                                         revng::IRBuilder &IRB) {
  using namespace llvm;

  for (auto &BB : *F) {
    auto *Term = BB.getTerminator();
    if ((isa<BranchInst>(Term) && cast<BranchInst>(Term)->isConditional())
        || isa<SwitchInst>(Term)) {
      Value *Condition = isa<BranchInst>(Term) ?
                           cast<BranchInst>(Term)->getCondition() :
                           cast<SwitchInst>(Term)->getCondition();

      OpaqueBranchConditionsPool.addFnAttribute(Attribute::NoUnwind);
      auto MemoryEffects = MemoryEffects::inaccessibleMemOnly();
      OpaqueBranchConditionsPool.setMemoryEffects(MemoryEffects);
      OpaqueBranchConditionsPool.addFnAttribute(Attribute::WillReturn);
      OpaqueBranchConditionsPool.setTags({ &FunctionTags::UniquedByPrototype });

      auto *FTy = llvm::FunctionType::get(Condition->getType(),
                                          { Condition->getType() },
                                          false);

      auto *OpaqueTrueCallee = OpaqueBranchConditionsPool.get(FTy,
                                                              FTy,
                                                              "opaque_true");

      IRB.SetInsertPoint(Term);
      auto *RetVal = IRB.CreateCall(OpaqueTrueCallee, { Condition });

      if (isa<BranchInst>(Term))
        cast<BranchInst>(Term)->setCondition(RetVal);
      else
        cast<SwitchInst>(Term)->setCondition(RetVal);
    }
  }
}

void CFGAnalyzer::materializePCValues(llvm::Function *F,
                                      revng::IRBuilder &Builder) {
  for (llvm::BasicBlock &BB : *F) {
    for (llvm::Instruction &I : BB) {
      if (std::optional Call = NewPCHelper.getCall(&I)) {
        MetaAddress NewPC = blockIDFromNewPC(*Call).start();
        Builder.SetInsertPoint(Call->call());
        PCH->setPC(Builder, NewPC);
      }
    }
  }
}

namespace {

/// A function that aligns its stack pointer moves it down by an amount that
/// depends on where its caller left it. The function never looks at what it
/// skips, so what it does is the same whatever the amount: take it to be none,
/// as the passes that later lay out the stack frame do. Otherwise no height of
/// the stack is known past that point, and nothing can be learned from them.
class RemoveStackRealignmentPass
  : public llvm::PassInfoMixin<RemoveStackRealignmentPass> {
private:
  llvm::GlobalVariable *StackPointer = nullptr;

public:
  RemoveStackRealignmentPass(llvm::GlobalVariable *StackPointer) :
    StackPointer(StackPointer) {}

  llvm::PreservedAnalyses run(llvm::Function &F,
                              llvm::FunctionAnalysisManager &) {
    using namespace llvm;
    using namespace llvm::PatternMatch;

    bool Changed = false;
    for (Instruction &I : instructions(F)) {
      auto *Store = dyn_cast<StoreInst>(&I);
      if (Store == nullptr or Store->getPointerOperand() != StackPointer)
        continue;

      Value *Source = nullptr;
      const APInt *Mask = nullptr;
      if (not match(Store->getValueOperand(),
                    m_And(m_Value(Source), m_APInt(Mask))))
        continue;

      auto *Load = dyn_cast<LoadInst>(Source);
      if (Load == nullptr or Load->getPointerOperand() != StackPointer
          or not (-*Mask).isPowerOf2())
        continue;

      Store->setOperand(0, Load);
      Changed = true;
    }

    return Changed ? PreservedAnalyses::none() : PreservedAnalyses::all();
  }
};

/// The helper that probes the stack before a large frame is allocated
/// computes the new stack pointer as "the stack pointer minus the size or,
/// if that wraps around, zero", so that probing from there faults. Where the
/// function goes on, the frame did fit: the difference is the stack pointer,
/// and what comes after has a known height again.
class AssumeStackAllocationFitsPass
  : public llvm::PassInfoMixin<AssumeStackAllocationFitsPass> {
private:
  llvm::GlobalVariable *StackPointer = nullptr;

public:
  AssumeStackAllocationFitsPass(llvm::GlobalVariable *StackPointer) :
    StackPointer(StackPointer) {}

  llvm::PreservedAnalyses run(llvm::Function &F,
                              llvm::FunctionAnalysisManager &) {
    using namespace llvm;
    using namespace llvm::PatternMatch;

    SmallVector<std::pair<Instruction *, Value *>, 4> ToReplace;
    for (Instruction &I : instructions(F)) {
      Value *From = nullptr;
      Value *Size = nullptr;
      Value *Difference = nullptr;
      ICmpInst::Predicate Predicate;

      // Either as a saturating subtraction or as the select it comes from
      bool IsSaturating = match(&I,
                                m_Intrinsic<Intrinsic::usub_sat>(m_Value(From),
                                                                 m_Value(Size)));
      bool IsSelect = match(&I,
                            m_Select(m_ICmp(Predicate,
                                            m_Value(From),
                                            m_Value(Size)),
                                     m_Zero(),
                                     m_Value(Difference)))
                      and Predicate == ICmpInst::ICMP_ULT
                      and match(Difference,
                                m_Sub(m_Specific(From), m_Specific(Size)));
      if (not IsSelect) {
        IsSelect = match(&I,
                         m_Select(m_ICmp(Predicate,
                                         m_Value(From),
                                         m_Value(Size)),
                                  m_Value(Difference),
                                  m_Zero()))
                   and Predicate == ICmpInst::ICMP_UGE
                   and match(Difference,
                             m_Sub(m_Specific(From), m_Specific(Size)));
      }

      // With a size known when the code was compiled, the test is made on
      // the difference: it is either kept or replaced by zero, depending on
      // how it compares, as an unsigned number, with a constant
      // (The test can be on the stack pointer before the size is taken
      // from it or after, and what follows can have been added to both
      // values: a stack address is told from a small number, whatever the
      // test that chooses between them is.)
      if (not IsSaturating and not IsSelect) {
        Value *Kept = nullptr;
        ConstantInt *Otherwise = nullptr;
        auto Compared = m_ICmp(Predicate, m_Value(From), m_ConstantInt());
        if ((match(&I,
                   m_Select(Compared, m_Value(Kept), m_ConstantInt(Otherwise)))
             or match(&I,
                      m_Select(Compared,
                               m_ConstantInt(Otherwise),
                               m_Value(Kept))))
            and ICmpInst::isUnsigned(Predicate)
            and Otherwise->getValue().abs().ult(0x10000)
            and isStackAddress(Kept)) {
          IsSelect = true;
          Difference = Kept;
        }
      }

      if ((not IsSaturating and not IsSelect) or not isStackAddress(From))
        continue;

      if (IsSaturating)
        Difference = BinaryOperator::CreateSub(From, Size, "", &I);
      ToReplace.emplace_back(&I, Difference);
    }

    for (auto &[Old, New] : ToReplace) {
      Old->replaceAllUsesWith(New);
      Old->eraseFromParent();
    }

    return ToReplace.empty() ? PreservedAnalyses::all() :
                               PreservedAnalyses::none();
  }

private:
  /// \return whether \p V is the stack pointer the function is entered with,
  ///         or what a call left of it, plus a constant
  bool isStackAddress(llvm::Value *V) const {
    using namespace llvm;
    using namespace llvm::PatternMatch;

    for (unsigned Depth = 0; Depth < 16; ++Depth) {
      Value *Base = nullptr;
      if (match(V, m_Add(m_Value(Base), m_ConstantInt()))
          or match(V, m_Sub(m_Value(Base), m_ConstantInt()))) {
        V = Base;
        continue;
      }

      // The stack pointer is a pointer where the code is 32-bit: it is
      // computed with as a number of that size
      if (isa<TruncInst>(V) or isa<ZExtInst>(V) or isa<PtrToIntInst>(V)
          or isa<IntToPtrInst>(V) or isa<BitCastInst>(V)) {
        V = cast<Instruction>(V)->getOperand(0);
        continue;
      }

      auto *Load = dyn_cast<LoadInst>(V);
      return Load != nullptr and Load->getPointerOperand() == StackPointer;
    }

    return false;
  }
};

} // namespace

namespace {

/// Values as a value plus a constant, or as a constant alone (no value).
/// Where the paths of a function join with the same sum, computed on each of
/// them, LLVM is left with a phi of those: it is that sum.
class SameOnAllPaths {
public:
  using Result = std::pair<llvm::Value *, int64_t>;

private:
  /// What each value was found to be: each is looked at once. A phi that is
  /// met again while it is being worked out stands for itself.
  llvm::DenseMap<llvm::Value *, Result> Known;

  /// Whether a value is the value a cast is applied to: not where what is
  /// found has to replace the value
  bool ThroughCasts = true;

public:
  SameOnAllPaths(bool ThroughCasts = true) : ThroughCasts(ThroughCasts) {}

  Result of(llvm::Value *V) {
    using namespace llvm;

    if (auto *Constant = dyn_cast<ConstantInt>(V))
      return { nullptr, Constant->getSExtValue() };

    if (not isa<Instruction>(V))
      return { V, 0 };

    auto It = Known.find(V);
    if (It != Known.end()) {
      // A phi that stood for another one while that one was being worked
      // out is what that one turned out to be
      Result Found = It->second;
      for (unsigned Hops = 0; Hops < 8; ++Hops) {
        if (Found.first == nullptr or Found.first == V
            or not isa<PHINode>(Found.first))
          break;
        auto Next = Known.find(Found.first);
        if (Next == Known.end() or Next->second.first == Found.first)
          break;
        Found = { Next->second.first, Next->second.second + Found.second };
      }
      return Found;
    }

    Known[V] = Result{ V, 0 };
    Result New = compute(V);
    Known[V] = New;
    return New;
  }

  /// Make the argument \p Index of \p Call the constant it amounts to, if it
  /// is one LLVM does not see
  void fold(llvm::CallBase *Call, unsigned Index) {
    using namespace llvm;
    Value *Argument = Call->getArgOperand(Index);
    if (isa<ConstantInt>(Argument) or not Argument->getType()->isIntegerTy())
      return;

    auto &&[Base, Offset] = of(Argument);
    if (Base != nullptr)
      return;

    unsigned Bits = Argument->getType()->getIntegerBitWidth();
    APInt Folded(64, uint64_t(Offset), true);
    Call->setArgOperand(Index,
                        ConstantInt::get(Argument->getType(),
                                         Folded.sextOrTrunc(Bits)));
  }

private:
  Result compute(llvm::Value *V) {
    using namespace llvm;

    if (ThroughCasts
        and (isa<TruncInst>(V) or isa<ZExtInst>(V) or isa<PtrToIntInst>(V)
             or isa<IntToPtrInst>(V) or isa<BitCastInst>(V)))
      return of(cast<Instruction>(V)->getOperand(0));

    if (auto *Operator = dyn_cast<BinaryOperator>(V)) {
      bool IsAdd = Operator->getOpcode() == Instruction::Add;
      if (IsAdd or Operator->getOpcode() == Instruction::Sub) {
        Result Left = of(Operator->getOperand(0));
        Result Right = of(Operator->getOperand(1));
        if (Right.first == nullptr)
          return { Left.first,
                   IsAdd ? Left.second + Right.second :
                           Left.second - Right.second };
        if (IsAdd and Left.first == nullptr)
          return { Right.first, Left.second + Right.second };
        if (not IsAdd and Left.first == Right.first)
          return { nullptr, Left.second - Right.second };
      }
      return { V, 0 };
    }

    if (auto *Phi = dyn_cast<PHINode>(V)) {
      std::optional<Result> Common;
      for (Value *Incoming : Phi->incoming_values()) {
        if (Incoming == Phi)
          continue;
        Result New = of(Incoming);
        if (New.first == Phi and New.second == 0)
          continue;
        if (Common.has_value() and *Common != New)
          return { V, 0 };
        Common = New;
      }
      if (Common.has_value())
        return *Common;
    }

    return { V, 0 };
  }
};

/// Where the paths of a function join with the same stack pointer, computed
/// on each of them, LLVM keeps a phi (it does not see through the phis of a
/// loop that refer to each other, nor through sums made in different blocks):
/// an access to memory through it is not seen as an access to the stack, and
/// what the function saved there (its return address, the registers it
/// restores) is not found again. The phi is replaced by the stack pointer the
/// function is entered with plus the constant all the paths bring.
///
/// Only that: the form the other values have is what the passes after this
/// one expect.
class MergeSameOnAllPathsPass
  : public llvm::PassInfoMixin<MergeSameOnAllPathsPass> {
private:
  llvm::GlobalVariable *StackPointer = nullptr;

public:
  explicit MergeSameOnAllPathsPass(llvm::GlobalVariable *StackPointer) :
    StackPointer(StackPointer) {}

  llvm::PreservedAnalyses run(llvm::Function &F,
                              llvm::FunctionAnalysisManager &FAM) {
    using namespace llvm;
    using llvm::BasicBlock;

    // The stack pointer the function is entered with: what is read from the
    // register at the start, before anything is written to it
    SmallPtrSet<Value *, 4> EntryLoads;
    for (Instruction &I : F.getEntryBlock()) {
      if (auto *Store = dyn_cast<StoreInst>(&I)) {
        if (skipCasts(Store->getPointerOperand()) == StackPointer)
          break;
      } else if (auto *Load = dyn_cast<LoadInst>(&I)) {
        if (skipCasts(Load->getPointerOperand()) == StackPointer)
          EntryLoads.insert(Load);
      }
    }

    // (Sums are followed through the casts between the stack pointer and the
    // numbers the code computes with: for a value of the size of the stack
    // pointer what comes out is exact)
    SameOnAllPaths Same;
    SmallVector<std::pair<PHINode *, SameOnAllPaths::Result>, 8> ToReplace;
    for (BasicBlock &BB : F) {
      if (&BB == &F.getEntryBlock())
        continue;

      for (PHINode &Phi : BB.phis()) {
        if (Phi.getType() != StackPointer->getValueType())
          continue;

        SameOnAllPaths::Result Found = Same.of(&Phi);
        if (Found.first != nullptr and EntryLoads.contains(Found.first))
          ToReplace.emplace_back(&Phi, Found);
      }
    }

    const DataLayout &DL = F.getParent()->getDataLayout();
    for (auto &[Phi, Found] : ToReplace) {
      revng::IRBuilder Builder(&*Phi->getParent()->getFirstInsertionPt());
      Value *Base = Found.first;
      if (Base->getType()->isPointerTy())
        Base = Builder.CreatePtrToInt(Base, DL.getIntPtrType(Base->getType()));
      Base = Builder.CreateZExtOrTrunc(Base, Phi->getType());
      auto *Offset = ConstantInt::getSigned(Phi->getType(), Found.second);
      Phi->replaceAllUsesWith(Builder.CreateAdd(Base, Offset));
      Phi->eraseFromParent();
    }

    return ToReplace.empty() ? PreservedAnalyses::all() :
                               PreservedAnalyses::none();
  }
};

} // namespace

void CFGAnalyzer::runOptimizationPipeline(llvm::Function *F) {
  using namespace llvm;

  // Some LLVM passes used later in the pipeline scan for cut-offs, meaning that
  // further computation may not be done when they are reached; making some
  // optimizations opportunities missed. Hence, we set the involved thresholds
  // (e.g., the maximum value that MemorySSA uses to take into account
  // stores/phis) to have initial unbounded value.
  static constexpr const char *MemSSALimit = "memssa-check-limit";
  static constexpr const char *MemDepBlockLimit = "memdep-block-scan-limit";

  using TemporaryUOption = TemporaryLLVMOption<unsigned>;
  TemporaryUOption MemSSALimitOption(MemSSALimit, UINT_MAX);
  TemporaryUOption MemDepBlockLimitOption(MemDepBlockLimit, UINT_MAX);

  // TODO: break it down in the future, and check if some passes can be dropped
  {
    FunctionPassManager FPM;

    // First stage: simplify the IR, promote the CSVs to local variables,
    // compute subexpressions elimination and resolve redundant expressions in
    // order to compute the stack height.
    FPM.addPass(RemoveNewPCCallsPass());
    FPM.addPass(RemoveStackRealignmentPass(Globals.spReg()));
    FPM.addPass(RemoveHelperCallsPass(Globals.spReg()));
    FPM.addPass(PromoteGlobalToLocalPass());
    FPM.addPass(SimplifyCFGPass());
    FPM.addPass(SROAPass(SROAOptions::ModifyCFG));
    FPM.addPass(EarlyCSEPass(true));
    FPM.addPass(JumpThreadingPass());

    // No other pass after JumpThreading depends or uses LVI.
    // LazyValueAnalysis registers deletion callbacks that get called by
    // following modifications of the IR. The cost of calling these callbacks
    // can get very high, and nobody will use the results anyway.
    // So let's invalidate LazyValueAnalysis straight after JT.
    FPM.addPass(InvalidateAnalysisPass<LazyValueAnalysis>());

    FPM.addPass(UnreachableBlockElimPass());
    FPM.addPass(InstCombinePass());
    FPM.addPass(AssumeStackAllocationFitsPass(Globals.spReg()));
    FPM.addPass(EarlyCSEPass(true));
    FPM.addPass(SimplifyCFGPass());
    FPM.addPass(MergedLoadStoreMotionPass());
    FPM.addPass(GVNPass());

    // The registers through which code reaches the block of its thread: on
    // x86, the bases of two segments
    SmallVector<GlobalVariable *, 2> ThreadPointers;
    for (const char *Name : { "_fs_base", "_gs_base" })
      if (GlobalVariable *CSV = M.getGlobalVariable(Name, true))
        ThreadPointers.push_back(CSV);

    // Second stage: add alias analysis info and canonicalize `i2p` + `add` into
    // `getelementptr` instructions. Since the IR may change remarkably, another
    // round of passes is necessary to take more optimization opportunities.
    using SDSAP = SegregateDirectStackAccessesPass;
    FPM.addPass(SDSAP(Globals.spReg(), ThreadPointers));
    FPM.addPass(EarlyCSEPass(true));
    FPM.addPass(AssumeStackAllocationFitsPass(Globals.spReg()));
    FPM.addPass(InstCombinePass());
    FPM.addPass(GVNPass());

    // The address of an access can become the stack pointer plus a constant
    // only now, where the paths that reach it leave the stack pointer the
    // same: such an access was taken for one that is not to the stack, and
    // what was stored there is not found. Tell the accesses apart again;
    // first, where LLVM has kept a phi of the same stack pointer, put the
    // stack pointer in its place.
    FPM.addPass(MergeSameOnAllPathsPass(Globals.spReg()));
    FPM.addPass(SDSAP(Globals.spReg(), ThreadPointers));
    FPM.addPass(GVNPass());

    // Third stage: if enabled, serialize the results and dump the functions on
    // disk with the alias information included as comments.
    if (IndirectBranchInfoSummaryPath.getNumOccurrences() == 1)
      FPM.addPass(IndirectBranchInfoPrinterPass(*OutputIBI));

    if (AAWriterPath.getNumOccurrences() == 1)
      FPM.addPass(AAWriterPass(*OutputAAWriter));

    ModuleAnalysisManager MAM;

    FunctionAnalysisManager FAM;
    FAM.registerPass([] {
      AAManager AA;
      AA.registerFunctionAnalysis<BasicAA>();
      AA.registerFunctionAnalysis<ScopedNoAliasAA>();

      return AA;
    });
    FAM.registerPass([&] { return ModuleAnalysisManagerFunctionProxy(MAM); });

    PassBuilder PB;
    PB.registerFunctionAnalyses(FAM);
    PB.registerModuleAnalyses(MAM);

    FPM.run(*F, FAM);

    // The last steps can be what brings the probe of the stack into the form
    // that is recognized: once more, and only then fold what it leaves
    AssumeStackAllocationFitsPass Fits(Globals.spReg());
    if (not Fits.run(*F, FAM).areAllPreserved()) {
      FAM.invalidate(*F, PreservedAnalyses::none());
      FunctionPassManager Again;
      Again.addPass(InstCombinePass());
      Again.addPass(GVNPass());
      Again.run(*F, FAM);
    }
  }
}

class ClobberedRegistersRegistry {
public:
  using CSVVector = llvm::SmallVector<GlobalVariable *, 16>;

private:
  const CSVVector &ABICSVs;
  CSVSet ClobberedRegs;

public:
  ClobberedRegistersRegistry(const CSVVector &ABICSVs) : ABICSVs(ABICSVs) {}

public:
  const CSVSet &getClobberedRegisters() const { return ClobberedRegs; }

public:
  void recordClobberedRegisters(llvm::CallBase *CI) {
    using namespace llvm;
    for (unsigned I = FirstPreservedRegisterIndex; I < CI->arg_size(); ++I) {
      auto *Register = dyn_cast<ConstantInt>(CI->getArgOperand(I));
      if (Register == nullptr or Register->getZExtValue() != 0)
        ClobberedRegs.insert(ABICSVs[I - FirstPreservedRegisterIndex]);
    }
  }

  void add(const CSVSet &Clobbered) {
    for (auto *GV : Clobbered)
      ClobberedRegs.insert(GV);
  }
};

/// Elect a final stack offset to tell whether the function is leaving
/// the stack pointer higher than it was at the function entry.
static std::optional<int64_t> electFSO(const auto &MaybeReturns) {
  auto It = std::min_element(MaybeReturns.begin(),
                             MaybeReturns.end(),
                             [](const auto &LHS, const auto &RHS) {
                               return std::get<1>(LHS) < std::get<1>(RHS);
                             });
  if (It == MaybeReturns.end())
    return {};
  return std::get<1>(*It);
}

static bool isNoReturn(const model::Binary &Binary,
                       FunctionSummaryOracle &Oracle,
                       const efa::CallEdge &Edge) {
  if (Edge.Attributes().contains(model::FunctionAttribute::NoReturn))
    return true;

  if (not Edge.DynamicFunction().empty())
    return Binary.ImportedDynamicFunctions()
      .at(Edge.DynamicFunction())
      .Attributes()
      .contains(model::FunctionAttribute::NoReturn);

  if (Edge.Destination().isValid())
    return Oracle.getLocalFunction(Edge.Destination().notInlinedAddress())
      .Attributes.contains(model::FunctionAttribute::NoReturn);

  return false;
}

FunctionSummary CFGAnalyzer::milkInfo(OutlinedFunction *OutlinedFunction,
                                      SortedVector<efa::BasicBlock> &&CFG) {
  using namespace llvm;
  using namespace efa::FunctionEdgeType;
  using namespace model::Architecture;
  int64_t CallPushSize = getCallPushSize(Binary->Architecture());

  revng_log(Log, "Milking info for " << OutlinedFunction->Address.toString());
  LoggerIndent Ident(Log);

  revng_log(Log, "Initial CFG:\n");
  if (Log.isEnabled()) {
    LoggerIndent Ident(Log);
    for (const efa::BasicBlock &Block : CFG) {
      serialize(Log, Block);
      Log << DoLog;
    }
  }

  using EdgeType = UpcastablePointer<efa::FunctionEdgeBase>;
  SmallVector<std::pair<CallBase *, EdgeType>, 4> IBIResult;

  // Temporary bins for IBIs that need extra processing
  SmallVector<std::tuple<CallBase *, int64_t, const FunctionSummary *>, 4>
    TailCalls;
  SmallVector<std::pair<CallBase *, int64_t>, 4> MaybeReturns;
  SmallVector<std::tuple<CallBase *, int64_t, const FunctionSummary *>, 4>
    MaybeIndirectTailCalls;

  for (CallBase *CI :
       callers(OutlinedFunction->IndirectBranchInfoMarker.get())) {
    if (CI->getParent()->getParent() != OutlinedFunction->Function.get())
      continue;

    Value *Argument = nullptr;
    bool JumpsToReturnAddress = false;

    // Is this indirect branch targeting jumping to the return address?
    {
      Argument = CI->getArgOperand(JumpsToReturnAddressIndex);
      auto *ConstantOffset = dyn_cast<ConstantInt>(Argument);
      if (ConstantOffset)
        JumpsToReturnAddress = ConstantOffset->getSExtValue() == 0;
    }

    const efa::BasicBlock &Block = blockFromIndirectBranchInfo(CI, CFG);

    // Is this a tail call? If so, we are very interested in the FSO since
    // it's useful to determine the FSO of the caller
    auto *CalledSymbolArgument = CI->getArgOperand(CalledSymbolIndex);
    StringRef CalledSymbol = extractFromConstantStringPtr(CalledSymbolArgument);
    auto &&[Summary, IsTailCall] = Oracle.getCallSite(OutlinedFunction->Address,
                                                      Block.ID(),
                                                      MetaAddress::invalid(),
                                                      CalledSymbol);

    Argument = CI->getArgOperand(StackPointerOffsetIndex);
    auto *StackPointerOffset = dyn_cast<ConstantInt>(Argument);
    if (StackPointerOffset != nullptr) {
      int64_t FSO = StackPointerOffset->getSExtValue();
      if (JumpsToReturnAddress) {
        if (FSO >= CallPushSize) {
          // We're jumping to the return address with a non-negative stack
          // offset. This might be a return instruction. Note it down as a
          // candidate
          MaybeReturns.emplace_back(CI, FSO);
        } else {
          // We're jumping to the return address but we're leaving the stack
          // higher than it was initially, it might be the return instruction
          // of a function to inline. Record it as BrokenReturn.
          IBIResult.emplace_back(CI, makeIndirectEdge(BrokenReturn));
        }
      } else if (IsTailCall) {
        if (Summary->ElectedFSO.has_value()) {
          // We have a tail call for which the FSO is known! We can exploit this
          // to know this function's FSO!
          TailCalls.emplace_back(CI, FSO + *Summary->ElectedFSO, Summary);
        }
      } else if (PopsItsTarget.contains(Block.ID())) {
        // A return instruction that does not find the return address where
        // it looks for it. It was taken for a tail call to whatever is there
        // (what a callee was assumed to leave on the stack, for instance, an
        // argument): something before it is wrong, and saying so makes it be
        // looked for.
        IBIResult.emplace_back(CI, makeIndirectEdge(LongJmp));
      } else {
        Argument = CI->getArgOperand(ReturnValuePreservedIndex);
        auto *ReturnValuePreserved = dyn_cast<ConstantInt>(Argument);
        if (ReturnValuePreserved != nullptr
            and getLimitedValue(ReturnValuePreserved) == 0
            and Summary->ElectedFSO.has_value()) {
          // Here the stack pointer offset is known and the value of the
          // return address (in the link register/top of the stack) has been
          // preserved.
          // This is definitely not a return but it might be an indirect
          // tail call.
          MaybeIndirectTailCalls.emplace_back(CI,
                                              FSO + *Summary->ElectedFSO,
                                              Summary);
        } else {
          // The return address is not preserved, something fishy is going on
          IBIResult.emplace_back(CI, makeIndirectEdge(LongJmp));
        }
      }
    } else if (JumpsToReturnAddress) {
      // We're jumping to the return address, but where we leave the stack
      // pointer is not known here: it depends on what the caller passed, as
      // with the helpers compilers emit to set up a frame or to probe the
      // stack. It might be known in the caller. Record it as BrokenReturn.
      IBIResult.emplace_back(CI, makeIndirectEdge(BrokenReturn));
    } else {
      // We're leaving the stack pointer in an unknown state
      IBIResult.emplace_back(CI, makeIndirectEdge(LongJmp));
    }
  }

  if (Log.isEnabled()) {
    Log << "TailCalls:\n";
    for (const auto &[Call, FSO, Summary] : TailCalls) {
      Log << "  " << ::getName(Call) << " (FSO: " << FSO << ")\n";
    }
    Log << DoLog;

    Log << "MaybeReturns:\n";
    for (const auto &[Call, FSO] : MaybeReturns) {
      Log << "  " << ::getName(Call) << " (FSO: " << FSO << ")\n";
    }
    Log << DoLog;

    Log << "MaybeIndirectTailCalls:\n";
    for (const auto &[Call, FSO, Summary] : MaybeIndirectTailCalls) {
      Log << "  " << ::getName(Call) << " (FSO: " << FSO << ")\n";
    }
    Log << DoLog;
  }

  // Elect a final stack offset
  std::optional<int64_t> MaybeWinFSO;

  // Elect a set of clobbered registers on all return points (i.e., actual
  // returns + actual tail calls)

  ClobberedRegistersRegistry ClobberedRegisters(ABICSVs);

  //
  // Process TailCalls
  //

  // Check all tail calls have voted the same FSO, emit a warning otherwise
  if (TailCalls.size() > 0) {
    MaybeWinFSO = electFSO(TailCalls);
    bool Different = false;
    for (auto &&[CI, FSO, Summary] : TailCalls) {
      if (FSO == *MaybeWinFSO) {
        ClobberedRegisters.recordClobberedRegisters(CI);
        ClobberedRegisters.add(Summary->ClobberedRegisters);
      } else {
        // We found a tail call that leads to a FSO that's not coherent with
        // the elected one. Purge it turning it into a LongJmp.
        Different = true;
        efa::BasicBlock &Block = blockFromIndirectBranchInfo(CI, CFG);
        revng_assert(Block.Successors().size() == 1);
        auto OldEdge = cast<efa::CallEdge>(Block.Successors().begin()->get());
        revng_assert(OldEdge->IsTailCall());
        Block.Successors() = { makeIndirectEdge(LongJmp) };
      }
    }

    if (Different) {
      revng_log(Log,
                "We have multiple tail calls which lead to uncoherent final "
                "stack sizes");
    }
  }

  //
  // Process MaybeReturns
  //

  // Use candidate returns to elect FSO, if necessary
  if (MaybeReturns.size() > 0) {
    if (not MaybeWinFSO.has_value())
      MaybeWinFSO = electFSO(MaybeReturns);
    revng_assert(MaybeWinFSO.has_value());
  }

  // Divide entries in MaybeReturns in Returns and BrokenReturns
  for (const auto &[CI, FSO] : MaybeReturns) {
    if (MaybeWinFSO.has_value() and FSO == *MaybeWinFSO) {
      // It's a return!
      IBIResult.emplace_back(CI, makeIndirectEdge(Return));
      ClobberedRegisters.recordClobberedRegisters(CI);
    } else {
      // It's jumping to the return address but leaves the stack pointer in a
      // state not compatible with being a return instruction, it might be the
      // return instruction of a function to inline. Record it as BrokenReturn.
      IBIResult.emplace_back(CI, makeIndirectEdge(BrokenReturn));
    }
  }

  //
  // Process MaybeIndirectTailCalls
  //

  // If no FSO was elected, retry focusing  on MaybeIndirectTailCalls
  bool HasOwnFSO = MaybeWinFSO.has_value();
  if (not HasOwnFSO) {
    revng_assert(MaybeReturns.size() == 0);
    MaybeWinFSO = electFSO(MaybeIndirectTailCalls);
  }

  for (const auto &[CI, FSO, Summary] : MaybeIndirectTailCalls) {
    auto *Argument = CI->getArgOperand(CalledSymbolIndex);
    StringRef Symbol = extractFromConstantStringPtr(Argument);

    // What is called this way returns in place of this function. If nothing
    // is known about it, and this function has returns of its own, it removes
    // from the stack what is left for them to remove: that is not to be
    // assumed, it follows.
    bool IsKnown = false;
    if (not Symbol.empty()) {
      const auto &Imported = Binary->ImportedDynamicFunctions();
      auto It = Imported.find(Symbol.str());
      IsKnown = It != Imported.end() and not It->Prototype().isEmpty();
    }
    efa::BasicBlock &Block = blockFromIndirectBranchInfo(CI, CFG);
    MetaAddress Site = Block.ID().notInlinedAddress();
    const model::Function *Function = Binary->Functions()
                                        .tryGet(OutlinedFunction->Address);
    if (Function != nullptr and Function->CallSitePrototypes().contains(Site))
      IsKnown = true;

    int64_t Removed = 0;
    if (HasOwnFSO)
      Removed = *MaybeWinFSO - (FSO - *Summary->ElectedFSO);
    bool Follows = HasOwnFSO and not IsKnown and Removed >= CallPushSize;

    if (MaybeWinFSO.has_value() and (FSO == *MaybeWinFSO or Follows)) {
      if (FSO != *MaybeWinFSO)
        CallSiteFSO[Site] = Removed;

      auto NewEdge = makeCall(MetaAddress::invalid());
      auto *Call = cast<efa::CallEdge>(NewEdge.get());
      Call->IsTailCall() = true;
      Call->DynamicFunction() = Symbol;
      IBIResult.emplace_back(CI, std::move(NewEdge));
      ClobberedRegisters.recordClobberedRegisters(CI);
      ClobberedRegisters.add(Summary->ClobberedRegisters);
    } else {
      IBIResult.emplace_back(CI, makeIndirectEdge(LongJmp));
    }
  }

  if (Log.isEnabled()) {
    Log << "MaybeWinFSO: ";
    if (MaybeWinFSO)
      Log << *MaybeWinFSO;
    else
      Log << "(nullopt)";
    Log << DoLog;

    Log << "ClobberedRegisters: {";
    const char *Prefix = "";
    for (GlobalVariable *CSV : ClobberedRegisters.getClobberedRegisters()) {
      Log << Prefix << CSV->getName();
      Prefix = ", ";
    }
    Log << "}" << DoLog;

    Log << "IBIResults:\n";
    for (const auto &[Call, Edge] : IBIResult) {
      Log << "  " << ::getName(Call) << ":\n";
      serialize(Log, Edge);
    }
    Log << DoLog;
  }

  //
  // Commit  IBIResults to the CFG
  //
  for (const auto &[CI, Edge] : IBIResult) {
    efa::BasicBlock &Block = blockFromIndirectBranchInfo(CI, CFG);
    Block.Successors().insert(std::move(Edge));
  }

  // Collect summary for information
  using efa::CallEdge;
  bool FoundReturn = false;
  bool FoundBrokenReturn = false;
  int BrokenReturnCount = 0;
  int NoReturnCount = 0;
  for (const auto &[CI, Edge] : IBIResult) {
    using namespace model::FunctionAttribute;
    auto *Call = dyn_cast<CallEdge>(Edge.get());
    if (Edge->Type() == Return) {
      FoundReturn = true;
    } else if (Call != nullptr and Call->IsTailCall()
               and not isNoReturn(*Binary, Oracle, *Call)) {
      FoundReturn = true;
    } else if (Edge->Type() == BrokenReturn) {
      FoundBrokenReturn = true;
      BrokenReturnCount++;
    } else {
      NoReturnCount++;
    }
  }

  revng_log(Log, "FoundReturn: " << FoundReturn);
  revng_log(Log, "FoundBrokenReturn: " << FoundBrokenReturn);
  revng_log(Log, "BrokenReturnCount: " << BrokenReturnCount);
  revng_log(Log, "NoReturnCount: " << NoReturnCount);

  // Function is elected to inline if there is one and only one broken return
  AttributesSet Attributes;
  if (FoundReturn) {
    // Do nothing
  } else if (FoundBrokenReturn && BrokenReturnCount == 1
             && NoReturnCount == 0) {
    Attributes.insert(model::FunctionAttribute::HasOneBrokenReturn);
  } else {
    Attributes.insert(model::FunctionAttribute::NoReturn);
  }

  if (IsTrial)
    for (efa::BasicBlock &Block : CFG)
      if (Block.Successors().empty())
        Block.Successors().insert(makeIndirectEdge(LongJmp));

  for (efa::BasicBlock &Block : CFG)
    revng_assert(Block.Successors().size() > 0);

  revng_log(Log, "Final CFG:\n");
  if (Log.isEnabled()) {
    LoggerIndent Ident(Log);
    for (const efa::BasicBlock &Block : CFG) {
      serialize(Log, Block);
      Log << DoLog;
    }
  }

  FunctionSummary Result(Attributes,
                         ClobberedRegisters.getClobberedRegisters(),
                         {},
                         std::move(CFG),
                         MaybeWinFSO);

  if (Log.isEnabled()) {
    Log << "Result:\n";
    Result.dump(Log);
    Log << DoLog;
  }

  return Result;
}

namespace {

/// How well an analysis explains a function
struct AnalysisScore {
  /// The indirect branches the function leaves through as its caller expects:
  /// the returns and the tail calls
  unsigned Returns = 0;

  /// The indirect branches taken for a longjmp
  unsigned LongJmps = 0;

  /// The registers the function clobbers although neither the default
  /// prototype nor one of its callees does
  unsigned Clobbered = 0;

  /// The calls at which the height of the stack is not known
  unsigned UnknownHeights = 0;

  bool leavesNothingToExplain() const {
    return LongJmps == 0 and Clobbered == 0 and UnknownHeights == 0;
  }

  bool leavesSomethingToExplain() const {
    return LongJmps != 0 or (Returns != 0 and Clobbered != 0)
           or UnknownHeights != 0;
  }

  /// Either more returns or tail calls in place of longjmps, or the same
  /// branches and fewer registers clobbered, or all the same and the height
  /// of the stack known at more calls. A return that does not come with the
  /// registers the function saved is not one: it is where code that is not
  /// reached, such as what follows a call that never returns, happens to end.
  bool isBetterThan(const AnalysisScore &Other) const {
    if (Returns > Other.Returns and LongJmps < Other.LongJmps)
      return Clobbered <= Other.Clobbered;
    if (Returns != Other.Returns or LongJmps != Other.LongJmps)
      return false;
    if (Clobbered != Other.Clobbered)
      return Clobbered < Other.Clobbered
             and UnknownHeights <= Other.UnknownHeights;
    return UnknownHeights < Other.UnknownHeights;
  }

  auto rank() const {
    return std::tuple(Returns,
                      -int64_t(LongJmps),
                      -int64_t(Clobbered),
                      -int64_t(UnknownHeights));
  }
};

/// A callee nothing is known about: that of a call site or, at all the sites
/// where the function calls itself, the function
struct UnknownCallSite {
  llvm::SmallVector<MetaAddress, 1> Blocks;

  /// Whether the call is in the block its function starts with
  bool IsFirst = false;

  /// Whether the callee is the function itself
  bool IsRecursive = false;

  /// The dynamic function called, if it is one: wherever it is called, it
  /// removes the same
  std::string Symbol;

  /// For each block, the address of the instruction that makes the call
  llvm::SmallVector<MetaAddress, 1> Instructions;

  /// Whether the function leaves no choice about what the callee removes
  bool IsRequired = false;

  /// What the callee cannot remove more than: what the function saved
  /// before the call stays where it is
  std::optional<uint64_t> Bound;

  /// Whether the function only says what this callee and others remove
  /// together
  bool IsAmbiguous = false;

  /// What its callee most likely removes from the stack, in words, and the
  /// most it can remove
  uint64_t Likeliest = 0;
  uint64_t Most = 0;
};

} // namespace

namespace {

/// What the heights of the stack of a function say about the unknown amounts
/// its callees move the stack pointer by.
///
/// With such an amount for each callee nothing is known about, a height is a
/// sum of unknowns and a constant. The paths that join at a point of the
/// function agree on the height there: that is an equation for each of them.
class StackEquations {
private:
  /// The unknowns of the callees come first; then, as they are met, the phis
  /// and the values that are not sums
  struct Sum {
    std::map<unsigned, int64_t> Terms;
    int64_t Constant = 0;
    bool IsValid = true;

    void add(const Sum &Other, int64_t Factor) {
      using Wide = __int128;
      constexpr int64_t Limit = int64_t(1) << 40;
      auto Fits = [](Wide Value) { return Value > -Limit and Value < Limit; };

      IsValid = IsValid and Other.IsValid;
      for (const auto &[Unknown, Coefficient] : Other.Terms) {
        Wide New = Wide(Terms[Unknown]) + Wide(Coefficient) * Factor;
        IsValid = IsValid and Fits(New);
        if (New == 0)
          Terms.erase(Unknown);
        else
          Terms[Unknown] = New;
      }

      Wide New = Wide(Constant) + Wide(Other.Constant) * Factor;
      IsValid = IsValid and Fits(New);
      Constant = New;
    }

    void scale(int64_t Factor) {
      Sum Old = std::move(*this);
      *this = Sum();
      add(Old, Factor);
    }

    bool operator==(const Sum &Other) const {
      return Terms == Other.Terms and Constant == Other.Constant
             and IsValid == Other.IsValid;
    }

    /// Divide by what all the coefficients have in common
    void normalize() {
      int64_t Common = std::abs(Constant);
      for (const auto &[Unknown, Coefficient] : Terms)
        Common = std::gcd(Common, std::abs(Coefficient));
      if (Common <= 1)
        return;
      Constant /= Common;
      for (auto &[Unknown, Coefficient] : Terms)
        Coefficient /= Common;
    }
  };

  /// A call to a callee nothing is known about, and the height of the stack
  /// it is made at
  struct Site {
    unsigned Unknown = 0;
    llvm::Instruction *Hook = nullptr;
    Sum Height;
  };

  /// A register the function saves, and the height of the slot it goes in
  struct Save {
    llvm::Instruction *Store = nullptr;
    Sum Slot;
  };

  /// A register the function is left with after reading it from the stack:
  /// what it had when the function was entered, the instruction that reads
  /// it back and the height it is read at
  struct ReadBack {
    llvm::Value *Initial = nullptr;
    llvm::Instruction *Read = nullptr;
    Sum From;
  };

  /// Something read from the stack at the height From that the function
  /// stores on the stack again, at the height Slot: if what was read is a
  /// register the function had saved there, this saves it once more. The
  /// heights are vague if they are not sums of what the equations are about.
  struct Resave {
    llvm::Instruction *Store = nullptr;
    Sum Slot;
    Sum From;
    bool IsVague = false;
  };

private:
  unsigned Callees = 0;
  llvm::Function *StackEffect = nullptr;
  llvm::GlobalVariable *StackPointer = nullptr;
  const CSVSet *CalleeSaved = nullptr;
  std::vector<Site> Sites;
  std::vector<Save> Saves;
  std::vector<ReadBack> ReadBacks;
  int64_t Least = 0;
  llvm::DenseMap<llvm::Value *, unsigned> Others;

  /// The values of the unknowns that are not a callee's, in their order
  std::vector<llvm::Value *> ValueOf;

  /// The sums \ref isStackAddress has looked at for the address it is asked
  /// about
  unsigned Visits = 0;
  llvm::DenseMap<llvm::Value *, Sum> Known;
  std::vector<llvm::PHINode *> Pending;

  /// Each is a sum that is zero
  std::vector<Sum> Equations;

  /// Those that say a register is read back from where it was saved
  std::vector<Sum> Restores;

public:
  /// \p Least is what a callee that removes nothing amounts to: none amounts
  /// to less
  StackEquations(unsigned Callees,
                 llvm::Function *StackEffect,
                 llvm::GlobalVariable *StackPointer,
                 const CSVSet *CalleeSaved,
                 int64_t Least) :
    Callees(Callees),
    StackEffect(StackEffect),
    StackPointer(StackPointer),
    CalleeSaved(CalleeSaved),
    Least(Least) {}

  /// The callee \p Unknown is called where \p Hook is, with the stack at the
  /// height \p Hook has
  void visitSite(unsigned Unknown, llvm::CallBase *Hook) {
    Sites.push_back({ Unknown, Hook, sumOf(Hook->getArgOperand(1)) });
  }

  /// If \p Store puts on the stack what a register the function has to
  /// preserve had when the function was entered, it saves it: what is there
  /// stays there until the function restores it
  void visitStore(llvm::StoreInst *Store) {
    using namespace llvm;
    auto *Value = dyn_cast<LoadInst>(Store->getValueOperand());
    if (Value == nullptr)
      return;
    auto *CSV = dyn_cast<GlobalVariable>(skipCasts(Value->getPointerOperand()));
    if (CSV == nullptr or not CalleeSaved->contains(CSV))
      return;

    std::optional<Sum> Slot = heightOf(Store->getPointerOperand());
    join();
    if (Slot.has_value())
      Saves.push_back({ Store, std::move(*Slot) });
  }

  /// \p Difference is what a register had when the function was entered
  /// minus what it has where the function is left. If it is read from the
  /// stack there, where from is kept for \ref visitRestores.
  void visitRestore(llvm::Value *Difference) {
    using namespace llvm;
    using namespace llvm::PatternMatch;

    Value *Initial = nullptr;
    Value *Pointer = nullptr;
    if (not match(Difference,
                  m_Sub(m_Value(Initial), m_Load(m_Value(Pointer)))))
      return;

    std::optional<Sum> Restored = heightOf(Pointer);
    join();
    if (not Restored.has_value())
      return;

    auto *Read = dyn_cast<Instruction>(cast<User>(Difference)->getOperand(1));
    ReadBacks.push_back({ Initial, Read, std::move(*Restored) });
  }

  /// A register the function is left with after reading it from the stack,
  /// and that it saved in one place, is read from that place: no evidence is
  /// stronger, so these equations go first.
  ///
  /// Not if the function can have saved it twice: with code after the place
  /// where it restores its registers (another function it ends with), what
  /// it reads back there can be pushed again, and where the last read comes
  /// from is then one of two places. That second save does not look like
  /// one: as long as the heights are not known, what is pushed is something
  /// read from the stack, not the register.
  void visitRestores(llvm::Function &F, const llvm::DominatorTree &DT) {
    using namespace llvm;

    // What is read from the stack and stored there again. The heights are
    // of anything here, not of the stack pointer: what is met on the way
    // must not give equations, so it is forgotten afterwards.
    std::vector<Resave> Resaves;
    {
      auto OldKnown = Known;
      auto OldOthers = Others;
      auto OldValueOf = ValueOf;
      unsigned First = Callees + Others.size();
      auto IsVague = [First](const Sum &Height) {
        return not Height.IsValid
               or (not Height.Terms.empty()
                   and Height.Terms.rbegin()->first >= First);
      };

      for (Instruction &I : instructions(F)) {
        auto *Store = dyn_cast<StoreInst>(&I);
        if (Store == nullptr)
          continue;
        auto *Value = dyn_cast<LoadInst>(Store->getValueOperand());
        if (Value == nullptr
            or isa<GlobalVariable>(skipCasts(Value->getPointerOperand())))
          continue;

        std::optional<Sum> From = heightOf(Value->getPointerOperand());
        if (not From.has_value())
          continue;
        std::optional<Sum> Slot = heightOf(Store->getPointerOperand());
        if (not Slot.has_value())
          continue;

        bool Vague = IsVague(*From) or IsVague(*Slot);
        Resaves.push_back({ Store, std::move(*Slot), std::move(*From), Vague });
      }

      Known = std::move(OldKnown);
      Others = std::move(OldOthers);
      ValueOf = std::move(OldValueOf);
      Pending.clear();
    }

    for (const ReadBack &Back : ReadBacks) {
      const Save *Only = nullptr;
      bool IsOnly = true;
      for (const Save &Save : Saves) {
        if (cast<StoreInst>(Save.Store)->getValueOperand() != Back.Initial)
          continue;
        IsOnly = IsOnly and (Only == nullptr or Only->Slot == Save.Slot);
        Only = &Save;
      }

      if (Only == nullptr or not IsOnly)
        continue;

      bool SavedAgain = false;
      for (const Resave &Again : Resaves) {
        if (Back.Read == nullptr or not DT.dominates(Again.Store, Back.Read))
          continue;
        if (Again.IsVague) {
          SavedAgain = true;
          continue;
        }
        if (Again.Slot == Only->Slot)
          continue;
        Sum Apart = Again.From;
        Apart.add(Only->Slot, -1);
        SavedAgain = SavedAgain or canBeZero(Apart);
      }

      if (SavedAgain)
        continue;

      Sum Equation = Back.From;
      Equation.add(Only->Slot, -1);
      if (Equation.IsValid and not Equation.Terms.empty())
        Restores.push_back(std::move(Equation));
    }
  }

  /// \return for each callee, the most it can move the stack pointer by: not
  ///         past the return address of the function, nor past what the
  ///         function saved before the call
  std::vector<std::optional<int64_t>> bounds(const llvm::DominatorTree &DT) {
    std::vector<std::optional<int64_t>> Result(Callees);
    auto Lower = [&Result](unsigned Unknown, const Sum &Room) {
      if (not Room.IsValid or not Room.Terms.empty() or Room.Constant <= 0)
        return;
      if (not Result[Unknown].has_value() or Room.Constant < *Result[Unknown])
        Result[Unknown] = Room.Constant;
    };

    for (const Site &Site : Sites) {
      Sum ToReturnAddress;
      ToReturnAddress.add(Site.Height, -1);
      Lower(Site.Unknown, ToReturnAddress);

      for (const Save &Save : Saves) {
        if (not DT.dominates(Save.Store, Site.Hook))
          continue;
        Sum Room = Save.Slot;
        Room.add(Site.Height, -1);
        Lower(Site.Unknown, Room);
      }
    }

    return Result;
  }

  /// Take what \p Height, a height of the stack, says
  void visit(llvm::Value *Height) {
    sumOf(Height);
    join();
  }

private:
  /// Each phi met since the last time is what comes in on each of its paths
  void join() {
    while (not Pending.empty()) {
      llvm::PHINode *Phi = Pending.back();
      Pending.pop_back();
      unsigned Unknown = Others.find(Phi)->second;
      for (llvm::Value *Incoming : Phi->incoming_values()) {
        if (Incoming == Phi)
          continue;
        Sum Equation = sumOf(Incoming);
        Sum Itself;
        Itself.Terms[Unknown] = 1;
        Equation.add(Itself, -1);
        if (Equation.IsValid)
          Equations.push_back(std::move(Equation));
      }
    }
  }

public:
  /// \return for each callee, the amount the equations leave no choice about.
  ///         Where they only say what some callees amount to together,
  ///         \p Likeliest, what each is taken to be otherwise, is made to
  ///         agree with them, and \p IsAmbiguous is set for those callees.
  ///
  /// \p Least is what a callee that removes nothing amounts to: none
  /// amounts to less.
  std::vector<std::optional<int64_t>>
  solve(std::vector<int64_t> &Likeliest,
        std::vector<bool> &IsAmbiguous,
        const std::vector<std::optional<int64_t>> &Most,
        int64_t Least) {
    for (unsigned I = 0; I < Callees; ++I)
      if (Most[I].has_value() and Likeliest[I] > *Most[I])
        Likeliest[I] = *Most[I];

    struct Row {
      Sum Equation;
      unsigned Pivot = 0;
    };
    std::vector<Row> Rows;

    auto Eliminate = [](Sum &From, const Row &With) {
      auto It = From.Terms.find(With.Pivot);
      if (It == From.Terms.end())
        return;
      int64_t Coefficient = It->second;
      From.scale(With.Equation.Terms.find(With.Pivot)->second);
      From.add(With.Equation, -Coefficient);
      From.normalize();
    };

    // An equation that contradicts those before it is dropped: something else
    // is wrong on one of the two paths. An unknown that is not a callee's goes
    // first, so that what is left is about the callees only.
    auto Reduce = [&Eliminate](std::vector<Sum> All) {
      std::vector<Row> Rows;
      for (Sum &Equation : All) {
        for (const Row &Other : Rows)
          Eliminate(Equation, Other);
        if (not Equation.IsValid or Equation.Terms.empty())
          continue;
        Rows.push_back({ Equation, Equation.Terms.rbegin()->first });
      }

      for (size_t I = Rows.size(); I-- > 0;)
        for (size_t J = 0; J < I; ++J)
          Eliminate(Rows[J].Equation, Rows[I]);

      return Rows;
    };

    std::vector<std::optional<int64_t>> Result(Callees);

    // Callees that count the same way where paths join and amount together
    // to the least they can: each of them removes nothing. (An argument
    // pushed before the calls that compute the next ones, for a callee that
    // leaves it to the caller.) From the joins alone: where a register is
    // read back from says less (it can have been saved more than once).
    for (const Row &Row : Reduce(Equations)) {
      const Sum &Equation = Row.Equation;
      if (Row.Pivot >= Callees or not Equation.IsValid
          or Equation.Terms.size() < 2)
        continue;

      bool AllPositive = true;
      bool AllNegative = true;
      int64_t AtLeast = Equation.Constant;
      for (const auto &[Unknown, Other] : Equation.Terms) {
        AllPositive = AllPositive and Other > 0;
        AllNegative = AllNegative and Other < 0;
        AtLeast += Other * Least;
      }
      if ((AllPositive or AllNegative) and AtLeast == 0)
        for (const auto &[Unknown, Other] : Equation.Terms)
          Result[Unknown] = Least;
    }

    for (unsigned I = 0; I < Callees; ++I)
      if (Result[I].has_value())
        Likeliest[I] = *Result[I];

    std::vector<Sum> All = Restores;
    All.insert(All.end(), Equations.begin(), Equations.end());
    Rows = Reduce(std::move(All));

    for (const Row &Row : Rows) {
      const Sum &Equation = Row.Equation;
      if (Row.Pivot >= Callees or not Equation.IsValid)
        continue;

      // (What the joins have settled stays)
      bool IsSettled = true;
      for (const auto &[Unknown, Other] : Equation.Terms)
        IsSettled = IsSettled and Result[Unknown].has_value();
      if (IsSettled)
        continue;

      // The rows of the callees have no other unknowns: those went first
      int64_t Coefficient = Equation.Terms.find(Row.Pivot)->second;
      int64_t Rest = Equation.Constant;
      for (const auto &[Unknown, Other] : Equation.Terms)
        if (Unknown != Row.Pivot)
          Rest += Other * Likeliest[Unknown];
      if (Rest % Coefficient != 0)
        continue;

      int64_t Value = -Rest / Coefficient;
      if (Equation.Terms.size() == 1) {
        Result[Row.Pivot] = Value;
      } else {
        // If that is more than the callee can remove, leave it: the answer
        // is another one
        if (not Most[Row.Pivot].has_value() or Value <= *Most[Row.Pivot])
          Likeliest[Row.Pivot] = Value;
        for (const auto &[Unknown, Other] : Equation.Terms)
          IsAmbiguous[Unknown] = true;
      }
    }

    return Result;
  }

private:
  /// \return whether \p Value, a sum, is zero for some amounts the callees
  ///         can remove: each of them at least what one that removes nothing
  ///         amounts to. What is not a callee's can be anything.
  bool canBeZero(const Sum &Value) const {
    if (not Value.IsValid)
      return true;
    if (Value.Terms.empty())
      return Value.Constant == 0;

    bool AllPositive = true;
    bool AllNegative = true;
    int64_t AtLeast = Value.Constant;
    for (const auto &[Unknown, Coefficient] : Value.Terms) {
      if (Unknown >= Callees)
        return true;
      AllPositive = AllPositive and Coefficient > 0;
      AllNegative = AllNegative and Coefficient < 0;
      AtLeast += Coefficient * Least;
    }

    if (AllPositive)
      return AtLeast <= 0;
    if (AllNegative)
      return AtLeast >= 0;
    return true;
  }

  /// \return the height of the stack at the address \p Pointer, if it is the
  ///         stack pointer the function is entered with plus a sum
  std::optional<Sum> heightOf(llvm::Value *Pointer) {
    using namespace llvm;
    const DataLayout &DL = StackPointer->getParent()->getDataLayout();

    Sum Result;
    while (true) {
      if (auto *GEP = dyn_cast<GEPOperator>(Pointer)) {
        APInt Offset(DL.getIndexTypeSizeInBits(GEP->getType()), 0);
        if (not GEP->accumulateConstantOffset(DL, Offset))
          return std::nullopt;
        Result.Constant += Offset.getSExtValue();
        Pointer = GEP->getPointerOperand();
      } else if (auto *Cast = dyn_cast<BitCastOperator>(Pointer)) {
        Pointer = Cast->getOperand(0);
      } else {
        break;
      }
    }

    // The stack pointer itself, read as a pointer
    if (auto *Load = dyn_cast<LoadInst>(Pointer))
      if (skipCasts(Load->getPointerOperand()) == StackPointer)
        return Result;

    auto *ToPointer = dyn_cast<Operator>(Pointer);
    if (ToPointer == nullptr or ToPointer->getOpcode() != Instruction::IntToPtr)
      return std::nullopt;

    Value *Address = ToPointer->getOperand(0);
    if (auto *Extended = dyn_cast<ZExtInst>(Address))
      Address = Extended->getOperand(0);

    Sum Absolute = sumOf(Address);
    if (not isStackAddress(Absolute, 0))
      return std::nullopt;

    // Where the paths of the function have joined, the stack pointer is one
    // of the unknowns: the height is then that unknown minus the stack
    // pointer the function is entered with, which the equations tell apart
    Sum Origin;
    Origin.Terms[unknownOf(StackPointer)] = 1;
    Result.add(Absolute, 1);
    Result.add(Origin, -1);
    return Result;
  }

  /// \return whether \p Address is the stack pointer the function is entered
  ///         with, plus constants and what callees remove, as such or
  ///         through the phis that join such addresses
  bool isStackAddress(const Sum &Address, unsigned Depth) {
    using namespace llvm;

    // (Not more than a few hundred phis are looked at for one address)
    if (Depth == 0)
      Visits = 0;
    if (not Address.IsValid or Depth > 8 or ++Visits > 400)
      return false;

    unsigned Origin = unknownOf(StackPointer);
    const PHINode *Joined = nullptr;
    bool HasOrigin = false;
    for (const auto &[Unknown, Coefficient] : Address.Terms) {
      if (Unknown < Callees)
        continue;
      if (Coefficient != 1 or HasOrigin or Joined != nullptr)
        return false;
      if (Unknown == Origin) {
        HasOrigin = true;
        continue;
      }
      Joined = dyn_cast_or_null<PHINode>(ValueOf.at(Unknown - Callees));
      if (Joined == nullptr)
        return false;
    }

    if (HasOrigin)
      return true;
    if (Joined == nullptr)
      return false;

    // A phi that only goes around a loop says nothing
    bool HasOther = false;
    for (Value *Incoming : Joined->incoming_values()) {
      if (Incoming == Joined)
        continue;
      Sum Other = sumOf(Incoming);
      auto It = Other.Terms.find(Others.find(Joined)->second);
      if (It != Other.Terms.end() and It->second == 1
          and Other.Terms.size() == 1 and Other.Constant == 0)
        continue;
      if (not isStackAddress(Other, Depth + 1))
        return false;
      HasOther = true;
    }

    return HasOther;
  }

  unsigned unknownOf(llvm::Value *V) {
    auto [It, IsNew] = Others.try_emplace(V, Callees + Others.size());
    if (IsNew) {
      ValueOf.push_back(V);
      if (auto *Phi = llvm::dyn_cast<llvm::PHINode>(V))
        Pending.push_back(Phi);
    }
    return It->second;
  }

  Sum sumOf(llvm::Value *V) {
    using namespace llvm;

    Sum Result;
    if (auto *Constant = dyn_cast<ConstantInt>(V)) {
      Result.Constant = Constant->getSExtValue();
      return Result;
    }

    auto It = Known.find(V);
    if (It != Known.end())
      return It->second;

    auto ConstantOf = [](Value *V) -> std::optional<int64_t> {
      if (auto *Constant = dyn_cast<ConstantInt>(V))
        return Constant->getSExtValue();
      return std::nullopt;
    };

    // The stack pointer is a pointer, and the code computes with numbers of
    // its own size: what is looked for here goes through that unchanged
    if (isa<TruncInst>(V) or isa<ZExtInst>(V) or isa<PtrToIntInst>(V)
        or isa<IntToPtrInst>(V) or isa<BitCastInst>(V)) {
      Result = sumOf(cast<Instruction>(V)->getOperand(0));
      Known[V] = Result;
      return Result;
    }

    bool IsSum = false;
    if (auto *Call = dyn_cast<CallBase>(V);
        Call != nullptr and Call->getCalledFunction() == StackEffect) {
      Result.Terms[getLimitedValue(Call->getArgOperand(0))] = 1;
      IsSum = true;
    } else if (auto *Load = dyn_cast<LoadInst>(V);
               Load != nullptr
               and skipCasts(Load->getPointerOperand()) == StackPointer) {
      // The stack pointer the function is entered with: one unknown,
      // however many times it is read
      Result.Terms[unknownOf(StackPointer)] = 1;
      IsSum = true;
    } else if (auto *Operator = dyn_cast<BinaryOperator>(V)) {
      Value *Left = Operator->getOperand(0);
      Value *Right = Operator->getOperand(1);
      switch (Operator->getOpcode()) {
      case Instruction::Add:
      case Instruction::Sub: {
        bool IsAdd = Operator->getOpcode() == Instruction::Add;
        Result = sumOf(Left);
        Result.add(sumOf(Right), IsAdd ? 1 : -1);
        IsSum = true;
      } break;

      case Instruction::Mul:
        if (std::optional<int64_t> Factor = ConstantOf(Right)) {
          Result = sumOf(Left);
          Result.scale(*Factor);
          IsSum = true;
        }
        break;

      case Instruction::Shl:
        if (std::optional<int64_t> Amount = ConstantOf(Right);
            Amount.has_value() and *Amount >= 0 and *Amount < 31) {
          Result = sumOf(Left);
          Result.scale(int64_t(1) << *Amount);
          IsSum = true;
        }
        break;

      default:
        break;
      }
    }

    if (not IsSum)
      Result.Terms[unknownOf(V)] = 1;

    Known[V] = Result;
    return Result;
  }
};

} // namespace

std::vector<std::optional<int64_t>>
CFGAnalyzer::solveCallEffects(const MetaAddress &Entry,
                              std::vector<int64_t> &Likeliest,
                              std::vector<bool> &IsAmbiguous,
                              std::vector<std::optional<int64_t>> &Most) {
  using namespace llvm;

  auto &&[OutlinedFunction, CFG] = optimize(Entry);
  Function *F = OutlinedFunction.Function.get();

  // The heights the optimizations were left with: those at the calls and at
  // the starts of the blocks, and those the function is left with
  int64_t Least = Oracle.getDefault().ElectedFSO.value_or(0);
  StackEquations Equations(Likeliest.size(),
                           StackEffect.get(),
                           Globals.spReg(),
                           &CalleeSaved,
                           Least);
  for (CallBase *Call : callers(StackHeightHook.get())) {
    if (Call->getFunction() != F)
      continue;
    Equations.visit(Call->getArgOperand(1));

    if (getLimitedValue(Call->getArgOperand(2)) != StackHeightAtCall)
      continue;
    BasicBlockID Block = BasicBlockID::fromValue(Call->getArgOperand(0));
    auto It = SymbolicSites.find(Block.notInlinedAddress());
    if (not Block.isInlined() and It != SymbolicSites.end())
      Equations.visitSite(It->second, Call);
  }

  for (Instruction &I : instructions(F))
    if (auto *Store = dyn_cast<StoreInst>(&I))
      Equations.visitStore(Store);

  Function *Marker = OutlinedFunction.IndirectBranchInfoMarker.get();
  for (CallBase *Call : callers(Marker)) {
    if (Call->getFunction() != F)
      continue;
    Equations.visit(Call->getArgOperand(StackPointerOffsetIndex));
    for (unsigned I = FirstPreservedRegisterIndex; I < Call->arg_size(); ++I)
      Equations.visitRestore(Call->getArgOperand(I));
  }

  DominatorTree DT(*F);
  Equations.visitRestores(*F, DT);
  Most = Equations.bounds(DT);
  return Equations.solve(Likeliest, IsAmbiguous, Most, Least);
}

FunctionSummary CFGAnalyzer::analyze(const MetaAddress &Entry) {
  return analyzeCallees(Entry);
}

std::optional<FunctionSummary>
CFGAnalyzer::analyzeElidedCleanups(const MetaAddress &Entry,
                                   std::map<MetaAddress, uint64_t> &Found) {
  // A caller that pushes the arguments of a call has to remove them, unless
  // the callee does. The code that removes them can be missing: after a call
  // that does not return, and where the function sets its stack pointer again
  // from its frame pointer before the difference matters. The stack is then
  // one height on that path and another on those it joins, or a register the
  // function saved comes back from the wrong slot, or the function returns
  // to something that is not its return address. None of that says which of
  // the two it is, and one does not have to: taking the arguments as removed
  // is right if the call returns and harmless if it does not. It is recorded
  // at the call site, see ElidedStackCleanup in the model.
  //
  // This is for when all the callees are known: before that, a callee that
  // has not been analyzed yet looks like one that removes nothing.
  const model::Function *Function = Binary->Functions().tryGet(Entry);
  std::optional<int64_t> Default = Oracle.getDefault().ElectedFSO;
  if (Function == nullptr or not Default.has_value())
    return std::nullopt;

  const CSVSet &ByDefault = Oracle.getDefault().ClobberedRegisters;
  auto RankOf = [this, &ByDefault](const FunctionSummary &Summary) {
    int64_t Exits = 0;
    int64_t LongJmps = 0;
    for (const efa::BasicBlock &Block : Summary.CFG) {
      for (const auto &Edge : Block.Successors()) {
        const auto *Call = llvm::dyn_cast<efa::CallEdge>(Edge.get());
        if (Edge->Type() == efa::FunctionEdgeType::Return
            or (Call != nullptr and Call->IsTailCall()))
          ++Exits;
        else if (Edge->Type() == efa::FunctionEdgeType::LongJmp)
          ++LongJmps;
      }
    }

    int64_t Clobbered = 0;
    for (llvm::GlobalVariable *CSV : Summary.ClobberedRegisters)
      if (not ByDefault.contains(CSV) and not CalleeClobbered.contains(CSV))
        ++Clobbered;

    return std::tuple(Exits, -LongJmps, -Clobbered, -int64_t(UnknownHeights));
  };

  ElidedCleanups.clear();
  std::optional<FunctionSummary> Best;
  FunctionSummary Base = analyzeOnce(Entry);
  auto BestRank = RankOf(Base);
  if (std::get<1>(BestRank) == 0 and std::get<2>(BestRank) == 0
      and std::get<3>(BestRank) == 0)
    return std::nullopt;

  // What the block of the call pushed, a register it saves first aside
  llvm::SmallVector<std::pair<MetaAddress, uint64_t>, 4> Candidates;
  for (const auto &[Block, Known] : KnownCalls) {
    uint64_t Pushed = Block == Entry ? Known.PushedAfterSaves : Known.Pushed;
    if (Pushed != 0 and Known.Removed == 0
        and Known.FinalStackOffset == *Default
        and Base.CFG.contains(BasicBlockID(Block))
        and not Function->CallSitePrototypes().contains(Block))
      Candidates.emplace_back(Block, Pushed);
  }

  // A dynamic function with a prototype is known too
  for (const UnknownCall &Call : UnknownCalls) {
    if (Call.Symbol.empty() or Call.FinalStackOffset != Default)
      continue;
    const auto &Imported = Binary->ImportedDynamicFunctions();
    auto It = Imported.find(Call.Symbol);
    uint64_t Pushed = Call.Block == Entry ? Call.PushedAfterSaves : Call.Pushed;
    if (It != Imported.end() and not It->Prototype().isEmpty() and Pushed != 0
        and Base.CFG.contains(BasicBlockID(Call.Block))
        and not Function->CallSitePrototypes().contains(Call.Block))
      Candidates.emplace_back(Call.Block, Pushed);
  }

  for (const auto &[Block, Pushed] : Candidates) {
    ElidedCleanups[Block] = Pushed;
    FunctionSummary Attempt = analyzeOnce(Entry);
    auto Rank = RankOf(Attempt);
    if (Rank > BestRank) {
      BestRank = Rank;
      Best = std::move(Attempt);
    } else {
      ElidedCleanups.erase(Block);
    }
  }

  Found = std::move(ElidedCleanups);
  ElidedCleanups.clear();
  return Best;
}

FunctionSummary CFGAnalyzer::analyzeCallees(const MetaAddress &Entry) {
  FunctionSummary Result = analyzeOnce(Entry);

  // The callee of an indirect call, and a dynamic function without a
  // prototype, are assumed to leave the stack as the default prototype says.
  // Where they remove their arguments, as the member functions and the system
  // functions of 32-bit Windows code do, the stack pointer is off from there
  // on: the function does not find its return address where it looks for it,
  // or it does, through a frame pointer, but restores the registers it saved
  // from the wrong place. What the callees remove can be found by trying: only
  // with the right amounts are the return address and the saved registers
  // where the function left them, and do the paths that reach a later call
  // agree on the height of the stack. This also holds where the stack pointer
  // is unknown at a return because the paths that reach it disagree on it.
  //
  // A function that calls itself is such a callee too, the first time it is
  // analyzed: nothing is known about it yet. What it is assumed to remove
  // there has to be what it is then found to remove.
  //
  // This is for the functions detect-abi describes the call sites of: what
  // is found goes in their prototypes.
  const model::Function *Function = Binary->Functions().tryGet(Entry);
  std::optional<int64_t> Default = Oracle.getDefault().ElectedFSO;
  if (Binary->Architecture() != model::Architecture::x86 or Function == nullptr
      or not Function->Prototype().isEmpty() or not Default.has_value())
    return Result;

  const CSVSet &ByDefault = Oracle.getDefault().ClobberedRegisters;
  auto ScoreOf = [this, &ByDefault](const FunctionSummary &Summary) {
    AnalysisScore Score;
    for (const efa::BasicBlock &Block : Summary.CFG) {
      for (const auto &Edge : Block.Successors()) {
        const auto *Call = llvm::dyn_cast<efa::CallEdge>(Edge.get());
        if (Edge->Type() == efa::FunctionEdgeType::Return
            or (Call != nullptr and Call->IsTailCall()))
          ++Score.Returns;
        else if (Edge->Type() == efa::FunctionEdgeType::LongJmp)
          ++Score.LongJmps;
      }
    }

    for (llvm::GlobalVariable *CSV : Summary.ClobberedRegisters)
      if (not ByDefault.contains(CSV) and not CalleeClobbered.contains(CSV))
        ++Score.Clobbered;

    Score.UnknownHeights = UnknownHeights;

    return Score;
  };

  // A callee removes at most what the block of its call pushed, and most
  // often all of it: the rest is a register the block saves first. The
  // arguments of the only such call of a function can also have been pushed
  // in another block, without making what the others remove uncertain.
  constexpr uint64_t MaxWords = 8;
  uint64_t Word = model::Architecture::getPointerSize(Binary->Architecture());
  llvm::SmallVector<UnknownCallSite, 4> Sites;
  bool HasAssumptions = false;

  // What a block pushes before a call is not for that call if a later one,
  // to something known, removes more than its own block pushes: an argument
  // of a call is pushed before the calls that compute the arguments after it.
  // Going back from each such call, through the blocks that end with a call
  // to something unknown, what is missing is theirs to leave.
  std::map<MetaAddress, uint64_t> Reserved;
  {
    std::map<MetaAddress, const UnknownCall *> ByEnd;
    for (const UnknownCall &Call : UnknownCalls) {
      auto It = Result.CFG.find(BasicBlockID(Call.Block));
      if (It != Result.CFG.end())
        ByEnd[It->End()] = &Call;
    }

    for (const auto &[Block, Known] : KnownCalls) {
      int64_t Missing = Known.FinalStackOffset - *Default
                        - int64_t(Known.Pushed);
      MetaAddress At = Block;
      while (Missing > 0) {
        auto It = ByEnd.find(At);
        if (It == ByEnd.end())
          break;
        const UnknownCall &Before = *It->second;
        uint64_t Left = std::min<uint64_t>(Before.Pushed, Missing);
        Reserved[Before.Block] = std::max(Reserved[Before.Block], Left);
        Missing -= Left;
        At = Before.Block;
        if (Before.Pushed == 0 and Missing > 0 and At == Block)
          break;
      }
    }
  }

  // What a block pushes before a call to something known that leaves it on
  // the stack, and that the code after the call does not remove, is still
  // there when the next call is made: it can be for that one. The arguments
  // of a call through a pointer are pushed before the call that checks the
  // pointer, which comes first and takes none of them.
  std::map<MetaAddress, uint64_t> Carried;
  for (const auto &[Block, Known] : KnownCalls) {
    auto It = Result.CFG.find(BasicBlockID(Block));
    if (It == Result.CFG.end())
      continue;

    // (The blocks are in the order of their addresses: what reaches this
    // one from the block before it is known by now)
    int64_t Left = Block == Entry ? Known.PushedAfterSaves : Known.Pushed;
    if (auto Before = Carried.find(Block); Before != Carried.end())
      Left += Before->second;
    Left -= Known.FinalStackOffset - *Default + int64_t(Known.Removed);
    if (Left > 0)
      Carried[It->End()] = Left;
  }

  for (const UnknownCall &Call : UnknownCalls) {
    // The call sites of the functions inlined in this one are theirs
    if (not Result.CFG.contains(BasicBlockID(Call.Block))
        or Function->CallSitePrototypes().contains(Call.Block))
      continue;

    if (not Call.Symbol.empty()) {
      const auto &Imported = Binary->ImportedDynamicFunctions();
      auto It = Imported.find(Call.Symbol);
      if (It == Imported.end() or not It->Prototype().isEmpty())
        continue;
    }

    auto IsSame = [&Call](const UnknownCallSite &Other) {
      return llvm::is_contained(Other.Blocks, Call.Block);
    };
    if (llvm::any_of(Sites, IsSame))
      continue;

    HasAssumptions = HasAssumptions or CallSiteFSO.contains(Call.Block);
    bool IsFirst = Call.Block == Entry;
    uint64_t Pushed = IsFirst ? Call.PushedAfterSaves : Call.Pushed;

    // What a known callee left there just before, unless the code after
    // this call removes it
    if (auto It = Carried.find(Call.Block); It != Carried.end()) {
      if (Call.Removed == 0)
        Pushed += It->second;
      else if (Call.Removed <= Pushed + It->second)
        Pushed = 0;
    }

    // What is left on the stack for a later call is not for this one
    if (auto It = Reserved.find(Call.Block); It != Reserved.end())
      Pushed -= std::min<uint64_t>(Pushed, It->second);

    uint64_t Likeliest = std::min(MaxWords, Pushed / Word);

    // The function removes the same wherever it calls itself, and its
    // arguments can have been pushed in another block
    auto IsItself = [](const UnknownCallSite &Other) {
      return Other.IsRecursive;
    };
    auto IsSameSymbol = [&Call](const UnknownCallSite &Other) {
      return not Call.Symbol.empty() and Other.Symbol == Call.Symbol;
    };
    auto It = llvm::find_if(Sites, IsItself);
    if (not Call.IsRecursive)
      It = llvm::find_if(Sites, IsSameSymbol);
    if (It != Sites.end()) {
      It->Blocks.push_back(Call.Block);
      It->Instructions.push_back(Call.Instruction);
      It->Likeliest = std::max(It->Likeliest, Likeliest);
      It->Most = MaxWords;
      continue;
    }

    UnknownCallSite &New = Sites.emplace_back();
    New.Blocks.push_back(Call.Block);
    New.Instructions.push_back(Call.Instruction);
    New.IsFirst = IsFirst;
    New.IsRecursive = Call.IsRecursive;
    New.Symbol = Call.Symbol;
    New.Likeliest = Likeliest;
    New.Most = Call.IsRecursive ? MaxWords : Likeliest;
  }

  AnalysisScore Base = ScoreOf(Result);
  auto Leave = [this](const AnalysisScore &Score) {
    Leftover = { Score.Returns,
                 Score.LongJmps,
                 Score.Clobbered,
                 Score.UnknownHeights };
    HasLeftover = Score.leavesSomethingToExplain();
  };
  Leave(Base);
  if (Sites.empty() or not Base.leavesSomethingToExplain())
    return Result;

  // What was assumed when the callees were known less well does not hold
  // anymore: start again
  auto Forget = [this, &Sites]() {
    for (const UnknownCallSite &Site : Sites) {
      for (const MetaAddress &Block : Site.Blocks) {
        CallSiteFSO.erase(Block);
        AmbiguousSites.erase(Block);
      }
    }
  };
  if (HasAssumptions) {
    Forget();
    Result = analyzeOnce(Entry);
    Base = ScoreOf(Result);
    Leave(Base);
    if (not Base.leavesSomethingToExplain())
      return Result;
  }

  // Before trying: with an unknown for what each callee removes, the paths
  // of the function that join give equations. What they leave no choice
  // about is not to be tried, and can be more than the block of the call
  // pushes: arguments are also pushed before a call that computes another.
  {
    constexpr int64_t MostRequiredWords = 64;
    auto WordsOf = [&](int64_t Effect) -> std::optional<uint64_t> {
      int64_t Extra = Effect - *Default;
      if (Extra < 0 or Extra % int64_t(Word) != 0
          or Extra / int64_t(Word) > MostRequiredWords)
        return std::nullopt;
      return Extra / Word;
    };

    std::vector<int64_t> Effects;
    for (size_t I = 0; I < Sites.size(); ++I) {
      Effects.push_back(*Default + Sites[I].Likeliest * Word);
      for (const MetaAddress &Block : Sites[I].Blocks) {
        SymbolicSites[Block] = I;
        AmbiguousSites.erase(Block);
      }
    }
    std::vector<bool> IsAmbiguous(Sites.size(), false);
    std::vector<std::optional<int64_t>> MostEffects;
    auto Required = solveCallEffects(Entry, Effects, IsAmbiguous, MostEffects);
    SymbolicSites.clear();

    for (size_t I = 0; I < Sites.size(); ++I) {
      // What the callee cannot remove is not to be tried either. The only
      // such call of a function can also have its arguments pushed in another
      // block.
      if (Sites.size() == 1)
        Sites[I].Most = MaxWords;
      if (MostEffects[I].has_value())
        Sites[I].Bound = WordsOf(*MostEffects[I]);
      if (Sites[I].Bound.has_value()) {
        Sites[I].Most = std::min(Sites[I].Most, *Sites[I].Bound);
        Sites[I].Likeliest = std::min(Sites[I].Likeliest, *Sites[I].Bound);
      }

      std::optional<uint64_t> Words;
      if (Required[I].has_value())
        Words = WordsOf(*Required[I]);
      else if (IsAmbiguous[I])
        Words = WordsOf(Effects[I]);
      if (not Words.has_value())
        continue;

      Sites[I].Likeliest = *Words;
      Sites[I].IsAmbiguous = IsAmbiguous[I];
      if (not Sites[I].Bound.has_value())
        Sites[I].Most = std::max(Sites[I].Most, *Words);
      Sites[I].IsRequired = Required[I].has_value();
      if (IsAmbiguous[I])
        for (size_t J = 0; J < Sites[I].Blocks.size(); ++J)
          AmbiguousSites[Sites[I].Blocks[J]] = Sites[I].Instructions[J];
    }
  }

  // An assumption: the words each callee removes
  using Assumption = llvm::SmallVector<uint64_t, 4>;
  auto Assume = [this, &Sites, &Default, Word](const Assumption &Words) {
    for (size_t I = 0; I < Sites.size(); ++I)
      for (const MetaAddress &Block : Sites[I].Blocks)
        CallSiteFSO[Block] = *Default + Words[I] * Word;
  };

  // What a function is assumed to remove where it calls itself has to be what
  // it is found to remove
  auto IsConsistent = [&Sites, &Default, Word](const FunctionSummary &Summary,
                                               const Assumption &Words) {
    for (size_t I = 0; I < Sites.size(); ++I) {
      std::optional<int64_t> Assumed = *Default + Words[I] * Word;
      if (Sites[I].IsRecursive and Summary.ElectedFSO != Assumed)
        return false;
    }
    return true;
  };

  // Start from the likeliest assumption and, as long as something is left to
  // explain, move to the best of those that differ from it at one call site.
  // If the likeliest assumption changes nothing, the callees are not what is
  // wrong, unless it is about the call the function starts with, whose block
  // also pushes what the function saves, about its only call, or about the
  // calls to itself.
  Assumption Current;
  for (const UnknownCallSite &Site : Sites)
    Current.push_back(Site.Likeliest);

  Assume(Current);
  FunctionSummary Best = analyzeOnce(Entry);
  AnalysisScore BestScore = ScoreOf(Best);
  if (not IsConsistent(Best, Current)) {
    Best = Result.clone();
    BestScore = Base;
  }
  bool IsOver = BestScore.leavesNothingToExplain() or Unsolved.contains(Entry);
  for (size_t Round = 0; not IsOver and Round < Sites.size(); ++Round) {
    bool OnlyFirst = Round == 0 and BestScore.rank() == Base.rank()
                     and Sites.size() != 1;
    Assumption Next;
    for (size_t I = 0; not IsOver and I < Sites.size(); ++I) {
      if (Sites[I].IsRequired
          or (OnlyFirst and not Sites[I].IsFirst and not Sites[I].IsRecursive))
        continue;

      for (uint64_t Words = Sites[I].Most + 1; not IsOver and Words > 0;) {
        --Words;
        if (Words == Current[I])
          continue;

        Assumption Other = Current;
        Other[I] = Words;
        Assume(Other);
        FunctionSummary Attempt = analyzeOnce(Entry);
        AnalysisScore AttemptScore = ScoreOf(Attempt);
        if (not IsConsistent(Attempt, Other)
            or AttemptScore.rank() <= BestScore.rank())
          continue;

        Best = std::move(Attempt);
        BestScore = AttemptScore;
        Next = Other;
        IsOver = BestScore.leavesNothingToExplain();
      }
    }

    if (Next.empty())
      break;
    Current = Next;
  }

  // Where the function only says what some callees remove together, one can
  // have been given what is another's: the heights of the stack are then all
  // known, and a register the function saved comes back from the wrong place.
  // Move a word from one to another, as long as that helps.
  constexpr unsigned MostMoves = 48;
  unsigned Moves = 0;
  auto Move = [&]() {
    for (size_t From = 0; From < Sites.size(); ++From) {
      if (not Sites[From].IsAmbiguous or Current[From] == 0)
        continue;

      for (size_t To = 0; To < Sites.size(); ++To) {
        if (To == From or not Sites[To].IsAmbiguous
            or (Sites[To].Bound.has_value()
                and Current[To] + 1 > *Sites[To].Bound))
          continue;

        if (Moves >= MostMoves)
          return false;
        ++Moves;

        Assumption Other = Current;
        --Other[From];
        ++Other[To];
        Assume(Other);
        FunctionSummary Attempt = analyzeOnce(Entry);
        AnalysisScore AttemptScore = ScoreOf(Attempt);
        if (not IsConsistent(Attempt, Other)
            or AttemptScore.rank() <= BestScore.rank())
          continue;

        Best = std::move(Attempt);
        BestScore = AttemptScore;
        Current = Other;
        return true;
      }
    }

    return false;
  };

  if (not Unsolved.contains(Entry))
    while (not BestScore.leavesNothingToExplain() and Move())
      ;

  if (not BestScore.isBetterThan(Base)) {
    Unsolved.insert(Entry);
    Forget();
    return Result;
  }

  Leave(BestScore);
  Assume(Current);
  return Best;
}

std::pair<OutlinedFunction, SortedVector<efa::BasicBlock>>
CFGAnalyzer::optimize(const MetaAddress &Entry) {
  using namespace llvm;
  using llvm::BasicBlock;

  revng::IRBuilder Builder(M.getContext());

  // Detect function boundaries
  OutlinedFunction OutlinedFunction = outline(Entry);

  // Recover the control-flow graph of the function
  SortedVector<efa::BasicBlock> CFG = collectDirectCFG(&OutlinedFunction);

  BasicBlock &FirstBlock = OutlinedFunction.Function->getEntryBlock();
  if (CFG.size() == 0) {
    // Handle the situation in which the entry block is not in root.
    // This can happen if a model::Function is a non-executable area.

    // Ensure there are not calls to newpc. If there were, it'd be a bug not to
    // have a CFG
    auto IsCallToNewPC = [](Instruction &I) -> bool {
      return NewPCHelper.getCall(&I).has_value();
    };
    revng_assert(none_of(instructions(OutlinedFunction.Function.get()),
                         IsCallToNewPC));
  } else {
    revng_assert(CFG.size() > 0);
  }

  // The analysis aims at identifying the callee-saved registers of a
  // function and establishing if a function returns properly, i.e., it
  // jumps to the return address (regular function). In order to achieve
  // this, the IR is crafted by loading the program counter, the stack
  // pointer, as well as the ABI registers CSVs respectively at function
  // prologue / epilogue. When the subtraction between their entry and end
  // values is found to be zero (after running an LLVM optimization
  // pipeline), we may infer if the function returns correctly, the stack
  // is left unaltered, etc. Hence, upon every original indirect jump
  // (candidate exit point), a marker of this kind is installed:
  //
  //                           jumps to RA,    SP,    rax,   rbx,   rbp
  // call i64 @indirect_branch_info(i128 0, i64 8, i64 %8, i64 0, i64 0)
  //
  // Here, subsequently the opt pipeline computation, we may tell that the
  // function jumps to its return address (thus, it is not a longjmp /
  // tail call), `rax` register has been clobbered by the callee, whereas
  // `rbx` and `rbp` are callee-saved registers.
  PopsItsTarget.clear();
  createIBIMarker(&OutlinedFunction);

  Function *F = OutlinedFunction.Function.get();

  // Prevent DCE by making branch conditions opaque
  opaqueBranchConditions(F, Builder);

  // Store the values that build up the program counter in order to have them
  // constant-folded away by the optimization pipeline.
  materializePCValues(F, Builder);

  // Execute the optimization pipeline over the outlined function
  runOptimizationPipeline(F);

  // The heights of the stack the paths of the function agree on, where LLVM
  // does not see that they do
  SameOnAllPaths Same;
  for (CallBase *Call : callers(StackHeightHook.get()))
    if (Call->getFunction() == F)
      Same.fold(Call, 1);
  for (CallBase *Call : callers(OutlinedFunction.IndirectBranchInfoMarker.get()))
    if (Call->getFunction() == F)
      Same.fold(Call, StackPointerOffsetIndex);

  return { std::move(OutlinedFunction), std::move(CFG) };
}

FunctionSummary CFGAnalyzer::analyzeOnce(const MetaAddress &Entry) {
  using namespace llvm;

  auto &&[OutlinedFunction, CFG] = optimize(Entry);
  Function *F = OutlinedFunction.Function.get();

  // The paths that reach a call agree on the height of the stack there, unless
  // something is wrong about a call before it. Where they agree at the start
  // of a block, record it: the steps that come after isolation need not find
  // it out again, with less to go by.
  UnknownHeights = 0;
  std::map<BasicBlockID, std::optional<int64_t>> Heights;
  for (CallBase *Call : callers(StackHeightHook.get())) {
    if (Call->getFunction() != F)
      continue;

    auto *Height = dyn_cast<ConstantInt>(Call->getArgOperand(1));
    if (getLimitedValue(Call->getArgOperand(2)) == StackHeightAtCall) {
      if (Height == nullptr)
        ++UnknownHeights;
      continue;
    }

    // A block the optimizations have duplicated has to have one height
    BasicBlockID ID = BasicBlockID::fromValue(Call->getArgOperand(0));
    std::optional<int64_t> New;
    if (Height != nullptr)
      New = Height->getSExtValue();
    auto [It, IsNew] = Heights.emplace(ID, New);
    if (not IsNew and It->second != New)
      It->second = std::nullopt;
  }

  for (efa::BasicBlock &Block : CFG) {
    auto It = Heights.find(Block.ID());
    if (It == Heights.end() or not It->second.has_value())
      continue;
    Block.HasStackDepth() = true;
    Block.StackDepth() = -*It->second;
  }

  // Squeeze out the results obtained from the optimization passes
  auto FunctionInfo = milkInfo(&OutlinedFunction, std::move(CFG));

  return FunctionInfo;
}

CallSummarizer::CallSummarizer(llvm::Module *M,
                               Function *PreCallHook,
                               Function *PostCallHook,
                               llvm::Function *RetHook,
                               llvm::Function *StackHeightHook,
                               llvm::Function *StackEffect,
                               GlobalVariable *SPCSV,
                               llvm::SmallSet<MetaAddress, 4> *ReturnBlocks,
                               const std::map<MetaAddress, int64_t>
                                 *CallSiteFSO,
                               const std::map<MetaAddress, unsigned>
                                 *SymbolicSites,
                               llvm::SmallVectorImpl<UnknownCall> *UnknownCalls,
                               std::map<MetaAddress, KnownCall> *KnownCalls,
                               const std::map<MetaAddress, uint64_t>
                                 *ElidedCleanups,
                               CSVSet *CalleeClobbered,
                               const CSVSet *CalleeSaved,
                               MetaAddress Caller) :
  M(M),
  PreCallHook(PreCallHook),
  PostCallHook(PostCallHook),
  RetHook(RetHook),
  StackHeightHook(StackHeightHook),
  StackEffect(StackEffect),
  SPCSV(SPCSV),
  ReturnBlocks(ReturnBlocks),
  Clobberer(M),
  CallSiteFSO(CallSiteFSO),
  SymbolicSites(SymbolicSites),
  UnknownCalls(UnknownCalls),
  KnownCalls(KnownCalls),
  ElidedCleanups(ElidedCleanups),
  CalleeClobbered(CalleeClobbered),
  CalleeSaved(CalleeSaved),
  Caller(Caller) {
}

/// \return the bytes the instructions of \p BB before \p End push and do not
///         pop, the return address of the call they end with aside. If
///         \p Saved is not null, what precedes the push of one of those
///         registers that still has the value it had at the start of \p BB
///         does not count: the block is the first of a function, saving it.
static uint64_t pushedBeforeCall(llvm::BasicBlock &BB,
                                 const llvm::Instruction *End,
                                 const llvm::GlobalVariable *SPCSV,
                                 const CSVSet *Saved) {
  using namespace llvm;
  int64_t Word = SPCSV->getValueType()->getIntegerBitWidth() / 8;

  // The values the stack pointer is known to have at this point
  SmallPtrSet<const Value *, 4> Current;

  // The registers written so far, and the values that are what a register
  // to save had at the start of the block
  SmallPtrSet<const Value *, 4> Written;
  SmallPtrSet<const Value *, 4> ToSave;

  int64_t Pushed = 0;
  for (Instruction &I : BB) {
    if (&I == End)
      break;

    if (auto *Load = dyn_cast<LoadInst>(&I)) {
      auto *CSV = dyn_cast<GlobalVariable>(Load->getPointerOperand());
      if (CSV == SPCSV)
        Current.insert(Load);
      else if (Saved != nullptr and CSV != nullptr and Saved->contains(CSV)
               and not Written.contains(CSV))
        ToSave.insert(Load);
      continue;
    }

    auto *Store = dyn_cast<StoreInst>(&I);
    if (Store == nullptr)
      continue;

    if (Store->getPointerOperand() != SPCSV) {
      // The value of a register to save going to memory is its being pushed
      if (isa<GlobalVariable>(Store->getPointerOperand()))
        Written.insert(Store->getPointerOperand());
      else if (ToSave.contains(Store->getValueOperand()))
        Pushed = -Word;
      continue;
    }

    // How far does this move the stack pointer?
    const Value *New = Store->getValueOperand();
    std::optional<int64_t> Delta;
    if (Current.contains(New)) {
      Delta = 0;
    } else if (auto *Operator = dyn_cast<BinaryOperator>(New)) {
      auto *Amount = dyn_cast<ConstantInt>(Operator->getOperand(1));
      bool IsAdd = Operator->getOpcode() == Instruction::Add;
      bool IsSub = Operator->getOpcode() == Instruction::Sub;
      if (Amount != nullptr and (IsAdd or IsSub)
          and Current.contains(Operator->getOperand(0)))
        Delta = IsAdd ? Amount->getSExtValue() : -Amount->getSExtValue();
    }

    if (Delta == -Word)
      Pushed += Word;
    else if (Delta.has_value() and *Delta >= 0)
      Pushed = std::max<int64_t>(0, Pushed - *Delta);
    else
      Pushed = 0;

    Current.clear();
    Current.insert(New);
  }

  return std::max<int64_t>(0, Pushed - Word);
}

/// \return the bytes the code after the call \p BB ends with removes from the
///         stack before doing anything else with it, if all it does is add to
///         the stack pointer: the caller removing the arguments of the call
static uint64_t removedAfterCall(llvm::BasicBlock &BB,
                                 const llvm::GlobalVariable *SPCSV) {
  using namespace llvm;

  // The block of a call that returns ends with a branch to where it returns
  auto *Branch = dyn_cast<BranchInst>(BB.getTerminator());
  if (Branch == nullptr or Branch->isConditional())
    return 0;
  llvm::BasicBlock *Next = Branch->getSuccessor(0);

  // The first instruction of the program that changes the stack pointer, and
  // whether it reads memory, as a `pop` does
  bool ReadsMemory = false;
  for (Instruction &I : *Next) {
    if (NewPCHelper.getCall(&I).has_value()) {
      ReadsMemory = false;
    } else if (auto *Load = dyn_cast<LoadInst>(&I)) {
      if (not isa<GlobalVariable>(Load->getPointerOperand()))
        ReadsMemory = true;
    } else if (auto *Store = dyn_cast<StoreInst>(&I);
               Store != nullptr and Store->getPointerOperand() == SPCSV) {
      auto *Add = dyn_cast<BinaryOperator>(Store->getValueOperand());
      if (ReadsMemory or Add == nullptr or Add->getOpcode() != Instruction::Add)
        return 0;

      auto *Base = dyn_cast<LoadInst>(Add->getOperand(0));
      auto *Amount = dyn_cast<ConstantInt>(Add->getOperand(1));
      if (Base == nullptr or Base->getPointerOperand() != SPCSV
          or Amount == nullptr or Amount->getSExtValue() <= 0)
        return 0;

      return Amount->getSExtValue();
    }
  }

  return 0;
}

void CallSummarizer::handleCall(MetaAddress CallerBlock,
                                revng::IRBuilder &Builder,
                                MetaAddress Callee,
                                const CSVSet &ClobberedRegisters,
                                const std::optional<int64_t> &MaybeFSO,
                                bool IsNoReturn,
                                bool IsTailCall,
                                llvm::Value *SymbolNamePointer) {
  using namespace llvm;
  LLVMContext &Context = getContext(M);

  // Mark end of basic block with a pre-hook call
  Value *IsTailCallValue = ConstantInt::getBool(Context, IsTailCall);

  SmallVector<Value *, 4> Args = { CallerBlock.toValue(M),
                                   Callee.toValue(M),
                                   SymbolNamePointer,
                                   IsTailCallValue };

  Module *M = Builder.GetInsertBlock()->getParent()->getParent();

  // Leave a trace of where the stack pointer is when the call is made
  if (not IsTailCall) {
    Builder.CreateCall(StackHeightHook,
                       { BasicBlockID(CallerBlock).toValue(M),
                         Builder.createLoad(SPCSV),
                         Builder.getInt32(StackHeightAtCall) });
  }

  Builder.CreateCall(PreCallHook, Args);

  // Emulate registers being clobbered, in the order of the names: the set is
  // ordered by the addresses of the CSVs
  for (GlobalVariable *Register : toSortedByName(ClobberedRegisters))
    Clobberer.clobber(Builder, Register);
  CalleeClobbered->insert(ClobberedRegisters.begin(), ClobberedRegisters.end());

  // For a call to something else than a function of the binary, and for one
  // of the function to itself, what has been found about this call site, if
  // anything, replaces what is assumed
  std::optional<int64_t> FSO = MaybeFSO;
  std::optional<unsigned> Unknown;
  bool IsRecursive = Callee.isValid() and Callee == Caller;
  if (not IsTailCall and (Callee.isInvalid() or IsRecursive)) {
    UnknownCall &New = UnknownCalls->emplace_back();
    New.Block = CallerBlock;
    New.FinalStackOffset = MaybeFSO;
    New.IsRecursive = IsRecursive;
    if (not isa<ConstantPointerNull>(SymbolNamePointer))
      New.Symbol = extractFromConstantStringPtr(SymbolNamePointer).str();
    llvm::BasicBlock &Block = *Builder.GetInsertBlock();
    const Instruction *End = &*Builder.GetInsertPoint();
    for (Instruction &I : Block) {
      if (&I == End)
        break;
      if (std::optional NewPC = NewPCHelper.getCall(&I))
        New.Instruction = blockIDFromNewPC(*NewPC).start();
    }
    New.Pushed = pushedBeforeCall(Block, End, SPCSV, nullptr);
    New.PushedAfterSaves = pushedBeforeCall(Block, End, SPCSV, CalleeSaved);

    // If the caller removes what it pushed, the callee does not
    uint64_t Removed = removedAfterCall(Block, SPCSV);
    New.Removed = Removed;
    if (Removed != 0 and Removed <= New.Pushed) {
      New.Pushed = 0;
      New.PushedAfterSaves = 0;
    }

    auto It = CallSiteFSO->find(CallerBlock);
    if (FSO.has_value() and It != CallSiteFSO->end())
      FSO = It->second;

    auto UnknownIt = SymbolicSites->find(CallerBlock);
    if (UnknownIt != SymbolicSites->end())
      Unknown = UnknownIt->second;
  } else if (not IsTailCall and FSO.has_value()) {
    llvm::BasicBlock &Block = *Builder.GetInsertBlock();
    const Instruction *End = &*Builder.GetInsertPoint();
    KnownCall New;
    New.Pushed = pushedBeforeCall(Block, End, SPCSV, nullptr);
    New.PushedAfterSaves = pushedBeforeCall(Block, End, SPCSV, CalleeSaved);
    New.FinalStackOffset = *FSO;
    New.Removed = removedAfterCall(Block, SPCSV);
    KnownCalls->insert_or_assign(CallerBlock, New);
  }

  // What the caller is taken to have removed without code that does it, see
  // CFGAnalyzer::analyzeElidedCleanups
  if (not IsTailCall and FSO.has_value()) {
    auto It = ElidedCleanups->find(CallerBlock);
    if (It != ElidedCleanups->end())
      FSO = *FSO + It->second;
  }

  // Adjust back the stack pointer
  if (not IsTailCall) {
    if (Unknown.has_value()) {
      auto *StackPointer = Builder.createLoad(SPCSV);
      Type *IndexType = StackEffect->getFunctionType()->getParamType(0);
      Value *Index = ConstantInt::get(IndexType, *Unknown);
      Value *Offset = Builder.CreateCall(StackEffect, { Index });
      auto *AdjustedStackPointer = Builder.CreateAdd(StackPointer, Offset);
      Builder.CreateStore(AdjustedStackPointer, SPCSV);
    } else if (FSO.has_value()) {
      auto *StackPointer = Builder.createLoad(SPCSV);
      Value *Offset = ConstantInt::get(StackPointer->getType(), *FSO);
      auto *AdjustedStackPointer = Builder.CreateAdd(StackPointer, Offset);
      Builder.CreateStore(AdjustedStackPointer, SPCSV);
    }
  }

  // Mark end of basic block with a post-hook call
  Builder.CreateCall(PostCallHook, Args);
}

void CallSummarizer::handlePostNoReturn(revng::IRBuilder &Builder,
                                        const llvm::DebugLoc &DbgLocation) {
  emitAbort(Builder, "", DbgLocation);
}

void CallSummarizer::handleIndirectJump(revng::IRBuilder &Builder,
                                        MetaAddress Block,
                                        const CSVSet &ClobberedRegisters,
                                        llvm::Value *SymbolNamePointer) {
  bool EmitCallHook = true;

  if (ReturnBlocks != nullptr) {
    bool IsReturn = ReturnBlocks->count(Block) != 0;
    EmitCallHook = not IsReturn;
  }

  CallInst *NewPreCallHook = nullptr;
  CallInst *NewPostCallHook = nullptr;
  if (EmitCallHook) {
    NewPreCallHook = Builder.CreateCall(PreCallHook,
                                        { Block.toValue(M),
                                          MetaAddress::invalid().toValue(M),
                                          SymbolNamePointer,
                                          Builder.getTrue() });

    // Emulate registers being clobbered, in the order of the names
    for (GlobalVariable *Register : toSortedByName(ClobberedRegisters))
      Clobberer.clobber(Builder, Register);
    CalleeClobbered->insert(ClobberedRegisters.begin(),
                            ClobberedRegisters.end());

    NewPostCallHook = Builder.CreateCall(PostCallHook,
                                         { Block.toValue(M),
                                           MetaAddress::invalid().toValue(M),
                                           SymbolNamePointer,
                                           Builder.getTrue() });
  }

  Builder.CreateCall(RetHook, { Block.toValue(M) });

  if (EmitCallHook) {
    NewPreCallHook->getParent()->splitBasicBlockBefore(NewPreCallHook);
    auto *PostPostCallHook = NewPostCallHook->getNextNode();
    NewPostCallHook->getParent()->splitBasicBlockBefore(PostPostCallHook);
  }
}

} // namespace efa
