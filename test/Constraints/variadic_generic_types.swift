// RUN: %target-typecheck-verify-swift -disable-availability-checking

// Parsing an UnresolvedSpecializeExpr containing a PackExpansionType
struct G<each T> {}

func f<each T>(_: repeat each T) {
  _ = G< >.self
  _ = G<Int>.self
  _ = G<Int, String>.self
  _ = G<repeat each T>.self
  _ = G<Int, repeat Array<each T>>.self
}

// Forming PackExpansionTypeReprs in simplifyTypeExpr()
func g<each T>(_: repeat each T) {
  _ = (repeat each T).self
  _ = (Int, repeat each T).self
  _ = ((repeat each T) -> ()).self
  _ = ((Int, repeat Array<each T>) -> ()).self

  _ = (repeat each Int).self
  // expected-error@-1 {{pack expansion 'Int' must contain at least one pack reference}}
  // expected-error@-2 {{'each' cannot be applied to non-pack type 'Int'}}{{15-19=}}
}

struct MissingMemberError<each T> {
  init() {
    self.doesNotExist = 1
    // expected-error@-1 {{value of type 'MissingMemberError<repeat each T>' has no member 'doesNotExist'}}
  }
}
