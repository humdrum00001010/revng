#pragma once

//
// This file is distributed under the MIT License. See LICENSE.md for details.
//

#include <optional>

#include "llvm/ADT/BitVector.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/PassManager.h"
#include "llvm/Pass.h"

namespace llvm {
class CallInst;
class Instruction;
} // namespace llvm

/// Inline supported helper calls in one function. Reuse an instance while
/// prepared helper bodies stay unchanged to share their revalidated policies.
class InlineHelpers {
public:
  void run(llvm::Function *F);

private:
  mutable llvm::DenseMap<const llvm::Function *, std::optional<llvm::BitVector>>
    PreparedPolicies;

  void doInline(llvm::CallInst *Call) const;
  bool doInline(llvm::Function *F) const;
  llvm::CallInst *getCallToInline(llvm::Instruction *I) const;
  bool shouldInline(const llvm::CallInst *Call) const;
};

/// Inline every `revng_inline` helper at its call site in the `Isolated`
/// functions, where both the static policy and the prepared body's critical
/// operands permit specialization at the call site. Does not link helper
/// bodies (use `LinkHelpersToInlinePass` first) and does not
/// delete inlined helper bodies (use `DeleteHelperBodiesPass` once at the end
/// of the pipeline).
void inlineHelpers(llvm::Module &M);

class InlineHelpersPass : public llvm::PassInfoMixin<InlineHelpersPass> {
public:
  llvm::PreservedAnalyses run(llvm::Module &M, llvm::ModuleAnalysisManager &);
};

/// `revng opt -inline-helpers` still goes through the legacy pass manager.
class InlineHelpersLegacyPass : public llvm::ModulePass {
public:
  static char ID;

public:
  InlineHelpersLegacyPass() : ModulePass(ID) {}

  bool runOnModule(llvm::Module &M) override;
};
