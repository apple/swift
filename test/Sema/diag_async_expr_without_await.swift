// RUN: %target-typecheck-verify-swift -disable-availability-checking

// REQUIRES: concurrency
class A {}
class B: A {}
typealias Foo = (name: String, age: Int)
func test() async {
  async let result: B? = nil
  async let person: Foo? = nil
  if let result: A = result {} // expected-error {{expression is 'async' but is not marked with 'await'}} {{22-22=await }}
  // expected-warning@-1 {{immutable value 'result' was never used; consider replacing with '_' or removing it}}
  // expected-note@-2 {{reference to async let 'result' is 'async'}}
  if let result: A {} // expected-error {{expression is 'async' but is not marked with 'await'}} {{19-19= = await result}}
  // expected-warning@-1 {{immutable value 'result' was never used; consider replacing with '_' or removing it}}
  // expected-note@-2 {{reference to async let 'result' is 'async'}}
  if let result = result {} // expected-error {{expression is 'async' but is not marked with 'await'}} {{19-19=await }}
  // expected-warning@-1 {{value 'result' was defined but never used; consider replacing with boolean test}}
  // expected-note@-2 {{reference to async let 'result' is 'async'}}
  if let result {} // expected-error {{expression is 'async' but is not marked with 'await'}} {{16-16= = await result}}
  // expected-warning@-1 {{value 'result' was defined but never used; consider replacing with boolean test}}
  // expected-note@-2 {{reference to async let 'result' is 'async'}}
  if let person: Foo = person {} // expected-error {{expression is 'async' but is not marked with 'await'}} {{24-24=await }}
  // expected-warning@-1 {{immutable value 'person' was never used; consider replacing with '_' or removing it}}
  // expected-note@-2 {{reference to async let 'person' is 'async'}}
  if let person: Foo {} // expected-error {{expression is 'async' but is not marked with 'await'}} {{21-21= = await person}}
  // expected-warning@-1 {{immutable value 'person' was never used; consider replacing with '_' or removing it}}
  // expected-note@-2 {{reference to async let 'person' is 'async'}}
  if let person: (String, Int) = person {} // expected-error {{expression is 'async' but is not marked with 'await'}} {{34-34=await }}
  // expected-warning@-1 {{immutable value 'person' was never used; consider replacing with '_' or removing it}}
  // expected-note@-2 {{reference to async let 'person' is 'async'}}
  if let person: (String, Int) {} // expected-error {{expression is 'async' but is not marked with 'await'}} {{31-31= = await person}}
  // expected-warning@-1 {{immutable value 'person' was never used; consider replacing with '_' or removing it}}
  // expected-note@-2 {{reference to async let 'person' is 'async'}}
  if let person = person {} // expected-error {{expression is 'async' but is not marked with 'await'}} {{19-19=await }}
  // expected-warning@-1 {{value 'person' was defined but never used; consider replacing with boolean test}}
  // expected-note@-2 {{reference to async let 'person' is 'async'}}
  if let person {} // expected-error {{expression is 'async' but is not marked with 'await'}} {{16-16= = await person}}
  // expected-warning@-1 {{value 'person' was defined but never used; consider replacing with boolean test}}
  // expected-note@-2 {{reference to async let 'person' is 'async'}}
}
