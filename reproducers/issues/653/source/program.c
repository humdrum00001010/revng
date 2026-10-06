/* The compiler emits a genuine x86 LDMXCSR instruction from this C builtin. */
volatile unsigned public_mxcsr = 0x1f80;
volatile unsigned public_result;
__attribute__((noinline)) unsigned configure_mxcsr(unsigned value) {
  __builtin_ia32_ldmxcsr(value);
  return __builtin_ia32_stmxcsr();
}
void _start(void) {
  public_result = configure_mxcsr(public_mxcsr);
  for (;;) {}
}
