// RUN: %target-swift-emit-silgen -verify %s

func consume_a_pointer(x: inout Int) { }

func func_that_rethrows<E: Error>(initializingWith callback: () throws(E) -> Void) throws(E) {
}

public func foo() {
  var pk = 42
  func_that_rethrows() {
    consume_a_pointer(x: &pk)
  }
}
