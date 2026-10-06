//
// This file is distributed under the MIT License. See LICENSE.md for details.
//

// A complete module must remain compilable when LLVM writes use const views.
// The read-only consumer declaration stays qualified. A writable aggregate view
// at the write preserves packed field alignment without changing the interface.
//
// RUN: %root/bin/revng clift-opt %s --emit-c -o /dev/null > %t.c
// RUN: %root/bin/revng clift-opt %s --emit-type-and-global-header -o /dev/null > %t.h
// RUN: FileCheck %s --check-prefix=HEADER < %t.h
// RUN: clang --target=i686-w64-windows-gnu -std=gnu2x -ffreestanding -fno-strict-overflow -fno-strict-aliasing -O0 -I %root/share/revng/include -include %t.h -c %t.c -o %t.obj
// RUN: llvm-nm --defined-only %t.obj | FileCheck %s --check-prefix=COFF
// RUN: clang --target=i686-w64-windows-gnu -std=gnu2x -ffreestanding -fno-strict-overflow -fno-strict-aliasing -O0 -I %root/share/revng/include -include %t.h -S -emit-llvm %t.c -o - | FileCheck %s --check-prefix=STORE
//
// HEADER: void readonly_consumer(const Record *view);
// HEADER: void write_field(const Record *view, int32_t value);
// HEADER: void write_direct(const Record *view, int32_t value);
// HEADER: void increment_field(const Record *view, int32_t value);
// HEADER: void decrement_field(const Record *view, int32_t value);
//
// COFF-DAG: T _write_field
// COFF-DAG: T _write_direct
// COFF-DAG: T _increment_field
// COFF-DAG: T _decrement_field
//
// STORE-LABEL: define{{.*}} @write_field(
// STORE: store i32 {{.*}}align 1
// STORE-LABEL: define{{.*}} @write_direct(
// STORE: store i32 {{.*}}align 1
// STORE-LABEL: define{{.*}} @increment_field(
// STORE: store i32 {{.*}}align 1
// STORE-LABEL: define{{.*}} @decrement_field(
// STORE: store i32 {{.*}}align 1

!void = !clift.void
!i32 = !clift.int<signed 4>
!u8 = !clift.int<unsigned 1>
!Record = !clift.struct<"/type-definition/10-StructDefinition" as "Record" : size(5) {
  "/struct-field/10-StructDefinition/0" as "prefix" : offset(0) !u8,
  "/struct-field/10-StructDefinition/1" as "value" : offset(1) !i32
}>
!View = !clift.ptr<4 to !clift.const<!Record>>
!Write = !clift.func<"/type-definition/20-CABIFunctionDefinition" as "WriterType" : !void(!View, !i32)>
!Read = !clift.func<"/type-definition/21-CABIFunctionDefinition" as "ReaderType" : !void(!View)>
#data_model = #clift.data_model<pointer = 4, long = 4, long double = 10>
module attributes {clift.module, clift.data_model = #data_model} {
  clift.func @readonly_consumer<!Read>(%p : !View {clift.name = "view"}) attributes {handle = "/function/0x1000:Code_x86"}
  clift.func @write_field<!Write>(%p : !View {clift.name = "view"}, %v : !i32 {clift.name = "value"}) attributes {handle = "/function/0x2000:Code_x86"} {
    clift.expr {
      %field = clift.ptr_access<1> %p : !View -> !i32
      %write = clift.assign %field, %v : !i32
      clift.yield %write : !i32
    }
    clift.expr {
      %reader = clift.use @readonly_consumer : !Read
      %call = clift.call %reader(%p) : !Read
      clift.yield %call : !void
    }
    clift.return {}
  }
  clift.func @write_direct<!Write>(%p : !View {clift.name = "view"}, %v : !i32 {clift.name = "value"}) attributes {handle = "/function/0x3000:Code_x86"} {
    clift.expr {
      %object = clift.indirection %p : !View
      %field = clift.access<1> %object : !clift.const<!Record> -> !i32
      %write = clift.assign %field, %v : !i32
      clift.yield %write : !i32
    }
    clift.expr {
      %reader = clift.use @readonly_consumer : !Read
      %call = clift.call %reader(%p) : !Read
      clift.yield %call : !void
    }
    clift.return {}
  }
  clift.func @increment_field<!Write>(%p : !View {clift.name = "view"}, %v : !i32 {clift.name = "value"}) attributes {handle = "/function/0x4000:Code_x86"} {
    clift.expr {
      %field = clift.ptr_access<1> %p : !View -> !i32
      %write = clift.inc %field : !i32
      clift.yield %write : !i32
    }
    clift.expr {
      %reader = clift.use @readonly_consumer : !Read
      %call = clift.call %reader(%p) : !Read
      clift.yield %call : !void
    }
    clift.return {}
  }
  clift.func @decrement_field<!Write>(%p : !View {clift.name = "view"}, %v : !i32 {clift.name = "value"}) attributes {handle = "/function/0x5000:Code_x86"} {
    clift.expr {
      %field = clift.ptr_access<1> %p : !View -> !i32
      %write = clift.post_dec %field : !i32
      clift.yield %write : !i32
    }
    clift.expr {
      %reader = clift.use @readonly_consumer : !Read
      %call = clift.call %reader(%p) : !Read
      clift.yield %call : !void
    }
    clift.return {}
  }
}
