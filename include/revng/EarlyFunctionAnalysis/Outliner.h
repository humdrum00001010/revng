#pragma once

//
// This file is distributed under the MIT License. See LICENSE.md for details.
//

#include <optional>

#include "llvm/IR/Module.h"
#include "llvm/Support/ModRef.h"
#include "llvm/Transforms/Utils/CodeExtractor.h"

#include "revng/BasicAnalyses/RootFunction.h"
#include "revng/EarlyFunctionAnalysis/FunctionSummaryOracle.h"
#include "revng/EarlyFunctionAnalysis/TemporaryOpaqueFunction.h"
#include "revng/Model/FunctionTags.h"
#include "revng/Model/NamedTypedRegister.h"
#include "revng/Support/OpaqueFunctionsPool.h"
#include "revng/Support/UniqueValuePtr.h"

namespace efa {

class CallHandler;
class OutlinedFunctionsMap;

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
      InlinedFunctionsByIndex = Other.InlinedFunctionsByIndex;

      Other.Address = MetaAddress::invalid();
      Other.Function = nullptr;
      Other.IndirectBranchInfoMarker.reset();
      Other.AnyPCCloned = nullptr;
      Other.UnexpectedPCCloned = nullptr;
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
private:
  llvm::Module &M;
  RootFunction &RootF;
  const CSVGlobals &Globals;
  FunctionSummaryOracle &Oracle;

  /// UnexpectedPCMarker is used to indicate that `unexpectedpc` basic
  /// block of a function to inline need to be adjusted to jump to
  /// `unexpectedpc` of their caller.
  TemporaryOpaqueFunction UnexpectedPCMarker;

  /// The functions that produce opaque return addresses, by type. Collecting
  /// them takes a visit of all the functions of the module: who creates many
  /// outliners for the same module can prepare a pool once (see
  /// prepareOpaqueReturnAddressPool) and hand it to each of them. Only an
  /// outliner creates such functions, and it does so through the pool.
  OpaqueFunctionsPool<llvm::Type *> OwnOpaqueReturnAddress;
  OpaqueFunctionsPool<llvm::Type *> &OpaqueReturnAddress;

  /// The analysis of `root` the code extractor needs. Computing it visits the
  /// whole of `root`: who creates many outliners for the same module can
  /// compute it once and hand it to each of them.
  std::optional<llvm::CodeExtractorAnalysisCache> OwnCEAC;
  const llvm::CodeExtractorAnalysisCache &CEAC;

public:
  /// \param SharedCEAC if not null, an analysis of the `root` function of
  ///        \p M that outlives this object and is used instead of computing
  ///        a new one
  Outliner(llvm::Module &M,
           RootFunction &RootF,
           const CSVGlobals &Globals,
           FunctionSummaryOracle &Oracle,
           const llvm::CodeExtractorAnalysisCache *SharedCEAC = nullptr,
           OpaqueFunctionsPool<llvm::Type *> *SharedReturnAddressPool =
             nullptr) :
    M(M),
    RootF(RootF),
    Globals(Globals),
    Oracle(Oracle),
    UnexpectedPCMarker(initializeUnexpectedPCMarker(M)),
    OwnOpaqueReturnAddress(&M, false),
    OpaqueReturnAddress(SharedReturnAddressPool != nullptr ?
                          *SharedReturnAddressPool :
                          OwnOpaqueReturnAddress),
    OwnCEAC(makeOwnCEAC(M, SharedCEAC)),
    CEAC(SharedCEAC != nullptr ? *SharedCEAC : *OwnCEAC) {

    if (SharedReturnAddressPool == nullptr)
      prepareOpaqueReturnAddressPool(OwnOpaqueReturnAddress);
  }

  /// Configure \p Pool, a pool for the module the outliners will work on, and
  /// fill it with the functions that are already there
  static void
  prepareOpaqueReturnAddressPool(OpaqueFunctionsPool<llvm::Type *> &Pool) {
    using namespace llvm;
    auto Effects = MemoryEffects::inaccessibleMemOnly(llvm::ModRefInfo::Ref);
    Pool.setMemoryEffects(Effects);
    Pool.addFnAttribute(Attribute::NoUnwind);
    Pool.addFnAttribute(Attribute::WillReturn);
    Pool.setTags({ &FunctionTags::OpaqueReturnAddressFunction,
                   &FunctionTags::UniquedByPrototype });
    Pool.initializeFromReturnType(FunctionTags::OpaqueReturnAddressFunction);
  }

public:
  OutlinedFunction outline(const MetaAddress &EntryAddress,
                           CallHandler *TheCallHandler);

private:
  static std::optional<llvm::CodeExtractorAnalysisCache>
  makeOwnCEAC(llvm::Module &M,
              const llvm::CodeExtractorAnalysisCache *SharedCEAC) {
    std::optional<llvm::CodeExtractorAnalysisCache> Result;
    if (SharedCEAC == nullptr)
      Result.emplace(*M.getFunction("root"));
    return Result;
  }

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
};

} // namespace efa
