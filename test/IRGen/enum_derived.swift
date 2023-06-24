// RUN: %empty-directory(%t)
// RUN: %target-swift-frontend -emit-module -module-name def_enum -o %t %S/Inputs/def_enum.swift
// RUN: %target-swift-frontend -I %t -O -primary-file %s -emit-ir | %FileCheck -check-prefix=CHECK -check-prefix=CHECK-NORMAL %s
// RUN: %target-swift-frontend -I %t -O -primary-file %s -enable-testing -emit-ir | %FileCheck -check-prefix=CHECK -check-prefix=CHECK-TESTABLE %s


import def_enum

// Check if hashValue, hash(into:) and == for an enum (without payload) are
// generated and check that functions are compiled in an optimal way.

enum E {
  case E0
  case E1
  case E2
  case E3
}

// Check if the == comparison can be compiled to a simple icmp instruction.

// CHECK-NORMAL-LABEL:define hidden swiftcc i1 @"$s12enum_derived1EO02__b1_A7_equalsySbAC_ACtFZ"(i8 %0, i8 %1)
// CHECK-TESTABLE-LABEL:define{{( dllexport)?}}{{( protected)?}} swiftcc i1 @"$s12enum_derived1EO02__b1_A7_equalsySbAC_ACtFZ"(i8 %0, i8 %1)
// CHECK: %2 = icmp eq i8 %0, %1
// CHECK: ret i1 %2

// Check if the hash(into:) method can be compiled to a simple zext instruction
// followed by a call to Hasher._combine(_:).

// CHECK-NORMAL-LABEL:define hidden swiftcc void @"$s12enum_derived1EO4hash4intoys6HasherVz_tF"
// CHECK-TESTABLE-LABEL:define{{( dllexport)?}}{{( protected)?}} swiftcc void @"$s12enum_derived1EO4hash4intoys6HasherVz_tF"
// CHECK: [[V:%.*]] = zext i8 %1 to i{{.*}}
// CHECK: tail call swiftcc void @"$ss6HasherV8_combineyySuF"(i{{.*}} [[V]], ptr
// CHECK: ret void

// Check for the presence of the hashValue getter, calling Hasher.init() and
// Hasher.finalize().

// CHECK-NORMAL-LABEL:define hidden swiftcc i{{.*}} @"$s12enum_derived1EO9hashValueSivg"(i8 %0)
// CHECK-TESTABLE-LABEL:define{{( dllexport)?}}{{( protected)?}} swiftcc i{{.*}} @"$s12enum_derived1EO9hashValueSivg"(i8 %0)
// CHECK: call swiftcc void @"$ss6HasherV5_seedABSi_tcfC"(ptr {{.*}})
// CHECK: call swiftcc i{{[0-9]+}} @"$ss6HasherV9_finalizeSiyF"(ptr {{.*}})
// CHECK: ret i{{[0-9]+}} %{{[0-9]+}}

// Derived conformances from extensions
// The actual enums are in Inputs/def_enum.swift

extension def_enum.TrafficLight : Error {}

extension def_enum.Term : Error {}

// CHECK-NORMAL-LABEL: define hidden {{.*}}i64 @"$s12enum_derived7PhantomO8rawValues5Int64Vvg"(i8 %0, ptr nocapture readnone %T) local_unnamed_addr
// CHECK-TESTABLE-LABEL: define{{( dllexport)?}}{{( protected)?}} {{.*}}i64 @"$s12enum_derived7PhantomO8rawValues5Int64Vvg"(i8 %0, ptr nocapture readnone %T)

enum Phantom<T> : Int64 {
  case Up
  case Down
}
