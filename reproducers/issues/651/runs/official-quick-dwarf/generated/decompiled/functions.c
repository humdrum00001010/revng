#include "types-and-globals.h"
#include "helpers.h"
_ABI(Microsoft_x86_cdecl)
void function_0x10001000_Code_x86(generic8_t *argument_0) {
  generic8_t *var_0 = argument_0;
  generic32_t var_1 = revng_undefined_local_sp();
  *argument_0 = *argument_0 + (generic8_t) 1;

  generic64_t var_2;
  helper_fldt_ST0_wrapper((void *) 10176U, var_1 + 8U, undef(generic32_t), undef(generic32_t), undef(generic32_t), (void *) 32U, (void *) 32U, (void *) 32U, (void *) 32U, (void *) 32U, (void *) 32U, (void *) 32U, (void *) 32U, (void *) 32U, &var_2, (void *) 32U, (void *) 32U, (void *) 32U, (void *) 32U, (void *) 32U, (void *) 32U, (void *) 32U, (void *) 32U, (void *) 32U, (void *) 32U, (void *) 32U, (void *) 32U, (void *) 32U, (void *) 32U, (void *) 32U);

  generic64_t var_3;
  helper_fldt_ST0_wrapper((void *) 10176U, (generic32_t) &argument_0[1U], undef(generic32_t), undef(generic32_t), undef(generic32_t), (void *) 32U, (void *) 32U, (void *) 32U, (void *) 32U, (void *) 32U, (void *) 32U, (void *) 32U, (void *) 32U, (void *) 32U, &var_3, (void *) 32U, (void *) 32U, (void *) 32U, (void *) 32U, (void *) 32U, (void *) 32U, (void *) 32U, (void *) 32U, (void *) 32U, (void *) 32U, (void *) 32U, (void *) 32U, (void *) 32U, (void *) 32U, (void *) 32U);

  generic64_t var_4;
  helper_fadd_STN_ST0_wrapper((void *) 10176U, 1U, undef(generic32_t), undef(generic16_t), undef(generic16_t), var_3, undef(generic16_t), undef(generic64_t), undef(generic16_t), undef(generic64_t), undef(generic16_t), undef(generic64_t), undef(generic16_t), undef(generic64_t), undef(generic16_t), undef(generic64_t), undef(generic16_t), undef(generic64_t), undef(generic16_t), undef(generic64_t), undef(generic16_t), undef(generic16_t), undef(generic8_t), undef(generic8_t), undef(generic8_t), undef(generic8_t), undef(generic8_t), undef(generic8_t), undef(generic8_t), undef(generic8_t), (void *) 32U, &var_4, (void *) 32U, (void *) 32U, (void *) 32U, (void *) 32U, (void *) 32U, (void *) 32U, (void *) 32U, (void *) 32U, (void *) 32U, (void *) 32U, (void *) 32U, (void *) 32U, (void *) 32U, (void *) 32U, (void *) 32U, (void *) 32U);

  generic64_t var_5 = var_4;
  helper_fpop_wrapper((void *) 10176U, undef(generic32_t), (void *) 32U, (void *) 32U, (void *) 32U, (void *) 32U, (void *) 32U, (void *) 32U, (void *) 32U, (void *) 32U, (void *) 32U);
  helper_fstt_ST0_wrapper((void *) 10176U, (generic32_t) &argument_0[1U], undef(generic32_t), undef(generic32_t), undef(generic32_t), var_5, undef(generic16_t), undef(generic64_t), undef(generic16_t), undef(generic64_t), undef(generic16_t), undef(generic64_t), undef(generic16_t), undef(generic64_t), undef(generic16_t), undef(generic64_t), undef(generic16_t), undef(generic64_t), undef(generic16_t), undef(generic64_t), undef(generic16_t));
  helper_fpop_wrapper((void *) 10176U, undef(generic32_t), (void *) 32U, (void *) 32U, (void *) 32U, (void *) 32U, (void *) 32U, (void *) 32U, (void *) 32U, (void *) 32U, (void *) 32U);
}

_ABI(Microsoft_x86_cdecl)
void function_0x10001020_Code_x86(struct_36 *argument_0, generic32_t argument_1) {
  struct_36 *var_0 = argument_0;
  generic32_t var_1 = argument_1;
  argument_0->offset_0 = argument_0->offset_0 + (generic8_t) 1;

  generic32_t var_2 = ((generic32_t) argument_0->offset_3 << 16U | argument_0->offset_1) ^ argument_1;
  argument_0->offset_1 = var_2;
  argument_0->offset_3 = var_2 >> 16U;
}

_ABI(Microsoft_x86_cdecl)
void function_0x10001050_Code_x86(struct_37 *argument_0, generic8_t argument_1) {
  struct_37 *var_0 = argument_0;
  generic8_t var_1 = argument_1;
  generic8_t var_2 = *(generic8_t *) (generic32_t) ((generic32_t) &var_1 & 0xFFFFFFFFULL);
  argument_0->offset_0 = argument_0->offset_0 + (generic8_t) 1;
  argument_0->offset_1 = var_2;
}

_ABI(Microsoft_x86_cdecl)
generic32_t function_0x10001060_Code_x86(generic32_t argument_0, generic32_t argument_1, generic32_t argument_2) {
  struct_20 stack;
  generic32_t var_0 = argument_0;
  generic32_t var_1 = argument_1;
  generic32_t var_2 = argument_2;
  generic64_t var_3;
  helper_flds_ST0_wrapper((void *) 10176U, *(generic32_t *) &segment_1, undef(generic32_t), undef(generic16_t), undef(generic16_t), undef(generic16_t), undef(generic8_t), undef(generic8_t), undef(generic8_t), undef(generic8_t), undef(generic8_t), undef(generic8_t), undef(generic8_t), undef(generic8_t), (void *) 32U, (void *) 32U, (void *) 32U, (void *) 32U, (void *) 32U, (void *) 32U, (void *) 32U, (void *) 32U, (void *) 32U, (void *) 32U, &var_3, (void *) 32U, (void *) 32U, (void *) 32U, (void *) 32U, (void *) 32U, (void *) 32U, (void *) 32U, (void *) 32U, (void *) 32U, (void *) 32U, (void *) 32U, (void *) 32U, (void *) 32U, (void *) 32U, (void *) 32U, (void *) 32U);
  helper_fstt_ST0_wrapper((void *) 10176U, (generic32_t) &stack, undef(generic32_t), undef(generic32_t), undef(generic32_t), var_3, undef(generic16_t), undef(generic64_t), undef(generic16_t), undef(generic64_t), undef(generic16_t), undef(generic64_t), undef(generic16_t), undef(generic64_t), undef(generic16_t), undef(generic64_t), undef(generic16_t), undef(generic64_t), undef(generic16_t), undef(generic64_t), undef(generic16_t));
  helper_fpop_wrapper((void *) 10176U, undef(generic32_t), (void *) 32U, (void *) 32U, (void *) 32U, (void *) 32U, (void *) 32U, (void *) 32U, (void *) 32U, (void *) 32U, (void *) 32U);

  struct_3 *var_4 = &segment_3;
  function_0x10001000_Code_x86((generic8_t *) &segment_3);

  generic32_t var_5 = 11259375U;
  generic8_t *var_6 = (generic8_t *) &segment_3 + 13U;
  function_0x10001020_Code_x86((struct_36 *) ((generic8_t *) &segment_3 + 13U), 11259375U);

  generic32_t var_7 = (uint8_t) (argument_1 ? (int8_t) 1 : (int8_t) 0);
  generic8_t *var_8 = (generic8_t *) &segment_3 + 18U;
  function_0x10001050_Code_x86((struct_37 *) ((generic8_t *) &segment_3 + 18U), argument_1 ? (int8_t) 1 : (int8_t) 0);
  return (uint8_t) (var_2 | argument_0 | argument_1 ? (int8_t) 1 : (int8_t) 0);
}

