;
; This file is distributed under the MIT License. See LICENSE.md for details.
;
; Complete synthetic consumer, with both exported roots.
target datalayout = "e-m:e-p:32:32-i64:64-n8:16:32-S128"
target triple = "i386-unknown-linux-gnu"
@runtime_index = global i32 0

declare i32 @read_runtime_index()
declare i32 @add_one(i32)

define i32 @runtime_export() !revng.tags !0 {
  %value = call i32 @read_runtime_index()
  ret i32 %value
}

define i32 @arithmetic_export(i32 %input) !revng.tags !0 {
  %result = call i32 @add_one(i32 %input)
  ret i32 %result
}
!0 = !{!"isolated"}
