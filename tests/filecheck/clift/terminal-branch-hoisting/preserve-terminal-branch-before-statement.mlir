//
// This file is distributed under the MIT License. See LICENSE.md for details.
//

// RUN: %root/bin/revng clift-opt %s --hoist-terminal-branches | FileCheck %s

!void = !clift.void
!int32_t = !clift.int<signed 4>
!f = !clift.func<"" : !void(!int32_t)>

// An indirectly terminal branch can precede another statement. Hoisting one
// of its terminal regions must not expose a direct jump before that statement.
module attributes {clift.module} {
  // CHECK-LABEL: clift.func @jump_suffix
  clift.func @jump_suffix<!f>(%arg0 : !int32_t) -> !void {
    // CHECK: %[[BREAK:.*]] = clift.make_label
    %break = clift.make_label
    // CHECK: %[[CONTINUE:.*]] = clift.make_label
    %continue = clift.make_label
    clift.for break %break continue %continue body {
      clift.if {
        %condition = clift.test %arg0 : !int32_t
        clift.yield %condition : !clift.bool
      // CHECK: } then {
      } then {
        // CHECK-NEXT: clift.continue_to %[[CONTINUE]]
        clift.continue_to %continue
      // CHECK-NEXT: } else {
      } else {
        // CHECK-NEXT: clift.break_to %[[BREAK]]
        clift.break_to %break
      // CHECK-NEXT: }
      }
      // CHECK-NEXT: clift.break_to %[[BREAK]]
      clift.break_to %break
    }
  }

  // CHECK-LABEL: clift.func @expression_suffix
  clift.func @expression_suffix<!f>(%arg0 : !int32_t) -> !void {
    // CHECK: %[[BREAK:.*]] = clift.make_label
    %break = clift.make_label
    // CHECK: %[[CONTINUE:.*]] = clift.make_label
    %continue = clift.make_label
    clift.for break %break continue %continue body {
      clift.if {
        %condition = clift.test %arg0 : !int32_t
        clift.yield %condition : !clift.bool
      // CHECK: } then {
      } then {
        // CHECK-NEXT: clift.continue_to %[[CONTINUE]]
        clift.continue_to %continue
      // CHECK-NEXT: } else {
      } else {
        // CHECK-NEXT: clift.break_to %[[BREAK]]
        clift.break_to %break
      // CHECK-NEXT: }
      }
      // CHECK-NEXT: clift.expr {
      clift.expr {
        %value = clift.imm 1 : !int32_t
        clift.yield %value : !int32_t
      }
    }
  }
}
