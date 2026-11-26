//
// This file is distributed under the MIT License. See LICENSE.md for details.
//

#include "llvm/IR/AssemblyAnnotationWriter.h"
#include "llvm/Support/FormattedStream.h"
#include "llvm/Support/GraphWriter.h"

#include "revng/Support/DOTGraphTraits.h"
#include "revng/Support/IRHelpers.h"
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

/// \return the successors of \p BB that are in \p NodeSet, in the order, and
///         as many times as, \p BB has them.
static SmallVector<BasicBlock *, 8> successorsIn(BasicBlock *BB,
                                                 const BlockSet &NodeSet) {
  SmallVector<BasicBlock *, 8> Result;

  // The dispatcher of `root` is a switch with one successor per jump target:
  // instead of looking for each of them in NodeSet, look for the switch among
  // the uses of the blocks in NodeSet, unless those turn out to be more.
  constexpr unsigned ManySuccessors = 64;
  auto *Switch = dyn_cast<SwitchInst>(BB->getTerminator());
  if (Switch != nullptr and Switch->getNumSuccessors() > ManySuccessors) {
    SmallVector<unsigned, 8> Indices;
    unsigned Uses = 0;
    for (BasicBlock *Successor : NodeSet) {
      for (const Use &U : Successor->uses()) {
        ++Uses;
        if (U.getUser() == Switch) {
          // The operands of a switch are the condition, the default
          // destination and then a value and a destination for each case
          revng_assert(U.getOperandNo() % 2 == 1);
          Indices.push_back((U.getOperandNo() - 1) / 2);
        }
      }

      if (Uses > Switch->getNumSuccessors())
        break;
    }

    if (Uses <= Switch->getNumSuccessors()) {
      llvm::sort(Indices);
      for (unsigned Index : Indices)
        Result.push_back(Switch->getSuccessor(Index));
      return Result;
    }
  }

  for (BasicBlock *Successor : successors(BB))
    if (NodeSet.contains(Successor))
      Result.push_back(Successor);

  return Result;
}

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

  // Create nodes for blocks
  for (BasicBlock *BB : NodeSet) {
    // Get/create node for block
    Node *Source = GetNode(BB);

    for (BasicBlock *Destination : successorsIn(BB, NodeSet)) {
      // Create node representing the edge on the CFG and connect it
      Node *EdgeNode = Result.addNode(BB, Destination);
      Source->addSuccessor(EdgeNode);
      EdgeNode->addSuccessor(GetNode(Destination));
    }
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
