// RUN: %target-typecheck-verify-swift

protocol P1 {
  typealias DependentInConcreteConformance = Self
}

class Base<T> : P1 { // expected-note {{arguments to generic parameter 'T' ('String' and 'Int') are expected to be equal}}
  typealias DependentClass = T

  required init(classInit: ()) {}

  func classSelfReturn() -> Self {}
}

protocol P2 {
  typealias FullyConcrete = Int
  typealias DependentProtocol = Self

  init(protocolInit: ())

  func protocolSelfReturn() -> Self
}

typealias BaseAndP2<T> = Base<T> & P2
typealias BaseIntAndP2 = BaseAndP2<Int>

class Derived : Base<Int>, P2 {
  required init(protocolInit: ()) {
    super.init(classInit: ())
  }

  required init(classInit: ()) {
    super.init(classInit: ())
  }

  func protocolSelfReturn() -> Self {}
}

class Other : Base<Int> {}

typealias OtherAndP2 = Other & P2

protocol P3 : class {}

protocol P4 {}

struct Unrelated {}

//
// If a class conforms to a protocol concretely, the resulting protocol
// composition type should be equivalent to the class type for redeclaration
// checking purposes.
//

func alreadyConforms<T>(_: Base<T>) {} // expected-note 3 {{'alreadyConforms' previously declared here}}
func alreadyConforms<T>(_: Base<T> & P1) {} // expected-error {{invalid redeclaration of 'alreadyConforms'}}
func alreadyConforms<T>(_: Base<T> & AnyObject) {} // expected-error {{invalid redeclaration of 'alreadyConforms'}}
func alreadyConforms<T>(_: Base<T> & P1 & AnyObject) {} // expected-error {{invalid redeclaration of 'alreadyConforms'}}

func alreadyConformsMeta<T>(_: Base<T>.Type) {} // expected-note 7 {{'alreadyConformsMeta' previously declared here}}
func alreadyConformsMeta<T>(_: (Base<T> & P1).Type) {} // expected-error {{invalid redeclaration of 'alreadyConformsMeta'}}
func alreadyConformsMeta<T>(_: (Base<T> & P1).Protocol) {} // expected-error {{invalid redeclaration of 'alreadyConformsMeta'}}
func alreadyConformsMeta<T>(_: (any Base<T> & P1).Type) {} // expected-error {{invalid redeclaration of 'alreadyConformsMeta'}}
func alreadyConformsMeta<T>(_: (Base<T> & AnyObject).Type) {} // expected-error {{invalid redeclaration of 'alreadyConformsMeta'}}
func alreadyConformsMeta<T>(_: (Base<T> & P1 & AnyObject).Type) {} // expected-error {{invalid redeclaration of 'alreadyConformsMeta'}}
func alreadyConformsMeta<T>(_: (Base<T> & P1 & AnyObject).Protocol) {} // expected-error {{invalid redeclaration of 'alreadyConformsMeta'}}
func alreadyConformsMeta<T>(_: (any Base<T> & P1 & AnyObject).Type) {} // expected-error {{invalid redeclaration of 'alreadyConformsMeta'}}

func alreadyConforms(_: P3) {} // expected-note {{'alreadyConforms' previously declared here}}
func alreadyConforms(_: P3 & AnyObject) {} // expected-error {{invalid redeclaration of 'alreadyConforms'}}

func alreadyConformsMeta(_: P3.Type) {} // expected-note {{'alreadyConformsMeta' previously declared here}}
func alreadyConformsMeta(_: (P3 & AnyObject).Type) {} // expected-error {{invalid redeclaration of 'alreadyConformsMeta'}}

func notARedeclaration(_: P4) {}
func notARedeclaration(_: P4 & AnyObject) {}

do {
  class C: P4 {}
  struct S<T: P4> {
    // Don't crash when computing minimal compositions inside a generic context.
    func redeclaration(_: C & P4) {} // expected-note {{'redeclaration' previously declared here}}
    func redeclaration(_: C & P4) {} // expected-error {{invalid redeclaration of 'redeclaration'}}
  }
}

// SE-0156 stipulates that a composition can contain multiple classes, as long
// as they are all the same.
func basicDiagnostics(
  _: Base<Int> & Base<Int>,
  _: Base<Int> & Derived, // expected-error{{protocol-constrained type cannot contain class 'Derived' because it already contains class 'Base<Int>'}}

  // Invalid typealias case
  _: Derived & OtherAndP2, // expected-error{{protocol-constrained type cannot contain class 'Other' because it already contains class 'Derived'}}

  // Valid typealias case
  _: OtherAndP2 & P3) {}

// A composition containing only a single class is actually identical to
// the class type itself.
struct Box<T : Base<Int>> {}

func takesBox(_: Box<Base<Int>>) {}

func passesBox(_ b: Box<Base<Int> & Base<Int>>) {
  takesBox(b)
}

// Test that subtyping behaves as you would expect.
func basicSubtyping(
  base: Base<Int>,
  baseAndP1: Base<Int> & P1,
  baseAndP2: Base<Int> & P2,
  baseAndP2AndAnyObject: Base<Int> & P2 & AnyObject,
  baseAndAnyObject: Base<Int> & AnyObject,
  derived: Derived,
  derivedAndP2: Derived & P2,
  derivedAndP3: Derived & P3,
  derivedAndAnyObject: Derived & AnyObject,
  p1AndAnyObject: P1 & AnyObject,
  p2AndAnyObject: P2 & AnyObject,
  anyObject: AnyObject) {

  // Errors
  let _: Base & P2 = base // expected-error {{value of type 'Base<Int>' does not conform to specified type 'P2'}}
  let _: Base<Int> & P2 = base // expected-error {{value of type 'Base<Int>' does not conform to specified type 'P2'}}
  let _: P3 = baseAndP1 // expected-error {{value of type 'any Base<Int> & P1' does not conform to specified type 'P3'}}
  let _: P3 = baseAndP2 // expected-error {{value of type 'any Base<Int> & P2' does not conform to specified type 'P3'}}
  let _: Derived = baseAndP1 // expected-error {{cannot convert value of type 'any Base<Int> & P1' to specified type 'Derived'}}
  let _: Derived = baseAndP2 // expected-error {{cannot convert value of type 'any Base<Int> & P2' to specified type 'Derived'}}
  let _: Derived & P2 = baseAndP2 // expected-error {{cannot convert value of type 'any Base<Int> & P2' to specified type 'Derived'}}

  let _ = Unrelated() as Derived & P2 // expected-error {{cannot convert value of type 'Unrelated' to type 'any Derived & P2' in coercion}}
  let _ = Unrelated() as? Derived & P2 // expected-warning {{always fails}}
  let _ = baseAndP2 as Unrelated // expected-error {{cannot convert value of type 'any Base<Int> & P2' to type 'Unrelated' in coercion}}
  let _ = baseAndP2 as? Unrelated // expected-warning {{always fails}}

  // Different behavior on Linux vs Darwin because of id-as-Any.
  // let _ = Unrelated() as AnyObject
  // let _ = Unrelated() as? AnyObject

  let _ = anyObject as Unrelated // expected-error {{'AnyObject' is not convertible to 'Unrelated'}}
  //expected-note@-1 {{did you mean to use 'as!' to force downcast?}} {{21-23=as!}}
  let _ = anyObject as? Unrelated

  // No-ops
  let _: Base & P1 = base
  let _: Base<Int> & P1 = base
  let _: Base & AnyObject = base
  let _: Base<Int> & AnyObject = base
  let _: Derived & AnyObject = derived

  let _ = base as Base<Int> & P1
  let _ = base as Base<Int> & AnyObject
  let _ = derived as Derived & AnyObject

  let _ = base as? Base<Int> & P1 // expected-warning {{always succeeds}}
  let _ = base as? Base<Int> & AnyObject // expected-warning {{always succeeds}}
  let _ = derived as? Derived & AnyObject // expected-warning {{always succeeds}}

  // Erasing superclass constraint
  let _: P1 = baseAndP1
  let _: P1 & AnyObject = baseAndP1
  let _: P1 = derived
  let _: P1 & AnyObject = derived
  let _: AnyObject = baseAndP1
  let _: AnyObject = baseAndP2
  let _: AnyObject = derived
  let _: AnyObject = derivedAndP2
  let _: AnyObject = derivedAndP3
  let _: AnyObject = derivedAndAnyObject

  let _ = baseAndP1 as P1
  let _ = baseAndP1 as P1 & AnyObject
  let _ = derived as P1
  let _ = derived as P1 & AnyObject
  let _ = baseAndP1 as AnyObject
  let _ = derivedAndAnyObject as AnyObject

  let _ = baseAndP1 as? P1 // expected-warning {{always succeeds}}
  let _ = baseAndP1 as? P1 & AnyObject // expected-warning {{always succeeds}}
  let _ = derived as? P1 // expected-warning {{always succeeds}}
  let _ = derived as? P1 & AnyObject // expected-warning {{always succeeds}}
  let _ = baseAndP1 as? AnyObject // expected-warning {{always succeeds}}
  let _ = derivedAndAnyObject as? AnyObject // expected-warning {{always succeeds}}

  // Erasing conformance constraint
  let _: Base = baseAndP1
  let _: Base<Int> = baseAndP1
  let _: Base = derivedAndP3
  let _: Base<Int> = derivedAndP3
  let _: Derived = derivedAndP2
  let _: Derived = derivedAndAnyObject

  let _ = baseAndP1 as Base<Int>
  let _ = derivedAndP3 as Base<Int>
  let _ = derivedAndP2 as Derived
  let _ = derivedAndAnyObject as Derived

  let _ = baseAndP1 as? Base<Int> // expected-warning {{always succeeds}}
  let _ = derivedAndP3 as? Base<Int> // expected-warning {{always succeeds}}
  let _ = derivedAndP2 as? Derived // expected-warning {{always succeeds}}
  let _ = derivedAndAnyObject as? Derived // expected-warning {{always succeeds}}

  // Upcasts
  let _: Base & P2 = derived
  let _: Base<Int> & P2 = derived
  let _: Base & P2 & AnyObject = derived
  let _: Base<Int> & P2 & AnyObject = derived
  let _: Base & P3 = derivedAndP3
  let _: Base<Int> & P3 = derivedAndP3

  let _ = derived as Base<Int> & P2
  let _ = derived as Base<Int> & P2 & AnyObject
  let _ = derivedAndP3 as Base<Int> & P3

  let _ = derived as? Base<Int> & P2 // expected-warning {{always succeeds}}
  let _ = derived as? Base<Int> & P2 & AnyObject // expected-warning {{always succeeds}}
  let _ = derivedAndP3 as? Base<Int> & P3 // expected-warning {{always succeeds}}

  // Calling methods with Self return
  let _: Base & P2 = baseAndP2.classSelfReturn()
  let _: Base<Int> & P2 = baseAndP2.classSelfReturn()
  let _: Base & P2 = baseAndP2.protocolSelfReturn()
  let _: Base<Int> & P2 = baseAndP2.protocolSelfReturn()

  // Downcasts
  let _ = baseAndP2 as Derived //expected-error {{'any Base<Int> & P2' is not convertible to 'Derived'}}
  // expected-note@-1 {{did you mean to use 'as!' to force downcast?}} {{21-23=as!}}
  let _ = baseAndP2 as? Derived
  
  let _ = baseAndP2 as Derived & P3 // expected-error {{'any Base<Int> & P2' is not convertible to 'any Derived & P3'}}
  // expected-note@-1 {{did you mean to use 'as!' to force downcast?}} {{21-23=as!}}
  let _ = baseAndP2 as? Derived & P3

  let _ = base as Derived & P2 //expected-error {{'Base<Int>' is not convertible to 'any Derived & P2'}}
  // expected-note@-1 {{did you mean to use 'as!' to force downcast?}}
  let _ = base as? Derived & P2

  // Invalid cases
  let _ = derived as Other & P2 // expected-error {{cannot convert value of type 'Derived' to type 'any Other & P2' in coercion}}
  let _ = derived as? Other & P2 // expected-warning {{always fails}}

  let _ = derivedAndP3 as Other // expected-error {{cannot convert value of type 'any Derived & P3' to type 'Other' in coercion}}
  let _ = derivedAndP3 as? Other // expected-warning {{always fails}}

  let _ = derivedAndP3 as Other & P3 // expected-error {{cannot convert value of type 'any Derived & P3' to type 'any Other & P3' in coercion}}
  let _ = derivedAndP3 as? Other & P3 // expected-warning {{always fails}}

  let _ = derived as Other // expected-error {{cannot convert value of type 'Derived' to type 'Other' in coercion}}
  let _ = derived as? Other // expected-warning {{always fails}}
}

// Test conversions in return statements
func eraseProtocolInReturn(baseAndP2: Base<Int> & P2) -> Base<Int> {
  return baseAndP2
}

func eraseProtocolInReturn(baseAndP2: (Base<Int> & P2)!) -> Base<Int> {
  return baseAndP2
}

func eraseProtocolInReturn(baseAndP2: Base<Int> & P2) -> Base<Int>? {
  return baseAndP2
}

func eraseClassInReturn(baseAndP2: Base<Int> & P2) -> P2 {
  return baseAndP2
}

func eraseClassInReturn(baseAndP2: (Base<Int> & P2)!) -> P2 {
  return baseAndP2
}

func eraseClassInReturn(baseAndP2: Base<Int> & P2) -> P2? {
  return baseAndP2
}

func upcastToExistentialInReturn(derived: Derived) -> Base<Int> & P2 {
  return derived
}

func upcastToExistentialInReturn(derived: Derived!) -> Base<Int> & P2 {
  return derived
}

func upcastToExistentialInReturn(derived: Derived) -> (Base<Int> & P2)? {
  return derived
}

func takesBase<T>(_: Base<T>) {}
func takesP2(_: P2) {}
func takesBaseMetatype<T>(_: Base<T>.Type) {}
func takesP2Metatype(_: P2.Type) {}

func takesBaseIntAndP2(_ x: Base<Int> & P2) {
  takesBase(x)
  takesP2(x)
}

func takesBaseIntAndP2Metatype(_ x: (Base<Int> & P2).Type) {
  takesBaseMetatype(x)
  takesP2Metatype(x)
}

func takesDerived(x: Derived) {
  takesBaseIntAndP2(x)
}

func takesDerivedMetatype(x: Derived.Type) {
  takesBaseIntAndP2Metatype(x)
}

//
// Looking up member types of subclass existentials.
//

func dependentMemberTypes<T : BaseIntAndP2>(
  _: T.DependentInConcreteConformance,
  _: T.DependentProtocol,
  _: T.DependentClass,
  _: T.FullyConcrete,

  _: BaseIntAndP2.DependentInConcreteConformance, // FIXME expected-error {{}}
  _: BaseIntAndP2.DependentProtocol, // expected-error {{cannot access type alias 'DependentProtocol' from 'BaseIntAndP2' (aka 'Base<Int> & P2'); use a concrete type or generic parameter base instead}}
  _: BaseIntAndP2.DependentClass,
  _: BaseIntAndP2.FullyConcrete) {}

func conformsToAnyObject<T : AnyObject>(_: T) {}
// expected-note@-1 {{where 'T' = 'any P1'}}
func conformsToP1<T : P1>(_: T) {}
func conformsToP2<T : P2>(_: T) {}
func conformsToBaseIntAndP2<T : Base<Int> & P2>(_: T) {}
// expected-note@-1 {{where 'T' = 'FakeDerived'}}
// expected-note@-2 {{where 'T' = 'T1'}}
// expected-note@-3 2 {{where 'T' = 'Base<Int>'}}

func conformsToBaseIntAndP2WithWhereClause<T>(_: T) where T : Base<Int> & P2 {}
// expected-note@-1 {{where 'T' = 'FakeDerived'}}
// expected-note@-2 {{where 'T' = 'T1'}}

class FakeDerived : Base<String>, P2 {
  required init(classInit: ()) {
    super.init(classInit: ())
  }

  required init(protocolInit: ()) {
    super.init(classInit: ())
  }

  func protocolSelfReturn() -> Self { return self }
}

//
// Metatype subtyping.
//

func metatypeSubtyping(
  base: Base<Int>.Type,
  derived: Derived.Type,
  derivedAndAnyObject: (Derived & AnyObject).Type,
  baseIntAndP2: (Base<Int> & P2).Type,
  baseIntAndP2AndAnyObject: (Base<Int> & P2 & AnyObject).Type) {

  // Erasing conformance constraint
  let _: Base<Int>.Type = baseIntAndP2
  let _: Base<Int>.Type = baseIntAndP2AndAnyObject
  let _: Derived.Type = derivedAndAnyObject
  let _: BaseAndP2<Int>.Type = baseIntAndP2AndAnyObject

  let _ = baseIntAndP2 as Base<Int>.Type
  let _ = baseIntAndP2AndAnyObject as Base<Int>.Type
  let _ = derivedAndAnyObject as Derived.Type
  let _ = baseIntAndP2AndAnyObject as BaseAndP2<Int>.Type

  let _ = baseIntAndP2 as? Base<Int>.Type // expected-warning {{always succeeds}}
  let _ = baseIntAndP2AndAnyObject as? Base<Int>.Type // expected-warning {{always succeeds}}
  let _ = derivedAndAnyObject as? Derived.Type // expected-warning {{always succeeds}}
  let _ = baseIntAndP2AndAnyObject as? BaseAndP2<Int>.Type // expected-warning {{always succeeds}}

  // Upcast
  let _: BaseAndP2<Int>.Type = derived
  let _: BaseAndP2<Int>.Type = derivedAndAnyObject

  let _ = derived as BaseAndP2<Int>.Type
  let _ = derivedAndAnyObject as BaseAndP2<Int>.Type

  let _ = derived as? BaseAndP2<Int>.Type // expected-warning {{always succeeds}}
  let _ = derivedAndAnyObject as? BaseAndP2<Int>.Type // expected-warning {{always succeeds}}

  // Erasing superclass constraint
  let _: P2.Type = baseIntAndP2
  let _: P2.Type = derived
  let _: P2.Type = derivedAndAnyObject
  let _: (P2 & AnyObject).Type = derived
  let _: (P2 & AnyObject).Type = derivedAndAnyObject

  let _ = baseIntAndP2 as P2.Type
  let _ = derived as P2.Type
  let _ = derivedAndAnyObject as P2.Type
  let _ = derived as (P2 & AnyObject).Type
  let _ = derivedAndAnyObject as (P2 & AnyObject).Type

  let _ = baseIntAndP2 as? P2.Type // expected-warning {{always succeeds}}
  let _ = derived as? P2.Type // expected-warning {{always succeeds}}
  let _ = derivedAndAnyObject as? P2.Type // expected-warning {{always succeeds}}
  let _ = derived as? (P2 & AnyObject).Type // expected-warning {{always succeeds}}
  let _ = derivedAndAnyObject as? (P2 & AnyObject).Type // expected-warning {{always succeeds}}

  // Initializers
  let _: Base<Int> & P2 = baseIntAndP2.init(classInit: ())
  let _: Base<Int> & P2 = baseIntAndP2.init(protocolInit: ())
  let _: Base<Int> & P2 & AnyObject = baseIntAndP2AndAnyObject.init(classInit: ())
  let _: Base<Int> & P2 & AnyObject = baseIntAndP2AndAnyObject.init(protocolInit: ())
  let _: Derived = derived.init(classInit: ())
  let _: Derived = derived.init(protocolInit: ())
  let _: Derived & AnyObject = derivedAndAnyObject.init(classInit: ())
  let _: Derived & AnyObject = derivedAndAnyObject.init(protocolInit: ())
}

//
// Conformance relation.
//

func conformsTo<T1 : P2, T2 : Base<Int> & P2>(
  anyObject: AnyObject,
  p1: P1,
  p2: P2,
  p3: P3,
  base: Base<Int>,
  badBase: Base<String>,
  derived: Derived,
  fakeDerived: FakeDerived,
  p2Archetype: T1,
  baseAndP2Archetype: T2) {

  // FIXME: Uninformative diagnostics

  // Errors
  conformsToAnyObject(p1)
  // expected-error@-1 {{global function 'conformsToAnyObject' requires that 'any P1' be a class type}}

  conformsToP1(p1)

  // FIXME: Following diagnostics are not great because when
  // `conformsTo*` methods are re-typechecked, they loose information
  // about `& P2` in generic parameter.

  conformsToBaseIntAndP2(base)
  // expected-error@-1 {{global function 'conformsToBaseIntAndP2' requires that 'Base<Int>' conform to 'P2'}}

  conformsToBaseIntAndP2(badBase)
  // expected-error@-1 {{global function 'conformsToBaseIntAndP2' requires that 'Base<Int>' conform to 'P2'}}
  // expected-error@-2 {{cannot convert value of type 'Base<String>' to expected argument type 'Base<Int>'}}

  conformsToBaseIntAndP2(fakeDerived)
  // expected-error@-1 {{global function 'conformsToBaseIntAndP2' requires that 'FakeDerived' inherit from 'Base<Int>'}}

  conformsToBaseIntAndP2WithWhereClause(fakeDerived)
  // expected-error@-1 {{global function 'conformsToBaseIntAndP2WithWhereClause' requires that 'FakeDerived' inherit from 'Base<Int>'}}

  conformsToBaseIntAndP2(p2Archetype)
  // expected-error@-1 {{global function 'conformsToBaseIntAndP2' requires that 'T1' inherit from 'Base<Int>'}}

  conformsToBaseIntAndP2WithWhereClause(p2Archetype)
  // expected-error@-1 {{global function 'conformsToBaseIntAndP2WithWhereClause' requires that 'T1' inherit from 'Base<Int>'}}

  // Good
  conformsToAnyObject(anyObject)
  conformsToAnyObject(baseAndP2Archetype)
  conformsToP1(derived)
  conformsToP1(baseAndP2Archetype)
  conformsToP2(derived)
  conformsToP2(baseAndP2Archetype)
  conformsToBaseIntAndP2(derived)
  conformsToBaseIntAndP2(baseAndP2Archetype)
  conformsToBaseIntAndP2WithWhereClause(derived)
  conformsToBaseIntAndP2WithWhereClause(baseAndP2Archetype)
}

// Subclass existentials inside inheritance clauses
class CompositionInClassInheritanceClauseAlias : BaseIntAndP2 {
  required init(classInit: ()) {
    super.init(classInit: ())
  }

  required init(protocolInit: ()) {
    super.init(classInit: ())
  }

  func protocolSelfReturn() -> Self { return self }
  func asBase() -> Base<Int> { return self }
}

class CompositionInClassInheritanceClauseDirect : Base<Int> & P2 {
  required init(classInit: ()) {
    super.init(classInit: ())
  }

  required init(protocolInit: ()) {
    super.init(classInit: ())
  }

  func protocolSelfReturn() -> Self { return self }
  func asBase() -> Base<Int> { return self }
}

protocol CompositionInAssociatedTypeInheritanceClause {
  associatedtype A : BaseIntAndP2
}

// Members of metatypes and existential metatypes

protocol ProtocolWithStaticMember {
  static func staticProtocolMember()
  func instanceProtocolMember()
}

class ClassWithStaticMember {
  static func staticClassMember() {}
  func instanceClassMember() {}
}

func staticMembers(
    m1: (ProtocolWithStaticMember & ClassWithStaticMember).Protocol,
    m2: (ProtocolWithStaticMember & ClassWithStaticMember).Type) {
  _ = m1.staticProtocolMember() // expected-error {{static member 'staticProtocolMember' cannot be used on protocol metatype '(any ClassWithStaticMember & ProtocolWithStaticMember).Type'}}
  _ = m1.staticProtocolMember // expected-error {{static member 'staticProtocolMember' cannot be used on protocol metatype '(any ClassWithStaticMember & ProtocolWithStaticMember).Type'}}

  _ = m1.staticClassMember() // expected-error {{static member 'staticClassMember' cannot be used on protocol metatype '(any ClassWithStaticMember & ProtocolWithStaticMember).Type'}}
  _ = m1.staticClassMember // expected-error {{static member 'staticClassMember' cannot be used on protocol metatype '(any ClassWithStaticMember & ProtocolWithStaticMember).Type'}}

  _ = m1.instanceProtocolMember
  _ = m1.instanceClassMember

  _ = m2.staticProtocolMember()
  _ = m2.staticProtocolMember

  _ = m2.staticClassMember()
  _ = m2.staticClassMember

  _ = m2.instanceProtocolMember // expected-error {{instance member 'instanceProtocolMember' cannot be used on type 'ClassWithStaticMember & ProtocolWithStaticMember'}}
  _ = m2.instanceClassMember // expected-error {{instance member 'instanceClassMember' cannot be used on type 'ClassWithStaticMember & ProtocolWithStaticMember'}}
}

// Make sure we correctly form subclass existentials in expression context.
func takesBaseIntAndPArray(_: [Base<Int> & P2]) {}

func passesBaseIntAndPArray() {
  takesBaseIntAndPArray([Base<Int> & P2]())
}

//
// Superclass constrained generic parameters
//

struct DerivedBox<T : Derived> {}
// expected-note@-1 {{requirement specified as 'T' : 'Derived' [with T = any Derived & P3]}}

func takesBoxWithP3(_: DerivedBox<Derived & P3>) {}
// expected-error@-1 {{'DerivedBox' requires that 'any Derived & P3' inherit from 'Derived'}}

// A bit of a tricky setup -- the real problem is that matchTypes() did the
// wrong thing when solving a Bind constraint where both sides were protocol
// compositions, but one of them had a superclass constraint containing type
// variables. We were checking type equality in this case, which is not
// correct; we have to do a 'deep equality' check, recursively matching the
// superclass types.
struct Generic<T> {
  var _x: (Base<T> & P2)!

  var x: (Base<T> & P2)? {
    get { return _x }
    set { _x = newValue }
    _modify {
      yield &_x
    }
  }
}
