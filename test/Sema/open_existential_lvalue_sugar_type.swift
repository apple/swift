// RUN: %target-typecheck-verify-swift

// rdar://121214563
// REQUIRES: rdar121214563

// https://github.com/apple/swift/issues/62219
do {
  struct Action {
      var intVar: Int
      var strVar: String
  }

  protocol TestDelegate: AnyObject {
      associatedtype ActionType
      var actions: [ActionType] { get set }
  }

  class TestDelegateImpl: TestDelegate {
      typealias ActionType = Action
      var actions: [Action] = []
  }

  class TestViewController {
      var testDelegate: (any TestDelegate)?
      func testFunc() {
          testDelegate?.actions.removeAll() // expected-error {{cannot use mutating member on immutable value: 'self' is immutable}}
      }
  }
}
