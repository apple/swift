// RUN: %target-typecheck-verify-swift -disable-availability-checking

protocol Q { }

protocol P {
  associatedtype A: Q
}

protocol P1<A> {
  associatedtype A
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
  let _: Int = pa // expected-error{{cannot convert value of type '(any Q)?' to specified type 'Int'}}

  var vp = p
  let vpa = acceptGeneric(vp)
  let _: Int = vpa // expected-error{{cannot convert value of type '(any Q)?' to specified type 'Int'}}

  let pqa = acceptGeneric(pq)
  let _: Int = pqa  // expected-error{{cannot convert value of type '(any Q)?' to specified type 'Int'}}

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
  let _: Int = e // expected-error{{cannot convert value of type '(any Q)?' to specified type 'Int'}}
}

// --- Multiple opened existentials in the same expression
func takeTwoGenerics<T1: P, T2: P>(_ a: T1, _ b: T2) -> (T1, T2) { (a, b) }

extension P {
  func combineThePs<T: P & Q>(_ other: T) -> (A, T.A)? { nil }
}

func testMultipleOpened(a: any P, b: any P & Q) {
  let r1 = takeTwoGenerics(a, b)
  let _: Int = r1  // expected-error{{cannot convert value of type '(any P, any P & Q)' to specified type 'Int'}}

  let r2 = a.combineThePs(b)
  let _: Int = r2  // expected-error{{cannot convert value of type '(any Q, any Q)?' to specified type 'Int'}}
}

// --- Opening existential metatypes
func conjureValue<T: P>(of type: T.Type) -> T? {
  nil
}

func testMagic(pt: any P.Type) {
  let pOpt = conjureValue(of: pt)
  let _: Int = pOpt // expected-error{{cannot convert value of type '(any P)?' to specified type 'Int'}}
}

// --- With primary associated types and opaque parameter types
protocol CollectionOf<Element>: Collection { }

extension Array: CollectionOf { }
extension Set: CollectionOf { }

// expected-note@+2{{required by global function 'reverseIt' where 'some CollectionOf<T>' = 'any CollectionOf'}}
@available(SwiftStdlib 5.1, *)
func reverseIt<T>(_ c: some CollectionOf<T>) -> some CollectionOf<T> {
  return c.reversed()
}

@available(SwiftStdlib 5.1, *)
func useReverseIt(_ c: any CollectionOf) {
  // Can't type-erase the `T` from the result.
  _ = reverseIt(c) // expected-error{{type 'any CollectionOf' cannot conform to 'CollectionOf'}}
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

// expected-note@+2{{required by global function 'getCollectionOfP' where 'T' = 'any P'}}
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
  // expected-note@-1{{required by generic struct 'X' where 'T' = 'any P'}}
  func f(_: T) { }
}

// expected-note@+1{{required by global function 'createX' where 'T' = 'any P'}}
func createX<T: P>(_ value: T) -> X<T> {
  X<T>()
}

func doNotOpenOuter(p: any P) {
  _ = X().f(p) // expected-error{{type 'any P' cannot conform to 'P'}}
  // expected-note@-1{{only concrete types such as structs, enums and classes can conform to protocols}}
}

func takesVariadic<T: P>(_ args: T...) { }
// expected-note@-1 2{{required by global function 'takesVariadic' where 'T' = 'any P'}}
// expected-note@-2{{in call to function 'takesVariadic'}}

func callVariadic(p1: any P, p2: any P) {
  takesVariadic() // expected-error{{generic parameter 'T' could not be inferred}}
  takesVariadic(p1) // expected-error{{type 'any P' cannot conform to 'P'}}
  // expected-note@-1{{only concrete types such as structs, enums and classes can conform to protocols}}
  takesVariadic(p1, p2) // expected-error{{type 'any P' cannot conform to 'P'}}
  // expected-note@-1{{only concrete types such as structs, enums and classes can conform to protocols}}
}

func takesInOut<T: P>(_ value: inout T) { }

func passesInOut(i: Int) {
  var p: any P = i
  takesInOut(&p)
}

func takesOptional<T: P>(_ value: T?) { }
// expected-note@-1{{required by global function 'takesOptional' where 'T' = 'any P'}}

func passesToOptional(p: any P, pOpt: (any P)?) {
  takesOptional(p) // okay
  takesOptional(pOpt) // expected-error{{type 'any P' cannot conform to 'P'}}
  // expected-note@-1{{only concrete types such as structs, enums and classes can conform to protocols}}
}


@available(SwiftStdlib 5.1, *)
func testReturningOpaqueTypes(p: any P) {
  let q = p.getQ()
  let _: Int = q  // expected-error{{cannot convert value of type 'any Q' to specified type 'Int'}}

  p.getCollectionOf() // expected-error{{member 'getCollectionOf' cannot be used on value of type 'any P'; consider using a generic constraint instead}}

  let q2 = getPQ(p)
  let _: Int = q2  // expected-error{{cannot convert value of type 'any Q' to specified type 'Int'}}

  getCollectionOfP(p) // expected-error{{type 'any P' cannot conform to 'P'}}
  // expected-note@-1{{only concrete types such as structs, enums and classes can conform to protocols}}

  let fi = funnyIdentity(p)
  let _: Int = fi // expected-error{{cannot convert value of type '(any P)?' to specified type 'Int'}}

  _ = arrayOfOne(p) // okay, arrays are covariant in their argument

  _ = createX(p) // expected-error{{type 'any P' cannot conform to 'P'}}
  // expected-note@-1{{only concrete types such as structs, enums and classes can conform to protocols}}
}

// Type-erasing vs. opening for parameters after the opened one.
func takeValueAndClosure<T: P>(_ value: T, body: (T) -> Void) { }
func takeValueAndClosureBackwards<T: P>(body: (T) -> Void, _ value: T) { }
// expected-note@-1{{required by global function 'takeValueAndClosureBackwards(body:_:)' where 'T' = 'any P'}}

func genericFunctionTakingP<T: P>(_: T) { }
func genericFunctionTakingPQ<T: P & Q>(_: T) { }

func overloadedGenericFunctionTakingP<T: P>(_: T) -> Int { 0 }
func overloadedGenericFunctionTakingP<T: P>(_: T) { }

func testTakeValueAndClosure(p: any P) {
  // Type-erase when not provided with a generic function.
  takeValueAndClosure(p) { x in
    print(x)
    let _: Int = x // expected-error{{cannot convert value of type 'any P' to specified type 'Int'}}
  }

  // Do not erase when referring to a generic function.
  takeValueAndClosure(p, body: genericFunctionTakingP)
  takeValueAndClosure(p, body: overloadedGenericFunctionTakingP)
  takeValueAndClosure(p, body: genericFunctionTakingPQ) // expected-error{{global function 'genericFunctionTakingPQ' requires that 'T' conform to 'Q'}}

  // Do not allow opening if there are any uses of the type parameter before
  // the opened parameter. This maintains left-to-right evaluation order.
  takeValueAndClosureBackwards( // expected-error{{type 'any P' cannot conform to 'P'}}
    // expected-note@-1{{only concrete types such as structs, enums and classes can conform to protocols}}
    body: { x in x as Int }, // expected-error{{'any P' is not convertible to 'Int'}}
    // expected-note@-1{{did you mean to use 'as!' to force downcast?}}
    p)
}

protocol B {
  associatedtype C: P where C.A == Double
  associatedtype D: P
  associatedtype E: P1 where E.A == Double
}

protocol D {
  associatedtype E
}

extension B {
  var testVar: (Int, [C]) { get { fatalError() } }

  func getC() -> C { fatalError() }
}

func testExplicitCoercionRequirement(v: any B, otherV: any B & D) {
  func getC<T: B>(_: T) -> T.C { fatalError() }
  func getE<T: B>(_: T) -> T.E { fatalError() }
  func getTuple<T: B>(_: T) -> (T, T.C) { fatalError() }
  func getNoError<T: B>(_: T) -> T.C.A { fatalError() }
  func getComplex<T: B>(_: T) -> ([(x: (a: T.C, b: Int), y: Int)], [Int: T.C]) { fatalError() }

  func overloaded<T: B>(_: T) -> (x: Int, y: T.C) { fatalError() }
  func overloaded<T: P>(_: T) -> Int { 42 }
  // expected-note@-1 {{candidate requires that 'any B' conform to 'P' (requirement specified as 'T' : 'P')}}

  _ = getC(v) // expected-error {{inferred result type 'any P' requires explicit coercion due to loss of generic requirements}} {{14-14=as any P}}
  _ = getC(v) as any P // Ok
  
  _ = getE(v) // expected-error {{inferred result type 'any P1<Double>' requires explicit coercion due to loss of generic requirements}} {{14-14=as any P1<Double>}}
  _ = getE(v) as any P1<Double> // Ok
  
  _ = getTuple(v) // expected-error {{inferred result type '(any B, any P)' requires explicit coercion due to loss of generic requirements}} {{18-18=as (any B, any P)}}
  _ = getTuple(v) as (any B, any P) // Ok
  // Ok because T.C.A == Double
  _ = getNoError(v)

  _ = getComplex(v) // expected-error {{inferred result type '([(x: (a: any P, b: Int), y: Int)], [Int : any P])' requires explicit coercion due to loss of generic requirements}} {{20-20=as ([(x: (a: any P, b: Int), y: Int)], [Int : any P])}}
  _ = getComplex(v) as ([(x: (a: any P, b: Int), y: Int)], [Int : any P]) // Ok

  _ = overloaded(v) // expected-error {{no exact matches in call to local function 'overloaded'}}
  // expected-note@-1 {{inferred result type '(x: Int, y: any P)' requires explicit coercion due to loss of generic requirements}} {{20-20=as (x: Int, y: any P)}}

  func acceptsAny<T>(_: T) {}

  acceptsAny(getC(v)) // expected-error {{inferred result type 'any P' requires explicit coercion due to loss of generic requirements}} {{21-21=as any P}}
  acceptsAny(getC(v) as any P) // Ok

  acceptsAny(getComplex(v)) // expected-error {{inferred result type '([(x: (a: any P, b: Int), y: Int)], [Int : any P])' requires explicit coercion due to loss of generic requirements}} {{27-27=as ([(x: (a: any P, b: Int), y: Int)], [Int : any P])}}
  acceptsAny(getComplex(v) as ([(x: (a: any P, b: Int), y: Int)], [Int : any P]))

  func getAssocNoRequirements<T: B>(_: T) -> (Int, [T.D]) { fatalError() }

  _ = getAssocNoRequirements(v) // Ok, `D` doesn't have any requirements

  // Test existential opening from protocol extension access
  _ = v.getC() // expected-error {{inferred result type 'any P' requires explicit coercion due to loss of generic requirements}} {{13-13=as any P}}
  _ = v.getC() as any P // Ok

  _ = v.testVar // expected-error {{inferred result type '(Int, [any P])' requires explicit coercion due to loss of generic requirements}} {{16-16=as (Int, [any P])}}
  _ = v.testVar as (Int, [any P])

  func getE<T: D>(_: T) -> T.E { fatalError() }

  _ = getE(otherV) // Ok `E` doesn't have a `where` clause

  func getSelf<T: B>(_: T) -> T { fatalError() } // expected-note {{found this candidate}}
  func getSelf<T: D>(_: T) -> T { fatalError() } // expected-note {{found this candidate}}

  _ = getSelf(v) // expected-error {{inferred result type 'any B' requires explicit coercion due to loss of generic requirements}} {{17-17=as any B}}
  _ = getSelf(v) as any B // Ok
  _ = getSelf(otherV) as any B & D // expected-error {{ambiguous use of 'getSelf'}}

  func getBDSelf<T: D>(_: T) -> T { fatalError() }
  _ = getBDSelf(otherV) // expected-error {{inferred result type 'any B & D' requires explicit coercion due to loss of generic requirements}} {{24-24=as any B & D}}
  _ = getBDSelf(otherV) as any B & D // Ok

  func getP<T: P>(_: T) {}
  getP(getC(v)) // expected-error {{inferred result type 'any P' requires explicit coercion due to loss of generic requirements}} {{8-8=(}} {{15-15=as any P)}}
  getP(v.getC()) // expected-error {{inferred result type 'any P' requires explicit coercion due to loss of generic requirements}}  {{8-8=(}} {{14-14=as any P)}}

  getP((getC(v) as any P))   // Ok - parens avoid opening suppression
  getP((v.getC() as any P))  // Ok - parens avoid opening suppression
}

class C1 {}
class C2<T>: C1 {}

// Test Associated Types
protocol P2 {
  associatedtype A
  associatedtype B: C2<A>

  func returnAssocTypeB() -> B
}

func testAssocReturn(p: any P2) {
  let _ = p.returnAssocTypeB()  // returns C1
}

protocol Q2 : P2 where A == Int {}

do {
  let q: any Q2
  let _ = q.returnAssocTypeB() // returns C1
}

// Test Primary Associated Types
protocol P3<A> {
  associatedtype A
  associatedtype B: C2<A>

  func returnAssocTypeB() -> B
}

func testAssocReturn(p: any P3<Int>) {
  let _ = p.returnAssocTypeB() // returns C2<A>
}

func testAssocReturn(p: any P3<any P3<String>>) {
  let _ = p.returnAssocTypeB()
}

protocol P4<A> {
  associatedtype A
  associatedtype B: C2<A>

  func returnPrimaryAssocTypeA() -> A
  func returnAssocTypeCollection() -> any Collection<A>
}

 //Confirm there is no way to access Primary Associated Type directly
func testPrimaryAssocReturn(p: any P4<Int>) {
  let _ = p.returnPrimaryAssocTypeA()
}

func testPrimaryAssocCollection(p: any P4<Float>) {
  let _: any Collection<Float> = p.returnAssocTypeCollection()
}

protocol P5<X> {
  associatedtype X = Void
}

struct K<T>: P5 {
  typealias X = T
}

extension P5 {
  @available(macOS 10.15, iOS 13.0, tvOS 13.0, watchOS 6.0, *)
  func foo() -> some P5<X>{
    K<X>()
  }
  func bar(_ handler: @escaping (X) -> Void) -> some P5<X> {
    K<X>()
  }
}

@available(macOS 10.15, iOS 13.0, tvOS 13.0, watchOS 6.0, *)
func testFoo(_ p: any P5<String>) -> any P5 {
  p.foo()
}

func testFooGeneric<U>(_ p: any P5<Result<U, Error>>) -> any P5 {
  p.foo()
}

@available(macOS 10.15, iOS 13.0, tvOS 13.0, watchOS 6.0, *)
func testBar<U>(_ p: any P5<Result<U, Error>>) -> any P5 {
  p.bar { _ in }
}

enum Node<T> {
  case e(any P5)
  case f(any P5<Result<T, Error>>)
}

struct S<T, U> {
  @available(macOS 10.15, iOS 13.0, tvOS 13.0, watchOS 6.0, *)
  func foo(_ elt: Node<U>) -> Node<T>? {
    switch elt {
    case let .e(p):
      return .e(p)
    case let .f(p):
      return .e(p.bar { _ in })
    }
  }
}
