// RUN: %target-swift-emit-silgen -verify %s

struct NC: ~Copyable {}

enum NoncopyableEnum: ~Copyable {
  case copyable(Int)
  case noncopyable(NC)
}

func test(foo: consuming NoncopyableEnum) {
  switch foo {
  case let x: // expected-warning{{'x' was never used}}
    break
  case .copyable(let x): // expected-warning{{already handled by previous patterns}} expected-warning{{'x' was never used}}
    break
  }
}
