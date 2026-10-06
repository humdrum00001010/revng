/* Independently authored complete freestanding C input. */
const unsigned public_selector = 1;
const unsigned public_values[3] = {5, 9, 13};
volatile unsigned public_result;
void _start(void) {
  public_result = public_values[public_selector];
  for (;;) {}
}
