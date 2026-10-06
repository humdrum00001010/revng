#include "types-and-globals.h"
#include "helpers.h"
_ABI(Microsoft_x86_cdecl)
generic32_t function_0x10001000_Code_x86(generic32_t argument_0, generic32_t argument_1) {
  generic32_t var_0 = argument_0;
  generic32_t var_1 = argument_1;
  return *(generic8_t *) ((argument_1 & 0x3U) + argument_0);
}

_ABI(Microsoft_x86_cdecl)
generic32_t function_0x10001010_Code_x86(generic32_t argument_0) {
  generic32_t var_0 = argument_0;
  generic32_t var_1 = 2U;
  generic32_t var_2 = argument_0;
  generic32_t var_3 = function_0x10001000_Code_x86(argument_0, 2U);
  generic32_t var_4 = 3U;
  generic32_t var_5 = argument_0;
  return function_0x10001000_Code_x86(argument_0, 3U) + var_3;
}

_ABI(Microsoft_x86_cdecl)
generic32_t function_0x10001040_Code_x86(generic32_t argument_0, generic32_t argument_1, generic32_t argument_2) {
  generic32_t var_0 = argument_0;
  generic32_t var_1 = argument_1;
  generic32_t var_2 = argument_2;
  struct_1 *var_3 = &segment_1;
  segment_2 = bit_cast(struct_2, function_0x10001010_Code_x86((generic32_t) &segment_1));
  return (uint8_t) (var_1 | var_2 | argument_0 ? (int8_t) 1 : (int8_t) 0);
}

