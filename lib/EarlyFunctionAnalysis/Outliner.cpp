//
// This file is distributed under the MIT License. See LICENSE.md for details.
//

#include "llvm/ADT/DepthFirstIterator.h"
#include "llvm/ADT/SCCIterator.h"
#include "llvm/IR/GlobalValue.h"
#include "llvm/IR/InstIterator.h"
#include "llvm/IR/PatternMatch.h"
#include "llvm/Support/Casting.h"
#include "llvm/Support/ScopedPrinter.h"
#include "llvm/Transforms/Utils/BasicBlockUtils.h"
#include "llvm/Transforms/Utils/Cloning.h"
#include "llvm/Transforms/Utils/CodeExtractor.h"
#include "llvm/Transforms/Utils/ValueMapper.h"

#include "revng/ADT/Queue.h"
#include "revng/BasicAnalyses/CSVGlobals.h"
#include "revng/EarlyFunctionAnalysis/CallGraph.h"
#include "revng/EarlyFunctionAnalysis/CallHandler.h"
#include "revng/EarlyFunctionAnalysis/Outliner.h"
#include "revng/Lift/Helpers.h"
#include "revng/Model/FunctionTags.h"
#include "revng/Model/IRHelpers.h"
#include "revng/Support/BlockType.h"
#include "revng/Support/EmitAbort.h"
#include "revng/Support/FunctionCallMarker.h"
#include "revng/Support/IRBuilder.h"
#include "revng/Support/IRHelpers.h"
#include "revng/Support/MetaAddress.h"
#include "revng/Support/NewPC.h"
#include "revng/Support/OpaqueRegisterUser.h"

using namespace llvm;

namespace efa {

class OutlinedFunctionsMap {
private:
  Module *M = nullptr;
  std::map<MetaAddress, UniqueValuePtr<Function>> Map;
  std::set<MetaAddress> Banned;

public:
  OutlinedFunctionsMap(Module *M) : M(M) {}
  ~OutlinedFunctionsMap() { clear(); }

public:
  auto begin() { return Map.begin(); }
  auto end() { return Map.end(); }

public:
  void clear() { Map.clear(); }

public:
  Function *get(MetaAddress Entry) {
    using namespace llvm;

    auto It = Map.find(Entry);
    if (It == Map.end()) {
      LLVMContext &Context = M->getContext();
      auto *FT = FunctionType::get(Type::getVoidTy(Context), {}, false);
      Function *New = Function::Create(FT,
                                       llvm::GlobalObject::ExternalLinkage,
                                       0,
                                       {},
                                       M);
      Map.insert(It, { Entry, UniqueValuePtr<Function>(New) });
      return New;
    } else {
      return It->second.get();
    }
  }

  void set(MetaAddress Entry, Function *F) { Map[Entry].reset(F); }

public:
  bool isBanned(MetaAddress BB) const { return Banned.contains(BB); }

  bool banRecursiveFunctions();
};

bool OutlinedFunctionsMap::banRecursiveFunctions() {
  using namespace llvm;
  bool Result = false;

  CallGraph CallGraph;
  std::map<Function *, BasicBlockNode *> ReverseMap;

  BasicBlockNode EntryNode(MetaAddress::invalid());
  BasicBlockNode *Entry = CallGraph.addNode(EntryNode);
  CallGraph.setEntryNode(Entry);

  // Populate graph and map
  for (auto &[Block, Function] : Map) {
    // We don't care about the address
    BasicBlockNode Node(MetaAddress::invalid());
    auto *NewNode = CallGraph.addNode(Node);
    ReverseMap[Function.get()] = NewNode;
    Entry->addSuccessor(NewNode);
  }

  // Add edges
  for (auto &[_, Function] : Map) {
    BasicBlockNode *Caller = ReverseMap.at(Function.get());
    for (Instruction &I : instructions(Function.get())) {
      if (auto *Call = dyn_cast<CallInst>(&I)) {
        auto It = ReverseMap.find(getCalledFunction(Call));
        if (It != ReverseMap.end()) {
          Caller->addSuccessor(It->second);
        }
      }
    }
  }

  // Ban everyone part of an SCC
  auto It = scc_begin(&CallGraph);
  auto End = scc_end(&CallGraph);
  for (; It != End; ++It) {
    if (not It.hasCycle())
      continue;

    auto &SCCNodes = *It;
    Result = Result or (SCCNodes.size() > 0);
    for (auto &Node : SCCNodes)
      Banned.insert(Node->Address);
  }

  return Result;
}

llvm::CallInst *Outliner::stubOf(MetaAddress Callee) {
  using namespace llvm;

  if (Callee.isInvalid())
    return nullptr;

  auto [It, IsNew] = Stubs.try_emplace(Callee, nullptr);
  if (not IsNew)
    return It->second;

  // One instruction, a jump, to what the lifter names as an imported function
  llvm::BasicBlock *Entry = RootF.getBlockAt(Callee);
  if (Entry == nullptr or getMarker(Entry, FunctionCallMarker) != nullptr)
    return nullptr;

  CallInst *JumpToSymbol = getMarker(Entry, JumpToSymbolMarker);
  if (JumpToSymbol == nullptr)
    return nullptr;

  unsigned Instructions = 0;
  for (Instruction &I : *Entry)
    if (NewPCHelper.getCall(&I).has_value())
      ++Instructions;
  if (Instructions != 1)
    return nullptr;

  Stubs[Callee] = JumpToSymbol;
  return JumpToSymbol;
}

void Outliner::integrateFunctionCallee(CallHandler *TheCallHandler,
                                       MetaAddress CallerFunction,
                                       llvm::BasicBlock *CallerBlock,
                                       llvm::CallInst *FunctionCall,
                                       llvm::CallInst *JumpToSymbol,
                                       MetaAddress Callee,
                                       OutlinedFunctionsMap &FunctionsMap) {
  llvm::LLVMContext &Context = M.getContext();

  auto &&[Summary, IsTailCall] = getCallSiteInfo(CallerFunction,
                                                 CallerBlock,
                                                 FunctionCall,
                                                 JumpToSymbol,
                                                 Callee);

  using namespace llvm;
  using llvm::BasicBlock;

  BasicBlock *BB = FunctionCall != nullptr ? FunctionCall->getParent() :
                                             JumpToSymbol->getParent();
  MetaAddress CallerBlockAddress = getBasicBlockAddress(getJumpTargetBlock(BB));
  revng_assert(CallerBlockAddress.isValid());

  bool TargetIsSymbol = JumpToSymbol != nullptr;
  Value *SymbolNamePointer = nullptr;
  if (TargetIsSymbol) {
    SymbolNamePointer = JumpToSymbol->getArgOperand(0);
  } else {
    using CPN = ConstantPointerNull;
    Type *I8Ptr = Type::getInt8PtrTy(Context);
    SymbolNamePointer = CPN::get(Type::getInt8PtrTy(Context));
  }

  // A call through a pointer whose callee is known all the same
  if (ResolvedCallees != nullptr and FunctionCall != nullptr
      and Callee.isInvalid() and not TargetIsSymbol) {
    auto It = ResolvedCallees->find(CallerBlockAddress);
    if (It != ResolvedCallees->end()) {
      Callee = It->second.first;
      if (not It->second.second.empty())
        SymbolNamePointer = getUniqueString(&M, It->second.second);
    }
  }

  // A call to a function that is only a jump to an imported one is a call to
  // the imported one
  if (FunctionCall != nullptr and not TargetIsSymbol) {
    if (CallInst *Stub = stubOf(Callee)) {
      Callee = MetaAddress::invalid();
      SymbolNamePointer = Stub->getArgOperand(0);
    }
  }

  if (FunctionCall == nullptr)
    IsTailCall = true;

  // Ignore any indirect tail call (i.e., not direct and not targeting a dynamic
  // symbol)
  if (IsTailCall and Callee.isInvalid() and SymbolNamePointer == nullptr)
    return;

  // The function call is replaced with 1) hooks that delimit the space of the
  // ABI analyses' traversals and 2) a summary of the registers clobbered by
  // that function.

  // What is the function type of the callee?

  // Inline if 1) marked as inline, 2) not the caller function itself and 3)
  // not banned from inlining
  bool IsInline = false;
  bool IsCallToUnexpected = false;
  if (FunctionCall != nullptr) {
    using namespace model::FunctionAttribute;
    bool IsMarkedInline = Summary->Attributes.contains(HasOneBrokenReturn);
    bool IsCallingTheCaller = CallerFunction == Callee;
    bool IsBanned = FunctionsMap.isBanned(Callee);
    IsInline = IsMarkedInline and not IsCallingTheCaller and not IsBanned;
  }

  using namespace model::FunctionAttribute;
  bool IsNoReturn = Summary->Attributes.contains(NoReturn);

  if (IsInline) {
    // Emit a call to the function to inline: a later step will inline this call
    revng_assert(Callee.isValid());
    auto *CI = CallInst::Create(FunctionsMap.get(Callee),
                                "",
                                BB->getTerminator());
  } else {
    Instruction *Term = BB->getTerminator();

    // TODO: the checks should be enabled conditionally based on the user.
    revng::IRBuilder Builder(Term);

    if (FunctionCall != nullptr) {
      // This is a proper call, storing the return address somewhere: inject a
      // store clobbering the return address. This will make the instruction
      // saving the return address dead, the optimizer will collect it and then
      // we'll drop this store too, resulting in an IR free of return addresses
      // being written around.
      Value *LinkRegister = FunctionCall->getArgOperand(3);
      Value *Pointer = nullptr;
      Type *PointeeType = nullptr;

      // Are we using the link register or storing the return address on the top
      // of the stack?
      if (auto *CSV = dyn_cast_or_null<GlobalVariable>(LinkRegister)) {
        Pointer = CSV;
        PointeeType = CSV->getValueType();
      } else {
        PointeeType = Globals.spReg()->getValueType();
        Pointer = Builder.CreateIntToPtr(Builder.createLoad(Globals.spReg()),
                                         PointeeType->getPointerTo());
      }

      // Inject the store
      std::string Name = ("opaque_"
                          + to_string(PointeeType->getIntegerBitWidth()));
      auto &Pool = State.OpaqueReturnAddress;
      auto *OpaqueReturnAddressFunction = Pool.get(PointeeType,
                                                   PointeeType,
                                                   {},
                                                   Name);
      Builder.CreateStore(Builder.CreateCall(OpaqueReturnAddressFunction),
                          Pointer);
    }

    TheCallHandler->handleCall(CallerBlockAddress,
                               Builder,
                               Callee,
                               Summary->ClobberedRegisters,
                               Summary->ElectedFSO,
                               IsNoReturn,
                               IsTailCall,
                               SymbolNamePointer);

    if (IsNoReturn) {
      llvm::DebugLoc DebugLocation = Term->getDebugLoc();
      Term->eraseFromParent();

      Builder.SetInsertPoint(BB);
      TheCallHandler->handlePostNoReturn(Builder, DebugLocation);
    }
  }

  // Erase markers
  if (FunctionCall != nullptr)
    eraseFromParent(FunctionCall);

  if (JumpToSymbol != nullptr)
    eraseFromParent(JumpToSymbol);
}

OutlinedFunction
Outliner::outlineFunctionInternal(CallHandler *TheCallHandler,
                                  const MetaAddress &FunctionAddress,
                                  OutlinedFunctionsMap &FunctionsToInline) {
  using namespace llvm;
  using llvm::BasicBlock;

  OutlinedFunction OutlinedFunction;
  OutlinedFunction.Address = FunctionAddress;
  OnceQueue<BasicBlock *> Queue;
  std::vector<BasicBlock *> BlocksToClone;

  auto *AnyPCBB = RootF.anyPC();
  auto *UnexpectedPCBB = RootF.unexpectedPC();
  Function *Root = AnyPCBB->getParent();

  BasicBlock *EntryBlock = RootF.getBlockAt(FunctionAddress);
  bool HasEntryBlock = EntryBlock != nullptr;

  if (HasEntryBlock)
    Queue.insert(EntryBlock);

  // The code the function goes on with after an exception is handled is
  // part of it too, although nothing in the function jumps there
  SmallVector<BasicBlock *, 2> ResumeBlocks;
  const auto &Functions = Oracle.binary().Functions();
  if (const model::Function *Function = Functions.tryGet(FunctionAddress)) {
    for (const MetaAddress &Address : Function->ResumeAddresses()) {
      BasicBlock *Block = RootF.getBlockAt(Address);
      if (Block != nullptr and not is_contained(ResumeBlocks, Block)) {
        ResumeBlocks.push_back(Block);
        Queue.insert(Block);
      }
    }
  }

  Queue.insert(AnyPCBB);
  Queue.insert(UnexpectedPCBB);

  //
  // Collect list of blocks to clone
  //
  while (not Queue.empty()) {
    BasicBlock *Current = Queue.pop();
    BlocksToClone.emplace_back(Current);

    if (auto *FunctionCall = getMarker(Current, FunctionCallMarker)) {
      // Compute callee
      MetaAddress PCCallee = MetaAddress::invalid();
      if (auto *Next = getOnlyCallee(Current, Oracle.binary()))
        PCCallee = getBasicBlockAddress(Next);

      CallInst *JumpToSymbol = getMarker(Current, JumpToSymbolMarker);
      auto &&[Summary, IsTailCall] = getCallSiteInfo(FunctionAddress,
                                                     FunctionCall->getParent(),
                                                     FunctionCall,
                                                     JumpToSymbol,
                                                     PCCallee);

      // Unless it's NoReturn, enqueue the call fallthrough
      using namespace model::FunctionAttribute;
      bool IsNoReturn = Summary->Attributes.contains(NoReturn);
      if (not IsNoReturn and not IsTailCall) {
        Queue.insert(getFallthrough(Current));
      }
    } else {
      for (auto *Successor : successors(Current)) {
        if (not isPartOfRootDispatcher(Successor))
          Queue.insert(Successor);
      }
    }
  }

  //
  // Create a copy of all the basic blocks to outline in `root`
  //
  llvm::ValueToValueMapTy VMap;
  SmallVector<BasicBlock *, 8> BlocksToExtract;

  for (const auto &BB : BlocksToClone) {
    BasicBlock *Cloned = CloneBasicBlock(BB, VMap, Twine("_cloned"), Root);

    VMap[BB] = Cloned;
    BlocksToExtract.emplace_back(Cloned);
  }

  OutlinedFunction.AnyPCCloned = cast<BasicBlock>(VMap[AnyPCBB]);
  OutlinedFunction.UnexpectedPCCloned = cast<BasicBlock>(VMap[UnexpectedPCBB]);

  //
  // Update references in cloned basic blocks
  //
  remapInstructionsInBlocks(BlocksToExtract, VMap);

  // The block the function is resumed from: whoever handles the exception
  // chooses where the function goes on. What leads to this block is added
  // when the function is complete, see connectResumePoint.
  if (not ResumeBlocks.empty()) {
    LLVMContext &Context = M.getContext();
    BasicBlock *Resume = BasicBlock::Create(Context, "resume", Root);
    revng::IRBuilder Builder(Resume);
    Function *Helper = exceptionHelper("revng_exception_resume", true);
    SmallVector<Value *, 1> Arguments;
    if (Helper->arg_size() == 1)
      Arguments.push_back(Builder.createLoad(M.getGlobalVariable("_ebp",
                                                                 true)));
    auto *Selector = Builder.CreateCall(Helper, Arguments);
    auto *Switch = Builder.CreateSwitch(Selector,
                                        cast<BasicBlock>(VMap[ResumeBlocks[0]]),
                                        ResumeBlocks.size() - 1);
    for (unsigned I = 1; I < ResumeBlocks.size(); ++I)
      Switch->addCase(Builder.getInt32(I),
                      cast<BasicBlock>(VMap[ResumeBlocks[I]]));
    BlocksToExtract.emplace_back(Resume);
    OutlinedFunction.ResumeCloned = Resume;
  }

  //
  // Switch successor of function calls from callee to fallthrough
  //
  std::map<llvm::CallInst *, MetaAddress> CallToCallee;
  for (const auto &BB : BlocksToExtract) {
    if (CallInst *FunctionCall = getMarker(BB, FunctionCallMarker)) {
      auto *Term = BB->getTerminator();

      // If the function callee is null and the call can go to more than one
      // place, we are dealing with an indirect call
      MetaAddress PCCallee = MetaAddress::invalid();
      if (BasicBlock *Next = getOnlyCallee(Term, Oracle.binary()))
        PCCallee = getBasicBlockAddress(Next);

      CallToCallee[FunctionCall] = PCCallee;

      CallInst *JumpToSymbol = getMarker(BB, JumpToSymbolMarker);
      auto &&[CalleeSummary, IsTailCall] = getCallSiteInfo(FunctionAddress,
                                                           BB,
                                                           FunctionCall,
                                                           JumpToSymbol,
                                                           PCCallee);

      using namespace model::FunctionAttribute;
      bool IsNoReturn = CalleeSummary->Attributes.contains(NoReturn);
      if (IsNoReturn) {
        auto *BB = Term->getParent();
        Term->eraseFromParent();

        // TODO: the checks should be enabled conditionally based on the user.
        revng::IRBuilder Builder(BB);
        Builder.CreateUnreachable();

        // Ensure markers are still close to the terminator
        if (JumpToSymbol != nullptr)
          JumpToSymbol->moveBefore(BB->getTerminator());
        FunctionCall->moveBefore(BB->getTerminator());
      } else if (IsTailCall) {
        revng_assert(not cast<BranchInst>(Term)->isConditional());
        auto *Br = BranchInst::Create(cast<BasicBlock>(VMap[AnyPCBB]));
        ReplaceInstWithInst(Term, Br);
      } else {
        auto *Br = BranchInst::Create(getFallthrough(Term));
        ReplaceInstWithInst(Term, Br);
      }

      // To allow correct function extraction, there must not exist users of
      // BBs to be extracted, so we destroy the blockaddress of the
      // fall-through BB in the `function_call` marker.
      unsigned ArgNo = 0;
      PointerType *I8PtrTy = Type::getInt8PtrTy(M.getContext());
      Constant *I8NullPtr = ConstantPointerNull::get(I8PtrTy);
      for (Value *Arg : FunctionCall->args()) {
        if (isa<BlockAddress>(Arg)) {
          FunctionCall->setArgOperand(ArgNo, I8NullPtr);
          if (Arg->use_empty())
            cast<BlockAddress>(Arg)->destroyConstant();
        }
        ++ArgNo;
      }
    }
  }

  if (not HasEntryBlock) {
    auto *EntryBlock = BasicBlock::Create(Root->getContext(), "", Root);
    emitAbort(EntryBlock, "This starts at non-executable address");
    BlocksToExtract.insert(BlocksToExtract.begin(), EntryBlock);
  }

  // Extract outlined function
  llvm::Function *F = CodeExtractor(BlocksToExtract)
                        .extractCodeRegion(State.CEAC);
  F->addFnAttr(Attribute::NullPointerIsValid);
  OutlinedFunction.Function = UniqueValuePtr<llvm::Function>(F);

  revng_assert(OutlinedFunction.Function != nullptr);
  revng_assert(OutlinedFunction.Function->arg_size() == 0);
  revng_assert(OutlinedFunction.Function->getReturnType()->isVoidTy());
  revng_assert(OutlinedFunction.Function->hasOneUser());

  // Remove the only user (call to the outlined function) in `root`
  auto It = OutlinedFunction.Function->user_begin();
  cast<Instruction>(*It)->getParent()->eraseFromParent();

  // The code does not go on after it raises an exception: what the lifter
  // emits after such a call, a jump to wherever the program counter is left,
  // is how the emulator resumes, not something the program does. Without this
  // a software interrupt is a jump to an unknown place, and the bytes that pad
  // the code, which are interrupts, lead into whatever follows them.
  SmallVector<Instruction *, 4> Raising;
  for (Instruction &I : llvm::instructions(F))
    if (auto *Call = dyn_cast<CallInst>(&I))
      if (const Function *Callee = getCalledFunction(Call))
        if (FunctionTags::Exceptional.isTagOf(Callee))
          Raising.push_back(Call);

  for (Instruction *Call : Raising) {
    BasicBlock *BB = Call->getParent();
    BB->splitBasicBlock(Call->getNextNode());
    Instruction *OldTerminator = BB->getTerminator();
    new UnreachableInst(M.getContext(), BB);
    eraseFromParent(OldTerminator);
  }

  if (not Raising.empty()) {
    // `anypc` and `unexpectedpc` stay: they are looked for later on
    df_iterator_default_set<BasicBlock *, 32> Reachable;
    for (BasicBlock *Root : { &F->getEntryBlock(),
                              OutlinedFunction.AnyPCCloned,
                              OutlinedFunction.UnexpectedPCCloned,
                              OutlinedFunction.ResumeCloned }) {
      if (Root == nullptr or Root->getParent() != F)
        continue;
      for (BasicBlock *BB : depth_first_ext(Root, Reachable))
        (void) BB;
    }

    SmallVector<BasicBlock *, 8> Dead;
    for (BasicBlock &BB : *F)
      if (not Reachable.contains(&BB))
        Dead.push_back(&BB);
    DeleteDeadBlocks(Dead);
  }

  // Integrate function callee
  for (auto &BB : *OutlinedFunction.Function) {
    MetaAddress Callee;
    CallInst *FunctionCall = getMarker(&BB, FunctionCallMarker);
    if (FunctionCall != nullptr)
      Callee = CallToCallee.at(FunctionCall);
    CallInst *JumpToSymbol = getMarker(&BB, JumpToSymbolMarker);
    // TODO: we don't integrate the call if it's a tail call (JumpToSymbol but
    //       no FunctionCall). Is this OK?
    if (FunctionCall != nullptr) {
      integrateFunctionCallee(TheCallHandler,
                              FunctionAddress,
                              &BB,
                              FunctionCall,
                              JumpToSymbol,
                              Callee,
                              FunctionsToInline);
    }
  }

  return OutlinedFunction;
}

void Outliner::createAnyPCHooks(CallHandler *TheCallHandler,
                                OutlinedFunction *OutlinedFunction) {
  using namespace llvm;
  LLVMContext &Context = M.getContext();

  SmallVector<Instruction *, 4> IndirectBranchPredecessors;
  if (OutlinedFunction->AnyPCCloned) {
    for (auto *Pred : predecessors(OutlinedFunction->AnyPCCloned)) {
      auto *Term = Pred->getTerminator();
      IndirectBranchPredecessors.emplace_back(Term);
    }
  }

  // Initialize ret-hook marker (needed for the ABI analyses on return values)
  // and fix pre-hook marker upon encountering a jump to `anypc`. Since we don't
  // know in advance whether it will be classified as a return or indirect tail
  // call, RegisterUsageAnalyses need to run on this potential call-site as
  // well. The results will not be merged eventually, if the indirect jump turns
  // out to be a proper return.
  for (auto *Term : IndirectBranchPredecessors) {
    auto *BB = Term->getParent();
    auto *JumpTargetBB = getJumpTargetBlock(BB);
    MetaAddress IndirectRetBBAddress = getBasicBlockAddress(JumpTargetBB);
    revng_assert(IndirectRetBBAddress.isValid());

    auto *Split = BB->splitBasicBlock(Term, BB->getName() + Twine("_anypc"));

    auto *JumpToAnyPC = Split->getTerminator();
    revng_assert(isa<BranchInst>(JumpToAnyPC));

    // TODO: the checks should be enabled conditionally based on the user.
    revng::IRBuilder Builder(JumpToAnyPC);

    using CPN = ConstantPointerNull;
    Value *SymbolName = CPN::get(Type::getInt8PtrTy(Context));
    CallInst *JumpToSymbol = getMarker(BB, JumpToSymbolMarker);

    auto &&[Summary, _] = getCallSiteInfo(OutlinedFunction->Address,
                                          BB,
                                          nullptr,
                                          JumpToSymbol,
                                          MetaAddress::invalid());

    if (JumpToSymbol != nullptr) {
      SymbolName = JumpToSymbol->getArgOperand(0);
      eraseFromParent(JumpToSymbol);
    }

    TheCallHandler->handleIndirectJump(Builder,
                                       IndirectRetBBAddress,
                                       Summary->ClobberedRegisters,
                                       SymbolName);
  }
}

class FixFunctionPreInlining {
private:
  unsigned InliningIndex = 0;
  std::set<std::pair<MetaAddress, llvm::Function *>> ToRestore;
  std::vector<MetaAddress> &InlinedFunctionsByIndex;

public:
  FixFunctionPreInlining(std::vector<MetaAddress> &InlinedFunctionsByIndex) :
    InlinedFunctionsByIndex(InlinedFunctionsByIndex) {}
  ~FixFunctionPreInlining() { revng_assert(ToRestore.size() == 0); }

public:
  void fix(const MetaAddress &CalleeAddress, llvm::CallBase *Caller) {
    Function *Callee = getCalledFunction(Caller);
    revng_assert(Callee != nullptr);
    setNewPCInlineIndex(CalleeAddress, Callee, ++InliningIndex);
    InlinedFunctionsByIndex.push_back(CalleeAddress);
    ToRestore.emplace(CalleeAddress, Callee);
  }

  void restore() {
    for (const auto &[Address, F] : ToRestore)
      setNewPCInlineIndex(Address, F, 0);
    ToRestore.clear();
  }

private:
  static void setNewPCInlineIndex(const MetaAddress &FunctionAddress,
                                  llvm::Function *F,
                                  unsigned InliningIndex) {
    revng_assert(FunctionAddress.isValid());

    for (llvm::Instruction &I : llvm::instructions(F)) {
      if (std::optional NewPCCall = NewPCHelper.getCall(&I)) {
        BasicBlockID OldID = blockIDFromNewPC(*NewPCCall);
        BasicBlockID NewID(OldID.start(), InliningIndex);
        NewPCCall->setArgument(NewPCArgument::InstructionID,
                               NewID.toValue(F->getParent()));
      }
    }
  }
};

OutlinedFunction Outliner::outline(const MetaAddress &Entry,
                                   CallHandler *Handler) {
  using namespace llvm;

  OutlinedFunction Result;
  OutlinedFunctionsMap FunctionsToInline(&M);

  unsigned Attempts = 0;
  do {
    ++Attempts;
    revng_assert(Attempts < 3);

    // Restart isolating functions
    FunctionsToInline.clear();

    // Outline functions but do not perform inlining
    Result = outlineFunctionInternal(Handler, Entry, FunctionsToInline);

    //
    // Fixed point creation of function that needs to be inlined
    //
    bool SomethingNew = true;
    while (SomethingNew) {
      SomethingNew = false;
      for (const auto &[Address, F] : FunctionsToInline) {
        if (F->isDeclaration()) {
          Function *ToInline = createFunctionToInline(Handler,
                                                      Address,
                                                      FunctionsToInline);
          F->replaceAllUsesWith(ToInline);
          FunctionsToInline.set(Address, ToInline);
          SomethingNew = true;
        }
      }
    }

  } while (FunctionsToInline.banRecursiveFunctions());

  //
  // Fixed point inlining
  //
  bool SomethingNew = true;
  FixFunctionPreInlining FunctionFixer(Result.InlinedFunctionsByIndex);
  while (SomethingNew) {
    SomethingNew = false;
    for (auto &[Address, F] : FunctionsToInline) {
      SmallVector<CallBase *> ToInline;
      for (CallBase *Caller : callers(F.get()))
        ToInline.push_back(Caller);

      for (CallBase *Caller : ToInline) {
        FunctionFixer.fix(Address, Caller);
        InlineFunctionInfo IFI;
        bool Success = InlineFunction(*Caller, IFI).isSuccess();
        revng_assert(Success);
        SomethingNew = true;
      }
    }
  }

  FunctionFixer.restore();

  // Everything we gathered, including the code of the functions we inlined,
  // is now part of the control-flow graph of `Entry`
  setNewPCOwner(Result.Function.get(), Entry);

  // Fix `unexpectedpc` of the callees to inline
  for (auto &I : instructions(Result.Function.get())) {
    if (CallInst *Call = getCallTo(&I, UnexpectedPCMarker.get())) {
      revng_assert(Result.UnexpectedPCCloned != nullptr);

      auto *Br = BranchInst::Create(Result.UnexpectedPCCloned);
      ReplaceInstWithInst(I.getParent()->getTerminator(), Br);
      Call->eraseFromParent();
      break;
    }
  }

  connectResumePoint(&Result);

  createAnyPCHooks(Handler, &Result);

  return Result;
}

llvm::Function *Outliner::exceptionHelper(llvm::StringRef Name, bool IsResume) {
  using namespace llvm;

  if (Function *Existing = M.getFunction(Name))
    return Existing;

  // Being resumed takes the frame of the function: what ran since the
  // function saved its stack pointer changed what is in it, and that is not
  // to be taken for what it was. Of the registers, the one that holds the
  // frame is all the function can count on.
  LLVMContext &Context = M.getContext();
  Type *Int32 = Type::getInt32Ty(Context);
  GlobalVariable *FramePointer = M.getGlobalVariable("_ebp", true);
  SmallVector<Type *, 1> Parameters;
  if (IsResume and FramePointer != nullptr)
    Parameters.push_back(FramePointer->getValueType());
  auto *Type = FunctionType::get(Int32, Parameters, false);
  auto *Helper = Function::Create(Type, GlobalValue::ExternalLinkage, Name, &M);
  FunctionTags::Helper.addTo(Helper);

  QuickMetadata QMD(Context);
  SmallVector<Metadata *, 16> Read;
  SmallVector<Metadata *, 16> Written;
  if (IsResume) {
    for (GlobalVariable *CSV : Globals.abiRegisters())
      if (CSV != nullptr and not Globals.isSPReg(CSV) and CSV != FramePointer)
        Written.push_back(QMD.get(CSV->getName()));
  }

  auto Usage = [&QMD](ArrayRef<Metadata *> CSVs) {
    return QMD.tuple({ QMD.get(uint32_t(0)), QMD.tuple(CSVs) });
  };
  Helper->setMetadata("revng.csvaccess.offsets.load", Usage(Read));
  Helper->setMetadata("revng.csvaccess.offsets.store", Usage(Written));

  return Helper;
}

std::pair<llvm::SmallVector<llvm::StoreInst *, 2>, int64_t>
Outliner::stackPointerSaves(llvm::Function &F) const {
  using namespace llvm;
  using namespace llvm::PatternMatch;

  GlobalVariable *StackPointer = Globals.spReg();
  GlobalVariable *FramePointer = M.getGlobalVariable("_ebp", true);

  // Where the function saves its stack pointer in its frame, in one of the
  // two places the code that handles exceptions on 32-bit Windows restores
  // it from: below the record of a C++ handler, and below that of a
  // structured one. From there on the function can be resumed.
  SmallVector<StoreInst *, 2> Anchors;
  std::optional<int64_t> Offset;
  if (FramePointer == nullptr)
    return { Anchors, 0 };

  for (Instruction &I : instructions(F)) {
    auto *Store = dyn_cast<StoreInst>(&I);
    if (Store == nullptr)
      continue;

    auto *Stored = dyn_cast<LoadInst>(Store->getValueOperand());
    if (Stored == nullptr or Stored->getPointerOperand() != StackPointer)
      continue;

    Value *Address = nullptr;
    if (not match(Store->getPointerOperand(), m_IntToPtr(m_Value(Address))))
      continue;
    if (auto *Extended = dyn_cast<ZExtInst>(Address))
      Address = Extended->getOperand(0);

    Value *Base = nullptr;
    ConstantInt *Displacement = nullptr;
    if (not match(Address, m_Add(m_Value(Base), m_ConstantInt(Displacement))))
      continue;
    auto *Frame = dyn_cast<LoadInst>(Base);
    if (Frame == nullptr or Frame->getPointerOperand() != FramePointer)
      continue;

    int64_t Slot = Displacement->getSExtValue();
    if (Slot != -0x10 and Slot != -0x18)
      continue;

    // Saved in two different places: neither is to be trusted
    if (Offset.has_value() and *Offset != Slot)
      return { {}, 0 };
    Offset = Slot;
    Anchors.push_back(Store);
  }

  return { Anchors, Offset.value_or(0) };
}

void Outliner::connectResumePoint(OutlinedFunction *Outlined) {
  using namespace llvm;
  using namespace llvm::PatternMatch;
  using llvm::BasicBlock;

  BasicBlock *Resume = Outlined->ResumeCloned;
  if (Resume == nullptr)
    return;

  Function *F = Outlined->Function.get();
  GlobalVariable *StackPointer = Globals.spReg();
  GlobalVariable *FramePointer = M.getGlobalVariable("_ebp", true);
  auto &&[Anchors, SlotOffset] = stackPointerSaves(*F);
  std::optional<int64_t> Offset = SlotOffset;
  bool IsUnique = true;

  // Only where the function itself gets to counts: code it would be resumed
  // at does not make a function one that can be resumed, and would be left
  // with nothing leading to it from the entry
  df_iterator_default_set<BasicBlock *, 32> FromEntry;
  for (BasicBlock *BB : depth_first_ext(&F->getEntryBlock(), FromEntry))
    (void) BB;
  llvm::erase_if(Anchors, [&FromEntry](StoreInst *Anchor) {
    return not FromEntry.contains(Anchor->getParent());
  });

  if (not IsUnique or Anchors.empty()) {
    // Nothing leads to the code the function would be resumed at: it goes
    // away, with whatever only it reaches
    df_iterator_default_set<BasicBlock *, 32> Reachable;
    for (BasicBlock *Root : { &F->getEntryBlock(),
                              Outlined->AnyPCCloned,
                              Outlined->UnexpectedPCCloned }) {
      if (Root == nullptr or Root->getParent() != F)
        continue;
      for (BasicBlock *BB : depth_first_ext(Root, Reachable))
        (void) BB;
    }

    SmallVector<BasicBlock *, 8> Dead;
    for (BasicBlock &BB : *F)
      if (not Reachable.contains(&BB))
        Dead.push_back(&BB);
    DeleteDeadBlocks(Dead);
    Outlined->ResumeCloned = nullptr;
    return;
  }

  // The function is resumed with the stack pointer it saved, and still
  // finds it where it saved it
  Instruction *Selector = nullptr;
  for (Instruction &I : *Resume)
    if (isa<CallInst>(&I))
      Selector = &I;
  revng_assert(Selector != nullptr);
  revng::IRBuilder Builder(&Resume->front());
  Type *StackPointerType = StackPointer->getValueType();
  Value *Frame = Builder.createLoad(FramePointer);
  Value *Address = Builder.CreateAdd(Frame,
                                     ConstantInt::getSigned(Frame->getType(),
                                                            *Offset));
  Value *Slot = Builder.CreateIntToPtr(Builder.CreateZExt(Address,
                                                          Builder.getInt64Ty()),
                                       StackPointerType->getPointerTo());
  Value *Saved = Builder.CreateLoad(StackPointerType, Slot);
  Builder.SetInsertPoint(Selector->getNextNode());
  Builder.CreateStore(Saved, Slot);
  Builder.CreateStore(Saved, StackPointer);

  for (StoreInst *Anchor : Anchors) {
    BasicBlock *BB = Anchor->getParent();
    BasicBlock *Rest = BB->splitBasicBlock(Anchor->getNextNode());
    Instruction *OldTerminator = BB->getTerminator();
    revng::IRBuilder Builder(OldTerminator);
    Value *Caught = Builder.CreateCall(exceptionHelper("revng_exception_"
                                                       "caught",
                                                       false));
    Builder.CreateCondBr(Builder.CreateIsNotNull(Caught), Resume, Rest);
    eraseFromParent(OldTerminator);
  }
}

llvm::Function *
Outliner::createFunctionToInline(CallHandler *TheCallHandler,
                                 const MetaAddress &Entry,
                                 OutlinedFunctionsMap &FunctionsToInline) {
  using namespace llvm;
  LLVMContext &Context = M.getContext();

  // Recreate outlined function
  OutlinedFunction OF = outlineFunctionInternal(TheCallHandler,
                                                Entry,
                                                FunctionsToInline);

  // Adjust `anypc` and `unexpectedpc` BBs of the function to inline
  revng_assert(OF.AnyPCCloned != nullptr);

  // Functions to inline must have one and only one broken return
  revng_assert(OF.AnyPCCloned->hasNPredecessors(1));

  // Replace the broken return with a `ret`
  auto *Br = OF.AnyPCCloned->getUniquePredecessor()->getTerminator();
  auto *Ret = ReturnInst::Create(Context);
  ReplaceInstWithInst(Br, Ret);

  if (OF.UnexpectedPCCloned != nullptr) {
    CallInst::Create(UnexpectedPCMarker.get(),
                     "",
                     OF.UnexpectedPCCloned->getTerminator());
  }

  return OF.releaseFunction();
}

std::pair<const FunctionSummary *, bool>
Outliner::getCallSiteInfo(MetaAddress CallerFunction,
                          llvm::BasicBlock *CallerBlock,
                          llvm::CallInst *FunctionCall,
                          llvm::CallInst *JumpToSymbol,
                          MetaAddress Callee) {
  using namespace llvm;
  using llvm::BasicBlock;

  Instruction *Term = CallerBlock->getTerminator();

  // Extract MetaAddress of JT of the call-site
  BasicBlockID
    CallSiteAddress = getBasicBlockID(getJumpTargetBlock(CallerBlock));
  revng_assert(CallSiteAddress.isValid());

  StringRef CalledSymbol;
  if (JumpToSymbol != nullptr) {
    // The target is a symbol
    CalledSymbol = extractFromConstantStringPtr(JumpToSymbol->getArgOperand(0));
  }

  // A call through a pointer whose callee is known all the same
  if (ResolvedCallees != nullptr and FunctionCall != nullptr
      and Callee.isInvalid() and CalledSymbol.empty()
      and not CallSiteAddress.isInlined()) {
    auto It = ResolvedCallees->find(CallSiteAddress.start());
    if (It != ResolvedCallees->end()) {
      Callee = It->second.first;
      CalledSymbol = It->second.second;
    }
  }

  // A call to a function that is only a jump to an imported one is a call to
  // the imported one
  if (FunctionCall != nullptr and CalledSymbol.empty()) {
    if (CallInst *Stub = stubOf(Callee)) {
      Callee = MetaAddress::invalid();
      CalledSymbol = extractFromConstantStringPtr(Stub->getArgOperand(0));
    }
  }

  return Oracle.getCallSite(CallerFunction,
                            CallSiteAddress,
                            Callee,
                            CalledSymbol);
}

} // namespace efa
