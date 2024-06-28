// RUN: %target-typecheck-verify-swift

// Make sure we can recover by ignoring the '}' in the #if.
struct S {
  func foo() {
#if true
  } // expected-error {{unexpected '}' in conditional compilation block}}
#endif
  }
  func bar() {
#if false
  } // expected-error {{unexpected '}' in conditional compilation block}}
#endif
  }
  func baz() {}
}
