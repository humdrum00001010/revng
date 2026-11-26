#pragma once

//
// This file is distributed under the MIT License. See LICENSE.md for details.
//

#include "llvm/ADT/SmallVector.h"

#include "revng/Model/Binary.h"

namespace abi::FunctionType {

class TypeDefinitionReferenceIndex;

/// Best effort `CABIFunctionDefinition` to `RawFunctionDefinition` conversion.
///
/// If `ABI` is not specified, `Binary.DefaultABI` is used instead.
///
/// \param Index An index of the references of \p Binary, which is used, and
///        kept up to date, to replace the references to \p Function.
///
/// \param UseSoftRegisterStateDeductions For specifics see the difference
///        between `abi::Definition::tryDeducingArgumentRegisterState` (`true`)
///        and `abi::Definition::enforceArgumentRegisterState` (`false`).
std::optional<model::UpcastableType>
tryConvertToCABI(const model::RawFunctionDefinition &Function,
                 TupleTree<model::Binary> &Binary,
                 TypeDefinitionReferenceIndex &Index,
                 std::optional<model::ABI::Values> ABI = std::nullopt,
                 bool UseSoftRegisterStateDeductions = true);

/// \return the ABIs of the family of \p ABI a function can turn out to follow
///         instead: those where the callee removes its stack arguments, among
///         which those that pass the first arguments in registers. Empty if
///         \p ABI is not one the caller cleans up after.
llvm::SmallVector<model::ABI::Values, 3>
calleeCleanupCounterparts(model::ABI::Values ABI);

} // namespace abi::FunctionType
