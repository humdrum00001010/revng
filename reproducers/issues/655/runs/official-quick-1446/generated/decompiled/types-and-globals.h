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

typedef struct _PACKED artificial_struct_returned_by_rawfunction_4 artificial_struct_returned_by_rawfunction_4;
typedef _ABI(raw_x86)
    artificial_struct_returned_by_rawfunction_4 rawfunction_4(void);

struct _PACKED _SIZE(8) artificial_struct_returned_by_rawfunction_4 {
  pointer_or_number32_t register_eax _STARTS_AT(0);
  pointer_or_number32_t register_edx _STARTS_AT(4);
};

typedef struct _PACKED HFONT__ HFONT__;
typedef HFONT__ *HFONT;
typedef struct _PACKED tagLOGFONTW tagLOGFONTW;
typedef tagLOGFONTW LOGFONTW;
typedef _ABI(Microsoft_x86_stdcall)
    HFONT cabifunction_5(const LOGFONTW *);

struct _PACKED _SIZE(4) HFONT__ {
  int32_t unused _STARTS_AT(0);
};

typedef int32_t LONG;
typedef uint8_t BYTE;
typedef int16_t WCHAR;
struct _PACKED _SIZE(92) tagLOGFONTW {
  LONG lfHeight _STARTS_AT(0);
  LONG lfWidth _STARTS_AT(4);
  LONG lfEscapement _STARTS_AT(8);
  LONG lfOrientation _STARTS_AT(12);
  LONG lfWeight _STARTS_AT(16);
  BYTE lfItalic _STARTS_AT(20);
  BYTE lfUnderline _STARTS_AT(21);
  BYTE lfStrikeOut _STARTS_AT(22);
  BYTE lfCharSet _STARTS_AT(23);
  BYTE lfOutPrecision _STARTS_AT(24);
  BYTE lfClipPrecision _STARTS_AT(25);
  BYTE lfQuality _STARTS_AT(26);
  BYTE lfPitchAndFamily _STARTS_AT(27);
  WCHAR lfFaceName[32] _STARTS_AT(28);
};

typedef struct _PACKED struct_17 struct_17;
struct _PACKED _SIZE(4) struct_17 {
  uint8_t padding_at_0[4];
};

typedef struct _PACKED struct_23 struct_23;
struct _PACKED _SIZE(92) struct_23 {
  generic32_t offset_0;
  generic32_t offset_4;
  uint8_t padding_at_8[8];
  generic32_t offset_16;
  uint8_t padding_at_20[72];
};

typedef _ABI(Microsoft_x86_cdecl)
    generic32_t cabifunction_25(struct_23 *, generic32_t);

typedef _ABI(Microsoft_x86_cdecl)
    generic32_t cabifunction_26(generic32_t, generic32_t, generic32_t);

//
// Functions
//

_ABI(Microsoft_x86_cdecl)
generic32_t function_0x10001000_Code_x86(struct_23 *argument_0, generic32_t argument_1);

_ABI(Microsoft_x86_cdecl)
generic32_t function_0x10001020_Code_x86(generic32_t argument_0, generic32_t argument_1, generic32_t argument_2);

//
// Imported Dynamic Functions
//

_ABI(Microsoft_x86_stdcall)
HFONT CreateFontIndirectW(const LOGFONTW *argument_0);

//
// Segments
//

struct_0 segment_0;

struct_1 segment_1;

struct_2 segment_2;

struct_3 segment_3;

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
struct _PACKED _SIZE(64) opaque_type_64 {
  uint8_t padding_at_0[64];
};
typedef struct _PACKED opaque_type_64 opaque_type_64;
struct _PACKED _SIZE(92) opaque_type_92 {
  uint8_t padding_at_0[92];
};
typedef struct _PACKED opaque_type_92 opaque_type_92;
struct _PACKED _SIZE(512) opaque_type_512 {
  uint8_t padding_at_0[512];
};
typedef struct _PACKED opaque_type_512 opaque_type_512;
