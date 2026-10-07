/// Promote CSVs in form of global variables to local variables.

//
// This file is distributed under the MIT License. See LICENSE.md for details.
//

#include "llvm/ADT/STLExtras.h"

#include "revng/EarlyFunctionAnalysis/PromoteGlobalToLocalVars.h"
#include "revng/Model/FunctionTags.h"
#include "revng/Support/IRBuilder.h"
#include "revng/Support/IRHelpers.h"
#include "revng/Support/OpaqueRegisterUser.h"

using namespace llvm;

llvm::PreservedAnalyses
PromoteGlobalToLocalPass::run(llvm::Function &F,
                              llvm::FunctionAnalysisManager &FAM) {

  // Collect the CSVs used by the current function.
  std::map<GlobalVariable *, Value *> CSVMap;
  for (auto &BB : F) {
    for (auto &I : BB) {
      Value *Pointer = nullptr;
      if (auto *Load = dyn_cast<LoadInst>(&I))
        Pointer = skipCasts(Load->getPointerOperand());
      else if (auto *Store = dyn_cast<StoreInst>(&I))
        Pointer = skipCasts(Store->getPointerOperand());
      else
        continue;

      if (auto *CSV = dyn_cast_or_null<GlobalVariable>(Pointer))
        if (not ShouldPromote or ShouldPromote(*CSV))
          CSVMap.try_emplace(CSV);
    }
  }

  revng::IRBuilder Builder(&F.getEntryBlock().front());

  // Create an equivalent local variable for each CSV.
  for (GlobalVariable *CSV : toSortedByName(llvm::make_first_range(CSVMap))) {
    auto *CSVTy = CSV->getValueType();
    auto *Alloca = Builder.CreateAlloca(CSVTy, nullptr, CSV->getName());
    CSVMap[CSV] = Alloca;
  }

  // Rewrite operands in this function instead of scanning the module-wide use
  // list of each CSV, which is shared by all the outlined functions.
  for (auto &BB : F) {
    for (auto &I : BB) {
      for (Use &U : I.operands()) {
        Value *Operand = U.get();
        auto *Cast = dyn_cast<ConstantExpr>(Operand);
        if (Cast != nullptr and Cast->isCast())
          Operand = Cast->getOperand(0);

        auto *CSV = dyn_cast<GlobalVariable>(Operand);
        auto It = CSVMap.find(CSV);
        if (It == CSVMap.end())
          continue;

        if (Cast != nullptr) {
          // Constant expressions can be shared with other functions.
          // Materialize a separate cast for this use before replacing its CSV
          // operand.
          Instruction *CastInst = Cast->getAsInstruction();
          CastInst->replaceUsesOfWith(CSV, It->second);
          CastInst->insertBefore(&I);
          U.set(CastInst);
        } else {
          U.set(It->second);
        }
      }
    }
  }

  // Load all the CSVs and store their value onto the local variables.
  for (const auto &[CSV, Alloca] : CSVMap)
    Builder.CreateStore(Builder.createLoad(CSV), Alloca);

  return PreservedAnalyses::none();
}
