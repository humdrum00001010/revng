/* Independent complete DLL probing architecture-unspecified result values.
 * The empty asm has an output register but no memory access or input contract.
 * No runtime case is executed; the real binary may return prior EAX contents.
 */
typedef void *HANDLE;
typedef unsigned int DWORD;
volatile unsigned int observed;
__declspec(noinline) unsigned int unspecified_register(void) {
  unsigned int value;
  __asm__ volatile ("" : "=a" (value));
  return value;
}
__declspec(noinline) unsigned int unspecified_flag(unsigned int dividend) {
  unsigned char overflow;
  unsigned int quotient = dividend;
  unsigned int divisor = 3;
  __asm__ volatile ("xorl %%edx, %%edx; divl %2; seto %0"
                   : "=qm" (overflow), "+a" (quotient)
                   : "r" (divisor) : "edx", "cc");
  return overflow;
}
__declspec(dllexport) int __stdcall DllMain(HANDLE module, DWORD reason, void *reserved) {
  observed = unspecified_register() + unspecified_flag(reason + 7U);
  return module != 0 || reason != 0 || reserved != 0;
}
