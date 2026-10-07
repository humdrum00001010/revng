//
// This file is distributed under the MIT License. See LICENSE.md for details.
//

// RUN: %root/bin/revng clift-opt %s --emit-field-accesses --canonicalize | FileCheck %s

!void = !clift.void
!generic64_t = !clift.int<generic 8>
!generic32_t = !clift.int<generic 4>
!int32_t = !clift.int<signed 4>
!int32_t$const = !clift.const<!int32_t>

!u_generic_const = !clift.union<
  "2" : {
    "" : !generic32_t,
    "" : !int32_t$const
  }
>

!s_generic_const = !clift.struct<
  "1" : size(8) {
    "" : offset(0) !int32_t,
    "" : offset(4) !u_generic_const
  }
>

!u_const_exact = !clift.union<
  "4" : {
    "" : !int32_t$const,
    "" : !int32_t
  }
>

!s_const_exact = !clift.struct<
  "3" : size(8) {
    "" : offset(0) !int32_t,
    "" : offset(4) !u_const_exact
  }
>

!s_plain = !clift.struct<
  "5" : size(8) {
    "" : offset(0) !int32_t,
    "" : offset(4) !int32_t
  }
>

!s_nested = !clift.struct<
  "6" : size(12) {
    "" : offset(0) !s_plain,
    "" : offset(8) !int32_t
  }
>

!f = !clift.func<"1000" as "f" : !void()>
!g = !clift.func<"1001" as "g" : !void()>

module attributes {clift.module} {

  // Access to the union field via an `int32_t` pointer.
  // `const int32_t` (alt 1, distance 1) beats `generic32_t` (alt 0,
  // distance max).
  clift.func @test_off_by_const_beats_unrelated<!f>() {
    %0 = clift.local : !s_generic_const
    clift.expr {
      %1 = clift.addressof %0 : !clift.ptr<8 to !s_generic_const>
      %2 = clift.bitcast %1 : !clift.ptr<8 to !s_generic_const> -> !clift.ptr<8 to !void>
      %3 = clift.bitcast %2 : !clift.ptr<8 to !void> -> !generic64_t
      %4 = clift.imm 4 : !generic64_t
      %5 = clift.add %3, %4 : !generic64_t
      %6 = clift.bitcast %5 : !generic64_t -> !clift.ptr<8 to !int32_t>
      clift.yield %6 : !clift.ptr<8 to !int32_t>
    }
  }

  // CHECK-LABEL: clift.func @test_off_by_const_beats_unrelated
  // CHECK: [[STRUCT:%[0-9]+]] = clift.local : !_1_
  // CHECK: [[ADDRESSOF1:%[0-9]+]] = clift.addressof [[STRUCT]] : !clift.ptr<8 to !_1_>
  // CHECK: [[ACCESS1:%[0-9]+]] = clift.ptr_access<1> [[ADDRESSOF1]]
  // CHECK: [[ACCESS2:%[0-9]+]] = clift.access<1> [[ACCESS1]]
  // CHECK: [[ADDRESSOF2:%[0-9]+]] = clift.addressof [[ACCESS2]]
  // CHECK: [[CAST:%[0-9]+]] = clift.bitcast [[ADDRESSOF2]]
  // CHECK: clift.yield [[CAST]] : !clift.ptr<8 to !int32_t>


  // Access to the union field via an `int32_t` pointer.
  // `int32_t` (alt 1, distance 0) beats `const int32_t` (alt 0, distance 1).
  clift.func @test_exact_match_beats_off_by_const<!g>() {
    %0 = clift.local : !s_const_exact
    clift.expr {
      %1 = clift.addressof %0 : !clift.ptr<8 to !s_const_exact>
      %2 = clift.bitcast %1 : !clift.ptr<8 to !s_const_exact> -> !clift.ptr<8 to !void>
      %3 = clift.bitcast %2 : !clift.ptr<8 to !void> -> !generic64_t
      %4 = clift.imm 4 : !generic64_t
      %5 = clift.add %3, %4 : !generic64_t
      %6 = clift.bitcast %5 : !generic64_t -> !clift.ptr<8 to !int32_t>
      clift.yield %6 : !clift.ptr<8 to !int32_t>
    }
  }

  // CHECK-LABEL: clift.func @test_exact_match_beats_off_by_const
  // CHECK: [[STRUCT:%[0-9]+]] = clift.local : !_3_
  // CHECK: [[ADDRESSOF1:%[0-9]+]] = clift.addressof [[STRUCT]] : !clift.ptr<8 to !_3_>
  // CHECK: [[ACCESS1:%[0-9]+]] = clift.ptr_access<1> [[ADDRESSOF1]]
  // CHECK: [[ACCESS2:%[0-9]+]] = clift.access<1> [[ACCESS1]]
  // CHECK: [[ADDRESSOF2:%[0-9]+]] = clift.addressof [[ACCESS2]]
  // CHECK: clift.yield [[ADDRESSOF2]] : !clift.ptr<8 to !int32_t>


  // Const on the enclosing object must propagate through both the indirect
  // access to its nested struct and the direct access to that struct's member.
  clift.func @test_const_object_propagates_to_nested_member<!f>() {
    %0 = clift.local : !clift.const<!s_nested>
    clift.expr {
      %1 = clift.addressof %0 : !clift.ptr<8 to !clift.const<!s_nested>>
      %2 = clift.bitcast %1 : !clift.ptr<8 to !clift.const<!s_nested>> -> !generic64_t
      %3 = clift.imm 4 : !generic64_t
      %4 = clift.add %2, %3 : !generic64_t
      %5 = clift.bitcast %4 : !generic64_t -> !clift.ptr<8 to !int32_t$const>
      clift.yield %5 : !clift.ptr<8 to !int32_t$const>
    }
  }

  // CHECK-LABEL: clift.func @test_const_object_propagates_to_nested_member
  // CHECK: [[CONST_OBJECT:%[0-9]+]] = clift.local : !clift.const<!_6_>
  // CHECK: [[CONST_POINTER:%[0-9]+]] = clift.addressof [[CONST_OBJECT]] : !clift.ptr<8 to !clift.const<!_6_>>
  // CHECK: [[CONST_NESTED:%[0-9]+]] = clift.ptr_access<0> [[CONST_POINTER]] : !clift.ptr<8 to !clift.const<!_6_>> -> !clift.const<!_5_>
  // CHECK: [[CONST_MEMBER:%[0-9]+]] = clift.access<1> [[CONST_NESTED]] : !clift.const<!_5_> -> !clift.const<!int32_t>
  // CHECK: [[CONST_MEMBER_ADDRESS:%[0-9]+]] = clift.addressof [[CONST_MEMBER]] : !clift.ptr<8 to !clift.const<!int32_t>>
  // CHECK: clift.yield [[CONST_MEMBER_ADDRESS]] : !clift.ptr<8 to !clift.const<!int32_t>>


  // Const on a pointer value does not qualify the object it points to.
  clift.func @test_const_pointer_preserves_mutable_member<!f>() {
    %0 = clift.local : !clift.const<!clift.ptr<8 to !s_plain>>
    clift.expr {
      %1 = clift.bitcast %0 : !clift.const<!clift.ptr<8 to !s_plain>> -> !generic64_t
      %2 = clift.imm 4 : !generic64_t
      %3 = clift.add %1, %2 : !generic64_t
      %4 = clift.bitcast %3 : !generic64_t -> !clift.ptr<8 to !int32_t>
      clift.yield %4 : !clift.ptr<8 to !int32_t>
    }
  }

  // CHECK-LABEL: clift.func @test_const_pointer_preserves_mutable_member
  // CHECK: [[CONST_POINTER_VALUE:%[0-9]+]] = clift.local : !clift.const<!clift.ptr<8 to !_5_>>
  // CHECK: [[MUTABLE_MEMBER:%[0-9]+]] = clift.ptr_access<1> [[CONST_POINTER_VALUE]] : !clift.const<!clift.ptr<8 to !_5_>> -> !int32_t
  // CHECK: [[MUTABLE_MEMBER_ADDRESS:%[0-9]+]] = clift.addressof [[MUTABLE_MEMBER]] : !clift.ptr<8 to !int32_t>
  // CHECK: clift.yield [[MUTABLE_MEMBER_ADDRESS]] : !clift.ptr<8 to !int32_t>
}
