//
// This file is distributed under the MIT License. See LICENSE.md for details.
//

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <string>

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/SCCIterator.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/FormattedStream.h"
#include "llvm/Support/raw_ostream.h"

#include "revng/ADT/FilteredGraphTraits.h"
#include "revng/DataLayoutAnalysis/DLALayouts.h"
#include "revng/DataLayoutAnalysis/DLATypeSystem.h"
#include "revng/Support/Assert.h"
#include "revng/Support/Debug.h"
#include "revng/Support/IRHelpers.h"

using namespace llvm;

using NodeAllocatorT = SpecificBumpPtrAllocator<dla::LayoutTypeSystemNode>;

static Logger CollapsedNodePrinter("dla-print-collapsed-in-dot");

void *operator new(size_t, NodeAllocatorT &NodeAllocator);
void *operator new(size_t, NodeAllocatorT &NodeAllocator) {
  return NodeAllocator.Allocate();
}

namespace dla {

void OffsetExpression::print(llvm::raw_ostream &OS) const {
  OS << "Off: " << Offset;
  auto NStrides = Strides.size();
  revng_assert(NStrides == TripCounts.size());
  if (not Strides.empty()) {
    for (decltype(NStrides) N = 0; N < NStrides; ++N) {
      OS << ", {S:" << Strides[N] << ",TC:";
      if (TripCounts[N].has_value())
        OS << TripCounts[N].value();
      else
        OS << "none";
      OS << '}';
    }
  }
}

void LayoutTypePtr::print(raw_ostream &Out) const {
  Out << '{';
  Out << "0x";
  Out.write_hex(reinterpret_cast<const unsigned long long>(V));
  Out << " [";
  if (isa<Function>(V)) {
    Out << "fname: " << V->getName();
  } else {
    if (auto *I = dyn_cast<Instruction>(V))
      Out << "In Func: " << I->getFunction()->getName() << " Instr: ";
    else if (auto *A = dyn_cast<Argument>(V))
      Out << "In Func: " << A->getParent()->getName() << " Arg: ";

    Out.write_escaped(getName(V));
  }
  Out << "], 0x";
  Out.write_hex(FieldIdx);
  Out << '}';
}

std::string LayoutTypePtr::toString() const {
  std::string S;
  llvm::raw_string_ostream OS(S);
  print(OS);
  return S;
}

void LayoutTypeSystemNode::print(llvm::raw_ostream &OS) const {
  OS << "LTSN ID: " << ID;
}

namespace {

static constexpr size_t str_len(const char *S) {
  return S ? (*S ? (1 + str_len(S + 1)) : 0UL) : 0UL;
}

// We use \l here instead of \n, because graphviz has this sick way of saying
// that the text in the node labels should be left-justified
static constexpr const char DoRet[] = "\\l";
static constexpr const char NoRet[] = "";
static_assert(sizeof(DoRet) == (str_len(DoRet) + 1));
static_assert(sizeof(NoRet) == (str_len(NoRet) + 1));

static constexpr const char Equal[] = "Equal";
static constexpr const char Instance[] = "Instance of: ";
static constexpr const char Pointer[] = "Points to: ";
static constexpr const char Unexpected[] = "Unexpected!";
static_assert(sizeof(Equal) == (str_len(Equal) + 1));
static_assert(sizeof(Instance) == (str_len(Instance) + 1));
static_assert(sizeof(Unexpected) == (str_len(Unexpected) + 1));
} // end unnamed namespace

void debug_function LayoutTypeSystem::dumpDotOnFile(const char *FName,
                                                    bool ShowCollapsed) const {
  std::error_code EC;
  raw_fd_ostream DotFile(FName, EC);
  revng_check(not EC, (EC.message() + ": " + FName).c_str());

  DotFile << "digraph LayoutTypeSystem {\n";
  DotFile << "  // List of nodes\n";

  for (const LayoutTypeSystemNode *L : getLayoutsRange()) {

    DotFile << "  node_" << L->ID << " [shape=rect,label=\"NODE ID: " << L->ID
            << " Size: " << L->Size << " InterferingChild: ";

    llvm::SmallVector<const llvm::Use *, 8> PtrUses;
    switch (L->InterferingInfo) {
    case Unknown:
      DotFile << 'U';
      break;
    case AllChildrenAreInterfering:
      DotFile << 'A';
      break;
    case AllChildrenAreNonInterfering:
      DotFile << 'N';
      break;
    default:
      revng_unreachable();
    }

    DotFile << " NonScalar: " << L->NonScalar;

    if (CollapsedNodePrinter.isEnabled() or ShowCollapsed)
      DebugPrinter->printNodeContent(*this, L, DotFile);

    DotFile << "\"];\n";
  }

  DotFile << "  // List of edges\n";

  for (LayoutTypeSystemNode *L : getLayoutsRange()) {

    uint64_t SrcNodeId = L->ID;

    for (const auto &PredP : L->Predecessors) {
      const TypeLinkTag *PredTag = PredP.second;
      const auto SameLink = [&](auto &OtherPair) {
        return SrcNodeId == OtherPair.first->ID and PredTag == OtherPair.second;
      };
      revng_assert(std::any_of(PredP.first->Successors.begin(),
                               PredP.first->Successors.end(),
                               SameLink));
    }

    std::string Extra;
    std::string Color;
    std::string Style;
    for (const auto &SuccP : L->Successors) {
      const TypeLinkTag *EdgeTag = SuccP.second;
      const auto SameLink = [&](auto &OtherPair) {
        return SrcNodeId == OtherPair.first->ID and EdgeTag == OtherPair.second;
      };
      revng_assert(std::any_of(SuccP.first->Predecessors.begin(),
                               SuccP.first->Predecessors.end(),
                               SameLink));
      const auto *TgtNode = SuccP.first;
      const char *EdgeLabel = nullptr;
      size_t LabelSize = 0;
      Extra.clear();
      Style.clear();
      switch (EdgeTag->getKind()) {
      case TypeLinkTag::LK_Equality: {
        EdgeLabel = Equal;
        LabelSize = sizeof(Equal) - 1;
        Color = ",color=green";
      } break;
      case TypeLinkTag::LK_Instance: {
        EdgeLabel = Instance;
        LabelSize = sizeof(Instance) - 1;
        Extra = dumpToString(EdgeTag->getOffsetExpr());
        Color = ",color=blue";
      } break;
      case TypeLinkTag::LK_Pointer: {
        EdgeLabel = Pointer;
        LabelSize = sizeof(Pointer) - 1;
        Color = ",color=purple";
        Style = ",style=dashed";
      } break;
      default: {
        EdgeLabel = Unexpected;
        LabelSize = sizeof(Unexpected) - 1;
        Color = ",color=red";
      } break;
      }
      DotFile << "  node_" << SrcNodeId << " -> node_" << TgtNode->ID
              << " [label=\"" << StringRef(EdgeLabel, LabelSize) << Extra
              << "\"" << Color << Style << "];\n";
    }
  }

  DotFile << "}\n";
}

LayoutTypeSystemNode *LayoutTypeSystem::createArtificialLayoutType() {
  using LTSN = LayoutTypeSystemNode;
  LTSN *New = new (NodeAllocator) LayoutTypeSystemNode(NID);
  revng_assert(New);
  // For a debugger: REVNG_DLA_WATCH_ID=N keeps the address of node N
  static const long long WatchID = [] {
    const char *Text = std::getenv("REVNG_DLA_WATCH_ID");
    return Text != nullptr ? std::atoll(Text) : -1LL;
  }();
  if (WatchID >= 0 and NID == static_cast<uint64_t>(WatchID))
    statistics::Watched = New;
  ++NID;
  EqClasses.growBy1();
  bool Success = Layouts.insert(New).second;
  revng_assert(Success);
  return New;
}

SmallVector<LayoutTypeSystemNode *, 2>
LayoutTypeSystem::createArtificialLayoutTypes(unsigned N) {
  llvm::SmallVector<LayoutTypeSystemNode *, 2> Result;
  while (N--)
    Result.push_back(createArtificialLayoutType());
  return Result;
}

static void fixPredSucc(LayoutTypeSystemNode *From,
                        LayoutTypeSystemNode *Into) {

  revng_assert(From != Into);

  uint64_t FromID = From->ID;
  using IDBasedKey = std::pair<uint64_t, const TypeLinkTag *>;

  // All the predecessors of all the successors of From are updated so that they
  // point to Into
  for (auto &[Successor, Tag] : From->Successors) {
    auto It = Successor->Predecessors.lower_bound(IDBasedKey{ FromID,
                                                              nullptr });
    auto End = Successor->Predecessors.upper_bound(IDBasedKey{ FromID + 1,
                                                               nullptr });
    while (It != End) {
      auto Next = std::next(It);
      auto Extracted = Successor->Predecessors.extract(It);
      revng_assert(Extracted);
      Successor->Predecessors.insert({ Into, Extracted.value().second });
      It = Next;
    }
  }

  // All the successors of all the predecessors of From are updated so that they
  // point to Into
  for (auto &[Predecessor, Tag] : From->Predecessors) {
    auto It = Predecessor->Successors.lower_bound(IDBasedKey{ FromID,
                                                              nullptr });
    auto End = Predecessor->Successors.upper_bound(IDBasedKey{ FromID + 1,
                                                               nullptr });
    while (It != End) {
      auto Next = std::next(It);
      auto Extracted = Predecessor->Successors.extract(It);
      revng_assert(Extracted);
      Predecessor->Successors.insert({ Into, Extracted.value().second });
      It = Next;
    }
  }

  // Merge all the predecessors and successors.
  {
    Into->Predecessors.insert(From->Predecessors.begin(),
                              From->Predecessors.end());
    Into->Successors.insert(From->Successors.begin(), From->Successors.end());
  }

  // Remove self-references from predecessors and successors.
  {
    const auto RemoveSelfEdges = [FromID,
                                  IntoID = Into->ID](auto &NeighborsSet) {
      auto FromIt = NeighborsSet.lower_bound(IDBasedKey{ FromID, nullptr });
      auto FromEnd = NeighborsSet.upper_bound(IDBasedKey{ FromID + 1,
                                                          nullptr });
      NeighborsSet.erase(FromIt, FromEnd);
      auto IntoIt = NeighborsSet.lower_bound(IDBasedKey{ IntoID, nullptr });
      auto IntoEnd = NeighborsSet.upper_bound(IDBasedKey{ IntoID + 1,
                                                          nullptr });
      NeighborsSet.erase(IntoIt, IntoEnd);
    };
    RemoveSelfEdges(Into->Predecessors);
    RemoveSelfEdges(Into->Successors);
  }
}

static Logger MergeLog("dla-merge-nodes");

void LayoutTypeSystem::absorbNode(LayoutTypeSystemNode *Into,
                                  LayoutTypeSystemNode *From) {
  revng_assert(From != Into);
  revng_log(MergeLog, "Merging: " << From->ID << " Into: " << Into->ID);

  Into->InterferingInfo = Unknown;
  if (Into->NonScalar or From->NonScalar) {
    revng_assert(not(Into->NonScalar and From->NonScalar)
                 or Into->Size == From->Size);
    auto *NonScalar = Into->NonScalar ? Into : From;
    auto *Other = Into->NonScalar ? From : Into;
    revng_assert(Other->Size <= NonScalar->Size);
    Into->Size = NonScalar->Size;
  } else {
    revng_assert(not Into->Size or From->Size <= Into->Size);
    Into->Size = std::max(Into->Size, From->Size);
  }
  Into->NonScalar |= From->NonScalar;

  const unsigned IntoID = Into->ID;
  EqClasses.join(IntoID, From->ID);

  fixPredSucc(From, Into);

  // The links of From are links of Into now
  From->Predecessors.clear();
  From->Successors.clear();
}

void LayoutTypeSystem::removeAbsorbedNode(LayoutTypeSystemNode *Absorbed) {
  revng_assert(Absorbed->Predecessors.empty());
  revng_assert(Absorbed->Successors.empty());

  bool Erased = Layouts.erase(Absorbed);
  revng_assert(Erased);
  Absorbed->~LayoutTypeSystemNode();
  NodeAllocator.Deallocate(Absorbed);
}

void LayoutTypeSystem::mergeNodes(llvm::ArrayRef<LayoutTypeSystemNode *>
                                    ToMerge) {
  revng_assert(ToMerge.size() > 0ULL);
  // If we're merging just one node there's nothing to do.
  if (ToMerge.size() <= 1ULL)
    return;

  LayoutTypeSystemNode *Into = ToMerge[0];
  for (LayoutTypeSystemNode *From : llvm::drop_begin(ToMerge, 1)) {
    absorbNode(Into, From);
    removeAbsorbedNode(From);
  }
}

void LayoutTypeSystem::removeNode(LayoutTypeSystemNode *ToRemove) {
  // Join the node's eq class with the removed class
  uint64_t TheID = ToRemove->ID;
  EqClasses.remove(TheID);
  revng_log(MergeLog, "Removing " << ToRemove->ID << "\n");

  using IDBasedKey = std::pair<uint64_t, const TypeLinkTag *>;

  for (auto &[Neighbor, Tag] : ToRemove->Successors) {
    auto &PredOfSucc = Neighbor->Predecessors;
    auto It = PredOfSucc.lower_bound(IDBasedKey{ TheID, nullptr });
    auto End = PredOfSucc.upper_bound(IDBasedKey{ TheID + 1, nullptr });
    PredOfSucc.erase(It, End);
  }

  for (auto &[Neighbor, Tag] : ToRemove->Predecessors) {
    auto &SuccOfPred = Neighbor->Successors;
    auto It = SuccOfPred.lower_bound(IDBasedKey{ TheID, nullptr });
    auto End = SuccOfPred.upper_bound(IDBasedKey{ TheID + 1, nullptr });
    SuccOfPred.erase(It, End);
  }

  bool Erased = Layouts.erase(ToRemove);
  revng_assert(Erased);
  ToRemove->~LayoutTypeSystemNode();
  NodeAllocator.Deallocate(ToRemove);
}

using NeighborIterator = LayoutTypeSystem::NeighborIterator;

static void moveEdgeTargetWithoutSumming(LayoutTypeSystemNode *OldTgt,
                                         LayoutTypeSystemNode *NewTgt,
                                         NeighborIterator InverseEdgeIt) {

  // First, move the successor from OldTgt to NewTgt
  LayoutTypeSystemNode *Src = InverseEdgeIt->first;
  auto SuccHandle = Src->Successors.extract({ OldTgt, InverseEdgeIt->second });
  revng_assert(not SuccHandle.empty());
  SuccHandle.value().first = NewTgt;
  Src->Successors.insert(std::move(SuccHandle));

  // Then, move the predecessor edge from OldTgt to NewTgt
  auto PredHandle = OldTgt->Predecessors.extract(InverseEdgeIt);
  revng_assert(not PredHandle.empty());
  NewTgt->Predecessors.insert(std::move(PredHandle));
}

static void moveEdgeSourceWithoutSumming(LayoutTypeSystemNode *OldSrc,
                                         LayoutTypeSystemNode *NewSrc,
                                         NeighborIterator EdgeIt) {
  // First, move the predecessor edge from OldSrc to NewSrc.
  LayoutTypeSystemNode *Tgt = EdgeIt->first;
  auto PredHandle = Tgt->Predecessors.extract({ OldSrc, EdgeIt->second });
  revng_assert(not PredHandle.empty());
  PredHandle.value().first = NewSrc;
  Tgt->Predecessors.insert(std::move(PredHandle));

  // Then, move the successor edge from OldSrc to NewSrc
  auto SuccHandle = OldSrc->Successors.extract(EdgeIt);
  revng_assert(not SuccHandle.empty());
  NewSrc->Successors.insert(std::move(SuccHandle));
}

void LayoutTypeSystem::moveEdgeTarget(LayoutTypeSystemNode *OldTgt,
                                      LayoutTypeSystemNode *NewTgt,
                                      NeighborIterator InverseEdgeIt,
                                      int64_t OffsetToSum) {

  if (not OldTgt or not NewTgt)
    return;

  if (not OffsetToSum)
    return moveEdgeTargetWithoutSumming(OldTgt, NewTgt, InverseEdgeIt);

  LayoutTypeSystemNode *Src = InverseEdgeIt->first;

  // Erase info in Src that represent the fact that OldTgt was a successor.
  bool Erased = Src->Successors.erase({ OldTgt, InverseEdgeIt->second });
  revng_assert(Erased);

  // Extract the predecessor edge to be moved from OldTgt to NewTgt
  auto OldPredHandle = OldTgt->Predecessors.extract(InverseEdgeIt);
  revng_assert(not OldPredHandle.empty());

  // Add new instance links with adjusted offsets from Src to NewTgt.
  const TypeLinkTag *EdgeTag = OldPredHandle.value().second;
  switch (EdgeTag->getKind()) {

  case TypeLinkTag::LK_Instance: {
    OffsetExpression NewOE = EdgeTag->getOffsetExpr();
    NewOE.Offset += OffsetToSum;
    addInstanceLink(Src, NewTgt, std::move(NewOE));
  } break;

  case TypeLinkTag::LK_Equality:
  case TypeLinkTag::LK_Pointer:
  default:
    revng_unreachable("unexpected edge kind");
  }
}

void LayoutTypeSystem::moveEdgeSource(LayoutTypeSystemNode *OldSrc,
                                      LayoutTypeSystemNode *NewSrc,
                                      NeighborIterator EdgeIt,
                                      int64_t OffsetToSum) {

  if (not OldSrc or not NewSrc)
    return;

  if (not OffsetToSum)
    return moveEdgeSourceWithoutSumming(OldSrc, NewSrc, EdgeIt);

  LayoutTypeSystemNode *Tgt = EdgeIt->first;

  // Erase info in Tgt that represent the fact that OldSrc was a predecessor.
  bool Erased = Tgt->Predecessors.erase({ OldSrc, EdgeIt->second });
  revng_assert(Erased);

  // Extract the successor edge to be moved from OldSrc to NewSrc
  auto OldSuccHandle = OldSrc->Successors.extract(EdgeIt);
  revng_assert(not OldSuccHandle.empty());

  // Add new instance links with adjusted offsets from NewSrc to Tgt.
  const TypeLinkTag *EdgeTag = OldSuccHandle.value().second;
  switch (EdgeTag->getKind()) {

  case TypeLinkTag::LK_Instance: {
    OffsetExpression NewOE = EdgeTag->getOffsetExpr();
    NewOE.Offset += OffsetToSum;
    addInstanceLink(NewSrc, Tgt, std::move(NewOE));
  } break;

  case TypeLinkTag::LK_Equality:
  case TypeLinkTag::LK_Pointer:
  default:
    revng_unreachable("unexpected edge kind");
  }
}

NeighborIterator LayoutTypeSystem::eraseEdge(LayoutTypeSystemNode *Src,
                                             NeighborIterator EdgeIt) {
  LayoutTypeSystemNode *Tgt = EdgeIt->first;

  // Erase the inverse edge from Tgt to Src
  bool Erased = Tgt->Predecessors.erase({ Src, EdgeIt->second });
  revng_assert(Erased);

  // Erase the actual forward edge from Src to Tgt
  return Src->Successors.erase(EdgeIt);
}

void LayoutTypeSystem::dropOutgoingEdges(LayoutTypeSystemNode *N) {
  auto It = N->Successors.begin();
  auto End = N->Successors.end();
  while (It != End)
    It = eraseEdge(N, It);
}

static Logger VerifyDLALog("dla-verify-strict");

bool LayoutTypeSystem::verifyConsistency() const {
  for (LayoutTypeSystemNode *NodePtr : Layouts) {
    if (not NodePtr) {
      if (VerifyDLALog.isEnabled())
        revng_check(false);
      return false;
    }
    // Check that predecessors and successors are consistent
    for (auto &P : NodePtr->Predecessors) {
      if (P.first == nullptr) {
        if (VerifyDLALog.isEnabled())
          revng_check(false);
        return false;
      }

      // same edge with same tag
      if (!P.first->Successors.contains({ NodePtr, P.second })) {
        if (VerifyDLALog.isEnabled())
          revng_check(false);
        return false;
      }
    }
    for (auto &P : NodePtr->Successors) {
      if (P.first == nullptr) {
        if (VerifyDLALog.isEnabled())
          revng_check(false);
        return false;
      }

      // same edge with same tag
      if (!P.first->Predecessors.contains({ NodePtr, P.second })) {
        if (VerifyDLALog.isEnabled())
          revng_check(false);
        return false;
      }
    }

    // Check that there are no self-edges
    for (auto &P : NodePtr->Predecessors) {
      LayoutTypeSystemNode *Pred = P.first;
      if (Pred == NodePtr) {
        if (VerifyDLALog.isEnabled())
          revng_check(false);
        return false;
      }
    }

    for (auto &P : NodePtr->Successors) {
      LayoutTypeSystemNode *Succ = P.first;
      if (Succ == NodePtr) {
        if (VerifyDLALog.isEnabled())
          revng_check(false);
        return false;
      }
    }

    // Verify that pointers are not also structs or unions
    unsigned NonPtrChildren = 0U;
    bool IsPointer = false;
    for (const auto &Edge : NodePtr->Successors) {
      if (isPointerEdge(Edge)) {
        // Node with outgoing pointer edges, only have one pointer edge.
        if (IsPointer) {
          if (VerifyDLALog.isEnabled())
            revng_check(false);
          return false;
        }
        IsPointer = true;
      } else if (isInstanceEdge(Edge)) {
        NonPtrChildren++;
      }

      if (IsPointer and NonPtrChildren > 0) {
        if (VerifyDLALog.isEnabled())
          revng_check(false);
        return false;
      }
    }
  }
  return true;
}

bool LayoutTypeSystem::verifyDAG() const {
  if (not verifyConsistency())
    return false;

  if (not verifyInstanceDAG())
    return false;

  // A graph is a DAG if and only if all its strongly connected components have
  // size 1
  std::set<const LayoutTypeSystemNode *> Visited;
  for (const auto &Node : llvm::nodes(this)) {
    revng_assert(Node != nullptr);
    if (Visited.contains(Node))
      continue;

    using NonPointerFilterT = EdgeFilteredGraph<LayoutTypeSystemNode *,
                                                isNotPointerEdge>;

    auto I = scc_begin(NonPointerFilterT(Node));
    auto E = scc_end(NonPointerFilterT(Node));
    for (; I != E; ++I) {
      Visited.insert(I->begin(), I->end());
      if (I.hasCycle()) {
        if (VerifyDLALog.isEnabled())
          revng_check(false);
        return false;
      }
    }
  }

  return true;
}

bool LayoutTypeSystem::verifyInstanceDAG() const {
  if (not verifyConsistency())
    return false;

  // A graph is a DAG if and only if all its strongly connected components have
  // size 1
  std::set<const LayoutTypeSystemNode *> Visited;
  for (const auto &Node : llvm::nodes(this)) {
    revng_assert(Node != nullptr);
    if (Visited.contains(Node))
      continue;

    using GraphNodeT = const LayoutTypeSystemNode *;
    using InstanceNodeT = EdgeFilteredGraph<GraphNodeT, isInstanceEdge>;
    auto I = scc_begin(InstanceNodeT(Node));
    auto E = scc_end(InstanceNodeT(Node));
    for (; I != E; ++I) {
      Visited.insert(I->begin(), I->end());
      if (I.hasCycle()) {
        if (VerifyDLALog.isEnabled())
          revng_check(false);
        return false;
      }
    }
  }

  return true;
}

bool LayoutTypeSystem::verifyPointerDAG() const {
  if (not verifyConsistency())
    return false;

  // A graph is a DAG if and only if all its strongly connected components have
  // size 1
  std::set<const LayoutTypeSystemNode *> Visited;
  for (const auto &Node : llvm::nodes(this)) {
    revng_assert(Node != nullptr);
    if (Visited.contains(Node))
      continue;

    using GraphNodeT = const LayoutTypeSystemNode *;
    using PointerNodeT = EdgeFilteredGraph<GraphNodeT, isPointerEdge>;
    auto I = scc_begin(PointerNodeT(Node));
    auto E = scc_end(PointerNodeT(Node));
    for (; I != E; ++I) {
      Visited.insert(I->begin(), I->end());
      if (I.hasCycle()) {
        if (VerifyDLALog.isEnabled())
          revng_check(false);
        return false;
      }
    }
  }

  return true;
}

bool LayoutTypeSystem::verifyNoEquality() const {
  if (not verifyConsistency())
    return false;
  for (const auto &Node : llvm::nodes(this)) {
    using LTSN = LayoutTypeSystemNode;
    for (const auto &Edge : llvm::children_edges<const LTSN *>(Node)) {
      if (isEqualityEdge(Edge)) {
        if (VerifyDLALog.isEnabled())
          revng_check(false);
        return false;
      }
    }
  }
  return true;
}

bool LayoutTypeSystem::verifyInstanceAtOffset0DAG() const {
  if (not verifyConsistency())
    return false;

  std::set<const LayoutTypeSystemNode *> Visited;
  for (const auto &Node : llvm::nodes(this)) {
    revng_assert(Node != nullptr);
    if (Visited.contains(Node))
      continue;

    using GraphNodeT = const LayoutTypeSystemNode *;
    using InstanceNodeT = EdgeFilteredGraph<GraphNodeT, isInstanceOff0>;
    auto I = scc_begin(InstanceNodeT(Node));
    auto E = scc_end(InstanceNodeT(Node));
    for (; I != E; ++I) {
      Visited.insert(I->begin(), I->end());
      if (I.hasCycle()) {
        if (VerifyDLALog.isEnabled())
          revng_check(false);
        return false;
      }
    }
  }

  return true;
}

bool LayoutTypeSystem::verifyLeafs() const {
  for (const auto &Node : llvm::nodes(this)) {
    if (isLeaf(Node) and Node->Size == 0) {
      if (VerifyDLALog.isEnabled())
        revng_check(false);
      return false;
    }
  }

  return true;
}

bool LayoutTypeSystem::verifyUnions() const {
  using GraphNodeT = const LayoutTypeSystemNode *;
  for (GraphNodeT Node : llvm::nodes(this)) {
    if (Node->InterferingInfo == AllChildrenAreInterfering
        and Node->Successors.size() <= 1) {
      if (VerifyDLALog.isEnabled())
        revng_check(false);
      return false;
    }
  }

  return true;
}

unsigned VectEqClasses::growBy1() {
  ++NElems;
  grow(NElems);
  return NElems;
}

bool orderByID() {
  static const bool Enabled = std::getenv("REVNG_DLA_ORDER_BY_ID") != nullptr;
  return Enabled;
}

namespace statistics {

static std::FILE *output() {
  static std::FILE *File = []() -> std::FILE * {
    const char *Path = std::getenv("REVNG_DLA_STATS");
    if (Path == nullptr or Path[0] == '\0')
      return nullptr;
    return std::fopen(Path, "a");
  }();
  return File;
}

bool enabled() {
  return output() != nullptr;
}

/// Resident memory and its peak, in kB
static std::pair<uint64_t, uint64_t> residentMemory() {
  uint64_t Now = 0;
  uint64_t Peak = 0;
  if (std::FILE *Status = std::fopen("/proc/self/status", "r")) {
    char Line[256];
    while (std::fgets(Line, sizeof(Line), Status) != nullptr) {
      unsigned long long Value = 0;
      if (std::sscanf(Line, "VmRSS: %llu", &Value) == 1)
        Now = Value;
      else if (std::sscanf(Line, "VmHWM: %llu", &Value) == 1)
        Peak = Value;
    }
    std::fclose(Status);
  }
  return { Now, Peak };
}

static std::string prefix(llvm::StringRef Label) {
  using Clock = std::chrono::steady_clock;
  static const Clock::time_point Start = Clock::now();
  static const std::clock_t StartCPU = std::clock();
  double Wall = std::chrono::duration<double>(Clock::now() - Start).count();
  double CPU = double(std::clock() - StartCPU) / CLOCKS_PER_SEC;
  auto [Resident, Peak] = residentMemory();
  char Buffer[160];
  std::snprintf(Buffer,
                sizeof(Buffer),
                " | wall %.2f s | cpu %.2f s | rss %llu kB | peak %llu kB",
                Wall,
                CPU,
                static_cast<unsigned long long>(Resident),
                static_cast<unsigned long long>(Peak));
  return Label.str() + Buffer;
}

void note(llvm::StringRef Label, llvm::StringRef Text) {
  std::FILE *File = output();
  if (File == nullptr)
    return;
  std::fprintf(File, "%s | %s\n", prefix(Label).c_str(), Text.str().c_str());
  std::fflush(File);
}

/// The node with the most predecessors at the last call of record(): for a
/// debugger, which can read it without evaluating expressions
const LayoutTypeSystemNode *MostPredecessors = nullptr;

/// See createArtificialLayoutType
LayoutTypeSystemNode *Watched = nullptr;

void record(llvm::StringRef Label, const LayoutTypeSystem &TS) {
  std::FILE *File = output();
  if (File == nullptr)
    return;

  // The time and memory of the step, before those of the measurement
  std::string Prefix = prefix(Label);

  // FNV-1a over 64-bit words
  uint64_t Hash = 0xcbf29ce484222325ULL;
  auto Mix = [&Hash](uint64_t Value) {
    for (unsigned I = 0; I < 8; ++I) {
      Hash ^= (Value >> (8 * I)) & 0xff;
      Hash *= 0x100000001b3ULL;
    }
  };

  uint64_t Links[3] = { 0, 0, 0 };
  uint64_t MostSuccessors = 0;
  uint64_t MostPredecessorsCount = 0;
  MostPredecessors = nullptr;
  for (const LayoutTypeSystemNode *Node : TS.getLayoutsRange()) {
    Mix(Node->ID);
    Mix(Node->Size);
    Mix(Node->InterferingInfo);
    Mix(Node->NonScalar);
    MostSuccessors = std::max<uint64_t>(MostSuccessors,
                                        Node->Successors.size());
    if (Node->Predecessors.size() > MostPredecessorsCount) {
      MostPredecessorsCount = Node->Predecessors.size();
      MostPredecessors = Node;
    }
    for (const auto &[Successor, Tag] : Node->Successors) {
      auto Kind = Tag->getKind();
      ++Links[Kind];
      Mix(Successor->ID);
      Mix(Kind);
      if (Kind == TypeLinkTag::LK_Instance) {
        const OffsetExpression &OE = Tag->getOffsetExpr();
        Mix(OE.Offset);
        Mix(OE.Strides.size());
        for (uint64_t Stride : OE.Strides)
          Mix(Stride);
        for (const std::optional<uint64_t> &TripCount : OE.TripCounts)
          Mix(TripCount.has_value() ? *TripCount + 1 : 0);
      }
    }
    Mix(0xffffffffffffffffULL);
  }

  // The equivalence classes, as long as they are not compressed
  const VectEqClasses &Classes = TS.getEqClasses();
  if (Classes.getNumClasses() == 0)
    for (unsigned I = 0; I < Classes.getNumElements(); ++I)
      Mix(Classes.findLeader(I));

  std::fprintf(File,
               "%s | nodes %llu of %llu | equality %llu | instance %llu |"
               " pointer %llu | most successors %llu | most predecessors %llu |"
               " fingerprint %016llx | most predecessors: node %llu, size "
               "%llu\n",
               Prefix.c_str(),
               static_cast<unsigned long long>(TS.getNumLayouts()),
               static_cast<unsigned long long>(TS.getNID()),
               static_cast<unsigned long long>(Links[TypeLinkTag::LK_Equality]),
               static_cast<unsigned long long>(Links[TypeLinkTag::LK_Instance]),
               static_cast<unsigned long long>(Links[TypeLinkTag::LK_Pointer]),
               static_cast<unsigned long long>(MostSuccessors),
               static_cast<unsigned long long>(MostPredecessorsCount),
               static_cast<unsigned long long>(Hash),
               static_cast<unsigned long long>(MostPredecessors ?
                                                 MostPredecessors->ID :
                                                 0),
               static_cast<unsigned long long>(MostPredecessors ?
                                                 MostPredecessors->Size :
                                                 0));
  std::fflush(File);
}

} // namespace statistics

void VectEqClasses::remove(const unsigned A) {
  if (RemovedID)
    join(A, *RemovedID);
  else
    RemovedID = A;
}

bool VectEqClasses::isRemoved(const unsigned ID) const {
  // No removed nodes
  if (not RemovedID)
    return false;

  // Uncompressed map
  if (getNumClasses() == 0)
    return (findLeader(ID) == findLeader(*RemovedID));

  // Compressed map
  unsigned ElementEqClass = lookupEqClass(ID);
  unsigned RemovedEqClass = lookupEqClass(*RemovedID);
  return (ElementEqClass == RemovedEqClass);
}

std::optional<unsigned> VectEqClasses::getEqClassID(const unsigned ID) const {
  unsigned EqID = lookupEqClass(ID);
  bool IsRemoved = (RemovedID) ? lookupEqClass(*RemovedID) == EqID : false;

  if (IsRemoved)
    return {};
  return EqID;
}

std::vector<unsigned>
VectEqClasses::computeEqClass(const unsigned ElemID) const {
  std::vector<unsigned> EqClass;

  for (unsigned OtherID = 0; OtherID < NElems; OtherID++)
    if (haveSameEqClass(ElemID, OtherID))
      EqClass.push_back(OtherID);

  return EqClass;
}

bool VectEqClasses::haveSameEqClass(unsigned ID1, unsigned ID2) const {
  // Uncompressed map
  if (getNumClasses() == 0)
    return findLeader(ID1) == findLeader(ID2);

  // Compressed map
  return lookupEqClass(ID1) == lookupEqClass(ID2);
}

void TSDebugPrinter::printNodeContent(const LayoutTypeSystem &TS,
                                      const LayoutTypeSystemNode *N,
                                      llvm::raw_fd_ostream &File) const {
  auto EqClasses = TS.getEqClasses();

  File << DoRet;
  if (EqClasses.isRemoved(N->ID))
    File << "Removed" << DoRet;

  File << "Equivalence Class: [";
  for (auto ID : EqClasses.computeEqClass(N->ID))
    File << ID << ", ";
  File << "]" << DoRet;
}

void writeToLog(Logger &Log, const TypeLinkTag &Tag, int /* Ignore */) {

  switch (Tag.getKind()) {
  case TypeLinkTag::LK_Equality: {
    Log << Equal;
  } break;
  case TypeLinkTag::LK_Instance: {
    Log << Instance << dumpToString(Tag.getOffsetExpr());
  } break;
  case TypeLinkTag::LK_Pointer: {
    Log << Pointer;
  } break;
  default: {
    Log << Unexpected;
  } break;
  }
}

} // end namespace dla
