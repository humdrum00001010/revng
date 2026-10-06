#include "types-and-globals.h"
#include "helpers.h"
_ABI(Microsoft_x86_cdecl)
generic32_t function_0x10001000_Code_x86(generic32_t argument_0, generic32_t argument_1, generic32_t argument_2) {
  struct_31 stack;
  generic32_t var_0 = argument_0;
  generic32_t var_1 = argument_1;
  generic32_t var_2 = argument_2;
  *(generic32_t *) (generic32_t) ((generic32_t) &stack & 0xFFFFFFFFULL) = 0U;

  DWORD var_3 = 0U;
  HANDLE var_4 = (HANDLE) NULL;
  void *var_5 = &stack;
  generic32_t var_6 = 0U;
  struct_2 *var_7 = &segment_2;
  generic32_t var_8 = 0U;
  *(generic32_t *) (generic32_t) ((generic32_t) &segment_2 + 52ULL) = (generic32_t) CreateDIBSection((HDC) NULL, (const BITMAPINFO *) &segment_2, 0U, (void **) &stack, var_4, var_3);

  generic8_t *var_9 = (generic8_t *) &segment_2 + 44U;
  *(generic32_t *) (generic32_t) ((generic32_t) &segment_2 + 56ULL) = (generic32_t) CreatePalette((const LOGPALETTE *) ((generic8_t *) &segment_2 + 44U));
  return (uint8_t) (var_1 | var_2 | argument_0 ? (int8_t) 1 : (int8_t) 0);
}

