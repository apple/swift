// RUN: %target-swift-frontend -disable-availability-checking -emit-ir %s | %FileCheck %s

protocol Butt { }

protocol Tubb: Butt { }

protocol P {
  associatedtype A: Butt
  associatedtype B: Butt
  func foo(_ x: A) -> B
}

struct Foo<T: Tubb>: P {
  func foo(_ x: T) -> some Tubb { return x }
}

// CHECK-LABEL: define {{.*}} @"$s030opaque_result_type_associated_C17_conformance_path3FooVyxGAA1PAA1AAaEP_AA4ButtPWT"
// CHECK: [[TUBB_CONFORMANCE:%.*]] = call swiftcc ptr @swift_getOpaqueTypeConformance{{2?}}({{.*}}, i{{.*}} 1)
// CHECK: [[BUTT_CONFORMANCE_ADDR:%.*]] = getelementptr {{.*}} [[TUBB_CONFORMANCE]], i32 1
// CHECK: [[BUTT_CONFORMANCE:%.*]] = load {{.*}} [[BUTT_CONFORMANCE_ADDR]]
// CHECK: ret {{.*}} [[BUTT_CONFORMANCE]]
