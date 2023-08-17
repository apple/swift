// RUN: %target-typecheck-verify-swift

protocol P {
  associatedtype SomeType
}

protocol P2 { 
  func wonka()
}

extension Int : P {
  typealias SomeType = Int
}

extension Double : P {
  typealias SomeType = Double
}

func f0(_ x: Int, 
        _ y: Float) { }

func f1(_: @escaping (Int, Float) -> Int) { }

func f2(_: (_: (Int) -> Int)) -> Int {}

func f3(_: @escaping (_: @escaping (Int) -> Float) -> Int) {}

func f4(_ x: Int) -> Int { }

func f5<T : P2>(_ : T) { }
// expected-note@-1 {{required by global function 'f5' where 'T' = '(Int) -> Int'}}
// expected-note@-2 {{required by global function 'f5' where 'T' = '(Int, String)'}}
// expected-note@-3 {{required by global function 'f5' where 'T' = 'Int.Type'}}
// expected-note@-4 {{where 'T' = 'Int'}}

func f6<T : P, U : P>(_ t: T, _ u: U) where T.SomeType == U.SomeType {}

var i : Int
var d : Double

// Check the various forms of diagnostics the type checker can emit.

// Tuple size mismatch.
f1(
   f4 // expected-error {{cannot convert value of type '(Int) -> Int' to expected argument type '(Int, Float) -> Int'}}
   ) 

// Tuple element unused.
f0(i, i, // expected-error@:7 {{cannot convert value of type 'Int' to expected argument type 'Float'}}
   i) // expected-error{{extra argument in call}}


// Cannot conform to protocols.
f5(f4)  // expected-error {{type '(Int) -> Int' cannot conform to 'P2'}} expected-note {{only concrete types such as structs, enums and classes can conform to protocols}}
f5((1, "hello"))  // expected-error {{type '(Int, String)' cannot conform to 'P2'}} expected-note {{only concrete types such as structs, enums and classes can conform to protocols}}
f5(Int.self) // expected-error {{type 'Int.Type' cannot conform to 'P2'}} expected-note {{only concrete types such as structs, enums and classes can conform to protocols}}

// Tuple element not convertible.
f0(i,
   d  // expected-error {{cannot convert value of type 'Double' to expected argument type 'Float'}}
   )

// Function result not a subtype.
f1(
   f0 // expected-error {{cannot convert value of type '(Int, Float) -> ()' to expected argument type '(Int, Float) -> Int'}}
   )

f3(
   f2 // expected-error {{cannot convert value of type '(@escaping ((Int) -> Int)) -> Int' to expected argument type '(@escaping (Int) -> Float) -> Int'}}
   )

f4(i, d) // expected-error {{extra argument in call}}

// Missing member.
i.missingMember() // expected-error{{value of type 'Int' has no member 'missingMember'}}

// Generic member does not conform.
extension Int {
  func wibble<T: P2>(_ x: T, _ y: T) -> T { return x } // expected-note {{where 'T' = 'Int'}}
  func wubble<T>(_ x: (Int) -> T) -> T { return x(self) }
}
i.wibble(3, 4) // expected-error {{instance method 'wibble' requires that 'Int' conform to 'P2'}}

// Generic member args correct, but return type doesn't match.
struct A : P2 {
  func wonka() {}
}
let a = A()
for j in i.wibble(a, a) { // expected-error {{for-in loop requires 'A' to conform to 'Sequence'}}
}

// Generic as part of function/tuple types
func f6<T:P2>(_ g: (Void) -> T) -> (c: Int, i: T) { // expected-warning {{when calling this function in Swift 4 or later, you must pass a '()' tuple; did you mean for the input type to be '()'?}} {{20-26=()}}
  return (c: 0, i: g(()))
}

func f7() -> (c: Int, v: A) {
  let g: (Void) -> A = { _ in return A() } // expected-warning {{when calling this function in Swift 4 or later, you must pass a '()' tuple; did you mean for the input type to be '()'?}} {{10-16=()}}
  return f6(g) // expected-error {{cannot convert return expression of type '(c: Int, i: A)' to return type '(c: Int, v: A)'}}
}

func f8<T:P2>(_ n: T, _ f: @escaping (T) -> T) {}  // expected-note {{where 'T' = 'Int'}}
// expected-note@-1 {{required by global function 'f8' where 'T' = '(Int, Double)'}}
f8(3, f4) // expected-error {{global function 'f8' requires that 'Int' conform to 'P2'}}
typealias Tup = (Int, Double)
func f9(_ x: Tup) -> Tup { return x }
f8((1,2.0), f9) // expected-error {{type '(Int, Double)' cannot conform to 'P2'}} expected-note {{only concrete types such as structs, enums and classes can conform to protocols}}

// <rdar://problem/19658691> QoI: Incorrect diagnostic for calling nonexistent members on literals
1.doesntExist(0)  // expected-error {{value of type 'Int' has no member 'doesntExist'}}
[1, 2, 3].doesntExist(0)  // expected-error {{value of type '[Int]' has no member 'doesntExist'}}
"awfawf".doesntExist(0)   // expected-error {{value of type 'String' has no member 'doesntExist'}}

// Does not conform to protocol.
f5(i)  // expected-error {{global function 'f5' requires that 'Int' conform to 'P2'}}

// Make sure we don't leave open existentials when diagnosing.
// <rdar://problem/20598568>
func pancakes(_ p: P2) {
  f4(p.wonka) // expected-error{{cannot convert value of type '() -> ()' to expected argument type 'Int'}}
  f4(p.wonka()) // expected-error{{cannot convert value of type '()' to expected argument type 'Int'}}
}

protocol Shoes {
  static func select(_ subject: Shoes) -> Self
}

// Here the opaque value has type (metatype_type (archetype_type ... ))
func f(_ x: Shoes, asType t: Shoes.Type) {
  return t.select(x) 
  // expected-error@-1 {{unexpected non-void return value in void function}}
  // expected-note@-2 {{did you mean to add a return type?}}
}

precedencegroup Starry {
  associativity: left
  higherThan: MultiplicationPrecedence
}

infix operator **** : Starry

func ****(_: Int, _: String) { }
i **** i // expected-error{{cannot convert value of type 'Int' to expected argument type 'String'}}

infix operator ***~ : Starry

func ***~(_: Int, _: String) { }
i ***~ i // expected-error{{cannot convert value of type 'Int' to expected argument type 'String'}}

@available(*, unavailable, message: "call the 'map()' method on the sequence")
public func myMap<C : Collection, T>( // expected-note {{'myMap' has been explicitly marked unavailable here}}
  _ source: C, _ transform: (C.Iterator.Element) -> T
) -> [T] {
  fatalError("unavailable function can't be called")
}

@available(*, unavailable, message: "call the 'map()' method on the optional value")
public func myMap<T, U>(_ x: T?, _ f: (T) -> U) -> U? {
  fatalError("unavailable function can't be called")
}

// <rdar://problem/20142523>
func rdar20142523() {
  _ = myMap(0..<10, { x in // expected-error {{'myMap' is unavailable: call the 'map()' method on the sequence}}
    ()
    return x
  })
}

// <rdar://problem/21080030> Bad diagnostic for invalid method call in boolean expression: (_, ExpressibleByIntegerLiteral)' is not convertible to 'ExpressibleByIntegerLiteral
func rdar21080030() {
  var s = "Hello"
  // https://github.com/apple/swift/issues/50141
  // This should be 'cannot_call_non_function_value'.
  if s.count() == 0 {} // expected-error{{cannot call value of non-function type 'Int'}} {{13-15=}}
}

// <rdar://problem/21248136> QoI: problem with return type inference mis-diagnosed as invalid arguments
func r21248136<T>() -> T { preconditionFailure() } // expected-note 2 {{in call to function 'r21248136()'}}

r21248136()            // expected-error {{generic parameter 'T' could not be inferred}}
let _ = r21248136()    // expected-error {{generic parameter 'T' could not be inferred}}


// <rdar://problem/16375647> QoI: Uncallable funcs should be compile time errors
func perform<T>() {}  // expected-error {{generic parameter 'T' is not used in function signature}}

// <rdar://problem/17080659> Error Message QOI - wrong return type in an overload
func recArea(_ h: Int, w : Int) {
  return h * w  
  // expected-error@-1 {{unexpected non-void return value in void function}}
  // expected-note@-2 {{did you mean to add a return type?}}
}

// <rdar://problem/17224804> QoI: Error In Ternary Condition is Wrong
func r17224804(_ monthNumber : Int) {
  // expected-error@+1:49 {{cannot convert value of type 'Int' to expected argument type 'String'}}
  let monthString = (monthNumber <= 9) ? ("0" + monthNumber) : String(monthNumber)
}

// <rdar://problem/17020197> QoI: Operand of postfix '!' should have optional type; type is 'Int?'
func r17020197(_ x : Int?, y : Int) {
  if x! {  }  // expected-error {{type 'Int' cannot be used as a boolean; test for '!= 0' instead}}

  // <rdar://problem/12939553> QoI: diagnostic for using an integer in a condition is utterly terrible
  if y {}    // expected-error {{type 'Int' cannot be used as a boolean; test for '!= 0' instead}}
}

// <rdar://problem/20714480> QoI: Boolean expr not treated as Bool type when function return type is different
func validateSaveButton(_ text: String) {
  return (text.count > 0) ? true : false  // expected-error {{unexpected non-void return value in void function}} expected-note {{did you mean to add a return type?}}
}

// <rdar://problem/20201968> QoI: poor diagnostic when calling a class method via a metatype
class r20201968C {
  func blah() {
    r20201968C.blah()  // expected-error {{instance member 'blah' cannot be used on type 'r20201968C'; did you mean to use a value of this type instead?}}
  }
}


// <rdar://problem/21459429> QoI: Poor compilation error calling assert
func r21459429(_ a : Int) {
  assert(a != nil, "ASSERT COMPILATION ERROR")
  // expected-warning @-1 {{comparing non-optional value of type 'Int' to 'nil' always returns true}}
}


// <rdar://problem/21362748> [WWDC Lab] QoI: cannot subscript a value of type '[Int]?' with an argument of type 'Int'
struct StructWithOptionalArray {
  var array: [Int]?
}

func testStructWithOptionalArray(_ foo: StructWithOptionalArray) -> Int {
  return foo.array[0]  // expected-error {{value of optional type '[Int]?' must be unwrapped to refer to member 'subscript' of wrapped base type '[Int]'}}
  // expected-note@-1{{chain the optional using '?' to access member 'subscript' only for non-'nil' base values}}{{19-19=?}}
  // expected-note@-2{{force-unwrap using '!' to abort execution if the optional value contains 'nil'}}{{19-19=!}}
}


// <rdar://problem/19774755> Incorrect diagnostic for unwrapping non-optional bridged types
var invalidForceUnwrap = Int()! // expected-error {{cannot force unwrap value of non-optional type 'Int'}} {{31-32=}}


// <rdar://problem/20905802> Swift using incorrect diagnostic sometimes on String().asdf
String().asdf  // expected-error {{value of type 'String' has no member 'asdf'}}


// <rdar://problem/21553065> Spurious diagnostic: '_' can only appear in a pattern or on the left side of an assignment
protocol r21553065Protocol {}
class r21553065Class<T : AnyObject> {} // expected-note{{requirement specified as 'T' : 'AnyObject'}}
_ = r21553065Class<r21553065Protocol>()  // expected-error {{'r21553065Class' requires that 'any r21553065Protocol' be a class type}}

// Type variables not getting erased with nested closures
struct Toe {
  let toenail: Nail // expected-error {{cannot find type 'Nail' in scope}}

  func clip() {
    toenail.inspect { x in
      toenail.inspect { y in }
    }
  }
}

// <rdar://problem/21447318> dot'ing through a partially applied member produces poor diagnostic
class r21447318 {
  var x = 42
  func doThing() -> r21447318 { return self }
}

func test21447318(_ a : r21447318, b : () -> r21447318) {
  a.doThing.doThing()  // expected-error {{method 'doThing' was used as a property; add () to call it}} {{12-12=()}}
  
  b.doThing() // expected-error {{function 'b' was used as a property; add () to call it}} {{4-4=()}}
}

// <rdar://problem/20409366> Diagnostics for init calls should print the class name
class r20409366C {
  init(a : Int) {}
  init?(a : r20409366C) {
    let req = r20409366C(a: 42)?  // expected-error {{cannot use optional chaining on non-optional value of type 'r20409366C'}} {{32-33=}}
  }
}


// <rdar://problem/18800223> QoI: wrong compiler error when swift ternary operator branches don't match
func r18800223(_ i : Int) {
  // 20099385
  _ = i == 0 ? "" : i  // expected-error {{result values in '? :' expression have mismatching types 'String' and 'Int'}}

  // 19648528
  _ = true ? [i] : i // expected-error {{result values in '? :' expression have mismatching types '[Int]' and 'Int'}}

  
  var buttonTextColor: String?
  _ = (buttonTextColor != nil) ? 42 : {$0}; // expected-error {{result values in '? :' expression have mismatching types 'Int' and '(_) -> _'}}
}

// <rdar://problem/21883806> Bogus "'_' can only appear in a pattern or on the left side of an assignment" is back
_ = { $0 }  // expected-error {{cannot infer type of closure parameter '$0' without a type annotation}}



_ = 4()   // expected-error {{cannot call value of non-function type 'Int'}}{{6-8=}}
_ = 4(1)  // expected-error {{cannot call value of non-function type 'Int'}}


// <rdar://problem/21784170> Incongruous `unexpected trailing closure` error in `init` function which is cast and called without trailing closure.
func rdar21784170() {
  let initial = (1.0 as Double, 2.0 as Double)
  (Array.init as (Double...) -> Array<Double>)(initial as (Double, Double)) // expected-error {{cannot convert value of type '(Double, Double)' to expected argument type 'Double'}}
}

// Diagnose passing an array in lieu of variadic parameters
func variadic(_ x: Int...) {}
func variadicArrays(_ x: [Int]...) {}
func variadicAny(_ x: Any...) {}
struct HasVariadicSubscript {
  subscript(_ x: Int...) -> Int {
    get { 0 }
  }
}
let foo = HasVariadicSubscript()

let array = [1,2,3]
let arrayWithOtherEltType = ["hello", "world"]

variadic(array) // expected-error {{cannot pass array of type '[Int]' as variadic arguments of type 'Int'}}
variadic([1,2,3]) // expected-error {{cannot pass array of type '[Int]' as variadic arguments of type 'Int'}}
// expected-note@-1 {{remove brackets to pass array elements directly}} {{10-11=}} {{16-17=}}
variadic([1,2,3,]) // expected-error {{cannot pass array of type '[Int]' as variadic arguments of type 'Int'}}
// expected-note@-1 {{remove brackets to pass array elements directly}} {{10-11=}} {{16-17=}} {{17-18=}}
variadic(0, array, 4) // expected-error {{cannot pass array of type '[Int]' as variadic arguments of type 'Int'}}
variadic(0, [1,2,3], 4) // expected-error {{cannot pass array of type '[Int]' as variadic arguments of type 'Int'}}
// expected-note@-1 {{remove brackets to pass array elements directly}} {{13-14=}} {{19-20=}}
variadic(0, [1,2,3,], 4) // expected-error {{cannot pass array of type '[Int]' as variadic arguments of type 'Int'}}
// expected-note@-1 {{remove brackets to pass array elements directly}} {{13-14=}} {{19-20=}} {{20-21=}}
variadic(arrayWithOtherEltType) // expected-error {{cannot convert value of type '[String]' to expected argument type 'Int'}}
variadic(1, arrayWithOtherEltType) // expected-error {{cannot convert value of type '[String]' to expected argument type 'Int'}}

// FIXME: https://github.com/apple/swift/issues/53499
variadic(["hello", "world"]) // expected-error 2 {{cannot convert value of type 'String' to expected element type 'Int'}}
// expected-error@-1 {{cannot pass array of type '[Int]' as variadic arguments of type 'Int'}}
// expected-note@-2 {{remove brackets to pass array elements directly}}

variadic([1] + [2] as [Int]) // expected-error {{cannot pass array of type '[Int]' as variadic arguments of type 'Int'}}

foo[array] // expected-error {{cannot pass array of type '[Int]' as variadic arguments of type 'Int'}}
foo[[1,2,3]] // expected-error {{cannot pass array of type '[Int]' as variadic arguments of type 'Int'}}
// expected-note@-1 {{remove brackets to pass array elements directly}} {{5-6=}} {{11-12=}}
foo[0, [1,2,3], 4] // expected-error {{cannot pass array of type '[Int]' as variadic arguments of type 'Int'}}
// expected-note@-1 {{remove brackets to pass array elements directly}} {{8-9=}} {{14-15=}}

variadicAny(array)
variadicAny([1,2,3])
variadicArrays(array)
variadicArrays([1,2,3])
variadicArrays(arrayWithOtherEltType) // expected-error {{cannot convert value of type '[String]' to expected argument type '[Int]'}}
// expected-note@-1 {{arguments to generic parameter 'Element' ('String' and 'Int') are expected to be equal}}
variadicArrays(1,2,3) // expected-error 3 {{cannot convert value of type 'Int' to expected argument type '[Int]'}}

protocol Proto {}
func f<T: Proto>(x: [T]) {}
func f(x: Int...) {}
f(x: [1,2,3])
// TODO(diagnostics): Diagnose both the missing conformance and the disallowed array splat to cover both overloads. 
// expected-error@-2 {{cannot pass array of type '[Int]' as variadic arguments of type 'Int'}}
// expected-note@-3 {{remove brackets to pass array elements directly}}

// <rdar://problem/21829141> BOGUS: unexpected trailing closure
func expect<T, U>(_: T) -> (U.Type) -> Int { return { a in 0 } }
func expect<T, U>(_: T, _: Int = 1) -> (U.Type) -> String { return { a in "String" } }
let expectType1 = expect(Optional(3))(Optional<Int>.self)
let expectType1Check: Int = expectType1

// <rdar://problem/19804707> Swift Enum Scoping Oddity
func rdar19804707() {
  enum Op {
    case BinaryOperator((Double, Double) -> Double)
  }
  var knownOps : Op
  knownOps = Op.BinaryOperator({$1 - $0})
  knownOps = Op.BinaryOperator(){$1 - $0}
  knownOps = Op.BinaryOperator{$1 - $0}

  knownOps = .BinaryOperator({$1 - $0})

  // rdar://19804707 - trailing closures for contextual member references.
  knownOps = .BinaryOperator(){$1 - $0}
  knownOps = .BinaryOperator{$1 - $0}

  _ = knownOps
}

// <rdar://problem/20491794> Error message does not tell me what the problem is
enum Color {
  case Red
  case Unknown(description: String)

  static func rainbow() -> Color {}
  
  static func overload(a : Int) -> Color {} // expected-note {{incorrect labels for candidate (have: '(_:)', expected: '(a:)')}}
  // expected-note@-1 {{candidate expects value of type 'Int' for parameter #1 (got 'Double')}}
  static func overload(b : Int) -> Color {} // expected-note {{incorrect labels for candidate (have: '(_:)', expected: '(b:)')}}
  // expected-note@-1 {{candidate expects value of type 'Int' for parameter #1 (got 'Double')}}
  
  static func frob(_ a : Int, b : inout Int) -> Color {}
  static var svar: Color { return .Red }
}

let _: (Int, Color) = [1,2].map({ ($0, .Unknown("")) })
// expected-error@-1 {{cannot convert value of type 'Array<(Int, _)>' to specified type '(Int, Color)'}}
// expected-error@-2 {{cannot infer contextual base in reference to member 'Unknown'}}

let _: [(Int, Color)] = [1,2].map({ ($0, .Unknown("")) })// expected-error {{missing argument label 'description:' in call}}

let _: [Color] = [1,2].map { _ in .Unknown("") }// expected-error {{missing argument label 'description:' in call}} {{44-44=description: }}

let _: (Int) -> (Int, Color) = { ($0, .Unknown("")) } // expected-error {{missing argument label 'description:' in call}} {{48-48=description: }}
let _: Color = .Unknown("") // expected-error {{missing argument label 'description:' in call}} {{25-25=description: }}
let _: Color = .Unknown // expected-error {{member 'Unknown(description:)' expects argument of type 'String'}}
let _: Color = .Unknown(42) // expected-error {{missing argument label 'description:' in call}}
// expected-error@-1 {{cannot convert value of type 'Int' to expected argument type 'String'}}
let _ : Color = .rainbow(42)  // expected-error {{argument passed to call that takes no arguments}}

let _ : (Int, Float) = (42.0, 12)  // expected-error {{cannot convert value of type '(Double, Float)' to specified type '(Int, Float)'}}

let _ : Color = .rainbow  // expected-error {{member 'rainbow()' is a function that produces expected type 'Color'; did you mean to call it?}} {{25-25=()}}

let _: Color = .overload(a : 1.0)  // expected-error {{cannot convert value of type 'Double' to expected argument type 'Int'}}
let _: Color = .overload(1.0)  // expected-error {{no exact matches in call to static method 'overload'}}
let _: Color = .overload(1)  // expected-error {{no exact matches in call to static method 'overload'}}
let _: Color = .frob(1.0, &i) // expected-error {{missing argument label 'b:' in call}}
// expected-error@-1 {{cannot convert value of type 'Double' to expected argument type 'Int'}}
let _: Color = .frob(1.0, b: &i) // expected-error {{cannot convert value of type 'Double' to expected argument type 'Int'}}
let _: Color = .frob(1, i)  // expected-error {{missing argument label 'b:' in call}}
// expected-error@-1 {{passing value of type 'Int' to an inout parameter requires explicit '&'}}
let _: Color = .frob(1, b: i)  // expected-error {{passing value of type 'Int' to an inout parameter requires explicit '&'}} {{28-28=&}}
let _: Color = .frob(1, &d) // expected-error {{missing argument label 'b:' in call}}
// expected-error@-1 {{cannot convert value of type 'Double' to expected argument type 'Int'}}
let _: Color = .frob(1, b: &d) // expected-error {{cannot convert value of type 'Double' to expected argument type 'Int'}}
var someColor : Color = .red // expected-error {{enum type 'Color' has no case 'red'; did you mean 'Red'?}}
someColor = .red  // expected-error {{enum type 'Color' has no case 'red'; did you mean 'Red'?}}
someColor = .svar() // expected-error {{cannot call value of non-function type 'Color'}}
someColor = .svar(1) // expected-error {{cannot call value of non-function type 'Color'}}

func testTypeSugar(_ a : Int) {
  typealias Stride = Int

  let x = Stride(a)
  x+"foo"            // expected-error {{binary operator '+' cannot be applied to operands of type 'Stride' (aka 'Int') and 'String'}}
// expected-note @-1 {{overloads for '+' exist with these partially matching parameter lists: (Int, Int), (String, String)}}
}

// <rdar://problem/21974772> SegFault in FailureDiagnosis::visitInOutExpr
func r21974772(_ y : Int) {
  let x = &(1.0 + y) // expected-error {{'&' may only be used to pass an argument to inout parameter}}
}

// <rdar://problem/22020088> QoI: missing member diagnostic on optional gives worse error message than existential/bound generic/etc
protocol r22020088P {}

func r22020088Foo<T>(_ t: T) {}

func r22020088bar(_ p: r22020088P?) {
  r22020088Foo(p.fdafs) // expected-error {{value of type '(any r22020088P)?' has no member 'fdafs'}}
}

// <rdar://problem/22288575> QoI: poor diagnostic involving closure, bad parameter label, and mismatch return type
func f(_ arguments: [String]) -> [ArraySlice<String>] {
  return arguments.split(maxSplits: 1, omittingEmptySubsequences: false, whereSeparator: { $0 == "--" })
}



struct AOpts : OptionSet {
  let rawValue : Int
}

class B {
  func function(_ x : Int8, a : AOpts) {}
  func f2(_ a : AOpts) {}
  static func f1(_ a : AOpts) {}
}

class GenClass<T> {}
struct GenStruct<T> {}
enum GenEnum<T> {}

func test(_ a : B) {
  B.f1(nil)              // expected-error {{'nil' is not compatible with expected argument type 'AOpts'}}
  a.function(42, a: nil) // expected-error {{'nil' is not compatible with expected argument type 'AOpts'}}
  a.function(42, nil)    // expected-error {{missing argument label 'a:' in call}}
  // expected-error@-1 {{'nil' is not compatible with expected argument type 'AOpts'}}
  a.f2(nil)              // expected-error {{'nil' is not compatible with expected argument type 'AOpts'}}

  func foo1(_ arg: Bool) -> Int {return nil}
  func foo2<T>(_ arg: T) -> GenClass<T> {return nil}
  func foo3<T>(_ arg: T) -> GenStruct<T> {return nil}
  func foo4<T>(_ arg: T) -> GenEnum<T> {return nil}
  // expected-error@-4 {{'nil' is incompatible with return type 'Int'}}
  // expected-error@-4 {{'nil' is incompatible with return type 'GenClass<T>'}}
  // expected-error@-4 {{'nil' is incompatible with return type 'GenStruct<T>'}}
  // expected-error@-4 {{'nil' is incompatible with return type 'GenEnum<T>'}}

  let clsr1: () -> Int = {return nil}
  let clsr2: () -> GenClass<Bool> = {return nil}
  let clsr3: () -> GenStruct<String> = {return nil}
  let clsr4: () -> GenEnum<Double?> = {return nil}
  // expected-error@-4 {{'nil' is not compatible with closure result type 'Int'}}
  // expected-error@-4 {{'nil' is not compatible with closure result type 'GenClass<Bool>'}}
  // expected-error@-4 {{'nil' is not compatible with closure result type 'GenStruct<String>'}}
  // expected-error@-4 {{'nil' is not compatible with closure result type 'GenEnum<Double?>'}}

  var number = 0
  var genClassBool = GenClass<Bool>()
  var funcFoo1 = foo1

  number = nil
  genClassBool = nil
  funcFoo1 = nil
  // expected-error@-3 {{'nil' cannot be assigned to type 'Int'}}
  // expected-error@-3 {{'nil' cannot be assigned to type 'GenClass<Bool>'}}
  // expected-error@-3 {{'nil' cannot be assigned to type '(Bool) -> Int'}}
}

// <rdar://problem/21684487> QoI: invalid operator use inside a closure reported as a problem with the closure
typealias MyClosure = ([Int]) -> Bool
func r21684487() {
  var closures = Array<MyClosure>()
  let testClosure = {(list: [Int]) -> Bool in return true}
  
  let closureIndex = closures.index{$0 === testClosure} // expected-error {{cannot check reference equality of functions;}}
}

// <rdar://problem/18397777> QoI: special case comparisons with nil
func r18397777(_ d : r21447318?) {
  let c = r21447318()

  if c != nil { // expected-warning {{comparing non-optional value of type 'r21447318' to 'nil' always returns true}}
  }
  
  if d {  // expected-error {{optional type 'r21447318?' cannot be used as a boolean; test for '!= nil' instead}} {{6-6=(}} {{7-7= != nil)}}
  }
  
  if !d { // expected-error {{optional type 'r21447318?' cannot be used as a boolean; test for '== nil' instead}} {{6-7=}} {{7-7=(}} {{8-8= == nil)}}

  }

  if !Optional(c) { // expected-error {{optional type 'Optional<r21447318>' cannot be used as a boolean; test for '== nil' instead}} {{6-7=}} {{7-7=(}} {{18-18= == nil)}}
  }
}


// <rdar://problem/22255907> QoI: bad diagnostic if spurious & in argument list
func r22255907_1<T>(_ a : T, b : Int) {}
func r22255907_2<T>(_ x : Int, a : T, b: Int) {}

func reachabilityForInternetConnection() {
  var variable: Int = 42
  r22255907_1(&variable, b: 2) // expected-error {{'&' used with non-inout argument of type 'Int'}} {{15-16=}}
  r22255907_2(1, a: &variable, b: 2)// expected-error {{'&' used with non-inout argument of type 'Int'}} {{21-22=}}
}

// <rdar://problem/21601687> QoI: Using "=" instead of "==" in if statement leads to incorrect error message
if i = 6 { } // expected-error {{use of '=' in a boolean context, did you mean '=='?}} {{6-7===}}

_ = (i = 6) ? 42 : 57 // expected-error {{use of '=' in a boolean context, did you mean '=='?}} {{8-9===}}


// <rdar://problem/22263468> QoI: Not producing specific argument conversion diagnostic for tuple init
func r22263468(_ a : String?) {
  typealias MyTuple = (Int, String)
  // TODO(diagnostics): This is a regression from diagnosing missing optional unwrap for `a`, we have to
  // re-think the way errors in tuple elements are detected because it's currently impossible to detect
  // exactly what went wrong here and aggregate fixes for different elements at the same time.
  _ = MyTuple(42, a) // expected-error {{tuple type '(Int, String?)' is not convertible to tuple type 'MyTuple' (aka '(Int, String)')}}
}

func testTupleConstructionInOutArg() {
  typealias II = (Int, Int)

  var i = 0
  _ = (Int, Int)(&i, 0) // expected-error {{'&' may only be used to pass an argument to inout parameter}}
  _ = II(&i, 0) // expected-error {{'&' may only be used to pass an argument to inout parameter}}
  _ = II(&i, &i) // expected-error 2{{'&' may only be used to pass an argument to inout parameter}}
}

// rdar://71829040 - "ambiguous without more context" error for tuple type mismatch.
func r71829040() {
  func object(forKey: String) -> Any? { nil }

  let flags: [String: String]
  // expected-error@+1 {{tuple type '(String, Bool)' is not convertible to tuple type '(String, String)'}}
  flags = Dictionary(uniqueKeysWithValues: ["keyA", "keyB"].map { ($0, object(forKey: $0) as? Bool ?? false) })
}

// rdar://22470302 - Crash with parenthesized call result.
class r22470302Class {
  func f() {}
}

func r22470302(_ c: r22470302Class) {
  print((c.f)(c))  // expected-error {{argument passed to call that takes no arguments}}
}



// <rdar://problem/21928143> QoI: Pointfree reference to generic initializer in generic context does not compile
extension String {
  @available(*, unavailable, message: "calling this is unwise")
  func unavail<T : Sequence> // expected-note {{'unavail' has been explicitly marked unavailable here}}
    (_ a : T) -> String where T.Iterator.Element == String {}
}
extension Array {
  func g() -> String {
    return "foo".unavail([""])  // expected-error {{'unavail' is unavailable: calling this is unwise}}
  }
  
  func h() -> String {
    return "foo".unavail([0])  // expected-error {{cannot convert value of type 'Int' to expected element type 'String'}}
  }
}

// <rdar://problem/22519983> QoI: Weird error when failing to infer archetype
func safeAssign<T: RawRepresentable>(_ lhs: inout T) -> Bool {}
// expected-note @-1 {{in call to function 'safeAssign'}}
let a = safeAssign // expected-error {{generic parameter 'T' could not be inferred}}

// <rdar://problem/21692808> QoI: Incorrect 'add ()' fixit with trailing closure
struct Radar21692808<Element> {
  init(count: Int, value: Element) {} // expected-note {{'init(count:value:)' declared here}}
}
func radar21692808() -> Radar21692808<Int> {
  return Radar21692808<Int>(count: 1) { // expected-error {{trailing closure passed to parameter of type 'Int' that does not accept a closure}}
    return 1
  }
}

// <rdar://problem/17557899> - This shouldn't suggest calling with ().
func someOtherFunction() {}
func someFunction() -> () {
  // Producing an error suggesting that this
  return someOtherFunction  // expected-error {{unexpected non-void return value in void function}}
}

// <rdar://problem/23560128> QoI: trying to mutate an optional dictionary result produces bogus diagnostic
func r23560128() {
  var a : (Int,Int)?
  a.0 = 42 // expected-error{{value of optional type '(Int, Int)?' must be unwrapped to refer to member '0' of wrapped base type '(Int, Int)'}}
  // expected-note@-1{{chain the optional }}
}

// <rdar://problem/21890157> QoI: wrong error message when accessing properties on optional structs without unwrapping
struct ExampleStruct21890157 {
  var property = "property"
}
var example21890157: ExampleStruct21890157?
example21890157.property = "confusing"  // expected-error {{value of optional type 'ExampleStruct21890157?' must be unwrapped to refer to member 'property' of wrapped base type 'ExampleStruct21890157'}}
  // expected-note@-1{{chain the optional }}


struct UnaryOp {}
_ = -UnaryOp() // expected-error {{unary operator '-' cannot be applied to an operand of type 'UnaryOp'}}

// <rdar://problem/23433271> Swift compiler segfault in failure diagnosis
func f23433271(_ x : UnsafePointer<Int>) {}
func segfault23433271(_ a : UnsafeMutableRawPointer) {
  f23433271(a[0])  // expected-error {{value of type 'UnsafeMutableRawPointer' has no subscripts}}
}



// <rdar://problem/23272739> Poor diagnostic due to contextual constraint
func r23272739(_ contentType: String) {
  let actualAcceptableContentTypes: Set<String> = []
  return actualAcceptableContentTypes.contains(contentType)  
  // expected-error@-1 {{unexpected non-void return value in void function}}
  // expected-note@-2 {{did you mean to add a return type?}}
}

// <rdar://problem/23641896> QoI: Strings in Swift cannot be indexed directly with integer offsets
func r23641896() {
  var g = "Hello World"
  g.replaceSubrange(0...2, with: "ce")  // expected-error {{cannot convert value of type 'ClosedRange<Int>' to expected argument type 'Range<String.Index>'}}

  _ = g[12]  // expected-error {{'subscript(_:)' is unavailable: cannot subscript String with an Int, use a String.Index instead.}}

}


// <rdar://problem/23718859> QoI: Incorrectly flattening ((Int,Int)) argument list to (Int,Int) when printing note
func test17875634() {
  var match: [(Int, Int)] = []
  var row = 1
  var col = 2
  
  match.append(row, col)  // expected-error {{instance method 'append' expects a single parameter of type '(Int, Int)'}} {{16-16=(}} {{24-24=)}}
}

// <https://github.com/apple/swift/pull/1205> Improved diagnostics for enums with associated values
enum AssocTest {
  case one(Int)
}

// FIXME(rdar://problem/65688291) - on iOS simulator this diagnostic is flaky,
// either `referencing operator function '==' on 'Equatable'` or `operator function '==' requires`
if AssocTest.one(1) == AssocTest.one(1) {} // expected-error{{requires that 'AssocTest' conform to 'Equatable'}}
// expected-note @-1 {{binary operator '==' cannot be synthesized for enums with associated values}}


// <rdar://problem/24251022> Swift 2: Bad Diagnostic Message When Adding Different Integer Types
func r24251022() {
  var a = 1
  var b: UInt32 = 2
  _ = a + b // expected-error {{binary operator '+' cannot be applied to operands of type 'Int' and 'UInt32'}} expected-note {{overloads for '+' exist with these partially matching parameter lists: (Int, Int), (UInt32, UInt32)}}
  a += a +
    b // expected-error {{cannot convert value of type 'UInt32' to expected argument type 'Int'}}
  a += b  // expected-error@:8 {{cannot convert value of type 'UInt32' to expected argument type 'Int'}}
}

func overloadSetResultType(_ a : Int, b : Int) -> Int {
  // https://twitter.com/_jlfischer/status/712337382175952896
  // TODO: <rdar://problem/27391581> QoI: Nonsensical "binary operator '&&' cannot be applied to two 'Bool' operands"
  return a == b && 1 == 2  // expected-error {{cannot convert return expression of type 'Bool' to return type 'Int'}}
}

postfix operator +++
postfix func +++ <T>(_: inout T) -> T { fatalError() }

// <rdar://problem/21523291> compiler error message for mutating immutable field is incorrect
func r21523291(_ bytes : UnsafeMutablePointer<UInt8>) {
  let i = 42 // expected-note {{change 'let' to 'var' to make it mutable}}

  _ = bytes[i+++]  // expected-error {{cannot pass immutable value to mutating operator: 'i' is a 'let' constant}}
}


// https://github.com/apple/swift/issues/44203
// Wrong error description when using '===' on non-class types
class C_44203 {
  func f(bytes : UnsafeMutablePointer<Int>, _ i : Int?) {
    _ = (i === nil) // expected-error {{value of type 'Int?' cannot be compared by reference; did you mean to compare by value?}} {{12-15===}}
    _ = (bytes === nil) // expected-error {{type 'UnsafeMutablePointer<Int>' is not optional, value can never be nil}}
    _ = (self === nil) // expected-warning {{comparing non-optional value of type 'AnyObject' to 'nil' always returns false}}
    _ = (self === .none) // expected-warning {{comparing non-optional value of type 'AnyObject' to 'Optional.none' always returns false}}
    _ = (self === Optional.none) // expected-warning {{comparing non-optional value of type 'AnyObject' to 'Optional.none' always returns false}}
    _ = (i !== nil) // expected-error {{value of type 'Int?' cannot be compared by reference; did you mean to compare by value?}} {{12-15=!=}}
    _ = (bytes !== nil) // expected-error {{type 'UnsafeMutablePointer<Int>' is not optional, value can never be nil}}
    _ = (self !== nil) // expected-warning {{comparing non-optional value of type 'AnyObject' to 'nil' always returns true}}
    _ = (self !== .none) // expected-warning {{comparing non-optional value of type 'AnyObject' to 'Optional.none' always returns true}}
    _ = (self !== Optional.none) // expected-warning {{comparing non-optional value of type 'AnyObject' to 'Optional.none' always returns true}}
  }
}

func nilComparison(i: Int, o: AnyObject) {
  _ = i == nil // expected-warning {{comparing non-optional value of type 'Int' to 'nil' always returns false}}
  _ = nil == i // expected-warning {{comparing non-optional value of type 'Int' to 'nil' always returns false}}
  _ = i != nil // expected-warning {{comparing non-optional value of type 'Int' to 'nil' always returns true}}
  _ = nil != i // expected-warning {{comparing non-optional value of type 'Int' to 'nil' always returns true}}

  _ = i == Optional.none // expected-warning {{comparing non-optional value of type 'Int' to 'Optional.none' always returns false}}
  _ = Optional.none == i // expected-warning {{comparing non-optional value of type 'Int' to 'Optional.none' always returns false}}
  _ = i != Optional.none // expected-warning {{comparing non-optional value of type 'Int' to 'Optional.none' always returns true}}
  _ = Optional.none != i // expected-warning {{comparing non-optional value of type 'Int' to 'Optional.none' always returns true}}
  
  // FIXME(integers): uncomment these tests once the < is no longer ambiguous
  // _ = i < nil  // _xpected-error {{type 'Int' is not optional, value can never be nil}}
  // _ = nil < i  // _xpected-error {{type 'Int' is not optional, value can never be nil}}
  // _ = i <= nil // _xpected-error {{type 'Int' is not optional, value can never be nil}}
  // _ = nil <= i // _xpected-error {{type 'Int' is not optional, value can never be nil}}
  // _ = i > nil  // _xpected-error {{type 'Int' is not optional, value can never be nil}}
  // _ = nil > i  // _xpected-error {{type 'Int' is not optional, value can never be nil}}
  // _ = i >= nil // _xpected-error {{type 'Int' is not optional, value can never be nil}}
  // _ = nil >= i // _xpected-error {{type 'Int' is not optional, value can never be nil}}

  _ = o === nil // expected-warning {{comparing non-optional value of type 'AnyObject' to 'nil' always returns false}}
  _ = o !== nil // expected-warning {{comparing non-optional value of type 'AnyObject' to 'nil' always returns true}}
}

// <rdar://problem/23709100> QoI: incorrect ambiguity error due to implicit conversion
func testImplConversion(a : Float?) -> Bool {}
func testImplConversion(a : Int?) -> Bool {
  let someInt = 42
  let a : Int = testImplConversion(someInt) // expected-error {{missing argument label 'a:' in call}} {{36-36=a: }}
  // expected-error@-1 {{cannot convert value of type 'Bool' to specified type 'Int'}}
}

// <rdar://problem/23752537> QoI: Bogus error message: Binary operator '&&' cannot be applied to two 'Bool' operands
class Foo23752537 {
  var title: String?
  var message: String?
}

extension Foo23752537 {
  func isEquivalent(other: Foo23752537) {
    // TODO: <rdar://problem/27391581> QoI: Nonsensical "binary operator '&&' cannot be applied to two 'Bool' operands"
    // expected-error@+1 {{unexpected non-void return value in void function}} 
    return (self.title != other.title && self.message != other.message) // expected-note {{did you mean to add a return type?}}
  }
}

// <rdar://problem/27391581> QoI: Nonsensical "binary operator '&&' cannot be applied to two 'Bool' operands"
func rdar27391581(_ a : Int, b : Int) -> Int {
  return a == b && b != 0
  // expected-error @-1 {{cannot convert return expression of type 'Bool' to return type 'Int'}}
}

// <rdar://problem/22276040> QoI: not great error message with "withUnsafePointer" sametype constraints
func read2(_ p: UnsafeMutableRawPointer, maxLength: Int) {}
func read<T : BinaryInteger>() -> T? {
  var buffer : T 
  let n = withUnsafeMutablePointer(to: &buffer) { (p) in
    read2(UnsafePointer(p), maxLength: MemoryLayout<T>.size) // expected-error {{cannot convert value of type 'UnsafePointer<T>' to expected argument type 'UnsafeMutableRawPointer'}}
  }
}

func f23213302() {
  var s = Set<Int>()
  s.subtract(1) // expected-error {{cannot convert value of type 'Int' to expected argument type 'Set<Int>'}}
}

// <rdar://problem/24202058> QoI: Return of call to overloaded function in void-return context
func rdar24202058(a : Int) {
  return a <= 480 
  // expected-error@-1 {{unexpected non-void return value in void function}}
  // expected-note@-2 {{did you mean to add a return type?}}
}

// https://github.com/apple/swift/issues/44361
// Warning about unused result with ternary operator
do {
  struct S {
    func foo() {}
  }

  let x: S?

  // Don't generate a warning about unused result since 'foo' returns 'Void'.
  true ? nil : x?.foo()
}

// Make sure RawRepresentable fix-its don't crash in the presence of type variables
class NSCache<K, V> {
  func object(forKey: K) -> V? {}
}

class CacheValue {
  func value(x: Int) -> Int {} // expected-note {{found candidate with type '(Int) -> Int'}}
  func value(y: String) -> String {} // expected-note {{found candidate with type '(String) -> String'}}
}

func valueForKey<K>(_ key: K) -> CacheValue? {
  let cache = NSCache<K, CacheValue>()
  return cache.object(forKey: key)?.value // expected-error {{no exact matches in reference to instance method 'value'}}
}

// https://github.com/apple/swift/issues/43863
do {
  func f1() {
    return true || false
    // expected-error@-1 {{unexpected non-void return value in void function}}
    // expected-note@-2 {{did you mean to add a return type?}}
  }
  func f2() -> Int {
    return true || false // expected-error {{cannot convert return expression of type 'Bool' to return type 'Int'}}
  }

  // Diagnostic message for initialization with binary operations as right side.
  let _: String = 1 + 2 + 3 // expected-error {{cannot convert value of type 'Int' to specified type 'String'}}
  let _: Dictionary<String, String> = ["hello": 1 + 2] // expected-error {{cannot convert value of type 'Int' to expected dictionary value type 'String'}}
  let _: Dictionary<String, String> = [(1 + 2): "world"] // expected-error {{cannot convert value of type 'Int' to expected dictionary key type 'String'}}
  let _: [String] = [1 + 2 + 3] // expected-error {{cannot convert value of type 'Int' to expected element type 'String'}}
}

// https://github.com/apple/swift/issues/44815
do {
  struct S {
    func bar(value: UInt) {}
  }

  let foo = S()
  let a: Int = 1
  let b: Int = 2
  let result = a / b
  foo.bar(value: a / b)  // expected-error {{cannot convert value of type 'Int' to expected argument type 'UInt'}}
  foo.bar(value: result) // expected-error {{cannot convert value of type 'Int' to expected argument type 'UInt'}}
  foo.bar(value: UInt(result)) // Ok
}

// https://github.com/apple/swift/issues/44772
// Erroneous diagnostic when unable to infer generic type
do {
  struct S<A, B> { // expected-note 4 {{'B' declared as parameter to type 'S'}} expected-note 2 {{'A' declared as parameter to type 'S'}} expected-note * {{generic type 'S' declared here}}
    init(a: A) {}
    init(b: B) {}
    init(c: Int) {}
    init(_ d: A) {}
    init(e: A?) {}
  }

  struct S_Array<A, B> { // expected-note {{'B' declared as parameter to type 'S_Array'}} expected-note * {{generic type 'S_Array' declared here}}
    init(_ a: [A]) {}
  }

  struct S_Dict<A: Hashable, B> { // expected-note {{'B' declared as parameter to type 'S_Dict'}} expected-note * {{generic type 'S_Dict' declared here}}
    init(a: [A: Double]) {}
  }

  S(a: 0) // expected-error {{generic parameter 'B' could not be inferred}} expected-note {{explicitly specify the generic arguments to fix this issue}}
  S(b: 1) // expected-error {{generic parameter 'A' could not be inferred}} expected-note {{explicitly specify the generic arguments to fix this issue}}
  S(c: 2)
  // expected-error@-1 {{generic parameter 'A' could not be inferred}}
  // expected-error@-2 {{generic parameter 'B' could not be inferred}}
  // expected-note@-3 {{explicitly specify the generic arguments to fix this issue}} {{4-4=<Any, Any>}}
  S(3) // expected-error {{generic parameter 'B' could not be inferred}} expected-note {{explicitly specify the generic arguments to fix this issue}}
  S_Array([4]) // expected-error {{generic parameter 'B' could not be inferred}} expected-note {{explicitly specify the generic arguments to fix this issue}}
  S(e: 5) // expected-error {{generic parameter 'B' could not be inferred}} expected-note {{explicitly specify the generic arguments to fix this issue}}
  S_Dict(a: ["pi": 3.14]) // expected-error {{generic parameter 'B' could not be inferred}} expected-note {{explicitly specify the generic arguments to fix this issue}}
  S<Int>(a: 0) // expected-error {{generic type 'S' specialized with too few type parameters (got 1, but expected 2)}}
  S<Int>(b: 1) // expected-error {{generic type 'S' specialized with too few type parameters (got 1, but expected 2)}}
  let _ = S<Int, Bool>(a: 0)    // Ok
  let _ = S<Int, Bool>(b: true) // Ok
  S<Int, Bool, Float>(a: 0) // expected-error {{generic type 'S' specialized with too many type parameters (got 3, but expected 2)}}
  S<Int, Bool, Float>(b: 0) // expected-error {{generic type 'S' specialized with too many type parameters (got 3, but expected 2)}}
}

// <rdar://problem/29850459> Swift compiler misreports type error in ternary expression

let r29850459_flag = true
let r29850459_a: Int = 0
let r29850459_b: Int = 1
func r29850459() -> Bool { return false }
let _ = (r29850459_flag ? r29850459_a : r29850459_b) + 42.0 // expected-error {{binary operator '+' cannot be applied to operands of type 'Int' and 'Double'}}
// expected-note@-1 {{overloads for '+' exist with these partially matching parameter lists: (Double, Double), (Int, Int)}}
let _ = ({ true }() ? r29850459_a : r29850459_b) + 42.0 // expected-error {{binary operator '+' cannot be applied to operands of type 'Int' and 'Double'}}
// expected-note@-1 {{overloads for '+' exist with these partially matching parameter lists: (Double, Double), (Int, Int)}}
let _ = (r29850459() ? r29850459_a : r29850459_b) + 42.0 // expected-error {{binary operator '+' cannot be applied to operands of type 'Int' and 'Double'}}
// expected-note@-1 {{overloads for '+' exist with these partially matching parameter lists: (Double, Double), (Int, Int)}}
let _ = ((r29850459_flag || r29850459()) ? r29850459_a : r29850459_b) + 42.0 // expected-error {{binary operator '+' cannot be applied to operands of type 'Int' and 'Double'}}
// expected-note@-1 {{overloads for '+' exist with these partially matching parameter lists: (Double, Double), (Int, Int)}}

// https://github.com/apple/swift/issues/48822
// Tailored diagnostics with fixits for numerical conversions

do {
  enum Foo: Int {
    case bar
  }

  // expected-error@+1 {{cannot convert value of type 'Float' to expected argument type 'Int'}} {{35-35=Int(}} {{43-43=)}}
  let _: Int = Foo.bar.rawValue * Float(0)

  // expected-error@+1 {{cannot convert value of type 'Int' to expected argument type 'Float'}} {{18-18=Float(}} {{34-34=)}}
  let _: Float = Foo.bar.rawValue * Float(0)

  // expected-error@+2 {{binary operator '*' cannot be applied to operands of type 'Int' and 'Float'}} {{none}}
  // expected-note@+1 {{overloads for '*' exist with these partially matching parameter lists: (Float, Float), (Int, Int)}}
  Foo.bar.rawValue * Float(0)
}
do {
  let lhs = Float(3)
  let rhs = Int(0)

  // expected-error@+1 {{cannot convert value of type 'Int' to expected argument type 'Float'}} {{24-24=Float(}} {{27-27=)}}
  let _: Float = lhs * rhs

  // expected-error@+1 {{cannot convert value of type 'Float' to expected argument type 'Int'}} {{16-16=Int(}} {{19-19=)}}
  let _: Int = lhs * rhs

  // expected-error@+2 {{binary operator '*' cannot be applied to operands of type 'Float' and 'Int'}} {{none}}
  // expected-note@+1 {{overloads for '*' exist with these partially matching parameter lists: (Float, Float), (Int, Int)}}
  lhs * rhs
}
do {
  // expected-error@+1 {{cannot convert value of type 'String' to expected argument type 'Int'}} {{none}}
  Int(3) * "0"

  struct S {}
  // expected-error@+1 {{cannot convert value of type 'S' to expected argument type 'Int'}} {{none}}
  Int(10) * S()
}

// FIXME: Operator lookup does not reach local types, so this must be a
// top-level struct (https://github.com/apple/swift/issues/51378).
struct S_48822: ExpressibleByIntegerLiteral {
  typealias IntegerLiteralType = Int
  init(integerLiteral: Int) {}
  static func +(lhs: S_48822, rhs: Int) -> Float { return 42.0 }
}
do {
  let x: Float = 1.0

  // expected-error@+2 {{binary operator '+' cannot be applied to operands of type 'S_48822' and 'Float'}} {{none}}
  // expected-note@+1 {{overloads for '+' exist with these partially matching parameter lists: (Float, Float), (S_48822, Int)}}
  let _: Float = S_48822(integerLiteral: 42) + x

  // expected-error@+1 {{cannot convert value of type 'Double' to expected argument type 'Int'}} {{48-48=Int(}} {{52-52=)}}
  let _: Float = S_48822(integerLiteral: 42) + 42.0

  // expected-error@+2 {{binary operator '+' cannot be applied to operands of type 'S_48822' and 'Float'}} {{none}}
  // expected-note@+1 {{overloads for '+' exist with these partially matching parameter lists: (Float, Float), (S_48822, Int)}}
  let _: Float = S_48822(integerLiteral: 42) + x + 1.0
}

// Ambiguous overload inside a trailing closure

func ambiguousCall() -> Int {} // expected-note {{found this candidate}}
func ambiguousCall() -> Float {} // expected-note {{found this candidate}}

func takesClosure(fn: () -> ()) {}

takesClosure() { ambiguousCall() } // expected-error {{ambiguous use of 'ambiguousCall()'}}

// https://github.com/apple/swift/issues/47269
// Useless diagnostics calling non-static method

class C1_47269 {
  private static func foo(x: Int, y: Bool) {
    self.bar(x: x)
    // expected-error@-1 {{instance member 'bar' cannot be used on type 'C1_47269'}}
  }

  private func bar(x: Int) {
  }
}

class C2_47269 {
  static func a() {
    self.f(x: 3, y: true)
    // expected-error@-1 {{instance member 'f' cannot be used on type 'C2_47269'}}
  }

  private func f(a: Int, b: Bool, c: String) {
    self.f(x: a, y: b)
  }

  private func f(x: Int, y: Bool) {
  }
}

// rdar://problem/32390726 - Bad Diagnostic: Don't suggest `var` to `let` when binding inside for-statement
for var i in 0..<10 { // expected-warning {{variable 'i' was never mutated; consider removing 'var' to make it constant}} {{5-9=}}
  _ = i + 1
}

// https://github.com/apple/swift/issues/47621
// Attempting to return result of 'reduce(_:_:)' in a method with no return
// produces ambiguous error
func f_47621() {
  let doubles: [Double] = [1, 2, 3]
  return doubles.reduce(0, +)
  // expected-error@-1 {{unexpected non-void return value in void function}}
  // expected-note@-2 {{did you mean to add a return type?}}
}

// rdar://problem/32934129 - QoI: misleading diagnostic
class L_32934129<T : Comparable> {
  init(_ value: T) { self.value = value }
  init(_ value: T, _ next: L_32934129<T>?) {
    self.value = value
    self.next = next
  }

  var value: T
  var next: L_32934129<T>? = nil

  func length() -> Int {
    func inner(_ list: L_32934129<T>?, _ count: Int) {
    guard let list = list else { return count } 
      // expected-error@-1 {{unexpected non-void return value in void function}}
      // expected-note@-2 {{did you mean to add a return type?}}
      return inner(list.next, count + 1)
    }

    return inner(self, 0) // expected-error {{cannot convert return expression of type '()' to return type 'Int'}}
  }
}

// rdar://problem/31671195 - QoI: erroneous diagnostic - cannot call value of non-function type

class C_31671195 {
  var name: Int { fatalError() }
  func name(_: Int) { fatalError() }
}
C_31671195().name(UInt(0))
// expected-error@-1 {{cannot convert value of type 'UInt' to expected argument type 'Int'}}


// rdar://problem/28456467 - QoI: erroneous diagnostic - cannot call value of non-function type

class AST_28456467 {
  var hasStateDef: Bool { return false }
}

protocol Expr_28456467 {}

class ListExpr_28456467 : AST_28456467, Expr_28456467 {
  let elems: [Expr_28456467]

  init(_ elems:[Expr_28456467] ) {
    self.elems = elems
  }

  override var hasStateDef: Bool {
    return elems.first(where: { $0.hasStateDef }) != nil
    // expected-error@-1 {{value of type 'any Expr_28456467' has no member 'hasStateDef'}}
  }
}

// https://github.com/apple/swift/issues/47657
do {
  var a = ["1", "2", "3", "4", "5"]
  var b = [String]()
  b = a[2...4] // expected-error {{cannot assign value of type 'ArraySlice<String>' to type '[String]'}}
}

// TODO(diagnostics):Figure out what to do when expressions are complex and completely broken
func rdar17170728() {
  var i: Int? = 1
  var j: Int?
  var k: Int? = 2

  let _ = [i, j, k].reduce(0 as Int?) {
    $0 && $1 ? $0! + $1! : ($0 ? $0! : ($1 ? $1! : nil))
    // expected-error@-1 4 {{optional type 'Int?' cannot be used as a boolean; test for '!= nil' instead}}
  }

  let _ = [i, j, k].reduce(0 as Int?) { // expected-error {{missing argument label 'into:' in call}}
    // expected-error@-1 {{cannot convert value of type 'Int?' to expected argument type '(inout @escaping (Bool, Bool) -> Bool?, Int?) throws -> ()'}}
    $0 && $1 ? $0 + $1 : ($0 ? $0 : ($1 ? $1 : nil))
    // expected-error@-1 {{binary operator '+' cannot be applied to two 'Bool' operands}}
  }
}

// https://github.com/apple/swift/issues/48493
// Failure to emit diagnostic for bad generic constraints

func elephant<T, U>(_: T) where T : Collection, T.Element == U, T.Element : Hashable {} // expected-note {{where 'U' = 'T'}}

func platypus<T>(a: [T]) {
    _ = elephant(a) // expected-error {{global function 'elephant' requires that 'T' conform to 'Hashable'}}
}

// Another case of the above.
func badTypes() {
  let sequence:AnySequence<[Int]> = AnySequence() { AnyIterator() { [3] }}
  // Notes, attached to declarations, explain that there is a difference between Array.init(_:) and
  // RangeReplaceableCollection.init(_:) which are both applicable in this case.
  let array = [Int](sequence)
  // expected-error@-1 {{no exact matches in call to initializer}}
}

// rdar://34357545
func unresolvedTypeExistential() -> Bool {
  return (Int.self==_{})
  // expected-error@-1 {{could not infer type for placeholder}}
  // expected-error@-2 {{type placeholder not allowed here}}
}

do {
  struct Array {}
  let foo: Swift.Array = Array() // expected-error {{cannot convert value of type 'Array' to specified type 'Array<Element>'}}
  // expected-error@-1 {{generic parameter 'Element' could not be inferred}}

  struct Error {}
  let bar: Swift.Error = Error() //expected-error {{value of type 'diagnostics.Error' does not conform to specified type 'Swift.Error'}}
  let baz: (Swift.Error) = Error() //expected-error {{value of type 'diagnostics.Error' does not conform to specified type 'Swift.Error'}}
  let baz2: Swift.Error = (Error()) //expected-error {{value of type 'diagnostics.Error' does not conform to specified type 'Swift.Error'}}
  let baz3: (Swift.Error) = (Error()) //expected-error {{value of type 'diagnostics.Error' does not conform to specified type 'Swift.Error'}}
  let baz4: ((Swift.Error)) = (Error()) //expected-error {{value of type 'diagnostics.Error' does not conform to specified type 'Swift.Error'}}
}

// SyntaxSugarTypes with unresolved types
func takesGenericArray<T>(_ x: [T]) {}
takesGenericArray(1) // expected-error {{cannot convert value of type 'Int' to expected argument type '[Int]'}}
func takesNestedGenericArray<T>(_ x: [[T]]) {}
takesNestedGenericArray(1) // expected-error {{cannot convert value of type 'Int' to expected argument type '[[Int]]'}}
func takesSetOfGenericArrays<T>(_ x: Set<[T]>) {}
takesSetOfGenericArrays(1) // expected-error {{cannot convert value of type 'Int' to expected argument type 'Set<[Int]>'}}
func takesArrayOfSetOfGenericArrays<T>(_ x: [Set<[T]>]) {}
takesArrayOfSetOfGenericArrays(1) // expected-error {{cannot convert value of type 'Int' to expected argument type '[Set<[Int]>]'}}
func takesArrayOfGenericOptionals<T>(_ x: [T?]) {}
takesArrayOfGenericOptionals(1) // expected-error {{cannot convert value of type 'Int' to expected argument type '[Int?]'}}
func takesGenericDictionary<T, U>(_ x: [T : U]) {}  // expected-note {{in call to function 'takesGenericDictionary'}}
takesGenericDictionary(true) // expected-error {{cannot convert value of type 'Bool' to expected argument type '[T : U]'}}
// expected-error@-1 {{generic parameter 'T' could not be inferred}}
// expected-error@-2 {{generic parameter 'U' could not be inferred}}
typealias Z = Int
func takesGenericDictionaryWithTypealias<T>(_ x: [T : Z]) {} // expected-note {{in call to function 'takesGenericDictionaryWithTypealias'}}
takesGenericDictionaryWithTypealias(true) // expected-error {{cannot convert value of type 'Bool' to expected argument type '[T : Z]'}}
// expected-error@-1 {{generic parameter 'T' could not be inferred}}
func takesGenericFunction<T>(_ x: ([T]) -> Void) {} // expected-note {{in call to function 'takesGenericFunction'}}
takesGenericFunction(true) // expected-error {{cannot convert value of type 'Bool' to expected argument type '([T]) -> Void'}}
// expected-error@-1 {{generic parameter 'T' could not be inferred}}
func takesTuple<T>(_ x: ([T], [T])) {} // expected-note {{in call to function 'takesTuple'}}
takesTuple(true) // expected-error {{cannot convert value of type 'Bool' to expected argument type '([T], [T])'}}
// expected-error@-1 {{generic parameter 'T' could not be inferred}}

// Void function returns non-void result fix-it

func voidFunc() {
  return 1 
  // expected-error@-1 {{unexpected non-void return value in void function}}
  // expected-note@-2 {{did you mean to add a return type?}}{{-1:16-16= -> <#Return Type#>}}
}

func voidFuncWithArgs(arg1: Int) {
  return 1 
  // expected-error@-1 {{unexpected non-void return value in void function}}
  // expected-note@-2 {{did you mean to add a return type?}}{{-1:33-33= -> <#Return Type#>}}
}

func voidFuncWithCondFlow() {
  if Bool.random() {
    return 1
    // expected-error@-1 {{unexpected non-void return value in void function}}
    // expected-note@-2 {{did you mean to add a return type?}}{{-2:28-28= -> <#Return Type#>}}
  } else {
    return 2
    // expected-error@-1 {{unexpected non-void return value in void function}}
    // expected-note@-2 {{did you mean to add a return type?}}{{-6:28-28= -> <#Return Type#>}}
  }
}

func voidFuncWithNestedVoidFunc() {
  func nestedVoidFunc() {
    return 1
    // expected-error@-1 {{unexpected non-void return value in void function}}
    // expected-note@-2 {{did you mean to add a return type?}}{{-1:24-24= -> <#Return Type#>}}
  }
}

func voidFuncWithEffects1() throws {
  return 1
  // expected-error@-1 {{unexpected non-void return value in void function}}
  // expected-note@-2 {{did you mean to add a return type?}}{{-1:35-35= -> <#Return Type#>}}
}

@available(SwiftStdlib 5.5, *)
func voidFuncWithEffects2() async throws {
  return 1
  // expected-error@-1 {{unexpected non-void return value in void function}}
  // expected-note@-2 {{did you mean to add a return type?}}{{-1:41-41= -> <#Return Type#>}}
}

@available(SwiftStdlib 5.5, *)
// expected-error@+1 {{'async' must precede 'throws'}}
func voidFuncWithEffects3() throws async {
  return 1
  // expected-error@-1 {{unexpected non-void return value in void function}}
  // expected-note@-2 {{did you mean to add a return type?}}{{-1:41-41= -> <#Return Type#>}}
}

@available(SwiftStdlib 5.5, *)
func voidFuncWithEffects4() async {
  return 1
  // expected-error@-1 {{unexpected non-void return value in void function}}
  // expected-note@-2 {{did you mean to add a return type?}}{{-1:34-34= -> <#Return Type#>}}
}

func voidFuncWithEffects5(_ closure: () throws -> Void) rethrows {
  return 1
  // expected-error@-1 {{unexpected non-void return value in void function}}
  // expected-note@-2 {{did you mean to add a return type?}}{{-1:65-65= -> <#Return Type#>}}
}

@available(SwiftStdlib 5.5, *)
func voidGenericFuncWithEffects<T>(arg: T) async where T: CustomStringConvertible {
  return 1
  // expected-error@-1 {{unexpected non-void return value in void function}}
  // expected-note@-2 {{did you mean to add a return type?}}{{-1:49-49= -> <#Return Type#>}}
}

// Special cases: These should not offer a note + fix-it

func voidFuncExplicitType() -> Void {
  return 1 // expected-error {{unexpected non-void return value in void function}}
}

class ClassWithDeinit {
  deinit {
    return 0 // expected-error {{unexpected non-void return value in void function}}
  }
}

class ClassWithVoidProp {
  var propertyWithVoidType: () { return 5 } // expected-error {{unexpected non-void return value in void function}}
}

class ClassWithPropContainingSetter {
  var propWithSetter: Int {
    get { return 0 }
    set { return 1 } // expected-error {{unexpected non-void return value in void function}}
  }
}

// https://github.com/apple/swift/issues/54389

struct Rect {
    let width: Int
    let height: Int
}

struct Frame {
    func rect(width: Int, height: Int) -> Rect {
        Rect(width: width, height: height)
    }

    let rect: Rect
}

func foo(frame: Frame) {
    frame.rect.width + 10.0 // expected-error {{binary operator '+' cannot be applied to operands of type 'Int' and 'Double'}}
    // expected-note@-1 {{overloads for '+' exist with these partially matching parameter lists: (Double, Double), (Int, Int)}}

}

// Make sure we prefer the conformance failure.
func f11(_ n: Int) {}
func f11<T : P2>(_ n: T, _ f: @escaping (T) -> T) {}  // expected-note {{where 'T' = 'Int'}}
f11(3, f4) // expected-error {{global function 'f11' requires that 'Int' conform to 'P2'}}

let f12: (Int) -> Void = { _ in }
func f12<T : P2>(_ n: T, _ f: @escaping (T) -> T) {} // expected-note {{where 'T' = 'Int'}}
f12(3, f4)// expected-error {{global function 'f12' requires that 'Int' conform to 'P2'}}

/// https://github.com/apple/swift/issues/57615
/// Bad diagnostic for `var` + `func` overload with mismatched call
// FIXME: Diagnostic still bad in local scope.
func f_57615(x: Int, y: Int) {}
var f_57615: Any = 0
f_57615(0, x: 0) // expected-error {{incorrect argument labels in call (have '_:x:', expected 'x:y:')}}

// https://github.com/apple/swift/issues/54669

struct S1_54669<Value> {}
struct S2_54669 {}

protocol P_54669 {}

func f_54669() -> S1_54669<[S2_54669]> {}

func genericFunc<S2_54669: P_54669>(_ completion:  @escaping (S1_54669<[S2_54669]>) -> Void) {
  let t = f_54669()
  completion(t) // expected-error {{cannot convert value of type 'diagnostics.S1_54669<[diagnostics.S2_54669]>' to expected argument type 'diagnostics.S1_54669<[S2_54669]>'}}
  // expected-note@-1 {{arguments to generic parameter 'Element' ('diagnostics.S2_54669' and 'S2_54669') are expected to be equal}}
}

func assignGenericMismatch() {
  var a: [Int]?
  var b: [String]

  a = b // expected-error {{cannot assign value of type '[String]' to type '[Int]'}}
  // expected-note@-1 {{arguments to generic parameter 'Element' ('String' and 'Int') are expected to be equal}}

  b = a // expected-error {{cannot assign value of type '[Int]' to type '[String]'}}
  // expected-note@-1 {{arguments to generic parameter 'Element' ('Int' and 'String') are expected to be equal}}
  // expected-error@-2 {{value of optional type '[Int]?' must be unwrapped to a value of type '[Int]'}}
  // expected-note@-3 {{coalesce using '??' to provide a default when the optional value contains 'nil'}}
  // expected-note@-4 {{force-unwrap using '!' to abort execution if the optional value contains 'nil'}}
}

// [Int] to [String]? argument to param conversion
let value: [Int] = []
func gericArgToParamOptional(_ param: [String]?) {}

gericArgToParamOptional(value) // expected-error {{convert value of type '[Int]' to expected argument type '[String]'}}
// expected-note@-1 {{arguments to generic parameter 'Element' ('Int' and 'String') are expected to be equal}}

// Inout Expr conversions
func gericArgToParamInout1(_ x: inout [[Int]]) {}
func gericArgToParamInout2(_ x: inout [[String]]) {
  gericArgToParamInout1(&x) // expected-error {{cannot convert value of type '[[String]]' to expected argument type '[[Int]]'}}
  // expected-note@-1 {{arguments to generic parameter 'Element' ('String' and 'Int') are expected to be equal}}
}

func gericArgToParamInoutOptional(_ x: inout [[String]]?) {
  gericArgToParamInout1(&x) // expected-error {{cannot convert value of type '[[String]]?' to expected argument type '[[Int]]'}}
  // expected-note@-1 {{arguments to generic parameter 'Element' ('String' and 'Int') are expected to be equal}}
  // expected-error@-2 {{value of optional type '[[String]]?' must be unwrapped to a value of type '[[String]]'}}
  // expected-note@-3 {{force-unwrap using '!' to abort execution if the optional value contains 'nil'}}
}

func gericArgToParamInout(_ x: inout [[Int]]) { // expected-note {{change variable type to '[[String]]?' if it doesn't need to be declared as '[[Int]]'}}
  gericArgToParamInoutOptional(&x) // expected-error {{cannot convert value of type '[[Int]]' to expected argument type '[[String]]?'}}
  // expected-note@-1 {{arguments to generic parameter 'Element' ('Int' and 'String') are expected to be equal}}
  // expected-error@-2 {{inout argument could be set to a value with a type other than '[[Int]]'; use a value declared as type '[[String]]?' instead}}
}

// https://github.com/apple/swift/issues/55169
do {
  struct S<E> {} // expected-note {{arguments to generic parameter 'E' ('Int' and 'Double') are expected to be equal}}
  func generic<T>(_ value: inout T, _ closure: (S<T>) -> Void) {}

  let arg: Int
  generic(&arg) { (g: S<Double>) -> Void in } // expected-error {{cannot convert value of type '(S<Double>) -> Void' to expected argument type '(S<Int>) -> Void'}}
}

// rdar://problem/62428353 - bad error message for passing `T` where `inout T` was expected
func rdar62428353<T>(_ t: inout T) {
  let v = t // expected-note {{change 'let' to 'var' to make it mutable}} {{3-6=var}}
  rdar62428353(v) // expected-error {{cannot pass immutable value as inout argument: 'v' is a 'let' constant}}
}

func rdar62989214() {
  struct Flag {
    var isTrue: Bool
  }

  @propertyWrapper @dynamicMemberLookup
  struct Wrapper<Value> {
    var wrappedValue: Value

    subscript<Subject>(
      dynamicMember keyPath: WritableKeyPath<Value, Subject>
    ) -> Wrapper<Subject> {
      get { fatalError() }
    }
  }

  func test(arr: Wrapper<[Flag]>, flag: Flag) {
    arr[flag].isTrue // expected-error {{cannot convert value of type 'Flag' to expected argument type 'Int'}}
  }
}

// https://github.com/apple/swift/issues/48258
do {
  func f1() -> String? {}
  f1!.count // expected-error {{function 'f1' was used as a property; add () to call it}} {{5-5=()}}

  func f2() -> Int? { 0 }
  let _: Int = f2! // expected-error {{function 'f2' was used as a property; add () to call it}} {{18-18=()}}
}


// rdar://74696023 - Fallback error when passing incorrect optional type to `==` operator
func rdar74696023() {
  struct MyError {
    var code: Int = 0
  }

  func test(error: MyError?, code: Int32) {
    guard error?.code == code else { fatalError() } // expected-error {{value of optional type 'Int?' must be unwrapped to a value of type 'Int'}}
    // expected-note@-1 {{coalesce using '??' to provide a default when the optional value contains 'nil'}}
    // expected-note@-2 {{force-unwrap using '!' to abort execution if the optional value contains 'nil'}}
  }
}

extension Int {
  static var optionalIntMember: Int? { 0 }
  static var optionalThrowsMember: Int? { get throws { 0 } }
}

func testUnwrapFixIts(x: Int?) throws {
  let _ = x + 2 // expected-error {{value of optional type 'Int?' must be unwrapped to a value of type 'Int'}}
  // expected-note@-1 {{coalesce using '??' to provide a default when the optional value contains 'nil'}} {{11-11=(}} {{12-12= ?? <#default value#>)}}
  // expected-note@-2 {{force-unwrap using '!' to abort execution if the optional value contains 'nil'}} {{12-12=!}}
  let _ = (x ?? 0) + 2

  let _ = 2 + x // expected-error {{value of optional type 'Int?' must be unwrapped to a value of type 'Int'}}
  // expected-note@-1 {{coalesce using '??' to provide a default when the optional value contains 'nil'}} {{15-15=(}} {{16-16= ?? <#default value#>)}}
  // expected-note@-2 {{force-unwrap using '!' to abort execution if the optional value contains 'nil'}} {{16-16=!}}
  let _ = 2 + (x ?? 0)

  func foo(y: Int) {}
  foo(y: x) // expected-error {{value of optional type 'Int?' must be unwrapped to a value of type 'Int'}}
  // expected-note@-1 {{coalesce using '??' to provide a default when the optional value contains 'nil'}} {{11-11= ?? <#default value#>}}
  // expected-note@-2 {{force-unwrap using '!' to abort execution if the optional value contains 'nil'}} {{11-11=!}}
  foo(y: x ?? 0) 

  let _ = x < 2 // expected-error {{value of optional type 'Int?' must be unwrapped to a value of type 'Int'}}
  // expected-note@-1 {{coalesce using '??' to provide a default when the optional value contains 'nil'}} {{12-12= ?? <#default value#>}}
  // expected-note@-2 {{force-unwrap using '!' to abort execution if the optional value contains 'nil'}} {{12-12=!}}
  let _ = x ?? 0 < 2

  let _ = 2 < x // expected-error {{value of optional type 'Int?' must be unwrapped to a value of type 'Int'}}
  // expected-note@-1 {{coalesce using '??' to provide a default when the optional value contains 'nil'}} {{16-16= ?? <#default value#>}}
  // expected-note@-2 {{force-unwrap using '!' to abort execution if the optional value contains 'nil'}} {{16-16=!}}
  let _ = 2 < x ?? 0

  let _: Int = (.optionalIntMember) // expected-error {{value of optional type 'Int?' must be unwrapped to a value of type 'Int'}}
  // expected-note@-1 {{coalesce using '??' to provide a default when the optional value contains 'nil'}} {{35-35= ?? <#default value#>}}
  // expected-note@-2 {{force-unwrap using '!' to abort execution if the optional value contains 'nil'}} {{35-35=!}}
  let _: Int = (.optionalIntMember ?? 0)

  let _ = 1 + .optionalIntMember // expected-error {{value of optional type 'Int?' must be unwrapped to a value of type 'Int'}}
  // expected-note@-1 {{coalesce using '??' to provide a default when the optional value contains 'nil'}} {{15-15=(}} {{33-33= ?? <#default value#>)}}
  // expected-note@-2 {{force-unwrap using '!' to abort execution if the optional value contains 'nil'}} {{33-33=!}}
  let _ = 1 + (.optionalIntMember ?? 0)

  let _ = try .optionalThrowsMember + 1 // expected-error {{value of optional type 'Int?' must be unwrapped to a value of type 'Int'}}
  // expected-note@-1 {{coalesce using '??' to provide a default when the optional value contains 'nil'}} {{15-15=(}} {{36-36= ?? <#default value#>)}}
  // expected-note@-2 {{force-unwrap using '!' to abort execution if the optional value contains 'nil'}} {{36-36=!}}
  let _ = try (.optionalThrowsMember ?? 0) + 1

  let _ = .optionalIntMember?.bitWidth > 0 // expected-error {{value of optional type 'Int?' must be unwrapped to a value of type 'Int'}}
  // expected-note@-1 {{coalesce using '??' to provide a default when the optional value contains 'nil'}} {{39-39= ?? <#default value#>}}
  // expected-note@-2 {{force-unwrap using '!' to abort execution if the optional value contains 'nil'}} {{11-11=(}} {{39-39=)!}}
  let _ = (.optionalIntMember?.bitWidth)! > 0
  let _ = .optionalIntMember?.bitWidth ?? 0 > 0

  let _ = .random() ? .optionalIntMember : 0 // expected-error {{value of optional type 'Int?' must be unwrapped to a value of type 'Int'}}
  // expected-note@-1 {{coalesce using '??' to provide a default when the optional value contains 'nil'}} {{41-41= ?? <#default value#>}}
  // expected-note@-2 {{force-unwrap using '!' to abort execution if the optional value contains 'nil'}} {{41-41=!}}
  let _ = .random() ? .optionalIntMember ?? 0 : 0

  let _: Int = try try try .optionalThrowsMember // expected-error {{value of optional type 'Int?' must be unwrapped to a value of type 'Int'}}
  // expected-note@-1 {{coalesce using '??' to provide a default when the optional value contains 'nil'}} {{49-49= ?? <#default value#>}}
  // expected-note@-2 {{force-unwrap using '!' to abort execution if the optional value contains 'nil'}} {{49-49=!}}
  let _: Int = try try try .optionalThrowsMember ?? 0

  let _: Int = try! .optionalThrowsMember // expected-error {{value of optional type 'Int?' must be unwrapped to a value of type 'Int'}}
  // expected-note@-1 {{coalesce using '??' to provide a default when the optional value contains 'nil'}} {{42-42= ?? <#default value#>}}
  // expected-note@-2 {{force-unwrap using '!' to abort execution if the optional value contains 'nil'}} {{42-42=!}}
  let _: Int = try! .optionalThrowsMember ?? 0
}

// https://github.com/apple/swift/issues/63746
func issue63746() {
  let fn1 = {
    switch 0 {
      case 1 where 0: // expected-error {{integer literal value '0' cannot be used as a boolean; did you mean 'false'?}}
        ()
    }
  }
  let fn2 = {
    switch 0 {
      case 1 where 0: // expected-error {{integer literal value '0' cannot be used as a boolean; did you mean 'false'?}}
        break
    }
  }
}

func rdar86611718(list: [Int]) {
  String(list.count())
  // expected-error@-1 {{cannot call value of non-function type 'Int'}}
}

// rdar://108977234 - failed to produce diagnostic when argument to AnyHashable parameter doesn't conform to Hashable protocol
do {
  struct NonHashable {}

  func test(result: inout [AnyHashable], value: NonHashable) {
    result.append(value) // expected-error {{argument type 'NonHashable' does not conform to expected type 'Hashable'}}
  }
}

// https://github.com/apple/swift/issues/66206
func testNilCoalescingOperatorRemoveFix() {
  let _ = "" ?? "" // expected-warning {{left side of nil coalescing operator '??' has non-optional type 'String', so the right side is never used}} {{13-19=}}
  let _ = ""     ?? "" // expected-warning {{left side of nil coalescing operator '??' has non-optional type 'String', so the right side is never used}} {{13-23=}}
  let _ = "" /* This is a comment */ ?? "" // expected-warning {{left side of nil coalescing operator '??' has non-optional type 'String', so the right side is never used}} {{13-43=}}

  let _ = "" // This is a comment
    ?? "" // expected-warning {{left side of nil coalescing operator '??' has non-optional type 'String', so the right side is never used}} {{1555:13-1556:10=}}

  let _ = "" // This is a comment
    /*
     * The blank line below is part of the test case, do not delete it
     */

    ?? "" // expected-warning {{left side of nil coalescing operator '??' has non-optional type 'String', so the right side is never used}} {{1558:13-1563:10=}}

  if ("" ?? // This is a comment // expected-warning {{left side of nil coalescing operator '??' has non-optional type 'String', so the right side is never used}} {{9-1566:9=}}
      "").isEmpty {}

  if ("" // This is a comment
      ?? "").isEmpty {} // expected-warning {{left side of nil coalescing operator '??' has non-optional type 'String', so the right side is never used}} {{1568:9-1569:12=}}
}
