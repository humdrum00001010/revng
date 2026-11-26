#pragma once

//
// This file is distributed under the MIT License. See LICENSE.md for details.
//

#include "llvm/Pass.h"

/// Give what a function allocates on its stack at run time a block of its own
///
/// After such an allocation the stack pointer is the top of what the function
/// pushes and the address of the block at once. Left as one value, the size
/// of the block is part of every stack pointer after it: no call there has a
/// stack of a known size, and its stack arguments cannot be found. This pass
/// tells the two apart, see StackTops.h: the block becomes the result of a
/// call that allocates it, and what the function pushes after it is in the
/// frame again.
struct DetachStackAllocationsPass : public llvm::ModulePass {
public:
  static char ID;

  DetachStackAllocationsPass() : llvm::ModulePass(ID) {}

  bool runOnModule(llvm::Module &M) override;
};
