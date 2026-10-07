#pragma once

//
// This file is distributed under the MIT License. See LICENSE.md for details.
//

#include <algorithm>
#include <map>
#include <set>

#include "llvm/IR/GlobalVariable.h"
#include "llvm/Support/MathExtras.h"

#include "revng/Model/Binary.h"
#include "revng/Model/PrimitiveType.h"
#include "revng/Model/Register.h"

namespace efa {

/// Record register types covering every confirmed CSV portion.
/// Power-of-two rounding must fit the physical register: x87 st0 is 10 bytes.
inline void recordRegisters(const std::set<llvm::GlobalVariable *> &CSVs,
                            model::Architecture::Values Architecture,
                            auto Inserter) {
  std::map<model::Register::Values, uint64_t> ConfirmedByteCounts;
  for (auto *CSV : CSVs) {
    model::Register::Portion Portion(CSV->getName(), Architecture);
    if (Portion.Register == model::Register::Invalid)
      continue;

    uint64_t &Bytes = ConfirmedByteCounts[Portion.Register];
    Bytes = std::max(Bytes, Portion.StartOffset + Portion.Size);
  }

  for (auto [Register, ByteCount] : ConfirmedByteCounts) {
    uint64_t RegisterSize = model::Register::getSize(Register);
    revng_assert(RegisterSize >= ByteCount);

    uint64_t TypeSize = std::min(llvm::PowerOf2Ceil(ByteCount), RegisterSize);
    auto Type = model::PrimitiveType::makeGeneric(TypeSize);
    revng_assert(*Type->size() >= ByteCount);
    revng_assert(RegisterSize >= *Type->size());

    Inserter.emplace(Register).Type() = std::move(Type);
  }
}

} // namespace efa
