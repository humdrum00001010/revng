//
// This file is distributed under the MIT License. See LICENSE.md for details.
//

#include <algorithm>
#include <iterator>
#include <unordered_map>
#include <utility>

#include "llvm/ADT/BitVector.h"
#include "llvm/ADT/DepthFirstIterator.h"
#include "llvm/ADT/GraphTraits.h"
#include "llvm/ADT/PostOrderIterator.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SetVector.h"
#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/iterator_range.h"

#include "revng/ADT/GenericGraph.h"
#include "revng/DataLayoutAnalysis/DLATypeSystem.h"
#include "revng/Support/Debug.h"

#include "DLAStep.h"
#include "FieldSizeComputation.h"

static Logger Log{ "sort-accesses-hierarchically" };

namespace dla {

// Returns true if N has an incoming instance strided edge
static bool hasStridedParent(const LayoutTypeSystemNode *N) {
  using InstanceGraph = EdgeFilteredGraph<const dla::LayoutTypeSystemNode *,
                                          dla::isNotPointerEdge>;
  using InverseInstance = llvm::Inverse<InstanceGraph>;
  for (const auto &Edge : llvm::children_edges<InverseInstance>(N))
    if (isInstanceEdge(Edge)
        and not Edge.second->getOffsetExpr().Strides.empty())
      return true;
  return false;
}

// Returns true if N has an incoming instance edge from a node different from
// Parent
static bool hasAnotherParent(const LayoutTypeSystemNode *N,
                             const LayoutTypeSystemNode *Parent) {
  using InstanceGraph = EdgeFilteredGraph<const dla::LayoutTypeSystemNode *,
                                          dla::isNotPointerEdge>;
  using InverseInstance = llvm::Inverse<InstanceGraph>;
  for (const auto *P : llvm::children<InverseInstance>(N))
    if (P != Parent)
      return true;

  return false;
}

// Returns true if N has a pointer successor (predecessor if OnInverse is true)
template<bool OnInverse>
static bool hasPointerSuccessor(const LayoutTypeSystemNode *N) {
  using ConstPointerGraph = EdgeFilteredGraph<const dla::LayoutTypeSystemNode *,
                                              dla::isPointerEdge>;
  using Directed = std::conditional_t<OnInverse,
                                      llvm::Inverse<ConstPointerGraph>,
                                      ConstPointerGraph>;

  for ([[maybe_unused]] const auto *_ : llvm::children<Directed>(N))
    return true;

  return false;
}

// Returns true if N has an incoming pointer edge
static bool hasPointerParent(const LayoutTypeSystemNode *N) {
  return hasPointerSuccessor<true>(N);
}

// Returns true if N has an outgoing pointer edge
static bool hasPointerChild(const LayoutTypeSystemNode *N) {
  return hasPointerSuccessor<false>(N);
}

// Returns true if Child is an instance child of Parent that should not be
// destroyed by this dla::Step.
static bool isFixedChild(const LayoutTypeSystemNode *Parent,
                         const LayoutTypeSystemNode *Child) {

  // If is'a a leaf node it's fixed because it represents an access.
  // If Child has an outgoing or incoming pointer edge, then it's fixed.
  // If there's a strided edge going to Child, then Child is fixed.
  // If Child has another Parent that is different from Parent, then it's
  // fixed
  static const bool OnePass = pipefix::enabled(pipefix::FixedChildInOnePass);
  if (OnePass) {
    if (Child->NonScalar or isLeaf(Child) or hasPointerChild(Child))
      return true;

    // hasPointerParent, hasStridedParent and hasAnotherParent, on each
    // incoming link in turn instead of each on all of them
    for (const auto &[Predecessor, Tag] : Child->Predecessors) {
      if (Tag->getKind() == TypeLinkTag::LK_Pointer)
        return true;
      if (Predecessor != Parent)
        return true;
      if (Tag->getKind() == TypeLinkTag::LK_Instance
          and not Tag->getOffsetExpr().Strides.empty())
        return true;
    }
    return false;
  }

  if (Child->NonScalar or isLeaf(Child) or hasPointerChild(Child)
      or hasPointerParent(Child) or hasStridedParent(Child)
      or hasAnotherParent(Child, Parent)) {
    return true;
  }

  // TODO: what if the Child has many incoming edges, and one comes
  // from Parent and the others all come from stuff that's between
  // Parent and Child? (e.g. P -> C1; P -> C2; C1 -> C2;) In the
  // current implementation C2 is considered fixed, because it has
  // another parent different from P.

  // TODO: what if the Child has both a in incoming strided edge and
  // a non-strided edge that come from Parent???
  //(e.g. P -offset-> C; P -strided-> C;)
  // In the current implementation C is considered fixed, because it
  // has an incoming strided edge.

  return false;
}

using NonPointerFilterT = EdgeFilteredGraph<dla::LayoutTypeSystemNode *,
                                            dla::isNotPointerEdge>;

static llvm::SetVector<LayoutTypeSystemNode *>
getVolatileChildren(LayoutTypeSystemNode *Parent) {
  llvm::SetVector<LayoutTypeSystemNode *> Result;
  for (auto *Child : llvm::children<NonPointerFilterT>(Parent))
    if (not isFixedChild(Parent, Child))
      Result.insert(Child);
  return Result;
}

using NeighborIterator = LayoutTypeSystem::NeighborIterator;

// This struct represents if an edge in LayoutTypeSystem can be pushed down
// another edge in LayoutTypeSystem, along with the resulting OffsetExpression
// if the push down takes place.
struct PushThroughComparisonResult {
  NeighborIterator ToPush;
  NeighborIterator Through;
  OffsetExpression OEAfterPush;
};

static OffsetExpression
computeOffsetAfterPush(NeighborIterator ToBePushed,
                       NeighborIterator ToBePushedThrough) {
  OffsetExpression Final;

  revng_assert(isInstanceEdge(*ToBePushed));
  revng_assert(isInstanceEdge(*ToBePushedThrough));

  const auto &ToPushOE = ToBePushed->second->getOffsetExpr();
  const auto &ThroughOE = ToBePushedThrough->second->getOffsetExpr();

  revng_assert(ToPushOE.Strides.empty() or ToPushOE.Strides.size() == 1);
  revng_assert(ThroughOE.Strides.empty() or ThroughOE.Strides.size() == 1);

  // The edge to push through always has the larger offset. Just subtract the
  // offset of the other edge.
  revng_assert(ToPushOE.Offset >= ThroughOE.Offset);
  Final.Offset = ToPushOE.Offset - ThroughOE.Offset;

  // If a strided edge is being pushed down another edge it means that the
  // whole array represented by the edge to push is contained inside the
  // element represented by the edge we're pushing it through (or a single
  // element of the array represented by the edge we're pushing it through, if
  // the edge we're pushing through is strided).
  // If the edge being pushed down is not strided, strides and trip counts are
  // empty.
  // So in all cases we can preserve Strides and TripCounts.
  Final.Strides = ToPushOE.Strides;
  Final.TripCounts = ToPushOE.TripCounts;

  // Pushing through a strided edge, we need to compute the reminder of the
  // offset inside the element of the array represented by the edge we're
  // pushing through.
  if (not ThroughOE.Strides.empty()) {
    revng_assert(ThroughOE.Strides.size() == 1);
    Final.Offset %= ThroughOE.Strides.front();
  }

  uint64_t ThroughElemSize = ToBePushedThrough->first->Size;
  uint64_t PushedFieldSize = getFieldSize(ToBePushed->first,
                                          ToBePushed->second);
  revng_assert(ThroughElemSize >= PushedFieldSize + Final.Offset);

  return Final;
}

// Returns true if adding an instance edge from Source to Target would close an
// instance loop.
static bool wouldCloseInstanceLoop(const LayoutTypeSystemNode *Parent,
                                   const LayoutTypeSystemNode *Source,
                                   const LayoutTypeSystemNode *Target) {
  // Self loops are never added in the first place.
  if (Source == Target)
    return false;

  // This function is expected to be called only in situations where Parent is
  // has instance edges going to both Source and Target.
  // For this reason, we do the reachability test on the Inverse. In this way we
  // we can use the Parent as a guard to keep the search quick and short.
  using InstanceGraph = EdgeFilteredGraph<const dla::LayoutTypeSystemNode *,
                                          dla::isInstanceEdge>;
  using InverseInstance = llvm::Inverse<InstanceGraph>;
  using LTSN = LayoutTypeSystemNode;
  using DFVisitSet = llvm::df_iterator_default_set<const LTSN *, 8>;

  DFVisitSet Visited;
  Visited.insert(Parent);
  for (const auto *Node :
       llvm::depth_first_ext(InverseInstance(Source), Visited))
    if (Node == Target)
      return true;

  return false;
}

// Compare the edges *AIt and *BIt to check if any of them can be pushed through
// the other. If so return proper info.
static std::optional<PushThroughComparisonResult>
canPushThrough(const LayoutTypeSystemNode *Parent,
               const NeighborIterator &AIt,
               const NeighborIterator &BIt) {
  revng_log(Log, "canPushThrough");
  LoggerIndent Indent{ Log };

  // An edge can never be pushed through itself
  if (AIt == BIt) {
    revng_log(Log, "nullopt: same");
    return std::nullopt;
  }

  // If any edge is not an instance edge, the edges are not comparable.
  if (not isInstanceEdge(*AIt) or not isInstanceEdge(*BIt)) {
    revng_log(Log, "nullopt: not instance");
    return std::nullopt;
  }

  // Here both edges are instance edges.

  const auto &[AChild, ATag] = *AIt;
  const auto &[BChild, BTag] = *BIt;

  if (isLeaf(AChild) and isLeaf(BChild)) {
    revng_log(Log, "nullopt: child");
    return std::nullopt;
  }

  const auto &[AOffset, AStrides, ATripCounts] = ATag->getOffsetExpr();
  const auto &[BOffset, BStrides, BTripCounts] = BTag->getOffsetExpr();

  revng_assert(AStrides.size() == ATripCounts.size());
  revng_assert(BStrides.size() == BTripCounts.size());
  revng_assert(AStrides.empty() or AStrides.size() == 1);
  revng_assert(BStrides.empty() or BStrides.size() == 1);

  uint64_t AFieldSize = getFieldSize(AChild, ATag);
  uint64_t BFieldSize = getFieldSize(BChild, BTag);

  uint64_t AFieldEnd = AOffset + AFieldSize;
  uint64_t BFieldEnd = BOffset + BFieldSize;

  // If A and B occupy disjoint ranges of memory, none of them can be pushed
  // through the other, so they are not comparable.
  if (AFieldEnd <= BOffset) {
    revng_log(Log, "nullopt: A ends before B starts");
    return std::nullopt;
  }

  if (BFieldEnd <= AOffset) {
    revng_log(Log, "nullopt: B ends before A starts");
    return std::nullopt;
  }

  // Here we have the guarantee that A and B occupy partly overlapping ranges.
  // They are not necessarily comparable yet, because they could still be partly
  // overlapping and not included.
  // Detect the partly overlapping case and return unordered for them
  if (AOffset < BOffset and AFieldEnd < BFieldEnd) {
    revng_log(Log, "nullopt: partly overlapping, A starts first");
    return std::nullopt;
  }
  if (AOffset > BOffset and AFieldEnd > BFieldEnd) {
    revng_log(Log, "nullopt: partly overlapping, B starts first");
    return std::nullopt;
  }

  // The two fields start at the same point and have the same size. None of them
  // strictly includes the other, so they need to be structurally inspected to
  // be merged. There's a separate pass doing this later in DLA, so we're not
  // taking care of it now.
  if (AOffset == BOffset and AFieldEnd == BFieldEnd) {
    revng_log(Log, "nullopt: equal ranges, needs structural inspection");
    return std::nullopt;
  }

  // Here we have the guarantee that one of the following holds:
  // - A is included in or occupies the same range as B
  // - B is included in or occupies the same range as A
  // Detect what is the case.
  bool AIsOuter = AFieldSize > BFieldSize;

  auto OuterIt = AIsOuter ? AIt : BIt;
  const auto &[Outer, OuterTag] = *OuterIt;

  // If the larger node is a leaf we cannot push the other down, so we bail out.
  if (isLeaf(Outer)) {
    revng_log(Log, "nullopt: outer is a leaf");
    return std::nullopt;
  }

  auto InnerIt = AIsOuter ? BIt : AIt;
  const auto &[Inner, InnerTag] = *InnerIt;

  const auto &InnerOffset = InnerTag->getOffsetExpr().Offset;
  const auto &[OuterOffset, OuterStrides, _] = OuterTag->getOffsetExpr();

  auto InnerFieldSize = AIsOuter ? BFieldSize : AFieldSize;
  auto OuterElemSize = OuterStrides.empty() ? Outer->Size :
                                              OuterStrides.front();

  // The inner fields starts at an higher offset (or equal) than the outer
  revng_assert(InnerOffset >= OuterOffset);
  auto OffsetAfterPush = InnerOffset - OuterOffset;

  auto OffsetInElem = OffsetAfterPush % OuterElemSize;
  auto EndByteInElem = OffsetInElem + InnerFieldSize;

  // When the inner field starts at the beginning of the outer and it has the
  // same size, it means that it spans the same exact bytes.
  // In principle there is nothing wrong with pushing inner through outer, but
  // that could close a instance loop, like in the following example if inner is
  // A->B and outer is A->C:
  //   A --> B --> C
  //    `----------^
  // So we detect this case and bail out.
  if (OffsetInElem == 0 and InnerFieldSize == Outer->Size
      and wouldCloseInstanceLoop(Parent, Outer, Inner)) {
    revng_log(Log,
              "nullopt: inner field covers a whole element of the "
              "outer, and pushing through would close an "
              "instance loop");
    return std::nullopt;
  }

  if (EndByteInElem > Outer->Size) {
    revng_log(Log,
              "nullopt: EndByteInElem: " << EndByteInElem << " > "
                                         << " Outer->Size: " << Outer->Size);
    return std::nullopt;
  }

  return PushThroughComparisonResult{
    .ToPush = InnerIt,
    .Through = OuterIt,
    .OEAfterPush = computeOffsetAfterPush(InnerIt, OuterIt)
  };
}

// Helper ordering for NeighborIterators. We need it here because we need to use
// such iterators and keys in associative containers.
// Notice that this might have undefined behaviour if dereferencing either LHS
// or RHS is undefined behaviour itself.
// The bottom line is that we should never insert invalid iterators into
// associative containers.
static std::weak_ordering operator<=>(const NeighborIterator &LHS,
                                      const NeighborIterator &RHS) {
  const auto &[LHSSucc, LHSTag] = *LHS;
  const auto &[RHSSucc, RHSTag] = *RHS;
  if (auto Cmp = LHSSucc->ID <=> RHSSucc->ID; Cmp != 0)
    return Cmp;

  if (nullptr == LHSTag or nullptr == RHSTag)
    return LHSTag <=> RHSTag;

  return *LHSTag <=> *RHSTag;
}

static llvm::SmallPtrSet<LayoutTypeSystemNode *, 8>
absorbVolatileChildren(LayoutTypeSystem &TS, LayoutTypeSystemNode *Parent) {
  revng_log(Log, "absorbVolatileChildren of: " << Parent->ID);
  LoggerIndent Indent{ Log };

  llvm::SmallPtrSet<LayoutTypeSystemNode *, 8> Absorbed;

  if (isLeaf(Parent))
    return Absorbed;

  revng_assert(not hasPointerChild(Parent));

  // For dla::statistics
  const size_t ChildrenBefore = Parent->Successors.size();
  size_t Rounds = 0;
  struct ReportGrowth {
    LayoutTypeSystemNode *Parent;
    const size_t &Before;
    const size_t &Rounds;
    llvm::SmallPtrSet<LayoutTypeSystemNode *, 8> &Absorbed;
    ~ReportGrowth() {
      size_t After = Parent->Successors.size();
      if (statistics::enabled() and After >= 2000 and After > Before) {
        std::string Text = "node " + std::to_string(Parent->ID) + " | size "
                           + std::to_string(Parent->Size) + " | parents "
                           + std::to_string(Parent->Predecessors.size())
                           + " | children before " + std::to_string(Before)
                           + " | children absorbed "
                           + std::to_string(Absorbed.size()) + " | in rounds "
                           + std::to_string(Rounds) + " | children after "
                           + std::to_string(After);
        statistics::note("ArrangeAccessesHierarchically: absorbed into a node",
                         Text);
      }
    }
  } Report{ Parent, ChildrenBefore, Rounds, Absorbed };

  for (auto VolatileChildren = getVolatileChildren(Parent);
       not VolatileChildren.empty();
       VolatileChildren = getVolatileChildren(Parent)) {
    ++Rounds;

    struct InstanceEdge {
      OffsetExpression OE;
      LayoutTypeSystemNode *Target = nullptr;

      // Ordering and comparison
      std::strong_ordering operator<=>(const InstanceEdge &) const = default;
      bool operator==(const InstanceEdge &) const = default;
    };

    llvm::SetVector<InstanceEdge,
                    llvm::SmallVector<InstanceEdge, 16>,
                    llvm::SmallSet<InstanceEdge, 16>>
      CompoundEdges;

    for (const auto &[Child, Tag] :
         llvm::children_edges<NonPointerFilterT>(Parent)) {
      // Don't mess up fixed children
      if (not VolatileChildren.contains(Child))
        continue;

      OffsetExpression OE = Tag->getOffsetExpr();

      // At this point we know that Child doesn't have incoming or
      // outgoing pointer edges. So all the edges involving Child are
      // instance edges.
      for (const auto &[GrandChild, ChildTag] :
           llvm::children_edges<NonPointerFilterT>(Child)) {
        revng_assert(not VolatileChildren.contains(GrandChild));
        auto New = InstanceEdge{
          OffsetExpression::append(OE, ChildTag->getOffsetExpr()), GrandChild
        };
        CompoundEdges.insert(New);
      }
    }

    // Remove all volatile nodes
    for (LayoutTypeSystemNode *Volatile : VolatileChildren) {
      Absorbed.insert(Volatile);
      TS.dropOutgoingEdges(Volatile);
      revng_log(Log, "Merge volatile child: " << Volatile->ID);
      TS.mergeNodes({ Parent, Volatile });
    }

    for (auto &[OffsetExpr, Target] : CompoundEdges.takeVector())
      TS.addInstanceLink(Parent, Target, std::move(OffsetExpr));
  }
  return Absorbed;
}

static void absorbVolatileChildren(LayoutTypeSystem &TS) {
  std::set<const dla::LayoutTypeSystemNode *> Visited;
  for (LayoutTypeSystemNode *Root : llvm::nodes(&TS)) {

    if (Visited.contains(Root))
      continue;

    if (not isRoot(Root))
      continue;

    for (LayoutTypeSystemNode *Node :
         llvm::post_order_ext(NonPointerFilterT(Root), Visited))
      absorbVolatileChildren(TS, Node);
  }
}

/// The offset that computeOffsetAfterPush returns, in 32 bytes instead of the
/// 136 of an OffsetExpression: it never has more than one stride (and trip
/// count), so there is no use for two vectors with room for four each. The
/// lists of ArrangeAccessesHierarchically hold one of these per pair of
/// overlapping children of a node.
struct CompactOffset {
  uint64_t Offset = 0;
  uint64_t Stride = 0;
  uint64_t TripCount = 0;
  bool HasStride = false;
  bool HasTripCount = false;

  CompactOffset() = default;

  explicit CompactOffset(OffsetExpression &&OE) : Offset(OE.Offset) {
    revng_assert(OE.Strides.size() <= 1);
    revng_assert(OE.Strides.size() == OE.TripCounts.size());
    if (not OE.Strides.empty()) {
      HasStride = true;
      Stride = OE.Strides.front();
      if (OE.TripCounts.front().has_value()) {
        HasTripCount = true;
        TripCount = *OE.TripCounts.front();
      }
    }
  }
};

/// computeOffsetAfterPush, without building an OffsetExpression on the way
static void offsetAfterPush(CompactOffset &Result,
                            NeighborIterator ToBePushed,
                            NeighborIterator ToBePushedThrough) {
  revng_assert(isInstanceEdge(*ToBePushed));
  revng_assert(isInstanceEdge(*ToBePushedThrough));

  const auto &ToPushOE = ToBePushed->second->getOffsetExpr();
  const auto &ThroughOE = ToBePushedThrough->second->getOffsetExpr();

  revng_assert(ToPushOE.Strides.empty() or ToPushOE.Strides.size() == 1);
  revng_assert(ThroughOE.Strides.empty() or ThroughOE.Strides.size() == 1);
  revng_assert(ToPushOE.Strides.size() == ToPushOE.TripCounts.size());

  revng_assert(ToPushOE.Offset >= ThroughOE.Offset);
  Result.Offset = ToPushOE.Offset - ThroughOE.Offset;

  Result.HasStride = not ToPushOE.Strides.empty();
  Result.Stride = Result.HasStride ? ToPushOE.Strides.front() : 0;
  Result.HasTripCount = Result.HasStride
                        and ToPushOE.TripCounts.front().has_value();
  Result.TripCount = Result.HasTripCount ? *ToPushOE.TripCounts.front() : 0;

  if (not ThroughOE.Strides.empty())
    Result.Offset %= ThroughOE.Strides.front();

  uint64_t ThroughElemSize = ToBePushedThrough->first->Size;
  uint64_t PushedFieldSize = getFieldSize(ToBePushed->first,
                                          ToBePushed->second);
  revng_assert(ThroughElemSize >= PushedFieldSize + Result.Offset);
}

static void offsetAfterPush(OffsetExpression &Result,
                            NeighborIterator ToBePushed,
                            NeighborIterator ToBePushedThrough) {
  Result = computeOffsetAfterPush(ToBePushed, ToBePushedThrough);
}

static uint64_t offsetOf(const OffsetExpression &OE) {
  return OE.Offset;
}

static uint64_t offsetOf(const CompactOffset &Compact) {
  return Compact.Offset;
}

static OffsetExpression toOffsetExpression(OffsetExpression &&OE) {
  return std::move(OE);
}

static OffsetExpression toOffsetExpression(CompactOffset &&Compact) {
  OffsetExpression OE(Compact.Offset);
  if (Compact.HasStride) {
    OE.Strides.push_back(Compact.Stride);
    if (Compact.HasTripCount)
      OE.TripCounts.push_back(Compact.TripCount);
    else
      OE.TripCounts.push_back(std::nullopt);
  }
  return OE;
}

/// The part of ArrangeAccessesHierarchically::runOnTypeSystem that pushes the
/// children of each node down its other children.
///
/// \tparam OffsetType how the offset of an edge after a push is stored while
///         the pushes of a node are being worked out: OffsetExpression, or
///         CompactOffset (see pipefix::CompactPushedOffsets).
template<typename OffsetType, typename QueueT>
static bool pushChildrenDown(LayoutTypeSystem &TS,
                             QueueT &Queue,
                             std::set<const LayoutTypeSystemNode *> &ToAnalyze,
                             const LayoutTypeSystemNode *FakeRoot) {
  bool Changed = false;

  // For dla::statistics
  struct {
    uint64_t Analyses = 0;
    uint64_t Checked = 0;
    uint64_t Described = 0;
    uint64_t Comparisons = 0;
    uint64_t Entries = 0;
    uint64_t Repeated = 0;
    uint64_t MostEdges = 0;
    uint64_t LongestList = 0;
  } Statistics;

  while (not ToAnalyze.empty()) {

    for (LayoutTypeSystemNode *Parent : Queue) {

      // Erase Parent from ToAnalyze
      bool AlreadyAnalyzed = not ToAnalyze.erase(Parent);

      // If erasure failed, Parent wasn't in ToAnalyze, so it was already
      // analyzed in a previous iteration, and we can skip it.
      if (AlreadyAnalyzed)
        continue;

      revng_log(Log, "Analyzing parent: " << Parent->ID);
      revng_assert(Parent != FakeRoot);
      LoggerIndent Indent{ Log };

      // If we reach this point we have to analyze Parent, to see if some of its
      // Instance children can be pushed down some other of its Instance
      // children

      // We want to look at all instance children edges of Parent, and compute a
      // partial ordering between them, with the relationship "A should be
      // pushed down B".

      // Before doing this we want to absorb potential other nodes that became
      // volatile when pushing them down.
      // For instance if we had A->B, A->C, B->C, and C was pushed down B, then
      // C becomes volatile inside B.
      {
        auto Absorbed = absorbVolatileChildren(TS, Parent);
        for (auto *A : Absorbed)
          ToAnalyze.erase(A);
      }

      // Represents an edge iterator pointing to an instance edge, along with an
      // OffsetExpression that is not the one currently attached to the instance
      // edge, but that it is used to move the edge around.
      struct EdgeWithOffset {
        NeighborIterator Edge;
        OffsetType FinalOE;
        // Position of Edge among the instance edges of Parent
        unsigned Index;
      };

      revng_log(Log, "Initialize ChildrenHierarchy");
      // A map whose key is an edge iterator representing an instance edge, and
      // the mapped type is a vector of other edges that can be pushed through
      // the key, along the updated offsets that they will have after pushing
      // them through it.
      // In particular, we only keep in this map the neighbor iterator that are
      // the roots of the hierarchy of instance edges, i.e. the instance edges
      // that cannot be pushed through any other edge.
      //
      // An edge is pushed through the edges it fits inside together with what
      // was pushed through it, so the same edge reaches the same list by more
      // than one way: directly, and through each edge between the two. The
      // entries are the same every time (the offset depends on the two edges
      // only), and when the list is used the later ones do nothing that the
      // first has not done: the link they add is there, the edge they mark for
      // erasure is marked, and the one check that can stop them
      // (wouldCloseInstanceLoop) cannot answer no after having answered yes,
      // because until the list has been used links are only added.
      // With a chain of edges nested one inside the other, the list of each
      // gets a copy of the lists of all the smaller ones: twice as many
      // entries at every level. So, with Once, an edge already in a list is
      // not added to it again; the order of the first entries is the same.
      // Which edges a list holds is kept as a set of bits, one per instance
      // edge of Parent: what a list lacks of another one is then found a
      // machine word at a time, and usually it lacks nothing.
      struct PushedEdges {
        // Position among the instance edges of Parent of the edge this is the
        // list of
        unsigned Index = 0;
        llvm::SmallVector<EdgeWithOffset> Edges;
        llvm::BitVector Present;
      };
      constexpr bool Once = std::is_same_v<OffsetType, CompactOffset>;
      unsigned EdgeCount = 0;
      llvm::BitVector Missing;

      // Adds Edge to the list, unless Once and it is there. Returns whether it
      // was added.
      auto AddTo = [&EdgeCount, &Statistics](PushedEdges &Pushed,
                                             EdgeWithOffset &&Edge) {
        if (Once) {
          if (Pushed.Present.empty())
            Pushed.Present.resize(EdgeCount);
          if (Pushed.Present.test(Edge.Index)) {
            ++Statistics.Repeated;
            return false;
          }
          Pushed.Present.set(Edge.Index);
        }
        Pushed.Edges.push_back(std::move(Edge));
        ++Statistics.Entries;
        return true;
      };
      std::map<NeighborIterator, PushedEdges> ChildrenHierarchy;

      using GT = llvm::GraphTraits<LayoutTypeSystemNode *>;

      // Initialize the ChildrenHierarchy.
      // At the beginning, all children edges are roots, and none of them has
      // other edges that can be pushed through them.
      for (auto AIt = GT::child_edge_begin(Parent),
                ChildEnd = GT::child_edge_end(Parent);
           AIt != ChildEnd;
           ++AIt) {
        if (isInstanceEdge(*AIt))
          ChildrenHierarchy[AIt].Index = EdgeCount++;
      }
      Statistics.MostEdges = std::max<uint64_t>(Statistics.MostEdges,
                                                EdgeCount);

      // See pipefix::OverlapIndexedComparison. Position I is the I-th entry of
      // ChildrenHierarchy, which is also the PushedEdges::Index of that entry:
      // both follow the order of Parent's successors.
      static const bool
        Indexed = pipefix::enabled(pipefix::OverlapIndexedComparison);
      const char *CheckSwitch = "REVNG_DLA_CHECK_PUSH";
      static const bool CheckIndexed = std::getenv(CheckSwitch) != nullptr;
      struct Span {
        uint64_t Start;
        uint64_t End;
        unsigned Position;
      };
      std::vector<typename decltype(ChildrenHierarchy)::iterator> EntryAt;
      std::vector<char> Alive;
      std::vector<char> IsLeafAt;
      std::vector<Span> Leaves;
      std::vector<Span> NonLeaves;
      uint64_t LongestLeaf = 0;
      uint64_t LongestNonLeaf = 0;
      if (Indexed) {
        EntryAt.reserve(EdgeCount);
        for (auto It = ChildrenHierarchy.begin(); It != ChildrenHierarchy.end();
             ++It) {
          revng_assert(It->second.Index == EntryAt.size());
          const auto &[Child, Tag] = *It->first;
          uint64_t Start = Tag->getOffsetExpr().Offset;
          uint64_t End = Start + getFieldSize(Child, Tag);
          bool Leaf = isLeaf(Child);
          Span S{ Start, End, static_cast<unsigned>(EntryAt.size()) };
          if (Leaf) {
            Leaves.push_back(S);
            LongestLeaf = std::max(LongestLeaf, End - Start);
          } else {
            NonLeaves.push_back(S);
            LongestNonLeaf = std::max(LongestNonLeaf, End - Start);
          }
          IsLeafAt.push_back(Leaf);
          EntryAt.push_back(It);
        }
        Alive.assign(EntryAt.size(), 1);
        auto ByStart = [](const Span &A, const Span &B) {
          return A.Start < B.Start;
        };
        llvm::sort(Leaves, ByStart);
        llvm::sort(NonLeaves, ByStart);
      }

      // The positions after Position whose range overlaps [Start, End), among
      // the spans in Sorted, whose longest one is Longest
      std::vector<unsigned> Candidates;
      auto Overlapping = [&Candidates, &Alive](const std::vector<Span> &Sorted,
                                               uint64_t Longest,
                                               unsigned Position,
                                               uint64_t Start,
                                               uint64_t End) {
        // A span overlaps if it starts before End and ends after Start; it
        // cannot start before Start - Longest
        uint64_t From = Start > Longest ? Start - Longest : 0;
        auto It = std::lower_bound(Sorted.begin(),
                                   Sorted.end(),
                                   From,
                                   [](const Span &S, uint64_t Value) {
                                     return S.Start < Value;
                                   });
        for (; It != Sorted.end() and It->Start < End; ++It)
          if (It->End > Start and It->Position > Position
              and Alive[It->Position])
            Candidates.push_back(It->Position);
      };
      ++Statistics.Analyses;
      if (statistics::enabled() and EdgeCount >= 20000
          and Statistics.Described < 6) {
        // What the children of such a node are
        ++Statistics.Described;
        std::map<uint64_t, uint64_t> BySize;
        std::set<uint64_t> Offsets;
        std::vector<std::pair<uint64_t, uint64_t>> Ranges;
        uint64_t Leaves = 0;
        uint64_t Strided = 0;
        uint64_t AtZero = 0;
        uint64_t MaxEnd = 0;
        for (const auto &[Edge, Unused] : ChildrenHierarchy) {
          const auto &[Child, Tag] = *Edge;
          const auto &OE = Tag->getOffsetExpr();
          uint64_t FieldSize = getFieldSize(Child, Tag);
          ++BySize[FieldSize];
          Offsets.insert(OE.Offset);
          Ranges.emplace_back(OE.Offset, OE.Offset + FieldSize);
          Leaves += isLeaf(Child);
          Strided += not OE.Strides.empty();
          AtZero += OE.Offset == 0;
          MaxEnd = std::max(MaxEnd, OE.Offset + FieldSize);
        }
        // Deepest nesting of distinct ranges
        llvm::sort(Ranges, [](const auto &A, const auto &B) {
          return A.first != B.first ? A.first < B.first : A.second > B.second;
        });
        Ranges.erase(std::unique(Ranges.begin(), Ranges.end()), Ranges.end());
        std::vector<uint64_t> Open;
        size_t Deepest = 0;
        for (const auto &[Start, End] : Ranges) {
          while (not Open.empty() and Open.back() < End)
            Open.pop_back();
          Open.push_back(End);
          Deepest = std::max(Deepest, Open.size());
        }
        std::string Sizes;
        std::vector<std::pair<uint64_t, uint64_t>> Top(BySize.begin(),
                                                       BySize.end());
        llvm::sort(Top, [](const auto &A, const auto &B) {
          return A.second > B.second;
        });
        for (size_t I = 0; I < std::min<size_t>(Top.size(), 8); ++I)
          Sizes += " " + std::to_string(Top[I].first) + "x"
                   + std::to_string(Top[I].second);
        std::string Text = "node " + std::to_string(Parent->ID) + " | size "
                           + std::to_string(Parent->Size) + " | non scalar "
                           + std::to_string(Parent->NonScalar) + " | parents "
                           + std::to_string(Parent->Predecessors.size())
                           + " | instance edges " + std::to_string(EdgeCount)
                           + " | leaves " + std::to_string(Leaves)
                           + " | strided " + std::to_string(Strided)
                           + " | at offset 0 " + std::to_string(AtZero)
                           + " | distinct offsets "
                           + std::to_string(Offsets.size())
                           + " | distinct ranges "
                           + std::to_string(Ranges.size())
                           + " | deepest nesting of ranges "
                           + std::to_string(Deepest) + " | highest end "
                           + std::to_string(MaxEnd) + " | distinct field sizes "
                           + std::to_string(BySize.size())
                           + " | most frequent (size x count)" + Sizes;
        statistics::note("ArrangeAccessesHierarchically: children of a large "
                         "node",
                         Text);
      }
      if (statistics::enabled() and EdgeCount >= 2000) {
        std::string Text = "node " + std::to_string(Parent->ID) + " | edges "
                           + std::to_string(EdgeCount) + " | analyses so far "
                           + std::to_string(Statistics.Analyses)
                           + " | comparisons so far "
                           + std::to_string(Statistics.Comparisons)
                           + " | entries so far "
                           + std::to_string(Statistics.Entries)
                           + " | still to analyse "
                           + std::to_string(ToAnalyze.size());
        statistics::note("ArrangeAccessesHierarchically: large node", Text);
      }

      revng_log(Log, "Compare children edges");
      // Now compare each root only with other roots
      auto ARootIt = ChildrenHierarchy.begin();
      auto ARootNext = ARootIt;
      auto HierarchyEnd = ChildrenHierarchy.end();
      for (; ARootIt != HierarchyEnd; ARootIt = ARootNext) {
        LoggerIndent ChildIndent{ Log };

        auto &[AEdgeIt, PushedInsideA] = *ARootIt;

        revng_log(Log,
                  "comparing AEdge: " << AEdgeIt->first->ID
                                      << " label: " << AEdgeIt->second);

        // A vector of instance edges that are contained in AEdgeIt and that
        // should be pushed through it, along with the new offsets they will
        // have after the push through.
        llvm::SmallVector<EdgeWithOffset> ContainedInA;

        // A vector of instance edges that contain AEdgeIt. AEdgeIt will have to
        // be pushed through all of them. The FinalOE in EdgeWithOffset here
        // represents the new offset that AEdgeIt will have after being pushed
        // through them.
        llvm::SmallVector<EdgeWithOffset> ContainsA;

        // The later entries to compare A with: all of them, or, with the
        // index, those whose comparison can find something, in the same order
        Candidates.clear();
        if (Indexed) {
          unsigned APosition = PushedInsideA.Index;
          const auto &[AChild, ATag] = *AEdgeIt;
          uint64_t AStart = ATag->getOffsetExpr().Offset;
          uint64_t AEnd = AStart + getFieldSize(AChild, ATag);
          // Two leaves are never compared: a leaf only meets non-leaves
          Overlapping(NonLeaves, LongestNonLeaf, APosition, AStart, AEnd);
          if (not IsLeafAt[APosition])
            Overlapping(Leaves, LongestLeaf, APosition, AStart, AEnd);
          llvm::sort(Candidates);

          if (CheckIndexed) {
            // The full comparison finds something only for candidates
            unsigned Next = 0;
            for (auto BRootIt = ARootNext; BRootIt != HierarchyEnd; ++BRootIt) {
              unsigned BPosition = BRootIt->second.Index;
              bool IsCandidate = Next < Candidates.size()
                                 and Candidates[Next] == BPosition;
              if (IsCandidate)
                ++Next;
              else
                revng_assert(not canPushThrough(Parent, AEdgeIt, BRootIt->first)
                                   .has_value());
            }
            revng_assert(Next == Candidates.size());
            ++Statistics.Checked;
          }
        }

        auto NextB = [&,
                      CandidateIndex = size_t(0),
                      BRootIt = ARootNext]() mutable -> auto {
          if (not Indexed)
            return BRootIt == HierarchyEnd ? HierarchyEnd : BRootIt++;
          if (CandidateIndex == Candidates.size())
            return HierarchyEnd;
          return EntryAt[Candidates[CandidateIndex++]];
        };

        for (auto BRootIt = NextB(); BRootIt != HierarchyEnd;
             BRootIt = NextB()) {
          LoggerIndent OtherChildIndent{ Log };

          auto &[BEdgeIt, ContainedInB] = *BRootIt;
          ++Statistics.Comparisons;
          revng_log(Log,
                    "with BEdge: " << BEdgeIt->first->ID
                                   << " label: " << BEdgeIt->second);

          auto MaybePushThrough = canPushThrough(Parent, AEdgeIt, BEdgeIt);
          if (not MaybePushThrough.has_value())
            continue;

          auto &[ToPush, Through, OEAfterPush] = MaybePushThrough.value();
          revng_assert(ToPush == AEdgeIt or ToPush == BEdgeIt);
          revng_assert(Through == AEdgeIt or Through == BEdgeIt);
          revng_assert(Through != ToPush);
          if (ToPush == AEdgeIt) {
            revng_assert(Through == BEdgeIt);
            // AEdgeIt can be pushed inside BEdgeIt
            auto BWithNewAOffset = EdgeWithOffset{
              BEdgeIt, OffsetType(std::move(OEAfterPush)), ContainedInB.Index
            };
            ContainsA.push_back(std::move(BWithNewAOffset));
          } else {
            revng_assert(ToPush == BEdgeIt and Through == AEdgeIt);
            // BEdgeIt can be pushed inside AEdgeIt
            auto BWithNewBOffset = EdgeWithOffset{
              BEdgeIt, OffsetType(std::move(OEAfterPush)), ContainedInB.Index
            };
            ContainedInA.push_back(std::move(BWithNewBOffset));
          }
        }

        // Now, push all the roots contained in A into A, and remove them from
        // ChildrenHierarchy, since they're not roots anymore.
        PushedInsideA.Edges.reserve(PushedInsideA.Edges.size()
                                    + ContainedInA.size());
        for (EdgeWithOffset &Contained : ContainedInA)
          AddTo(PushedInsideA, EdgeWithOffset(Contained));
        for (const EdgeWithOffset &Contained : ContainedInA) {
          if (Indexed)
            Alive[Contained.Index] = 0;
          ChildrenHierarchy.erase(Contained.Edge);
        }

        // Then, for all the other edges that contain A, A can be pushed through
        // them, along with all the nodes that have already been pushed through
        // A itself (that transitively can be pushed through the others too).
        // After we've pushed A through all edges in ContainsA, we can remove A
        // itelf from ChildrenHierarchy, since it's not a root anymore.
        ARootNext = std::next(ARootIt);
        if (not ContainsA.empty()) {
          const unsigned AIndex = PushedInsideA.Index;
          for (const EdgeWithOffset &Larger : ContainsA) {
            const NeighborIterator &LargerThanA = Larger.Edge;
            const OffsetType &FinalOE = Larger.FinalOE;

            // If A needs to be pushed through LargerThanA, and all things that
            // were previously pushed pushed through A can now be pushed through
            // LargerThanA.
            auto &PushedThroughLargerThanA = ChildrenHierarchy.at(LargerThanA);

            // With Once: is there anything in the list of A that the list of
            // LargerThanA lacks? If not, there is no need to go through it.
            bool SomethingToAdd = not PushedInsideA.Edges.empty();
            if (Once and SomethingToAdd
                and not PushedThroughLargerThanA.Present.empty()) {
              Missing = PushedInsideA.Present;
              Missing.reset(PushedThroughLargerThanA.Present);
              SomethingToAdd = Missing.any();
              if (not SomethingToAdd)
                Statistics.Repeated += PushedInsideA.Edges.size();
            }

            if (SomethingToAdd) {
              if (not Once) {
                auto &Edges = PushedThroughLargerThanA.Edges;
                Edges.reserve(Edges.size() + PushedInsideA.Edges.size() + 1);
              }
              for (auto &[EdgeToPush, OEAfterPush, Index] :
                   PushedInsideA.Edges) {
                if (Once and not PushedThroughLargerThanA.Present.empty()
                    and PushedThroughLargerThanA.Present.test(Index)) {
                  ++Statistics.Repeated;
                  continue;
                }

                // We have to recompute the offset of EdgeToPush here, after
                // being pushed through LargerThanA, because this value was
                // never computed before, given that these two edges have never
                // been compared before.
                EdgeWithOffset Pushed{ EdgeToPush, OffsetType(), Index };
                offsetAfterPush(Pushed.FinalOE, EdgeToPush, LargerThanA);
                AddTo(PushedThroughLargerThanA, std::move(Pushed));
              }
            }

            // Then also AEdgeIt can be pushed through LargerThanA. The final
            // offset of AEdgeIt after being pushed through has already been
            // computed in advance, so we just use that.
            AddTo(PushedThroughLargerThanA,
                  EdgeWithOffset{ AEdgeIt, FinalOE, AIndex });
          }
          // Now AEdgeIt is not a root anymore, so we have to erase it and
          // update ARootNext.
          if (Indexed)
            Alive[PushedInsideA.Index] = 0;
          ARootNext = ChildrenHierarchy.erase(ARootIt);
        }
      }

      llvm::SmallSet<NeighborIterator, 4> EdgesToErase;
      {
        revng_log(Log, "Collect EdgesToErase");
        LoggerIndent IndentCollect{ Log };
        for (auto &[EdgeToPushThrough, Pushed] : ChildrenHierarchy) {
          auto &EdgesToPush = Pushed.Edges;

          if (EdgesToPush.empty())
            continue;

          auto *ToPushThrough = EdgeToPushThrough->first;
          ToAnalyze.insert(ToPushThrough);

          Statistics.LongestList = std::max<uint64_t>(Statistics.LongestList,
                                                      EdgesToPush.size());
          for (auto &[PushedEdgeIt, FinalOE, Unused] : EdgesToPush) {
            auto *ToPushDown = PushedEdgeIt->first;

            // We already checked that we don't close an instance loop, but we
            // did it on the graph before any push happened, and edges were only
            // compared pairwise.
            // Two individual pushes, each individually harmless, can still
            // close a loop when they are both applied.
            // This check is meant to detect this case.
            //
            // A loop is only closed if ToPushDown already reaches
            // ToPushThrough via instance edges, which means ToPushThrough is
            // not larger than ToPushDown itself. But at the same time, the fact
            // that ToPushDown is being pushed down ToPushThrough means that the
            // size of the pushed field will not be larger than ToPushThrough.
            // This can only happen when the field that is being pushed down
            // will end up covering the whole ToPushThrough.
            // This is cheap to check and more rarely requires a visit.
            uint64_t PushedFieldSize = getFieldSize(ToPushDown,
                                                    PushedEdgeIt->second);
            if (offsetOf(FinalOE) == 0
                and PushedFieldSize == ToPushThrough->Size
                and wouldCloseInstanceLoop(Parent, ToPushThrough, ToPushDown)) {
              revng_log(Log,
                        "skip: " << ToPushThrough->ID << " -> "
                                 << ToPushDown->ID
                                 << " would close an instance loop");
              continue;
            }

            // An edge can be pused through many edges. It is enough that we
            // manage to push it through one to decide that we want to get rid
            // of the original edge, because in the very moment we push it
            // through at least one other edge we are preserving the information
            // it represents, just reshaping it into another form.
            EdgesToErase.insert(PushedEdgeIt);

            revng_assert(not isLeaf(ToPushThrough));
            TS.addInstanceLink(ToPushThrough,
                               ToPushDown,
                               toOffsetExpression(std::move(FinalOE)));
          }
        }
      }

      // Now finally clean up the edges that were pushed down.
      {
        revng_log(Log, "Erase edges");
        LoggerIndent IndentErase{ Log };
        for (const auto &ToErase : EdgesToErase) {
          revng_log(Log,
                    "erase: " << Parent->ID << " -> " << ToErase->first->ID
                              << " label: " << ToErase->second);
          TS.eraseEdge(Parent, ToErase);
        }
      }

      Changed |= not EdgesToErase.empty();
    }
  }

  if (statistics::enabled()) {
    std::string Text = "analyses of a node "
                       + std::to_string(Statistics.Analyses)
                       + " | pairs of edges compared "
                       + std::to_string(Statistics.Comparisons)
                       + " | entries in the lists "
                       + std::to_string(Statistics.Entries)
                       + " | entries not added because already there "
                       + std::to_string(Statistics.Repeated)
                       + " | most instance edges of a node "
                       + std::to_string(Statistics.MostEdges)
                       + " | longest list "
                       + std::to_string(Statistics.LongestList)
                       + " | edges whose comparisons were checked "
                       + std::to_string(Statistics.Checked);
    statistics::note("ArrangeAccessesHierarchically: lists of pushed edges",
                     Text);
  }

  return Changed;
}

bool ArrangeAccessesHierarchically::runOnTypeSystem(LayoutTypeSystem &TS) {
  if (VerifyLog.isEnabled())
    revng_assert(TS.verifyDAG());

  bool Changed = false;

  absorbVolatileChildren(TS);

  // This dla::Step will heavily mutate the graph while iterating on it.
  // Despite thinking hard about it, we couldn't come up with a sane visit order
  // that could be pre-computed and guaranteed to be stable during the mutation,
  // or could be updated cheaply.
  // So we precompute some kind of global topological ordering, and use it as a
  // hard-coded ordering for analysis throughout the dla::Step.
  // Then we go fixed-point until we don't have anything left to recompute, but
  // we always follow this order because it's still better than iterating
  // randomly, or with some other ordering that is not stable.

  // Add a fake root to be able to compute a RPOT from that. What's important
  // here is not that it's strictly a RPOT, but that it is a topological
  // traversal.
  auto *FakeRoot = TS.createArtificialLayoutType();
  for (LayoutTypeSystemNode *Root : llvm::nodes(&TS)) {
    revng_assert(Root != nullptr);
    if (not isRoot(Root))
      continue;

    TS.addInstanceLink(FakeRoot, Root, OffsetExpression{});
  }

  // Compute the RPOT
  auto Queue = llvm::ReversePostOrderTraversal(NonPointerFilterT(FakeRoot));

  // Compute the node to analyze.
  std::set<const LayoutTypeSystemNode *> ToAnalyze;
  for (const auto *Node : Queue)
    ToAnalyze.insert(Node);

  // The fake root is never to analyze and can already be removed.
  ToAnalyze.erase(FakeRoot);
  TS.removeNode(FakeRoot);

  if (pipefix::enabled(pipefix::CompactPushedOffsets))
    Changed |= pushChildrenDown<CompactOffset>(TS, Queue, ToAnalyze, FakeRoot);
  else
    Changed |= pushChildrenDown<OffsetExpression>(TS,
                                                  Queue,
                                                  ToAnalyze,
                                                  FakeRoot);

  if (VerifyLog.isEnabled())
    revng_assert(TS.verifyDAG());

  return Changed;
}

} // end namespace dla
