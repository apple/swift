// RUN: %target-swift-frontend -emit-sil %s -verify

func test0(a: (() -> ()) -> (), b: () -> ()) {
  a(b) // expected-error {{passing a non-escaping function parameter 'b' to a call to a non-escaping function parameter can allow re-entrant modification of a variable}}
}

func test0(fn: (() -> ()) -> ()) {
  fn { fn {} } // expected-error {{passing a closure which captures a non-escaping function parameter 'fn' to a call to a non-escaping function parameter can allow re-entrant modification of a variable}}
}

func test1(fn: (() -> ()) -> ()) {
  func foo() {
    fn { fn {} } // expected-error {{passing a closure which captures a non-escaping function parameter 'fn' to a call to a non-escaping function parameter can allow re-entrant modification of a variable}}
  }
}

func test2(x: inout Int, fn: (() -> ()) -> ()) {
  func foo(myfn: () -> ()) {
    x += 1
    myfn()
  }

  // Make sure we only complain about calls to noescape parameters.
  foo { fn {} }
}

func test3(fn: (() -> ()) -> ()) {
  { myfn in myfn { fn {} } }(fn) // expected-error {{passing a closure which captures a non-escaping function parameter 'fn' to a call to a non-escaping function parameter can allow re-entrant modification of a variable}}
}

func test4(fn: (() -> ()) -> ()) {
  func foo() {
    fn {}
  }

  fn(foo) // expected-error {{passing a closure which captures a non-escaping function parameter 'fn' to a call to a non-escaping function parameter can allow re-entrant modification of a variable}}
}

// rdar://problem/34496304
func test5(outer: (() throws -> Int) throws -> Int) throws -> Int {
  func descend(_ inner: (() throws -> Int) throws -> Int) throws -> Int {
    return try inner { // expected-error {{passing a closure which captures a non-escaping function parameter 'inner' to a call to a non-escaping function parameter can allow re-entrant modification of a variable}}
      try descend(inner)
    }
  }

  return try descend(outer)
}
