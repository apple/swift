// RUN: %target-typecheck-verify-swift -disable-availability-checking

// REQUIRES: asserts

typealias FnType = @isolated(any) () -> ()

func testInParameter(function: @isolated(any) () -> ()) {}

func testLookahead() {
  let array = [@isolated(any) () -> ()]()
  _ = array
}
