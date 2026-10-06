/* Independently authored complete C DLL: padded packed field candidates. */
typedef void *HANDLE;
typedef unsigned int DWORD;

#pragma pack(push, 1)
struct ExtendedRecord {
  unsigned char prefix;
  long double value;
};
struct NarrowRecord {
  unsigned char prefix;
  unsigned _BitInt(24) value;
};
struct BooleanRecord {
  unsigned char prefix;
  _Bool value;
};
#pragma pack(pop)

static volatile struct ExtendedRecord extended_record = {3, 1.25L};
static volatile struct NarrowRecord narrow_record = {5, 0x123456};
static volatile struct BooleanRecord boolean_record = {7, 1};

__declspec(dllexport) __declspec(noinline)
void update_extended(volatile struct ExtendedRecord *record, long double value) {
  record->prefix += 1;
  record->value += value;
}

__declspec(dllexport) __declspec(noinline)
void update_narrow(volatile struct NarrowRecord *record, unsigned _BitInt(24) value) {
  record->prefix += 1;
  record->value ^= value;
}

__declspec(dllexport) __declspec(noinline)
void update_boolean(volatile struct BooleanRecord *record, _Bool value) {
  record->prefix += 1;
  record->value = value;
}

__declspec(dllexport) int __stdcall DllMain(HANDLE module, DWORD reason, void *reserved) {
  update_extended(&extended_record, 0.5L);
  update_narrow(&narrow_record, 0xabcdef);
  update_boolean(&boolean_record, reason != 0);
  return module != 0 || reason != 0 || reserved != 0;
}
