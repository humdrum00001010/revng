#pragma once

//
// This file is distributed under the MIT License. See LICENSE.md for details.
//

#ifdef DISABLE_ATTRIBUTES
#define __attribute__(argument)
#endif

// Helper macros
#define _CUSTOM_ATTRIBUTE(value) __attribute__((annotate(#value)))
#define _CUSTOM_ANNOTATION(key, value) \
  __attribute__((annotate(#key ":" #value)))

// Custom attributes
#define _STACK _CUSTOM_ATTRIBUTE(stack)
#define _CAN_CONTAIN_CODE _CUSTOM_ATTRIBUTE(can_contain_code)
#define _HAS_ONE_BROKEN_RETURN _CUSTOM_ATTRIBUTE(has_one_broken_return)

// Custom attributes with an argument
#define _REG(x) _CUSTOM_ANNOTATION(reg, x)
// Native compilation is opt-in and only applies to supported Windows x86 ABIs.
#ifdef REVNG_ENABLE_NATIVE_ABI
#if !defined(_WIN32) || (!defined(__i386__) && !defined(_M_IX86))
#error REVNG_ENABLE_NATIVE_ABI requires a 32-bit Windows x86 target
#endif
#if defined(DISABLE_ATTRIBUTES)
#error REVNG_ENABLE_NATIVE_ABI requires attributes to remain enabled
#endif
#if !defined(__GNUC__) && !defined(__clang__)
#error REVNG_ENABLE_NATIVE_ABI requires GNU-style calling convention attributes
#endif
#define _REVNG_NATIVE_ABI_Microsoft_x86_cdecl __attribute__((cdecl))
#define _REVNG_NATIVE_ABI_Microsoft_x86_stdcall __attribute__((stdcall))
#define _REVNG_NATIVE_ABI_Microsoft_x86_fastcall __attribute__((fastcall))
#define _REVNG_NATIVE_ABI_Microsoft_x86_thiscall __attribute__((thiscall))
// Raw register/stack layouts have no equivalent single C calling convention.
#define _REVNG_NATIVE_ABI_raw_x86
#define _REVNG_NATIVE_ABI_EXPAND(x) _REVNG_NATIVE_ABI_##x
#define _ABI(x) _CUSTOM_ANNOTATION(abi, x) _REVNG_NATIVE_ABI_EXPAND(x)
#else
#define _ABI(x) _CUSTOM_ANNOTATION(abi, x)
#endif
#define _STARTS_AT(x) _CUSTOM_ANNOTATION(field_start_offset, x)
#define _SIZE(x) _CUSTOM_ANNOTATION(struct_size, x)
#define _ENUM_UNDERLYING(x) _CUSTOM_ANNOTATION(enum_underlying_type, x)

// Real attribute aliases
#define _PACKED __attribute__((packed))
#define _ALWAYS_INLINE __attribute__((always_inline))
#define _NORETURN __attribute__((noreturn))
