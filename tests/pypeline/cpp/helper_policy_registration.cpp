//
// This file is distributed under the MIT License. See LICENSE.md for details.
//

#include "revng/Pipebox/LLVMPipe.h"
#include "revng/HelperInliningAnalyses/DetectUninlinableHelpers.h"
#include "llvm/PassRegistry.h"

int main() {
  using Base = revng::pypeline::pipes::detail::PureLLVMPassesPipeBase;
  Base First("passes: [detect-uninlinable-helpers]");
  Base Second("passes: [detect-uninlinable-helpers]");
  return llvm::PassRegistry::getPassRegistry()->getPassInfo(llvm::StringRef("detect-uninlinable-helpers")) == nullptr;
}
