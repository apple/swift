// RUN: %target-typecheck-verify-swift

// a concrete move-only type
struct MO: ~Copyable {
  var x: Int?
}

struct Container: ~Copyable {
  var mo: MO = MO()
}

// utilities

struct LocalArray<Element> {}

protocol P {}

protocol Box<T> {
  associatedtype T
  func get() -> T
}

class RefBox<T>: Box { // expected-note 2{{'where T: Copyable' is implicit here}}
  var val: T
  init(_ t: T) { val = t }
  func get() -> T { return val }
}

struct ValBox<T>: Box { // expected-note 2{{'where T: Copyable' is implicit here}}
  var val: T
  init(_ t: T) { val = t }
  func get() -> T { return val }
}

class NotStoredGenerically<T> {
  func take(_ t: T) {}
  func give() -> T { fatalError("todo") }
}

enum Maybe<T> { // expected-note {{'where T: Copyable' is implicit here}}
  case none
  case just(T)
}

func takeConcrete(_ m: borrowing MO) {}
func takeGeneric<T>(_ t: T) {} // expected-note 5{{'where T: Copyable' is implicit here}}
func takeGenericSendable<T>(_ t: T) where T: Sendable {}
func takeMaybe<T>(_ m: Maybe<T>) {} // expected-note 2{{'where T: Copyable' is implicit here}}
func takeAnyBoxErased(_ b: any Box) {}
func takeAnyBox<T>(_ b: any Box<T>) {} // expected-note 2{{'where T: Copyable' is implicit here}}
func takeAny(_ a: Any) {}
func takeAnyObject(_ a: AnyObject) {}
func genericVarArg<T>(_ t: T...) {} // expected-note {{'where T: Copyable' is implicit here}}

var globalMO: MO = MO()


// ----------------------
// --- now some tests ---
// ----------------------

// some top-level tests
let _: MO = globalMO
takeGeneric(globalMO) // expected-error {{global function 'takeGeneric' requires that 'MO' conform to 'Copyable'}}




func testAny() {
  let _: Any = MO() // expected-error {{value of type 'MO' does not conform to specified type 'Copyable'}}
  takeAny(MO()) // expected-error {{argument type 'MO' does not conform to expected type 'Copyable'}}
}

func testBasic(_ mo: borrowing MO) {
  takeConcrete(globalMO)
  takeConcrete(MO())

  takeGeneric(globalMO) // expected-error {{global function 'takeGeneric' requires that 'MO' conform to 'Copyable'}}
  takeGeneric(MO()) // expected-error {{global function 'takeGeneric' requires that 'MO' conform to 'Copyable'}}
  takeGeneric(mo) // expected-error {{global function 'takeGeneric' requires that 'MO' conform to 'Copyable'}}

  takeAny(mo) // expected-error {{argument type 'MO' does not conform to expected type 'Copyable'}}
  print(mo) // expected-error {{argument type 'MO' does not conform to expected type 'Copyable'}}

  takeGeneric { () -> Int? in mo.x }
  genericVarArg(5)
  genericVarArg(mo) // expected-error {{global function 'genericVarArg' requires that 'MO' conform to 'Copyable'}}

  takeGeneric( (mo, 5) ) // expected-error {{global function 'takeGeneric' requires that 'MO' conform to 'Copyable'}}
  takeGeneric( ((mo, 5), 19) ) // expected-error {{global function 'takeGeneric' requires that 'MO' conform to 'Copyable'}}
  takeGenericSendable((mo, mo)) // expected-error {{global function 'takeGenericSendable' requires that 'MO' conform to 'Copyable'}}

  let singleton : (MO) = (mo)
  takeGeneric(singleton) // expected-error {{global function 'takeGeneric' requires that 'MO' conform to 'Copyable'}}

  takeAny((mo)) // expected-error {{argument type 'MO' does not conform to expected type 'Copyable'}}
  takeAny((mo, mo)) // expected-error {{global function 'takeAny' requires that 'MO' conform to 'Copyable'}}
}

func checkBasicBoxes() {
  let mo = MO()

  let vb = ValBox(consume mo) // expected-error {{generic struct 'ValBox' requires that 'MO' conform to 'Copyable'}}
  _ = vb.get()
  _ = vb.val

  let rb = RefBox(MO())  // expected-error {{generic class 'RefBox' requires that 'MO' conform to 'Copyable'}}
  _ = rb.get()
  _ = rb.val

  let vb2: ValBox<MO> = .init(MO())  // expected-error {{type 'MO' does not conform to protocol 'Copyable'}}
}

func checkExistential() {
  takeAnyBox( // expected-error {{global function 'takeAnyBox' requires that 'MO' conform to 'Copyable'}}
      RefBox(MO()))

  takeAnyBox( // expected-error {{global function 'takeAnyBox' requires that 'MO' conform to 'Copyable'}}
      ValBox(globalMO))

  takeAnyBoxErased(
      RefBox(MO())) // expected-error {{generic class 'RefBox' requires that 'MO' conform to 'Copyable'}}

  takeAnyBoxErased(
      ValBox(globalMO)) // expected-error {{generic struct 'ValBox' requires that 'MO' conform to 'Copyable'}}
}

func checkMethodCalls() {
  let tg: NotStoredGenerically<MO> = NotStoredGenerically() // expected-error {{type 'MO' does not conform to protocol 'Copyable'}}
  tg.take(MO())
  tg.give()

  let _ = Maybe.just(MO()) // expected-error {{generic enum 'Maybe' requires that 'MO' conform to 'Copyable'}}

  let _: Maybe<MO> = .none // expected-error {{type 'MO' does not conform to protocol 'Copyable'}}
  let _ = Maybe<MO>.just(MO()) // expected-error {{type 'MO' does not conform to protocol 'Copyable'}}
  let _: Maybe<MO> = .just(MO()) // expected-error {{type 'MO' does not conform to protocol 'Copyable'}}

  takeMaybe(.just(MO())) // expected-error {{global function 'takeMaybe' requires that 'MO' conform to 'Copyable'}}
  takeMaybe(true ? .none : .just(MO()))
  // expected-error@-1 {{global function 'takeMaybe' requires that 'MO' conform to 'Copyable'}}
}

func checkCasting(_ b: any Box, _ mo: borrowing MO, _ a: Any) {
  // casting dynamically is allowed, but should always fail since you can't
  // construct such a type.
  let box = b as! ValBox<MO> // expected-error {{type 'MO' does not conform to protocol 'Copyable'}}
  let dup = box

  let _: MO = dup.get()
  let _: MO = dup.val

  let _: Any = MO.self
  let _: AnyObject = MO.self // expected-error {{value of type 'MO.Type' expected to be instance of class or class-constrained type}}
  let _ = MO.self as Any
  let _ = MO.self is Any // expected-warning {{'is' test is always true}}

  // FIXME: well this message is confusing. It's actually that `any Sendable` requires Copyable, not protocol `Sendable`!
  let _: Sendable = (MO(), MO()) // expected-error {{protocol 'Sendable' requires that 'MO' conform to 'Copyable'}}

  let _: Sendable = MO() // expected-error {{value of type 'MO' does not conform to specified type 'Copyable'}}
  let _: Copyable = mo // expected-error {{value of type 'MO' does not conform to specified type 'Copyable'}}
  let _: AnyObject = MO() // expected-error {{value of type 'MO' expected to be instance of class or class-constrained type}}
  let _: Any = mo // expected-error {{value of type 'MO' does not conform to specified type 'Copyable'}}

  _ = MO() as P // expected-error {{cannot convert value of type 'MO' to type 'any P' in coercion}}
  _ = MO() as any P // expected-error {{cannot convert value of type 'MO' to type 'any P' in coercion}}
  _ = MO() as Any // expected-error {{cannot convert value of type 'MO' to type 'Any' in coercion}}
  _ = MO() as MO
  _ = MO() as AnyObject // expected-error {{value of type 'MO' expected to be instance of class or class-constrained type}}
  _ = 5 as MO // expected-error {{cannot convert value of type 'Int' to type 'MO' in coercion}}
  _ = a as MO // expected-error {{cannot convert value of type 'Any' to type 'MO' in coercion}}
  _ = b as MO // expected-error {{cannot convert value of type 'any Box' to type 'MO' in coercion}}

  _ = MO() is AnyHashable // expected-warning {{cast from 'MO' to unrelated type 'AnyHashable' always fails}}
  // expected-error@-1 {{noncopyable types cannot be conditionally cast}}
  _ = MO() is AnyObject // expected-warning {{cast from 'MO' to unrelated type 'AnyObject' always fails}}
  // expected-error@-1 {{noncopyable types cannot be conditionally cast}}
  _ = MO() is Any // expected-warning {{cast from 'MO' to unrelated type 'Any' always fails}}
  // expected-error@-1 {{noncopyable types cannot be conditionally cast}}
  _ = MO() is P // expected-warning {{cast from 'MO' to unrelated type 'any P' always fails}}
  // expected-error@-1 {{noncopyable types cannot be conditionally cast}}
  _ = MO() is MO // expected-warning {{'is' test is always true}}
  // expected-error@-1 {{noncopyable types cannot be conditionally cast}}

  _ = 5 is MO // expected-warning {{cast from 'Int' to unrelated type 'MO' always fails}}
  // expected-error@-1 {{noncopyable types cannot be conditionally cast}}
  _ = a is MO // expected-warning {{cast from 'Any' to unrelated type 'MO' always fails}}
  // expected-error@-1 {{noncopyable types cannot be conditionally cast}}
  _ = b is MO // expected-warning {{cast from 'any Box' to unrelated type 'MO' always fails}}
  // expected-error@-1 {{noncopyable types cannot be conditionally cast}}

  _ = MO() as! AnyHashable // expected-warning {{cast from 'MO' to unrelated type 'AnyHashable' always fails}}
  // expected-error@-1 {{noncopyable types cannot be conditionally cast}}
  _ = MO() as! AnyObject // expected-warning {{cast from 'MO' to unrelated type 'AnyObject' always fails}}
  // expected-error@-1 {{noncopyable types cannot be conditionally cast}}
  _ = MO() as! Any // expected-warning {{cast from 'MO' to unrelated type 'Any' always fails}}
  // expected-error@-1 {{noncopyable types cannot be conditionally cast}}
  _ = MO() as! P // expected-warning {{cast from 'MO' to unrelated type 'any P' always fails}}
  // expected-error@-1 {{noncopyable types cannot be conditionally cast}}
  _ = MO() as! MO // expected-warning {{forced cast of 'MO' to same type has no effect}}
  // expected-error@-1 {{noncopyable types cannot be conditionally cast}}

  _ = 5 as! MO // expected-warning {{cast from 'Int' to unrelated type 'MO' always fails}}
  // expected-error@-1 {{noncopyable types cannot be conditionally cast}}
  _ = a as! MO // expected-warning {{cast from 'Any' to unrelated type 'MO' always fails}}
  // expected-error@-1 {{noncopyable types cannot be conditionally cast}}
  _ = b as! MO // expected-warning {{cast from 'any Box' to unrelated type 'MO' always fails}}
  // expected-error@-1 {{noncopyable types cannot be conditionally cast}}

  _ = MO() as? AnyHashable // expected-warning {{cast from 'MO' to unrelated type 'AnyHashable' always fails}}
  // expected-error@-1 {{noncopyable types cannot be conditionally cast}}
  _ = MO() as? AnyObject // expected-warning {{cast from 'MO' to unrelated type 'AnyObject' always fails}}
  // expected-error@-1 {{noncopyable types cannot be conditionally cast}}
  _ = MO() as? Any // expected-warning {{cast from 'MO' to unrelated type 'Any' always fails}}
  // expected-error@-1 {{noncopyable types cannot be conditionally cast}}
  _ = MO() as? P // expected-warning {{cast from 'MO' to unrelated type 'any P' always fails}}
  // expected-error@-1 {{noncopyable types cannot be conditionally cast}}
  _ = MO() as? MO // expected-warning {{conditional cast from 'MO' to 'MO' always succeeds}}
  // expected-error@-1 {{noncopyable types cannot be conditionally cast}}

  _ = 5 as? MO // expected-warning {{cast from 'Int' to unrelated type 'MO' always fails}}
  // expected-error@-1 {{noncopyable types cannot be conditionally cast}}
  _ = a as? MO // expected-warning {{cast from 'Any' to unrelated type 'MO' always fails}}
  // expected-error@-1 {{noncopyable types cannot be conditionally cast}}
  _ = b as? MO // expected-warning {{cast from 'any Box' to unrelated type 'MO' always fails}}
  // expected-error@-1 {{noncopyable types cannot be conditionally cast}}

}

// FIXME: rdar://115752211 (deal with existing Swift modules that lack Copyable requirements)
// the stdlib right now is not yet being compiled with NoncopyableGenerics
func checkStdlibTypes(_ mo: borrowing MO) {
  _ = "\(mo)" // expected-error {{no exact matches in call to instance method 'appendInterpolation'}}
  let _: String = String(describing: mo) // expected-error {{no exact matches in call to initializer}}

  let _: [MO] = // expected-error {{type 'MO' does not conform to protocol 'Copyable'}}
      [MO(), MO()]
  let _: [MO] = // expected-error {{type 'MO' does not conform to protocol 'Copyable'}}
      []
  let _: [String: MO] = // expected-error {{type 'MO' does not conform to protocol 'Copyable'}}
      ["hello" : MO()]  // expected-error{{type '(String, MO)' containing noncopyable element is not supported}}

  _ = [MO()] // expected-error {{generic struct 'Array' requires that 'MO' conform to 'Copyable'}}

  let _: Array<MO> = .init() // expected-error {{type 'MO' does not conform to protocol 'Copyable'}}
  _ = [MO]() // expected-error {{type 'MO' does not conform to protocol 'Copyable'}}

  let _: String = "hello \(mo)" // expected-error {{no exact matches in call to instance method 'appendInterpolation'}}
}

func copyableExistentials(_ a: Any, _ e1: Error, _ e2: any Error, _ ah: AnyHashable) {
  takeGeneric(a)
  takeGeneric(e1)
  takeGeneric(e2)
  takeGeneric(ah)
}

// ensure that associated types can't be witnessed by move-only types

protocol HasType<Ty> {
  associatedtype Ty // expected-note 3{{protocol requires nested type 'Ty'; add nested type 'Ty' for conformance}}
}

class SomeGuy: HasType { // expected-error {{type 'SomeGuy' does not conform to protocol 'HasType'}}
  typealias Ty = MO // expected-note {{possibly intended match 'SomeGuy.Ty' (aka 'MO') does not conform to 'Copyable'}}
}

struct AnotherGuy: HasType { // expected-error {{type 'AnotherGuy' does not conform to protocol 'HasType'}}
  struct Ty: ~Copyable {} // expected-note {{possibly intended match 'AnotherGuy.Ty' does not conform to 'Copyable'}}
}

protocol Gives: HasType {
  func give() -> Ty
}

struct GenerousGuy: Gives { // expected-error {{type 'GenerousGuy' does not conform to protocol 'HasType'}}
  typealias Ty = MO // expected-note {{possibly intended match 'GenerousGuy.Ty' (aka 'MO') does not conform to 'Copyable'}}
  func give() -> Ty {}
}

func doBadMetatypeStuff<T>(_ t: T) {
  let y = t as! Any.Type
  if let MO_MetaType = y as? MO.Type { // expected-warning {{cast from 'any Any.Type' to unrelated type 'MO.Type' always fails}}
    let x = MO_MetaType.init()
    let _ = x
  }
}
func tryToDoBadMetatypeStuff() {
  doBadMetatypeStuff(MO.self)
}
