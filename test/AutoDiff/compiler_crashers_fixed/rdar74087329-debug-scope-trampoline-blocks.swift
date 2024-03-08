// RUN: %target-build-swift %s
// RUN: %target-swift-frontend -c -g -Xllvm -verify-di-holes=true %s

// rdar://74087329 (DI verification failure with trampoline blocks in VJP)

import _Differentiation

func foo(x: Float?) -> Float {
   _ = withoutDerivative(at: x ?? 0)
   return 0
}
gradient(at: 0, of: foo)
