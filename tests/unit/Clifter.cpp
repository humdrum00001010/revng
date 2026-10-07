//
// This file is distributed under the MIT License. See LICENSE.md for details.
//

#define BOOST_TEST_MODULE Clifter
bool init_unit_test();
#include "boost/test/unit_test.hpp"

#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Module.h"

#include "mlir/IR/Verifier.h"

#include "revng/Clifter/Clifter.h"
#include "revng/Model/CABIFunctionDefinition.h"
#include "revng/Model/IRHelpers.h"
#include "revng/Model/PointerType.h"
#include "revng/Model/PrimitiveType.h"
#include "revng/Model/StructDefinition.h"
#include "revng/Model/TypedefDefinition.h"

BOOST_AUTO_TEST_CASE(ConstQualifiedCallArgument) {
  TupleTree<model::Binary> Model;
  Model->Architecture() = model::Architecture::x86;
  auto &Segment = Model->Segments()[MetaAddress(0x1000,
                                                MetaAddressType::Generic32)];
  Segment.VirtualSize() = 0x2000;
  Segment.FileSize() = Segment.VirtualSize();
  Segment.IsExecutable() = true;

  // The parameter is a const-qualified typedef of a pointer to a const struct.
  // Its top-level qualifier belongs to the parameter object, while the
  // pointee's qualifier must survive conversion of the argument value.
  auto &&[Struct, StructType] = Model->makeStructDefinition();
  Struct.Size() = 4;
  Struct.Fields()[0].Type() = model::PrimitiveType::makeUnsigned(4);
  StructType->IsConst() = true;
  auto PointerType = model::PointerType::make(std::move(StructType), 4);
  auto &&[Alias, AliasType] = Model->makeTypedefDefinition();
  Alias.UnderlyingType() = std::move(PointerType);
  AliasType->IsConst() = true;

  auto &&[CalleePrototype, CalleeType] = Model->makeCABIFunctionDefinition();
  CalleePrototype.ABI() = model::ABI::SystemV_x86;
  CalleePrototype.addArgument(std::move(AliasType));
  auto CalleePrototypeKey = CalleePrototype.key();
  MetaAddress CalleeAddress(0x2000, MetaAddressType::Code_x86);
  Model->Functions()[CalleeAddress].Prototype() = std::move(CalleeType);

  auto &&[CallerPrototype, CallerType] = Model->makeCABIFunctionDefinition();
  CallerPrototype.ABI() = model::ABI::SystemV_x86;
  MetaAddress CallerAddress(0x1000, MetaAddressType::Code_x86);
  Model->Functions()[CallerAddress].Prototype() = std::move(CallerType);
  BOOST_REQUIRE(Model->verify(true));

  llvm::LLVMContext LLVMContext;
  llvm::Module LLVMModule("const-call-argument", LLVMContext);
  LLVMModule.setDataLayout("e-p:32:32-i64:64-n8:16:32-S128");
  LLVMModule.setTargetTriple("i386-pc-linux-gnu");
  auto *Void = llvm::Type::getVoidTy(LLVMContext);
  auto *I32 = llvm::Type::getInt32Ty(LLVMContext);
  auto *Callee = llvm::Function::Create(llvm::FunctionType::get(Void,
                                                                { I32 },
                                                                false),
                                        llvm::GlobalValue::ExternalLinkage,
                                        "callee",
                                        LLVMModule);
  setMetaAddressMetadata(Callee, FunctionEntryMDName, CalleeAddress);
  auto *Caller = llvm::Function::Create(llvm::FunctionType::get(Void, false),
                                        llvm::GlobalValue::ExternalLinkage,
                                        "caller",
                                        LLVMModule);
  setMetaAddressMetadata(Caller, FunctionEntryMDName, CallerAddress);
  auto *Entry = llvm::BasicBlock::Create(LLVMContext, "entry", Caller);
  llvm::IRBuilder<> Builder(Entry);
  auto *Call = Builder.CreateCall(Callee, { Builder.getInt32(0) });
  setStringMetadata(Call,
                    PrototypeMDName,
                    Model->getTypeDefinitionReference(CalleePrototypeKey)
                      .toString());
  Builder.CreateRetVoid();

  mlir::MLIRContext Context;
  Context.loadDialect<clift::CliftDialect>();
  auto Module = clift::makeModule(&Context);
  auto Importer = clift::Clifter::make(*Module, *Model);
  auto Function = Importer->import(Caller);

  clift::CallOp ImportedCall;
  Function.walk([&](clift::CallOp Op) { ImportedCall = Op; });
  BOOST_REQUIRE(static_cast<bool>(ImportedCall));
  auto ParameterType = ImportedCall.getFunctionType().getArgumentTypes()[0];
  BOOST_CHECK(clift::isConst(ParameterType));

  auto Cast = ImportedCall.getArguments()[0].getDefiningOp<clift::BitCastOp>();
  BOOST_REQUIRE(static_cast<bool>(Cast));
  BOOST_CHECK(Cast.getType() == clift::removeConst(ParameterType));
  BOOST_CHECK(not clift::isConst(Cast.getType()));
  BOOST_CHECK(mlir::isa<clift::TypedefType>(Cast.getType()));
  auto Pointer = clift::unwrapped_cast<clift::PointerType>(Cast.getType());
  BOOST_CHECK_EQUAL(Pointer.getPointerSize(), 4);
  BOOST_CHECK(clift::isConst(Pointer.getPointeeType()));
  BOOST_CHECK(clift::unwrapped_isa<clift::StructType>(Pointer
                                                        .getPointeeType()));
  BOOST_CHECK(mlir::succeeded(mlir::verify(*Module)));
}
