#pragma once

//
// This file is distributed under the MIT License. See LICENSE.md for details.
//

// This binding is only for an explicitly selected native Windows i386 backend.
// Its driver must validate the original PE32 image and x86 model. It represents
// the new native function's entry SP; it does not supply a guest machine state
// or establish the original function's ABI, frame layout or runtime behavior.
// Include this header after the generated helper function declarations.
#if defined(REVNG_NATIVE_WINDOWS_I386_ENTRY_SP)

#if !defined(_WIN32) || !defined(__i386__) || __SIZEOF_POINTER__ != 4
#error Native entry-SP lowering requires a Windows i386 target
#endif

#if !defined(__NO_INLINE__)
#error Native entry-SP lowering requires -fno-inline
#endif

#if !defined(__has_builtin)
#error Native entry-SP lowering requires a compiler intrinsic
#elif !__has_builtin(_AddressOfReturnAddress)
#error Native entry-SP lowering requires -fms-extensions and the compiler intrinsic
#endif

void *_AddressOfReturnAddress(void);

// On i386 the return-address slot is exactly SP on native function entry.
#define revng_undefined_local_sp() ((unsigned int) _AddressOfReturnAddress())

#endif
