// RUN: %target-typecheck-verify-swift

// This test focuses on the prevention of users from _writing_ types where
// a move-only type is substituted for a generic parameter.
//
// This means this is hitting on the TypeRepr -> Type phase in TypeCheckType.

// ----------------------
// ---   utilities    ---
// ----------------------

class Klass {}

let asdf: any Hashable & Klass

// a concrete move-only type
@_moveOnly struct MO {
  var x: Int?
}

@_moveOnly struct GenericMO<T> {
  var t: T
}

protocol Box<T> {
  associatedtype T
  func get() -> T
}

struct ValBox<T>: Box {
  var val: T
  init(_ t: T) { val = t }
  func get() -> T { return val }
}

enum Maybe<T> {
  case none
  case just(T)
}

struct CerebralValley<T> {
  struct GenericBro<U> {}
  struct TechBro {}
}

// ----------------------
// --- now some tests ---
// ----------------------

func basic_vararg(_ va: MO...) {} // expected-error {{noncopyable type 'MO' cannot be used with generics yet}}

func illegalTypes<T>(_ t: T) {
  let _: Array<MO> // expected-error {{noncopyable type 'MO' cannot be used with generics yet}}
  let _: Maybe<MO> // expected-error {{noncopyable type 'MO' cannot be used with generics yet}}
  let _: Dictionary<MO, String> // expected-error {{noncopyable type 'MO' cannot be used with generics yet}}
  let _: [MO] // expected-error {{noncopyable type 'MO' cannot be used with generics yet}}
  let _: [String : MO] // expected-error {{noncopyable type 'MO' cannot be used with generics yet}}
  let _: [MO : MO] // expected-error 2{{noncopyable type 'MO' cannot be used with generics yet}}
  let _: [MO : T] // expected-error {{noncopyable type 'MO' cannot be used with generics yet}}

  _ = t as! ValBox<MO> // expected-error {{noncopyable type 'MO' cannot be used with generics yet}}

  let _: Optional<MO> // expected-error {{noncopyable type 'MO' cannot be used with generics yet}}
  let _: MO? // expected-error {{noncopyable type 'MO' cannot be used with generics yet}}
  let _: MO?? // expected-error {{noncopyable type 'MO' cannot be used with generics yet}}
  let _: MO! // expected-error {{noncopyable type 'MO' cannot be used with generics yet}}
  let _: MO?! // expected-error {{noncopyable type 'MO' cannot be used with generics yet}}

  let _: Klass & MO // expected-error {{non-protocol, non-class type 'MO' cannot be used within a protocol-constrained type}}
  let _: any MO // expected-error {{'any' has no effect on concrete type 'MO'}}
  let _: any GenericMO<T> // expected-error {{'any' has no effect on concrete type 'GenericMO<T>'}}

  let _: CerebralValley<MO>.TechBro // expected-error {{noncopyable type 'MO' cannot be used with generics yet}}
  let _: CerebralValley<Int>.GenericBro<MO> // expected-error {{noncopyable type 'MO' cannot be used with generics yet}}

  let _: GenericMO<MO> // expected-error {{noncopyable type 'MO' cannot be used with generics yet}}
}

func illegalInExpr() {

}
