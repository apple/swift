// RUN: %target-typecheck-verify-swift -swift-version 4 -enable-builtin-module
import Builtin

func int8Or16(_ x: Int8) -> Int8 { return x }
func int8Or16(_ x: Int16) -> Int16 { return x }

// Explicit one-way constraints for testing purposes.
func testTernaryOneWay(b: Bool) {
  // Okay: backward inference works.
  let _: Float = b ? 3.14159 : 17

  // Errors due to one-way inference.
  let _: Float = b ? Builtin.one_way(3.14159) // expected-error{{cannot convert value of type 'Double' to specified type 'Float'}}
                   : 17
  let _: Float = b ? 3.14159 // expected-error {{cannot convert value of type 'Int' to specified type 'Float'}}
                   : Builtin.one_way(17)
  let _: Float = b ? Builtin.one_way(3.14159) // expected-error {{cannot convert value of type 'Double' to specified type 'Float'}}
                   : Builtin.one_way(17)

  // Okay: default still works.
  let _: Double = b ? Builtin.one_way(3.14159) : 17
  let _: Double = b ? 3.14159 : Builtin.one_way(17)
  // FIXME: expected-error@-1{{cannot convert value of type 'Int' to specified type 'Double'}}
  // The above fails because we are minimizing the set of partial solutions
  // for the integer literal 17, so we don't try it as a Double

  let _: Int8 = b ? Builtin.one_way(int8Or16(17)) : Builtin.one_way(int8Or16(42))
}
