// RUN: %target-typecheck-verify-swift

//===----------------------------------------------------------------------===//
// Deduction of generic arguments
//===----------------------------------------------------------------------===//

func identity<T>(_ value: T) -> T { return value }

func identity2<T>(_ value: T) -> T { return value }
// expected-note@-1 {{'identity2' produces 'Y', not the expected contextual result type 'X'}}
func identity2<T>(_ value: T) -> Int { return 0 }
// expected-note@-1 {{'identity2' produces 'Int', not the expected contextual result type 'X'}}

struct X { }
struct Y { }

func useIdentity(_ x: Int, y: Float, i32: Int32) {
  var x2 = identity(x)
  var y2 = identity(y)

  // Deduction that involves the result type
  x2 = identity(17)
  var i32_2 : Int32 = identity(17)

  // Deduction where the result type and input type can get different results
  var xx : X, yy : Y
  xx = identity(yy) // expected-error{{cannot assign value of type 'Y' to type 'X'}}
  xx = identity2(yy) // expected-error{{no 'identity2' candidates produce the expected contextual result type 'X'}}
}

func twoIdentical<T>(_ x: T, _ y: T) -> T {}

func useTwoIdentical(_ xi: Int, yi: Float) {
  var x = xi, y = yi
  x = twoIdentical(x, x)
  y = twoIdentical(y, y)
  x = twoIdentical(x, 1)
  x = twoIdentical(1, x)
  y = twoIdentical(1.0, y)
  y = twoIdentical(y, 1.0)
  
  twoIdentical(x, y) // expected-error{{conflicting arguments to generic parameter 'T' ('Int' vs. 'Float')}}
}

func mySwap<T>(_ x: inout T,
               _ y: inout T) {
  let tmp = x
  x = y
  y = tmp
}

func useSwap(_ xi: Int, yi: Float) {
  var x = xi, y = yi
  mySwap(&x, &x)
  mySwap(&y, &y)
  
  mySwap(x, x) // expected-error {{passing value of type 'Int' to an inout parameter requires explicit '&'}} {{10-10=&}}
    // expected-error @-1 {{passing value of type 'Int' to an inout parameter requires explicit '&'}} {{13-13=&}}
  
  mySwap(&x, &y) // expected-error{{cannot convert value of type 'Float' to expected argument type 'Int'}}
}

func takeTuples<T, U>(_: (T, U), _: (U, T)) {
}

func useTuples(_ x: Int, y: Float, z: (Float, Int)) {
  takeTuples((x, y), (y, x))

  takeTuples((x, y), (x, y))
  // expected-error@-1 {{conflicting arguments to generic parameter 'U' ('Float' vs. 'Int')}}
  // expected-error@-2 {{conflicting arguments to generic parameter 'T' ('Int' vs. 'Float')}}
  // FIXME: Use 'z', which requires us to fix our tuple-conversion
  // representation.
}

func acceptFunction<T, U>(_ f: (T) -> U, _ t: T, _ u: U) {}

func passFunction(_ f: (Int) -> Float, x: Int, y: Float) {
   acceptFunction(f, x, y)
   acceptFunction(f, y, y) // expected-error{{cannot convert value of type 'Float' to expected argument type 'Int'}}
}

func returnTuple<T, U>(_: T) -> (T, U) { } // expected-note {{in call to function 'returnTuple'}}

func testReturnTuple(_ x: Int, y: Float) {
  returnTuple(x) // expected-error{{generic parameter 'U' could not be inferred}}
  
  var _ : (Int, Float) = returnTuple(x)
  var _ : (Float, Float) = returnTuple(y)

  // <rdar://problem/22333090> QoI: Propagate contextual information in a call to operands
  var _ : (Int, Float) = returnTuple(y) // expected-error{{conflicting arguments to generic parameter 'T' ('Float' vs. 'Int')}}
}


func confusingArgAndParam<T, U>(_ f: (T) -> U, _ g: (U) -> T) {
  confusingArgAndParam(g, f)
  confusingArgAndParam(f, g)
}

func acceptUnaryFn<T, U>(_ f: (T) -> U) { }
func acceptUnaryFnSame<T>(_ f: (T) -> T) { }

func acceptUnaryFnRef<T, U>(_ f: inout (T) -> U) { }
func acceptUnaryFnSameRef<T>(_ f: inout (T) -> T) { }

func unaryFnIntInt(_: Int) -> Int {}

func unaryFnOvl(_: Int) -> Int {}
func unaryFnOvl(_: Float) -> Int {}

// Variable forms of the above functions
var unaryFnIntIntVar : (Int) -> Int = unaryFnIntInt

func passOverloadSet() {
  // Passing a non-generic function to a generic function
  acceptUnaryFn(unaryFnIntInt)
  acceptUnaryFnSame(unaryFnIntInt)

  // Passing an overloaded function set to a generic function
  acceptUnaryFn(unaryFnOvl) // expected-error{{conflicting arguments to generic parameter 'T' ('Int' vs. 'Float')}}
  acceptUnaryFnSame(unaryFnOvl)

  // Passing a variable of function type to a generic function
  acceptUnaryFn(unaryFnIntIntVar)
  acceptUnaryFnSame(unaryFnIntIntVar)

  // Passing a variable of function type to a generic function to an inout parameter
  acceptUnaryFnRef(&unaryFnIntIntVar)
  acceptUnaryFnSameRef(&unaryFnIntIntVar)

  acceptUnaryFnRef(unaryFnIntIntVar) // expected-error{{passing value of type '(Int) -> Int' to an inout parameter requires explicit '&'}} {{20-20=&}}
}

func acceptFnFloatFloat(_ f: (Float) -> Float) {}
func acceptFnDoubleDouble(_ f: (Double) -> Double) {}

func passGeneric() {
  acceptFnFloatFloat(identity)
  acceptFnFloatFloat(identity2)
}

//===----------------------------------------------------------------------===//
// Simple deduction for generic member functions
//===----------------------------------------------------------------------===//
struct SomeType {
  func identity<T>(_ x: T) -> T { return x }

  func identity2<T>(_ x: T) -> T { return x } // expected-note 2{{found this candidate}}
  func identity2<T>(_ x: T) -> Float { } // expected-note 2{{found this candidate}}

  func returnAs<T>() -> T {}
}

func testMemberDeduction(_ sti: SomeType, ii: Int, fi: Float) {
  var st = sti, i = ii, f = fi
  i = st.identity(i)
  f = st.identity(f)
  i = st.identity2(i)
  f = st.identity2(f) // expected-error{{ambiguous use of 'identity2'}}
  i = st.returnAs()
  f = st.returnAs()
  acceptFnFloatFloat(st.identity)
  acceptFnFloatFloat(st.identity2) // expected-error{{ambiguous use of 'identity2'}}
  acceptFnDoubleDouble(st.identity2)
}

struct StaticFuncs {
  static func chameleon<T>() -> T {}
  func chameleon2<T>() -> T {}
}

struct StaticFuncsGeneric<U> {
  // FIXME: Nested generics are very broken
  // static func chameleon<T>() -> T {}
}

func chameleon<T>() -> T {}

func testStatic(_ sf: StaticFuncs, sfi: StaticFuncsGeneric<Int>) {
  var x: Int16
  x = StaticFuncs.chameleon()
  x = sf.chameleon2()
  // FIXME: Nested generics are very broken
  // x = sfi.chameleon()
  // typealias SFI = StaticFuncsGeneric<Int>
  // x = SFI.chameleon()
  _ = x
}

//===----------------------------------------------------------------------===//
// Deduction checking for constraints
//===----------------------------------------------------------------------===//
protocol IsBefore {
  func isBefore(_ other: Self) -> Bool
}

func min2<T : IsBefore>(_ x: T, _ y: T) -> T { // expected-note {{where 'T' = 'Float'}}
  if y.isBefore(x) { return y }
  return x
}

extension Int : IsBefore {
  func isBefore(_ other: Int) -> Bool { return self < other }
}

func callMin(_ x: Int, y: Int, a: Float, b: Float) {
  _ = min2(x, y)
  min2(a, b) // expected-error{{global function 'min2' requires that 'Float' conform to 'IsBefore'}}
}

func rangeOfIsBefore<R : IteratorProtocol>(_ range: R) where R.Element : IsBefore {} // expected-note {{where 'R.Element' = 'IndexingIterator<[Double]>.Element' (aka 'Double')}}

func callRangeOfIsBefore(_ ia: [Int], da: [Double]) {
  rangeOfIsBefore(ia.makeIterator())
  rangeOfIsBefore(da.makeIterator()) // expected-error{{global function 'rangeOfIsBefore' requires that 'IndexingIterator<[Double]>.Element' (aka 'Double') conform to 'IsBefore'}}
}

func testEqualIterElementTypes<A: IteratorProtocol, B: IteratorProtocol>(_ a: A, _ b: B) where A.Element == B.Element {}
// expected-note@-1 {{where 'A.Element' = 'IndexingIterator<[Int]>.Element' (aka 'Int'), 'B.Element' = 'IndexingIterator<[Double]>.Element' (aka 'Double')}}
func compareIterators() {
  var a: [Int] = []
  var b: [Double] = []
  testEqualIterElementTypes(a.makeIterator(), b.makeIterator())
  // expected-error@-1 {{global function 'testEqualIterElementTypes' requires the types 'IndexingIterator<[Int]>.Element' (aka 'Int') and 'IndexingIterator<[Double]>.Element' (aka 'Double') be equivalent}}
}

protocol P_GI {
  associatedtype Y
}

class C_GI : P_GI {
  typealias Y = Double
}

class GI_Diff {}
func genericInheritsA<T>(_ x: T) where T : P_GI, T.Y : GI_Diff {}
// expected-note@-1 {{where 'T.Y' = 'C_GI.Y' (aka 'Double')}}
genericInheritsA(C_GI())
// expected-error@-1 {{global function 'genericInheritsA' requires that 'C_GI.Y' (aka 'Double') inherit from 'GI_Diff'}}

//===----------------------------------------------------------------------===//
// Deduction for member operators
//===----------------------------------------------------------------------===//
protocol Addable {
  static func +(x: Self, y: Self) -> Self
}
func addAddables<T : Addable, U>(_ x: T, y: T, u: U) -> T {
  u + u // expected-error{{binary operator '+' cannot be applied to two 'U' operands}}
  return x+y
}

//===----------------------------------------------------------------------===//
// Deduction for bound generic types
//===----------------------------------------------------------------------===//
struct MyVector<T> { func size() -> Int {} }

func getVectorSize<T>(_ v: MyVector<T>) -> Int { // expected-note {{in call to function 'getVectorSize'}}
  return v.size()
}

func ovlVector<T>(_ v: MyVector<T>) -> X {}
func ovlVector<T>(_ v: MyVector<MyVector<T>>) -> Y {}

func testGetVectorSize(_ vi: MyVector<Int>, vf: MyVector<Float>) {
  var i : Int
  i = getVectorSize(vi)
  i = getVectorSize(vf)

  getVectorSize(i) // expected-error{{cannot convert value of type 'Int' to expected argument type 'MyVector<T>'}}
  // expected-error@-1 {{generic parameter 'T' could not be inferred}}

  var x : X, y : Y
  x = ovlVector(vi)
  x = ovlVector(vf)
  
  var vvi : MyVector<MyVector<Int>>
  y = ovlVector(vvi)

  var yy = ovlVector(vvi)
  yy = y
  y = yy
}

// <rdar://problem/15104554>
postfix operator <*>

protocol MetaFunction {
  associatedtype Result
  static postfix func <*> (_: Self) -> Result?
}

protocol Bool_ {}
struct False : Bool_ {}
struct True : Bool_ {}

postfix func <*> <B>(_: Test<B>) -> Int? { return .none }
postfix func <*> (_: Test<True>) -> String? { return .none }

class Test<C: Bool_> : MetaFunction {
  typealias Result = Int
} // picks first <*>
typealias Inty = Test<True>.Result 
var iy : Inty = 5 // okay, because we picked the first <*>
var iy2 : Inty = "hello" // expected-error{{cannot convert value of type 'String' to specified type 'Inty' (aka 'Int')}}

// rdar://problem/20577950
class DeducePropertyParams {
  let badSet: Set = ["Hello"]
}

// SR-69
struct A {}
func foo() {
    for i in min(1,2) { // expected-error{{for-in loop requires 'Int' to conform to 'Sequence'}}
    }
    let j = min(Int(3), Float(2.5)) // expected-error{{conflicting arguments to generic parameter 'T' ('Int' vs. 'Float')}}
    let k = min(A(), A()) // expected-error{{global function 'min' requires that 'A' conform to 'Comparable'}}
    let oi : Int? = 5
    let l = min(3, oi) // expected-error {{value of optional type 'Int?' must be unwrapped to a value of type 'Int'}}
    // expected-note@-1 {{coalesce using '??' to provide a default when the optional value contains 'nil'}}
    // expected-note@-2 {{force-unwrap using '!' to abort execution if the optional value contains 'nil'}}
}

infix operator +&
func +&<R, S>(lhs: inout R, rhs: S) where R : RangeReplaceableCollection, S : Sequence, R.Element == S.Element {}
// expected-note@-1 {{where 'R.Element' = 'String', 'S.Element' = 'String.Element' (aka 'Character')}}

func rdar33477726_1() {
  var arr: [String] = []
  arr +& "hello"
  // expected-error@-1 {{operator function '+&' requires the types 'String' and 'String.Element' (aka 'Character') be equivalent}}
}

func rdar33477726_2<R, S>(_: R, _: S) where R: Sequence, S == R.Element {}
rdar33477726_2("answer", 42)
// expected-error@-1 {{cannot convert value of type 'Int' to expected argument type 'String.Element' (aka 'Character')}}

prefix operator +-
prefix func +-<T>(_: T) where T: Sequence, T.Element == Int {}
// expected-note@-1 {{where 'T.Element' = 'String.Element' (aka 'Character')}}

+-"hello"
// expected-error@-1 {{operator function '+-' requires the types 'String.Element' (aka 'Character') and 'Int' be equivalent}}

func test_transitive_subtype_deduction_for_generic_params() {
  class A {}

  func foo<T: A>(_: [(String, (T) -> Void)]) {}

  func bar<U>(_: @escaping (U) -> Void) -> (U) -> Void {
    return { _ in }
  }

  // Here we have:
  //  - `W subtype of A`
  //  - `W subtype of U`
  //
  // Type variable associated with `U` has to be attempted
  // first because solver can't infer bindings for `W` transitively
  // through `U`.
  func baz<W: A>(_ arr: [(String, (W) -> Void)]) {
    foo(arr.map { ($0.0, bar($0.1)) }) // Ok
  }

  func fiz<T>(_ a: T, _ op: (T, T) -> Bool, _ b: T) {}
  func biz(_ v: Int32) {
    fiz(v, !=, -1) // Ok because -1 literal should be inferred as Int32
  }
}

// SR-13776
func SR13776<E: Equatable>(_ e1: E, _ e2: E) {} 

func SR1377f<C1: Collection, C2: Collection>(_ c1: C1, _ c2: C2) where C1.Element: Equatable, C2.Element: Equatable {
    SR13776(c1.map({ $0 }), c2.map({ $0 })) // expected-error {{conflicting arguments to generic parameter 'E' ('[C1.Element]' vs. '[C2.Element]')}}
}

struct SR13776A {}
struct SR13776B {}
struct SR13776C: Equatable {}

SR13776(SR13776A(), SR13776B()) // expected-error {{conflicting arguments to generic parameter 'E' ('SR13776A' vs. 'SR13776B')}}
SR13776(SR13776A(), SR13776C()) // expected-error {{conflicting arguments to generic parameter 'E' ('SR13776A' vs. 'SR13776C')}}

// Chooses the E == SR13776C as best solution.
SR13776(SR13776C(), SR13776A()) // expected-error {{cannot convert value of type 'SR13776A' to expected argument type 'SR13776C'}}

// Generic mismatch involving deep-equality.
func SR13776_Test<T>(_ expression1: @autoclosure () throws -> T, 
                     _ expression2: @autoclosure () throws -> T) where T : Equatable {}

extension Collection {
  func aCollectionExtension() -> [SubSequence] { [] }
}

let expected : [[Int]] = []
let actual = [1, 2, 3].aCollectionExtension()

SR13776_Test(expected, actual) 
// expected-error@-1 {{conflicting arguments to generic parameter 'T' ('[[Int]]' vs. '[ArraySlice<Int>]')}}
