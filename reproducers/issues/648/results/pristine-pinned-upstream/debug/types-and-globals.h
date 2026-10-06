#pragma once

//
// This header has been generated using rev.ng.
//

#include "attributes.h"
#include "primitive-types.h"
#include "runtime-library.h"

//
// Types
//

typedef struct _PACKED struct_0 struct_0;
typedef struct _PACKED struct_1 struct_1;
struct _PACKED _SIZE(8) struct_1 {
  uint8_t padding_at_0[8];
};
struct _PACKED _SIZE(224) struct_0 {
  uint8_t padding_at_0[216];
  struct_1 rodata _STARTS_AT(216);
};

typedef struct _PACKED struct_2 struct_2;
typedef struct _PACKED struct_3 struct_3;
struct _CAN_CONTAIN_CODE _PACKED _SIZE(228) struct_3 {
  uint8_t padding_at_0[228];
};
struct _CAN_CONTAIN_CODE _PACKED _SIZE(228) struct_2 {
  struct_3 text _STARTS_AT(0);
};

typedef struct _PACKED struct_4 struct_4;
typedef struct _PACKED struct_5 struct_5;
struct _PACKED _SIZE(8) struct_5 {
  generic64_t target _STARTS_AT(0);
};
typedef struct _PACKED struct_6 struct_6;
struct _PACKED _SIZE(16) struct_6 {
  generic64_t storage _STARTS_AT(0);
  generic64_t observation _STARTS_AT(8);
};
struct _PACKED _SIZE(28) struct_4 {
  struct_5 data _STARTS_AT(0);
  uint8_t padding_at_8[4];
  struct_6 bss _STARTS_AT(12);
};

typedef struct _PACKED artificial_struct_returned_by_rawfunction_7 artificial_struct_returned_by_rawfunction_7;
typedef _ABI(raw_x86)
    artificial_struct_returned_by_rawfunction_7 rawfunction_7(void);

struct _PACKED _SIZE(8) artificial_struct_returned_by_rawfunction_7 {
  pointer_or_number32_t register_eax _STARTS_AT(0);
  pointer_or_number32_t register_edx _STARTS_AT(4);
};

typedef union _PACKED Target Target;
typedef struct _PACKED Small Small;
typedef uint32_t u32;
struct _PACKED _SIZE(4) Small {
  u32 value _STARTS_AT(0);
};
typedef struct _PACKED Wide Wide;
struct _PACKED _SIZE(8) Wide {
  u32 value _STARTS_AT(0);
  u32 extra _STARTS_AT(4);
};
union _PACKED Target {
  Small small;
  Wide wide;
};

typedef _ABI(SystemV_x86) void cabifunction_16(void);

typedef struct _PACKED struct_22 struct_22;
struct _PACKED _SIZE(4) struct_22 {
  uint8_t padding_at_0[4];
};

typedef struct _PACKED struct_23 struct_23;
struct _PACKED _SIZE(4) struct_23 {
  uint8_t padding_at_0[4];
};

typedef struct _PACKED struct_25 struct_25;
struct _PACKED _SIZE(8) struct_25 {
  uint8_t padding_at_0[8];
};

typedef struct _PACKED struct_26 struct_26;
struct _PACKED _SIZE(56) struct_26 {
  uint8_t padding_at_0[56];
};

typedef _ABI(SystemV_x86) void cabifunction_34(generic64_t *);

typedef _ABI(SystemV_x86)
    generic32_t cabifunction_35(struct_22);

typedef _ABI(SystemV_x86)
    generic32_t cabifunction_36(struct_23);

typedef _ABI(SystemV_x86) void cabifunction_37(generic64_t *);

//
// Functions
//

_ABI(SystemV_x86)
void scalar_view(generic64_t *argument_0);

_ABI(SystemV_x86)
generic32_t small_target(struct_22 argument_0);

_ABI(SystemV_x86)
generic32_t wide_target(struct_23 argument_0);

_ABI(SystemV_x86)
void overlapping_views(generic64_t *argument_0);

_NORETURN _ABI(SystemV_x86) void _start(void);

//
// Segments
//

struct_0 segment_0;

struct_2 segment_1;

struct_4 segment_2;

//
// Opaque types
//

struct _PACKED _SIZE(1) opaque_type_1 {
  uint8_t padding_at_0[1];
};
typedef struct _PACKED opaque_type_1 opaque_type_1;
struct _PACKED _SIZE(2) opaque_type_2 {
  uint8_t padding_at_0[2];
};
typedef struct _PACKED opaque_type_2 opaque_type_2;
struct _PACKED _SIZE(4) opaque_type_4 {
  uint8_t padding_at_0[4];
};
typedef struct _PACKED opaque_type_4 opaque_type_4;
struct _PACKED _SIZE(8) opaque_type_8 {
  uint8_t padding_at_0[8];
};
typedef struct _PACKED opaque_type_8 opaque_type_8;
struct _PACKED _SIZE(10) opaque_type_10 {
  uint8_t padding_at_0[10];
};
typedef struct _PACKED opaque_type_10 opaque_type_10;
struct _PACKED _SIZE(12) opaque_type_12 {
  uint8_t padding_at_0[12];
};
typedef struct _PACKED opaque_type_12 opaque_type_12;
struct _PACKED _SIZE(16) opaque_type_16 {
  uint8_t padding_at_0[16];
};
typedef struct _PACKED opaque_type_16 opaque_type_16;
struct _PACKED _SIZE(28) opaque_type_28 {
  uint8_t padding_at_0[28];
};
typedef struct _PACKED opaque_type_28 opaque_type_28;
struct _PACKED _SIZE(56) opaque_type_56 {
  uint8_t padding_at_0[56];
};
typedef struct _PACKED opaque_type_56 opaque_type_56;
struct _PACKED _SIZE(224) opaque_type_224 {
  uint8_t padding_at_0[224];
};
typedef struct _PACKED opaque_type_224 opaque_type_224;
struct _PACKED _SIZE(228) opaque_type_228 {
  uint8_t padding_at_0[228];
};
typedef struct _PACKED opaque_type_228 opaque_type_228;
