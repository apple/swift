// RUN: %target-typecheck-verify-swift

// a concrete move-only type
@_moveOnly struct MO {
  var x: Int?
}

@_moveOnly struct Container {
  var mo: MO = MO()
}

// utilities

struct LocalArray<Element> {}

protocol P {}

protocol Box<T> {
  associatedtype T
  func get() -> T
}

class RefBox<T>: Box { // expected-note@:14 6{{generic parameter 'T' has an implicit Copyable requirement}}
  var val: T
  init(_ t: T) { val = t }
  func get() -> T { return val }
}

struct ValBox<T>: Box { // expected-note@:15 6{{generic parameter 'T' has an implicit Copyable requirement}}
  var val: T
  init(_ t: T) { val = t }
  func get() -> T { return val }
}

class NotStoredGenerically<T> {
  func take(_ t: T) {}
  func give() -> T { fatalError("todo") }
}

enum Maybe<T> { // expected-note@:12 3{{generic parameter 'T' has an implicit Copyable requirement}}
  case none
  case just(T)
}

func takeConcrete(_ m: borrowing MO) {}
func takeGeneric<T>(_ t: T) {} // expected-note@:18 5{{generic parameter 'T' has an implicit Copyable requirement}}
func takeGenericSendable<T>(_ t: T) where T: Sendable {}
func takeMaybe<T>(_ m: Maybe<T>) {} // expected-note@:16 2{{generic parameter 'T' has an implicit Copyable requirement}}
func takeAnyBoxErased(_ b: any Box) {}
func takeAnyBox<T>(_ b: any Box<T>) {} // expected-note@:17 2{{generic parameter 'T' has an implicit Copyable requirement}}
func takeAny(_ a: Any) {}
func takeAnyObject(_ a: AnyObject) {}
func genericVarArg<T>(_ t: T...) {} // expected-note@:20 {{generic parameter 'T' has an implicit Copyable requirement}}

var globalMO: MO = MO()


// ----------------------
// --- now some tests ---
// ----------------------

// some top-level tests
let _: MO = globalMO
takeGeneric(globalMO) // expected-error {{noncopyable type 'MO' cannot be substituted for copyable generic parameter 'T' in 'takeGeneric'}}




func testAny() {
  let _: Any = MO() // expected-error {{noncopyable type 'MO' cannot be erased to copyable existential type 'Any'}}
  takeAny(MO()) // expected-error {{noncopyable type 'MO' cannot be erased to copyable existential type 'Any'}}
}

func testBasic(_ mo: borrowing MO) {
  takeConcrete(globalMO)
  takeConcrete(MO())

  takeGeneric(globalMO) // expected-error {{noncopyable type 'MO' cannot be substituted for copyable generic parameter 'T' in 'takeGeneric'}}
  takeGeneric(MO()) // expected-error {{noncopyable type 'MO' cannot be substituted for copyable generic parameter 'T' in 'takeGeneric'}}
  takeGeneric(mo) // expected-error {{noncopyable type 'MO' cannot be substituted for copyable generic parameter 'T' in 'takeGeneric'}}

  takeAny(mo) // expected-error {{noncopyable type 'MO' cannot be erased to copyable existential type 'Any'}}
  print(mo) // expected-error {{noncopyable type 'MO' cannot be erased to copyable existential type 'Any'}}
  _ = "\(mo)" // expected-error {{noncopyable type 'MO' cannot be substituted for copyable generic parameter 'T' in 'appendInterpolation'}}
  let _: String = String(describing: mo) // expected-error {{noncopyable type 'MO' cannot be substituted for copyable generic parameter 'Subject' in 'init(describing:)'}}

  takeGeneric { () -> Int? in mo.x }
  genericVarArg(5)
  genericVarArg(mo) // expected-error {{noncopyable type 'MO' cannot be substituted for copyable generic parameter 'T' in 'genericVarArg'}}

  takeGeneric( (mo, 5) ) // expected-error {{tuple with noncopyable element type 'MO' is not supported}}
  takeGeneric( ((mo, 5), 19) ) // expected-error {{tuple with noncopyable element type 'MO' is not supported}}
  takeGenericSendable((mo, mo)) // expected-error 2{{tuple with noncopyable element type 'MO' is not supported}}

  let singleton : (MO) = (mo)
  takeGeneric(singleton) // expected-error {{noncopyable type 'MO' cannot be substituted for copyable generic parameter 'T' in 'takeGeneric'}}

  takeAny((mo)) // expected-error {{noncopyable type 'MO' cannot be erased to copyable existential type 'Any'}}
  takeAny((mo, mo)) // expected-error {{noncopyable type '(MO, MO)' cannot be erased to copyable existential type 'Any'}}
}

func checkBasicBoxes() {
  let mo = MO()

  let vb = ValBox(consume mo) // expected-error 2{{noncopyable type 'MO' cannot be substituted for copyable generic parameter 'T' in 'ValBox'}}
  _ = vb.get()
  _ = vb.val

  let rb = RefBox(MO())  // expected-error 2{{noncopyable type 'MO' cannot be substituted for copyable generic parameter 'T' in 'RefBox'}}
  _ = rb.get()
  _ = rb.val

  let vb2: ValBox<MO> = .init(MO())  // expected-error {{noncopyable type 'MO' cannot be used with generic type 'ValBox<T>' yet}}
}

func checkExistential() {
  takeAnyBox( // expected-error {{noncopyable type 'MO' cannot be substituted for copyable generic parameter 'T' in 'takeAnyBox'}}
      RefBox(MO())) // expected-error 2{{noncopyable type 'MO' cannot be substituted for copyable generic parameter 'T' in 'RefBox'}}

  takeAnyBox( // expected-error {{noncopyable type 'MO' cannot be substituted for copyable generic parameter 'T' in 'takeAnyBox'}}
      ValBox(globalMO)) // expected-error 2{{noncopyable type 'MO' cannot be substituted for copyable generic parameter 'T' in 'ValBox'}}

  takeAnyBoxErased(
      RefBox(MO())) // expected-error 2{{noncopyable type 'MO' cannot be substituted for copyable generic parameter 'T' in 'RefBox'}}

  takeAnyBoxErased(
      ValBox(globalMO)) // expected-error 2{{noncopyable type 'MO' cannot be substituted for copyable generic parameter 'T' in 'ValBox'}}
}

func checkMethodCalls() {
  let tg: NotStoredGenerically<MO> = NotStoredGenerically() // expected-error {{noncopyable type 'MO' cannot be used with generic type 'NotStoredGenerically<T>' yet}}
  tg.take(MO())
  tg.give()

  let _: Maybe<MO> = .none // expected-error {{noncopyable type 'MO' cannot be used with generic type 'Maybe<T>' yet}}
  let _ = Maybe<MO>.just(MO()) // expected-error {{noncopyable type 'MO' cannot be used with generic type 'Maybe<T>' yet}}
  let _: Maybe<MO> = .just(MO()) // expected-error {{noncopyable type 'MO' cannot be used with generic type 'Maybe<T>' yet}}

  // FIXME: MO isn't logically being substituted into takeMaybe. seems like nonsense duplication?
  takeMaybe(.just(MO())) // expected-error {{noncopyable type 'MO' cannot be substituted for copyable generic parameter 'T' in 'Maybe'}}
                         // expected-error@-1 {{noncopyable type 'MO' cannot be substituted for copyable generic parameter 'T' in 'takeMaybe'}}
  takeMaybe(true ? .none : .just(MO()))
  // expected-error@-1 {{noncopyable type 'MO' cannot be substituted for copyable generic parameter 'T' in 'takeMaybe'}}
  // expected-error@-2 2{{noncopyable type 'MO' cannot be substituted for copyable generic parameter 'T' in 'Maybe'}}
}

func checkCasting(_ b: any Box, _ mo: borrowing MO, _ a: Any) {
  // casting dynamically is allowed, but should always fail since you can't
  // construct such a type.
  let box = b as! ValBox<MO> // expected-error {{noncopyable type 'MO' cannot be used with generic type 'ValBox<T>' yet}}
  let dup = box

  let _: MO = dup.get()
  let _: MO = dup.val

  let _: Any = MO.self // expected-error {{metatype 'MO.Type' cannot be cast to 'Any' because 'MO' is noncopyable}}
  let _: AnyObject = MO.self // expected-error {{metatype 'MO.Type' cannot be cast to 'AnyObject' because 'MO' is noncopyable}}
  let _ = MO.self as Any // expected-error {{metatype 'MO.Type' cannot be cast to 'Any' because 'MO' is noncopyable}}
  let _ = MO.self is Any // expected-warning {{cast from 'MO.Type' to unrelated type 'Any' always fails}}

  let _: Sendable = (MO(), MO()) // expected-error {{noncopyable type '(MO, MO)' cannot be erased to copyable existential type 'any Sendable'}}
  let _: Sendable = MO() // expected-error {{noncopyable type 'MO' cannot be erased to copyable existential type 'any Sendable'}}
  let _: _Copyable = mo // expected-error {{'_Copyable' is unavailable}}
                        // expected-error@-1 {{noncopyable type 'MO' cannot be erased to copyable existential type 'any _Copyable'}}
  let _: AnyObject = MO() // expected-error {{noncopyable type 'MO' cannot be erased to copyable existential type 'AnyObject'}}
  let _: Any = mo // expected-error {{noncopyable type 'MO' cannot be erased to copyable existential type 'Any'}}

  _ = MO() as P // expected-error {{noncopyable type 'MO' cannot be erased to copyable existential type 'any P'}}
  _ = MO() as any P // expected-error {{noncopyable type 'MO' cannot be erased to copyable existential type 'any P'}}
  _ = MO() as Any // expected-error {{noncopyable type 'MO' cannot be erased to copyable existential type 'Any'}}
  _ = MO() as MO
  _ = MO() as AnyObject // expected-error {{noncopyable type 'MO' cannot be erased to copyable existential type 'AnyObject'}}
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

func checkStdlibTypes(_ mo: borrowing MO) {
  let _: [MO] = // expected-error {{noncopyable type 'MO' cannot be used with generic type 'Array<Element>' yet}}
      [MO(), MO()]
  let _: [MO] = // expected-error {{noncopyable type 'MO' cannot be used with generic type 'Array<Element>' yet}}
      []
  let _: [String: MO] = // expected-error {{noncopyable type 'MO' cannot be used with generic type 'Dictionary<Key, Value>' yet}}
      ["hello" : MO()]  // expected-error{{type '(String, MO)' containing noncopyable element is not supported}}

  _ = [MO()] // expected-error {{noncopyable type 'MO' cannot be substituted for copyable generic parameter 'Element' in 'Array'}}

  let _: Array<MO> = .init() // expected-error {{noncopyable type 'MO' cannot be used with generic type 'Array<Element>' yet}}
  _ = [MO]() // expected-error {{noncopyable type 'MO' cannot be used with generic type 'Array<Element>' yet}}

  let s: String = "hello \(mo)" // expected-error {{noncopyable type 'MO' cannot be substituted for copyable generic parameter 'T' in 'appendInterpolation'}}
}

func copyableExistentials(_ a: Any, _ e1: Error, _ e2: any Error, _ ah: AnyHashable) {
  takeGeneric(a)
  takeGeneric(e1)
  takeGeneric(e2)
  takeGeneric(ah)
}

// ensure that associated types can't be witnessed by move-only types

protocol HasType<Ty> {
  associatedtype Ty // expected-note 3{{protocol requires nested type 'Ty'; do you want to add it?}}
}

class SomeGuy: HasType { // expected-error {{type 'SomeGuy' does not conform to protocol 'HasType'}}
  typealias Ty = MO // expected-note {{possibly intended match 'SomeGuy.Ty' (aka 'MO') does not conform to '_Copyable'}}
}

struct AnotherGuy: HasType { // expected-error {{type 'AnotherGuy' does not conform to protocol 'HasType'}}
  @_moveOnly struct Ty {} // expected-note {{possibly intended match 'AnotherGuy.Ty' does not conform to '_Copyable'}}
}

protocol Gives: HasType {
  func give() -> Ty
}

struct GenerousGuy: Gives { // expected-error {{type 'GenerousGuy' does not conform to protocol 'HasType'}}
  typealias Ty = MO // expected-note {{possibly intended match 'GenerousGuy.Ty' (aka 'MO') does not conform to '_Copyable'}}
  func give() -> Ty {}
}

func doBadMetatypeStuff<T>(_ t: T) { // expected-note@:25 {{generic parameter 'T' has an implicit Copyable requirement}}
  let y = t as! Any.Type
  if let MO_MetaType = y as? MO.Type { // expected-warning {{cast from 'any Any.Type' to unrelated type 'MO.Type' always fails}}
    let x = MO_MetaType.init()
    let _ = x
  }
}
func tryToDoBadMetatypeStuff() {
  doBadMetatypeStuff(MO.self) // expected-error {{metatype 'MO.Type' of noncopyable type 'MO' cannot be substituted for copyable generic parameter 'T' in 'doBadMetatypeStuff'}}
}

func packingHeat<each T>(_ t: repeat each T) {}
func packIt() {
  packingHeat(MO())  // expected-error {{parameter pack containing noncopyable element 'MO' is not supported}}
}
