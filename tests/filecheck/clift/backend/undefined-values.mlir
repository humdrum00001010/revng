//
// This file is distributed under the MIT License. See LICENSE.md for details.
//

// RUN: %root/bin/revng clift-opt --emit-c %s -o /dev/null | clang -std=gnu2x -x c -O0 -S -emit-llvm -include %root/share/revng/include/primitive-types.h -o - - | FileCheck %s

!int32_t = !clift.int<signed 4>
!f = !clift.func<"/type-definition/1-CABIFunctionDefinition" : !int32_t()>

module attributes {clift.module} {
  clift.func @sample<!f>() attributes {
    handle = "/function/0x400000:Code_x86_64"
  } {
    clift.return {
      %value = clift.undef : !int32_t
      clift.yield %value : !int32_t
    }
  }
}

// CHECK-NOT: @undef_value
// CHECK: define {{.*}}i32 @sample(
// CHECK: freeze i32 poison
// CHECK: ret i32
// CHECK-NOT: @undef_value
