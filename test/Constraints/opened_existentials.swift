// RUN: %target-typecheck-verify-swift -enable-experimental-opened-existential-types -enable-parameterized-protocol-types -enable-experimental-opaque-parameters

protocol Q { }

protocol P {
  associatedtype A: Q
}

extension Int: P {
  typealias A = Double
}

extension Array: P where Element: P {
  typealias A = String
}

extension Double: Q { }
extension String: Q { }

func acceptGeneric<T: P>(_: T) -> T.A? { nil }
func acceptCollection<C: Collection>(_ c: C) -> C.Element { c.first! }

// --- Simple opening of existential values
func testSimpleExistentialOpening(p: any P, pq: any P & Q, c: any Collection) {
  let pa = acceptGeneric(p)
  let _: Int = pa // expected-error{{cannot convert value of type 'Q?' to specified type 'Int'}}

  let pqa = acceptGeneric(pq)
  let _: Int = pqa  // expected-error{{cannot convert value of type 'Q?' to specified type 'Int'}}

  let element = acceptCollection(c) 
  let _: Int = element // expected-error{{cannot convert value of type 'Any' to specified type 'Int'}}
}

// --- Requirements on nested types
protocol CollectionOfPs: Collection where Self.Element: P { }

func takeCollectionOfPs<C: Collection>(_: C) -> C.Element.A?    
  where C.Element: P
{
  nil
}

func testCollectionOfPs(cp: any CollectionOfPs) {
  let e = takeCollectionOfPs(cp)
  let _: Int = e // expected-error{{cannot convert value of type 'Q?' to specified type 'Int'}}
}

// --- Multiple opened existentials in the same expression
func takeTwoGenerics<T1: P, T2: P>(_ a: T1, _ b: T2) -> (T1, T2) { (a, b) }

extension P {
  func combineThePs<T: P & Q>(_ other: T) -> (A, T.A)? { nil }
}

func testMultipleOpened(a: any P, b: any P & Q) {
  let r1 = takeTwoGenerics(a, b)
  let _: Int = r1  // expected-error{{cannot convert value of type '(P, P & Q)' to specified type 'Int'}}

  let r2 = a.combineThePs(b)
  let _: Int = r2  // expected-error{{cannot convert value of type '(Q, Q)?' to specified type 'Int'}}  
}

// --- Opening existential metatypes
func conjureValue<T: P>(of type: T.Type) -> T? {
  nil
}

func testMagic(pt: any P.Type) {
  let pOpt = conjureValue(of: pt)
  let _: Int = pOpt // expected-error{{cannot convert value of type 'P?' to specified type 'Int'}}
}

// --- With primary associated types and opaque parameter types
protocol CollectionOf: Collection {
  @_primaryAssociatedType associatedtype Element
}

extension Array: CollectionOf { }
extension Set: CollectionOf { }

// expected-note@+2{{required by global function 'reverseIt' where 'some CollectionOf<T>' = 'CollectionOf'}}
@available(SwiftStdlib 5.1, *)
func reverseIt<T>(_ c: some CollectionOf<T>) -> some CollectionOf<T> {
  return c.reversed()
}

@available(SwiftStdlib 5.1, *)
func useReverseIt(_ c: any CollectionOf) {
  // Can't type-erase the `T` from the result.
  _ = reverseIt(c) // expected-error{{protocol 'CollectionOf' as a type cannot conform to the protocol itself}}
  // expected-note@-1{{only concrete types such as structs, enums and classes can conform to protocols}}
}

/// --- Opening existentials when returning opaque types.
@available(SwiftStdlib 5.1, *)
extension P {
  func getQ() -> some Q {
    let a: A? = nil
    return a!
  }

  func getCollectionOf() -> some CollectionOf<A> {
    return [] as [A]
  }
}

@available(SwiftStdlib 5.1, *)
func getPQ<T: P>(_: T) -> some Q {
  let a: T.A? = nil
  return a!
}

// expected-note@+2{{required by global function 'getCollectionOfP' where 'T' = 'P'}}
@available(SwiftStdlib 5.1, *)
func getCollectionOfP<T: P>(_: T) -> some CollectionOf<T.A> {
  return [] as [T.A]
}

func funnyIdentity<T: P>(_ value: T) -> T? {
  value
}

func arrayOfOne<T: P>(_ value: T) -> [T] {
  [value]
}

struct X<T: P> {
  // expected-note@-1{{required by generic struct 'X' where 'T' = 'P'}}
  func f(_: T) { }
}

// expected-note@+1{{required by global function 'createX' where 'T' = 'P'}}
func createX<T: P>(_ value: T) -> X<T> {
  X<T>()
}

func doNotOpenOuter(p: any P) {
  _ = X().f(p) // expected-error{{protocol 'P' as a type cannot conform to the protocol itself}}
  // expected-note@-1{{only concrete types such as structs, enums and classes can conform to protocols}}
}

func takesVariadic<T: P>(_ args: T...) { }
// expected-note@-1 2{{required by global function 'takesVariadic' where 'T' = 'P'}}
// expected-note@-2{{in call to function 'takesVariadic'}}

func callVariadic(p1: any P, p2: any P) {
  takesVariadic() // expected-error{{generic parameter 'T' could not be inferred}}
  takesVariadic(p1) // expected-error{{protocol 'P' as a type cannot conform to the protocol itself}}
  // expected-note@-1{{only concrete types such as structs, enums and classes can conform to protocols}}
  takesVariadic(p1, p2) // expected-error{{protocol 'P' as a type cannot conform to the protocol itself}}
  // expected-note@-1{{only concrete types such as structs, enums and classes can conform to protocols}}
}

func takesInOut<T: P>(_ value: inout T) { }

func passesInOut(i: Int) {
  var p: any P = i
  takesInOut(&p)
}

func takesOptional<T: P>(_ value: T?) { }
// expected-note@-1{{required by global function 'takesOptional' where 'T' = 'P'}}

func passesToOptional(p: any P, pOpt: (any P)?) {
  takesOptional(p) // okay
  takesOptional(pOpt) // expected-error{{protocol 'P' as a type cannot conform to the protocol itself}}
  // expected-note@-1{{only concrete types such as structs, enums and classes can conform to protocols}}
}


@available(SwiftStdlib 5.1, *)
func testReturningOpaqueTypes(p: any P) {
  let q = p.getQ()
  let _: Int = q  // expected-error{{cannot convert value of type 'Q' to specified type 'Int'}}

  p.getCollectionOf() // expected-error{{member 'getCollectionOf' cannot be used on value of protocol type 'P'; use a generic constraint instead}}

  let q2 = getPQ(p)
  let _: Int = q2  // expected-error{{cannot convert value of type 'Q' to specified type 'Int'}}

  getCollectionOfP(p) // expected-error{{protocol 'P' as a type cannot conform to the protocol itself}}
  // expected-note@-1{{only concrete types such as structs, enums and classes can conform to protocols}}

  let fi = funnyIdentity(p)
  let _: Int = fi // expected-error{{cannot convert value of type 'P?' to specified type 'Int'}}

  _ = arrayOfOne(p) // okay, arrays are covariant in their argument

  _ = createX(p) // expected-error{{protocol 'P' as a type cannot conform to the protocol itself}}
  // expected-note@-1{{only concrete types such as structs, enums and classes can conform to protocols}}
}
