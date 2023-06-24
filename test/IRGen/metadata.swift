// RUN: %empty-directory(%t)
// RUN: %target-swift-frontend -emit-module -enable-library-evolution -emit-module-path=%t/resilient_struct.swiftmodule -module-name=resilient_struct %S/../Inputs/resilient_struct.swift
// RUN: %target-swift-frontend -module-name A -I %t  %S/Inputs/metadata2.swift -primary-file %s -emit-ir | %FileCheck %s --check-prefix=CHECK --check-prefix=CHECK-%target-os

import resilient_struct

enum Singleton {
  case only
}

// CHECK: @"$s1A1GC14zeroSizedFieldAA9SingletonOvpWvd" = hidden constant i{{(64|32)}} 0
// Check that the instance start is after the header (at 8 or 16).
// CHECK-macosx: _DATA__TtC1A1G = internal constant {{.*}} { i32 {{(128|129)}}, i32 {{(16|8|40)}}
// CHECK-ios: _DATA__TtC1A1G = internal constant {{.*}} { i32 {{(128|129)}}, i32 {{(16|8|40)}}
// CHECK-watchos: _DATA__TtC1A1G = internal constant {{.*}} { i32 {{(128|129)}}, i32 {{(16|8|40)}}
// CHECK-tvos: _DATA__TtC1A1G = internal constant {{.*}} { i32 128, i32 {{(16|8)}}

class G {
  var zeroSizedField = Singleton.only
  var r = ResilientInt(i:1)
}

// CHECK-LABEL: define {{.*}}swiftcc %swift.metadata_response @"$s1A12MyControllerCMr"(ptr %0, ptr %1, ptr %2)
// CHECK-NOT: ret
// CHECK:  call swiftcc %swift.metadata_response @"$s1A17InternalContainerVMa"(
// CHECK:  ret
class MyController {
  var c = InternalContainer(item: [])
  var c2 = InternalContainer2(item: [])
  var e = InternalSingletonEnum()
  var e2 = InternalSingletonEnum2()
  func update(_ n: InternalContainer) {
    c = n
  }
}
