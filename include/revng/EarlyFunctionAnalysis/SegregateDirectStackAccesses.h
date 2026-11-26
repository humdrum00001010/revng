#pragma once

//
// This file is distributed under the MIT License. See LICENSE.md for details.
//

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/IR/PassManager.h"

namespace llvm {
class GlobalVariable;
}

class SegregateDirectStackAccessesPass
  : public llvm::PassInfoMixin<SegregateDirectStackAccessesPass> {
private:
  llvm::GlobalVariable *StackPointer = nullptr;

  /// The registers that hold the address of the block of the thread
  llvm::SmallVector<llvm::GlobalVariable *, 2> ThreadPointers;

public:
  using CSVList = llvm::ArrayRef<llvm::GlobalVariable *>;
  explicit SegregateDirectStackAccessesPass(llvm::GlobalVariable *StackPointer,
                                            CSVList ThreadPointers = {}) :
    StackPointer(StackPointer),
    ThreadPointers(ThreadPointers.begin(), ThreadPointers.end()) {}

  llvm::PreservedAnalyses run(llvm::Function &F,
                              llvm::FunctionAnalysisManager &FAM);
};
