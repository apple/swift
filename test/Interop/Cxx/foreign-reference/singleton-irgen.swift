// RUN: %target-swift-emit-ir %s -I %S/Inputs -enable-experimental-cxx-interop -validate-tbd-against-ir=none -disable-llvm-verify -Xcc -fignore-exceptions -disable-availability-checking | %FileCheck %s
//
// XFAIL: OS=linux-android, OS=linux-androideabi

import Singleton

// CHECK: %struct.DeletedSpecialMembers = type { i32 }

// CHECK-LABEL: define {{.*}}swiftcc void @"$s4main4testyyF"

// CHECK: [[X:%.*]] = alloca ptr
// CHECK: [[TMP:%.*]] = alloca ptr

// CHECK: [[CREATED:%.*]] = call ptr @{{_ZN21DeletedSpecialMembers6createEv|"\?create\@DeletedSpecialMembers\@\@SAPEAU1\@XZ"}}()
// CHECK: store ptr [[CREATED]], ptr [[X]]
// CHECK: store ptr [[CREATED]], ptr [[TMP]]

// CHECK: [[TMP_LOAD:%.*]] = load ptr, ptr [[TMP]]
// CHECK: call i32 @{{_ZNK21DeletedSpecialMembers4testEv|"\?test\@DeletedSpecialMembers\@\@QEBAHXZ"}}(ptr [[TMP_LOAD]])

// CHECK: call void @{{_Z8mutateItR21DeletedSpecialMembers|"\?mutateIt\@\@YAXAEAUDeletedSpecialMembers\@\@\@Z"}}(ptr [[CREATED]])

// CHECK: ret void

public func test() {
  var x = DeletedSpecialMembers.create()
  _ = x.test()
  mutateIt(x)
}
