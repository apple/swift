import _Differentiation

public struct Struct {
  public func method(_ x: Float) -> Float { x }
}

// Test cross-module recognition of functions with multiple semantic results.
@differentiable(reverse)
public func swap(_ x: inout Float, _ y: inout Float) {
  let tmp = x; x = y; y = tmp
}

@differentiable(reverse)
public func swapCustom(_ x: inout Float, _ y: inout Float) {
  let tmp = x; x = y; y = tmp
}
@derivative(of: swapCustom)
public func vjpSwapCustom(_ x: inout Float, _ y: inout Float) -> (
  value: Void, pullback: (inout Float, inout Float) -> Void
) {
  swapCustom(&x, &y)
  return ((), {v1, v2 in
    let tmp = v1; v1 = v2; v2 = tmp
  })
}
