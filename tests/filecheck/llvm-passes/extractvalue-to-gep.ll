;
; This file is distributed under the MIT License. See LICENSE.md for details.
;

; RUN: %root/bin/revng opt %s -extractvalue-to-gep -S -o - | FileCheck %s

; =============================================================================
; =============================================================================
; Tests that extractvalue is turned into a load from an i8 GEP into a copy of
; the aggregate, and crucially that the GEP is only emitted when the accessed
; field is at a non-zero offset. A field at offset 0 must be loaded directly
; from the base pointer, with no `getelementptr ..., i64 0`.
; =============================================================================
; =============================================================================

declare { i64, i16 } @make_struct()

; A struct value produced by a call is spilled to an alloca, then its fields are
; read back. Field 0 (offset 0) must be a direct load from the alloca; field 1
; (offset 8) must go through a GEP with offset 8. No zero-offset GEP may appear.
;
; CHECK-LABEL: define void @struct_from_call
; CHECK: [[ALLOCA:%[a-zA-Z0-9_]+]] = alloca { i64, i16 }
; CHECK: [[STRUCT:%[a-zA-Z0-9_]+]] = call { i64, i16 } @make_struct()
; CHECK: store { i64, i16 } [[STRUCT]], ptr [[ALLOCA]]
; CHECK-NOT: getelementptr i8, ptr [[ALLOCA]], i64 0
; CHECK: [[FIELD0:%[a-zA-Z0-9_]+]] = load i64, ptr [[ALLOCA]]
; CHECK: [[GEP1:%[a-zA-Z0-9_]+]] = getelementptr i8, ptr [[ALLOCA]], i64 8
; CHECK: [[FIELD1:%[a-zA-Z0-9_]+]] = load i16, ptr [[GEP1]]
; CHECK: store i64 [[FIELD0]], ptr null
; CHECK: store i16 [[FIELD1]], ptr null
define void @struct_from_call() {
  %s = call { i64, i16 } @make_struct()
  %f0 = extractvalue { i64, i16 } %s, 0
  %f1 = extractvalue { i64, i16 } %s, 1
  store i64 %f0, ptr null
  store i16 %f1, ptr null
  ret void
}

; A struct value that is already in memory (the aggregate operand is a load)
; reuses the load's pointer as the base, so no extra alloca is created. Field 0
; (offset 0) is a direct load from the base; field 1 (offset 4) goes through a
; GEP with offset 4. Again no zero-offset GEP may appear.
;
; CHECK-LABEL: define void @struct_from_load
; CHECK-NOT: getelementptr i8, ptr %p, i64 0
; CHECK: [[LF0:%[a-zA-Z0-9_]+]] = load i32, ptr %p
; CHECK: [[LGEP:%[a-zA-Z0-9_]+]] = getelementptr i8, ptr %p, i64 4
; CHECK: [[LF1:%[a-zA-Z0-9_]+]] = load i32, ptr [[LGEP]]
; CHECK: store i32 [[LF0]], ptr null
; CHECK: store i32 [[LF1]], ptr null
define void @struct_from_load(ptr %p) {
  %s = load { i32, i32 }, ptr %p
  %f0 = extractvalue { i32, i32 } %s, 0
  %f1 = extractvalue { i32, i32 } %s, 1
  store i32 %f0, ptr null
  store i32 %f1, ptr null
  ret void
}

; =============================================================================
; =============================================================================
; Tests that an extraction whose aggregate has been folded into a constant is
; replaced by the constant field it selects. There is no instruction producing
; the aggregate, so there is nothing to spill to an alloca, but the extraction
; still has to go: `OpaqueExtractValue` is opaque to LLVM, so nothing else
; removes it, and the stages downstream of this pass have no case for it.
; =============================================================================
; =============================================================================

declare !revng.tags !0 i32 @OpaqueExtractvalue({ i32, i1 }, i64)
declare !revng.tags !0 i1 @OpaqueExtractvalue.1({ i32, i1 }, i64)

!0 = !{!"opaque-extract-value", !"uniqued-by-prototype"}

; Both fields of a `zeroinitializer` are known, so both calls must disappear,
; leaving the constants behind. This is the shape instcombine produces when it
; proves a multiplication with overflow check cannot overflow.
;
; CHECK-LABEL: define void @opaque_from_constant
; CHECK-NOT: @OpaqueExtractvalue
; CHECK: store i32 0, ptr null
; CHECK: store i1 false, ptr null
; CHECK-NOT: @OpaqueExtractvalue
define void @opaque_from_constant() {
  %f0 = call i32 @OpaqueExtractvalue({ i32, i1 } zeroinitializer, i64 0)
  %f1 = call i1 @OpaqueExtractvalue.1({ i32, i1 } zeroinitializer, i64 1)
  store i32 %f0, ptr null
  store i1 %f1, ptr null
  ret void
}

; The same holds for a plain `extractvalue`, even though LLVM folds those on
; its own well before this pass runs. No alloca may be created for it.
;
; CHECK-LABEL: define void @plain_from_constant
; CHECK-NOT: alloca
; CHECK: store i32 7, ptr null
; CHECK: store i16 9, ptr null
define void @plain_from_constant() {
  %f0 = extractvalue { i32, i16 } { i32 7, i16 9 }, 0
  %f1 = extractvalue { i32, i16 } { i32 7, i16 9 }, 1
  store i32 %f0, ptr null
  store i16 %f1, ptr null
  ret void
}

; An optimized overflow result can retain a runtime field while its overflow
; bit is constant. Resolve both fields before creating aggregate storage, and
; remove the construction once its extraction users have disappeared.
;
; CHECK-LABEL: define void @opaque_from_insertvalue
; CHECK-NOT: alloca
; CHECK-NOT: insertvalue
; CHECK-NOT: @OpaqueExtractvalue
; CHECK: store i32 %value, ptr null
; CHECK: store i1 false, ptr null
; CHECK-NEXT: ret void
define void @opaque_from_insertvalue(i32 %value) {
  %aggregate = insertvalue { i32, i1 } { i32 poison, i1 false }, i32 %value, 0
  %f0 = call i32 @OpaqueExtractvalue({ i32, i1 } %aggregate, i64 0)
  %f1 = call i1 @OpaqueExtractvalue.1({ i32, i1 } %aggregate, i64 1)
  store i32 %f0, ptr null
  store i1 %f1, ptr null
  ret void
}

; Replacing one poison field does not define a different poison field.
;
; CHECK-LABEL: define void @opaque_preserves_other_poison
; CHECK-NOT: alloca
; CHECK-NOT: insertvalue
; CHECK-NOT: @OpaqueExtractvalue
; CHECK: store i32 %value, ptr null
; CHECK: store i1 poison, ptr null
; CHECK-NEXT: ret void
define void @opaque_preserves_other_poison(i32 %value) {
  %aggregate = insertvalue { i32, i1 } poison, i32 %value, 0
  %f0 = call i32 @OpaqueExtractvalue({ i32, i1 } %aggregate, i64 0)
  %f1 = call i1 @OpaqueExtractvalue.1({ i32, i1 } %aggregate, i64 1)
  store i32 %f0, ptr null
  store i1 %f1, ptr null
  ret void
}

; The latest write wins, disjoint writes are skipped, and untouched fields
; keep the value supplied by the base aggregate.
;
; CHECK-LABEL: define void @plain_multiple_insertions
; CHECK-NOT: alloca
; CHECK-NOT: insertvalue
; CHECK-NOT: extractvalue
; CHECK: store i32 %last, ptr null
; CHECK: store i16 9, ptr null
; CHECK: store i32 %other, ptr null
; CHECK-NEXT: ret void
define void @plain_multiple_insertions(i32 %first, i32 %last, i32 %other) {
  %a = insertvalue { i32, i16, i32 } { i32 7, i16 9, i32 11 }, i32 %first, 0
  %b = insertvalue { i32, i16, i32 } %a, i32 %other, 2
  %c = insertvalue { i32, i16, i32 } %b, i32 %last, 0
  %f0 = extractvalue { i32, i16, i32 } %c, 0
  %f1 = extractvalue { i32, i16, i32 } %c, 1
  %f2 = extractvalue { i32, i16, i32 } %c, 2
  store i32 %f0, ptr null
  store i16 %f1, ptr null
  store i32 %f2, ptr null
  ret void
}

; A nested extraction follows a prefix insertion and then the selected inner
; field. An intervening insertion into a disjoint outer field has no effect.
;
; CHECK-LABEL: define void @plain_nested_prefix
; CHECK-NOT: alloca
; CHECK-NOT: insertvalue
; CHECK-NOT: extractvalue
; CHECK: store i32 %value, ptr null
; CHECK: store i16 13, ptr null
; CHECK: store i32 %other, ptr null
; CHECK-NEXT: ret void
define void @plain_nested_prefix(i32 %value, i32 %other) {
  %inner = insertvalue { i32, i16 } { i32 poison, i16 13 }, i32 %value, 0
  %a = insertvalue { { i32, i16 }, i32 } poison, { i32, i16 } %inner, 0
  %b = insertvalue { { i32, i16 }, i32 } %a, i32 %other, 1
  %f00 = extractvalue { { i32, i16 }, i32 } %b, 0, 0
  %f01 = extractvalue { { i32, i16 }, i32 } %b, 0, 1
  %f1 = extractvalue { { i32, i16 }, i32 } %b, 1
  store i32 %f00, ptr null
  store i16 %f01, ptr null
  store i32 %f1, ptr null
  ret void
}

; Nested writes to a sibling field must also leave the selected field alone.
;
; CHECK-LABEL: define void @plain_nested_disjoint
; CHECK-NOT: alloca
; CHECK-NOT: insertvalue
; CHECK-NOT: extractvalue
; CHECK: store i32 %last, ptr null
; CHECK: store i16 13, ptr null
; CHECK-NEXT: ret void
define void @plain_nested_disjoint(i32 %first, i32 %last, i32 %other) {
  %a = insertvalue { { i32, i16 }, i32 } { { i32, i16 } { i32 7, i16 13 }, i32 17 }, i32 %first, 0, 0
  %b = insertvalue { { i32, i16 }, i32 } %a, i32 %other, 1
  %c = insertvalue { { i32, i16 }, i32 } %b, i32 %last, 0, 0
  %f00 = extractvalue { { i32, i16 }, i32 } %c, 0, 0
  %f01 = extractvalue { { i32, i16 }, i32 } %c, 0, 1
  store i32 %f00, ptr null
  store i16 %f01, ptr null
  ret void
}

declare { i32, i32 } @unknown_struct()

; A field obtained from an unknown producer cannot be replaced by an insertion
; into another field. Keep the existing materialization for the unresolved
; extraction, while resolving the field that was explicitly written.
;
; CHECK-LABEL: define void @unresolved_field
; CHECK: [[UNKNOWN_STORAGE:%[a-zA-Z0-9_]+]] = alloca { i32, i32 }
; CHECK: %unknown = call { i32, i32 } @unknown_struct()
; CHECK: %aggregate = insertvalue { i32, i32 } %unknown, i32 %value, 0
; CHECK: store { i32, i32 } %aggregate, ptr [[UNKNOWN_STORAGE]]
; CHECK: [[UNKNOWN_GEP:%[a-zA-Z0-9_]+]] = getelementptr i8, ptr [[UNKNOWN_STORAGE]], i64 4
; CHECK: [[UNKNOWN_FIELD:%[a-zA-Z0-9_]+]] = load i32, ptr [[UNKNOWN_GEP]]
; CHECK: store i32 %value, ptr null
; CHECK: store i32 [[UNKNOWN_FIELD]], ptr null
define void @unresolved_field(i32 %value) {
  %unknown = call { i32, i32 } @unknown_struct()
  %aggregate = insertvalue { i32, i32 } %unknown, i32 %value, 0
  %f0 = extractvalue { i32, i32 } %aggregate, 0
  %f1 = extractvalue { i32, i32 } %aggregate, 1
  store i32 %f0, ptr null
  store i32 %f1, ptr null
  ret void
}

; Opaque calls do not validate aggregate indices as extractvalue does. Leave
; invalid indices unchanged rather than invoking the resolver with bad indices.
;
; CHECK-LABEL: define void @opaque_invalid_indices
; CHECK-NOT: alloca
; CHECK: %out_of_bounds = call i32 @OpaqueExtractvalue({ i32, i1 } { i32 7, i1 false }, i64 2)
; CHECK: %negative = call i32 @OpaqueExtractvalue({ i32, i1 } { i32 7, i1 false }, i64 {{(-1|u0xffffffffffffffff)}})
; CHECK: %maximum = call i32 @OpaqueExtractvalue({ i32, i1 } { i32 7, i1 false }, i64 {{(4294967295|u0xffffffff)}})
; CHECK: store i32 %out_of_bounds, ptr null
; CHECK: store i32 %negative, ptr null
; CHECK: store i32 %maximum, ptr null
define void @opaque_invalid_indices() {
  %out_of_bounds = call i32 @OpaqueExtractvalue({ i32, i1 } { i32 7, i1 false }, i64 2)
  %negative = call i32 @OpaqueExtractvalue({ i32, i1 } { i32 7, i1 false }, i64 -1)
  %maximum = call i32 @OpaqueExtractvalue({ i32, i1 } { i32 7, i1 false }, i64 4294967295)
  store i32 %out_of_bounds, ptr null
  store i32 %negative, ptr null
  store i32 %maximum, ptr null
  ret void
}
