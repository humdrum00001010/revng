#include "types-and-globals.h"
#include "helpers.h"
_ABI(Microsoft_x86_cdecl)
generic32_t function_0x10001000_Code_x86(struct_23 *argument_0, generic32_t argument_1) {
  struct_23 *var_0 = argument_0;
  generic32_t var_1 = argument_1;
  argument_0->offset_0 = argument_1;
  argument_0->offset_4 = argument_0->offset_4 + 1U;
  argument_0->offset_16 = argument_0->offset_16 + -1U;

  struct_23 *var_2 = argument_0;
  return (generic32_t) CreateFontIndirectW((const LOGFONTW *) argument_0);
}

_ABI(Microsoft_x86_cdecl)
generic32_t function_0x10001020_Code_x86(generic32_t argument_0, generic32_t argument_1, generic32_t argument_2) {
  generic32_t var_0 = argument_0;
  generic32_t var_1 = argument_1;
  generic32_t var_2 = argument_2;
  generic32_t var_3 = (argument_1 & 0x3U) + 18U;
  struct_2 *var_4 = &segment_2;
  *(generic32_t *) (generic32_t) ((generic32_t) &segment_2 + 92ULL) = function_0x10001000_Code_x86((struct_23 *) &segment_2, (argument_1 & 0x3U) + 18U);
  return (uint8_t) (var_0 | var_2 | argument_1 ? (int8_t) 1 : (int8_t) 0);
}

