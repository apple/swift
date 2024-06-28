// RUN: %empty-directory(%t)
// RUN: %target-build-swift %s -o %t/a.out
// RUN: %target-codesign %t/a.out
// RUN: %target-run %t/a.out
// REQUIRES: executable_test
protocol P {
  func foo() -> Int
}

struct X : P {
  func foo() -> Int { return 1 }
}

struct Y<T> {
  var wrapped: T
}

extension Y: P where T: P {
  func foo() -> Int { return wrapped.foo() + 10 }
}

func tryAsP(_ value: Any) -> Int {
  if let p = value as? P {
    return p.foo()
  }

  return 0
}

extension Dictionary: P where Value == (Key) -> Bool {
  func foo() -> Int { return 2 }
}


let yx = Y(wrapped: X())
precondition(tryAsP(yx) == 11)

let dict: [Int : (Int) -> Bool] = [:]
precondition(tryAsP(dict) == 2)

let yDict = Y(wrapped: dict)
precondition(tryAsP(yDict) == 12)

// <rdar://128774651> Generic argument indexing runs off the end when a key
// argument comes after a non-key argument.
struct Outer<T> {}

extension Outer where T == Int {
  struct Inner<U> {
    // Add some stored properties that will have field offsets in the metadata.
    // If we read past the end of the generic arguments, we'll find these and
    // crash on something that's definitely not a valid pointer.
    var a: T?
    var b: T?
    var c: T?
    var d: T?
  }
}

extension Outer.Inner: P where U == String {
  func foo() -> Int { return 1 }
}

let conformingInner: Any = Outer.Inner<String>()
let nonconformingInner: Any = Outer.Inner<Int>()
precondition(conformingInner is P)
precondition(!(nonconformingInner is P))
