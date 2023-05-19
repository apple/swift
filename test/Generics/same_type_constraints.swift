// RUN: %target-typecheck-verify-swift -swift-version 4 -warn-redundant-requirements

protocol Fooable {
  associatedtype Foo

  var foo: Foo { get }
}

protocol Barrable {
  associatedtype Bar: Fooable
  var bar: Bar { get }
}

struct X {}
struct Y: Fooable {
  typealias Foo = X
  var foo: X { return X() }
}
struct Z: Barrable {
  typealias Bar = Y
  var bar: Y { return Y() }
}

protocol TestSameTypeRequirement {
  func foo<F1: Fooable>(_ f: F1) where F1.Foo == X
}
struct SatisfySameTypeRequirement : TestSameTypeRequirement {
  func foo<F2: Fooable>(_ f: F2) where F2.Foo == X {}
}

protocol TestSameTypeAssocTypeRequirement {
  associatedtype Assoc
  func foo<F1: Fooable>(_ f: F1) where F1.Foo == Assoc
}
struct SatisfySameTypeAssocTypeRequirement : TestSameTypeAssocTypeRequirement {
  typealias Assoc = X
  func foo<F2: Fooable>(_ f: F2) where F2.Foo == X {}
}
struct SatisfySameTypeAssocTypeRequirementDependent<T>
  : TestSameTypeAssocTypeRequirement
{
  typealias Assoc = T
  func foo<F3: Fooable>(_ f: F3) where F3.Foo == T {}
}

// Pulled in from old standard library to keep the following test
// (LazySequenceOf) valid.
public struct GeneratorOf<T> : IteratorProtocol, Sequence {

  /// Construct an instance whose `next()` method calls `nextElement`.
  public init(_ nextElement: @escaping () -> T?) {
    self._next = nextElement
  }
  
  /// Construct an instance whose `next()` method pulls its results
  /// from `base`.
  public init<I : IteratorProtocol>(_ base: I) where I.Element == T {
    var base = base
    self._next = { base.next() }
  }
  
  /// Advance to the next element and return it, or `nil` if no next
  /// element exists.
  ///
  /// Precondition: `next()` has not been applied to a copy of `self`
  /// since the copy was made, and no preceding call to `self.next()`
  /// has returned `nil`.
  public mutating func next() -> T? {
    return _next()
  }

  /// `GeneratorOf<T>` is also a `Sequence`, so it `generate`\ s
  /// a copy of itself
  public func makeIterator() -> GeneratorOf {
    return self
  }
  let _next: () -> T?
}

// rdar://problem/19009056
public struct LazySequenceOf<S : Sequence, A> : Sequence where S.Iterator.Element == A {
  public func makeIterator() -> GeneratorOf<A> { 
    return GeneratorOf<A>({ return nil })
  }
  public subscript(i : A) -> A { return i }
}

public func iterate<A>(_ f : @escaping (A) -> A) -> (_ x : A) -> LazySequenceOf<Iterate<A>, A>? {
  return { x in nil }
}

public final class Iterate<A> : Sequence {
  typealias IteratorProtocol = IterateGenerator<A>
  public func makeIterator() -> IterateGenerator<A> {
    return IterateGenerator<A>()
  }
}

public final class IterateGenerator<A> : IteratorProtocol {
  public func next() -> A? {
    return nil
  }
}

// rdar://problem/18475138
public protocol Observable : class {
    associatedtype Output
    func addObserver(_ obj : @escaping (Output) -> Void)
}

public protocol Bindable : class {
    associatedtype Input
    func foo()
}

class SideEffect<In> : Bindable {
  typealias Input = In
  func foo() { }
}

struct Composed<Left: Bindable, Right: Observable> where Left.Input == Right.Output {}

infix operator <- : AssignmentPrecedence

func <- <
    Right
    >(lhs: @escaping (Right.Output) -> Void, rhs: Right) -> Composed<SideEffect<Right>, Right>?
{
  return nil
}

// rdar://problem/17855378
struct Pair<T, U> {
    typealias Type_ = (T, U)
}

protocol Seq {
  associatedtype Element

  func zip<OtherSeq: Seq, ResultSeq: Seq> (_ otherSeq: OtherSeq) -> ResultSeq
    where ResultSeq.Element == Pair<Element, OtherSeq.Element>.Type_
}

// rdar://problem/18435371
extension Dictionary {
    func multiSubscript<S : Sequence>(_ seq: S) -> [Value?] where S.Iterator.Element == Key {
        var result = [Value?]()
        for seqElt in seq {
            result.append(self[seqElt])
        }
        return result
    }
}

// rdar://problem/19245317
protocol P {
	associatedtype T: P
}

struct S<A: P> {
	init<Q: P>(_ q: Q) where Q.T == A {} // expected-warning {{redundant conformance constraint 'A' : 'P'}}
}

// rdar://problem/19371678
protocol Food { }
class Grass : Food { }

protocol Animal {
    associatedtype EdibleFood:Food
    func eat(_ f:EdibleFood)
}
class Cow : Animal {
    func eat(_ f: Grass) { }
}

struct SpecificAnimal<F:Food> : Animal {
    typealias EdibleFood=F
    let _eat:(_ f:F) -> ()

    init<A:Animal>(_ selfie:A) where A.EdibleFood == F { // expected-warning {{redundant conformance constraint 'F' : 'Food'}}
        _eat = { selfie.eat($0) }
    }
    func eat(_ f:F) {
        _eat(f)
    }
}

// rdar://problem/18803556
struct Something<T> {
    var items: [T] = []
}

extension Something {
    init<S : Sequence>(_ s: S) where S.Iterator.Element == T {
        for item in s {
            items.append(item)
        }
    }
}

// rdar://problem/18120419
func TTGenWrap<T, I : IteratorProtocol>(_ iterator: I) where I.Element == (T,T)
{
  var iterator = iterator
  _ = iterator.next()
}

func IntIntGenWrap<I : IteratorProtocol>(_ iterator: I) where I.Element == (Int,Int)
{
  var iterator = iterator
  _ = iterator.next()
}

func GGWrap<I1 : IteratorProtocol, I2 : IteratorProtocol>(_ i1: I1, _ i2: I2) where I1.Element == I2.Element
{
  var i1 = i1
  var i2 = i2
  _ = i1.next()
  _ = i2.next()
}

func testSameTypeTuple(_ a: Array<(Int,Int)>, s: ArraySlice<(Int,Int)>) {
  GGWrap(a.makeIterator(), s.makeIterator())
  TTGenWrap(a.makeIterator())
  IntIntGenWrap(s.makeIterator())
}

// rdar://problem/20256475
protocol FooProtocol {
  associatedtype Element

  func getElement() -> Element
}
protocol Bar {
  associatedtype Foo : FooProtocol

  func getFoo() -> Foo

  mutating func extend<C : FooProtocol>(_ elements: C)
    where C.Element == Foo.Element
}

// rdar://problem/21620908
protocol P1 { }

protocol P2Base { }

protocol P2 : P2Base {
  associatedtype Q : P1

  func getQ() -> Q
}

struct XP1<T : P2Base> : P1 {
  func wibble() { }
}

func sameTypeParameterizedConcrete<C : P2>(_ c: C) where C.Q == XP1<C> {
  c.getQ().wibble()
}

// rdar://problem/21621421
protocol P3 {
  associatedtype AssocP3 : P1
}

protocol P4 {
  associatedtype AssocP4 : P3
}

struct X1 : P1 { }

struct X3 : P3 {
  typealias AssocP3 = X1
}

func foo<C : P4>(_ c: C) where C.AssocP4 == X3 {}

struct X4 : P4 {
  typealias AssocP4 = X3
}

func testFoo(_ x3: X4) {
  foo(x3)
}

// rdar://problem/21625478
struct X6<T> { }

protocol P6 { }

protocol P7 {
  associatedtype AssocP7
}

protocol P8 {
  associatedtype AssocP8 : P7
  associatedtype AssocOther
}

func testP8<C : P8>(_ c: C) where C.AssocOther == X6<C.AssocP8.AssocP7> {}

// setGenericSignature() was getting called twice here
struct Ghost<T> {}

protocol Timewarp {
  associatedtype Wormhole
}

struct Teleporter<A, B> where A : Timewarp, A.Wormhole == Ghost<B> {}

struct Beam {}

struct EventHorizon : Timewarp {
  typealias Wormhole = Ghost<Beam>
}

func activate<T>(_ t: T) {}

activate(Teleporter<EventHorizon, Beam>())

// rdar://problem/29288428
class C {}

protocol P9 {
  associatedtype A
}

struct X7<T: P9> where T.A : C { }

extension X7 where T.A == Int { } // expected-error {{no type for 'T.A' can satisfy both 'T.A : _NativeClass' and 'T.A == Int}}
// expected-error@-1 {{no type for 'T.A' can satisfy both 'T.A : C' and 'T.A == Int'}}
struct X8<T: C> { }

extension X8 where T == Int { } // expected-error {{no type for 'T' can satisfy both 'T : _NativeClass' and 'T == Int'}}
// expected-error@-1 {{no type for 'T' can satisfy both 'T : C' and 'T == Int'}}

protocol P10 {
	associatedtype A
	associatedtype B
	associatedtype C
	associatedtype D
	associatedtype E
}

protocol P11: P10 where A == B { }

func intracomponent<T: P11>(_: T)
  where T.A == T.B { } // expected-warning{{redundant same-type constraint 'T.A' == 'T.B'}}

func intercomponentSameComponents<T: P10>(_: T)
  where T.A == T.B,
        T.B == T.A { } // expected-warning{{redundant same-type constraint 'T.B' == 'T.A'}}

func intercomponentMoreThanSpanningTree<T: P10>(_: T)
  where T.A == T.B,
        T.B == T.C,
        T.D == T.E,
        T.D == T.B,
        T.E == T.B  // expected-warning{{redundant same-type constraint 'T.E' == 'T.B'}}
        { }

func trivialRedundancy<T: P10>(_: T) where T.A == T.A { } // expected-warning{{redundant same-type constraint 'T.A' == 'T.A'}}

struct X11<T: P10> where T.A == T.B { }

func intracomponentInferred<T>(_: X11<T>)
  where T.A == T.B { }

// Suppress redundant same-type constraint warnings from result types.
struct StructTakingP1<T: P1> { }

func resultTypeSuppress<T: P1>() -> StructTakingP1<T> {
  return StructTakingP1()
}

// Check directly-concrete same-type constraints
typealias NotAnInt = Double

extension X11 where NotAnInt == Int { }
// expected-error@-1{{generic signature requires types 'NotAnInt' (aka 'Double') and 'Int' to be the same}}

// rdar://45307061 - dropping delayed same-type constraints when merging
// equivalence classes

protocol FakeIterator {
  associatedtype Element
}

protocol FakeSequence {
  associatedtype Iterator : FakeIterator
  associatedtype Element where Iterator.Element == Element
}

protocol ObserverType {
  associatedtype E
}

struct Bad<S: FakeSequence, O> where S.Element : ObserverType, S.Element.E == O {}

func good<S: FakeSequence, O>(_: S, _: O) where S.Element : ObserverType, O == S.Element.E {
  _ = Bad<S, O>()
}

func bad<S: FakeSequence, O>(_: S, _: O) where S.Element : ObserverType, O == S.Iterator.Element.E {
  _ = Bad<S, O>()
}

func ugly<S: FakeSequence, O>(_: S, _: O) where S.Element : ObserverType, O == S.Iterator.Element.E, O == S.Element.E {
// expected-warning@-1 {{redundant same-type constraint 'O' == 'S.Element.E'}}
  _ = Bad<S, O>()
}
