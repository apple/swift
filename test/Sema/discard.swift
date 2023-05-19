// RUN: %target-typecheck-verify-swift -enable-experimental-feature MoveOnlyEnumDeinits

// Typechecking for the discard statement.

func discard() -> Int {}

let x = discard()

discard x // expected-error {{'discard' statement cannot appear in top-level code}}

func `discard`(_ t: Int) {} // expected-note {{'discard' declared here}}

func globalFunc() {
  discard x // expected-error {{'discard' statement cannot appear in global function}}
}

class C {
  deinit {
    discard self // expected-error {{'discard' statement cannot appear in deinitializer}}
  }
}

struct S {

  static func staticFunc() {
    discard x // expected-error {{'discard' statement cannot appear in static method}}
  }

  var member: Int {
    get {
      discard x // expected-error {{'discard' statement can only appear in noncopyable type's member}}
    }
  }

  __consuming func f() {
    discard self // expected-error {{'discard' statement can only appear in noncopyable type's member}}
  }
}

enum E: Error { case err }

@_moveOnly struct File {
  let fd: Int

  init() throws {
    self.fd = 0

    discard self // expected-error {{'discard' statement cannot appear in initializer}}

    throw E.err
  }

  init(_ b: Bool) throws {
    try self.init()

    discard self // expected-error {{'discard' statement cannot appear in initializer}}

    throw E.err
  }

  __consuming func close() {
    defer { discard self }

    if fd >= 0 {
      discard self
      return
    }

    discard self
  }

  var what: Int {
    __consuming get {
      discard self
      return 0
    }
  }

  __consuming func badClose() {
    discard self.fd // expected-error {{you can only discard 'self'}}{{13-20=self}}
    // expected-error@-1 {{cannot convert value of type 'Int' to expected discard type 'File'}}

    discard Self // expected-error {{you can only discard 'self'}}{{13-17=self}}
    // expected-error@-1 {{cannot convert value of type 'File.Type' to expected discard type 'File'}}

    discard Self.self // expected-error {{you can only discard 'self'}}{{13-22=self}}
    // expected-error@-1 {{cannot convert value of type 'File.Type' to expected discard type 'File'}}

    discard self + self // expected-error {{you can only discard 'self'}}{{13-24=self}}
    // expected-error@-1 {{binary operator '+' cannot be applied to two 'File' operands}}

    // NOTE: I think this error comes from it trying to call discard's subscript
    discard [self]  // expected-error {{reference to member 'subscript' cannot be resolved without a contextual type}}

    discard {} // expected-error {{trailing closure passed to parameter of type 'Int' that does not accept a closure}}

    discard (self) // expected-error {{cannot convert value of type 'File' to expected argument type 'Int'}}

    // FIXME: we should get an error about it being illegal to discard in a closure.
    let _ = { // expected-error {{type of expression is ambiguous without more context}}
      discard self
      return 0
    }()

    func hello() {
      discard self // expected-error {{'discard' statement cannot appear in local function}}
    }
  }

  deinit {
    discard self // expected-error {{'discard' statement cannot appear in deinitializer}}
  }

  static func staticFunc() {
    discard x // expected-error {{'discard' statement cannot appear in static method}}
  }
}

@_moveOnly enum FileWrapper {
  case valid(File)
  case invalid(File)
  case nothing

  init() throws {
    discard self // expected-error {{'discard' statement cannot appear in initializer}}
    throw E.err
  }

  __consuming func take() throws -> File {
    if case let .valid(f) = self {
      return f
    }
    discard self
    throw E.err
  }

  var validFile: File {
    __consuming get {
      if case let .valid(f) = self {
        return f
      }
      discard self
    }
  }

  deinit {
    try? take().close()
  }
}

struct NoDeinitStruct: ~Copyable {
  consuming func blah() {
    discard self // expected-error {{'discard' has no effect for type 'NoDeinitStruct' unless it has a deinitializer}}{{5-18=}}
  }
}

enum NoDeinitEnum: ~Copyable {
  case whatever

  consuming func blah() {
    discard self // expected-error {{'discard' has no effect for type 'NoDeinitEnum' unless it has a deinitializer}}{{5-18=}}
  }
}

struct HasGenericNotStored<T>: ~Copyable { // expected-note 2{{arguments to generic parameter 'T' ('Int' and 'T') are expected to be equal}}
  consuming func discard() { discard self }

  consuming func bad_discard1() {
    discard HasGenericNotStored<Int>()
    // expected-error@-1 {{cannot convert value of type 'HasGenericNotStored<Int>' to expected discard type 'HasGenericNotStored<T>'}}
    // expected-error@-2 {{you can only discard 'self'}}
  }

  consuming func bad_discard2() {
    let `self` = HasGenericNotStored<Int>()
    discard `self`
    // expected-error@-1 {{cannot convert value of type 'HasGenericNotStored<Int>' to expected discard type 'HasGenericNotStored<T>'}}
    // expected-error@-2 {{you can only discard 'self'}}{{13-19=self}}
  }

  func identity(_ t: T) -> T { return t }
  deinit{}
}

struct Court: ~Copyable {
  let x: Int

  consuming func discard(_ other: consuming Court) {
    let `self` = other
    discard `self` // expected-error {{you can only discard 'self'}}{{13-19=self}}
  }

  deinit { print("deinit of \(self.x)") }
}
