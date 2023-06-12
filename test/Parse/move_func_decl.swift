// RUN: %target-typecheck-verify-swift  -disable-availability-checking

// rdar://100872195 (error: 'move' can only be applied to a local binding ('let', 'var', or parameter) , error: Can not use feature when experimental move only is disabled!)
//
// Identifiers with a single underscore are not reserved for use by the language implementation. It is perfectly valid for a library to define its own '_move'.
// The contextual consume keyword should only be parse when it is followed by an lvalue, so should *not* conflict with user-defined '_move' functions.
// https://github.com/apple/swift-evolution/blob/main/proposals/0366-move-function.md#source-compatibility

func _move<T>(t: T) -> T { return t }

func testUserMove() {
  let t = String()
  let _ = _move(t: t)
}

struct What {
  func _move(_ x: String) -> String { return x }

  func testMethod() {
    let t = String()
    let _ = _move(t)
  }
}
