// RUN: %target-swift-frontend -O -parse-stdlib -emit-ir -module-name ZeroInit -verify %s | %FileCheck %s

// REQUIRES: swift_in_compiler

import Swift

@frozen
public struct TestInt {
  @usableFromInline
  var _value : Builtin.Int32
  @_transparent
  public init() {
    _value = Builtin.zeroInitializer()
  }
}

@frozen
public struct TestFloat {
  @usableFromInline
  var _value : Builtin.FPIEEE32
  @_transparent
  public init() {
    _value = Builtin.zeroInitializer()
  }
}

@frozen
public struct TestVector {
  @usableFromInline
  var _value : Builtin.Vec4xFPIEEE32
  @_transparent
  public init() {
    _value = Builtin.zeroInitializer()
  }
}

public struct Foo {
  public static var x : TestInt = TestInt()
  public static var y : TestFloat = TestFloat()
  public static var z : TestVector = TestVector()
}

// CHECK: @"$s8ZeroInit3FooV1xAA7TestIntVvpZ" ={{.*}} global %T8ZeroInit7TestIntV zeroinitializer
// CHECK: @"$s8ZeroInit3FooV1yAA9TestFloatVvpZ" ={{.*}} global %T8ZeroInit9TestFloatV zeroinitializer
// CHECK: @"$s8ZeroInit3FooV1zAA10TestVectorVvpZ" ={{.*}} global %T8ZeroInit10TestVectorV zeroinitializer
// CHECK-NOT: swift_once
