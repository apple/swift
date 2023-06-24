// RUN: %target-typecheck-verify-swift

// rdar://108738034: Make sure we can type-check this.
enum E<T>: Error {
  case e(T)
}

struct S {
  func bar(_: (Error?) -> Void) {}
}

func foo(_ s: S) {
  s.bar { error in
      guard let error = error else {
        return
      }
      if case let E<Int>.e(y) = error {
        print(y)
      }
    }
}
