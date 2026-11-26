#pragma once

//
// This file is distributed under the MIT License. See LICENSE.md for details.
//

#include "llvm/ADT/StringExtras.h"
#include "llvm/IR/Module.h"
#include "llvm/Support/ModRef.h"
#include "llvm/Transforms/Utils/CodeExtractor.h"

#include "revng/BasicAnalyses/RootFunction.h"
#include "revng/EarlyFunctionAnalysis/FunctionSummaryOracle.h"
#include "revng/EarlyFunctionAnalysis/TemporaryOpaqueFunction.h"
#include "revng/Model/Binary.h"
#include "revng/Model/FunctionTags.h"
#include "revng/Model/NamedTypedRegister.h"
#include "revng/Support/BlockType.h"
#include "revng/Support/OpaqueFunctionsPool.h"
#include "revng/Support/UniqueValuePtr.h"

namespace efa {

class CallHandler;
class OutlinedFunctionsMap;

/// Where a value is read from, and how many bytes
struct ReadPlace {
  MetaAddress Address = MetaAddress::invalid();
  unsigned Size = 0;
};

/// \return where \p Target is read from, if it is read from an address that
///         is known
inline std::optional<ReadPlace> placeReadFrom(llvm::Value *Target,
                                              const model::Binary &Binary) {
  using namespace llvm;

  while (isa<TruncInst>(Target) or isa<ZExtInst>(Target))
    Target = cast<Instruction>(Target)->getOperand(0);
  auto *Load = dyn_cast<LoadInst>(Target);
  if (Load == nullptr)
    return std::nullopt;

  // The address: a number, or what stands for the start of a segment plus
  // an offset
  Value *Pointer = Load->getPointerOperand();
  while (isa<IntToPtrInst>(Pointer) or isa<BitCastInst>(Pointer)
         or isa<TruncInst>(Pointer) or isa<ZExtInst>(Pointer))
    Pointer = cast<Instruction>(Pointer)->getOperand(0);
  if (auto *Expression = dyn_cast<ConstantExpr>(Pointer))
    if (Expression->getOpcode() == Instruction::IntToPtr)
      Pointer = Expression->getOperand(0);

  uint64_t Address = 0;
  if (auto *Operator = dyn_cast<BinaryOperator>(Pointer);
      Operator != nullptr and Operator->getOpcode() == Instruction::Add) {
    auto *Offset = dyn_cast<ConstantInt>(Operator->getOperand(1));
    if (Offset == nullptr or Offset->getBitWidth() > 64)
      return std::nullopt;
    Address = Offset->getZExtValue();
    Pointer = Operator->getOperand(0);
    while (isa<TruncInst>(Pointer) or isa<ZExtInst>(Pointer))
      Pointer = cast<Instruction>(Pointer)->getOperand(0);
  }

  if (auto *Constant = dyn_cast<ConstantInt>(Pointer)) {
    if (Constant->getBitWidth() > 64)
      return std::nullopt;
    Address += Constant->getZExtValue();
  } else if (auto *Call = dyn_cast<CallBase>(Pointer)) {
    Function *Getter = Call->getCalledFunction();
    if (Getter == nullptr or not FunctionTags::SegmentGlobalGetter.isTagOf(Getter))
      return std::nullopt;
    StringRef Name = Getter->getName();
    uint64_t Start = 0;
    if (not Name.consume_front("get_segment_0x")
        or Name.take_while(isHexDigit).getAsInteger(16, Start))
      return std::nullopt;
    Address += Start;
  } else {
    return std::nullopt;
  }

  unsigned Size = Load->getType()->isIntegerTy() ?
                    Load->getType()->getIntegerBitWidth() / 8 :
                    0;
  MetaAddress Where = MetaAddress::fromGeneric(Binary.Architecture(), Address);
  if (Size == 0 or Where.isInvalid())
    return std::nullopt;

  return ReadPlace{ Where, Size };
}

/// \return whether the program cannot write the memory at \p Place
inline bool isConstantMemory(const ReadPlace &Place,
                             const model::Binary &Binary) {
  for (const model::Segment &Segment : Binary.Segments())
    if (Segment.contains(Place.Address, Place.Size))
      return not Segment.IsWriteable();
  return false;
}

/// \return whether \p Target, the address a call goes to, is read from an
///         address that is known and lies in memory the program cannot write
inline bool isReadFromConstantMemory(llvm::Value *Target,
                                     const model::Binary &Binary) {
  std::optional<ReadPlace> Place = placeReadFrom(Target, Binary);
  return Place.has_value() and isConstantMemory(*Place, Binary);
}

/// \return the block a call goes to: the callee its function_call marker
///         names or, for a call through a pointer, the only address the
///         lifter found the program counter can have there, if the pointer is
///         read from memory that cannot change: an entry of a virtual table
///         that is known where the call is made. The call is then to what
///         the entry points to. (The lifter also finds what a pointer the
///         program can write starts with, such as the slot of a function
///         imported at its first use: that is where the call goes the first
///         time.)
inline llvm::BasicBlock *getOnlyCallee(llvm::Instruction *T,
                                       const model::Binary &Binary) {
  if (llvm::BasicBlock *Callee = getFunctionCallCallee(T))
    return Callee;

  if (not hasMarker(T, FunctionCallMarker))
    return nullptr;

  // The values that were found are checked one part of the program counter
  // at a time, and anything else is unexpected: follow the checks down to
  // the blocks they lead to
  llvm::BasicBlock *Only = nullptr;
  llvm::Value *Target = nullptr;
  llvm::SmallVector<llvm::Instruction *, 4> Queue{ T };
  unsigned Visited = 0;
  while (not Queue.empty()) {
    auto *Switch = llvm::dyn_cast<llvm::SwitchInst>(Queue.pop_back_val());
    if (Switch == nullptr or ++Visited > 16
        or getType(Switch->getDefaultDest()) != BlockType::UnexpectedPCBlock)
      return nullptr;

    for (const auto &Case : Switch->cases()) {
      llvm::BasicBlock *Successor = Case.getCaseSuccessor();
      BlockType::Values Type = getType(Successor);
      if (Type == BlockType::IndirectBranchDispatcherHelperBlock) {
        Queue.push_back(Successor->getTerminator());
      } else if (Type == BlockType::JumpTargetBlock) {
        if (Only != nullptr and Only != Successor)
          return nullptr;
        Only = Successor;
        Target = Switch->getCondition();
      } else {
        return nullptr;
      }
    }
  }

  if (Only == nullptr or not isReadFromConstantMemory(Target, Binary))
    return nullptr;

  return Only;
}

inline llvm::BasicBlock *getOnlyCallee(llvm::BasicBlock *BB,
                                       const model::Binary &Binary) {
  return getOnlyCallee(BB->getTerminator(), Binary);
}

/// An outlined function helper object.
struct OutlinedFunction {
public:
  MetaAddress Address;

  /// The marker for indirect jumps
  // NOTE: do not move this field, F holds a reference to it and therefore it
  //       needs to be destroyed first.
  UniqueValuePtr<llvm::Function> IndirectBranchInfoMarker;

  /// The actual LLVM outlined function
  UniqueValuePtr<llvm::Function> Function;

  // Special blocks
  llvm::BasicBlock *AnyPCCloned = nullptr;
  llvm::BasicBlock *UnexpectedPCCloned = nullptr;

  /// Where the function is resumed from after an exception is handled, if
  /// the model says it is resumed somewhere
  llvm::BasicBlock *ResumeCloned = nullptr;

  std::vector<MetaAddress> InlinedFunctionsByIndex = { MetaAddress::invalid() };

public:
  OutlinedFunction() = default;

  OutlinedFunction(const OutlinedFunction &Other) = delete;
  OutlinedFunction &operator=(const OutlinedFunction &) = delete;

  OutlinedFunction(OutlinedFunction &&Other) { *this = std::move(Other); }

  OutlinedFunction &operator=(OutlinedFunction &&Other) {
    if (this != &Other) {
      Address = Other.Address;
      Function = std::move(Other.Function);
      IndirectBranchInfoMarker = std::move(Other.IndirectBranchInfoMarker);
      AnyPCCloned = Other.AnyPCCloned;
      UnexpectedPCCloned = Other.UnexpectedPCCloned;
      ResumeCloned = Other.ResumeCloned;
      InlinedFunctionsByIndex = Other.InlinedFunctionsByIndex;

      Other.Address = MetaAddress::invalid();
      Other.Function = nullptr;
      Other.IndirectBranchInfoMarker.reset();
      Other.AnyPCCloned = nullptr;
      Other.UnexpectedPCCloned = nullptr;
      Other.ResumeCloned = nullptr;
    }
    return *this;
  }

public:
  llvm::Function *releaseFunction() {
    auto *ToReturn = Function.release();
    Function = nullptr;
    return ToReturn;
  }
};

/// This class, given an Oracle, can outline functions from root
class Outliner {
public:
  /// The part of the state of an Outliner that depends on the module alone.
  ///
  /// Preparing it takes a visit of `root` and one of the functions of the
  /// module, so it can be shared by the outliners of a module: outlining does
  /// not change the blocks of `root`, and the functions the pool holds are
  /// created through it.
  struct ModuleState {
    OpaqueFunctionsPool<llvm::Type *> OpaqueReturnAddress;
    llvm::CodeExtractorAnalysisCache CEAC;

    explicit ModuleState(llvm::Module &M) :
      OpaqueReturnAddress(&M, false), CEAC(*M.getFunction("root")) {

      using namespace llvm;
      auto Effects = MemoryEffects::inaccessibleMemOnly(llvm::ModRefInfo::Ref);
      OpaqueReturnAddress.setMemoryEffects(Effects);
      OpaqueReturnAddress.addFnAttribute(Attribute::NoUnwind);
      OpaqueReturnAddress.addFnAttribute(Attribute::WillReturn);
      OpaqueReturnAddress.setTags({ &FunctionTags::OpaqueReturnAddressFunction,
                                    &FunctionTags::UniquedByPrototype });
      OpaqueReturnAddress
        .initializeFromReturnType(FunctionTags::OpaqueReturnAddressFunction);
    }
  };

public:
  /// The callee of the calls through a pointer that has been found by other
  /// means than the code of the call, if whoever outlines has any: by the
  /// address of the block of the call, a function of the binary or the name
  /// of a dynamic function
  using ResolvedCallee = std::pair<MetaAddress, std::string>;
  const std::map<MetaAddress, ResolvedCallee> *ResolvedCallees = nullptr;

private:
  llvm::Module &M;
  RootFunction &RootF;
  const CSVGlobals &Globals;
  FunctionSummaryOracle &Oracle;

  /// UnexpectedPCMarker is used to indicate that `unexpectedpc` basic
  /// block of a function to inline need to be adjusted to jump to
  /// `unexpectedpc` of their caller.
  TemporaryOpaqueFunction UnexpectedPCMarker;

  ModuleState &State;

public:
  /// \param State must outlive this object
  Outliner(llvm::Module &M,
           RootFunction &RootF,
           const CSVGlobals &Globals,
           FunctionSummaryOracle &Oracle,
           ModuleState &State) :
    M(M),
    RootF(RootF),
    Globals(Globals),
    Oracle(Oracle),
    UnexpectedPCMarker(initializeUnexpectedPCMarker(M)),
    State(State) {}

public:
  OutlinedFunction outline(const MetaAddress &EntryAddress,
                           CallHandler *TheCallHandler);

  /// \return the instructions of \p F, an outlined function, that save the
  ///         stack pointer in the frame where the code that handles
  ///         exceptions restores it from, if they all save it in the same
  ///         place, and where that is from the frame pointer
  std::pair<llvm::SmallVector<llvm::StoreInst *, 2>, int64_t>
  stackPointerSaves(llvm::Function &F) const;

private:
  static TemporaryOpaqueFunction initializeUnexpectedPCMarker(llvm::Module &M) {
    return { llvm::FunctionType::get(llvm::Type::getVoidTy(M.getContext()),
                                     false),
             "unexpectedpc_hook",
             &M };
  }

private:
  OutlinedFunction
  outlineFunctionInternal(CallHandler *TheCallHandler,
                          const MetaAddress &EntryAddress,
                          OutlinedFunctionsMap &FunctionsToInline);

  /// \return a description of the call and boolean indicating whether the call
  ///         site is a tail call or not.
  std::pair<const FunctionSummary *, bool>
  getCallSiteInfo(MetaAddress CallerFunction,
                  llvm::BasicBlock *CallerBlock,
                  llvm::CallInst *FunctionCall,
                  llvm::CallInst *JumpToSymbol,
                  MetaAddress Callee);

  void integrateFunctionCallee(CallHandler *TheCallHandler,
                               MetaAddress Caller,
                               llvm::BasicBlock *CallerBlock,
                               llvm::CallInst *FunctionCall,
                               llvm::CallInst *JumpToSymbol,
                               MetaAddress Callee,
                               OutlinedFunctionsMap &FunctionsToInline);

  llvm::Function *
  createFunctionToInline(CallHandler *TheCallHandler,
                         const MetaAddress &Entry,
                         OutlinedFunctionsMap &FunctionsToInline);

  void createAnyPCHooks(CallHandler *TheCallHandler, OutlinedFunction *F);

  /// Make the code \p F is resumed from reachable: from where \p F saves its
  /// stack pointer in its frame, which is what whoever resumes it restores.
  /// If \p F does not, the code it would be resumed at is dropped.
  void connectResumePoint(OutlinedFunction *F);

  /// The functions that stand for an exception being caught by the function
  /// and for its being resumed
  llvm::Function *exceptionHelper(llvm::StringRef Name, bool IsResume);
};

} // namespace efa
