/* Independent complete C DLL: declaration-level const pointer typedefs. */
typedef void *HANDLE;
typedef unsigned int DWORD;
typedef const unsigned char *ByteView;
typedef ByteView const FixedByteView;
static const unsigned char sample[] = {11, 23, 37, 41};
volatile unsigned int observed;
__declspec(noinline) unsigned int read_view(FixedByteView view, unsigned int index) {
  return view[index & 3U];
}
__declspec(noinline) unsigned int pass_view(ByteView const view) {
  return read_view(view, 2U) + read_view(view, 3U);
}
__declspec(dllexport) int __stdcall DllMain(HANDLE module, DWORD reason, void *reserved) {
  FixedByteView view = sample;
  observed = pass_view(view);
  return module != 0 || reason != 0 || reserved != 0;
}
