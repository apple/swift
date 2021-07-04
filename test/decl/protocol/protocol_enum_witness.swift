// RUN: %target-typecheck-verify-swift

////// WITNESS MATCHING FOR PAYLOAD-LESS ENUMS //////

// Requirement is settable, so witness cannot satisfy it //

protocol Foo1 {
  static var bar: Self { get set } // expected-note {{protocol requires property 'bar' with type 'Bar1'; do you want to add a stub?}}
}

enum Bar1: Foo1 { // expected-error {{type 'Bar1' does not conform to protocol 'Foo1'}}
  case bar // expected-note {{candidate is not settable, but protocol requires it}}
}

// Witness has associated values, which is unsupported //

protocol Foo2 {
  static var bar: Self { get set } // expected-note {{protocol requires property 'bar' with type 'Bar2'; do you want to add a stub?}}
}

enum Bar2: Foo2 { // expected-error {{type 'Bar2' does not conform to protocol 'Foo2'}}
  case bar(Int) // expected-note {{candidate is an enum case with associated values, but protocol does not allow it}}
}

protocol Foo3 {
  static var bar: Self { get } // expected-note {{protocol requires property 'bar' with type 'Bar3'; do you want to add a stub?}}
}

enum Bar3: Foo3 { // expected-error {{type 'Bar3' does not conform to protocol 'Foo3'}}
  case bar(Int) // expected-note {{candidate is an enum case with associated values, but protocol does not allow it}}
}

// Requirement is not static, so it cannot be witnessed by the enum case //

protocol Foo4 {
  var bar: Self { get } // expected-note {{protocol requires property 'bar' with type 'Bar4'; do you want to add a stub?}}
}

enum Bar4: Foo4 { // expected-error {{type 'Bar4' does not conform to protocol 'Foo4'}}
  case bar // expected-note {{candidate operates on a type, not an instance as required}}
}

protocol Foo5 {
  var bar: Self { get set } // expected-note {{protocol requires property 'bar' with type 'Bar5'; do you want to add a stub?}}
}

enum Bar5: Foo5 { // expected-error {{type 'Bar5' does not conform to protocol 'Foo5'}}
  case bar // expected-note {{candidate operates on a type, not an instance as required}}
}

// Requirement does not have Self type, so it cannot be witnessed by the enum case //

protocol Foo6 {
  static var bar: Int { get } // expected-note {{protocol requires property 'bar' with type 'Int'; do you want to add a stub?}}
}

enum Bar6: Foo6 { // expected-error {{type 'Bar6' does not conform to protocol 'Foo6'}}
  case bar // expected-note {{candidate has non-matching type '(Bar6.Type) -> Bar6'}}
}

// Valid cases

protocol Foo7 {
  static var bar: Self { get }
}

enum Bar7: Foo7 {
  case bar // Okay
}

protocol Foo8 {
  static var bar: Bar8 { get }
}

enum Bar8: Foo8 {
  case bar // Okay
}

////// WITNESS MATCHING FOR PAYLOAD ENUMS //////

// Witness does not have argument label, but requirement does

protocol Foo9 {
  static func bar(f: Int) -> Self // expected-note {{requirement 'bar(f:)' declared here}}
}

enum Bar9: Foo9 {
  case bar(_ f: Int) // expected-error {{enum case 'bar' has different argument labels from those required by protocol 'Foo9' ('bar(f:)')}}
}

// Witness does not have any labels, but requirement does

protocol Foo10 {
  static func bar(g: Int) -> Self // expected-note {{requirement 'bar(g:)' declared here}}
}

enum Bar10: Foo10 {
  case bar(Int) // expected-error {{enum case 'bar' has different argument labels from those required by protocol 'Foo10' ('bar(g:)')}}
}

// Witness does not have a payload, but requirement is a function

protocol Foo11 {
  static func bar(h: Int) -> Self // expected-note {{protocol requires function 'bar(h:)' with type '(Int) -> Bar11'; do you want to add a stub?}}
}

enum Bar11: Foo11 { // expected-error {{type 'Bar11' does not conform to protocol 'Foo11'}}
  case bar // expected-note {{candidate has non-matching type '(Bar11.Type) -> Bar11'}}
}

// Witness is static, but requirement is not

protocol Foo12 {
  func bar(i: Int) -> Self // expected-note {{protocol requires function 'bar(i:)' with type '(Int) -> Bar12'; do you want to add a stub?}}
}

enum Bar12: Foo12 { // expected-error {{type 'Bar12' does not conform to protocol 'Foo12'}}
  case bar // expected-note {{candidate operates on a type, not an instance as required}}
}

// Valid cases

protocol Foo13 {
  static func bar(j: Int) -> Self
  static func baz(_ k: String) -> Bar13
}

enum Bar13: Foo13 {
  case bar(j: Int) // Okay
  case baz(_ k: String) // Okay
}

////// REQUIREMENT IS A THROWING FUNCTION //////

protocol ThrowingFactory {
  static func horse(_: Int) throws -> Self
}

enum HorseFactory : ThrowingFactory {
  case horse(Int)
}

protocol MissingAccessorsVar {
  // expected-error@+2 {{property in protocol must have explicit { get } or { get set } specifier}}
  // expected-note@+1 {{protocol requires property 'bar' with type 'Bar14'; do you want to add a stub?}}
  static var bar: Self
}
enum Bar14: MissingAccessorsVar { // expected-error {{type 'Bar14' does not conform to protocol 'MissingAccessorsVar'}}
  case bar // expected-note {{candidate is not settable, but protocol requires it}}
}

protocol MissingAccessorsLet {
  static let bar: Self // expected-error {{protocols cannot require properties to be immutable; declare read-only properties by using 'var' with a '{ get }' specifier}}
}
enum Bar15: MissingAccessorsLet { // OK
  case bar
}
