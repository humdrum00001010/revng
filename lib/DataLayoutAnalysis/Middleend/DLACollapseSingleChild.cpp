//
// This file is distributed under the MIT License. See LICENSE.md for details.
//

#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/DepthFirstIterator.h"
#include "llvm/ADT/STLExtras.h"

#include "revng/DataLayoutAnalysis/DLATypeSystem.h"
#include "revng/Support/Assert.h"
#include "revng/Support/Debug.h"

#include "DLAStep.h"

using LTSN = dla::LayoutTypeSystemNode;
using PointerFilterT = EdgeFilteredGraph<LTSN *, dla::isPointerEdge>;

using namespace llvm;

static Logger Log("dla-collapse-single-child");

namespace dla {
bool CollapseSingleChild::collapseSingle(LayoutTypeSystem &TS,
                                         LayoutTypeSystemNode *Node) {
  LoggerIndent Indent{ Log };
  bool Changed = false;

  auto HasSingleNonStridedChild = [](const LTSN *Node) {
    const auto &SuccBegin = Node->Successors.begin();
    return (Node->Successors.size() == 1) and isInstanceEdge(*SuccBegin)
           and SuccBegin->second->getOffsetExpr().Strides.empty();
  };

  // Get nodes that have a single instance-at-offset-0 child
  bool Merged = true;
  while (Merged and HasSingleNonStridedChild(Node)) {
    auto &ChildEdge = *(Node->Successors.begin());
    auto &Off = ChildEdge.second->getOffsetExpr().Offset;
    LTSN *Child = ChildEdge.first;

    Merged = false;

    if (not Off) {
      // If the only child is at offset 0, and it has the same size as the
      // parent node, the two nodes are indistinguishable, hence they can be
      // merged.

      revng_log(Log, "Has single child at offset 0. Child: " << Child->ID);
      LoggerIndent MoreIndent{ Log };

      // If the parent has a size different from the child, bail out.
      const auto ChildSize = Child->Size;
      if (ChildSize != Node->Size) {
        revng_log(Log,
                  "Size mismatch! Node = " << Node->Size
                                           << " Child = " << ChildSize);
        break;
      }

      if (llvm::is_contained(llvm::depth_first(PointerFilterT(Child)), Node)) {
        revng_log(Log, "Child points to Node");
        break;
      }

      revng_log(Log, "Collapsing " << Child->ID << " into " << Node->ID);

      TS.mergeNodes({ /*Into=*/Node, /*From=*/Child });

      Changed = true;
      Merged = true;

    } else if (isPointerRoot(Node) and not isInstanceRoot(Node)) {
      // If the node doesn't have incoming pointer nodes, we can try to absorbe
      // even instance edges with offset different than zero, pushing the
      // padding out of the parent Node.
      // If it has an incoming pointer node we cannot do anything because we'd
      // change the size of the pointee (i.e. the Child)

      // If the parent has a size different from the child + offset, bail out.
      const auto ChildSize = Child->Size;
      if ((ChildSize + Off) != Node->Size) {
        revng_log(Log,
                  "Size mismatch! Node = " << Node->Size << " Child = "
                                           << ChildSize << " Offset = " << Off);
        break;
      }

      // If we would end up merging a NonScalar node into the the non-NonScalar,
      // we'd end up losing information about the size of the NonScalar one,
      // which is wrong since it's fixed and it comes from the Model. In that
      // case we bail out.
      if (Child->NonScalar) {
        revng_log(Log,
                  "Cannot merge NonScalar Child into Node! Node = "
                    << Node->Size << " Child = " << ChildSize
                    << " Offset = " << Off);
        break;
      }

      // Move Node's predecessor edges to Child, adding Off.
      auto PredIt = Node->Predecessors.begin();
      auto PredEnd = Node->Predecessors.end();
      while (PredIt != PredEnd) {
        auto Next = std::next(PredIt);
        TS.moveEdgeTarget(Node, Child, PredIt, Off);
        PredIt = Next;
      }

      TS.mergeNodes({ /*Into=*/Node, /*From=*/Child });
      Node->Size = ChildSize;

      Changed = true;
      Merged = true;
    }
  }

  return Changed;
}

/// What CollapseSingleChild::runOnTypeSystem does, node after node in order
/// of ID with collapseSingle, moving fewer links.
///
/// When a node absorbs its only child, collapseSingle moves all the links of
/// the child to the node. Here the links of the one with fewer links are
/// moved to the other. If that is the node, the child holds the links of the
/// two from then on, and stands for the node in everything that follows: it
/// is the one that goes on absorbing its own only child, and it is not
/// visited again when the visit reaches its ID (the child it was no longer
/// exists). The node itself stays in the type system without links, and gets
/// them back before returning: what is left at the end is the node, as with
/// collapseSingle, and the links of the child were moved once more, not once
/// for every node that absorbed them. Nothing in between looks at the ID of a
/// node, or at the order of the links in a set with more than one link, both
/// of which are those of the child and not of the node until then.
static bool collapseAllIntoLarger(LayoutTypeSystem &TS) {
  bool Changed = false;

  // The nodes that hold the links of a node that absorbed them, and that
  // node, which has no links meanwhile
  llvm::DenseMap<LTSN *, LTSN *> StandsFor;

  auto Links = [](const LTSN *Node) {
    return Node->Successors.size() + Node->Predecessors.size();
  };

  // Merges Child into Node, as mergeNodes({ Node, Child }) does, and returns
  // the node that holds the links of Node from then on.
  auto Absorb = [&TS, &StandsFor, &Links](LTSN *Node, LTSN *Child) -> LTSN * {
    // If Child stands for a node, that is the node being absorbed: it goes
    if (auto It = StandsFor.find(Child); It != StandsFor.end()) {
      TS.removeAbsorbedNode(It->second);
      StandsFor.erase(It);
    }

    if (Links(Child) <= Links(Node)) {
      TS.mergeNodes({ Node, Child });
      return Node;
    }

    // Child takes the links of Node and stands for it, or for the node that
    // Node stands for
    if (auto It = StandsFor.find(Node); It != StandsFor.end()) {
      LTSN *Absorbing = It->second;
      StandsFor.erase(It);
      TS.mergeNodes({ Child, Node });
      StandsFor[Child] = Absorbing;
    } else {
      TS.absorbNode(Child, Node);
      StandsFor[Child] = Node;
    }

    return Child;
  };

  auto HasSingleNonStridedChild = [](const LTSN *Node) {
    const auto &SuccBegin = Node->Successors.begin();
    return (Node->Successors.size() == 1) and isInstanceEdge(*SuccBegin)
           and SuccBegin->second->getOffsetExpr().Strides.empty();
  };

  // Neither the node being visited nor a node before it in the order of the
  // visit is removed from under the iteration, except for the nodes that are
  // left without links, which are never the one being visited
  for (LTSN *Node : llvm::nodes(&TS)) {
    // A node that stands for another one has been visited as that one
    if (StandsFor.count(Node) != 0)
      continue;

    // Same decisions as collapseSingle
    bool Merged = true;
    while (Merged and HasSingleNonStridedChild(Node)) {
      auto &ChildEdge = *(Node->Successors.begin());
      const uint64_t Off = ChildEdge.second->getOffsetExpr().Offset;
      LTSN *Child = ChildEdge.first;

      Merged = false;

      if (not Off) {
        if (Child->Size != Node->Size)
          break;

        if (llvm::is_contained(llvm::depth_first(PointerFilterT(Child)), Node))
          break;

        Node = Absorb(Node, Child);

        Changed = true;
        Merged = true;

      } else if (isPointerRoot(Node) and not isInstanceRoot(Node)) {
        const auto ChildSize = Child->Size;
        if ((ChildSize + Off) != Node->Size)
          break;

        if (Child->NonScalar)
          break;

        // Move Node's predecessor edges to Child, adding Off.
        auto PredIt = Node->Predecessors.begin();
        auto PredEnd = Node->Predecessors.end();
        while (PredIt != PredEnd) {
          auto Next = std::next(PredIt);
          TS.moveEdgeTarget(Node, Child, PredIt, Off);
          PredIt = Next;
        }

        // collapseSingle merges and then gives the node the size of the
        // child. Whichever of the two holds the links, it ends with that
        // size: set it first, so that the two have the same size when they
        // are merged.
        Node->Size = ChildSize;
        Node = Absorb(Node, Child);
        revng_assert(Node->Size == ChildSize);

        Changed = true;
        Merged = true;
      }
    }
  }

  // Give each node back its links, with those it absorbed. In order of ID,
  // so that what is done does not depend on where the nodes are in memory.
  std::vector<std::pair<LTSN *, LTSN *>> ToRestore;
  for (auto &[Stand, Absorbing] : StandsFor)
    ToRestore.emplace_back(Absorbing, Stand);
  llvm::sort(ToRestore, [](const auto &Left, const auto &Right) {
    return Left.first->ID < Right.first->ID;
  });
  for (auto &[Absorbing, Stand] : ToRestore) {
    // The size was worked out on the node that held the links
    Absorbing->Size = Stand->Size;
    TS.mergeNodes({ Absorbing, Stand });
  }

  return Changed;
}

bool CollapseSingleChild::runOnTypeSystem(LayoutTypeSystem &TS) {
  bool Changed = false;
  if (VerifyLog.isEnabled())
    revng_assert(TS.verifyDAG());

  if (pipefix::enabled(pipefix::CollapseIntoLargerNode)) {
    Changed = collapseAllIntoLarger(TS);
  } else {
    for (LTSN *Node : llvm::nodes(&TS)) {
      revng_log(Log, "Analyzing Node: " << Node->ID);
      Changed |= collapseSingle(TS, Node);
    }
  }

  if (VerifyLog.isEnabled())
    revng_assert(TS.verifyDAG());

  return Changed;
}

} // end namespace dla
