// RUN: %target-swift-frontend -typecheck -dump-ast %s | %FileCheck %s

// FIXME: Make this a SILGen test instead.
// Even though redundant conversions are eventually optimized away, test from
// the get-go that we build these implicit conversions only when necessary.
protocol P {}

class A {
  required init() {}

  func method() -> Self { self }
  var property: Self { self }
  subscript() -> Self { self }

  static func staticMethod() -> Self { .init() }
  static var staticProperty: Self { .init() }
  static subscript() -> Self { .init() }
}

class B: A {
  func test() -> Self {
    // CHECK: covariant_return_conversion_expr implicit type='Self' location={{.*}}.swift:[[@LINE+1]]
    _ = super.method()
    // CHECK: covariant_function_conversion_expr implicit type='() -> Self' location={{.*}}.swift:[[@LINE+1]]
    _ = super.method
    // CHECK: covariant_return_conversion_expr implicit type='Self' location={{.*}}.swift:[[@LINE+1]]
    _ = super.property
    // CHECK: covariant_return_conversion_expr implicit type='Self' location={{.*}}.swift:[[@LINE+1]]
    _ = super[]

    return super.property
  }

  static func testStatic() -> Self {
    // CHECK: covariant_return_conversion_expr implicit type='Self' location={{.*}}.swift:[[@LINE+1]]
    _ = super.staticMethod()
    // CHECK: covariant_function_conversion_expr implicit type='() -> Self' location={{.*}}.swift:[[@LINE+1]]
    _ = super.staticMethod
    // CHECK-NOT: function_conversion_expr {{.*}} location={{.*}}.swift:[[@LINE+3]]
    // CHECK-NOT: covariant_function_conversion_expr {{.*}} location={{.*}}.swift:[[@LINE+2]]
    // CHECK: covariant_return_conversion_expr implicit type='Self' location={{.*}}.swift:[[@LINE+1]]
    _ = self.method
    // CHECK: covariant_return_conversion_expr implicit type='Self' location={{.*}}.swift:[[@LINE+1]]
    _ = self.init()
    // CHECK: covariant_return_conversion_expr implicit type='Self' location={{.*}}.swift:[[@LINE+1]]
    _ = super.staticProperty
    // CHECK: covariant_return_conversion_expr implicit type='Self' location={{.*}}.swift:[[@LINE+1]]
    _ = super[]

    return super.staticProperty
  }
}

func testOnExistential(arg: P & A) {
  // FIXME: This could be a single conversion.
  // CHECK: function_conversion_expr implicit type='() -> any A & P' location={{.*}}.swift:[[@LINE+2]]
  // CHECK-NEXT: covariant_function_conversion_expr implicit type='() -> any A & P' location={{.*}}.swift:[[@LINE+1]]
  _ = arg.method
}

class Generic<T> {}
extension Generic where T == Never {
  func method() -> Self { self }
  var property: Self { self }
  subscript() -> Self { self }

  func test() {
    // CHECK-NOT: conversion_expr {{.*}} location={{.*}}.swift:{{[[@LINE+1]]|[[@LINE+2]]|[[@LINE+3]]|[[@LINE+4]]}}
    _ = Generic().method()
    _ = Generic().method
    _ = Generic().property
    _ = Generic()[]
  }
}

final class Final {
  static func useSelf(_ body: (Self) -> ()) {}
}
func testNoErasure(_ body: (Final) -> ()) {
  return Final.useSelf(body)
}
