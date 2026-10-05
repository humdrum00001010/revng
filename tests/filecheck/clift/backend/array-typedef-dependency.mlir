//
// This file is distributed under the MIT License. See LICENSE.md for details.
//

// RUN: %root/bin/revng clift-opt --emit-type-and-global-header %s -o /dev/null | FileCheck %s
// RUN: %root/bin/revng clift-opt --emit-type-and-global-header=ptml %s -o /dev/null | %root/bin/revng ptml | FileCheck %s

!uint8_t = !clift.int<unsigned 1>

!element = !clift.struct<
  "/type-definition/2-StructDefinition" as "element" : size(1) {
    "/struct-field/2-StructDefinition/0" as "value" : offset(0) !uint8_t
  }
>
!element_alias = !clift.typedef<
  "/type-definition/1-TypedefDefinition" as "element_alias" : !element
>
!elements = !clift.array<1 x !element_alias>
!container = !clift.struct<
  "/type-definition/0-StructDefinition" as "container" : size(1) {
    "/struct-field/0-StructDefinition/0" as "items" : offset(0) !elements
  }
>

// CHECK: struct _PACKED _SIZE(1) element {
// CHECK: uint8_t value _STARTS_AT(0);
// CHECK: };
// CHECK: struct _PACKED _SIZE(1) container {
// CHECK: element_alias items[1] _STARTS_AT(0);
// CHECK: };

module attributes {clift.module, clift.types = [!container]} {
}
