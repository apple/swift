// RUN: %target-typecheck-verify-swift
enum MSV : Error {
  case Foo, Bar, Baz

  var _domain: String { return "" }
  var _code: Int { return 0 }
}

func a() {}
func b() {}
func c() {}
func d() {}
func e() {}
func thrower() throws {}

func opaque_error() -> Error { return MSV.Foo }

func one() {
  throw MSV.Foo // expected-error {{error is not handled because the enclosing function is not declared 'throws'}}
}

func two() {
  throw opaque_error() // expected-error {{error is not handled because the enclosing function is not declared 'throws'}}
}

func three() {
  do {
    throw opaque_error() // expected-error {{error is not handled because the enclosing catch is not exhaustive}}
  } catch let e as MSV {
    _ = e
  }
}

func four() {
  do {
    throw opaque_error()
  } catch let e {
    _ = e
  }
}

func five() {
  do {
    throw opaque_error()
  } catch let e as MSV {
    _ = e
  } catch _ {
  }
}

func six() {
  do {
    do {
      throw opaque_error()
    } catch let e as MSV {
      _ = e
    }
  } catch _ {
  }
}

func seven_helper() throws -> Int { throw MSV.Baz }

struct seven {
  var x: Int {
    do {
      return try seven_helper()
    } catch {
      return 0
    }
  }

  var y: Int {
    return try! seven_helper()
  }

  var z: Int {
    return (try? seven_helper()) ?? 0
  }
}

class eight {
  lazy var x: Int = {
    do {
      return try seven_helper()
    } catch {
      return 0
    }
  }()

  lazy var y: Int = {
    return try! seven_helper()
  }()

  lazy var z: Int = {
    return (try? seven_helper()) ?? 0
  }()
}

func multiPattern() {
  do {
    throw opaque_error()
  } catch MSV.Foo, _ {
    _ = e
  }
}

protocol ThrowingProto {
  func foo() throws
  static func bar() throws
}

func testExistential(_ p : ThrowingProto) throws {
  try p.foo()
  try type(of: p).bar()
}
func testGeneric<P : ThrowingProto>(p : P) throws {
  try p.foo()
  try P.bar()
}

// Don't warn about the "useless" try in these cases.
func nine_helper(_ x: Int, y: Int) throws {} // expected-note {{'nine_helper(_:y:)' declared here}}
func nine() throws {
  try nine_helper(y: 0) // expected-error {{missing argument for parameter #1 in call}}
}
func ten_helper(_ x: Int) {}
func ten_helper(_ x: Int, y: Int) throws {} // expected-note {{'ten_helper(_:y:)' declared here}}
func ten() throws {
  try ten_helper(y: 0) // expected-error {{missing argument for parameter #1 in call}} {{18-18=<#Int#>, }}
}

// rdar://21074857
func eleven_helper(_ fn: () -> ()) {}
func eleven_one() {
  eleven_helper {
    do {
      try thrower()
    // FIXME: suppress the double-emission of the 'always true' warning
    } catch let e as Error { // expected-warning {{immutable value 'e' was never used}} {{17-18=_}} expected-warning 2 {{'as' test is always true}}
    }
  }
}
func eleven_two() {
  eleven_helper { // expected-error {{invalid conversion from throwing function of type '() throws -> ()' to non-throwing function type '() -> ()'}}
    do {
      try thrower()
    } catch let e as MSV {
    }
  }
}

enum Twelve { case Payload(Int) }
func twelve_helper(_ fn: (Int, Int) -> ()) {}
func twelve() {
  twelve_helper { (a, b) in // expected-error {{invalid conversion from throwing function of type '(Int, Int) throws -> ()' to non-throwing function type '(Int, Int) -> ()'}}
    do {
      try thrower()
    } catch Twelve.Payload(a...b) {
    }
  }
}

struct Thirteen : Error, Equatable {}
func ==(a: Thirteen, b: Thirteen) -> Bool { return true }

func thirteen_helper(_ fn: (Thirteen) -> ()) {}
func thirteen() {
  thirteen_helper { (a) in // expected-error {{invalid conversion from throwing function of type '(Thirteen) throws -> ()' to non-throwing function type '(Thirteen) -> ()'}}
    do {
      try thrower()
      // FIXME: Bad diagnostic (https://github.com/apple/swift/issues/63459)
    } catch a { // expected-error {{binary operator '~=' cannot be applied to two 'any Error' operands}}
    }
  }
}

// https://github.com/apple/swift/issues/48950
protocol ClassProto: AnyObject {}
do {
  enum E: Error {
    case castError
  }

  do {
    struct S1 {}
    struct S2: Error {}

    do {
      throw E.castError
    } catch is S1 {} // expected-warning {{cast from 'any Error' to unrelated type 'S1' always fails}}

    do {
      throw E.castError
    } catch is S2 {} // Ok
  }

  do {
    class C1 {}
    class C2: ClassProto & Error {}

    do {
      throw E.castError
    } catch let error as C1 { // Okay
      print(error)
    } catch {}

    do {
      throw E.castError
    } catch let error as C2 { // Okay
      print(error)
    } catch {}

    let err: Error
    _ = err as? (C1 & Error) // Ok
    _ = err as? (Error & ClassProto) // Ok
  }

  func f<T>(error: Error, as type: T.Type) -> Bool {
    return (error as? T) != nil // Ok
  }
}

// https://github.com/apple/swift/issues/53803
protocol P {}
do {
  class Super {}
  class Sub: Super, P {}
  final class Final {}

  let x: any P

  if let _ = x as? Super {} // Okay

  if let _ = x as? Final {} // expected-warning {{cast from 'any P' to unrelated type 'Final' always fails}}
}

// https://github.com/apple/swift/issues/56091
do {
  func f() throws -> String {}

  func invalid_interpolation() {
    _ = try "\(f())" // expected-error {{errors thrown from here are not handled}}
    _ = "\(try f())" // expected-error {{errors thrown from here are not handled}}
  }

  func valid_interpolation() throws {
    _ = try "\(f())"
    _ = "\(try f())"
  }
}

// rdar://problem/72748150
func takesClosure(_: (() -> ())) throws -> Int {}

func passesClosure() {
    _ = try takesClosure { } // expected-error {{errors thrown from here are not handled}}
}
