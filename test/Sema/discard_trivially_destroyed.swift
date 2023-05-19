// RUN: %target-swift-emit-silgen -verify %s

// Yes, you read that right. This is being checked in SILGen! So we have to
// separate these tests from ones emitting errors during Sema.

class ClassyGal {}

@propertyWrapper
struct Appending<Val> {
  let terminator: Val
  var store: [Val] = []

  var wrappedValue: [Val] {
    get {
      return store + [terminator]
    }
    set {
      fatalError("mutation is the root of all bugs")
    }
  }
}

struct StructuredGuy: ~Copyable {
  let x = ClassyGal()
  // expected-note@-1 {{type 'ClassyGal' cannot be trivially destroyed}}

  @Appending(terminator: ClassyGal())
  var bestC: [ClassyGal]

  consuming func doDiscard() { discard self }
  // expected-error@-1 {{can only 'discard' type 'StructuredGuy' if it contains trivially-destroyed stored properties at this time}}
  deinit {}
}

struct AppendyEnby: ~Copyable {
  @Appending(terminator: 0)
  var string: [Int]
  // expected-note@-1 {{type 'Appending<Int>' cannot be trivially destroyed}}


  consuming func doDiscard() { discard self }
  // expected-error@-1 {{can only 'discard' type 'AppendyEnby' if it contains trivially-destroyed stored properties at this time}}
  deinit {}
}

struct HasGeneric: ~Copyable {
  var thing: Any // expected-note {{type 'Any' cannot be trivially destroyed}}

  consuming func discard() { discard self }
  // expected-error@-1 {{can only 'discard' type 'HasGeneric' if it contains trivially-destroyed stored properties at this time}}

  deinit{}
}

struct WrappingNoncopyable: ~Copyable {
  var computed: String { "mine" }

  var x: AppendyEnby // expected-note{{type 'AppendyEnby' cannot be trivially destroyed}}
  consuming func doDiscard() { discard self }
  // expected-error@-1 {{can only 'discard' type 'WrappingNoncopyable' if it contains trivially-destroyed stored properties at this time}}
  deinit {}
}

struct LazyGuy: ~Copyable {
  lazy var thing: String = "asdf"
  // expected-note@-1 {{type 'String?' cannot be trivially destroyed}}

  consuming func doDiscard() { discard self }
  // expected-error@-1 {{can only 'discard' type 'LazyGuy' if it contains trivially-destroyed stored properties at this time}}
  deinit {}
}

struct BoringNoncopyable: ~Copyable {
  let x = 0
  let y = 0
}

struct Boring {
  var x = 0
  var y = 1.9
}

// FIXME: Despite not having a deinit, the noncopyable struct isn't considered trivial?
struct ContainsBoring: ~Copyable {
  let z: BoringNoncopyable // expected-note {{type 'BoringNoncopyable' cannot be trivially destroyed}}
  consuming func discard() { discard self }
  // expected-error@-1 {{can only 'discard' type 'ContainsBoring' if it contains trivially-destroyed stored properties at this time}}
  deinit {}
}

struct AllOK: ~Copyable {
  var maybeDigits: Int?
  var maybeFloat: Float?
  let dbl: Double
  var location: Boring = Boring()
  var unsafePtr: UnsafePointer<Int>

  consuming func doDiscard() { discard self }
  deinit {}
}

struct HasGenericStored<T>: ~Copyable {
  let t: T // expected-note {{type 'T' cannot be trivially destroyed}}
  consuming func discard() { discard self } // expected-error {{can only 'discard' type 'HasGenericStored<T>' if it contains trivially-destroyed stored properties at this time}}
  deinit{}
}

struct HasAny: ~Copyable {
  var t: Any // expected-note {{type 'Any' cannot be trivially destroyed}}
  consuming func discard() { discard self } // expected-error {{can only 'discard' type 'HasAny' if it contains trivially-destroyed stored properties at this time}}
  deinit{}
}
