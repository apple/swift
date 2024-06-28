// RUN: %target-typecheck-verify-swift -enable-experimental-feature FullTypedThrows

// REQUIRES: asserts

enum MyError: Error {
case failed
case epicFailed
}

enum HomeworkError: Error {
case dogAteIt
case forgot
}

func processMyError(_: MyError) { }

func doSomething() throws(MyError) { }
func doHomework() throws(HomeworkError) { }

func testDoCatchErrorTyped(cond: Bool) {
  do {
    throw MyError.failed
  } catch {
    assert(error == .failed)
    processMyError(error)
  }

  do {
    if cond {
      throw MyError.failed
    } else {
      throw HomeworkError.dogAteIt
    }
  } catch {
    processMyError(error) // expected-error{{cannot convert value of type 'any Error' to expected argument type 'MyError'}}
  }

  // Throwing a typed error in a do...catch catches the error with that type.
  do {
    try doSomething()
  } catch {
    assert(error == .failed)
    processMyError(error)
  }

  // Throwing a typed error in a do...catch lets us pattern-match against that
  // type.
  do {
    try doSomething()
  } catch .failed {
    // okay, matches one of the cases of MyError
  } catch {
    assert(error == .epicFailed)
  }

  // Rethrowing an error because the catch is not exhaustive.
  do {
    try doSomething()
    // expected-error@-1{{errors thrown from here are not handled because the enclosing catch is not exhaustive}}
  } catch .failed {
  }

  // "as X" errors are never exhaustive.
  do {
    try doSomething()
    // FIXME: should error errors thrown from here are not handled because the enclosing catch is not exhaustive
  } catch let error as MyError { // expected-warning{{'as' test is always true}}
    _ = error
  }

    // Rethrowing an error because the catch is not exhaustive.
  do {
    try doSomething()
    // expected-error@-1{{errors thrown from here are not handled because the enclosing catch is not exhaustive}}
  } catch is HomeworkError {
    // expected-warning@-1{{cast from 'MyError' to unrelated type 'HomeworkError' always fails}}
  }
}

func testDoCatchMultiErrorType() {
  // Throwing different typed errors results in 'any Error'
  do {
    try doSomething()
    try doHomework()
  } catch .failed { // expected-error{{type 'any Error' has no member 'failed'}}

  } catch {
    let _: Int = error // expected-error{{cannot convert value of type 'any Error' to specified type 'Int'}}
  }
}

func testDoCatchRethrowsUntyped() throws {
  do {
    try doSomething()
  } catch .failed {
  } // okay, rethrows with a conversion to 'any Error'
}

func testDoCatchRethrowsTyped() throws(HomeworkError) {
  do {
    try doHomework()
  } catch .dogAteIt {
  } // okay, rethrows

  do {
    try doSomething()
  } catch .failed {

  } // expected-error{{thrown expression type 'MyError' cannot be converted to error type 'HomeworkError'}}

  do {
    try doSomething()
    try doHomework()
  } catch let e as HomeworkError {
    _ = e
  } // expected-error{{thrown expression type 'any Error' cannot be converted to error type 'HomeworkError'}}

  do {
    try doSomething()
    try doHomework()
  } catch {

  } // okay, the thrown 'any Error' has been caught
}

func testTryIncompatibleTyped(cond: Bool) throws(HomeworkError) {
  try doHomework() // okay

  try doSomething() // expected-error{{thrown expression type 'MyError' cannot be converted to error type 'HomeworkError'}}

  do {
    if cond {
      throw .dogAteIt // expected-error{{type 'any Error' has no member 'dogAteIt'}}
    } else {
      try doSomething()
    }
  } catch let error as Never {
    // expected-warning@-1{{'catch' block is unreachable because no errors are thrown in 'do' block}}
    throw .forgot
  } // expected-error {{thrown expression type 'any Error' cannot be converted to error type 'HomeworkError'}}
}

func doSomethingWithoutThrowing() { }

func testDoCatchWithoutThrowing() {
  do {
    try doSomethingWithoutThrowing() // expected-warning{{no calls to throwing functions occur within 'try' expression}}
  } catch HomeworkError.forgot { // expected-warning{{'catch' block is unreachable because no errors are thrown in 'do' block}}
  } catch {
  }
}

// "Rethrow-like" functions are only allowed to be called from rethrows
// functions as a compatibility hack, which is removed under FullTypedThrows.
func rethrowsLike<E>(_ body: () throws(E) -> Void) throws(E) { }

func fromRethrows(body: () throws -> Void) rethrows {
  try rethrowsLike(body) // expected-error{{call can throw, but the error is not handled; a function declared 'rethrows' may only throw if its parameter does}}
}

// Explicit specification of the thrown type within a `do..catch` block.
func testDoCatchExplicitTyped() {
  do throws {
    try doHomework() // would normally infer HomeworkError
  } catch {
    let _: Int = error // expected-error{{cannot convert value of type 'any Error' to specified type 'Int'}}
  }

  do throws(any Error) {
    try doHomework() // would normally infer HomeworkError
  } catch {
    let _: Int = error // expected-error{{cannot convert value of type 'any Error' to specified type 'Int'}}
  }

  do throws(HomeworkError) {
    throw .forgot // okay, HomeworkError.forgot based on context
  } catch {
    let _: Int = error // expected-error{{cannot convert value of type 'HomeworkError' to specified type 'Int'}}
  }

  do throws(HomeworkError) { // expected-error{{a 'do' statement with a 'throws' clause must have at least one 'catch'}}
  }
}

func tryBangQuestionMismatchingContext() throws(MyError) {
  try! doHomework()
  try? doHomework()
  try doHomework() // expected-error{{thrown expression type 'HomeworkError' cannot be converted to error type 'MyError'}}
}

func apply<T, E: Error>(body: () throws(E) -> T) throws(E) -> T {
  return try body()
}

func testDoCatchErrorTypedInClosure(cond: Bool) {
  apply {
    do throws(MyError) {
      throw .failed
    } catch {
      assert(error == .failed)
      processMyError(error)
    }
  }
}

struct ThrowingMembers {
  subscript(i: Int) -> Int {
    get throws(MyError) { i }
  }

  var intOrThrows: Int {
    get throws(MyError) { 5 }
  }
}

struct ThrowingStaticSubscript {
  static subscript(i: Int) -> Int {
    get throws(MyError) { i }
  }

  static var intOrThrows: Int {
    get throws(MyError) { 5 }
  }
}

var globalIntOrThrows: Int {
  get throws(MyError) { 5 }
}

func testDoCatchInClosure(cond: Bool, x: ThrowingMembers) {
  apply {
    do {
      _ = try doSomething()
    } catch {
      let _: MyError = error
    }
  }

  apply {
    do {
      throw MyError.failed
    } catch {
      let _: MyError = error
    }
  }

  apply {
    do {
      if cond {
        throw MyError.failed
      }

      try doHomework()
    } catch {
      let _: MyError = error
      // expected-error@-1{{cannot convert value of type 'any Error' to specified type 'MyError'}}
    }
  }

  apply {
    do {
      do {
        _ = try doSomething()
      } catch .failed {
        // pick off one case, but this still rethrows
      }
    } catch {
      let _: MyError = error
    }
  }

  // Subscripts as potential throw sites
  apply {
    do {
      _ = try x[5]
    } catch {
      let _: MyError = error
    }
  }

  apply {
    do {
      _ = try ThrowingStaticSubscript[5]
    } catch {
      let _: MyError = error
    }
  }

  // Property accesses as potential throw sites
  apply {
    do {
      _ = try x.intOrThrows
    } catch {
      let _: MyError = error
    }
  }

  apply {
    do {
      _ = try ThrowingStaticSubscript.intOrThrows
    } catch {
      let _: MyError = error
    }
  }

  apply {
    do {
      _ = try globalIntOrThrows
    } catch {
      let _: MyError = error
    }
  }
}

func takesThrowingAutoclosure(_: @autoclosure () throws(MyError) -> Int) {}
func takesNonThrowingAutoclosure(_: @autoclosure () throws(Never) -> Int) {}

func getInt() throws -> Int { 0 }

func throwingAutoclosures() {
  takesThrowingAutoclosure(try getInt())
  // expected-error@-1 {{thrown expression type 'any Error' cannot be converted to error type 'MyError'}}

  takesNonThrowingAutoclosure(try getInt())
  // expected-error@-1 {{thrown expression type 'any Error' cannot be converted to error type 'Never'}}
}

func noThrow() throws(Never) {
  throw MyError.epicFailed
  // expected-error@-1 {{thrown expression type 'MyError' cannot be converted to error type 'Never'}}

  try doSomething()
  // expected-error@-1 {{thrown expression type 'MyError' cannot be converted to error type 'Never'}}

  do throws(Never) {
    throw MyError.epicFailed
    // expected-error@-1 {{thrown expression type 'MyError' cannot be converted to error type 'Never'}}
  } catch {}
}
