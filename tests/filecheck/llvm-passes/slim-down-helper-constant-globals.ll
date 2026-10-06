;
; This file is distributed under the MIT License. See LICENSE.md for details.
;
; Eligibility relies on an immutable initializer remaining available after
; slimming. External constant declarations do not provide that evidence.
;
; RUN: split-file %s %t
; RUN: %root/bin/revng opt -enable-new-pm=0 -detect-uninlinable-helpers %t/helpers.ll -o %t/full.bc
; RUN: %root/bin/revng opt -enable-new-pm=0 -slim-down-helpers-module -sroa -instsimplify -simplifycfg -strip-dead-debug-info %t/full.bc -o %t/inline.bc
; RUN: %root/bin/revng opt -enable-new-pm=0 -verify %t/callers.ll -o %t/callers.bc
; RUN: %root/bin/revng merge llvm %t/inline.bc %t/callers.bc -o %t/linked.bc
; RUN: %root/bin/revng opt -enable-new-pm=0 -inline-helpers -delete-helper-bodies -verify -S %t/linked.bc -o - | FileCheck %s
;
; CHECK-LABEL: define i32 @constant_export()
; CHECK: ret i32 9
; CHECK-LABEL: define i32 @arithmetic_export(i32 %input)
; CHECK: add i32 %input, 1
; CHECK-LABEL: define i32 @external_export()
; CHECK: call i32 @read_external_index()
; CHECK-LABEL: define i32 @mutable_export()
; CHECK: call i32 @read_mutable_index()

;--- helpers.ll
target datalayout = "e-m:e-p:32:32-i64:64-n8:16:32-S128"
target triple = "i386-unknown-linux-gnu"
@selector = internal constant i32 1
@values = internal constant [3 x i32] [i32 5, i32 9, i32 13]
@external_selector = external constant i32
@mutable_selector = global i32 2
define i32 @read_constant_index() section "revng_inline" {
  %index = load i32, ptr @selector, align 4
  %address = getelementptr [3 x i32], ptr @values, i32 0, i32 %index
  %value = load i32, ptr %address, align 4
  ret i32 %value
}
define i32 @add_one(i32 %value) section "revng_inline" {
  %result = add i32 %value, 1
  ret i32 %result
}
define i32 @read_external_index() section "revng_inline" {
  %index = load i32, ptr @external_selector, align 4
  %address = getelementptr [3 x i32], ptr @values, i32 0, i32 %index
  %value = load i32, ptr %address, align 4
  ret i32 %value
}
define i32 @read_mutable_index() section "revng_inline" {
  %index = load i32, ptr @mutable_selector, align 4
  %address = getelementptr [3 x i32], ptr @values, i32 0, i32 %index
  %value = load i32, ptr %address, align 4
  ret i32 %value
}

;--- callers.ll
target datalayout = "e-m:e-p:32:32-i64:64-n8:16:32-S128"
target triple = "i386-unknown-linux-gnu"
declare i32 @read_constant_index()
declare i32 @add_one(i32)
declare i32 @read_external_index()
declare i32 @read_mutable_index()
define i32 @constant_export() !revng.tags !0 {
  %value = call i32 @read_constant_index()
  ret i32 %value
}
define i32 @arithmetic_export(i32 %input) !revng.tags !0 {
  %value = call i32 @add_one(i32 %input)
  ret i32 %value
}
define i32 @external_export() !revng.tags !0 {
  %value = call i32 @read_external_index()
  ret i32 %value
}
define i32 @mutable_export() !revng.tags !0 {
  %value = call i32 @read_mutable_index()
  ret i32 %value
}
!0 = !{!"isolated"}
