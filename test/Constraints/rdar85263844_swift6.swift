// RUN: %target-typecheck-verify-swift -swift-version 6

// '-swift-version 6' is currently asserts-only
// REQUIRES: asserts

// rdar://85263844 - initializer 'init(_:)' requires the types be equivalent
func rdar85263844(arr: [(q: String, a: Int)]) -> AnySequence<(question: String, answer: Int)> {
  AnySequence(arr.map { $0 })
  // expected-error@-1 {{initializer 'init(_:)' requires the types '(question: String, answer: Int)' and '(q: String, a: Int)' be equivalent}}
}

// Another case for rdar://85263844
protocol P {
  associatedtype Element
}
extension Array : P {}

public struct S4<T> {
  init<S : P>(_ x: S) where S.Element == T {}
  init(_ x: Int) {}
}

extension S4 where T == (outer: Int, y: Int) {
  init(arr: [Int]) {
    // FIXME: This ideally shouldn't compile either, but because of the way we
    // generate constraints for it, it continues to compile. We should fix
    // tuple subtyping for Swift 6 mode to not accept label mismatches.
    self.init(arr.map { (inner: $0, y: $0) })
    // expected-warning@-1 {{tuple conversion from '(inner: Int, y: Int)' to '(outer: Int, y: Int)' mismatches labels}}
  }
}

public func rdar85263844_2(_ x: [Int]) -> S4<(outer: Int, y: Int)> {
  // FIXME: Bad error message.
  S4(x.map { (inner: $0, y: $0) }) // expected-error {{type of expression is ambiguous without a type annotation}}
}
