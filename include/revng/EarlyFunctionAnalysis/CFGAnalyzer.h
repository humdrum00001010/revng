#pragma once

//
// This file is distributed under the MIT License. See LICENSE.md for details.
//

#include <map>
#include <memory>
#include <optional>

#include "llvm/ADT/SmallVector.h"

#include "revng/EarlyFunctionAnalysis/CallHandler.h"
#include "revng/EarlyFunctionAnalysis/Outliner.h"
#include "revng/EarlyFunctionAnalysis/TemporaryOpaqueFunction.h"
#include "revng/Model/Binary.h"
#include "revng/Model/ProgramCounterHandler.h"
#include "revng/Support/IRBuilder.h"
#include "revng/Support/OpaqueFunctionsPool.h"
#include "revng/Support/OpaqueRegisterUser.h"

class FunctionSummaryOracle;

namespace llvm {
class Module;
class GlobalVariable;
class Function;
} // namespace llvm

namespace efa {

class CallSummarizer : public CallHandler {
private:
  llvm::Module *M = nullptr;
  llvm::Function *PreCallHook = nullptr;
  llvm::Function *PostCallHook = nullptr;
  llvm::Function *RetHook = nullptr;
  llvm::GlobalVariable *SPCSV = nullptr;
  llvm::SmallSet<MetaAddress, 4> *ReturnBlocks = nullptr;
  OpaqueRegisterUser Clobberer;

  /// The final stack offset of the callee of an indirect call, where one has
  /// been found, by the address of the block of the call
  const std::map<MetaAddress, int64_t> *IndirectCallFSO = nullptr;

  /// The blocks of the indirect calls met while outlining
  llvm::SmallVectorImpl<MetaAddress> *IndirectCalls = nullptr;

public:
  CallSummarizer(llvm::Module *M,
                 llvm::Function *PreCallHook,
                 llvm::Function *PostCallHook,
                 llvm::Function *RetHook,
                 llvm::GlobalVariable *SPCSV,
                 llvm::SmallSet<MetaAddress, 4> *ReturnBlocks,
                 const std::map<MetaAddress, int64_t> *IndirectCallFSO,
                 llvm::SmallVectorImpl<MetaAddress> *IndirectCalls);

public:
  void handleCall(MetaAddress CallerBlock,
                  revng::IRBuilder &Builder,
                  MetaAddress Callee,
                  const CSVSet &ClobberedRegisters,
                  const std::optional<int64_t> &MaybeFSO,
                  bool IsNoReturn,
                  bool IsTailCall,
                  llvm::Value *SymbolNamePointer) final;

  void handlePostNoReturn(revng::IRBuilder &Builder,
                          const llvm::DebugLoc &DbgLocation) final;

  void handleIndirectJump(revng::IRBuilder &Builder,
                          MetaAddress Block,
                          const CSVSet &ClobberedRegisters,
                          llvm::Value *SymbolNamePointer) final;
};

/// This class, given an Oracle, analyzes a function returning its CFG, the set
/// of callee saved registers, whether it's noreturn or not and its final stack
/// offset
class CFGAnalyzer {
private:
  llvm::Module &M;
  const CSVGlobals &Globals;
  std::unique_ptr<ProgramCounterHandler> PCH;
  FunctionSummaryOracle &Oracle;
  const TupleTree<model::Binary> &Binary;

  llvm::SmallVector<llvm::GlobalVariable *, 16> ABICSVs;

  /// PreCallHook and PostCallHook mark the presence of an original function
  /// call, and surround a basic block containing the registers clobbered by the
  /// function called. They take the MetaAddress of the callee and the
  /// call-site.
  TemporaryOpaqueFunction PreCallHook;
  TemporaryOpaqueFunction PostCallHook;
  TemporaryOpaqueFunction RetHook;

  Outliner::ModuleState OutlinerState;
  Outliner Outliner;

  OpaqueFunctionsPool<llvm::Type *> OpaqueBranchConditionsPool;
  std::unique_ptr<llvm::raw_ostream> OutputAAWriter;
  std::unique_ptr<llvm::raw_ostream> OutputIBI;

  /// The final stack offset found for the callee of an indirect call, by the
  /// address of the block of the call
  std::map<MetaAddress, int64_t> IndirectCallFSO;

  /// The blocks of the indirect calls of the function outlined last
  llvm::SmallVector<MetaAddress, 4> IndirectCalls;

public:
  CFGAnalyzer(llvm::Module &M,
              RootFunction &Root,
              const CSVGlobals &Globals,
              const TupleTree<model::Binary> &Binary,
              FunctionSummaryOracle &Oracle);

public:
  llvm::Function *preCallHook() const { return PreCallHook.get(); }
  llvm::Function *postCallHook() const { return PostCallHook.get(); }
  llvm::Function *retHook() const { return RetHook.get(); }
  const auto &abiCSVs() const { return ABICSVs; }

public:
  FunctionSummary analyze(const MetaAddress &Entry);

  OutlinedFunction outline(const MetaAddress &Entry);

  /// \return the final stack offset found for the callee of the indirect call
  ///         in the block at \p CallerBlock, if it is not the default one
  std::optional<int64_t>
  indirectCallFinalStackOffset(const MetaAddress &CallerBlock) const {
    auto It = IndirectCallFSO.find(CallerBlock);
    if (It == IndirectCallFSO.end())
      return std::nullopt;
    return It->second;
  }

private:
  FunctionSummary analyzeOnce(const MetaAddress &Entry);

private:
  static llvm::FunctionType *createCallMarkerType(llvm::Module &M);
  static llvm::FunctionType *createRetMarkerType(llvm::Module &M);

private:
  std::optional<UpcastablePointer<efa::FunctionEdgeBase>>
  handleCall(llvm::CallInst *PreCallHookCall);

  SortedVector<efa::BasicBlock> collectDirectCFG(OutlinedFunction *F);

  void createIBIMarker(OutlinedFunction *F);

  void opaqueBranchConditions(llvm::Function *F, revng::IRBuilder &);

  void materializePCValues(llvm::Function *F, revng::IRBuilder &);

  void runOptimizationPipeline(llvm::Function *F);

  FunctionSummary milkInfo(OutlinedFunction *F,
                           SortedVector<efa::BasicBlock> &&CFG);

  struct State {
    llvm::Value *StackPointer;
    llvm::Value *ReturnPC;
    llvm::SmallVector<llvm::Value *, 16> CSVs;
  };
  State loadState(revng::IRBuilder &Builder) const;
};

} // namespace efa
