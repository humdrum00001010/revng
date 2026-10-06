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

typedef struct _PACKED HBITMAP__ HBITMAP__;
typedef HBITMAP__ *HBITMAP;
typedef struct _PACKED HDC__ HDC__;
typedef HDC__ *HDC;
typedef struct _PACKED tagBITMAPINFO tagBITMAPINFO;
typedef tagBITMAPINFO BITMAPINFO;
typedef uint32_t UINT;
typedef void *HANDLE;
typedef uint32_t DWORD;
typedef _ABI(Microsoft_x86_stdcall)
    HBITMAP cabifunction_5(HDC, const BITMAPINFO *, UINT, void **, HANDLE, DWORD);

struct _PACKED _SIZE(4) HBITMAP__ {
  int32_t unused _STARTS_AT(0);
};

struct _PACKED _SIZE(4) HDC__ {
  int32_t unused _STARTS_AT(0);
};

typedef struct _PACKED tagBITMAPINFOHEADER tagBITMAPINFOHEADER;
typedef tagBITMAPINFOHEADER BITMAPINFOHEADER;
typedef int32_t LONG;
typedef uint16_t WORD;
struct _PACKED _SIZE(40) tagBITMAPINFOHEADER {
  DWORD biSize _STARTS_AT(0);
  LONG biWidth _STARTS_AT(4);
  LONG biHeight _STARTS_AT(8);
  WORD biPlanes _STARTS_AT(12);
  WORD biBitCount _STARTS_AT(14);
  DWORD biCompression _STARTS_AT(16);
  DWORD biSizeImage _STARTS_AT(20);
  LONG biXPelsPerMeter _STARTS_AT(24);
  LONG biYPelsPerMeter _STARTS_AT(28);
  DWORD biClrUsed _STARTS_AT(32);
  DWORD biClrImportant _STARTS_AT(36);
};
typedef struct _PACKED tagRGBQUAD tagRGBQUAD;
typedef tagRGBQUAD RGBQUAD;
struct _PACKED _SIZE(44) tagBITMAPINFO {
  BITMAPINFOHEADER bmiHeader _STARTS_AT(0);
  RGBQUAD bmiColors[1] _STARTS_AT(40);
};

typedef uint8_t BYTE;
struct _PACKED _SIZE(4) tagRGBQUAD {
  BYTE rgbBlue _STARTS_AT(0);
  BYTE rgbGreen _STARTS_AT(1);
  BYTE rgbRed _STARTS_AT(2);
  BYTE rgbReserved _STARTS_AT(3);
};

typedef struct _PACKED HPALETTE__ HPALETTE__;
typedef HPALETTE__ *HPALETTE;
typedef struct _PACKED tagLOGPALETTE tagLOGPALETTE;
typedef tagLOGPALETTE LOGPALETTE;
typedef _ABI(Microsoft_x86_stdcall)
    HPALETTE cabifunction_6(const LOGPALETTE *);

struct _PACKED _SIZE(4) HPALETTE__ {
  int32_t unused _STARTS_AT(0);
};

typedef struct _PACKED tagPALETTEENTRY tagPALETTEENTRY;
typedef tagPALETTEENTRY PALETTEENTRY;
struct _PACKED _SIZE(8) tagLOGPALETTE {
  WORD palVersion _STARTS_AT(0);
  WORD palNumEntries _STARTS_AT(2);
  PALETTEENTRY palPalEntry[1] _STARTS_AT(4);
};

struct _PACKED _SIZE(4) tagPALETTEENTRY {
  BYTE peRed _STARTS_AT(0);
  BYTE peGreen _STARTS_AT(1);
  BYTE peBlue _STARTS_AT(2);
  BYTE peFlags _STARTS_AT(3);
};

typedef struct _PACKED struct_31 struct_31;
struct _PACKED _SIZE(8) struct_31 {
  uint8_t padding_at_0[8];
};

typedef _ABI(Microsoft_x86_cdecl)
    generic32_t cabifunction_42(generic32_t, generic32_t, generic32_t);

//
// Functions
//

_ABI(Microsoft_x86_cdecl)
generic32_t function_0x10001000_Code_x86(generic32_t argument_0, generic32_t argument_1, generic32_t argument_2);

//
// Imported Dynamic Functions
//

_ABI(Microsoft_x86_stdcall)
HBITMAP CreateDIBSection(HDC argument_0, const BITMAPINFO *argument_1, UINT argument_2, void **argument_3, HANDLE argument_4, DWORD argument_5);

_ABI(Microsoft_x86_stdcall)
HPALETTE CreatePalette(const LOGPALETTE *argument_0);

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
struct _PACKED _SIZE(40) opaque_type_40 {
  uint8_t padding_at_0[40];
};
typedef struct _PACKED opaque_type_40 opaque_type_40;
struct _PACKED _SIZE(44) opaque_type_44 {
  uint8_t padding_at_0[44];
};
typedef struct _PACKED opaque_type_44 opaque_type_44;
struct _PACKED _SIZE(512) opaque_type_512 {
  uint8_t padding_at_0[512];
};
typedef struct _PACKED opaque_type_512 opaque_type_512;
