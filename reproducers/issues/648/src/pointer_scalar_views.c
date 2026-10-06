/* Independently authored full binary input. No rev.ng model or IR is supplied. */
typedef unsigned int u32;
struct Small { u32 value; };
struct Wide { u32 value; u32 extra; };
union Storage {
  double scalar;
  struct Small *small;
  struct Wide *wide;
};
union Target {
  struct Small small;
  struct Wide wide;
};
_Static_assert(sizeof(void *) == 4, "This fixture is compiled for i386");
_Static_assert(sizeof(double) == 8, "Wider scalar memory view");
_Static_assert(sizeof(union Storage) == 8, "Union storage must retain scalar width");

__attribute__((noinline)) double scalar_view(volatile double *slot) {
  return *slot;
}
__attribute__((noinline)) u32 small_target(struct Small *target) {
  return target->value;
}
__attribute__((noinline)) u32 wide_target(struct Wide *target) {
  return target->value + target->extra;
}
__attribute__((noinline)) double overlapping_views(void *slot) {
  double scalar = scalar_view((volatile double *) slot);
  struct Small *small = *(struct Small *volatile *) slot;
  struct Wide *wide = *(struct Wide *volatile *) slot;
  return scalar + (double) small_target(small) + (double) wide_target(wide);
}

static union Target target = { .wide = {17, 23} };
static union Storage storage;
static volatile double observation;

__attribute__((noreturn)) void _start(void) {
  storage.scalar = 0.0;
  storage.wide = &target.wide;
  observation = overlapping_views(&storage);
  for (;;) {
    __asm__ volatile("pause");
  }
}
