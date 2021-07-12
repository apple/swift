// RUN: %target-swift-frontend -dump-parse -verify %s | %FileCheck %s

// expected-warning @+1 {{'@differentiable' has been renamed to '@differentiable(reverse)'}} {{23-23=(reverse)}}
let a: @differentiable (Float) -> Float // okay
// CHECK: (pattern_named "a"
// CHECK-NEXT: (type_attributed attrs=@differentiable(reverse)

let b: @differentiable(reverse) (Float) -> Float // okay
// CHECK: (pattern_named "b"
// CHECK-NEXT: (type_attributed attrs=@differentiable(reverse)

let c: @differentiable(reverse) (Float, @noDerivative Float) -> Float // okay
// CHECK: (pattern_named "c"
// CHECK-NEXT: (type_attributed attrs=@differentiable(reverse)
// CHECK-NEXT:  (type_function
// CHECK-NEXT:    (arg_type=type_tuple
// CHECK-NEXT:      (type_ident
// CHECK-NEXT:        (component "Float" unbound))
// CHECK-NEXT:      (type_attributed attrs=@noDerivative
// CHECK-NEXT:        (type_ident
// CHECK-NEXT:          (component "Float" unbound)))
// CHECK-NEXT:    (result_type=type_ident
// CHECK-NEXT:      (component "Float" unbound)))))

let d: @differentiable(reverse) (Float) throws -> Float // okay
// CHECK: (pattern_named "d"
// CHECK-NEXT: (type_attributed attrs=@differentiable(reverse)

let e: @differentiable(reverse) (Float) throws -> Float // okay
// CHECK: (pattern_named "e"
// CHECK-NEXT: (type_attributed attrs=@differentiable(reverse)

// Generic type test.
struct A<T> {
  func foo() {
    let local: @differentiable(reverse) (T) -> T // okay
    // CHECK: (pattern_named "local"
    // CHECK-NEXT: (type_attributed attrs=@differentiable(reverse)
  }
}

// expected-error @+1 {{expected ')' in '@differentiable' attribute}}
let c: @differentiable(reverse (Float) -> Float

// expected-error @+1 {{expected ')' in '@differentiable' attribute}}
let c: @differentiable(notValidArg (Float) -> Float

// expected-error @+1 {{unknown differentiability kind 'notValidArg'; only 'reverse' is supported}}
let d: @differentiable(notValidArg) (Float) -> Float

// Using 'linear' as a type
struct B {
  struct linear {}
  let propertyB1: @differentiable(reverse) (linear) -> Float // okay
  // CHECK: (pattern_named "propertyB1"
  // CHECK-NEXT: (type_attributed attrs=@differentiable(reverse)

  let propertyB2: @differentiable(reverse) (linear) -> linear // okay
  // CHECK: (pattern_named "propertyB2"
  // CHECK-NEXT: (type_attributed attrs=@differentiable(reverse)

  let propertyB3: @differentiable(reverse) (linear, linear) -> linear // okay
  // CHECK: (pattern_named "propertyB3"
  // CHECK-NEXT: (type_attributed attrs=@differentiable(reverse)

  let propertyB4: @differentiable(reverse) (linear, Float) -> linear // okay
  // CHECK: (pattern_named "propertyB4"
  // CHECK-NEXT: (type_attributed attrs=@differentiable(reverse)

  let propertyB5: @differentiable(reverse) (Float, linear) -> linear // okay
  // CHECK: (pattern_named "propertyB5"
  // CHECK-NEXT: (type_attributed attrs=@differentiable(reverse)

  let propertyB6: @differentiable(reverse) (linear, linear, Float, linear)
    -> Float // okay
  // CHECK: (pattern_named "propertyB6"
  // CHECK-NEXT: (type_attributed attrs=@differentiable(reverse)

  // expected-error @+1 {{expected ')' in '@differentiable' attribute}}
  let propertyB7: @differentiable(reverse (linear) -> Float
}

// Using 'reverse' as a typealias
struct C {
  typealias reverse = (C) -> C
  let propertyC1: @differentiable(reverse) (reverse) -> Float // okay
  // CHECK: (pattern_named "propertyC1"
  // CHECK-NEXT: (type_attributed attrs=@differentiable(reverse)

  let propertyC2: @differentiable(reverse) (reverse) -> reverse // okay
  // CHECK: (pattern_named "propertyC2"
  // CHECK-NEXT: (type_attributed attrs=@differentiable(reverse)

  let propertyC3: @differentiable(reverse) reverse // okay
  // CHECK: (pattern_named "propertyC3"
  // CHECK-NEXT: (type_attributed attrs=@differentiable(reverse)

  let propertyC4: reverse // okay
  // CHECK: (pattern_named "propertyC4"

  let propertyC6: @differentiable(reverse) @convention(c) reverse // okay
  // CHECK: (pattern_named "propertyC6"
  // CHECK-NEXT: (type_attributed attrs=@differentiable(reverse)
}
