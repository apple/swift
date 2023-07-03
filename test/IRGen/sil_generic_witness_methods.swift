// RUN: %target-swift-frontend -primary-file %s -emit-ir | %FileCheck %s

// REQUIRES: CPU=x86_64

// FIXME: These should be SIL tests, but we can't parse generic types in SIL
// yet.

protocol P {
  func concrete_method()
  static func concrete_static_method()
  func generic_method<Z>(_ x: Z)
}

struct S {}

// CHECK-LABEL: define hidden swiftcc void @"$s27sil_generic_witness_methods05call_D0{{[_0-9a-zA-Z]*}}F"(ptr noalias nocapture %0, ptr noalias nocapture %1, ptr %T, ptr %U, ptr %T.P)
func call_methods<T: P, U>(_ x: T, y: S, z: U) {
  // CHECK: [[STATIC_METHOD_ADDR:%.*]] = getelementptr inbounds ptr, ptr %T.P, i32 2
  // CHECK: [[STATIC_METHOD_PTR:%.*]] = load ptr, ptr [[STATIC_METHOD_ADDR]], align 8
  // CHECK: call swiftcc void [[STATIC_METHOD_PTR]](ptr swiftself %T, ptr %T, ptr %T.P)
  T.concrete_static_method()

  // CHECK: [[CONCRETE_METHOD_PTR_GEP:%.*]] = getelementptr inbounds ptr, ptr %T.P, i32 1
  // CHECK: [[CONCRETE_METHOD_PTR:%.*]] = load ptr, ptr [[CONCRETE_METHOD_PTR_GEP]]
  // CHECK: call swiftcc void [[CONCRETE_METHOD_PTR]](ptr noalias nocapture swiftself {{%.*}}, ptr %T, ptr %T.P)
  x.concrete_method()
  // CHECK: [[GENERIC_METHOD_ADDR:%.*]] = getelementptr inbounds ptr, ptr %T.P, i32 3
  // CHECK: [[GENERIC_METHOD_PTR:%.*]] = load ptr, ptr [[GENERIC_METHOD_ADDR]], align 8
  // CHECK: call swiftcc void [[GENERIC_METHOD_PTR]](ptr noalias nocapture {{.*}}, ptr {{.*}} @"$s27sil_generic_witness_methods1SVMf", {{.*}} ptr noalias nocapture swiftself {{.*}}, ptr %T, ptr %T.P)
  x.generic_method(y)
  // CHECK: [[GENERIC_METHOD_ADDR:%.*]] = getelementptr inbounds ptr, ptr %T.P, i32 3
  // CHECK: [[GENERIC_METHOD_PTR:%.*]] = load ptr, ptr [[GENERIC_METHOD_ADDR]], align 8
  // CHECK: call swiftcc void [[GENERIC_METHOD_PTR]](ptr noalias nocapture {{.*}}, ptr %U, ptr noalias nocapture swiftself {{.*}}, ptr %T, ptr %T.P)
  x.generic_method(z)
}

// CHECK-LABEL: define hidden swiftcc void @"$s27sil_generic_witness_methods017call_existential_D0{{[_0-9a-zA-Z]*}}F"(ptr noalias nocapture dereferenceable({{.*}}) %0)
func call_existential_methods(_ x: P, y: S) {
  // CHECK: [[METADATA_ADDR:%.*]] = getelementptr inbounds %T27sil_generic_witness_methods1PP, ptr [[X:%0]], i32 0, i32 1
  // CHECK: [[METADATA:%.*]] = load ptr, ptr [[METADATA_ADDR]], align 8
  // CHECK: [[WTABLE_ADDR:%.*]] = getelementptr inbounds %T27sil_generic_witness_methods1PP, ptr [[X]], i32 0, i32 2
  // CHECK: [[WTABLE:%.*]] = load ptr, ptr [[WTABLE_ADDR]], align 8
  // CHECK: [[CONCRETE_METHOD_PTR_GEP:%.*]] = getelementptr inbounds ptr, ptr [[WTABLE]], i32 1
  // CHECK: [[CONCRETE_METHOD_PTR:%.*]] = load ptr, ptr [[CONCRETE_METHOD_PTR_GEP]], align 8
  // CHECK: call swiftcc void [[CONCRETE_METHOD_PTR]](ptr noalias nocapture swiftself {{%.*}}, ptr [[METADATA]], ptr [[WTABLE]])
  x.concrete_method()

  // CHECK: [[METADATA_ADDR:%.*]] = getelementptr inbounds %T27sil_generic_witness_methods1PP, ptr [[X]], i32 0, i32 1
  // CHECK: [[METADATA:%.*]] = load ptr, ptr [[METADATA_ADDR]], align 8
  // CHECK: [[WTABLE_ADDR:%.*]] = getelementptr inbounds %T27sil_generic_witness_methods1PP, ptr [[X:%.*]], i32 0, i32 2
  // CHECK: [[WTABLE:%.*]] = load ptr, ptr [[WTABLE_ADDR]], align 8
  // CHECK: [[GENERIC_METHOD_ADDR:%.*]] = getelementptr inbounds ptr, ptr [[WTABLE]], i32 3
  // CHECK: [[GENERIC_METHOD_PTR:%.*]] = load ptr, ptr [[GENERIC_METHOD_ADDR]], align 8
  // CHECK: call swiftcc void [[GENERIC_METHOD_PTR]](ptr noalias nocapture {{.*}}, ptr {{.*}} @"$s27sil_generic_witness_methods1SVMf", {{.*}} ptr noalias nocapture swiftself {{%.*}}, ptr [[METADATA]], ptr [[WTABLE]])
  x.generic_method(y)
}
