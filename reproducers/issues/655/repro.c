/* Independently authored complete C DLL using the public WinGDI interface. */
typedef void *HANDLE;
typedef unsigned int DWORD;
typedef int LONG;
typedef unsigned char BYTE;
typedef unsigned short WCHAR;

typedef struct tagLOGFONTW {
  LONG lfHeight;
  LONG lfWidth;
  LONG lfEscapement;
  LONG lfOrientation;
  LONG lfWeight;
  BYTE lfItalic;
  BYTE lfUnderline;
  BYTE lfStrikeOut;
  BYTE lfCharSet;
  BYTE lfOutPrecision;
  BYTE lfClipPrecision;
  BYTE lfQuality;
  BYTE lfPitchAndFamily;
  WCHAR lfFaceName[32];
} LOGFONTW;

__declspec(dllimport) HANDLE __stdcall CreateFontIndirectW(const LOGFONTW *font);

static LOGFONTW default_font = {16, 0, 0, 0, 400, 0, 0, 0, 1, 0, 0, 0, 0,
                               {'S', 'a', 'n', 's', 0}};
volatile HANDLE font_result;

__declspec(dllexport) __declspec(noinline)
HANDLE adjust_font(LOGFONTW *font, LONG height) {
  font->lfHeight = height;
  ++font->lfWidth;
  font->lfWeight--;
  return CreateFontIndirectW(font);
}

__declspec(dllexport) int __stdcall DllMain(HANDLE module, DWORD reason, void *reserved) {
  font_result = adjust_font(&default_font, 18 + (LONG)(reason & 3));
  return module != 0 || reason != 0 || reserved != 0;
}
