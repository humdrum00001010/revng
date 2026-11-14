//
// This file is distributed under the MIT License. See LICENSE.md for details.
//

#include "llvm/IR/AssemblyAnnotationWriter.h"
#include "llvm/Support/FormattedStream.h"
#include "llvm/Support/GraphWriter.h"

#include "revng/Support/DOTGraphTraits.h"
#include "revng/Support/IRHelpers.h"
#include "revng/Support/VMFixes.h"
#include "revng/ValueMaterializer/ControlFlowEdgesGraph.h"

using namespace llvm;

std::string ControlFlowEdgesNode::toString() const {
  revng_assert(Source != nullptr);
  std::string Result = getName(Source);
  if (Destination != nullptr)
    Result += " -> " + getName(Destination);
  return Result;
}

void ControlFlowEdgesGraph::dump() const {
  WriteGraph(this, "cfeg");
}

using BlockSet = SmallPtrSetImpl<BasicBlock *>;

ControlFlowEdgesGraph
ControlFlowEdgesGraph::fromNodeSet(const BlockSet &NodeSet) {
  using namespace llvm;

  ControlFlowEdgesGraph Result;

  auto GetNode = [&Result](BasicBlock *BB) -> Node * {
    auto It = Result.NodeMap.find(BB);
    if (It != Result.NodeMap.end())
      return It->second;
    else
      return Result.NodeMap[BB] = Result.addNode(BB);
  };

  auto AddEdge =
    [&Result, &GetNode](Node *Source, BasicBlock *BB, BasicBlock *Destination) {
      // Create node representing the edge on the CFG and connect it
      Node *EdgeNode = Result.addNode(BB, Destination);
      Source->addSuccessor(EdgeNode);
      EdgeNode->addSuccessor(GetNode(Destination));
    };

  // Create nodes for blocks
  for (BasicBlock *BB : NodeSet) {
    // Get/create node for block
    Node *Source = GetNode(BB);

    // A switch with many cases (the dispatcher of root has one per jump
    // target): instead of scanning all its successors looking for those in
    // NodeSet, look among the uses of the blocks in NodeSet for those that are
    // operands of the switch.
    constexpr unsigned ManySuccessors = 64;
    auto *Switch = dyn_cast<SwitchInst>(BB->getTerminator());
    if (vmfix::enabled(vmfix::EdgesFromUses) and Switch != nullptr
        and Switch->getNumSuccessors() > ManySuccessors) {
      // Indices of the successors of the switch that are in NodeSet
      SmallVector<unsigned, 8> Indices;
      unsigned Visited = 0;
      bool GiveUp = false;
      for (BasicBlock *Destination : NodeSet) {
        for (const Use &U : Destination->uses()) {
          // Do not spend more than a plain scan of the successors would
          if (++Visited > Switch->getNumSuccessors()) {
            GiveUp = true;
            break;
          }

          if (U.getUser() != Switch)
            continue;

          // Operands of a switch: condition, default destination, then one
          // (value, destination) pair per case
          unsigned OperandIndex = U.getOperandNo();
          revng_assert(OperandIndex % 2 == 1);
          unsigned SuccessorIndex = (OperandIndex - 1) / 2;
          revng_assert(Switch->getSuccessor(SuccessorIndex) == Destination);
          Indices.push_back(SuccessorIndex);
        }

        if (GiveUp)
          break;
      }

      if (not GiveUp) {
        // Same order (and same duplicates) as the scan of the successors
        llvm::sort(Indices);

        if (vmfix::verifying()) {
          static vmfix::VerifyCounter Counter("edges of a large switch (uses "
                                              "vs scan of the successors)");
          SmallVector<BasicBlock *, 8> FromUses;
          for (unsigned Index : Indices)
            FromUses.push_back(Switch->getSuccessor(Index));
          SmallVector<BasicBlock *, 8> FromScan;
          for (BasicBlock *Destination : successors(BB))
            if (NodeSet.contains(Destination))
              FromScan.push_back(Destination);
          if (FromUses != FromScan)
            vmfix::verificationFailed(Counter.Name);
          Counter.record(FromScan.size());
        }

        for (unsigned Index : Indices)
          AddEdge(Source, BB, Switch->getSuccessor(Index));
        continue;
      }
    }

    for (BasicBlock *Destination : successors(BB))
      if (NodeSet.contains(Destination))
        AddEdge(Source, BB, Destination);
  }

  return Result;
}

class HighlightInstructions : public AssemblyAnnotationWriter {
public:
  using InstructionSet = SmallPtrSetImpl<Instruction *>;

private:
  const InstructionSet &Interesting;
  bool Open = false;

public:
  HighlightInstructions(const InstructionSet &Interesting) :
    Interesting(Interesting) {}

  ~HighlightInstructions() { revng_assert(not Open); }

public:
  void emitInstructionAnnot(const Instruction *I,
                            formatted_raw_ostream &Stream) final {
    if (Open) {
      Open = false;
      Stream << "</FONT>";
    }

    if (Interesting.contains(I)) {
      Open = true;
      Stream << "<FONT COLOR=\"RED\">";
    }
  }

  void finish(raw_ostream &Stream) {
    if (Open) {
      Open = false;
      Stream << "</FONT>";
    }
  }
};

using CFEG = ControlFlowEdgesGraph;

/// Put Source and Destination BasicBlocks in the node label
std::string DOTGraphTraits<const CFEG *>::getNodeLabel(const CFEG::Node *Node,
                                                       const CFEG *Graph) {
  std::string Result;

  Result += "<FONT FACE=\"monospace\">";

  if (Node->Destination != nullptr) {
    Result += "From " + getName(Node->Source) + HTMLNewline;
    Result += "  to " + getName(Node->Destination);
  } else {
    {
      HighlightInstructions Highlighter(Graph->interstingInstructions());
      raw_string_ostream Stream(Result);
      Node->Source->print(Stream, &Highlighter);
      Highlighter.finish(Stream);
    }
    replaceAll(Result, "\n", HTMLNewline);
  }

  Result += "</FONT>";

  return Result;
}

std::string
DOTGraphTraits<const CFEG *>::getNodeAttributes(const CFEG::Node *Node,
                                                const CFEG *Graph) {
  std::string Result;

  if (Node->Destination != nullptr)
    Result += "style=dashed";

  return Result;
}
