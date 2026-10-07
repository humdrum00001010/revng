//
// This file is distributed under the MIT License. See LICENSE.md for details.
//

#define BOOST_TEST_MODULE DetectABIRegisters
bool init_unit_test();
#include "boost/test/unit_test.hpp"

#include <set>

#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/GlobalVariable.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"

#include "revng/Model/Binary.h"
#include "revng/Model/PrimitiveType.h"
#include "revng/Model/RawFunctionDefinition.h"
#include "revng/Model/Register.h"

#include "lib/EarlyFunctionAnalysis/RecordRegisters.h"

namespace {

struct Fixture {
  llvm::LLVMContext Context;
  llvm::Module Module{ "detect-abi-registers", Context };

  std::set<llvm::GlobalVariable *>
  makeCSVs(model::Register::Values Register, unsigned Count) {
    std::set<llvm::GlobalVariable *> Result;
    for (const auto &CSV : model::Register::getCSVs(Register)) {
      if (Result.size() == Count)
        break;
      auto *Type = llvm::IntegerType::get(Context, CSV.Size * 8);
      Result.insert(new llvm::GlobalVariable(Module,
                                             Type,
                                             false,
                                             llvm::GlobalValue::ExternalLinkage,
                                             nullptr,
                                             CSV.Name));
    }
    return Result;
  }
};

void checkType(const model::UpcastableType &Type, uint64_t Size) {
  BOOST_REQUIRE(not Type.isEmpty());
  const auto *Primitive = llvm::dyn_cast<model::PrimitiveType>(Type.get());
  BOOST_REQUIRE(Primitive != nullptr);
  BOOST_TEST(Primitive->PrimitiveKind() == model::PrimitiveKind::Generic);
  BOOST_TEST(Primitive->Size() == Size);
  BOOST_TEST(Primitive->isValid());
}

} // namespace

BOOST_AUTO_TEST_CASE(X87ArgumentsAndReturnsCoverAllTenBytes) {
  Fixture F;
  auto CSVs = F.makeCSVs(model::Register::st0_x86, 1);
  BOOST_REQUIRE_EQUAL(CSVs.size(), 1);

  // Exercise the same recording helper and output containers used by both
  // direct-function and indirect-call prototype finalization.
  model::RawFunctionDefinition Prototype;
  efa::recordRegisters(CSVs,
                       model::Architecture::x86,
                       Prototype.Arguments().batch_insert());
  efa::recordRegisters(CSVs,
                       model::Architecture::x86,
                       Prototype.ReturnValues().batch_insert());

  BOOST_REQUIRE_EQUAL(Prototype.Arguments().size(), 1);
  BOOST_REQUIRE_EQUAL(Prototype.ReturnValues().size(), 1);
  checkType(Prototype.Arguments().at(model::Register::st0_x86).Type(), 10);
  checkType(Prototype.ReturnValues().at(model::Register::st0_x86).Type(), 10);
}

BOOST_AUTO_TEST_CASE(PartialVectorStillRoundsUpWithinTheRegister) {
  Fixture F;
  auto CSVs = F.makeCSVs(model::Register::zmm0_x86_64, 3);
  BOOST_REQUIRE_EQUAL(CSVs.size(), 3);

  model::RawFunctionDefinition Prototype;
  efa::recordRegisters(CSVs,
                       model::Architecture::x86_64,
                       Prototype.Arguments().batch_insert());

  BOOST_REQUIRE_EQUAL(Prototype.Arguments().size(), 1);
  // Three genuine 8-byte CSV portions cover 24 bytes, requiring 32 bytes.
  checkType(Prototype.Arguments().at(model::Register::zmm0_x86_64).Type(), 32);
}
