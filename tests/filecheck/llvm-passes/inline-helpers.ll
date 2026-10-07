;
; This file is distributed under the MIT License. See LICENSE.md for details.
;
; Check that `inline-helpers` consumes the `!revng.inline.policy`
; metadata attached by `detect-uninlinable-helpers`
; at helpers-build time and uses it to gate per-call-site inlining.
; A helper with a non-zero policy must only be inlined at call sites
; where every listed argument is a compile-time constant. A helper
; with a zero policy must be inlined when its prepared body remains safe.
; Helper preparation can introduce critical operands after the static policy
; was computed, so the runtime pass must revalidate the body it will inline.
;
; RUN: %root/bin/revng opt -link-helpers-to-inline -inline-helpers -delete-helper-bodies %s -S -o - | FileCheck %s --implicit-check-not=getelementptr

target datalayout = "e-p:64:64"

!revng.qemu_architecture = !{!0}
!0 = !{!"x86_64"}

; A helper with a source-level switch on arg #1 - the static pass marked
; arg #1 as the single critical operand (bit 1 set in `i2 2`).
;
; Once `inline-helpers` has finished its per-isolated-function pass, the
; helper's body is blanked: the dynamic-op call site is left for the
; linker, so the helper survives as a declaration.
; CHECK: declare {{.*}}@helper_with_critical
define i64 @helper_with_critical(i64 %x, i32 %op) section "revng_inline" !revng.inline.policy !20 {
entry:
  switch i32 %op, label %default [
    i32 0, label %case0
    i32 1, label %case1
  ]
case0:
  ret i64 %x
case1:
  %inc = add i64 %x, 1
  ret i64 %inc
default:
  ret i64 0
}

; A helper with no critical operands — the static pass produced the
; empty BitVector, encoded as `i1 0`. Since all its calls get inlined
; away, the body is blanked and it ends up as a declaration too.
; CHECK: declare {{.*}}@helper_no_critical
define i64 @helper_no_critical(i64 %x) section "revng_inline" !revng.inline.policy !21 {
entry:
  %r = add i64 %x, 42
  ret i64 %r
}

; An Isolated function with three calls to the helpers above: two
; call sites of `@helper_with_critical` - one with a constant `op`
; (inlineable) and one with a dynamic `op` (not inlineable) - plus
; one call to `@helper_no_critical` (always inlined).
; CHECK-LABEL: define i64 @my_isolated_function(i64 %v, i32 %op)
define i64 @my_isolated_function(i64 %v, i32 %op) !revng.tags !10 {
entry:
  ; The constant-op call site must be inlined away.
  ; CHECK-NOT: call i64 @helper_with_critical(i64 %v, i32 0)
  %a = call i64 @helper_with_critical(i64 %v, i32 0)
  ; The dynamic-op call site must survive.
  ; CHECK: call i64 @helper_with_critical(i64 %v, i32 %op)
  %b = call i64 @helper_with_critical(i64 %v, i32 %op)
  ; The no-critical-args call must be inlined away.
  ; CHECK-NOT: call i64 @helper_no_critical
  %c = call i64 @helper_no_critical(i64 %v)
  %ab = add i64 %a, %b
  %abc = add i64 %ab, %c
  ret i64 %abc
}

; A prepared helper acquired a table lookup from a nested helper after the
; static producer attached an always-inline policy. Its index now depends on
; a runtime CSV load, even when the caller passes a constant argument.
@runtime_index = global i32 0
@rounding_table = external global [4 x i8]

; CHECK: declare {{.*}}@helper_prepared_runtime
define i8 @helper_prepared_runtime(i32 %op) section "revng_inline" !revng.inline.policy !21 {
entry:
  store i32 %op, ptr @runtime_index
  %loaded = load i32, ptr @runtime_index
  %index = and i32 %loaded, 3
  %wide = zext i32 %index to i64
  %element = getelementptr [4 x i8], ptr @rounding_table, i64 0, i64 %wide
  %value = load i8, ptr %element
  ret i8 %value
}

; CSV wrappers can still inline, exposing the original helper call whose
; summary and opaque runtime implementation must survive.
; CHECK: declare {{.*}}@helper_prepared_runtime_wrapper
define i8 @helper_prepared_runtime_wrapper(i32 %op) section "revng_inline" !revng.inline.policy !21 {
entry:
  %value = call i8 @helper_prepared_runtime(i32 %op)
  ret i8 %value
}

; CHECK-LABEL: define i8 @reject_prepared_runtime(i32 %op)
; CHECK-NOT: @rounding_table
; CHECK-NOT: @runtime_index
; CHECK-NOT: call i8 @helper_prepared_runtime_wrapper
; CHECK: call i8 @helper_prepared_runtime(i32 %op)
; CHECK-NEXT: {{.*}}call i8 @helper_prepared_runtime(i32 2)
; CHECK-NOT: @rounding_table
; CHECK-NOT: @runtime_index
; CHECK-NOT: call i8 @helper_prepared_runtime_wrapper
; CHECK: ret i8
define i8 @reject_prepared_runtime(i32 %op) !revng.tags !10 {
entry:
  %dynamic = call i8 @helper_prepared_runtime_wrapper(i32 %op)
  %constant = call i8 @helper_prepared_runtime_wrapper(i32 2)
  %result = add i8 %dynamic, %constant
  ret i8 %result
}

; Revalidation also discovers new critical formal arguments. Dynamic calls
; must stay opaque, while constants still permit safe specialization.
; CHECK: declare {{.*}}@helper_prepared_argument
define i64 @helper_prepared_argument(ptr %base, i64 %index) section "revng_inline" !revng.inline.policy !22 {
entry:
  %element = getelementptr i64, ptr %base, i64 %index
  %address = ptrtoint ptr %element to i64
  ret i64 %address
}

; CHECK-LABEL: define i64 @specialize_prepared_argument(i64 %index)
; CHECK: call i64 @helper_prepared_argument(ptr inttoptr (i64 {{(4096|u0x1000)}} to ptr), i64 %index)
; CHECK-NOT: call i64 @helper_prepared_argument
; CHECK: add i64 {{.*}}, {{(4120|u0x1018)}}
; CHECK: ret i64
define i64 @specialize_prepared_argument(i64 %index) !revng.tags !10 {
entry:
  %dynamic = call i64 @helper_prepared_argument(ptr inttoptr (i64 4096 to ptr), i64 %index)
  %constant = call i64 @helper_prepared_argument(ptr inttoptr (i64 4096 to ptr), i64 3)
  %result = add i64 %dynamic, %constant
  ret i64 %result
}

; A constant index is insufficient when the pointer base stays dynamic.
; CHECK-LABEL: define i64 @reject_prepared_base(ptr %base)
; CHECK: call i64 @helper_prepared_argument(ptr %base, i64 3)
; CHECK: ret i64
define i64 @reject_prepared_base(ptr %base) !revng.tags !10 {
entry:
  %address = call i64 @helper_prepared_argument(ptr %base, i64 3)
  ret i64 %address
}

; Prepared SIMD helpers can retain local array temporaries. Their GEPs do not
; disappear just because the indices are constants.
; CHECK: declare {{.*}}@helper_prepared_local
define i8 @helper_prepared_local(i8 %x) section "revng_inline" !revng.inline.policy !21 {
entry:
  %storage = alloca [2 x i8]
  %element = getelementptr [2 x i8], ptr %storage, i64 0, i64 1
  store i8 %x, ptr %element
  %value = load i8, ptr %element
  ret i8 %value
}

; CHECK-LABEL: define i8 @reject_prepared_local(i8 %x)
; CHECK-NOT: alloca
; CHECK: call i8 @helper_prepared_local(i8 %x)
; CHECK: ret i8
define i8 @reject_prepared_local(i8 %x) !revng.tags !10 {
entry:
  %value = call i8 @helper_prepared_local(i8 %x)
  ret i8 %value
}

; Constant-only induction cycles do not make a GEP index constant. This case
; has a constant base and no alloca, so it exercises the index independently.
; CHECK: declare {{.*}}@helper_prepared_loop
define i64 @helper_prepared_loop(ptr %base) section "revng_inline" !revng.inline.policy !21 {
entry:
  br label %loop
loop:
  %index = phi i64 [ 0, %entry ], [ %next, %loop ]
  %element = getelementptr i8, ptr %base, i64 %index
  %next = add i64 %index, 1
  %again = icmp ult i64 %next, 4
  br i1 %again, label %loop, label %exit
exit:
  %address = ptrtoint ptr %element to i64
  ret i64 %address
}

; CHECK-LABEL: define i64 @reject_prepared_loop()
; CHECK: call i64 @helper_prepared_loop(ptr inttoptr (i64 {{(4096|u0x1000)}} to ptr))
; CHECK: ret i64
define i64 @reject_prepared_loop() !revng.tags !10 {
entry:
  %address = call i64 @helper_prepared_loop(ptr inttoptr (i64 4096 to ptr))
  ret i64 %address
}

!10 = !{!"isolated"}
!20 = !{i3 2}
!21 = !{i2 0}
!22 = !{i3 0}
