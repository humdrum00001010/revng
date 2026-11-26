#pragma once

//
// This file is distributed under the MIT License. See LICENSE.md for details.
//

#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>

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

/// A call whose callee is not known to the analysis of the caller: something
/// else than a function of the binary, or the caller itself. What it removes
/// from the stack is assumed
struct UnknownCall {
  /// The address of the block of the call
  MetaAddress Block;

  /// Whether the function calls itself here
  bool IsRecursive = false;

  /// The name of the dynamic function called, empty for an indirect call
  std::string Symbol;

  /// The bytes the block pushes before the call and does not pop: the callee
  /// can remove them, but not more
  uint64_t Pushed = 0;

  /// The same, without what the block pushes up to the last register it
  /// saves: what holds if the block is the first of its function
  uint64_t PushedAfterSaves = 0;
};

class CallSummarizer : public CallHandler {
private:
  llvm::Module *M = nullptr;
  llvm::Function *PreCallHook = nullptr;
  llvm::Function *PostCallHook = nullptr;
  llvm::Function *RetHook = nullptr;
  llvm::Function *StackHeightHook = nullptr;
  llvm::GlobalVariable *SPCSV = nullptr;
  llvm::SmallSet<MetaAddress, 4> *ReturnBlocks = nullptr;
  OpaqueRegisterUser Clobberer;

  /// The final stack offset of the callee of a call site, where one has been
  /// found, by the address of the block of the call
  const std::map<MetaAddress, int64_t> *CallSiteFSO = nullptr;

  /// The calls to something else than a function of the binary met while
  /// outlining
  llvm::SmallVectorImpl<UnknownCall> *UnknownCalls = nullptr;

  /// The registers the callees met while outlining clobber
  CSVSet *CalleeClobbered = nullptr;

  /// The registers a function saves before using them
  const CSVSet *CalleeSaved = nullptr;

  /// The function being outlined
  MetaAddress Caller;

public:
  CallSummarizer(llvm::Module *M,
                 llvm::Function *PreCallHook,
                 llvm::Function *PostCallHook,
                 llvm::Function *RetHook,
                 llvm::Function *StackHeightHook,
                 llvm::GlobalVariable *SPCSV,
                 llvm::SmallSet<MetaAddress, 4> *ReturnBlocks,
                 const std::map<MetaAddress, int64_t> *CallSiteFSO,
                 llvm::SmallVectorImpl<UnknownCall> *UnknownCalls,
                 CSVSet *CalleeClobbered,
                 const CSVSet *CalleeSaved,
                 MetaAddress Caller);

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

  /// StackHeightHook takes the stack pointer before each function call. Once
  /// the value it had at the entry is subtracted from it, the height of the
  /// stack at the call is known where the result has become a constant.
  TemporaryOpaqueFunction StackHeightHook;

  Outliner::ModuleState OutlinerState;
  Outliner Outliner;

  OpaqueFunctionsPool<llvm::Type *> OpaqueBranchConditionsPool;
  std::unique_ptr<llvm::raw_ostream> OutputAAWriter;
  std::unique_ptr<llvm::raw_ostream> OutputIBI;

  /// The final stack offset found for the callee of a call site, by the
  /// address of the block of the call
  std::map<MetaAddress, int64_t> CallSiteFSO;

  /// The calls of the function outlined last to something else than a function
  /// of the binary
  llvm::SmallVector<UnknownCall, 4> UnknownCalls;

  /// The registers the callees of the function outlined last clobber
  CSVSet CalleeClobbered;

  /// The registers the default prototype preserves
  CSVSet CalleeSaved;

  /// The functions for which no assumption about their call sites helped
  std::set<MetaAddress> Unsolved;

  /// The calls of the function analyzed last at which the height of the stack
  /// is not known
  unsigned UnknownHeights = 0;

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

  /// \return the final stack offset found for the callee of the call in the
  ///         block at \p CallerBlock, if one has been
  std::optional<int64_t>
  callSiteFinalStackOffset(const MetaAddress &CallerBlock) const {
    auto It = CallSiteFSO.find(CallerBlock);
    if (It == CallSiteFSO.end())
      return std::nullopt;
    return It->second;
  }

private:
  FunctionSummary analyzeOnce(const MetaAddress &Entry);

private:
  static llvm::FunctionType *createCallMarkerType(llvm::Module &M);
  static llvm::FunctionType *createRetMarkerType(llvm::Module &M);
  static llvm::FunctionType *createStackHeightMarkerType(const CSVGlobals &);

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
