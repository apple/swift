import _Differentiation
import a

extension Struct: Differentiable {
  public struct TangentVector: Differentiable & AdditiveArithmetic {}
  public mutating func move(by _: TangentVector) {}

  @usableFromInline
  @derivative(of: method, wrt: x)
  func vjpMethod(_ x: Float) -> (value: Float, pullback: (Float) -> Float) {
    (x, { $0 })
  }
}

// Test cross-module recognition of functions with multiple semantic results.
@differentiable(reverse)
func multiply_swap(_ x: Float, _ y: Float) -> Float {
  var tuple = (x, y)
  swap(&tuple.0, &tuple.1)
  return tuple.0 * tuple.1
}

@differentiable(reverse)
func multiply_swapCustom(_ x: Float, _ y: Float) -> Float {
  var tuple = (x, y)
  swapCustom(&tuple.0, &tuple.1)
  return tuple.0 * tuple.1
}
