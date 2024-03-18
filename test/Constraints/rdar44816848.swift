// RUN: %target-typecheck-verify-swift

class A {}
class B: A {}
class C: A {}

struct S {
  func foo<T: A>(types: [T.Type]) {}
}

func bar(_ s: S, _ forced_s: S!) {
  s.foo(types: [A.self, B.self]) // ok
  s.foo(types: [B.self, A.self]) // ok

  s.foo(types: [A.self, B.self, C.self]) // ok
  s.foo(types: [B.self, C.self, A.self]) // ok
  s.foo(types: [C.self, A.self, B.self]) // ok

  s.foo(types: [A.self, C.self, B.self]) // ok
  s.foo(types: [B.self, A.self, C.self]) // ok
  s.foo(types: [C.self, B.self, A.self]) // ok

  forced_s.foo(types: [A.self, B.self]) // ok
  forced_s.foo(types: [B.self, A.self]) // ok
}
