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

  // Create an equivalent local variable, replace all the uses of the CSV.
  auto SortedCSVs = toSortedByName(llvm::make_first_range(CSVMap));
  for (GlobalVariable *CSV : SortedCSVs) {
    auto *CSVTy = CSV->getValueType();
    CSVMap[CSV] = Builder.CreateAlloca(CSVTy, nullptr, CSV->getName());
  }

  replaceAllUsesInFunctionWith(&F, CSVMap);

  // Load all the CSVs and store their value onto the local variables, in the
  // order of the names: the map is ordered by the addresses of the CSVs
  for (GlobalVariable *CSV : SortedCSVs)
    Builder.CreateStore(Builder.createLoad(CSV), CSVMap[CSV]);

  return PreservedAnalyses::none();
}
