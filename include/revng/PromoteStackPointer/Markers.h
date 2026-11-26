#pragma once

//
// This file is distributed under the MIT License. See LICENSE.md for details.
//

#include "revng/Support/IRHelper.h"

/// Stands for the value of the stack pointer on entry to a function
inline IRHelper<> UndefinedLocalSPMarker("revng_undefined_local_sp");

/// Records the height of the stack at a call site
inline IRHelper<> StackSizeAtCallSite("stack_size_at_call_site");

/// Stands for an allocation a function makes on its stack at run time: takes
/// the size, returns the address of the block
inline IRHelper<> StackAllocation("revng_stack_alloc");
