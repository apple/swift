// RUN: %swift %use_no_opaque_pointers -swift-version 4 -target arm64e-apple-ios12.0 -parse-stdlib -parse-as-library %s -emit-ir -module-name test -Xcc -Xclang -Xcc -fptrauth-calls | %FileCheck %s --check-prefix=CHECK
// RUN: %swift -swift-version 4 -target arm64e-apple-ios12.0 -parse-stdlib -parse-as-library %s -emit-ir -module-name test -Xcc -Xclang -Xcc -fptrauth-calls

// REQUIRES: CPU=arm64e
// REQUIRES: OS=ios

@objc class A {
  @objc func foo() {}
}

// CHECK: @"$s4test1AC3fooyyFTo.ptrauth" = private constant { i8*, i32, i64, i64 } { i8* bitcast (void (%0*, i8*)* @"$s4test1AC3fooyyFTo" to i8*), i32 0, i64 ptrtoint (i8** getelementptr inbounds ({ i32, i32, [1 x { i8*, i8*, i8* }] }, { i32, i32, [1 x { i8*, i8*, i8* }] }* @_INSTANCE_METHODS__TtC4test1A, i32 0, i32 2, i32 0, i32 2) to i64), i64 0 }, section "llvm.ptrauth"

@objc protocol P {
  func bar()
}
@objc class B : P {
  func bar() {}
}
// CHECK: @_PROTOCOL_INSTANCE_METHODS__TtP4test1P_ = {{.*}} [{ i8*, i8*, i8* } { i8* getelementptr inbounds ([4 x i8], [4 x i8]* @"\01L_selector_data(bar)", i64 0, i64 0), i8* getelementptr inbounds ([8 x i8], [8 x i8]* {{@.*}}, i64 0, i64 0), i8* null }]
