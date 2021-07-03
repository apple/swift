// RUN: %target-typecheck-verify-swift

protocol HasSelfRequirements {
  func foo(_ x: Self)

  func returnsOwnProtocol() -> HasSelfRequirements // expected-error{{protocol 'HasSelfRequirements' can only be used as a generic constraint because it has Self or associated type requirements}} {{educational-notes=associated-type-requirements}}
}
protocol Bar {
  // init() methods should not prevent use as an existential.
  init()

  func bar() -> Bar
}

func useBarAsType(_ x: Bar) {}

protocol Pub : Bar { }

func refinementErasure(_ p: Pub) {
  useBarAsType(p)
}

typealias Compo = HasSelfRequirements & Bar

struct CompoAssocType {
  typealias Compo = HasSelfRequirements & Bar
}

func useAsRequirement<T: HasSelfRequirements>(_ x: T) { }
func useCompoAsRequirement<T: HasSelfRequirements & Bar>(_ x: T) { }
func useCompoAliasAsRequirement<T: Compo>(_ x: T) { }
func useNestedCompoAliasAsRequirement<T: CompoAssocType.Compo>(_ x: T) { }

func useAsWhereRequirement<T>(_ x: T) where T: HasSelfRequirements {}
func useCompoAsWhereRequirement<T>(_ x: T) where T: HasSelfRequirements & Bar {}
func useCompoAliasAsWhereRequirement<T>(_ x: T) where T: Compo {}
func useNestedCompoAliasAsWhereRequirement<T>(_ x: T) where T: CompoAssocType.Compo {}

func useAsType(_ x: HasSelfRequirements) { } // expected-error{{protocol 'HasSelfRequirements' can only be used as a generic constraint}}
func useCompoAsType(_ x: HasSelfRequirements & Bar) { } // expected-error{{protocol 'HasSelfRequirements' can only be used as a generic constraint}}
func useCompoAliasAsType(_ x: Compo) { } // expected-error{{protocol 'HasSelfRequirements' can only be used as a generic constraint}}
func useNestedCompoAliasAsType(_ x: CompoAssocType.Compo) { } // expected-error{{protocol 'HasSelfRequirements' can only be used as a generic constraint}}

struct TypeRequirement<T: HasSelfRequirements> {}
struct CompoTypeRequirement<T: HasSelfRequirements & Bar> {}
struct CompoAliasTypeRequirement<T: Compo> {}
struct NestedCompoAliasTypeRequirement<T: CompoAssocType.Compo> {}

struct CompoTypeWhereRequirement<T> where T: HasSelfRequirements & Bar {}
struct CompoAliasTypeWhereRequirement<T> where T: Compo {}
struct NestedCompoAliasTypeWhereRequirement<T> where T: CompoAssocType.Compo {}

struct Struct1<T> { }
struct Struct2<T : Pub & Bar> { }
struct Struct3<T : Pub & Bar & P3> { } // expected-error {{cannot find type 'P3' in scope}}
struct Struct4<T> where T : Pub & Bar {}

struct Struct5<T : protocol<Pub, Bar>> { } // expected-error {{'protocol<...>' composition syntax has been removed; join the protocols using '&'}}
struct Struct6<T> where T : protocol<Pub, Bar> {} // expected-error {{'protocol<...>' composition syntax has been removed; join the protocols using '&'}}

typealias T1 = Pub & Bar
typealias T2 = protocol<Pub , Bar> // expected-error {{'protocol<...>' composition syntax has been removed; join the protocols using '&'}}

// rdar://problem/20593294
protocol HasAssoc {
  associatedtype Assoc
  func foo()
}

func testHasAssoc(_ x: Any) {
  if let p = x as? HasAssoc { // expected-error {{protocol 'HasAssoc' can only be used as a generic constraint}} {{educational-notes=associated-type-requirements}}
    p.foo() // don't crash here.
  }
}

// rdar://problem/16803384
protocol InheritsAssoc : HasAssoc {
  func silverSpoon()
}

func testInheritsAssoc(_ x: InheritsAssoc) { // expected-error {{protocol 'InheritsAssoc' can only be used as a generic constraint}}
  x.silverSpoon()
}

// SR-38
var b: HasAssoc // expected-error {{protocol 'HasAssoc' can only be used as a generic constraint because it has Self or associated type requirements}}

// Further generic constraint error testing - typealias used inside statements
protocol P {}
typealias MoreHasAssoc = HasAssoc & P
func testHasMoreAssoc(_ x: Any) {
  if let p = x as? MoreHasAssoc { // expected-error {{protocol 'HasAssoc' can only be used as a generic constraint}}
    p.foo() // don't crash here.
  }
}

struct Outer {
  typealias Any = Int // expected-error {{keyword 'Any' cannot be used as an identifier here}} expected-note {{if this name is unavoidable, use backticks to escape it}} {{13-16=`Any`}}
  typealias `Any` = Int
  static func aa(a: `Any`) -> Int { return a }
}

typealias X = Struct1<Pub & Bar>
_ = Struct1<Pub & Bar>.self

typealias BadAlias<T> = T
where T : HasAssoc, T.Assoc == HasAssoc
// expected-error@-1 {{protocol 'HasAssoc' can only be used as a generic constraint because it has Self or associated type requirements}}

struct BadStruct<T>
where T : HasAssoc,
      T.Assoc == HasAssoc {}
// expected-error@-1 {{protocol 'HasAssoc' can only be used as a generic constraint because it has Self or associated type requirements}}

protocol BadProtocol where T == HasAssoc {
  // expected-error@-1 {{protocol 'HasAssoc' can only be used as a generic constraint because it has Self or associated type requirements}}
  associatedtype T

  associatedtype U : HasAssoc
    where U.Assoc == HasAssoc
  // expected-error@-1 {{protocol 'HasAssoc' can only be used as a generic constraint because it has Self or associated type requirements}}
}

extension HasAssoc where Assoc == HasAssoc {}
// expected-error@-1 {{protocol 'HasAssoc' can only be used as a generic constraint because it has Self or associated type requirements}}

func badFunction<T>(_: T)
where T : HasAssoc,
      T.Assoc == HasAssoc {}
// expected-error@-1 {{protocol 'HasAssoc' can only be used as a generic constraint because it has Self or associated type requirements}}

struct BadSubscript {
  subscript<T>(_: T) -> Int
  where T : HasAssoc,
        T.Assoc == HasAssoc {
    // expected-error@-1 {{protocol 'HasAssoc' can only be used as a generic constraint because it has Self or associated type requirements}}
    get {}
    set {}
  }
}

struct OuterGeneric<T> {
  func contextuallyGenericMethod() where T == HasAssoc {}
  // expected-error@-1 {{protocol 'HasAssoc' can only be used as a generic constraint because it has Self or associated type requirements}}

  struct Inner {}
}

func returnsNestedType() -> OuterGeneric<HasAssoc>.Inner {}
// expected-error@-1 {{protocol 'HasAssoc' can only be used as a generic constraint because it has Self or associated type requirements}}

protocol ProtocolWithNestedTypeResult {}

extension ProtocolWithNestedTypeResult {
  func returnsNestedTypeSelf() -> OuterGeneric<Self>.Inner {}
}

func callsNestedTypeSelf(_ p: ProtocolWithNestedTypeResult) {
  _ = p.returnsNestedTypeSelf()
  // expected-error@-1 {{member 'returnsNestedTypeSelf' cannot be used on value of protocol type 'ProtocolWithNestedTypeResult'; use a generic constraint instead}}
}