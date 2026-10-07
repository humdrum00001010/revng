//
// This file is distributed under the MIT License. See LICENSE.md for details.
//

#define BOOST_TEST_MODULE SegregateStackAccesses
bool init_unit_test();
#include "boost/test/unit_test.hpp"

#include "llvm/IR/DIBuilder.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/InstIterator.h"
#include "llvm/IR/Module.h"

#include "revng/ABI/ModelHelpers.h"
#include "revng/Model/PrimitiveType.h"
#include "revng/Model/RawFunctionDefinition.h"
#include "revng/PromoteStackPointer/Markers.h"
#include "revng/PromoteStackPointer/SegregateStackAccesses.h"
#include "revng/Ranks/Location.h"
#include "revng/Ranks/Ranks.h"

using namespace llvm;
using namespace revng::pypeline;

namespace {

struct ReturnRegister {
  model::Register::Values Register;
  unsigned ModelBytes;
  unsigned CarrierBits;
};

void addReturns(model::RawFunctionDefinition &Prototype,
                ArrayRef<ReturnRegister> Registers) {
  Prototype.Architecture() = model::Architecture::x86_64;
  Prototype.FinalStackOffset() = 8;
  for (const ReturnRegister &Register : Registers)
    Prototype
      .addReturnValue(Register.Register,
                      model::PrimitiveType::makeGeneric(Register.ModelBytes));
}

void checkRegisterSet(Type *ReturnType, ArrayRef<ReturnRegister> Registers) {
  auto *Struct = dyn_cast<StructType>(ReturnType);
  BOOST_REQUIRE(Struct != nullptr);
  BOOST_CHECK(Struct->isPacked());
  BOOST_REQUIRE_EQUAL(Struct->getNumElements(), Registers.size());
  for (auto [Index, Register] : enumerate(Registers))
    BOOST_CHECK(Struct->getElementType(Index)
                  ->isIntegerTy(Register.CarrierBits));
}

void checkLayout(ArrayRef<ReturnRegister> Registers) {
  model::RawFunctionDefinition Prototype;
  Prototype.ID() = 0;
  addReturns(Prototype, Registers);
  BOOST_REQUIRE(Prototype.verify(true));
  auto Layout = abi::FunctionType::Layout::make(Prototype);
  LLVMContext Context;
  auto &FunctionType = layoutToLLVMFunctionType<false>(Context,
                                                       Prototype.Architecture(),
                                                       Layout);
  checkRegisterSet(FunctionType.getReturnType(), Registers);
}

struct Fixture {
  Model TheModel;
  LLVMFunctionContainer Container;
  Module M{ "segregate-stack-accesses", Container.getContext() };
  const MetaAddress CallerAddress{ 0x1000, MetaAddressType::Code_x86_64 };
  const MetaAddress CalleeAddress{ 0x2000, MetaAddressType::Code_x86_64 };

  Fixture() {
    model::Binary &Binary = *TheModel.get();
    Binary.Architecture() = model::Architecture::x86_64;
    auto &Segment = Binary.Segments()[MetaAddress(0x1000,
                                                  MetaAddressType::Generic64)];
    Segment.VirtualSize() = 0x2000;
    Segment.FileSize() = Segment.VirtualSize();
    Segment.IsExecutable() = true;
    M.setDataLayout("e-m:e-i64:64-f80:128-n8:16:32:64-S128");
    M.setTargetTriple("x86_64-pc-linux-gnu");
  }

  model::TypeDefinition::Key addPrototype(MetaAddress Address,
                                          ArrayRef<ReturnRegister> Registers) {
    auto &&[Prototype, Type] = TheModel.get()->makeRawFunctionDefinition();
    addReturns(Prototype, Registers);
    auto Key = Prototype.key();
    TheModel.get()->Functions()[Address].Prototype() = std::move(Type);
    return Key;
  }

  Function *addFunction(StringRef Name, MetaAddress Address, Type *ReturnType) {
    auto *Result = Function::Create(FunctionType::get(ReturnType, false),
                                    GlobalValue::ExternalLinkage,
                                    Name,
                                    M);
    setMetaAddressMetadata(Result, FunctionEntryMDName, Address);
    FunctionTags::Isolated.addTo(Result);
    return Result;
  }

  DebugLoc location() {
    DIBuilder DIB(M);
    DIFile *File = DIB.createFile("synthetic.c", "./");
    DIB.createCompileUnit(dwarf::DW_LANG_C, File, "revng", true, "", 0);
    auto *Signature = DIB.createSubroutineType(DIB.getOrCreateTypeArray({}));
    auto Name = locationString(revng::ranks::Instruction,
                               CallerAddress,
                               BasicBlockID(CallerAddress),
                               CallerAddress);
    auto Flags = DISubprogram::toSPFlags(false, true, false);
    auto *Scope = DIB.createFunction(File,
                                     Name,
                                     "",
                                     File,
                                     1,
                                     Signature,
                                     1,
                                     DINode::FlagPrototyped,
                                     Flags);
    auto *InlinedAt = DILocation::get(M.getContext(), 0, 0, Scope);
    DebugLoc Result = DILocation::get(M.getContext(), 0, 0, Scope, InlinedAt);
    DIB.finalize();
    return Result;
  }

  StructType *carrierType(ArrayRef<ReturnRegister> Registers) {
    SmallVector<Type *> Fields;
    for (const ReturnRegister &Register : Registers)
      Fields.push_back(IntegerType::get(M.getContext(), Register.CarrierBits));
    return StructType::get(M.getContext(), Fields, true);
  }

  Function &run(Function &Caller) {
    BOOST_REQUIRE(TheModel.get()->verify(true));
    revng::forceVerify(&M);
    piperuns::SegregateStackAccesses Pass(TheModel, "", "", Container);
    Pass.runOnLLVMFunction(TheModel.get()->Functions().at(CallerAddress),
                           Caller);
    revng::forceVerify(&M);
    return *M.getFunction("caller");
  }

  void checkCall(ArrayRef<ReturnRegister> Registers) {
    addPrototype(CallerAddress, {});
    auto CalleeKey = addPrototype(CalleeAddress, Registers);
    auto *ReturnType = carrierType(Registers);
    auto *Callee = addFunction("callee", CalleeAddress, ReturnType);
    auto *Caller = addFunction("caller",
                               CallerAddress,
                               Type::getVoidTy(M.getContext()));
    auto *Entry = BasicBlock::Create(M.getContext(), "entry", Caller);
    IRBuilder<> Builder(Entry);
    Builder.SetCurrentDebugLocation(location());
    auto *MarkerType = FunctionType::get(Builder.getVoidTy(),
                                         { Builder.getInt64Ty() },
                                         false);
    auto *Marker = StackSizeAtCallSite.getOrCreate(M, MarkerType).function();
    auto *MarkerCall = Builder.CreateCall(Marker, { Builder.getInt64(0) });
    auto Prototype = TheModel.get()
                       ->getTypeDefinitionReference(CalleeKey)
                       .toString();
    setStringMetadata(MarkerCall, PrototypeMDName, Prototype);
    auto *Call = Builder.CreateCall(Callee);
    setStringMetadata(Call, PrototypeMDName, Prototype);
    SmallVector<CallInst *> ExtractConsumers;
    SmallVector<StoreInst *> StoreConsumers;

    // Keep both return registers live through the opaque extraction helpers
    // used in the isolated input to this pipe.
    for (unsigned Index = 0; Index < Registers.size(); ++Index) {
      Type *FieldType = ReturnType->getElementType(Index);
      auto *ExtractType = FunctionType::get(FieldType,
                                            { ReturnType,
                                              Builder.getInt64Ty() },
                                            false);
      auto *Extract = Function::Create(ExtractType,
                                       GlobalValue::ExternalLinkage,
                                       "extract",
                                       M);
      FunctionTags::OpaqueExtractValue.addTo(Extract);
      auto *Value = Builder.CreateCall(Extract,
                                       { Call, Builder.getInt64(Index) });
      ExtractConsumers.push_back(Value);
      auto *Output = new GlobalVariable(M,
                                        FieldType,
                                        false,
                                        GlobalValue::ExternalLinkage,
                                        nullptr,
                                        "result");
      StoreConsumers.push_back(Builder.CreateStore(Value, Output));
    }
    Builder.CreateRetVoid();

    Function &Result = run(*Caller);
    unsigned Calls = 0;
    unsigned Extracts = 0;
    unsigned Stores = 0;
    CallInst *IsolatedCall = nullptr;
    for (Instruction &I : instructions(Result)) {
      if (auto *Call = getCallToIsolatedFunction(&I)) {
        BOOST_REQUIRE(Call->getCalledFunction() != nullptr);
        BOOST_CHECK_EQUAL(Call->getCalledFunction()->getName().str(), "callee");
        checkRegisterSet(Call->getType(), Registers);
        IsolatedCall = Call;
        ++Calls;
      }
      if (auto *Extract = getCallToTagged(&I,
                                          FunctionTags::OpaqueExtractValue)) {
        auto Index = cast<ConstantInt>(Extract->getArgOperand(1))
                       ->getZExtValue();
        BOOST_REQUIRE_LT(Index, Registers.size());
        BOOST_CHECK(Extract->getType()
                      ->isIntegerTy(Registers[Index].CarrierBits));
        checkRegisterSet(Extract->getArgOperand(0)->getType(), Registers);
        ++Extracts;
      }
      if (isa<StoreInst>(I))
        ++Stores;
    }
    BOOST_CHECK_EQUAL(Calls, 1);
    BOOST_CHECK_EQUAL(Extracts, Registers.size());
    BOOST_CHECK_EQUAL(Stores, Registers.size());
    BOOST_REQUIRE(IsolatedCall != nullptr);
    for (unsigned Index = 0; Index < Registers.size(); ++Index) {
      BOOST_CHECK(ExtractConsumers[Index]->getArgOperand(0) == IsolatedCall);
      BOOST_CHECK(StoreConsumers[Index]->getValueOperand()
                  == ExtractConsumers[Index]);
    }
    checkRegisterSet(M.getFunction("callee")->getReturnType(), Registers);
  }

  void checkReturn(ArrayRef<ReturnRegister> Registers) {
    addPrototype(CallerAddress, Registers);
    auto *ReturnType = carrierType(Registers);
    auto *Caller = addFunction("caller", CallerAddress, ReturnType);
    auto *Entry = BasicBlock::Create(M.getContext(), "entry", Caller);
    IRBuilder<> Builder(Entry);
    SmallVector<Type *> Types(ReturnType->elements());
    auto *Initializer = Function::Create(FunctionType::get(ReturnType,
                                                           Types,
                                                           false),
                                         GlobalValue::ExternalLinkage,
                                         "struct_initializer",
                                         M);
    FunctionTags::StructInitializer.addTo(Initializer);
    SmallVector<Value *> Values;
    for (Type *Type : Types)
      Values.push_back(ConstantInt::get(Type, 1));
    Builder.CreateRet(Builder.CreateCall(Initializer, Values));

    Function &Result = run(*Caller);
    checkRegisterSet(Result.getReturnType(), Registers);
    auto *Return = cast<ReturnInst>(Result.getEntryBlock().getTerminator());
    checkRegisterSet(Return->getReturnValue()->getType(), Registers);
  }
};

} // namespace

BOOST_AUTO_TEST_CASE(PartialVectorRegisterSetLayout) {
  for (unsigned Bytes : { 16, 32, 64 })
    checkLayout({ { model::Register::rax_x86_64, 8, 64 },
                  { model::Register::zmm0_x86_64, Bytes, Bytes * 8 } });
}

BOOST_AUTO_TEST_CASE(SmallGPRRegisterSetLayoutKeepsFullCarrier) {
  checkLayout({ { model::Register::rax_x86_64, 4, 64 },
                { model::Register::rdx_x86_64, 8, 64 } });
}

BOOST_AUTO_TEST_CASE(SmallScalarReturnLayoutUsesModelWidth) {
  model::RawFunctionDefinition Prototype;
  Prototype.ID() = 0;
  addReturns(Prototype, { { model::Register::rax_x86_64, 4, 64 } });
  auto Layout = abi::FunctionType::Layout::make(Prototype);
  LLVMContext Context;
  auto &Type = layoutToLLVMFunctionType<false>(Context,
                                               Prototype.Architecture(),
                                               Layout);
  BOOST_CHECK(Type.getReturnType()->isIntegerTy(32));
}

BOOST_AUTO_TEST_CASE(PartialVectorCallKeepsLiveRegisterConsumers) {
  Fixture F;
  F.checkCall({ { model::Register::rax_x86_64, 8, 64 },
                { model::Register::zmm0_x86_64, 16, 128 } });
}

BOOST_AUTO_TEST_CASE(SmallGPRCallKeepsFullRegisterConsumers) {
  Fixture F;
  F.checkCall({ { model::Register::rax_x86_64, 4, 64 },
                { model::Register::rdx_x86_64, 8, 64 } });
}

BOOST_AUTO_TEST_CASE(PartialVectorFunctionReturnKeepsCarrierType) {
  Fixture F;
  F.checkReturn({ { model::Register::rax_x86_64, 8, 64 },
                  { model::Register::zmm0_x86_64, 16, 128 } });
}

BOOST_AUTO_TEST_CASE(SmallGPRFunctionReturnKeepsFullCarrierType) {
  Fixture F;
  F.checkReturn({ { model::Register::rax_x86_64, 4, 64 },
                  { model::Register::rdx_x86_64, 8, 64 } });
}
