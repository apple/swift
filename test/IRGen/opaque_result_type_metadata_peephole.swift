// RUN: %empty-directory(%t)
// RUN: %target-swift-frontend -swift-version 5 -disable-availability-checking -static -enable-library-evolution -emit-module-path %t/opaque_result_type_metadata_external.swiftmodule %S/Inputs/opaque_result_type_metadata_external.swift
// RUN: %target-swift-frontend -swift-version 5 -disable-availability-checking -emit-ir -I %t %s | %FileCheck %s --check-prefix=CHECK --check-prefix=DEFAULT
// RUN: %target-swift-frontend -swift-version 5 -disable-availability-checking -emit-ir -I %t %s -enable-implicit-dynamic | %FileCheck %s --check-prefix=CHECK --check-prefix=IMPLICIT-DYNAMIC


import opaque_result_type_metadata_external

func foo() -> some P {
  return 0
}

// The mangled underlying type in foo2 ought to look through foo()'s opaque type
// CHECK-LABEL: @"$s36opaque_result_type_metadata_peephole4foo2QryFQOMQ" = {{.*}} constant
// DEFAULT-SAME:            @"symbolic Si"
// DEFAULT-SAME:            @"get_witness_table Si36opaque_result_type_metadata_external1PHpyHC
// IMPLICIT-DYNAMIC-SAME:   @"symbolic _____yQo_ 36opaque_result_type_metadata_peephole3fooQryFQO"
// IMPLICIT-DYNAMIC-SAME:   @"get_witness_table x36opaque_result_type_metadata_external1PHD1_0a1_b1_c1_D9_peephole3fooQryFQOyQo_HO
func foo2() -> some P {
  return foo()
}

func external_opaque_wrap() -> some P {
  return external_opaque()
}

func external_inlinable_wrap() -> some P {
  return external_inlinable()
}

func bar<T: P>(x: T) {
  T.stat()
}

dynamic func replaceable_opaque() -> some P {
  return 0
}

dynamic var replaceable_opaque_var: some P {
  return 0
}

// CHECK-LABEL: define {{.*}} @"$s36opaque_result_type_metadata_peephole26testOpaqueMetadataAccessesyyF"
public func testOpaqueMetadataAccesses() {
  // We can look through, since it's internal
  // DEFAULT: call {{.*}}3bar{{.*}}(ptr {{.*}}, ptr @"$sSiN", ptr @"$sSi36opaque_result_type_metadata_external1PAAWP")
  // IMPLICIT-DYNAMIC: call {{.*}}3bar{{.*}}(ptr {{.*}}, ptr %{{.*}}, ptr %{{.*}})
  bar(x: foo())
  // We can look through, since it's internal
  // DEFAULT: call {{.*}}3bar{{.*}}(ptr {{.*}}, ptr @"$sSiN", ptr @"$sSi36opaque_result_type_metadata_external1PAAWP")
  // IMPLICIT-DYNAMIC: call {{.*}}3bar{{.*}}(ptr {{.*}}, ptr %{{.*}}, ptr %{{.*}})
  bar(x: foo2())
  // We can't look through, since it's resilient
  // CHECK: call {{.*}}3bar{{.*}}(ptr {{.*}}, ptr %[[EXTERNAL_OPAQUE_METADATA:.*]], ptr %[[EXTERNAL_OPAQUE_PROTO:.*]])
  bar(x: external_opaque())
  // We can look through, since it's inlinable
  // CHECK: call {{.*}}3bar{{.*}}(ptr {{.*}}, ptr @"$sSiN", ptr @"$sSi36opaque_result_type_metadata_external1PAAWP")
  bar(x: external_inlinable())
  // We can look through to the wrapped resilient opaque type
  // DEFAULT: call {{.*}}3bar{{.*}}(ptr {{.*}}, ptr %[[EXTERNAL_OPAQUE_METADATA]], ptr %[[EXTERNAL_OPAQUE_PROTO]])
  // IMPLICIT-DYNAMIC: call {{.*}}3bar{{.*}}(ptr {{.*}}, ptr %{{.*}}, ptr %{{.*}})
  bar(x: external_opaque_wrap())
  // We can look all the way through to the inlinable underlying type
  // DEFAULT: call {{.*}}3bar{{.*}}(ptr {{.*}}, ptr @"$sSiN", ptr @"$sSi36opaque_result_type_metadata_external1PAAWP")
  // IMPLICIT-DYNAMIC: call {{.*}}3bar{{.*}}(ptr {{.*}}, ptr %{{.*}}, ptr %{{.*}})
  bar(x: external_inlinable_wrap())

  // We can't look through since it's dynamically replaceable
  // CHECK: call {{.*}}3bar{{.*}}(ptr {{.*}}, ptr %{{.*}}, ptr %{{.*}})
  bar(x: replaceable_opaque())
  // We can't look through since it's dynamically replaceable
  // CHECK: call {{.*}}3bar{{.*}}(ptr {{.*}}, ptr %{{.*}}, ptr %{{.*}})
  bar(x: replaceable_opaque_var)
}
