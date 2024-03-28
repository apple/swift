// RUN: %target-typecheck-verify-swift -module-name test

struct X {
  typealias MyInt = Int
  func getInt() -> MyInt { return 7 }
}

extension X {
  typealias MyReal = Double
  func getFloat() -> MyReal { return 3.14 }
}

protocol MyIteratorProtocol {}
protocol MySequence {
  associatedtype Iterator : MyIteratorProtocol
  func makeIterator() -> Iterator
}

struct IteratorWrapper<I : MyIteratorProtocol> {
  var index: Int
  var elements: I
}

struct SequenceWrapper<T : MySequence> {
  var input : T

  typealias Iterator = IteratorWrapper<T.Iterator>
  func makeIterator() -> Iterator {
    return Iterator(index: 0, elements: input.makeIterator())
  }
}

// Nested types with non-identifier qualifiers

protocol P1 {} // expected-note {{'P1' declared here}}
protocol P2 {}

extension Optional {
  typealias Wrapped = Wrapped
}

do {
  struct S {
    struct Bar {}
    struct Gen<T> {}
  }

  // Used to check for type equality.
  enum CheckTypes<T, U> {
    enum Match where T == U {}
  }

  // Valid examples.

  let _: CheckTypes<S.Bar, (S).Bar>.Match
  let _: CheckTypes<S.Gen<S>, (S).Gen<S>>.Match

  let _: CheckTypes<S, S?.Wrapped>.Match
  let _: CheckTypes<S, (S)?.Wrapped>.Match
  let _: CheckTypes<S, (S?).Wrapped>.Match
  let _: CheckTypes<S?, S??.Wrapped>.Match
  let _: CheckTypes<S, S?.Wrapped?.Wrapped>.Match
  let _: CheckTypes<S.Gen<S>, S?.Wrapped.Gen<S>>.Match
  let _: CheckTypes<S.Gen<S>, (S?.Wrapped).Gen<S>>.Match
  let _: CheckTypes<S, [S].Element>.Match
  let _: CheckTypes<Dictionary<Int, S>.Element, [Int : S].Element>.Match

  // Invalid examples.

  let _: Int!.Wrapped // expected-warning {{using '!' is not allowed here; treating this as '?' instead}}
  let _: (Int!).Wrapped // expected-warning {{using '!' is not allowed here; treating this as '?' instead}}

  let _: Any.Undef // expected-error {{'Undef' is not a member type of type 'Any'}}
  let _: Int.Type.Undef // expected-error {{'Undef' is not a member type of type 'Swift.Int.Type'}}
  let _: P1.Protocol.Undef // expected-error {{'Undef' is not a member type of type '(any test.P1).Type'}}
  let _: (Int).Undef // expected-error {{'Undef' is not a member type of struct 'Swift.Int'}}
  let _: (Int.Undef1).Undef2 // expected-error {{'Undef1' is not a member type of struct 'Swift.Int'}}
  let _: Int?.Undef // expected-error {{'Undef' is not a member type of generic enum 'Swift.Int?'}}
  let _: [Int].Undef // expected-error {{'Undef' is not a member type of generic struct '[Swift.Int]}}
  let _: [Int : Int].Undef // expected-error {{'Undef' is not a member type of generic struct '[Swift.Int : Swift.Int]'}}
  let _: (any P1).Undef // expected-error {{'Undef' is not a member type of protocol 'any test.P1'}}
  let _: (any P1 & P2).Undef // expected-error {{'Undef' is not a member type of type 'any test.P1 & test.P2'}}
  let _: ().Undef // expected-error {{'Undef' is not a member type of type '()'}}
  let _: (Int, Int).Undef // expected-error {{'Undef' is not a member type of type '(Swift.Int, Swift.Int)'}}
  let _: ((Int) -> Void).Undef // expected-error {{'Undef' is not a member type of type '(Swift.Int) -> Swift.Void'}}
}

// Accept member type expressions rooted on 'Self' or non-identifier qualifiers
// until Swift 6.
do {
  struct Test {
    enum E {}

    func blackHole<T>(_: T.Type) {}

    func test() {
      blackHole(Self.E)
      // expected-warning@-1 {{expected member name or initializer call after type name; this will be an error in Swift 6}}
      // expected-note@-2 {{use '.self' to reference the type object}}
      blackHole((Test).E)
      // expected-warning@-1 {{expected member name or initializer call after type name; this will be an error in Swift 6}}
      // expected-note@-2 {{use '.self' to reference the type object}}
      blackHole([Test].Element)
      // expected-warning@-1 {{expected member name or initializer call after type name; this will be an error in Swift 6}}
      // expected-note@-2 {{use '.self' to reference the type object}}
      // expected-note@-3 {{add arguments after the type to construct a value of the type}}
      blackHole([Int : Test].Element)
      // expected-warning@-1 {{expected member name or initializer call after type name; this will be an error in Swift 6}}
      // expected-note@-2 {{use '.self' to reference the type object}}
      blackHole(Test?.Wrapped)
      // expected-warning@-1 {{expected member name or initializer call after type name; this will be an error in Swift 6}}
      // expected-note@-2 {{use '.self' to reference the type object}}
      // expected-note@-3 {{add arguments after the type to construct a value of the type}}
    }
  }
}
