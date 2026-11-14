#pragma once

//
// This file is distributed under the MIT License. See LICENSE.md for details.
//

#include "revng/Model/Binary.h"

namespace abi::FunctionType {

class TypeDefinitionReferenceIndex;

/// Best effort `CABIFunctionDefinition` to `RawFunctionDefinition` conversion.
///
/// If `ABI` is not specified, `Binary.DefaultABI` is used instead.
///
/// \param UseSoftRegisterStateDeductions For specifics see the difference
///        between `abi::Definition::tryDeducingArgumentRegisterState` (`true`)
///        and `abi::Definition::enforceArgumentRegisterState` (`false`).
///
/// \param Index if not null, an index of the references of \p Binary, used
///        (and kept up to date) instead of visiting the whole model to
///        replace the references to the converted definition.
std::optional<model::UpcastableType>
tryConvertToCABI(const model::RawFunctionDefinition &Function,
                 TupleTree<model::Binary> &Binary,
                 std::optional<model::ABI::Values> ABI = std::nullopt,
                 bool UseSoftRegisterStateDeductions = true,
                 TypeDefinitionReferenceIndex *Index = nullptr);

} // namespace abi::FunctionType
