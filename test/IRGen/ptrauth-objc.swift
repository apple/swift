// RUN: %swift -swift-version 4 -target arm64e-apple-ios12.0 -parse-stdlib -parse-as-library %s -emit-ir -module-name test -Xcc -Xclang -Xcc -fptrauth-calls | %FileCheck %s --check-prefix=CHECK

// REQUIRES: CPU=arm64e
// REQUIRES: OS=ios

@objc class A {
  @objc func foo() {}
}

// CHECK: @"$s4test1AC3fooyyFTo.ptrauth" = private constant { ptr, i32, i64, i64 } { ptr @"$s4test1AC3fooyyFTo", i32 0, i64 ptrtoint (ptr getelementptr inbounds ({ i32, i32, [1 x { ptr, ptr, ptr }] }, ptr @_INSTANCE_METHODS__TtC4test1A, i32 0, i32 2, i32 0, i32 2) to i64), i64 0 }, section "llvm.ptrauth"

@objc protocol P {
  func bar()
}
@objc class B : P {
  func bar() {}
}
// CHECK: @_PROTOCOL_INSTANCE_METHODS__TtP4test1P_ = {{.*}} [{ ptr, ptr, ptr } { ptr @"\01L_selector_data(bar)", ptr {{@.*}}, ptr null }]
