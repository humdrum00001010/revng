//
// This file is distributed under the MIT License. See LICENSE.md for details.
//

#define BOOST_TEST_MODULE FunctionIsolation
bool init_unit_test();
#include <set>

#include "boost/test/unit_test.hpp"

#include "llvm/IR/InstIterator.h"

#include "revng/EarlyFunctionAnalysis/CallEdge.h"
#include "revng/EarlyFunctionAnalysis/FunctionEdge.h"
#include "revng/FunctionIsolation/IsolateFunctions.h"
#include "revng/Model/IRHelpers.h"
#include "revng/Support/BlockType.h"
#include "revng/Support/FunctionCallMarker.h"
#include "revng/Support/NewPC.h"

using namespace llvm;
using namespace revng::pypeline;

BOOST_AUTO_TEST_CASE(CallInsideFunctionInlinedTwice) {
  BOOST_TEST_CHECKPOINT("Create the synthetic model");
  Model TheModel;
  model::Binary &Binary = *TheModel.get();
  Binary.Architecture() = model::Architecture::x86_64;

  auto Address = [](uint64_t Value) {
    return MetaAddress(Value, MetaAddressType::Code_x86_64);
  };
  MetaAddress Outer = Address(0x1000);
  MetaAddress SecondCall = Address(0x1005);
  MetaAddress OuterReturn = Address(0x100a);
  MetaAddress Inlinee = Address(0x2000);
  MetaAddress InlineeReturn = Address(0x2005);
  MetaAddress Callee = Address(0x3000);
  Binary.Functions()[Outer];
  Binary.Functions()[Inlinee]
    .Attributes()
    .insert(model::FunctionAttribute::HasOneBrokenReturn);
  Binary.Functions()[Callee];

  BOOST_TEST_CHECKPOINT("Create the input LLVM module");
  LLVMRootContainer Input;
  Module &M = Input.getModule();
  BOOST_TEST_CHECKPOINT("Configure the input LLVM module");
  LLVMContext &Context = M.getContext();
  M.setDataLayout("e-m:e-i64:64-f80:128-n8:16:32:64-S128");
  M.setTargetTriple("x86_64-pc-linux-gnu");

  BOOST_TEST_CHECKPOINT("Create the input register globals");
  auto AddGlobal = [&](StringRef Name, unsigned Bits) {
    auto *Type = IntegerType::get(Context, Bits);
    return new GlobalVariable(M,
                              Type,
                              false,
                              GlobalValue::ExternalLinkage,
                              ConstantInt::get(Type, 0),
                              Name);
  };
  AddGlobal(model::Architecture::getPCCSVName(Binary.Architecture()), 64);
  auto StackPointer = model::Architecture::getStackPointer(Binary
                                                             .Architecture());
  AddGlobal(model::Register::getCSVName(StackPointer), 64);
  AddGlobal("pc_epoch", 32);
  AddGlobal("pc_address_space", 16);
  AddGlobal("pc_type", 16);

  BOOST_TEST_CHECKPOINT("Create the input IR helpers");
  auto *Void = Type::getVoidTy(Context);
  auto *Pointer = Type::getInt8PtrTy(Context);
  auto *I64 = Type::getInt64Ty(Context);
  auto *NewPCType = FunctionType::get(Void,
                                      { Pointer, I64, I64, Pointer },
                                      false);
  Function *NewPC = NewPCHelper
                      .create(M, NewPCType, GlobalValue::ExternalLinkage)
                      .function();
  auto *CallType = FunctionType::get(Void,
                                     { Pointer, Pointer, Pointer, Pointer },
                                     false);
  Function *CallMarker = FunctionCallMarker
                           .create(M, CallType, GlobalValue::ExternalLinkage)
                           .function();
  FunctionTags::Marker.addTo(CallMarker);

  BOOST_TEST_CHECKPOINT("Create the input root function");
  auto *Root = Function::Create(FunctionType::get(Void, false),
                                GlobalValue::ExternalLinkage,
                                "root",
                                M);
  auto AddBlock = [&](StringRef Name) {
    return BasicBlock::Create(Context, Name, Root);
  };
  auto *Entry = AddBlock("entry");
  auto *Dispatcher = AddBlock("dispatcher");
  auto *AnyPC = AddBlock("anypc");
  auto *UnexpectedPC = AddBlock("unexpectedpc");
  auto *First = AddBlock("first_call");
  auto *Second = AddBlock("second_call");
  auto *Return = AddBlock("outer_return");
  auto *Inner = AddBlock("inlinee_call");
  auto *InnerReturn = AddBlock("inlinee_return");
  auto *Called = AddBlock("callee");

  BranchInst::Create(Dispatcher, Entry);
  setBlockType(BranchInst::Create(First, Dispatcher),
               BlockType::RootDispatcherBlock);
  setBlockType(BranchInst::Create(Dispatcher, AnyPC), BlockType::AnyPCBlock);
  setBlockType(BranchInst::Create(Dispatcher, UnexpectedPC),
               BlockType::UnexpectedPCBlock);

  auto MarkPC = [&](BasicBlock *BB, MetaAddress PC) {
    revng::IRBuilder Builder(BB);
    Builder.CreateCall(NewPC,
                       { BasicBlockID(PC).toValue(&M),
                         Builder.getInt64(5),
                         Builder.getInt64(1),
                         MetaAddress::invalid().toValue(&M) });
  };
  auto MarkCall = [&](BasicBlock *BB,
                      BasicBlock *Target,
                      BasicBlock *Fallthrough,
                      MetaAddress FallthroughPC) {
    revng::IRBuilder Builder(BB);
    Builder.CreateCall(CallMarker,
                       { BlockAddress::get(Target),
                         BlockAddress::get(Fallthrough),
                         FallthroughPC.toValue(&M),
                         ConstantPointerNull::get(Pointer) });
    Builder.CreateBr(Target);
  };
  MarkPC(First, Outer);
  MarkCall(First, Inner, Second, SecondCall);
  MarkPC(Second, SecondCall);
  MarkCall(Second, Inner, Return, OuterReturn);
  MarkPC(Return, OuterReturn);
  BranchInst::Create(AnyPC, Return);
  MarkPC(Inner, Inlinee);
  MarkCall(Inner, Called, InnerReturn, InlineeReturn);
  MarkPC(InnerReturn, InlineeReturn);
  BranchInst::Create(AnyPC, InnerReturn);
  MarkPC(Called, Callee);
  BranchInst::Create(AnyPC, Called);
  revng::forceVerify(&M);

  // Only the requested outer function's CFG is available. The inlinee is
  // represented by two distinct inlining indices in that CFG, and has no
  // standalone object in the input container.
  BOOST_TEST_CHECKPOINT("Create the outer function CFG");
  CFGMap CFG;
  efa::ControlFlowGraph &FM = CFG.getElement(ObjectID(Outer))->MainFunction();
  FM.Entry() = Outer;
  auto AddCFGBlock = [&](MetaAddress Start, uint64_t Index, MetaAddress End) {
    efa::BasicBlock Block(BasicBlockID(Start, Index));
    Block.End() = End;
    if (Index != 0)
      Block.InlinedFrom() = Inlinee;
    FM.Blocks().insert(Block);
  };
  AddCFGBlock(Outer, 0, SecondCall);
  AddCFGBlock(SecondCall, 0, OuterReturn);
  AddCFGBlock(OuterReturn, 0, OuterReturn + 5);
  for (uint64_t Index : { 1, 2 }) {
    AddCFGBlock(Inlinee, Index, InlineeReturn);
    AddCFGBlock(InlineeReturn, Index, InlineeReturn + 5);
    auto Edge = UpcastablePointer<efa::FunctionEdgeBase>::make<
      efa::CallEdge>(BasicBlockID(Callee), efa::FunctionEdgeType::FunctionCall);
    FM.Blocks().at(BasicBlockID(Inlinee, Index)).Successors().insert(Edge);
  }
  auto AddBranch = [&](BasicBlockID Source, BasicBlockID Destination) {
    auto Edge = UpcastablePointer<efa::FunctionEdgeBase>::make<
      efa::FunctionEdge>(Destination, efa::FunctionEdgeType::DirectBranch);
    FM.Blocks().at(Source).Successors().insert(Edge);
  };
  AddBranch(BasicBlockID(Outer), BasicBlockID(Inlinee, 1));
  AddBranch(BasicBlockID(InlineeReturn, 1), BasicBlockID(SecondCall));
  AddBranch(BasicBlockID(SecondCall), BasicBlockID(Inlinee, 2));
  AddBranch(BasicBlockID(InlineeReturn, 2), BasicBlockID(OuterReturn));
  auto ReturnEdge = UpcastablePointer<efa::FunctionEdgeBase>::make<
    efa::FunctionEdge>(BasicBlockID::invalid(), efa::FunctionEdgeType::Return);
  FM.Blocks().at(BasicBlockID(OuterReturn)).Successors().insert(ReturnEdge);

  LLVMFunctionContainer Output;
  {
    BOOST_TEST_CHECKPOINT("Construct the isolation pipe");
    piperuns::Isolate Isolator(TheModel, "", "", CFG, Input, Output);
    BOOST_TEST_CHECKPOINT("Isolate the outer function");
    Isolator.runOnFunction(Binary.Functions().at(Outer));
    BOOST_TEST_CHECKPOINT("Split the isolated function into its output module");
  }

  BOOST_TEST_CHECKPOINT("Check the isolated calls and inlining indices");
  BOOST_REQUIRE_EQUAL(Output.objects().size(), 1);
  const Module &Result = Output.getModule(ObjectID(Outer));
  Function *Isolated = Result
                         .getFunction(llvmName(Binary.Functions().at(Outer)));
  BOOST_REQUIRE(Isolated != nullptr);
  BOOST_CHECK(not Isolated->isDeclaration());
  auto CalleeName = llvmName(Binary.Functions().at(Callee));
  unsigned Calls = 0;
  std::set<uint64_t> InliningIndices;
  for (const Instruction &I : instructions(Isolated)) {
    if (const auto *Call = dyn_cast<CallInst>(&I)) {
      const Function *Target = Call->getCalledFunction();
      if (Target != nullptr and Target->getName() == CalleeName)
        ++Calls;
    }
    if (auto PC = NewPCHelper.getCall(&I)) {
      BasicBlockID ID = blockIDFromNewPC(*PC);
      if (ID.start() == Inlinee)
        InliningIndices.insert(ID.inliningIndex());
    }
  }
  BOOST_CHECK_EQUAL(Calls, 2);
  BOOST_CHECK(InliningIndices == std::set<uint64_t>({ 1, 2 }));
  BOOST_CHECK(Result.getFunction("isolated_call_marker") == nullptr);
  BOOST_CHECK(Result.getFunction(llvmName(Binary.Functions().at(Inlinee)))
              == nullptr);
  revng::forceVerify(&Result);
}
