// RUN: %target-swift-emit-ir %s -I %S/Inputs -enable-experimental-cxx-interop -validate-tbd-against-ir=none -disable-llvm-verify -Xcc -fignore-exceptions -disable-availability-checking | %FileCheck %s
//
// XFAIL: OS=linux-android, OS=linux-androideabi

import POD

// TODO: this should not be opaque.
// CHECK: %TSo7IntPairV = type <{ %Ts5Int32V, %Ts5Int32V }>
// CHECK: %struct.IntPair = type { i32, i32 }

// CHECK-LABEL: define {{.*}}swiftcc void @"$s4main4testyyF"

// CHECK: [[X:%.*]] = alloca ptr
// CHECK: [[TMP:%.*]] = alloca ptr

// CHECK: [[CREATED:%.*]] = call ptr @{{_ZN7IntPair6createEv|"\?create\@IntPair\@\@SAPEAU1\@XZ"}}()
// CHECK: store ptr [[CREATED]], ptr [[X]]

// CHECK: [[B_FIELD:%.*]] = getelementptr inbounds %TSo7IntPairV, ptr [[CREATED]], i32 0, i32 1
// CHECK: [[INT_VALUE:%.*]] = getelementptr inbounds %Ts5Int32V, ptr [[B_FIELD]], i32 0, i32 0
// CHECK: store i32 42, ptr [[INT_VALUE]], align 4

// CHECK: store ptr [[CREATED]], ptr [[TMP]]
// CHECK: [[TMP_LOAD:%.*]] = load ptr, ptr [[TMP]]
// CHECK: call i32 @{{_ZNK7IntPair4testEv|"\?test\@IntPair\@\@QEBAHXZ"}}(ptr [[TMP_LOAD]])

// CHECK: ret void

public func test() {
  var x = IntPair.create()
  x.b = 42
  _ = x.test()
}
