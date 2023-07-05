// RUN: %target-swift-emit-ir %s -I %S/Inputs -enable-experimental-cxx-interop -validate-tbd-against-ir=none -disable-llvm-verify -Xcc -fignore-exceptions -disable-availability-checking | %FileCheck %s
//
// XFAIL: OS=linux-android, OS=linux-androideabi

import MoveOnly


// CHECK-LABEL: define {{.*}}swiftcc void @"$s4main4testyyF"

// CHECK: [[X:%.*]] = alloca ptr
// CHECK: [[TMP:%.*]] = alloca ptr

// CHECK: [[CREATED:%.*]] = call ptr @{{_ZN8MoveOnly6createEv|"\?create\@MoveOnly\@\@SAPEAU1\@XZ"}}()
// CHECK: store ptr [[CREATED]], ptr [[X]]
// CHECK: store ptr [[CREATED]], ptr [[TMP]]

// CHECK: [[TMP_LOAD:%.*]] = load ptr, ptr [[TMP]]
// CHECK: call i32 @{{_ZNK8MoveOnly4testEv|"\?test\@MoveOnly\@\@QEBAHXZ"}}(ptr [[TMP_LOAD]])

// CHECK: ret void

public func test() {
  var x = MoveOnly.create()
  _ = x.test()
}
