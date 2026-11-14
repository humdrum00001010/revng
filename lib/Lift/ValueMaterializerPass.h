#pragma once

//
// This file is distributed under the MIT License. See LICENSE.md for details.
//

#include <vector>

#include "llvm/IR/PassManager.h"
#include "llvm/IR/ValueHandle.h"

#include "revng/BasicAnalyses/MaterializedValue.h"
#include "revng/Support/MetaAddress.h"
#include "revng/ValueMaterializer/ValueMaterializer.h"

namespace llvm {
class DataLayout;
}

class JumpTargetManager;

class StaticDataMemoryOracle final : public MemoryOracle {
private:
  JumpTargetManager &JTM;
  const MetaAddress::Features &Features;
  bool IsLittleEndian = false;

public:
  StaticDataMemoryOracle(const llvm::DataLayout &DL,
                         JumpTargetManager &JTM,
                         const MetaAddress::Features &Features);
  ~StaticDataMemoryOracle() final = default;

  MaterializedValue load(uint64_t LoadAddress, unsigned LoadSize) final;
};

class ValueMaterializerPass
  : public llvm::PassInfoMixin<ValueMaterializerPass> {
public:
  using HandleVector = std::vector<llvm::WeakVH>;

private:
  StaticDataMemoryOracle &MO;

  /// Loads popping the program counter from the stack, as recorded before the
  /// function got optimized. A handle is null if its load has been removed
  /// (e.g., replaced with the value that was stored at the top of the stack).
  const HandleVector *StackPops = nullptr;

public:
  ValueMaterializerPass(StaticDataMemoryOracle &MO,
                        const HandleVector *StackPops = nullptr) :
    MO(MO), StackPops(StackPops) {}

  llvm::PreservedAnalyses run(llvm::Function &F,
                              llvm::FunctionAnalysisManager &);

  static llvm::Function *createMarker(llvm::Module &M);
};
