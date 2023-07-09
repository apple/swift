// RUN: %empty-directory(%t)
// RUN: %target-swift-frontend %s -emit-module -parse-as-library -o %t
// RUN: llvm-bcanalyzer %t/differentiable_attr.swiftmodule | %FileCheck %s -check-prefix=BCANALYZER
// RUN: %target-sil-opt -enable-sil-verify-all %t/differentiable_attr.swiftmodule -o - | %FileCheck %s

// BCANALYZER-NOT: UnknownCode

import _Differentiation

// CHECK: @differentiable(reverse, wrt: x)
// CHECK-NEXT: func simple(x: Float) -> Float
@differentiable(reverse)
func simple(x: Float) -> Float {
  return x
}

// CHECK: @differentiable(_linear, wrt: x)
// CHECK-NEXT: func simple2(x: Float) -> Float
@differentiable(_linear)
func simple2(x: Float) -> Float {
  return x
}

// CHECK: @differentiable(_linear, wrt: x)
// CHECK-NEXT: func simple4(x: Float) -> Float
@differentiable(_linear, wrt: x)
func simple4(x: Float) -> Float {
  return x
}

func jvpSimple(x: Float) -> (Float, (Float) -> Float) {
  return (x, { v in v })
}

func vjpSimple(x: Float) -> (Float, (Float) -> Float) {
  return (x, { v in v })
}

// CHECK: @differentiable(reverse, wrt: x)
// CHECK-NEXT: func testWrtClause(x: Float, y: Float) -> Float
@differentiable(reverse, wrt: x)
func testWrtClause(x: Float, y: Float) -> Float {
  return x
}

// CHECK: @differentiable(reverse, wrt: x)
// CHECK-NEXT: func testInout(x: inout Float)
@differentiable(reverse)
func testInout(x: inout Float) {
  x = x * 2.0
}

// CHECK: @differentiable(reverse, wrt: x)
// CHECK-NEXT: func testInoutResult(x: inout Float) -> Float
@differentiable(reverse)
func testInoutResult(x: inout Float) -> Float {
  x = x * 2.0
  return x
}

// CHECK: @differentiable(reverse, wrt: (x, y))
// CHECK-NEXT: func testMultipleInout(x: inout Float, y: inout Float)
@differentiable(reverse)
func testMultipleInout(x: inout Float, y: inout Float) {
  x = x * y
  y = x
}

struct InstanceMethod : Differentiable {
  // CHECK: @differentiable(reverse, wrt: (self, y))
  // CHECK-NEXT: func testWrtClause(x: Float, y: Float) -> Float
  @differentiable(reverse, wrt: (self, y))
  func testWrtClause(x: Float, y: Float) -> Float {
    return x
  }

  struct TangentVector: Differentiable, AdditiveArithmetic {
    typealias TangentVector = Self
    static func ==(_: Self, _: Self) -> Bool { fatalError() }
    static var zero: Self { fatalError() }
    static func +(_: Self, _: Self) -> Self { fatalError() }
    static func -(_: Self, _: Self) -> Self { fatalError() }
  }
  mutating func move(by offset: TangentVector) {}
}

// CHECK: @differentiable(reverse, wrt: x where T : Differentiable)
// CHECK-NEXT: func testOnlyWhereClause<T>(x: T) -> T where T : Numeric
@differentiable(reverse where T : Differentiable)
func testOnlyWhereClause<T : Numeric>(x: T) -> T {
  return x
}

// CHECK: @differentiable(reverse, wrt: x where T : Differentiable)
// CHECK-NEXT: func testWhereClause<T>(x: T) -> T where T : Numeric
@differentiable(reverse where T : Differentiable)
func testWhereClause<T : Numeric>(x: T) -> T {
  return x
}

protocol P {}
extension P {
  // CHECK: @differentiable(reverse, wrt: self where Self : Differentiable)
  // CHECK-NEXT: func testWhereClauseMethod() -> Self
  @differentiable(reverse, wrt: self where Self : Differentiable)
  func testWhereClauseMethod() -> Self {
    return self
  }
}
extension P where Self : Differentiable {
  func vjpTestWhereClauseMethod() -> (Self, (Self.TangentVector) -> Self.TangentVector) {
    return (self, { v in v })
  }
}

// CHECK: @differentiable(reverse, wrt: x where T : Differentiable, T == T.TangentVector)
// CHECK-NEXT: func testWhereClauseMethodTypeConstraint<T>(x: T) -> T where T : Numeric
@differentiable(reverse where T : Differentiable, T == T.TangentVector)
func testWhereClauseMethodTypeConstraint<T : Numeric>(x: T) -> T {
  return x
}
func vjpTestWhereClauseMethodTypeConstraint<T>(x: T) -> (T, (T) -> T)
  where T : Numeric, T : Differentiable, T == T.TangentVector
{
  return (x, { v in v })
}

extension P {
  // CHECK: @differentiable(reverse, wrt: self where Self : Differentiable, Self == Self.TangentVector)
  // CHECK-NEXT: func testWhereClauseMethodTypeConstraint() -> Self
  @differentiable(reverse, wrt: self where Self.TangentVector == Self, Self : Differentiable)
  func testWhereClauseMethodTypeConstraint() -> Self {
    return self
  }
}
extension P where Self : Differentiable, Self == Self.TangentVector {
  func vjpTestWhereClauseMethodTypeConstraint() -> (Self, (Self.TangentVector) -> Self.TangentVector) {
    return (self, { v in v })
  }
}
