/* Independently authored complete DLL exercising public bitmap/palette APIs. */
typedef unsigned char BYTE;
typedef unsigned short WORD;
typedef unsigned int DWORD;
typedef int LONG;
typedef void *HANDLE;
struct RGBQUAD { BYTE blue, green, red, reserved; };
typedef struct RGBQUAD RGBQUAD;
struct BITMAPINFOHEADER { DWORD size; LONG width, height; WORD planes, bits; DWORD compression, image_size; LONG xppm, yppm; DWORD used, important; };
typedef struct BITMAPINFOHEADER BITMAPINFOHEADER;
struct BITMAPINFO { BITMAPINFOHEADER header; RGBQUAD colors[1]; };
typedef struct BITMAPINFO BITMAPINFO;
struct PALETTEENTRY { BYTE red, green, blue, flags; };
typedef struct PALETTEENTRY PALETTEENTRY;
struct LOGPALETTE { WORD version, entries; PALETTEENTRY colors[1]; };
typedef struct LOGPALETTE LOGPALETTE;
__declspec(dllimport) HANDLE __stdcall CreateDIBSection(HANDLE, const BITMAPINFO *, DWORD, void **, HANDLE, DWORD);
__declspec(dllimport) HANDLE __stdcall CreatePalette(const LOGPALETTE *);
volatile HANDLE bitmap_result;
volatile HANDLE palette_result;
__declspec(dllexport) int __stdcall DllMain(HANDLE module, DWORD reason, void *reserved) {
  static BITMAPINFO info = {{40, 1, 1, 1, 32, 0, 4, 0, 0, 1, 0}, {{0, 0, 0, 0}}};
  static LOGPALETTE palette = {0x300, 1, {{0, 0, 0, 0}}};
  void *pixels = 0;
  bitmap_result = CreateDIBSection(0, &info, 0, &pixels, 0, 0);
  palette_result = CreatePalette(&palette);
  return module != 0 || reason != 0 || reserved != 0;
}
