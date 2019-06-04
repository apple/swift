// RUN: %target-swift-frontend -parse -verify %s

/// Good

@differentiating(sin) // ok
func jvpSin(x: @nondiff Float) -> (value: Float, differential: (Float) -> (Float)) {
  return (x, { $0 })
}
@differentiating(sin, wrt: x) // ok
func vjpSinExplicitWrt(x: Float) -> (value: Float, pullback: (Float) -> Float) {
  return (x, { $0 })
}

@differentiating(add, wrt: (x, y)) // ok
func vjpAddWrtXY(x: Float, y: Float) -> (value: Float, pullback: (Float) 
-> (Float, Float)) {
  return (x + y, { ($0, $0) })
}

extension AdditiveArithmetic where Self : Differentiable {
  @differentiating(+) // ok
  static func vjpPlus(x: Self, y: Self) -> (value: Self, 
  pullback: (Self.TangentVector) -> (Self.TangentVector, Self.TangentVector)) {
    return (x + y, { v in (v, v) })
  }
}

@differentiating(foo, linear) // ok
func dfoo(x: Float) -> (value: Float, differential: (Float) -> (Float)) {
  return (x, { $0 })
}

@differentiating(foo, linear, wrt: x) // ok
func dfoo(x: Float) -> (value: Float, differential: (Float) -> (Float)) {
  return (x, { $0 })
}

/// Bad

// expected-error @+3 {{expected an original function name}}
// expected-error @+2 {{expected ')' in 'differentiating' attribute}}
// expected-error @+1 {{expected declaration}}
@differentiating(3)
func dfoo(x: Float) -> (value: Float, differential: (Float) -> (Float)) {
  return (x, { $0 })
}

// expected-error @+2 {{expected ')' in 'differentiating' attribute}}
// expected-error @+1 {{expected declaration}}
@differentiating(linear, foo)
func dfoo(x: Float) -> (value: Float, differential: (Float) -> (Float)) {
  return (x, { $0 })
}