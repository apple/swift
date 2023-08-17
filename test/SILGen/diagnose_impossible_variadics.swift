// RUN: %target-swift-emit-silgen -verify -disable-availability-checking %s

func consume<V>(_ value: V) {}
func produce<V>(_ type: V.Type) -> V { preconditionFailure("called") }

func testConsume<each T>(fn: @escaping (repeat each T) -> Bool) {
  consume(fn)  // expected-error {{cannot fully abstract a value of variadic function type}}
}

func testConsumeClosure<each T>(_ type: (repeat each T).Type) {
  consume { (args: repeat each T) in true }  // expected-error {{cannot fully abstract a value of variadic function type}}
}

func testProduce<each T>() -> (repeat each T) -> Bool {
  return produce(((repeat each T) -> Bool).self)  // expected-error {{cannot fully abstract a value of variadic function type}}
}

func testProduceAndCall<each T>(args: repeat each T) -> Bool {
  return produce(((repeat each T) -> Bool).self)(repeat each args)  // expected-error {{cannot fully abstract a value of variadic function type}}
}
