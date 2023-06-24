// RUN: %target-swift-emit-silgen -disable-availability-checking %s | %FileCheck %s

public struct G<T> {}

// CHECK-LABEL: sil [ossa] @$s4main6caller2fnyyAA1GVyxGxQpXE_tRvzlF : $@convention(thin) <each T> (@guaranteed @noescape @callee_guaranteed @substituted <each τ_0_0> (@pack_guaranteed Pack{repeat G<each τ_0_0>}) -> () for <Pack{repeat each T}>) -> () {
public func caller<each T>(fn: (repeat G<each T>) -> ()) {
  fn(repeat G<each T>())
}

// rdar://107108803
public struct UsesG<each Input> {
  public init<E>(builder: (repeat G<each Input>) -> E) {}
}
UsesG<Int, String, Bool> { a, b, c in 0 }

// rdar://107367324
func returnVariadicClosure<each T, R>(f: @escaping (repeat each T) -> R) -> (repeat each T) -> R {
    { (t: repeat each T) -> R in
        let tuple = (repeat each t)
        print(tuple)
        return f(repeat each t)
    }
}
