/* Independently authored complete freestanding C input. */
volatile unsigned public_values[4] = {5, 9, 13, 17};
volatile unsigned public_result;
__attribute__((noinline)) unsigned read_second(const unsigned *base) {
  return base[1];
}
void _start(void) {
  public_result = read_second((const unsigned *)public_values);
  for (;;) {}
}
