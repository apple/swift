// RUN: %target-swift-frontend %s -parse-as-library -module-name a -emit-sil -g -o - | %FileCheck %s
struct S {}
public class UIView {}
public protocol View {}
public final class Signal<Value> {
    public func map<U>(_ transform: @escaping (Value) -> U) -> Signal<U> {
        return Signal<U>()
    }
}
public final class C<V: View, V1: View>: UIView {
    private let t1: C<V, V1>? = nil
    private let t2: C<V1, V>? = nil
    func foo() -> Signal<(S, UIView)> {
    // CHECK: sil {{.*}}s1a1CC3foo
    // CHECK: debug_value {{.*}} name "self"
    // CHECK-NOT: debug_value {{.*}} name "view"
    // CHECK: return %
	return (
            Signal<S>()
	    .map { [view = t1!] in ($0, view) },
            Signal<S>()
	    .map { [view = t2!] in ($0, view) }
	).0
    }
}

