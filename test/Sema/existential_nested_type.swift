// RUN: %target-swift-frontend -typecheck -verify -swift-version 4 %s

// rdar://29605388 -- Swift 3 admitted opening an existential type with
// associated types in one case.

// See test/IRGen/existential_nested_type.swift for the Swift 3 test.

protocol HasAssoc {
  associatedtype A
}

enum MyError : Error {
  case bad(Any)
}

func checkIt(_ js: Any) throws {
  switch js {
    case let dbl as HasAssoc: // expected-error {{protocol 'HasAssoc' can only be used as a conformance constraint because it has Self or associated type requirements}}
      throw MyError.bad(dbl)

    default:
      fatalError("wrong")
  }
}
