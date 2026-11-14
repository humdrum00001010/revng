#pragma once

//
// This file is distributed under the MIT License. See LICENSE.md for details.
//

#include <map>
#include <unordered_map>

#include "llvm/ADT/SmallSet.h"

#include "revng/ADT/ConstantRangeSet.h"
#include "revng/MFP/Graph.h"
#include "revng/MFP/MFP.h"
#include "revng/ValueMaterializer/ControlFlowEdgesGraph.h"
#include "revng/ValueMaterializer/DataFlowRangeAnalysis.h"

namespace llvm {
class Instruction;
class LazyValueInfo;
class DominatorTree;
} // namespace llvm

class DataFlowGraph;

/// For a tracked instruction, the instructions whose constraints can be used
/// in its place: the instruction itself, its truncations and the zero
/// extensions of those. They depend only on the instruction, so they are
/// collected once and reused across applications of the transfer function and,
/// if the cache outlives the query, across queries on the same (unmodified)
/// function.
class AVICandidatesCache {
public:
  using Set = llvm::SmallSet<llvm::Instruction *, 4>;

private:
  std::unordered_map<const llvm::Instruction *, Set> Map;

public:
  static Set compute(llvm::Instruction *I);

  const Set &get(llvm::Instruction *I) {
    auto It = Map.find(I);
    if (It == Map.end())
      It = Map.emplace(I, compute(I)).first;
    return It->second;
  }
};

class AdvancedValueInfoMFI {
public:
  using LatticeElement = std::map<llvm::Instruction *, ConstantRangeSet>;
  using GraphType = const ControlFlowEdgesGraph *;
  using Label = const ControlFlowEdgesGraph::Node *;
  using ResultsMap = std::map<Label, mfp::MFPResult<LatticeElement>>;
  using InstructionsSet = llvm::SmallPtrSetImpl<llvm::Instruction *>;
  using ExtraStateType = mfp::NoExtraState;

private:
  llvm::LazyValueInfo &LVI;
  DataFlowRangeAnalysis &DFRA;
  const llvm::DominatorTree &DT;
  llvm::Instruction *Context;
  llvm::SmallPtrSetImpl<llvm::Instruction *> &Instructions;
  bool ZeroExtendConstraints = false;
  AVICandidatesCache &CandidatesCache;

  /// Self-check mode only: instructions whose cached candidates have already
  /// been compared, in this query, with a fresh walk of their users
  mutable llvm::SmallPtrSet<llvm::Instruction *, 8> VerifiedCandidates;

public:
  AdvancedValueInfoMFI(llvm::LazyValueInfo &LVI,
                       DataFlowRangeAnalysis &DFRA,
                       const llvm::DominatorTree &DT,
                       llvm::Instruction *Context,
                       InstructionsSet &Instructions,
                       bool ZeroExtendConstraints,
                       AVICandidatesCache &CandidatesCache) :
    LVI(LVI),
    DFRA(DFRA),
    DT(DT),
    Context(Context),
    Instructions(Instructions),
    ZeroExtendConstraints(ZeroExtendConstraints),
    CandidatesCache(CandidatesCache) {}

public:
  LatticeElement combineValues(const LatticeElement &LHS,
                               const LatticeElement &RHS) const;

  bool isLessOrEqual(const LatticeElement &LHS,
                     const LatticeElement &RHS) const;

  LatticeElement applyTransferFunction(Label L,
                                       const LatticeElement &E,
                                       mfp::NoExtraState &) const;

public:
  static void dump(GraphType CFEG, const ResultsMap &AllResults);
};

static_assert(mfp::MonotoneFrameworkInstance<AdvancedValueInfoMFI>);

/// \p DFG the data flow graph containing the instructions we're interested in.
/// \p Context the position in the function for the current query.
std::tuple<
  std::map<llvm::Instruction *, ConstantRangeSet>,
  ControlFlowEdgesGraph,
  std::map<const ForwardNode<ControlFlowEdgesNode> *,
           mfp::MFPResult<std::map<llvm::Instruction *, ConstantRangeSet>>>>
runAVI(const DataFlowGraph &DFG,
       llvm::Instruction *Context,
       const llvm::DominatorTree &DT,
       llvm::LazyValueInfo &LVI,
       DataFlowRangeAnalysis &DFRA,
       bool ZeroExtendConstraints,
       AVICandidatesCache *CandidatesCache = nullptr);

template<>
void mfp::dump(llvm::raw_ostream &Stream,
               unsigned Indent,
               const std::map<llvm::Instruction *, ConstantRangeSet> &Element);

template<>
void mfp::dumpLabel(llvm::raw_ostream &Stream,
                    const ControlFlowEdgesGraph::Node *const &Label);
