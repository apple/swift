// RUN: %target-build-swift -module-name test -wmo -O -emit-ir -Xfrontend -num-threads -Xfrontend 0 %s %S/Inputs/nested_types_defs.swift -o - | %FileCheck %s

// Make sure we generate the outer metadata.

// CHECK-DAG: @"$s4test5OuterVMf" = internal constant {{.*}} {{@"\$sytWV(\.ptrauth[.0-9]*)?"|ptr @"\$s4test5OuterVWV"}}, {{.*}} @"$s4test5OuterVMn{{(\.ptrauth[.0-9]*)?}}"
// CHECK-DAG: @"$s4test6Outer2VMf" = internal constant {{.*}} {{@"\$sytWV(\.ptrauth[.0-9]*)?"|ptr @"\$s4test6Outer2VWV"}}, {{.*}} @"$s4test6Outer2VMn{{(\.ptrauth[.0-9]*)?}}"
// CHECK-DAG: @"$s4test6Outer3VMf" = internal constant {{.*}} {{@"\$sytWV(\.ptrauth[.0-9]*)?"|ptr @"\$s4test6Outer3VWV"}}, {{.*}} @"$s4test6Outer3VMn{{(\.ptrauth[.0-9]*)?}}"
// CHECK-DAG: @"$s4test6Outer4VMf" = internal constant {{.*}} {{@"\$sytWV(\.ptrauth[.0-9]*)?"|ptr @"\$s4test6Outer4VWV"}}, {{.*}} @"$s4test6Outer4VMn{{(\.ptrauth[.0-9]*)?}}"

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
