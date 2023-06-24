// RUN: %target-build-swift %use_no_opaque_pointers -module-name test -wmo -O -emit-ir -Xfrontend -num-threads -Xfrontend 0 %s %S/Inputs/nested_types_defs.swift -o - | %FileCheck %s
// RUN: %target-build-swift -module-name test -wmo -O -emit-ir -Xfrontend -num-threads -Xfrontend 0 %s %S/Inputs/nested_types_defs.swift -o -

// Make sure we generate the outer metadata.

// CHECK-DAG: @"$s4test5OuterVMf" = internal constant {{.*}} {{@"\$sytWV"|i8\*\* getelementptr inbounds \(%swift.vwtable, %swift.vwtable\* @"\$s4test5OuterVWV", i32 0, i32 0\)}}, {{.*}} @"$s4test5OuterVMn{{(\.ptrauth)?}}"
// CHECK-DAG: @"$s4test6Outer2VMf" = internal constant {{.*}} {{@"\$sytWV"|i8\*\* getelementptr inbounds \(%swift.vwtable, %swift.vwtable\* @"\$s4test6Outer2VWV", i32 0, i32 0\)}}, {{.*}} @"$s4test6Outer2VMn{{(\.ptrauth)?}}"
// CHECK-DAG: @"$s4test6Outer3VMf" = internal constant {{.*}} {{@"\$sytWV"|i8\*\* getelementptr inbounds \(%swift.vwtable, %swift.vwtable\* @"\$s4test6Outer3VWV", i32 0, i32 0\)}}, {{.*}} @"$s4test6Outer3VMn{{(\.ptrauth)?}}"
// CHECK-DAG: @"$s4test6Outer4VMf" = internal constant {{.*}} {{@"\$sytWV"|i8\*\* getelementptr inbounds \(%swift.vwtable, %swift.vwtable\* @"\$s4test6Outer4VWV", i32 0, i32 0\)}}, {{.*}} @"$s4test6Outer4VMn{{(\.ptrauth[.0-9]*)?}}"

class C<T> { }

struct Y {
  let x:C<Outer.Inner>
  let y:C<Outer2.InnerE>
  let z:C<Outer3.InnerC>
  let w:C<Outer4.InnerExtension>
}

public func test() {
  var c = Y(x: C<Outer.Inner>(), y: C<Outer2.InnerE>(), z: C<Outer3.InnerC>(),
            w: C<Outer4.InnerExtension>())

  print("a \(c)")
}
