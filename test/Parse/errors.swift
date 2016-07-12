// RUN: %target-parse-verify-swift

enum MSV : ErrorProtocol {
  case Foo, Bar, Baz
  case CarriesInt(Int)

  var domain: String { return "" }
  var code: Int { return 0 }
}

func opaque_error() -> ErrorProtocol { return MSV.Foo }

func one() {
  do {
    true ? () : throw opaque_error() // expected-error {{expected expression after '? ... :' in ternary expression}}
  } catch {
    _ = error
  }

  do {
    
  } catch { // expected-warning {{'catch' block is unreachable because no errors are thrown in 'do' block}}
    let error2 = error
  }

  do {
  } catch where true { // expected-warning {{'catch' block is unreachable because no errors are thrown in 'do' block}}
    let error2 = error
  } catch {
    // discard
  }
  
  // <rdar://problem/20985280> QoI: improve diagnostic on improper pattern match on type
  do {
    throw opaque_error()
  } catch MSV { // expected-error {{'is' keyword required to pattern match against type name}} {{11-11=is }}
  } catch {
    // discard
  }

  do {
    throw opaque_error()
  } catch is ErrorProtocol {  // expected-warning {{'is' test is always true}} expected-warning 2 {{catch' block is empty, errors thrown in 'do' block are unhandled}}
    
  }
  
  func foo() throws {}
  
  do {
#if false
    try foo()
#endif
  } catch {    // don't warn, #if code should be scanned.
    _ = error
  }

  do {
#if false
    throw opaque_error()
#endif
  } catch {    // don't warn, #if code should be scanned.
    _ = error
  }
  
  // Catch block not handling error
  do {
  } catch {}    // expected-warning {{'catch' block is unreachable because no errors are thrown in 'do' block}}

  do {
    #if false
      throw opaque_error()
    #endif
  } catch {}  // expected-warning {{'catch' block is empty, errors thrown in 'do' block are unhandled}}
  
  do {
    try genError()
  } catch {} // expected-warning {{'catch' block is empty, errors thrown in 'do' block are unhandled}}
  
}

func takesAutoclosure(_ fn : @autoclosure () -> Int) {}
func takesThrowingAutoclosure(_ fn : @autoclosure () throws -> Int) {}

func genError() throws -> Int { throw MSV.Foo }
func genNoError() -> Int { return 0 }

func testAutoclosures() throws {
  takesAutoclosure(genError()) // expected-error {{call can throw, but it is not marked with 'try' and it is executed in a non-throwing autoclosure}}
  takesAutoclosure(genNoError())

  try takesAutoclosure(genError()) // expected-error {{call can throw, but it is executed in a non-throwing autoclosure}}
  try takesAutoclosure(genNoError()) // expected-warning {{no calls to throwing functions occur within 'try' expression}}

  takesAutoclosure(try genError()) // expected-error {{call can throw, but it is executed in a non-throwing autoclosure}}
  takesAutoclosure(try genNoError()) // expected-warning {{no calls to throwing functions occur within 'try' expression}}

  takesThrowingAutoclosure(try genError())
  takesThrowingAutoclosure(try genNoError()) // expected-warning {{no calls to throwing functions occur within 'try' expression}}

  try takesThrowingAutoclosure(genError())
  try takesThrowingAutoclosure(genNoError()) // expected-warning {{no calls to throwing functions occur within 'try' expression}}

  takesThrowingAutoclosure(genError()) // expected-error {{call can throw but is not marked with 'try'}}
  takesThrowingAutoclosure(genNoError())
}

struct IllegalContext {
  var x: Int = genError() // expected-error {{call can throw, but errors cannot be thrown out of a property initializer}}

  func foo(_ x: Int = genError()) {} // expected-error {{call can throw, but errors cannot be thrown out of a default argument}}

  func catcher() throws {
    do {
      _ = try genError()
    } catch MSV.CarriesInt(genError()) { // expected-error {{call can throw, but errors cannot be thrown out of a catch pattern}}
    } catch MSV.CarriesInt(let i) where i == genError() { // expected-error {{call can throw, but errors cannot be thrown out of a catch guard expression}}
    }
  }
}

func illformed() throws {
    do {
      _ = try genError()

    // TODO: this recovery is terrible
    } catch MSV.CarriesInt(let i) where i == genError()) { // expected-error {{call can throw, but errors cannot be thrown out of a catch guard expression}} expected-error {{expected '{'}} expected-error {{braced block of statements is an unused closure}} expected-error {{expression resolves to an unused function}}
    }
}

func postThrows() -> Int throws { // expected-error{{'throws' may only occur before '->'}}{{19-19=throws }}{{25-32=}}
  return 5
}

func postThrows2() -> throws Int { // expected-error{{'throws' may only occur before '->'}}{{20-22=throws}}{{23-29=->}}
  return try postThrows()
}

func postRethrows(_ f: () throws -> Int) -> Int rethrows { // expected-error{{'rethrows' may only occur before '->'}}{{42-42=rethrows }}{{48-57=}}
  return try f()
}

func postRethrows2(_ f: () throws -> Int) -> rethrows Int { // expected-error{{'rethrows' may only occur before '->'}}{{43-45=rethrows}}{{46-54=->}}
  return try f()
}

// rdar://21328447
func fixitThrow0() throw {} // expected-error{{expected throwing specifier; did you mean 'throws'?}} {{20-25=throws}}
func fixitThrow1() throw -> Int {} // expected-error{{expected throwing specifier; did you mean 'throws'?}} {{20-25=throws}}
func fixitThrow2() throws {
  var _: (Int)
  throw MSV.Foo
  var _: (Int) throw -> Int // expected-error{{expected throwing specifier; did you mean 'throws'?}} {{16-21=throws}}
}
