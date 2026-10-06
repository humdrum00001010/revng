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
struct _CAN_CONTAIN_CODE _PACKED _SIZE(512) struct_0 {
  uint8_t padding_at_0[512];
};

typedef struct _PACKED struct_1 struct_1;
struct _PACKED _SIZE(512) struct_1 {
  uint8_t padding_at_0[512];
};

typedef struct _PACKED struct_2 struct_2;
struct _PACKED _SIZE(512) struct_2 {
  uint8_t padding_at_0[512];
};

typedef struct _PACKED struct_3 struct_3;
struct _PACKED _SIZE(512) struct_3 {
  uint8_t padding_at_0[512];
};

typedef struct _PACKED struct_4 struct_4;
struct _PACKED _SIZE(512) struct_4 {
  uint8_t padding_at_0[512];
};

typedef struct _PACKED artificial_struct_returned_by_rawfunction_5 artificial_struct_returned_by_rawfunction_5;
typedef _ABI(raw_x86)
    artificial_struct_returned_by_rawfunction_5 rawfunction_5(void);

struct _PACKED _SIZE(8) artificial_struct_returned_by_rawfunction_5 {
  pointer_or_number32_t register_eax _STARTS_AT(0);
  pointer_or_number32_t register_edx _STARTS_AT(4);
};

typedef struct _PACKED struct_14 struct_14;
struct _PACKED _SIZE(20) struct_14 {
  uint8_t padding_at_0[20];
};

typedef struct _PACKED struct_24 struct_24;
struct _PACKED _SIZE(4) struct_24 {
  generic8_t offset_0;
  generic16_t offset_1;
  generic8_t offset_3;
};

typedef struct _PACKED struct_25 struct_25;
struct _PACKED _SIZE(2) struct_25 {
  generic8_t offset_0;
  generic8_t offset_1;
};

typedef _ABI(Microsoft_x86_cdecl) void cabifunction_27(generic8_t *);

typedef _ABI(Microsoft_x86_cdecl) void cabifunction_28(struct_24 *, generic32_t);

typedef _ABI(Microsoft_x86_cdecl) void cabifunction_29(struct_25 *, generic8_t);

typedef _ABI(Microsoft_x86_cdecl)
    generic32_t cabifunction_30(generic32_t, generic32_t, generic32_t);

//
// Functions
//

_ABI(Microsoft_x86_cdecl)
void function_0x10001000_Code_x86(generic8_t *argument_0);

_ABI(Microsoft_x86_cdecl)
void function_0x10001020_Code_x86(struct_24 *argument_0, generic32_t argument_1);

_ABI(Microsoft_x86_cdecl)
void function_0x10001050_Code_x86(struct_25 *argument_0, generic8_t argument_1);

_ABI(Microsoft_x86_cdecl)
generic32_t function_0x10001060_Code_x86(generic32_t argument_0, generic32_t argument_1, generic32_t argument_2);

//
// Segments
//

struct_0 segment_0;

struct_1 segment_1;

struct_2 segment_2;

struct_3 segment_3;

struct_4 segment_4;

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
struct _PACKED _SIZE(20) opaque_type_20 {
  uint8_t padding_at_0[20];
};
typedef struct _PACKED opaque_type_20 opaque_type_20;
struct _PACKED _SIZE(512) opaque_type_512 {
  uint8_t padding_at_0[512];
};
typedef struct _PACKED opaque_type_512 opaque_type_512;
