// RUN: %target-swift-frontend -emit-sil -O %s | %FileCheck %s

// Ensure that autoDiffCreateLinearMapContext call is not LICM'ed
import _Differentiation;

public struct R: Differentiable {
    @noDerivative public var z: Int
}

public struct Z: Differentiable {
    public var r: [R] = []
}

public struct B: Differentiable {
    public var h = [Float]();
    public var e = Z()
}

public extension Array {
    @differentiable(reverse where Element: Differentiable)
    mutating func update(at x: Int, with n: Element) {
        self[x] = n
    }
}

public extension Array where Element: Differentiable {
    @derivative(of: update(at:with:))
    mutating func v(at x: Int, with nv: Element) ->
      (value: Void,
       pullback: (inout TangentVector) -> (Element.TangentVector)) {
        update(at: x, with: nv);
        let f = count;
        return ((),
                { v in
                    if v.base.count < f {
                        v.base = [Element.TangentVector](repeating: .zero, count: f)
                    };
                    let d = v[x];
                    v.base[x] = .zero;
                    return d}
        )
    }
}

extension B {
    @differentiable(reverse)
    mutating func a() {
        for idx in 0 ..< withoutDerivative(at: self.e.r).count {
            let z = self.e.r[idx].z;
            let c = self.h[z];
            self.h.update(at: z, with: c + 2.4)
        }
    }
}

public func b(y: B) -> (value: B,
                        pullback: (B.TangentVector) -> (B.TangentVector)) {
    let s = valueWithPullback(at: y, of: s);
    return (value: s.value, pullback: s.pullback)
}

@differentiable(reverse)
public func s(y: B) -> B {
    @differentiable(reverse)
    func q(_ u: B) -> B {
        var l = u;
        for _ in 0 ..< 1 {
            l.a()
        };
        return l
    };
    let w = m(q);
    return w(y)
}

// CHECK-LABEL: sil private @$s12licm_context1s1yAA1BVAE_tF1qL_yA2EFTJrSpSr :
// CHECK: autoDiffCreateLinearMapContext
// CHECK: autoDiffCreateLinearMapContext
// CHECK-LABEL: end sil function '$s12licm_context1s1yAA1BVAE_tF1qL_yA2EFTJrSpSr'

func o<T, R>(_ x: T, _ f: @differentiable(reverse) (T) -> R) -> R {
    f(x)
}

func m<T, R>(_ f: @escaping @differentiable(reverse) (T) -> R) -> @differentiable(reverse) (T) -> R {
    { x in o(x, f) }
}

let m = b(y: B());
let grad = m.pullback(B.TangentVector(h: Array<Float>.TangentVector(), e: Z.TangentVector(r: Array<R>.TangentVector())))
