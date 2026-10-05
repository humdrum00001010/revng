//
// This file is distributed under the MIT License. See LICENSE.md for details.
//

// RUN: clang --target=i686-w64-windows-gnu -std=gnu2x -O0 -S -emit-llvm -I %root/share/revng/include %s -o - | FileCheck %s
// RUN: not clang --target=i686-w64-windows-gnu -std=gnu2x -fsyntax-only -DTEST_POINTER -I %root/share/revng/include %s 2>&1 | FileCheck %s --check-prefix=UNSUPPORTED
// RUN: not clang --target=i686-w64-windows-gnu -std=gnu2x -fsyntax-only -DTEST_AGGREGATE -I %root/share/revng/include %s 2>&1 | FileCheck %s --check-prefix=UNSUPPORTED

#include "primitive-types.h"

#if defined(TEST_POINTER)
void *unsupported_pointer(void) {
  return undef(void *);
}
#elif defined(TEST_AGGREGATE)
typedef struct {
  int value;
} aggregate;

aggregate unsupported_aggregate(void) {
  return undef(aggregate);
}
#else

generic8_t arbitrary8(void) {
  return undef(generic8_t);
}
generic16_t arbitrary16(void) {
  return undef(generic16_t);
}
generic32_t arbitrary32(void) {
  return undef(generic32_t);
}
generic64_t arbitrary64(void) {
  return undef(generic64_t);
}
float arbitrary_float(void) {
  return undef(float);
}
double arbitrary_double(void) {
  return undef(double);
}
typedef unsigned vector4 __attribute__((vector_size(16)));
vector4 arbitrary_vector(void) {
  return undef(vector4);
}

#endif

// CHECK-NOT: @undef_value
// CHECK: freeze i8 poison
// CHECK: freeze i16 poison
// CHECK: freeze i32 poison
// CHECK: freeze i64 poison
// CHECK: freeze float poison
// CHECK: freeze double poison
// CHECK: freeze <4 x i32> poison
// CHECK-NOT: @undef_value
// UNSUPPORTED: error: 1st argument must be a vector, integer or floating point type
