#pragma once

//
// This file is distributed under the MIT License. See LICENSE.md for details.
//

#include "revng/BasicAnalyses/RootFunction.h"
#include "revng/EarlyFunctionAnalysis/ControlFlowGraphCache.h"
#include "revng/Model/Binary.h"

/// \return the addresses of code no function reaches that are worth a
///         function, in the order of the blocks of `root`
std::vector<MetaAddress> unusedCodeAddresses(llvm::Module &M,
                                             RootFunction &Root,
                                             model::Binary &Binary,
                                             ControlFlowGraphCache &FMC);

/// \return the addresses that the code of the functions \p IsTrusted accepts
///         uses only, plus something, as the place memory is accessed at:
///         they are the addresses of data, such as the table of a `switch`,
///         not of functions
std::set<uint64_t>
addressesUsedAsData(llvm::Module &M,
                    RootFunction &Root,
                    model::Binary &Binary,
                    ControlFlowGraphCache &FMC,
                    llvm::function_ref<bool(const model::Function &)>
                      IsTrusted);

void collectFunctionsFromUnusedAddresses(llvm::Module &M,
                                         RootFunction &Root,
                                         model::Binary &Binary,
                                         ControlFlowGraphCache &FMC);
