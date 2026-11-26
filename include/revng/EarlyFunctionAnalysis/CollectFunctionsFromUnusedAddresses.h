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

void collectFunctionsFromUnusedAddresses(llvm::Module &M,
                                         RootFunction &Root,
                                         model::Binary &Binary,
                                         ControlFlowGraphCache &FMC);
