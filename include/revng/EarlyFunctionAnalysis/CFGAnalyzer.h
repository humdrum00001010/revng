#pragma once

//
// This file is distributed under the MIT License. See LICENSE.md for details.
//

#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

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
/// A call to something known: a function of the binary, or a dynamic one
/// with a prototype
struct KnownCall {
  /// What the block of the call pushes before it
  uint64_t Pushed = 0;

  /// The same, after the last register the function saves there
  uint64_t PushedAfterSaves = 0;

  /// Where the callee leaves the stack pointer
  int64_t FinalStackOffset = 0;

  /// What the code after the call removes from the stack before doing
  /// anything else with it
  uint64_t Removed = 0;
};

struct UnknownCall {
  /// The address of the block of the call
  MetaAddress Block;

  /// The address of the instruction that makes the call, if it is known
  MetaAddress Instruction;

  /// Where the callee leaves the stack pointer, as far as it is known or
  /// assumed
  std::optional<int64_t> FinalStackOffset;

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

  /// What the code after the call removes from the stack before doing
  /// anything else with it
  uint64_t Removed = 0;

  /// What the block pushes before the call, whoever removes it
  uint64_t AllPushed = 0;

  /// The same, without what the block pushes up to the last register it
  /// saves
  uint64_t AllPushedAfterSaves = 0;
};

class CallSummarizer : public CallHandler {
private:
  llvm::Module *M = nullptr;
  llvm::Function *PreCallHook = nullptr;
  llvm::Function *PostCallHook = nullptr;
  llvm::Function *RetHook = nullptr;
  llvm::Function *StackHeightHook = nullptr;
  llvm::Function *StackEffect = nullptr;
  llvm::Function *CallTargetHook = nullptr;
  llvm::GlobalVariable *SPCSV = nullptr;
  llvm::GlobalVariable *PCCSV = nullptr;
  llvm::SmallSet<MetaAddress, 4> *ReturnBlocks = nullptr;
  OpaqueRegisterUser Clobberer;

  /// The final stack offset of the callee of a call site, where one has been
  /// found, by the address of the block of the call
  const std::map<MetaAddress, int64_t> *CallSiteFSO = nullptr;

  /// The call sites whose callee, in this analysis, moves the stack pointer by
  /// an unknown amount, and which unknown
  const std::map<MetaAddress, unsigned> *SymbolicSites = nullptr;

  /// The calls to something else than a function of the binary met while
  /// outlining
  llvm::SmallVectorImpl<UnknownCall> *UnknownCalls = nullptr;

  /// The calls to something known, by the block of the call
  std::map<MetaAddress, KnownCall> *KnownCalls = nullptr;

  /// The calls to something known after which the caller is taken to have
  /// removed what it pushed, although no code does: by the block of the
  /// call, the size
  const std::map<MetaAddress, uint64_t> *ElidedCleanups = nullptr;

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
                 llvm::Function *StackEffect,
                 llvm::Function *CallTargetHook,
                 llvm::GlobalVariable *SPCSV,
                 llvm::GlobalVariable *PCCSV,
                 llvm::SmallSet<MetaAddress, 4> *ReturnBlocks,
                 const std::map<MetaAddress, int64_t> *CallSiteFSO,
                 const std::map<MetaAddress, unsigned> *SymbolicSites,
                 llvm::SmallVectorImpl<UnknownCall> *UnknownCalls,
                 std::map<MetaAddress, KnownCall> *KnownCalls,
                 const std::map<MetaAddress, uint64_t> *ElidedCleanups,
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

  /// StackHeightHook takes the stack pointer before each function call and at
  /// the start of each block, with the identifier of the block in the latter
  /// case. Once the value it had at the entry is subtracted from it, the height
  /// of the stack there is known where the result has become a constant.
  TemporaryOpaqueFunction StackHeightHook;

  /// What a callee nothing is known about moves the stack pointer by: an
  /// unknown, the same wherever the argument is the same
  TemporaryOpaqueFunction StackEffect;

  /// Marks what a call goes through (the value the program counter is given
  /// there), with what is known about who that is: read after the
  /// optimizations, to see which calls of a function go to the same place
  TemporaryOpaqueFunction CallTargetHook;

  Outliner::ModuleState OutlinerState;
  Outliner Outliner;

  OpaqueFunctionsPool<llvm::Type *> OpaqueBranchConditionsPool;
  std::unique_ptr<llvm::raw_ostream> OutputAAWriter;
  std::unique_ptr<llvm::raw_ostream> OutputIBI;

  /// The final stack offset found for the callee of a call site, by the
  /// address of the block of the call
  std::map<MetaAddress, int64_t> CallSiteFSO;

  /// The callee of the calls through a pointer that go where another call
  /// of the same function goes, one that the code says the callee of (a
  /// register loaded once with the address of a function and called several
  /// times: the lifter knows who is called the first time only). By the
  /// address of the block of the call: a function of the binary, or the name
  /// of a dynamic function.
  std::map<MetaAddress, std::pair<MetaAddress, std::string>> ResolvedCallees;

  /// Whether the last analyses found such callees
  bool HasNewCallees = false;

  /// The call sites whose callee moves the stack pointer by an unknown amount
  /// in the analysis that finds what the function requires it to be
  std::map<MetaAddress, unsigned> SymbolicSites;

  /// The calls of the function outlined last to something else than a function
  /// of the binary
  llvm::SmallVector<UnknownCall, 4> UnknownCalls;

  /// The calls of the function being analyzed to something known
  std::map<MetaAddress, KnownCall> KnownCalls;

  /// See CallSummarizer::ElidedCleanups
  std::map<MetaAddress, uint64_t> ElidedCleanups;

  /// The call sites where the function says what a group of callees removes
  /// from the stack together, but not how much each: what is assumed there is
  /// one of the possible answers. With each, the address of the instruction
  /// that makes the call.
  std::map<MetaAddress, MetaAddress> AmbiguousSites;

  /// The registers the callees of the function outlined last clobber
  CSVSet CalleeClobbered;

  /// The registers the default prototype preserves
  CSVSet CalleeSaved;

  /// The functions for which no assumption about their call sites helped
  std::set<MetaAddress> Unsolved;

  /// The calls of the function analyzed last at which the height of the stack
  /// is not known
  unsigned UnknownHeights = 0;

  /// Whether the function is being analyzed to see if something holds, see
  /// tryOut
  bool IsTrial = false;

  /// What the last analysis of unknown callees left to explain: the returns
  /// and tail calls, the longjmps, the registers clobbered for no known
  /// reason and the calls at an unknown height of the stack
  std::tuple<unsigned, unsigned, unsigned, unsigned> Leftover = { 0, 0, 0, 0 };
  bool HasLeftover = false;

  /// The blocks of the function being analyzed that end by taking where to go
  /// from the top of the stack and removing it: a return instruction
  std::set<BasicBlockID> PopsItsTarget;

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

  /// Analyze a function whose callees are all known, and see whether what is
  /// not explained in it is explained by a caller that left out the code
  /// that removes the arguments of a call.
  ///
  /// \param Found the calls where it is, by block, with the size of what the
  ///        caller is taken to have removed
  /// \return the function analyzed with that, if something was found
  std::optional<FunctionSummary>
  analyzeElidedCleanups(const MetaAddress &Entry,
                        std::map<MetaAddress, uint64_t> &Found);

  /// \return whether the function at \p Entry saves its stack pointer in its
  ///         frame where the code that handles exceptions restores it from:
  ///         only such a function can be resumed somewhere
  bool savesStackPointer(const MetaAddress &Entry) {
    OutlinedFunction Outlined = outline(Entry);
    if (Outlined.Function == nullptr)
      return false;
    return not Outliner.stackPointerSaves(*Outlined.Function).first.empty();
  }

  /// Analyze the function to see whether what the model has just been made
  /// to say about it holds. If it does not, the function can come out with a
  /// path that leads nowhere, which is an error anywhere else: here the block
  /// such a path ends with leaves the function for an unknown place, and the
  /// caller rejects the result.
  FunctionSummary tryOut(const MetaAddress &Entry) {
    IsTrial = true;
    FunctionSummary Result = analyze(Entry);
    IsTrial = false;
    return Result;
  }

  OutlinedFunction outline(const MetaAddress &Entry);

  /// \return the address of the instruction that makes the call of
  ///         \p CallerBlock, if what its callee removes is one of several
  ///         answers the function allows and the instruction is known
  MetaAddress ambiguousCall(const MetaAddress &CallerBlock) const {
    auto It = AmbiguousSites.find(CallerBlock);
    return It == AmbiguousSites.end() ? MetaAddress::invalid() : It->second;
  }

  /// \return the size of what the caller is taken to have removed after the
  ///         call of \p CallerBlock, to something known, without code that
  ///         does it; 0 if nothing
  uint64_t elidedCleanup(const MetaAddress &CallerBlock) const {
    auto It = ElidedCleanups.find(CallerBlock);
    return It == ElidedCleanups.end() ? 0 : It->second;
  }

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

  /// analyzeOnce, again as long as it finds out who a call through a pointer
  /// goes to
  FunctionSummary analyzeResolving(const MetaAddress &Entry);

  /// See ResolvedCallees
  void resolveCallees(llvm::Function *F);

  /// Find what the callees nothing is known about remove from the stack
  FunctionSummary analyzeCallees(const MetaAddress &Entry);


  /// The function, outlined and optimized, and its direct control flow
  std::pair<OutlinedFunction, SortedVector<efa::BasicBlock>>
  optimize(const MetaAddress &Entry);

  /// \return for each of the \p Unknowns of SymbolicSites, what the paths of
  ///         the function that join require it to be, if they require it
  /// \p Likeliest is what each unknown is taken to be if the function does
  /// not say; those that are only known together with others are changed to
  /// agree with what the function says, and \p IsAmbiguous is set for them
  /// \p Most is, for each unknown that has one, the most it can be: a callee
  /// does not remove what the function saved before calling it.
  std::vector<std::optional<int64_t>>
  solveCallEffects(const MetaAddress &Entry,
                   std::vector<int64_t> &Likeliest,
                   std::vector<bool> &IsAmbiguous,
                   std::vector<std::optional<int64_t>> &Most);

private:
  static llvm::FunctionType *createCallMarkerType(llvm::Module &M);
  static llvm::FunctionType *createRetMarkerType(llvm::Module &M);
  static llvm::FunctionType *createStackHeightMarkerType(const CSVGlobals &);
  static llvm::FunctionType *createStackEffectType(const CSVGlobals &);
  static llvm::FunctionType *createCallTargetMarkerType(const CSVGlobals &);

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
