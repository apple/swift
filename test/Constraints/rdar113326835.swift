// RUN: %target-typecheck-verify-swift

// rdar://113326835 - Make sure we type-check the conjunctions in source order,
// the first closure should be type-checked before we attempt the
// TapExpr/SingleValueExpr conjunctions, since they rely on 'k' being resolved.

func global<T>(_ x: T) -> String { "" }
func global(_ x: Any.Type) -> String { "" }

protocol P {
  associatedtype X
}

struct Q<X>: P {
  init() {}
  func bar(_: String) -> Self { fatalError() }
  func qux<U: P>(_: (X) -> U) -> Q<U.X> { fatalError() }
}

struct J<X>: P {
  init(_: X) {}
  func baz<T>(_ transform: (X) -> T) -> Q<T> { fatalError() }
}

func foo(a: Int) -> Q<String> {
  J(a)
    .baz { x in
      ()
      return a
    }
    .qux { k in
      Q<String>().bar("\(k)")
    }
}

func bar(a: Int) -> Q<String> {
  J(a)
    .baz { x in
      ()
      return a
    }
    .qux { k in
      Q<String>().bar(if .random() { global(k) } else { global(k) })
      // expected-error@-1 {{'if' may only be used as expression in return, throw, or as the source of an assignment}}
    }
}
