// RUN: %target-swift-frontend %s -Onone -emit-ir -g -o - | %FileCheck %s

public class C<T>
{
  func c(_ i : T)
  {
    // Ensure that the type metadata for T is eagerly loaded at -Onone.
    // CHECK: define {{.*}} @"$s17EagerTypeMetadata1CC1cyyxF"
    // CHECK: %T = load ptr, ptr
    // CHECK-SAME: !dbg ![[LOC:[0-9]+]], !invariant.load
    var x = [i]
  }
}
// CHECK: !DIDerivedType(tag: DW_TAG_typedef, name: "T",
// CHECK-SAME:           baseType: ![[PTRTY:[0-9]+]]
// CHECK: ![[PTRTY]] = !DIDerivedType(tag: DW_TAG_pointer_type, name: "$sBpD", baseType: null, size: {{64|32}})
// CHECK: ![[LOC]] = !DILocation(line: 0,

