// RUN: %target-typecheck-verify-swift

struct MyType<TyA, TyB> { // expected-note {{generic type 'MyType' declared here}}
  // expected-note @-1 {{arguments to generic parameter 'TyB' ('S' and 'Int') are expected to be equal}}
  // expected-note @-2 6 {{arguments to generic parameter 'TyA' ('Float' and 'Int') are expected to be equal}}
  // expected-note @-3 2 {{arguments to generic parameter 'TyA' ('Float' and 'Double') are expected to be equal}}
  // expected-note @-4 2 {{arguments to generic parameter 'TyB' ('Int' and 'Float') are expected to be equal}}
  // expected-note @-5 2 {{arguments to generic parameter 'TyB' ('Double' and 'Float') are expected to be equal}}
  var a : TyA, b : TyB
}

//
// Type aliases that reference unbound generic types -- not really generic,
// but they behave as such, in the sense that you can apply generic
// arguments to them.
//

typealias OurType = MyType

typealias YourType = Swift.Optional

struct Container {
  typealias YourType = Swift.Optional
}

let _: OurType<Int, String>
let _: YourType<Int>
let _: Container.YourType<Int>

// The question of whether a type alias is implicitly generic should not
// arise until after we attempt to resolve the underlying type.
do {
  struct Outer<T> {
    typealias NotGeneric = Outer
    struct Inner<U> {
      typealias NotGeneric = Outer
    }
  }

  let _: Outer<Int>.NotGeneric<Bool>
  // expected-error@-1 {{cannot specialize non-generic type 'Outer<Int>.NotGeneric' (aka 'Outer<Int>')}}
  let _: Outer<Int>.Inner<Never>.NotGeneric<Bool>
  // expected-error@-1 {{cannot specialize non-generic type 'Outer<Int>.Inner<Never>.NotGeneric' (aka 'Outer<Int>')}}
}

//
// Bona-fide generic type aliases
//

typealias DS<T> = MyType<String, T>

typealias BadA<T : Int> = MyType<String, T>  // expected-error {{type 'T' constrained to non-protocol, non-class type 'Int'}}

typealias BadB<T where T == Int> = MyType<String, T>  // expected-error {{associated types must not have a generic parameter list}}
// expected-warning@-1 {{same-type requirement makes generic parameter 'T' non-generic}}

typealias BadC<T,T> = MyType<String, T>  // expected-error {{invalid redeclaration of 'T'}}
// expected-note @-1 {{'T' previously declared here}}

typealias Tuple2<T1, T2> = (T1, T2)

typealias Tuple3<T1> = (T1, T1) where T1 : Hashable


let _ : Tuple2<Int, String> = (1, "foo")
let _ : Tuple2 = (1, "foo")
let _ : Tuple2<Int, String> = ("bar",  // expected-error {{cannot convert value of type '(String, String)' to specified type 'Tuple2<Int, String>' (aka '(Int, String)')}}
                               "foo")

func f() {
  typealias Tuple2b<T1, T2> = (T1, T2)
  let _ : Tuple2b = (1, "foo")
  
}


typealias A<T1, T2> = MyType<T2, T1>  // expected-note {{generic type 'A' declared here}}

typealias B<T1> = MyType<T1, T1>

typealias C<T> = MyType<String, T>

// Type aliases with unused generic params.
typealias D<T1, T2, T3> = MyType<T2, T1>  // expected-note 3 {{'T3' declared as parameter to type 'D'}}

typealias E<T1, T2> = Int  // expected-note {{generic type 'E' declared here}}
// expected-note@-1 {{'T1' declared as parameter to type 'E'}}
// expected-note@-2 {{'T2' declared as parameter to type 'E'}}

typealias F<T1, T2> = (T1) -> T2

// Type alias of type alias.
typealias G<S1, S2> = A<S1, S2>

let _ : E<Int, Float> = 42
let _ : E<Float> = 42   // expected-error {{generic type 'E' specialized with too few type parameters (got 1, but expected 2)}}
let _ : E = 42
// expected-error@-1 {{generic parameter 'T1' could not be inferred}}
// expected-error@-2 {{generic parameter 'T2' could not be inferred}}
let _ : D = D(a: 1, b: 2)
// expected-error@-1 {{generic parameter 'T3' could not be inferred}}
// expected-note@-2 {{explicitly specify the generic arguments to fix this issue}} {{14-14=<Int, Int, Any>}}

let _ : D<Int, Int, Float> = D<Int, Int, Float>(a: 1, b: 2)

let _ : D = D<Int, Int, Float>(a: 1, b: 2)
// expected-error@-1 {{generic parameter 'T3' could not be inferred}}


// expected-error @+2 {{generic parameter 'T3' could not be inferred}}
// expected-note @+1 {{explicitly specify the generic arguments to fix this issue}} {{31-31=<Int, Int, Any>}}
let _ : D<Int, Int, Float> = D(a: 1, b: 2)

let _ : F = { (a : Int) -> Int in a }  // Infer the types of F

let _ : F = { a in a } // expected-error {{cannot infer type of closure parameter 'a' without a type annotation}}

_ = MyType(a: "foo", b: 42)
_ = A(a: "foo", b: 42)
_ = A<Int, String>(a: "foo", b: 42)
_ = A<String, Int>(a: "foo", // expected-error {{cannot convert value of type 'String' to expected argument type 'Int'}}
  b: 42) // expected-error {{cannot convert value of type 'Int' to expected argument type 'String'}}
_ = B(a: 12, b: 42)
_ = B(a: 12, b: 42 as Float)
_ = B(a: "foo", b: 42)     // expected-error {{conflicting arguments to generic parameter 'T1' ('Int' vs. 'String')}}
_ = C(a: "foo", b: 42)
_ = C(a: 42,        // expected-error {{cannot convert value of type 'Int' to expected argument type 'String'}}
  b: 42)

_ = G(a: "foo", b: 42)
_ = G<Int, String>(a: "foo", b: 42)

// Generic typealias cannot have unbound generic type.
typealias VeryBad1<T> = MyType // expected-error {{reference to generic type 'MyType' requires arguments in <...>}}
typealias VeryBad2<T> = Swift.Array // expected-error {{reference to generic type 'Array' requires arguments in <...>}}

struct MyTypeWithHashable<TyA, TyB : Hashable> {
}

typealias MTWHInt<HT : Hashable> = MyTypeWithHashable<Int, HT>
typealias MTWHInt2<HT> = MyTypeWithHashable<Int, HT> // expected-error {{type 'HT' does not conform to protocol 'Hashable'}}

func f(a : MyTypeWithHashable<Int, Int>) {
  f(a: MyTypeWithHashable<Int, Int>())
  f(a: MTWHInt<Int>())
}


// Unqualified lookup of generic typealiases nested inside generic contexts
class GenericClass<T> {
  typealias TA<U> = MyType<T, U>
  typealias TAI<U> = MyType<Int, U>

  func testNestedUnbound(t: T) {
    typealias Nested<X, Y> = MyType<X, Y>
    _ = Nested(a: t, b: t)
  }

  func testCapture<S>(s: S, t: T) -> TA<S> {
    return TA<S>(a: t, b: s)
  }

  func testCaptureUnbound<S>(s: S, t: T) -> TA<S> {
    return TA(a: t, b: s)
  }

  func testConcrete1(s: Int, t: T) -> TA<Int> {
    return TA<Int>(a: t, b: s)
  }

  func testConcreteUnbound1(s: Int, t: T) -> TA<Int> {
    return TA(a: t, b: s)
  }

  func testConcrete2(s: Float, t: Int) -> TAI<Float> {
    return TAI<Float>(a: t, b: s)
  }

  func testConcreteUnbound2(s: Float, t: Int) -> TAI<Float> {
    return TAI(a: t, b: s)
  }

  func testCaptureInvalid1<S>(s: S, t: T) -> TA<Int> {
    return TA<S>(a: t, b: s) // expected-error {{cannot convert return expression of type 'GenericClass<T>.TA<S>' (aka 'MyType<T, S>') to return type 'GenericClass<T>.TA<Int>' (aka 'MyType<T, Int>')}}
  }

  func testCaptureInvalid2<S>(s: Int, t: T) -> TA<S> {
    return TA(a: t, b: s) // expected-error {{cannot convert value of type 'Int' to expected argument type 'S'}}
  }

  struct NestedStruct<U> {
    typealias TA<V> = MyType<(T, V), (U, V)>

    func testCapture<S>(x: (T, S), y: (U, S)) -> TA<S> {
      return TA(a: x, b: y)
    }
  }

  // Stupid corner case -- underlying type is not dependent
  typealias NotDependent<U> = Int

  func misleadingCode(_: NotDependent<String>) {}
}

let gc = GenericClass<Double>()
let fn: MyType<Double, Int> = gc.testCapture(s: 1, t: 1.0)

func use<T>(_ t: T) {}
use(fn)

// Make sure we apply base substitutions to the interface type of the typealias
class ConcreteClass : GenericClass<String> {
  func testSubstitutedCapture1<S>(s: S, t: String) -> TA<S> {
    return TA<S>(a: t, b: s)
  }

  func testSubstitutedCapture2<S>(s: S, t: String) -> TA<S> {
    return TA(a: t, b: s)
  }

  func testSubstitutedCapture3(s: Int, t: String) -> TA<Int> {
    return TA<Int>(a: t, b: s)
  }

  func testSubstitutedCapture4(s: Int, t: String) -> TA<Int> {
    return TA(a: t, b: s)
  }

  func testSubstitutedCapture5(s: Float, t: Int) -> TAI<Float> {
    return TAI<Float>(a: t, b: s)
  }

  func testSubstitutedCapture6(s: Float, t: Int) -> TAI<Float> {
    return TAI(a: t, b: s)
  }
}

// Qualified lookup of generic typealiases nested inside concrete contexts
struct ConcreteStruct {
  typealias O<T> = Optional<T>
}

func takesUnsugaredType1(m: MyType<String, Float>) {}
func takesSugaredType1(m: ConcreteClass.TA<Float>) {
  takesUnsugaredType1(m: m)
}

let _ = ConcreteStruct.O(123)
let _ = ConcreteStruct.O<Int>(123)

let _: ConcreteStruct.O = ConcreteStruct.O(123)
let _: ConcreteStruct.O = ConcreteStruct.O<Int>(123)

let _: ConcreteStruct.O<Int> = ConcreteStruct.O(123)
let _: ConcreteStruct.O<Int> = ConcreteStruct.O<Int>(123)

// Qualified lookup of generic typealiases nested inside generic contexts
//
// FIXME marks cases which still don't work correctly, and either produce a
// spurious diagnostic, or are actually invalid and do not diagnose.
//
// This occurs because the constraint solver does the wrong thing with an
// UnresolvedSpecializeExpr applied to a generic typealias.
//
// In the other cases, we manage to fold the UnresolvedSpecializeExpr in the
// precheckExpression() phase, which handles generic typealiases correctly.

do {
  let x1 = GenericClass.TA<Float>(a: 4.0, b: 1) // FIXME
  let x2 = GenericClass.TA<Float>(a: 1, b: 4.0) // FIXME

  // FIXME: Should not diagnose
  let _: MyType<Double, Float> = x1 // expected-error {{cannot assign value of type 'MyType<Float, Int>' to type 'MyType<Double, Float>'}}
  let _: MyType<Int, Float> = x2 // expected-error {{cannot assign value of type 'MyType<Float, Double>' to type 'MyType<Int, Float>'}}
}

let _ = GenericClass<Int>.TA(a: 4.0, b: 1) // expected-error {{cannot convert value of type 'Double' to expected argument type 'Int'}}
let _ = GenericClass<Int>.TA(a: 1, b: 4.0)

let _ = GenericClass<Int>.TA<Float>(a: 4.0, b: 1) // expected-error {{cannot convert value of type 'Double' to expected argument type 'Int'}}
let _ = GenericClass<Int>.TA<Float>(a: 1, b: 4.0)

let _: GenericClass.TA = GenericClass.TA(a: 4.0, b: 1)
let _: GenericClass.TA = GenericClass.TA(a: 1, b: 4.0)

do {
  let x1: GenericClass.TA = GenericClass.TA<Float>(a: 4.0, b: 1) // FIXME
  let x2: GenericClass.TA = GenericClass.TA<Float>(a: 1, b: 4.0) // FIXME

  // FIXME: Should not diagnose
  let _: MyType<Double, Float> = x1 // expected-error {{cannot assign value of type 'MyType<Float, Int>' to type 'MyType<Double, Float>'}}
  let _: MyType<Int, Float> = x2 // expected-error {{cannot assign value of type 'MyType<Float, Double>' to type 'MyType<Int, Float>'}}
}

let _: GenericClass.TA = GenericClass<Int>.TA(a: 4.0, b: 1) // expected-error {{cannot convert value of type 'Double' to expected argument type 'Int'}}
let _: GenericClass.TA = GenericClass<Int>.TA(a: 1, b: 4.0)

let _: GenericClass.TA = GenericClass<Int>.TA<Float>(a: 4.0, b: 1) // expected-error {{cannot convert value of type 'Double' to expected argument type 'Int'}}
let _: GenericClass.TA = GenericClass<Int>.TA<Float>(a: 1, b: 4.0)

let _: GenericClass<Int>.TA = GenericClass.TA(a: 4.0, b: 1) // expected-error {{cannot convert value of type 'Double' to expected argument type 'Int'}}
let _: GenericClass<Int>.TA = GenericClass.TA(a: 1, b: 4.0)

let _: GenericClass<Int>.TA = GenericClass.TA<Float>(a: 4.0, b: 1) // expected-error {{cannot assign value of type 'MyType<Float, Int>' to type 'MyType<Int, Int>'}}
let _: GenericClass<Int>.TA = GenericClass.TA<Float>(a: 1, b: 4.0) // expected-error {{cannot assign value of type 'MyType<Float, Double>' to type 'MyType<Int, Double>'}}

let _: GenericClass<Int>.TA = GenericClass<Int>.TA(a: 4.0, b: 1) // expected-error {{cannot convert value of type 'Double' to expected argument type 'Int'}}
let _: GenericClass<Int>.TA = GenericClass<Int>.TA(a: 1, b: 4.0)

let _: GenericClass<Int>.TA = GenericClass<Int>.TA<Float>(a: 4.0, b: 1) // expected-error {{cannot convert value of type 'Double' to expected argument type 'Int'}}
let _: GenericClass<Int>.TA = GenericClass<Int>.TA<Float>(a: 1, b: 4.0)

let _: GenericClass<Int>.TA<Float> = GenericClass.TA(a: 4.0, b: 1) // expected-error {{cannot convert value of type 'Double' to expected argument type 'Int'}}
let _: GenericClass<Int>.TA<Float> = GenericClass.TA(a: 1, b: 4.0)

let _: GenericClass<Int>.TA<Float> = GenericClass.TA<Float>(a: 4.0, b: 1) // expected-error {{cannot assign value of type 'MyType<Float, Float>' to type 'GenericClass<Int>.TA<Float>' (aka 'MyType<Int, Float>')}}
let _: GenericClass<Int>.TA<Float> = GenericClass.TA<Float>(a: 1, b: 4.0) // expected-error {{cannot assign value of type 'MyType<Float, Float>' to type 'GenericClass<Int>.TA<Float>' (aka 'MyType<Int, Float>')}}

let _: GenericClass<Int>.TA<Float> = GenericClass<Int>.TA(a: 4.0, b: 1) // expected-error {{cannot convert value of type 'Double' to expected argument type 'Int'}}
let _: GenericClass<Int>.TA<Float> = GenericClass<Int>.TA(a: 1, b: 4.0)

let _: GenericClass<Int>.TA<Float> = GenericClass<Int>.TA<Float>(a: 4.0, b: 1) // expected-error {{cannot convert value of type 'Double' to expected argument type 'Int'}}
let _: GenericClass<Int>.TA<Float> = GenericClass<Int>.TA<Float>(a: 1, b: 4.0)

func takesUnsugaredType2(m: MyType<Int, Float>) {}
func takesSugaredType2(m: GenericClass<Int>.TA<Float>) {
  takesUnsugaredType2(m: m)
}


//
// Error paths
//

// This works, but in the body of the extension we see the original type
// parameters of A<>'s underlying type MyType<>, rather than the type
// parameters of A<>.
extension A {}

extension A<Int> {}  // expected-error {{generic type 'A' specialized with too few type parameters (got 1, but expected 2)}}
extension A<Float,Int> {}
extension C<T> {}  // expected-error {{cannot find type 'T' in scope}}
extension C<Int> {}


protocol ErrorQ {
  associatedtype Y
}
protocol ErrorP {
  associatedtype X: ErrorQ // expected-note {{protocol requires nested type 'X'; do you want to add it?}}
}

typealias ErrorA<T: ErrorP> = T.X.Y

struct ErrorB : ErrorP { // expected-error {{type 'ErrorB' does not conform to protocol 'ErrorP'}}
  typealias X = ErrorC // expected-note {{possibly intended match 'ErrorB.X' (aka 'ErrorC') does not conform to 'ErrorQ'}}
}

struct ErrorC {
  typealias Y = Int
}

typealias Y = ErrorA<ErrorB>

typealias Id<T> = T

extension Id {} // expected-error {{non-nominal type 'Id' cannot be extended}}

class OuterGeneric<T> {
  typealias Alias<U> = AnotherGeneric<U>
  // expected-note@-1 {{generic type 'Alias' declared here}}
  class InnerNonGeneric : Alias {}
  // expected-error@-1 {{reference to generic type 'OuterGeneric<T>.Alias' requires arguments in <...>}}
}

class AnotherGeneric<T> {}

//
// Generic typealiases in protocols
//

protocol P {
  associatedtype A
  typealias G1<T> = MyType<Self, T>
  typealias G2<T> = MyType<T, A>
  typealias G3<T> = () -> ()
  typealias G4<T> = (T) -> ()

  func firstRequirement(_: G1<Int>)
  func secondRequirement(_: G2<Int>)
  func thirdRequirement(_: G3<Int>)
  func fourthRequirement(_: G4<Int>)

  func firstRequirementGeneric<T>(_: G1<T>)
  func secondRequirementGeneric<T>(_: G2<T>)
  func thirdRequirementGeneric<T>(_: G3<T>, _: T)
  func fourthRequirementGeneric<T>(_: G4<T>)
}

struct S : P {
  typealias A = Float

  func shouldFail(fn: (Int) -> ()) {
    thirdRequirement(fn)
    // expected-error@-1 {{cannot convert value of type '(Int) -> ()' to expected argument type '() -> ()'}}
  }

  func firstRequirement(_: G1<Int>) {}
  func secondRequirement(_: G2<Int>) {}
  func thirdRequirement(_: G3<Int>) {}
  func fourthRequirement(_: G4<Int>) {}

  func firstRequirementGeneric<T>(_: G1<T>) {
    _ = G1<T>.self
  }

  func secondRequirementGeneric<T>(_: G2<T>) {
    _ = G2<T>.self
  }

  func thirdRequirementGeneric<T>(_: G3<T>, _: T) {
    _ = G3<T>.self
  }

  func fourthRequirementGeneric<T>(_: G4<T>) {
    _ = G4<T>.self
  }

  func expressionContext() {
    let _: G1 = MyType<S, Int>(a: S(), b: 3)
    let _: G1<Int> = MyType<S, Int>(a: S(), b: 3)

    let _: S.G1 = MyType<S, Int>(a: S(), b: 3)
    let _: S.G1<Int> = MyType<S, Int>(a: S(), b: 3)

    let _: G2 = MyType<Int, Float>(a: 3, b: 1.0)
    let _: G2<Int> = MyType<Int, Float>(a: 3, b: 1.0)

    let _: S.G2 = MyType<Int, Float>(a: 3, b: 1.0)
    let _: S.G2<Int> = MyType<Int, Float>(a: 3, b: 1.0)
  }
}

func takesMyType(x: MyType<S, Int>) {}

func takesMyType(y: MyType<Int, Float>) {}

func f(x: S.G1<Int>, y: S.G2<Int>) {
  takesMyType(x: x)
  takesMyType(y: y)
}

//
// Generic typealiases with requirements
//

typealias Element<S> = S.Iterator.Element where S : Sequence

func takesInt(_: Element<[Int]>) {}

takesInt(10)

func failsRequirementCheck(_: Element<Int>) {}
// expected-error@-1 {{type 'Int' does not conform to protocol 'Sequence'}}

//
// Sugar in base types of a typealias.
//
struct X<T, U> {
  typealias GY<V> = [V]
}

typealias GX<T> = X<T, T>

func testSugar(_ gx: GX<Int>, _ gy: GX<Int>.GY<Double>, gz: GX<Int>.GY<Double>.Element) {
  let i: Int = gx   // expected-error{{cannot convert value of type 'GX<Int>' (aka 'X<Int, Int>') to specified type 'Int'}}
  let i2: Int = gy  // expected-error{{cannot convert value of type 'GX<Int>.GY<Double>' (aka 'Array<Double>') to specified type 'Int'}}
  let i3: Int = gz // expected-error{{cannot convert value of type 'GX<Int>.GY<Double>.Element' (aka 'Double') to specified type 'Int'}}
}
