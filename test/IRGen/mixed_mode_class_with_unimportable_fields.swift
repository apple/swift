// RUN: %empty-directory(%t)
// RUN: %target-swift-frontend -emit-module -o %t/UsingObjCStuff.swiftmodule -module-name UsingObjCStuff -I %t -I %S/Inputs/mixed_mode -swift-version 5 %S/Inputs/mixed_mode/UsingObjCStuff.swift
// RUN: %target-swift-frontend -emit-ir -I %t -I %S/Inputs/mixed_mode -module-name main -swift-version 4 %s | %FileCheck %s --check-prefix=CHECK --check-prefix=CHECK-%target-ptrsize --check-prefix=CHECK-V4 -DWORD=i%target-ptrsize --check-prefix=CHECK-V4-STABLE-ABI-%target-mandates-stable-abi
// RUN: %target-swift-frontend -emit-ir -I %t -I %S/Inputs/mixed_mode -module-name main -swift-version 5 %s | %FileCheck %s --check-prefix=CHECK --check-prefix=CHECK-%target-ptrsize --check-prefix=CHECK-V5 -DWORD=i%target-ptrsize --check-prefix=CHECK-V5-STABLE-ABI-%target-mandates-stable-abi

// REQUIRES: objc_interop

import UsingObjCStuff

public class SubButtHolder: ButtHolder {
  final var w: Float = 0

  override public func virtual() {}

  public func subVirtual() {}
}

public class SubSubButtHolder: SubButtHolder {
  public override func virtual() {}
  public override func subVirtual() {}
}

// CHECK-LABEL: define {{(protected )?}}{{(dllexport )?}}swiftcc void @"$s4main17accessFinalFields2ofyp_ypt14UsingObjCStuff10ButtHolderC_tF"
public func accessFinalFields(of holder: ButtHolder) -> (Any, Any) {

  // ButtHolder.y cannot be imported in Swift 4 mode, so make sure we use field
  // offset globals here.

  // CHECK-V4: [[OFFSET:%.*]] = load [[WORD]], ptr @"$s14UsingObjCStuff10ButtHolderC1xSivpWvd"
  // CHECK-V4: getelementptr inbounds i8, ptr {{.*}}, [[WORD]] [[OFFSET]]

  // CHECK-V4: [[OFFSET:%.*]] = load [[WORD]], ptr @"$s14UsingObjCStuff10ButtHolderC1zSSvpWvd"
  // CHECK-V4: getelementptr inbounds i8, ptr {{.*}}, [[WORD]] [[OFFSET]]

  // ButtHolder.y is correctly imported in Swift 5 mode, so we can use fixed offsets.

  // CHECK-V5: [[OFFSET:%.*]] = getelementptr inbounds %T14UsingObjCStuff10ButtHolderC, ptr %2, i32 0, i32 1

  // CHECK-V5: [[OFFSET:%.*]] = getelementptr inbounds %T14UsingObjCStuff10ButtHolderC, ptr %2, i32 0, i32 3

  return (holder.x, holder.z)
}

// CHECK-LABEL: define {{(protected )?}}{{(dllexport )?}}swiftcc void @"$s4main17accessFinalFields5ofSubyp_ypyptAA0F10ButtHolderC_tF"
public func accessFinalFields(ofSub holder: SubButtHolder) -> (Any, Any, Any) {
  // We should use the runtime-adjusted ivar offsets since we may not have
  // a full picture of the layout in mixed Swift language version modes.

  // ButtHolder.y cannot be imported in Swift 4 mode, so make sure we use field
  // offset globals here.

  // CHECK-V4: [[OFFSET:%.*]] = load [[WORD]], ptr @"$s14UsingObjCStuff10ButtHolderC1xSivpWvd"
  // CHECK-V4: getelementptr inbounds i8, ptr {{.*}}, [[WORD]] [[OFFSET]]

  // CHECK-V4: [[OFFSET:%.*]] = load [[WORD]], ptr @"$s14UsingObjCStuff10ButtHolderC1zSSvpWvd"
  // CHECK-V4: getelementptr inbounds i8, ptr {{.*}}, [[WORD]] [[OFFSET]]

  // CHECK-V4: [[OFFSET:%.*]] = load [[WORD]], ptr @"$s4main13SubButtHolderC1wSfvpWvd"

  // CHECK-V4: getelementptr inbounds i8, ptr {{.*}}, [[WORD]] [[OFFSET]]
  
  // ButtHolder.y is correctly imported in Swift 5 mode, so we can use fixed offsets.

  // CHECK-V5: [[OFFSET:%.*]] = getelementptr inbounds %T14UsingObjCStuff10ButtHolderC, ptr %3, i32 0, i32 1

  // CHECK-V5: [[OFFSET:%.*]] = getelementptr inbounds %T14UsingObjCStuff10ButtHolderC, ptr %3, i32 0, i32 3

  // CHECK-V5: [[OFFSET:%.*]] = getelementptr inbounds %T4main13SubButtHolderC, ptr %3, i32 0, i32 4

  return (holder.x, holder.z, holder.w)
}

// CHECK-LABEL: define {{(protected )?}}{{(dllexport )?}}swiftcc void @"$s4main12invokeMethod2onyAA13SubButtHolderC_tF"
public func invokeMethod(on holder: SubButtHolder) {
  // CHECK-64: [[IMPL_ADDR:%.*]] = getelementptr inbounds {{.*}}, [[WORD]] 13
  // CHECK-32: [[IMPL_ADDR:%.*]] = getelementptr inbounds {{.*}}, [[WORD]] 16
  // CHECK: [[IMPL:%.*]] = load {{.*}} [[IMPL_ADDR]]
  // CHECK: call swiftcc void [[IMPL]]
  holder.virtual()
  // CHECK-64: [[IMPL_ADDR:%.*]] = getelementptr inbounds {{.*}}, [[WORD]] 16
  // CHECK-32: [[IMPL_ADDR:%.*]] = getelementptr inbounds {{.*}}, [[WORD]] 19
  // CHECK: [[IMPL:%.*]] = load {{.*}} [[IMPL_ADDR]]
  // CHECK: call swiftcc void [[IMPL]]
  holder.subVirtual()
}

// CHECK-V4-LABEL: define internal swiftcc %swift.metadata_response @"$s4main13SubButtHolderCMr"(ptr %0, ptr %1, ptr %2)
// Under macCatalyst the deployment target is always >= 13
// CHECK-V4-STABLE-ABI-TRUE:    call swiftcc %swift.metadata_response @swift_updateClassMetadata2(
// CHECK-V4-STABLE-ABI-FALSE:   call swiftcc %swift.metadata_response @swift_initClassMetadata2(

// CHECK-V4-LABEL: define internal swiftcc %swift.metadata_response @"$s4main03SubB10ButtHolderCMr"(ptr %0, ptr %1, ptr %2)
// CHECK-V4-STABLE-ABI-TRUE:    call swiftcc %swift.metadata_response @swift_updateClassMetadata2(
// CHECK-V4-STABLE-ABI-FALSE:   call swiftcc %swift.metadata_response @swift_initClassMetadata2(
