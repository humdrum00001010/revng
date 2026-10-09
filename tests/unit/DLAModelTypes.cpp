//
// This file is distributed under the MIT License. See LICENSE.md for details.
//

#define BOOST_TEST_MODULE DLAModelTypes
bool init_unit_test();

#include <algorithm>
#include <array>
#include <vector>

#include "boost/test/unit_test.hpp"

#include "llvm/IR/Constants.h"
#include "llvm/IR/LLVMContext.h"

#include "revng/DataLayoutAnalysis/DLATypeSystem.h"
#include "revng/Model/PointerType.h"
#include "revng/Model/PrimitiveType.h"
#include "revng/Model/StructDefinition.h"

#include "lib/DataLayoutAnalysis/Backend/DLAMakeModelTypes.h"

using LTSN = dla::LayoutTypeSystemNode;

struct EnableVerification {
  EnableVerification() { VerifyLog.enable(); }
};
BOOST_GLOBAL_FIXTURE(EnableVerification);

struct TypeGraph {
  llvm::LLVMContext Context;
  TupleTree<model::Binary> Model;
  dla::LayoutTypeSystem TS;
  dla::LayoutTypePtrVect Values;
  TypeMapT Types;
  uint64_t PointerSize;

  explicit TypeGraph(model::Architecture::Values Architecture) :
    PointerSize(model::Architecture::getPointerSize(Architecture)) {
    Model->Architecture() = Architecture;
  }

  LTSN *addNode(uint64_t Size) {
    LTSN *Node = TS.createArtificialLayoutType();
    Node->Size = Size;
    Node->InterferingInfo = dla::AllChildrenAreNonInterfering;
    const llvm::Value
      *Value = llvm::ConstantInt::get(llvm::Type::getInt64Ty(Context),
                                      Values.size() + 1);
    Values.emplace_back(Value);
    return Node;
  }

  LTSN *addPointer() { return addNode(PointerSize); }

  void materialize() {
    BOOST_REQUIRE(VerifyLog.isEnabled());
    BOOST_REQUIRE(TS.verifyDAG());
    TS.getEqClasses().compress();
    Types = dla::makeModelTypes(TS, Values, Model);
    BOOST_REQUIRE(Model->verify(true));
    BOOST_REQUIRE_EQUAL(Types.size(), Values.size());
    for (const auto &[Value, Type] : Types)
      BOOST_REQUIRE(Type->verify());
  }

  const model::UpcastableType &type(const LTSN *Node) const {
    return Types.at(Values.at(Node->ID));
  }

  const model::Type *pointeeAfter(const LTSN *Node, unsigned Depth) const {
    const model::Type *Current = type(Node).get();
    for (unsigned I = 0; I < Depth; ++I) {
      const auto *Pointer = llvm::dyn_cast<model::PointerType>(Current);
      BOOST_REQUIRE(Pointer != nullptr);
      BOOST_CHECK_EQUAL(Pointer->PointerSize(), PointerSize);
      Current = Pointer->PointeeType().get();
    }
    return Current;
  }

  void checkVoidPointer(const LTSN *Node, unsigned Depth = 1) const {
    const auto *
      Pointee = llvm::dyn_cast<model::PrimitiveType>(pointeeAfter(Node, Depth));
    BOOST_REQUIRE(Pointee != nullptr);
    BOOST_CHECK(Pointee->PrimitiveKind() == model::PrimitiveKind::Void);
    BOOST_CHECK_EQUAL(Pointee->Size(), 0U);
  }
};

static constexpr std::array Architectures = { model::Architecture::x86,
                                              model::Architecture::x86_64 };

BOOST_AUTO_TEST_CASE(RejectedSelfLinkPreservesScalar) {
  for (auto Architecture : Architectures) {
    TypeGraph G(Architecture);
    LTSN *Node = G.addPointer();
    auto [Tag, Added] = G.TS.addPointerLink(Node, Node);
    BOOST_CHECK(Tag == nullptr);
    BOOST_CHECK(not Added);
    BOOST_CHECK(Node->Successors.empty());
    BOOST_CHECK(Node->Predecessors.empty());
    G.materialize();
    const auto *Scalar = llvm::dyn_cast<model::PrimitiveType>(G.type(Node)
                                                                .get());
    BOOST_REQUIRE(Scalar != nullptr);
    BOOST_CHECK(Scalar->PrimitiveKind() == model::PrimitiveKind::Generic);
    BOOST_CHECK_EQUAL(Scalar->Size(), G.PointerSize);
  }
}

BOOST_AUTO_TEST_CASE(TwoPointerCycle) {
  for (auto Architecture : Architectures) {
    TypeGraph G(Architecture);
    LTSN *A = G.addPointer();
    LTSN *B = G.addPointer();
    G.TS.addPointerLink(A, B);
    G.TS.addPointerLink(B, A);
    BOOST_CHECK(not G.TS.verifyPointerDAG());
    G.materialize();
    G.checkVoidPointer(A);
    G.checkVoidPointer(B);
    BOOST_CHECK(G.Model->TypeDefinitions().empty());
    // Backend resolution does not rewrite the input graph or weaken its
    // independent pointer-DAG verifier.
    BOOST_CHECK(not G.TS.verifyPointerDAG());
  }
}

BOOST_AUTO_TEST_CASE(CompleteCyclesAndEnteringTailsAreOrderIndependent) {
  for (auto Architecture : Architectures) {
    for (bool Reverse : { false, true }) {
      TypeGraph G(Architecture);
      std::array<LTSN *, 7> Nodes{};
      for (unsigned I = 0; I < Nodes.size(); ++I) {
        unsigned Index = Reverse ? Nodes.size() - I - 1 : I;
        Nodes[Index] = G.addPointer();
      }
      // A three-node cycle, a separate two-node cycle, and a two-pointer tail
      // entering the first cycle. Reversing creation and edge insertion order
      // must not turn any cycle member into void ** or shorten the tail.
      std::array Edges = { std::pair{ 0U, 1U }, std::pair{ 1U, 2U },
                           std::pair{ 2U, 0U }, std::pair{ 3U, 4U },
                           std::pair{ 4U, 3U }, std::pair{ 5U, 2U },
                           std::pair{ 6U, 5U } };
      if (Reverse)
        std::reverse(Edges.begin(), Edges.end());
      for (auto [From, To] : Edges)
        G.TS.addPointerLink(Nodes[From], Nodes[To]);
      G.materialize();
      for (unsigned I = 0; I < 5; ++I)
        G.checkVoidPointer(Nodes[I]);
      G.checkVoidPointer(Nodes[5], 2);
      G.checkVoidPointer(Nodes[6], 3);
      BOOST_CHECK(G.Model->TypeDefinitions().empty());
    }
  }
}

BOOST_AUTO_TEST_CASE(AcyclicPointerChainKeepsItsScalarPointee) {
  for (auto Architecture : Architectures) {
    TypeGraph G(Architecture);
    LTSN *A = G.addPointer();
    LTSN *B = G.addPointer();
    LTSN *Scalar = G.addNode(8);
    G.TS.addPointerLink(A, B);
    G.TS.addPointerLink(B, Scalar);
    G.materialize();
    const auto
      *Pointee = llvm::dyn_cast<model::PrimitiveType>(G.pointeeAfter(A, 2));
    BOOST_REQUIRE(Pointee != nullptr);
    BOOST_CHECK(Pointee->PrimitiveKind() == model::PrimitiveKind::Generic);
    BOOST_CHECK_EQUAL(Pointee->Size(), 8U);
    BOOST_CHECK(*G.type(B)
                == *model::PointerType::make(G.type(Scalar).copy(),
                                             Architecture));
    BOOST_CHECK(G.Model->TypeDefinitions().empty());
  }
}

BOOST_AUTO_TEST_CASE(RecursiveStructKeepsItsDefinitionAnchor) {
  for (auto Architecture : Architectures) {
    TypeGraph G(Architecture);
    LTSN *StructNode = G.addNode(G.PointerSize);
    StructNode->NonScalar = true;
    LTSN *PointerNode = G.addPointer();
    G.TS.addInstanceLink(StructNode, PointerNode, dla::OffsetExpression{});
    G.TS.addPointerLink(PointerNode, StructNode);
    G.materialize();
    BOOST_REQUIRE_EQUAL(G.Model->TypeDefinitions().size(), 1U);
    const auto *Struct = llvm::dyn_cast<
      model::StructDefinition>(G.type(StructNode)->tryGetAsDefinition());
    BOOST_REQUIRE(Struct != nullptr);
    BOOST_REQUIRE_EQUAL(Struct->Fields().size(), 1U);
    const auto *FieldPointer = llvm::dyn_cast<
      model::PointerType>(Struct->Fields().at(0).Type().get());
    BOOST_REQUIRE(FieldPointer != nullptr);
    BOOST_CHECK_EQUAL(FieldPointer->PointerSize(), G.PointerSize);
    BOOST_CHECK(FieldPointer->PointeeType()->tryGetAsDefinition() == Struct);
    BOOST_CHECK(G.pointeeAfter(PointerNode, 1)->tryGetAsDefinition() == Struct);
  }
}
