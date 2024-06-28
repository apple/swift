// RUN: %empty-directory(%t)
// RUN: %target-swift-frontend -emit-module -emit-module-path %t/UnavailableFunction.swiftmodule -module-name UnavailableFunction -strict-concurrency=complete %S/Inputs/UnavailableFunction.swift
// RUN: %target-swift-frontend -verify -I %t %s -emit-sil -o /dev/null
// RUN: %target-swift-frontend -verify -I %t %s -emit-sil -o /dev/null -strict-concurrency=targeted
// RUN: %target-swift-frontend -verify -I %t %s -emit-sil -o /dev/null -strict-concurrency=complete

// REQUIRES: concurrency
// REQUIRES: asserts

import UnavailableFunction

@available(SwiftStdlib 5.1, *)
func okay() {}

// expected-error@+1{{'@_unavailableFromAsync' attribute cannot be applied to this declaration}}
@_unavailableFromAsync
@available(SwiftStdlib 5.1, *)
struct Foo { }

// expected-error@+1{{'@_unavailableFromAsync' attribute cannot be applied to this declaration}}
@_unavailableFromAsync
@available(SwiftStdlib 5.1, *)
extension Foo { }

// expected-error@+1{{'@_unavailableFromAsync' attribute cannot be applied to this declaration}}
@_unavailableFromAsync
@available(SwiftStdlib 5.1, *)
class Bar {
  // expected-error@+1{{'@_unavailableFromAsync' attribute cannot be applied to this declaration}}
  @_unavailableFromAsync
  deinit { }
}

// expected-error@+1{{'@_unavailableFromAsync' attribute cannot be applied to this declaration}}
@_unavailableFromAsync
@available(SwiftStdlib 5.1, *)
actor Baz { }

@available(SwiftStdlib 5.1, *)
struct Bop {
  @_unavailableFromAsync(message: "Use Bop(a: Int) instead")
  init() {}                 // expected-note 4 {{'init()' declared here}}

  init(a: Int) { }
}

@available(SwiftStdlib 5.1, *)
extension Bop {
  @_unavailableFromAsync
  func foo() {}             // expected-note 4 {{'foo()' declared here}}


  @_unavailableFromAsync
  mutating func muppet() { }  // expected-note 4 {{'muppet()' declared here}}
}

@_unavailableFromAsync
@available(SwiftStdlib 5.1, *)
func foo() {}               // expected-note 4 {{'foo()' declared here}}

@available(SwiftStdlib 5.1, *)
func makeAsyncClosuresSynchronously(bop: inout Bop) -> (() async -> Void) {
  return { () async -> Void in
    // Unavailable methods
    _ = Bop()     // expected-warning@:9{{'init' is unavailable from asynchronous contexts; Use Bop(a: Int) instead}}
    _ = Bop(a: 32)
    bop.foo()     // expected-warning@:9{{'foo' is unavailable from asynchronous contexts}}
    bop.muppet()  // expected-warning@:9{{'muppet' is unavailable from asynchronous contexts}}
    unavailableFunction() // expected-warning@:5{{'unavailableFunction' is unavailable from asynchronous contexts}}
    noasyncFunction() // expected-warning@:5{{'noasyncFunction' is unavailable from asynchronous contexts}}

    // Can use them from synchronous closures
    _ = { Bop() }()
    _ = { bop.foo() }()
    _ = { bop.muppet() }()
    _ = { noasyncFunction() }()

    // Unavailable global function
    foo()         // expected-warning{{'foo' is unavailable from asynchronous contexts}}

    // Okay function
    okay()
  }
}

@available(SwiftStdlib 5.1, *)
@_unavailableFromAsync
func asyncFunc() async { // expected-error{{asynchronous global function 'asyncFunc()' must be available from asynchronous contexts}}

  var bop = Bop(a: 32)
  _ = Bop()     // expected-warning@:7{{'init' is unavailable from asynchronous contexts; Use Bop(a: Int) instead}}
  bop.foo()     // expected-warning@:7{{'foo' is unavailable from asynchronous contexts}}
  bop.muppet()  // expected-warning@:7{{'muppet' is unavailable from asynchronous contexts}}
  unavailableFunction() // expected-warning@:3{{'unavailableFunction' is unavailable from asynchronous contexts}}
  noasyncFunction() // expected-warning@:3{{'noasyncFunction' is unavailable from asynchronous contexts}}

  // Unavailable global function
  foo()         // expected-warning{{'foo' is unavailable from asynchronous contexts}}

  // Available function
  okay()

  _ = { () -> Void in
    // Check unavailable things inside of a nested synchronous closure
    _ = Bop()
    foo()
    bop.foo()
    bop.muppet()
    unavailableFunction()
    noasyncFunction()

    _ = { () async -> Void in
      // Check Unavailable things inside of a nested async closure
      foo()           // expected-warning@:7{{'foo' is unavailable from asynchronous contexts}}
      bop.foo()       // expected-warning@:11{{'foo' is unavailable from asynchronous contexts}}
      bop.muppet()    // expected-warning@:11{{'muppet' is unavailable from asynchronous contexts}}
      _ = Bop()       // expected-warning@:11{{'init' is unavailable from asynchronous contexts; Use Bop(a: Int) instead}}
      unavailableFunction() // expected-warning@:7{{'unavailableFunction' is unavailable from asynchronous contexts}}
      noasyncFunction() // expected-warning@:7{{'noasyncFunction' is unavailable from asynchronous contexts}}
    }
  }

  _ = { () async -> Void in
    _ = Bop()     // expected-warning@:9{{'init' is unavailable from asynchronous contexts; Use Bop(a: Int) instead}}
    foo()         // expected-warning@:5{{'foo' is unavailable from asynchronous contexts}}
    bop.foo()     // expected-warning@:9{{'foo' is unavailable from asynchronous contexts}}
    bop.muppet()  // expected-warning@:9{{'muppet' is unavailable from asynchronous contexts}}
    unavailableFunction() // expected-warning@:5{{'unavailableFunction' is unavailable from asynchronous contexts}}
    noasyncFunction() // expected-warning@:5{{'noasyncFunction' is unavailable from asynchronous contexts}}

    _ = {
      foo()
      bop.foo()
      _ = Bop()
      unavailableFunction()
    }
  }

}

// Parsing tests

// expected-error@+2 {{expected declaration}}
// expected-error@+1:24{{unknown option 'nope' for attribute '_unavailableFromAsync'}}
@_unavailableFromAsync(nope: "almost right, but not quite")
func blarp1() {}

// expected-error@+2 {{expected declaration}}
// expected-error@+1 {{expected ':' after label 'message'}}
@_unavailableFromAsync(message; "almost right, but not quite")
func blarp2() {}

// expected-error@+1:31 {{'=' has been replaced with ':' in attribute arguments}}{{31-32=: }}
@_unavailableFromAsync(message="almost right, but not quite")
func blarp3() {}

// expected-error@+2 {{expected declaration}}
// expected-error@+1 {{expected string literal in '_unavailableFromAsync' attribute}}
@_unavailableFromAsync(message: 32)
func blarp4() {}

// expected-error@+2 {{expected declaration}}
// expected-error@+1 {{message cannot be an interpolated string}}
@_unavailableFromAsync(message: "blarppy blarp \(31 + 10)")
func blarp5() {}

// expected-error@+1:48 {{expected ')' in '_unavailableFromAsync' attribute}}{{48-48=)}}
@_unavailableFromAsync(message: "blarppy blarp"
func blarp6() {}
