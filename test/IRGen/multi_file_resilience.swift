// RUN: rm -rf %t
// RUN: mkdir %t

// RUN: %target-swift-frontend -emit-module -enable-library-evolution \
// RUN:   -emit-module-path=%t/resilient_struct.swiftmodule \
// RUN:   -module-name=resilient_struct %S/../Inputs/resilient_struct.swift

// RUN: %target-swift-frontend -module-name main -I %t -emit-ir -primary-file %s %S/Inputs/OtherModule.swift | %FileCheck %s -DINT=i%target-ptrsize

// Check that we correctly handle resilience when parsing as SIL + SIB.
// RUN: %target-swift-frontend -emit-sib -module-name main %S/Inputs/OtherModule.swift -I %t -o %t/other.sib
// RUN: %target-swift-frontend -emit-silgen -module-name main -primary-file %s %S/Inputs/OtherModule.swift -I %t -o %t/main.sil
// RUN: %target-swift-frontend -emit-ir -module-name main -primary-file %t/main.sil %t/other.sib -I %t | %FileCheck %s -DINT=i%target-ptrsize

// This is a single-module version of the test case in
// multi_module_resilience.
// rdar://39763787

// CHECK-LABEL: define {{(dllexport |protected )?}}swiftcc void @"$s4main7copyFoo3fooAA0C0VAE_tF"
// CHECK: [[T0:%.*]] = call swiftcc %swift.metadata_response @"$s4main3FooVMa"([[INT]] 0)
// CHECK: [[METADATA:%.*]] = extractvalue %swift.metadata_response [[T0]], 0
// CHECK: [[VWT:%.*]] = load ptr,
//   Allocate 'copy'.
// CHECK: [[SIZE_ADDR:%.*]] = getelementptr inbounds %swift.vwtable, ptr [[VWT]], i32 0, i32 8
// CHECK: [[SIZE:%.*]] = load [[INT]], ptr [[SIZE_ADDR]]
// CHECK: [[ALLOCA:%.*]] = alloca i8, [[INT]] [[SIZE]],
//   Perform 'initializeWithCopy' via the VWT instead of trying to inline it.
// CHECK: [[T0:%.*]] = getelementptr inbounds ptr, ptr [[VWT]], i32 2
// CHECK: [[T1:%.*]] = load ptr, ptr [[T0]],
// CHECK: call ptr [[T1]](ptr noalias [[ALLOCA]], ptr noalias %1, ptr [[METADATA]])
//   Perform 'initializeWithCopy' via the VWT.
// CHECK: call ptr [[T1]](ptr noalias %0, ptr noalias [[ALLOCA]], ptr [[METADATA]])
public func copyFoo(foo: Foo) -> Foo {
  let copy = foo
  return copy
}
