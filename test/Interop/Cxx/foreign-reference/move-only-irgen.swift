// RUN: %target-swift-emit-ir %use_no_opaque_pointers %s -I %S/Inputs -enable-experimental-cxx-interop -validate-tbd-against-ir=none -disable-llvm-verify -Xcc -fignore-exceptions -disable-availability-checking | %FileCheck %s
// RUN: %target-swift-emit-ir %s -I %S/Inputs -enable-experimental-cxx-interop -validate-tbd-against-ir=none -disable-llvm-verify -Xcc -fignore-exceptions -disable-availability-checking
//
// XFAIL: OS=linux-android, OS=linux-androideabi

import MoveOnly

// TODO: this should not be opaque.
// CHECK: %TSo8MoveOnlyV = type opaque
// CHECK: %struct.MoveOnly = type { i8 }


// CHECK-LABEL: define {{.*}}swiftcc void @"$s4main4testyyF"

// CHECK: [[X:%.*]] = alloca %TSo8MoveOnlyV*
// CHECK: [[TMP:%.*]] = alloca %TSo8MoveOnlyV*

// CHECK: [[CREATED:%.*]] = call %struct.MoveOnly* @{{_ZN8MoveOnly6createEv|"\?create\@MoveOnly\@\@SAPEAU1\@XZ"}}()
// CHECK: [[SWIFT_CREATED:%.*]] = bitcast %struct.MoveOnly* [[CREATED]] to %TSo8MoveOnlyV*
// CHECK: store %TSo8MoveOnlyV* [[SWIFT_CREATED]], %TSo8MoveOnlyV** [[X]]
// CHECK: store %TSo8MoveOnlyV* [[SWIFT_CREATED]], %TSo8MoveOnlyV** [[TMP]]

// CHECK: [[TMP_LOAD:%.*]] = load %TSo8MoveOnlyV*, %TSo8MoveOnlyV** [[TMP]]
// CHECK: [[CLANG_CREATED:%.*]] = bitcast %TSo8MoveOnlyV* [[TMP_LOAD]] to %struct.MoveOnly*
// CHECK: call i32 @{{_ZNK8MoveOnly4testEv|"\?test\@MoveOnly\@\@QEBAHXZ"}}(%struct.MoveOnly* [[CLANG_CREATED]])

// CHECK: ret void

public func test() {
  var x = MoveOnly.create()
  _ = x.test()
}
