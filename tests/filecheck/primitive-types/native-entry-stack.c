//
// This file is distributed under the MIT License. See LICENSE.md for details.
//

// RUN: clang --target=i686-w64-windows-gnu -std=gnu2x -O2 -fms-extensions -fno-inline -DREVNG_NATIVE_WINDOWS_I386_ENTRY_SP -I %root/share/revng/include -S -emit-llvm %s -o - | FileCheck %s
// RUN: not clang --target=x86_64-w64-windows-gnu -std=gnu2x -O2 -fms-extensions -fno-inline -DREVNG_NATIVE_WINDOWS_I386_ENTRY_SP -I %root/share/revng/include -fsyntax-only %s 2>&1 | FileCheck %s --check-prefix=TARGET
// RUN: not clang --target=i686-w64-windows-gnu -std=gnu2x -O2 -fms-extensions -DREVNG_NATIVE_WINDOWS_I386_ENTRY_SP -I %root/share/revng/include -fsyntax-only %s 2>&1 | FileCheck %s --check-prefix=INLINE
// RUN: not clang --target=i686-w64-windows-gnu -std=gnu2x -O2 -fno-inline -DREVNG_NATIVE_WINDOWS_I386_ENTRY_SP -I %root/share/revng/include -fsyntax-only %s 2>&1 | FileCheck %s --check-prefix=INTRINSIC

// This represents the declaration emitted by the normal helper header.
unsigned int revng_undefined_local_sp(void);
#include "native-entry-stack.h"

unsigned int entry_stack(void) {
  return revng_undefined_local_sp();
}
unsigned int entry_relative(void) {
  return revng_undefined_local_sp() + 8U;
}
unsigned int temporary_stack(void) {
  return revng_undefined_local_sp() - 16U;
}

// CHECK: define {{.*}}i32 @entry_stack(
// CHECK: call ptr @llvm.addressofreturnaddress.p0()
// CHECK: define {{.*}}i32 @entry_relative(
// CHECK: call ptr @llvm.addressofreturnaddress.p0()
// CHECK: define {{.*}}i32 @temporary_stack(
// CHECK: call ptr @llvm.addressofreturnaddress.p0()
// CHECK-NOT: declare i32 @revng_undefined_local_sp
// TARGET: error: Native entry-SP lowering requires a Windows i386 target
// INLINE: error: Native entry-SP lowering requires -fno-inline
// INTRINSIC: error: Native entry-SP lowering requires -fms-extensions
