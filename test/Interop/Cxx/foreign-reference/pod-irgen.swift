// RUN: %target-swift-emit-ir %use_no_opaque_pointers %s -I %S/Inputs -enable-experimental-cxx-interop -validate-tbd-against-ir=none -disable-llvm-verify -Xcc -fignore-exceptions -disable-availability-checking | %FileCheck %s
// RUN: %target-swift-emit-ir %s -I %S/Inputs -enable-experimental-cxx-interop -validate-tbd-against-ir=none -disable-llvm-verify -Xcc -fignore-exceptions -disable-availability-checking
//
// XFAIL: OS=linux-android, OS=linux-androideabi

import POD

// TODO: this should not be opaque.
// CHECK: %TSo7IntPairV = type <{ %Ts5Int32V, %Ts5Int32V }>
// CHECK: %struct.IntPair = type { i32, i32 }

// CHECK-LABEL: define {{.*}}swiftcc void @"$s4main4testyyF"

// CHECK: [[X:%.*]] = alloca %TSo7IntPairV*
// CHECK: [[TMP:%.*]] = alloca %TSo7IntPairV*

// CHECK: [[CREATED:%.*]] = call %struct.IntPair* @{{_ZN7IntPair6createEv|"\?create\@IntPair\@\@SAPEAU1\@XZ"}}()
// CHECK: [[SWIFT_CREATED:%.*]] = bitcast %struct.IntPair* [[CREATED]] to %TSo7IntPairV*
// CHECK: store %TSo7IntPairV* [[SWIFT_CREATED]], %TSo7IntPairV** [[X]]

// CHECK: [[B_FIELD:%.*]] = getelementptr inbounds %TSo7IntPairV, %TSo7IntPairV* [[SWIFT_CREATED]], i32 0, i32 1
// CHECK: [[INT_VALUE:%.*]] = getelementptr inbounds %Ts5Int32V, %Ts5Int32V* [[B_FIELD]], i32 0, i32 0
// CHECK: store i32 42, i32* [[INT_VALUE]], align 4

// CHECK: store %TSo7IntPairV* [[SWIFT_CREATED]], %TSo7IntPairV** [[TMP]]
// CHECK: [[TMP_LOAD:%.*]] = load %TSo7IntPairV*, %TSo7IntPairV** [[TMP]]
// CHECK: [[CLANG_CREATED:%.*]] = bitcast %TSo7IntPairV* [[TMP_LOAD]] to %struct.IntPair*
// CHECK: call i32 @{{_ZNK7IntPair4testEv|"\?test\@IntPair\@\@QEBAHXZ"}}(%struct.IntPair* [[CLANG_CREATED]])

// CHECK: ret void

public func test() {
  var x = IntPair.create()
  x.b = 42
  _ = x.test()
}
