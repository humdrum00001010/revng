//
// This file is distributed under the MIT License. See LICENSE.md for details.
//

#define BOOST_TEST_MODULE PromoteGlobalToLocalVars
bool init_unit_test();
#include "boost/test/unit_test.hpp"

#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/AsmParser/Parser.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/InstIterator.h"
#include "llvm/IR/Verifier.h"
#include "llvm/Support/SourceMgr.h"

#include "revng/EarlyFunctionAnalysis/PromoteGlobalToLocalVars.h"
#include "revng/UnitTestHelpers/LLVMTestHelpers.h"
#include "revng/UnitTestHelpers/UnitTestHelpers.h"

using namespace llvm;

static const char *ModuleText = R"LLVM(
  @zeta = global i64 22
  @alpha = global i64 11
  @excluded = global i64 33
  @call_only = global i64 44
  @published_address = global i64 ptrtoint (ptr @alpha to i64)

  declare void @consume(ptr, ptr, ptr, ptr, ptr, i64, i64, i64, i64)

  define i64 @current() {
  entry:
    %alpha_pointer = addrspacecast ptr @alpha to ptr addrspace(1)
    %alpha_load = load i64, ptr addrspace(1) %alpha_pointer
    %cast_load = load i64, ptr addrspace(1) addrspacecast (ptr @alpha to ptr addrspace(1))
    store i64 %cast_load, ptr @zeta
    %excluded_load = load i64, ptr @excluded
    call void @consume(ptr @alpha, ptr @zeta, ptr @excluded, ptr @call_only,
                       ptr getelementptr (i64, ptr @alpha, i64 1),
                       i64 ptrtoint (ptr @alpha to i64),
                       i64 ptrtoint (ptr @alpha to i64),
                       i64 ptrtoint (ptr @excluded to i64),
                       i64 ptrtoint (ptr getelementptr (i64, ptr @alpha, i64 1) to i64))
    ret i64 %excluded_load
  }

  define void @other() {
  entry:
    %cast_load = load i64, ptr addrspace(1) addrspacecast (ptr @alpha to ptr addrspace(1))
    store i64 %cast_load, ptr @zeta
    call void @consume(ptr @alpha, ptr @zeta, ptr @excluded, ptr @call_only,
                       ptr getelementptr (i64, ptr @alpha, i64 1),
                       i64 ptrtoint (ptr @alpha to i64),
                       i64 ptrtoint (ptr @alpha to i64),
                       i64 ptrtoint (ptr @excluded to i64),
                       i64 ptrtoint (ptr getelementptr (i64, ptr @alpha, i64 1) to i64))
    ret void
  }
)LLVM";

static CallInst *getCall(Function *F) {
  for (Instruction &I : instructions(F))
    if (auto *Call = dyn_cast<CallInst>(&I))
      return Call;
  return nullptr;
}

static std::string printFunction(Function *F) {
  std::string Result;
  raw_string_ostream Stream(Result);
  F->print(Stream);
  return Result;
}

static void checkPromotion(PromoteGlobalToLocalPass Pass,
                           bool PromoteExcluded) {
  LLVMContext Context;
  SMDiagnostic Diagnostic;
  std::unique_ptr<Module> M = parseAssemblyString(ModuleText,
                                                  Diagnostic,
                                                  Context);
  BOOST_REQUIRE(M != nullptr);
  BOOST_REQUIRE(not verifyModule(*M, &errs()));
  Function *Current = M->getFunction("current");
  Function *Other = M->getFunction("other");
  CallInst *Call = getCall(Current);
  CallInst *OtherCall = getCall(Other);
  BOOST_REQUIRE(Call != nullptr);
  BOOST_REQUIRE(OtherCall != nullptr);
  auto *AlphaPointer = cast<AddrSpaceCastInst>(instructionByName(Current,
                                                                 "alpha_"
                                                                 "pointer"));
  auto *CastLoad = cast<LoadInst>(instructionByName(Current, "cast_load"));
  auto *ExcludedLoad = cast<LoadInst>(instructionByName(Current,
                                                        "excluded_load"));
  auto *ZetaStore = cast<StoreInst>(CastLoad->getNextNode());

  // These expressions really are shared, both with another function and with a
  // global initializer. Rewriting one function must leave those uses intact.
  Value *SharedAddress = Call->getArgOperand(5);
  Value *SharedLoadCast = CastLoad->getPointerOperand();
  Value *NonCastExpression = Call->getArgOperand(4);
  Value *ExcludedAddress = Call->getArgOperand(7);
  Value *NestedExpression = Call->getArgOperand(8);
  auto *Published = M->getGlobalVariable("published_address");
  BOOST_REQUIRE(isa<ConstantExpr>(SharedAddress));
  BOOST_REQUIRE(isa<ConstantExpr>(SharedLoadCast));
  BOOST_REQUIRE(isa<ConstantExpr>(NonCastExpression));
  BOOST_CHECK(not cast<ConstantExpr>(NonCastExpression)->isCast());
  BOOST_CHECK(SharedAddress == Call->getArgOperand(6));
  BOOST_CHECK(SharedAddress == OtherCall->getArgOperand(5));
  BOOST_CHECK(SharedAddress == Published->getInitializer());
  BOOST_CHECK(SharedLoadCast
              == cast<LoadInst>(instructionByName(Other, "cast_load"))
                   ->getPointerOperand());
  std::string OtherBefore = printFunction(Other);

  FunctionAnalysisManager FAM;
  Pass.run(*Current, FAM);
  BOOST_CHECK(not verifyModule(*M, &errs()));
  BOOST_CHECK_EQUAL(printFunction(Other), OtherBefore);
  BOOST_CHECK(Published->getInitializer() == SharedAddress);

  auto *Alpha = cast<AllocaInst>(instructionByName(Current, "alpha"));
  auto *Zeta = cast<AllocaInst>(instructionByName(Current, "zeta"));
  AllocaInst *Excluded = nullptr;
  if (PromoteExcluded)
    Excluded = cast<AllocaInst>(instructionByName(Current, "excluded"));

  std::vector<std::string> AllocaNames;
  for (Instruction &I : instructions(Current))
    if (auto *Alloca = dyn_cast<AllocaInst>(&I))
      AllocaNames.push_back(Alloca->getName().str());
  std::vector<std::string> ExpectedNames = { "alpha", "zeta" };
  if (PromoteExcluded)
    ExpectedNames = { "alpha", "excluded", "zeta" };
  BOOST_CHECK_EQUAL_COLLECTIONS(AllocaNames.begin(),
                                AllocaNames.end(),
                                ExpectedNames.begin(),
                                ExpectedNames.end());

  BOOST_CHECK(Call->getArgOperand(0) == Alpha);
  BOOST_CHECK(Call->getArgOperand(1) == Zeta);
  BOOST_CHECK(AlphaPointer->getOperand(0) == Alpha);
  Value *ExpectedExcluded = PromoteExcluded ? static_cast<Value *>(Excluded) :
                                              M->getGlobalVariable("excluded");
  BOOST_CHECK(Call->getArgOperand(2) == ExpectedExcluded);
  BOOST_CHECK(ExcludedLoad->getPointerOperand() == ExpectedExcluded);
  BOOST_CHECK(Call->getArgOperand(3) == M->getGlobalVariable("call_only"));
  BOOST_CHECK(Call->getArgOperand(4) == NonCastExpression);
  BOOST_CHECK(Call->getArgOperand(8) == NestedExpression);
  BOOST_CHECK(ZetaStore->getPointerOperand() == Zeta);

  auto *LoadCast = dyn_cast<AddrSpaceCastInst>(CastLoad->getPointerOperand());
  BOOST_REQUIRE(LoadCast != nullptr);
  BOOST_CHECK(LoadCast->getOperand(0) == Alpha);
  BOOST_CHECK(LoadCast->getParent() == CastLoad->getParent());
  BOOST_CHECK(LoadCast->comesBefore(CastLoad));
  for (unsigned Index : { 5, 6 }) {
    auto *Cast = dyn_cast<PtrToIntInst>(Call->getArgOperand(Index));
    BOOST_REQUIRE(Cast != nullptr);
    BOOST_CHECK(Cast->getOperand(0) == Alpha);
    BOOST_CHECK(Cast->getParent() == Call->getParent());
    BOOST_CHECK(Cast->comesBefore(Call));
  }
  BOOST_CHECK(Call->getArgOperand(5) != Call->getArgOperand(6));
  if (PromoteExcluded) {
    auto *Cast = dyn_cast<PtrToIntInst>(Call->getArgOperand(7));
    BOOST_REQUIRE(Cast != nullptr);
    BOOST_CHECK(Cast->getOperand(0) == Excluded);
  } else {
    BOOST_CHECK(Call->getArgOperand(7) == ExcludedAddress);
  }

  // Every new local must be initialized from the original global, before the
  // original body accesses it. In particular, these loads must not be rewritten
  // into loads from the newly allocated, uninitialized locals.
  SmallPtrSet<AllocaInst *, 4> Initialized;
  for (Instruction &I : instructions(Current)) {
    auto *Store = dyn_cast<StoreInst>(&I);
    if (Store == nullptr or Store == ZetaStore)
      continue;
    auto *Local = dyn_cast<AllocaInst>(Store->getPointerOperand());
    BOOST_REQUIRE(Local != nullptr);
    auto *Load = dyn_cast<LoadInst>(Store->getValueOperand());
    BOOST_REQUIRE(Load != nullptr);
    BOOST_CHECK(Load->getPointerOperand()
                == M->getGlobalVariable(Local->getName()));
    BOOST_CHECK(Store->comesBefore(AlphaPointer));
    BOOST_CHECK(Initialized.insert(Local).second);
  }
  BOOST_CHECK_EQUAL(Initialized.size(), ExpectedNames.size());
}

BOOST_AUTO_TEST_CASE(SharedGlobalsAndConstantExpressions) {
  checkPromotion(PromoteGlobalToLocalPass(), true);
}

BOOST_AUTO_TEST_CASE(FilterRestrictsAllReplacements) {
  auto Filter = [](const GlobalVariable &CSV) {
    return CSV.getName() != "excluded";
  };
  checkPromotion(PromoteGlobalToLocalPass(Filter), false);
}

BOOST_AUTO_TEST_CASE(FunctionLocalReplacementHelper) {
  LLVMContext Context;
  SMDiagnostic Diagnostic;
  std::unique_ptr<Module> M = parseAssemblyString(ModuleText,
                                                  Diagnostic,
                                                  Context);
  BOOST_REQUIRE(M != nullptr);
  Function *Current = M->getFunction("current");
  Function *Other = M->getFunction("other");
  CallInst *Call = getCall(Current);
  CallInst *OtherCall = getCall(Other);
  auto *Old = M->getGlobalVariable("alpha");
  auto *Published = M->getGlobalVariable("published_address");
  IRBuilder<> Builder(&Current->getEntryBlock().front());
  auto *Replacement = Builder.CreateAlloca(Old->getValueType());
  Value *SharedAddress = Call->getArgOperand(5);
  Value *NonCastExpression = Call->getArgOperand(4);
  Value *NestedExpression = Call->getArgOperand(8);
  Value *ExcludedAddress = Call->getArgOperand(7);
  std::string CurrentBefore = printFunction(Current);
  std::string OtherBefore = printFunction(Other);
  BOOST_REQUIRE(SharedAddress == OtherCall->getArgOperand(5));
  BOOST_REQUIRE(SharedAddress == Published->getInitializer());

  BOOST_CHECK(not replaceAllUsesInFunctionWith(Current, Old, Old));
  BOOST_CHECK_EQUAL(printFunction(Current), CurrentBefore);
  BOOST_CHECK(replaceAllUsesInFunctionWith(Current, Old, Replacement));
  BOOST_CHECK(not verifyModule(*M, &errs()));
  BOOST_CHECK_EQUAL(printFunction(Other), OtherBefore);
  BOOST_CHECK(Published->getInitializer() == SharedAddress);
  BOOST_CHECK(Call->getArgOperand(0) == Replacement);
  auto *AlphaPointer = instructionByName(Current, "alpha_pointer");
  BOOST_CHECK(AlphaPointer->getOperand(0) == Replacement);
  auto *CastLoad = cast<LoadInst>(instructionByName(Current, "cast_load"));
  auto *LoadCast = dyn_cast<AddrSpaceCastInst>(CastLoad->getPointerOperand());
  BOOST_REQUIRE(LoadCast != nullptr);
  BOOST_CHECK(LoadCast->getOperand(0) == Replacement);
  for (unsigned Index : { 5, 6 }) {
    auto *Cast = dyn_cast<PtrToIntInst>(Call->getArgOperand(Index));
    BOOST_REQUIRE(Cast != nullptr);
    BOOST_CHECK(Cast->getOperand(0) == Replacement);
    BOOST_CHECK(Cast->comesBefore(Call));
  }
  BOOST_CHECK(Call->getArgOperand(5) != Call->getArgOperand(6));
  BOOST_CHECK(Call->getArgOperand(4) == NonCastExpression);
  BOOST_CHECK(Call->getArgOperand(8) == NestedExpression);
  std::string CurrentAfter = printFunction(Current);
  BOOST_CHECK(not replaceAllUsesInFunctionWith(Current, Old, Replacement));
  BOOST_CHECK(not replaceAllUsesInFunctionWith(Current,
                                               Published,
                                               Replacement));
  BOOST_CHECK_EQUAL(printFunction(Current), CurrentAfter);

  // Old can itself be a constant expression. Its direct uses are replaced
  // directly, while cast users of that expression are materialized locally.
  BOOST_CHECK(replaceAllUsesInFunctionWith(Current,
                                           NonCastExpression,
                                           Replacement));
  BOOST_CHECK(Call->getArgOperand(4) == Replacement);
  auto *NestedCast = dyn_cast<PtrToIntInst>(Call->getArgOperand(8));
  BOOST_REQUIRE(NestedCast != nullptr);
  BOOST_CHECK(NestedCast->getOperand(0) == Replacement);
  BOOST_CHECK(not replaceAllUsesInFunctionWith(Current,
                                               NonCastExpression,
                                               Replacement));
  auto *Integer = ConstantInt::get(Type::getInt64Ty(Context), 99);
  BOOST_CHECK(replaceAllUsesInFunctionWith(Current, ExcludedAddress, Integer));
  BOOST_CHECK(Call->getArgOperand(7) == Integer);
  BOOST_CHECK(not replaceAllUsesInFunctionWith(Current,
                                               ExcludedAddress,
                                               Integer));
  BOOST_CHECK(not verifyModule(*M, &errs()));
  BOOST_CHECK_EQUAL(printFunction(Other), OtherBefore);
  BOOST_CHECK(Published->getInitializer() == SharedAddress);
}
