// RUN: %target-typecheck-verify-swift

enum MyError : Error {
  case bad
}

class Base {
  init(_ fn: () throws -> ()) rethrows {} // expected-note {{overridden declaration is here}}

  convenience init(rethrows: (), fn: () throws -> ()) rethrows {
    try self.init(fn)
  }

  convenience init(throws1: (), fn: () throws -> ()) rethrows {
    throw MyError.bad // expected-error {{a function declared 'rethrows' may only throw if its parameter does}}
  }

  convenience init(throws2: (), fn: () throws -> ()) rethrows {
    try self.init { throw MyError.bad }
    // expected-error@-1 {{call can throw, but the error is not handled; a function declared 'rethrows' may only throw if its parameter does}}
    // expected-note@-2 {{call is to 'rethrows' function, but argument function can throw}}
  }

  convenience init() {
    self.init {}
  }
}

class DerivedInheritInit : Base {}

_ = DerivedInheritInit {}
_ = try DerivedInheritInit { throw MyError.bad }

_ = DerivedInheritInit(rethrows: ()) {}
_ = try DerivedInheritInit(rethrows: ()) { throw MyError.bad }

class DerivedOverrideInitBad : Base {
  override init(_ fn: () throws -> ()) throws {} // expected-error {{override of 'rethrows' initializer should also be 'rethrows'}}
}

class DerivedOverrideInitGood : Base {
  override init(_ fn: () throws -> ()) rethrows {}
}

_ = DerivedOverrideInitGood {}
_ = try DerivedOverrideInitGood { throw MyError.bad }

_ = DerivedOverrideInitGood(rethrows: ()) {}
_ = try DerivedOverrideInitGood(rethrows: ()) { throw MyError.bad }

class DerivedWithSuperInitCall : Base {
  override init(_ fn: () throws -> ()) rethrows {
    try super.init(fn)
  }

  init(throws1: (), _ fn: () throws -> ()) rethrows {
    throw MyError.bad // expected-error {{a function declared 'rethrows' may only throw if its parameter does}}
  }

  init(throws2: (), _ fn: () throws -> ()) rethrows {
    try super.init { throw MyError.bad }
    // expected-error@-1 {{call can throw, but the error is not handled; a function declared 'rethrows' may only throw if its parameter does}}
    // expected-note@-2 {{call is to 'rethrows' function, but argument function can throw}}
  }
}
