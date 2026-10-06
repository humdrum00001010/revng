#include "types-and-globals.h"
#include "helpers.h"
_ABI(Microsoft_x86_cdecl)
generic32_t function_0x10001000_Code_x86(void) {
  return undef(generic32_t);
}

_ABI(Microsoft_x86_cdecl)
struct_19 function_0x10001010_Code_x86(generic32_t argument_0) {
  struct_10 stack;
  generic32_t var_0 = argument_0;
  *(generic64_t *) 32U = 0ULL;
  *(generic8_t *) (generic32_t) ((generic32_t) &stack & 0xFFFFFFFFULL) = 0;

  struct_19 var_1;
  *(generic32_t *) (generic32_t) ((generic32_t) &var_1 & 0xFFFFFFFFULL) = 0U;
  *(generic32_t *) (generic32_t) ((generic32_t) &var_1.offset_4 & 0xFFFFFFFFULL) = argument_0 % 3U;
  return var_1;
}

_ABI(Microsoft_x86_cdecl)
struct_21 function_0x10001030_Code_x86(generic32_t argument_0, generic32_t argument_1, generic32_t argument_2) {
  generic32_t var_0 = argument_0;
  generic32_t var_1 = argument_1;
  generic32_t var_2 = argument_2;
  generic32_t var_3 = function_0x10001000_Code_x86();
  generic32_t var_4 = argument_1 + 7U;
  struct_19 var_5 = function_0x10001010_Code_x86(argument_1 + 7U);
  generic32_t var_6 = *(generic32_t *) (generic32_t) ((generic32_t) &var_5.offset_4 & 0xFFFFFFFFULL);
  segment_2 = bit_cast(struct_2, *(generic32_t *) (generic32_t) ((generic32_t) &var_5 & 0xFFFFFFFFULL) + var_3);

  struct_21 var_7;
  *(generic32_t *) (generic32_t) ((generic32_t) &var_7 & 0xFFFFFFFFULL) = (uint8_t) (var_0 | var_2 | argument_1 ? (int8_t) 1 : (int8_t) 0);
  *(generic32_t *) (generic32_t) ((generic32_t) &var_7.offset_4 & 0xFFFFFFFFULL) = var_6;
  return var_7;
}

