// RUN: rm -rf %t
// RUN: mkdir %t

// RUN: %target-swift-frontend -emit-module -enable-library-evolution \
// RUN:   -emit-module-path=%t/resilient_struct.swiftmodule \
// RUN:   -module-name=resilient_struct %S/../Inputs/resilient_struct.swift

// RUN: %target-swift-frontend -emit-module -I %t \
// RUN:   -emit-module-path=%t/OtherModule.swiftmodule \
// RUN:   -module-name=OtherModule %S/Inputs/OtherModule.swift

// RUN: %target-swift-frontend -module-name main -I %t -emit-ir %s | %FileCheck %s -DINT=i%target-ptrsize

// rdar://39763787

import OtherModule

// CHECK-LABEL: define {{(dllexport |protected )?}}swiftcc void @"$s4main7copyFoo3foo11OtherModule0C0VAF_tF"
// CHECK: [[T0:%.*]] = call swiftcc %swift.metadata_response @"$s11OtherModule3FooVMa"([[INT]] 0)
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

// CHECK-LABEL: define {{(dllexport |protected )?}}swiftcc void @"$s4main7copyBar3bar11OtherModule0C0VAF_tF"
// CHECK: [[T0:%.*]] = call swiftcc %swift.metadata_response @"$s11OtherModule3BarVMa"([[INT]] 0)
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
public func copyBar(bar: Bar) -> Bar {
  let copy = bar
  return copy
}
