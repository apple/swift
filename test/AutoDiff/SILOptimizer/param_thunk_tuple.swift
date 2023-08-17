// RUN: %target-swift-frontend -emit-sil %s | %FileCheck %s

// Verify the result type of a subset parameters thunk matches the declaration:
//
// CHECK: // autodiff subset parameters thunk for forward-mode derivative from f(x:)
// CHECK-NEXT: sil shared [transparent] [thunk] @$s17param_thunk_tuple{{.*}} : $@convention(thin) (X)
// CHECK-SAME: -> (Float, Double, @owned @callee_guaranteed (X.TangentVector) -> Float)
// CHECK: return
// CHECK-SAME: %{{.*}} : $(Float, Double, @callee_guaranteed (X.TangentVector) -> Float)
//
// CHECK: // autodiff subset parameters thunk for reverse-mode derivative from f(x:)
// CHECK-NEXT: sil shared [transparent] [thunk] @$s17param_thunk_tuple{{.*}} : $@convention(thin) (X)
// CHECK-SAME: -> (Float, Double, @owned @callee_guaranteed (Float) -> X.TangentVector)
// CHECK: return
// CHECK-SAME: %{{.*}} : $(Float, Double, @callee_guaranteed (Float) -> X.TangentVector)

import _Differentiation

struct X: Differentiable {
    var a: Float
    var b: Double
}

@differentiable(reverse)
func f(x: X) -> (Float, Double) {
    (x.a, x.b)
}

@differentiable(reverse)
func g1(x: X) -> Float {
    f(x: x).0
}

