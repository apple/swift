// RUN: %target-typecheck-verify-swift -enable-experimental-feature TupleConformances -parse-stdlib

// REQUIRES: asserts

import Swift

protocol P {
  associatedtype A
  associatedtype B

  func f()
}

extension Builtin.TheTupleType: P where repeat each Elements: P {
  typealias A = (repeat (each Elements).A)
  typealias B = Float
  func f() {}
}

extension Int: P {
  typealias A = Int
  typealias B = String
  func f() {}
}

func returnsPA<T: P>(_: T) -> T.A.Type {}
func returnsPB<T: P>(_: T) -> T.B.Type {}

func same<T>(_: T, _: T) {}

func useConformance() {
  same(returnsPA((1, 2, 3)), (Int, Int, Int).self)
  same(returnsPB((1, 2, 3)), Float.self)

  (1, 2, 3).f()
}

////

extension Builtin.TheTupleType: Equatable where repeat each Elements: Equatable {
  // FIXME: Hack
  @_disfavoredOverload
  public static func ==(lhs: Self, rhs: Self) -> Bool {
    var result = true
    func update<E: Equatable>(lhs: E, rhs: E) {
      result = result && (lhs == rhs)
    }

    repeat update(lhs: each lhs, rhs: each rhs)
    return result
  }
}

extension Builtin.TheTupleType: Hashable where repeat each Elements: Hashable {
  public func hash(into hasher: inout Hasher) {
    repeat (each self).hash(into: &hasher)
  }
}
