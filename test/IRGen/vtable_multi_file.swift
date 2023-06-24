// RUN: %target-swift-frontend %use_no_opaque_pointers -enable-objc-interop -primary-file %s %S/Inputs/vtable_multi_file_helper.swift -emit-ir | %FileCheck --check-prefixes=CHECK,CHECK-OBJC %s
// RUN: %target-swift-frontend %use_no_opaque_pointers -disable-objc-interop -primary-file %s %S/Inputs/vtable_multi_file_helper.swift -emit-ir | %FileCheck --check-prefixes=CHECK,CHECK-NO-OBJC %s
// RUN: %target-swift-frontend -enable-objc-interop -primary-file %s %S/Inputs/vtable_multi_file_helper.swift -emit-ir
// RUN: %target-swift-frontend -disable-objc-interop -primary-file %s %S/Inputs/vtable_multi_file_helper.swift -emit-ir

// REQUIRES: CPU=x86_64

// CHECK-LABEL: @"$s17vtable_multi_file7DerivedCMf" = internal global
// CHECK-SAME: @"$s17vtable_multi_file4BaseC6methodyyF"
class Derived : Base {
  func another() {}
}

func markUsed<T>(_ t: T) {}

// CHECK-LABEL: define hidden swiftcc void @"$s17vtable_multi_file36baseClassVtablesIncludeImplicitInitsyyF"() {{.*}} {
func baseClassVtablesIncludeImplicitInits() {
  // CHECK: [[TMP:%.*]] = call swiftcc %swift.metadata_response @"$s17vtable_multi_file8SubclassCMa"(i64 0)
  // CHECK: [[T0:%.*]] = extractvalue %swift.metadata_response [[TMP]], 0
  // CHECK: [[T1:%.*]] = bitcast %swift.type* [[T0]] to { i64, %swift.bridge* } (%swift.type*)**
  // CHECK-OBJC: [[T2:%.*]] = getelementptr inbounds { i64, %swift.bridge* } (%swift.type*)*, { i64, %swift.bridge* } (%swift.type*)** [[T1]], i64 11
  // CHECK-NO-OBJC: [[T2:%.*]] = getelementptr inbounds { i64, %swift.bridge* } (%swift.type*)*, { i64, %swift.bridge* } (%swift.type*)** [[T1]], i64 8
  // CHECK: load { i64, %swift.bridge* } (%swift.type*)*, { i64, %swift.bridge* } (%swift.type*)** [[T2]]
  markUsed(Subclass.classProp)
}

func forEachFinalizesVTable(_ h: Holder) {
  for _ in h.getSillySequence() {}
}
