// RUN: %target-typecheck-verify-swift -disable-availability-checking

// REQUIRES: asserts

typealias FnType = @isolated(any) () -> ()

func testInParameter(function: @isolated(any) () -> ()) {}

func testLookahead() {
  let array = [@isolated(any) () -> ()]()
  _ = array
}

func testInvalidIsolation(_ x: @isolated(foo) () -> Void) {}
// expected-error@-1 {{expected 'any' as the isolation kind}} {{42-45=any}}
