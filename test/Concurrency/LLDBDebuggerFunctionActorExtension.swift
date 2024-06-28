// RUN: %target-swift-frontend  -disable-availability-checking -debugger-support %s -emit-sil -o /dev/null -verify
// RUN: %target-swift-frontend  -disable-availability-checking -debugger-support %s -emit-sil -o /dev/null -verify -strict-concurrency=targeted
// RUN: %target-swift-frontend  -disable-availability-checking -debugger-support %s -emit-sil -o /dev/null -verify -strict-concurrency=complete

// REQUIRES: concurrency
// REQUIRES: asserts

// This test simulates LLDB's expression evaluator making an otherwise illegal
// synchronous call into an extension of an actor, as it would to run `p n` in
// this example.

actor A {
  var n : Int = 0
}

extension A {
  final func lldb_wrapped_expr() {
    n = 1
  }
}

@LLDBDebuggerFunction
func lldb_expr(a: A) {
  a.lldb_wrapped_expr()
}
