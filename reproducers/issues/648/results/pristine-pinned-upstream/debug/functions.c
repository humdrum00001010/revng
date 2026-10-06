#include "types-and-globals.h"
#include "helpers.h"
_ABI(SystemV_x86)
void scalar_view(generic64_t *argument_0) {
  generic64_t *var_0 = argument_0;
  generic64_t var_1;
  helper_fldl_ST0_wrapper((void *) 10176U, *argument_0, undef(generic32_t), undef(generic16_t), undef(generic16_t), undef(generic16_t), undef(generic8_t), undef(generic8_t), undef(generic8_t), undef(generic8_t), undef(generic8_t), undef(generic8_t), undef(generic8_t), undef(generic8_t), (void *) 32U, (void *) 32U, (void *) 32U, (void *) 32U, (void *) 32U, (void *) 32U, (void *) 32U, (void *) 32U, (void *) 32U, (void *) 32U, &var_1, (void *) 32U, (void *) 32U, (void *) 32U, (void *) 32U, (void *) 32U, (void *) 32U, (void *) 32U, (void *) 32U, (void *) 32U, (void *) 32U, (void *) 32U, (void *) 32U, (void *) 32U, (void *) 32U, (void *) 32U, (void *) 32U);
}

_ABI(SystemV_x86)
generic32_t small_target(struct_22 argument_0) {
  return *bit_cast(generic32_t *, argument_0);
}

_ABI(SystemV_x86)
generic32_t wide_target(struct_23 argument_0) {
  return *(bit_cast(generic32_t *, argument_0) + 1U) + *bit_cast(generic32_t *, argument_0);
}

_ABI(SystemV_x86)
void overlapping_views(generic64_t *argument_0) {
  struct_26 stack;
  generic64_t *var_0 = argument_0;
  generic64_t *var_1 = argument_0;
  scalar_view(argument_0);

  generic64_t var_2 = helper_fstl_ST0_wrapper((void *) 10176U, undef(generic32_t), undef(generic16_t), undef(generic16_t), undef(generic64_t), undef(generic16_t), undef(generic64_t), undef(generic16_t), undef(generic64_t), undef(generic16_t), undef(generic64_t), undef(generic16_t), undef(generic64_t), undef(generic16_t), undef(generic64_t), undef(generic16_t), undef(generic64_t), undef(generic16_t), undef(generic64_t), undef(generic16_t), undef(generic16_t), undef(generic8_t), undef(generic8_t), undef(generic8_t), undef(generic8_t), undef(generic8_t), undef(generic8_t), undef(generic8_t), undef(generic8_t), (void *) 32U, (void *) 32U);
  *(generic64_t *) (generic32_t) ((generic32_t) ((generic8_t *) &stack + 20U) & 0xFFFFFFFFULL) = var_2;
  helper_fpop_wrapper((void *) 10176U, undef(generic32_t), (void *) 32U, (void *) 32U, (void *) 32U, (void *) 32U, (void *) 32U, (void *) 32U, (void *) 32U, (void *) 32U, (void *) 32U);

  generic32_t var_3 = *(generic32_t *) argument_0;
  struct_22 var_4;
  *(generic32_t *) (generic32_t) ((generic32_t) &var_4 & 0xFFFFFFFFULL) = var_3;

  generic64_t var_5 = float64_sub(*(generic64_t *) (generic32_t) ((generic32_t) &segment_0 + 216ULL) | small_target(var_4), *(generic64_t *) (generic32_t) ((generic32_t) &segment_0 + 216ULL), (void *) 10790U);
  *(generic64_t *) 32U = 0ULL;

  generic64_t var_6 = float64_add(var_5, *(generic64_t *) (generic32_t) ((generic32_t) ((generic8_t *) &stack + 20U) & 0xFFFFFFFFULL), (void *) 10790U);
  *(generic64_t *) 32U = 0ULL;
  *(generic64_t *) (generic32_t) ((generic32_t) ((generic8_t *) &stack + 28U) & 0xFFFFFFFFULL) = var_6;
  *(generic64_t *) (generic32_t) ((generic32_t) ((generic8_t *) &stack + 36U) & 0xFFFFFFFFULL) = 0ULL;

  struct_23 var_7;
  *(generic32_t *) (generic32_t) ((generic32_t) &var_7 & 0xFFFFFFFFULL) = var_3;

  generic64_t var_8 = float64_sub(*(generic64_t *) (generic32_t) ((generic32_t) &segment_0 + 216ULL) | wide_target(var_7), *(generic64_t *) (generic32_t) ((generic32_t) &segment_0 + 216ULL), (void *) 10790U);
  *(generic64_t *) 32U = 0ULL;

  generic64_t var_9 = float64_add(var_8, *(generic64_t *) (generic32_t) ((generic32_t) ((generic8_t *) &stack + 28U) & 0xFFFFFFFFULL), (void *) 10790U);
  *(generic64_t *) 32U = 0ULL;
  *(generic64_t *) (generic32_t) ((generic32_t) ((generic8_t *) &stack + 12U) & 0xFFFFFFFFULL) = var_9;

  generic64_t var_10;
  helper_fldl_ST0_wrapper((void *) 10176U, var_9, undef(generic32_t), undef(generic16_t), undef(generic16_t), undef(generic16_t), undef(generic8_t), undef(generic8_t), undef(generic8_t), undef(generic8_t), undef(generic8_t), undef(generic8_t), undef(generic8_t), undef(generic8_t), (void *) 32U, (void *) 32U, (void *) 32U, (void *) 32U, (void *) 32U, (void *) 32U, (void *) 32U, (void *) 32U, (void *) 32U, (void *) 32U, &var_10, (void *) 32U, (void *) 32U, (void *) 32U, (void *) 32U, (void *) 32U, (void *) 32U, (void *) 32U, (void *) 32U, (void *) 32U, (void *) 32U, (void *) 32U, (void *) 32U, (void *) 32U, (void *) 32U, (void *) 32U, (void *) 32U);
}

_NORETURN _ABI(SystemV_x86) void _start(void) {
  *(generic32_t *) (generic32_t) ((generic32_t) &segment_2 + 16ULL) = 0U;
  *(generic32_t *) (generic32_t) ((generic32_t) &segment_2 + 12ULL) = (generic32_t) &segment_2;

  generic8_t *var_0 = (generic8_t *) &segment_2.bss;
  overlapping_views(&segment_2.bss.storage);

  generic64_t var_1 = helper_fstl_ST0_wrapper((void *) 10176U, undef(generic32_t), undef(generic16_t), undef(generic16_t), undef(generic64_t), undef(generic16_t), undef(generic64_t), undef(generic16_t), undef(generic64_t), undef(generic16_t), undef(generic64_t), undef(generic16_t), undef(generic64_t), undef(generic16_t), undef(generic64_t), undef(generic16_t), undef(generic64_t), undef(generic16_t), undef(generic64_t), undef(generic16_t), undef(generic16_t), undef(generic8_t), undef(generic8_t), undef(generic8_t), undef(generic8_t), undef(generic8_t), undef(generic8_t), undef(generic8_t), undef(generic8_t), (void *) 32U, (void *) 32U);
  *(generic64_t *) (generic32_t) ((generic32_t) &segment_2 + 20ULL) = var_1;
  helper_fpop_wrapper((void *) 10176U, undef(generic32_t), (void *) 32U, (void *) 32U, (void *) 32U, (void *) 32U, (void *) 32U, (void *) 32U, (void *) 32U, (void *) 32U, (void *) 32U);
}

