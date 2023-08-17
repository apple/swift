// RUN: %target-swift-frontend %s -enable-experimental-feature RawLayout -emit-ir -g -o - | %FileCheck %s

@_rawLayout(size: 12, alignment: 4)
// CHECK: !DICompositeType(tag: DW_TAG_structure_type, name: "S8_4"
// CHECK-SAME:             size: 96,
// CHECK-SAME:             align: 32,
struct S8_4: ~Copyable {}

var s = S8_4()
