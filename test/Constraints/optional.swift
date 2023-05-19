// RUN: %target-typecheck-verify-swift

// REQUIRES: objc_interop

func markUsed<T>(_ t: T) {}

class A {
  @objc func do_a() {}

  @objc(do_b_2:) func do_b(_ x: Int) {}
  @objc func do_b(_ x: Float) {}

  @objc func do_c(x: Int) {} // expected-note {{incorrect labels for candidate (have: '(_:)', expected: '(x:)')}}
  @objc func do_c(y: Int) {} // expected-note {{incorrect labels for candidate (have: '(_:)', expected: '(y:)')}}
}

func test0(_ a: AnyObject) {
  a.do_a?()

  a.do_b?(1)
  a.do_b?(5.0)

  a.do_c?(1) // expected-error {{no exact matches in call to instance method 'do_c'}}
  a.do_c?(x: 1)
}

func test1(_ a: A) {
  a?.do_a() // expected-error {{cannot use optional chaining on non-optional value of type 'A'}} {{4-5=}}
  a!.do_a() // expected-error {{cannot force unwrap value of non-optional type 'A'}} {{4-5=}}
  // Produce a specialized diagnostic here?
  a.do_a?() // expected-error {{cannot use optional chaining on non-optional value of type '() -> ()'}} {{9-10=}}
}

// <rdar://problem/15508756>
extension Optional {
  func bind<U>(_ f: (Wrapped) -> U?) -> U? {
    switch self {
    case .some(let x):
      return f(x)
    case .none:
      return .none
    }
  }
}

var c: String? = Optional<Int>(1)
  .bind {(x: Int) in markUsed("\(x)!"); return "two" }

func test4() {
  func foo() -> Int { return 0 }
  func takes_optfn(_ f : () -> Int?) -> Int? { return f() }

  _ = takes_optfn(foo)

  func takes_objoptfn(_ f : () -> AnyObject?) -> AnyObject? { return f() }
  func objFoo() -> AnyObject { return A() }
  _ = takes_objoptfn(objFoo) // okay
  func objBar() -> A { return A() }
  _ = takes_objoptfn(objBar) // okay
}

func test5() -> Int? {
  return nil
}

func test6<T>(_ x : T) {
  _ = x as? Int? // Okay.  We know nothing about T, so cannot judge.
}

class B : A { }

func test7(_ x : A) {
  _ = x as? B? // Okay: Injecting into an Optional
}

func test7a(_ x : B) {
  _ = x as? A // expected-warning{{conditional cast from 'B' to 'A' always succeeds}}
}

func test8(_ x : AnyObject?) {
  let _ : A = x as! A
}


// Partial ordering with optionals
func test9_helper<T: P>(_ x: T) -> Int { }
func test9_helper<T: P>(_ x: T?) -> Double { }

func test9_helper2<T>(_ x: T) -> Int { }
func test9_helper2<T>(_ x: T?) -> Double { }

func test9(_ i: Int, io: Int?) {
  let result = test9_helper(i)
  var _: Int = result
  let result2 = test9_helper(io)
  let _: Double = result2

  let result3 = test9_helper2(i)
  var _: Int = result3
  let result4 = test9_helper2(io)
  let _: Double = result4
}

protocol P { }

func test10_helper<T : P>(_ x: T) -> Int { }
func test10_helper<T : P>(_ x: T?) -> Double { }

extension Int : P { }

func test10(_ i: Int, io: Int?) {
  let result = test10_helper(i)
  var _: Int = result

  let result2 = test10_helper(io)
  var _: Double = result2
}

var z: Int? = nil
z = z ?? 3

var fo: Float? = 3.14159

func voidOptional(_ handler: () -> ()?) {}
func testVoidOptional() {
  let noop: () -> Void = {}
  voidOptional(noop)

  let optNoop: (()?) -> ()? = { return $0 }
  voidOptional(optNoop)
}

protocol Proto1 {}
protocol Proto2 {}
struct Nilable: ExpressibleByNilLiteral {
	init(nilLiteral: ()) {}
}
func testTernaryWithNil<T>(b: Bool, s: String, i: Int, a: Any, t: T, m: T.Type, p: Proto1 & Proto2, arr: [Int], opt: Int?, iou: Int!, n: Nilable) {
  let t1 = b ? s : nil
  let _: Double = t1 // expected-error{{value of type 'String?'}}
  let t2 = b ? nil : i
  let _: Double = t2 // expected-error{{value of type 'Int?'}}
  let t3 = b ? "hello" : nil
  let _: Double = t3 // expected-error{{value of type 'String?'}}
  let t4 = b ? nil : 1
  let _: Double = t4 // expected-error{{value of type 'Int?'}}
  let t5 = b ? (s, i) : nil
  let _: Double = t5 // expected-error{{value of type '(String, Int)?}}
  let t6 = b ? nil : (i, s)
  let _: Double = t6 // expected-error{{value of type '(Int, String)?}}
  let t7 = b ? ("hello", 1) : nil
  let _: Double = t7 // expected-error{{value of type '(String, Int)?}}
  let t8 = b ? nil : (1, "hello")
  let _: Double = t8 // expected-error{{value of type '(Int, String)?}}
  let t9 = b ? { $0 * 2 } : nil
  let _: Double = t9 // expected-error{{value of type '((Int) -> Int)?}}
  let t10 = b ? nil : { $0 * 2 }
  let _: Double = t10 // expected-error{{value of type '((Int) -> Int)?}}
  let t11 = b ? a : nil
  let _: Double = t11 // expected-error{{value of type 'Any?'}}
  let t12 = b ? nil : a
  let _: Double = t12 // expected-error{{value of type 'Any?'}}
  let t13 = b ? t : nil
  let _: Double = t13 // expected-error{{value of type 'T?'}}
  let t14 = b ? nil : t
  let _: Double = t14 // expected-error{{value of type 'T?'}}
  let t15 = b ? m : nil
  let _: Double = t15 // expected-error{{value of type 'T.Type?'}}
  let t16 = b ? nil : m
  let _: Double = t16 // expected-error{{value of type 'T.Type?'}}
  let t17 = b ? p : nil
  let _: Double = t17 // expected-error{{value of type '(any Proto1 & Proto2)?'}}
  let t18 = b ? nil : p
  let _: Double = t18 // expected-error{{value of type '(any Proto1 & Proto2)?'}}
  let t19 = b ? arr : nil
  let _: Double = t19 // expected-error{{value of type '[Int]?'}}
  let t20 = b ? nil : arr
  let _: Double = t20 // expected-error{{value of type '[Int]?'}}
  let t21 = b ? opt : nil
  let _: Double = t21 // expected-error{{value of type 'Int?'}}
  let t22 = b ? nil : opt
  let _: Double = t22 // expected-error{{value of type 'Int?'}}
  let t23 = b ? iou : nil
  let _: Double = t23 // expected-error{{value of type 'Int?'}}
  let t24 = b ? nil : iou
  let _: Double = t24 // expected-error{{value of type 'Int?'}}
  let t25 = b ? n : nil
  let _: Double = t25 // expected-error{{value of type 'Nilable'}}
  let t26 = b ? nil : n
  let _: Double = t26 // expected-error{{value of type 'Nilable'}}
}

// inference with IUOs
infix operator ++++

protocol PPPP {
  static func ++++(x: Self, y: Self) -> Bool
}

func compare<T: PPPP>(v: T, u: T!) -> Bool {
  return v ++++ u
}

// https://github.com/apple/swift/issues/45356
func f_45356(x: String?, y: String?) {
  _ = x.map { xx in
    y.map { _ in "" } ?? "\(xx)"
  }
}

// https://github.com/apple/swift/issues/45836
// Invalid diagnostic calling implicitly unwrapped closure
do {
  var x : ((Int) -> ())!
  x?(a: 2) // expected-error {{extraneous argument label 'a:' in call}}
  x!(a: 2) // expected-error {{extraneous argument label 'a:' in call}}
  x(a: 2)  // expected-error {{extraneous argument label 'a:' in call}}

  struct S {
    var callback: (([AnyObject]) -> Void)!
  }

  S().callback?("test") // expected-error {{cannot convert value of type 'String' to expected argument type '[AnyObject]'}}
  S().callback!("test") // expected-error {{cannot convert value of type 'String' to expected argument type '[AnyObject]'}}
  S().callback("test")  // expected-error {{cannot convert value of type 'String' to expected argument type '[AnyObject]'}}

  _? = nil  // expected-error {{'nil' requires a contextual type}}
  _?? = nil // expected-error {{'nil' requires a contextual type}}
}

// rdar://problem/29993596
func takeAnyObjects(_ lhs: AnyObject?, _ rhs: AnyObject?) { }

infix operator !====

func !====(_ lhs: AnyObject?, _ rhs: AnyObject?) -> Bool { return false }

func testAnyObjectImplicitForce(lhs: AnyObject?!, rhs: AnyObject?) {
  if lhs !==== rhs { }

  takeAnyObjects(lhs, rhs)
}

// https://github.com/apple/swift/issues/46639

protocol P1 { }

class C1: P1 { }

protocol P2 {
    var prop: C1? { get }
}

class C2 {
    var p1: P1?
    var p2: P2?

    var computed: P1? {
        return p1 ?? p2?.prop
    }
}


// rdar://problem/31779785
class X { }

class Bar {
  let xOpt: X?
  let b: Bool

  init() {
    let result = b ? nil : xOpt
    let _: Int = result // expected-error{{cannot convert value of type 'X?' to specified type 'Int'}}
  }
}

// rdar://problem/37508855
func rdar37508855(_ e1: X?, _ e2: X?) -> [X] {
  return [e1, e2].filter { $0 == nil }.map { $0! }
}

func se0213() {
  struct Q: ExpressibleByStringLiteral {
    typealias StringLiteralType =  String

    var foo: String

    init?(_ possibleQ: StringLiteralType) {
      return nil
    }

    init(stringLiteral str: StringLiteralType) {
      self.foo = str
    }
  }

  _ = Q("why")?.foo // Ok
  _ = Q("who")!.foo // Ok
  _ = Q?("how") // Ok
}

func rdar45218255(_ i: Int) {
  struct S<T> {
    init(_:[T]) {}
  }

  _ = i!           // expected-error {{cannot force unwrap value of non-optional type 'Int'}} {{8-9=}}
  _ = [i!]         // expected-error {{cannot force unwrap value of non-optional type 'Int'}} {{9-10=}}
  _ = S<Int>([i!]) // expected-error {{cannot force unwrap value of non-optional type 'Int'}} {{16-17=}}
}

// rdar://problem/47967277
// https://github.com/apple/swift/issues/52299
// Cannot assign through '!': '$0' is immutable

func f1_52299() {
  func foo<T : Equatable>(_: @autoclosure () throws -> T,
                          _: @autoclosure () throws -> T) {}

  class A {
    var bar: String?
  }

  let r1 = A()
  let r2 = A()

  let arr1: [A] = []
  foo(Set(arr1.map { $0.bar! }), Set([r1, r2].map { $0.bar! })) // Ok
}

func f2_52299(cString: UnsafePointer<CChar>) {
  struct S {
    var a: Int32 = 0
    var b = ContiguousArray<CChar>(repeating: 0, count: 10)
  }

  var s = S()

  withUnsafeMutablePointer(to: &s.a) { ptrA in
    s.b.withUnsafeMutableBufferPointer { bufferB in
      withVaList([ptrA, bufferB.baseAddress!]) { ptr in } // Ok
    }
  }
}

// rdar://problem/47776586 - Diagnostic refers to '&' which is not present in the source code
func rdar47776586() {
  func foo(_: inout Int) {}
  var x: Int? = 0
  foo(&x) // expected-error {{value of optional type 'Int?' must be unwrapped to a value of type 'Int'}}
  // expected-note@-1 {{force-unwrap using '!' to abort execution if the optional value contains 'nil'}} {{7-7=(}} {{9-9=)!}}

  var dict = [1: 2]
  dict[1] += 1 // expected-error {{value of optional type 'Int?' must be unwrapped to a value of type 'Int'}}
  // expected-note@-1 {{force-unwrap using '!' to abort execution if the optional value contains 'nil'}} {{10-10=!}}
}

struct S {
  var foo: Optional<() -> Int?> = nil
  var bar: Optional<() -> Int?> = nil

  mutating func test(_ clj: @escaping () -> Int) {
    if let fn = foo {
      bar = fn  // Ok
      bar = clj // Ok
    }
  }
}

// rdar://problem/53238058 - Crash in getCalleeLocator while trying to produce a diagnostic about missing optional unwrap
//                           associated with an argument to a call

func rdar_53238058() {
  struct S {
    init(_: Double) {}
    init<T>(_ value: T) where T : BinaryFloatingPoint {}
  }

  func test(_ str: String) {
    _ = S(Double(str)) // expected-error {{value of optional type 'Double?' must be unwrapped to a value of type 'Double'}}
    // expected-note@-1 {{coalesce using '??' to provide a default when the optional value contains 'nil'}}
    // expected-note@-2 {{force-unwrap using '!' to abort execution if the optional value contains 'nil'}}
  }
}

// https://github.com/apple/swift/issues/50936
// Inconsistent ambiguity with optional and non-optional inout-to-pointer
do {
  struct S {
    init(_ x: UnsafeMutablePointer<Int>) {}
    init(_ x: UnsafeMutablePointer<Int>?) {}

    static func foo(_ x: UnsafeMutablePointer<Int>) {}
    static func foo(_ x: UnsafeMutablePointer<Int>?) {}

    static func bar(_ x: UnsafeMutablePointer<Int>, _ y: Int) {}
    static func bar(_ x: UnsafeMutablePointer<Int>?, _ y: Int) {}
  }

  var foo = 0

  _ = S(&foo)      // Ok
  _ = S.init(&foo) // Ok
  S.foo(&foo)  // Ok
  S.bar(&foo, 42) // Ok
}

// https://github.com/apple/swift/issues/53499
// Slightly misleading diagnostics for contextual failures with multiple fixes
do {
  func bar(_: Int) {}

  bar(["hello"].first)
  // expected-error@-1 {{cannot convert value of type 'String?' to expected argument type 'Int'}}
}

// rdar://problem/57668873 - Too eager force optional unwrap fix

@objc class Window {}

@objc protocol WindowDelegate {
  @objc optional var window: Window? { get set }
}

func test_force_unwrap_not_being_too_eager() {
  struct WindowContainer {
    unowned(unsafe) var delegate: WindowDelegate? = nil
  }

  let obj: WindowContainer = WindowContainer()
  if let _ = obj.delegate?.window { // Ok
  }
}

// rdar://problem/57097401
func invalidOptionalChaining(a: Any) {
  a == "="? // expected-error {{cannot use optional chaining on non-optional value of type 'String'}}
  // expected-error@-1 {{binary operator '==' cannot be applied to operands of type 'Any' and 'String?'}}
}

/// https://github.com/apple/swift/issues/54739
/// Force unwrapping `nil` compiles without warning
do {
  struct S {
    var foo: Int
  }

  _ = S(foo: nil!) // expected-error {{'nil' literal cannot be force unwrapped}}
  _ = nil! // expected-error {{'nil' literal cannot be force unwrapped}}
  _ = (nil!) // expected-error {{'nil' literal cannot be force unwrapped}}
  _ = (nil)! // expected-error {{'nil' literal cannot be force unwrapped}}
  _ = ((nil))! // expected-error {{'nil' literal cannot be force unwrapped}}
  _ = nil? // expected-error {{'nil' requires a contextual type}}
  _ = ((nil?)) // expected-error {{'nil' requires a contextual type}}
  _ = ((nil))? // expected-error {{'nil' requires a contextual type}}
  _ = ((nil)?) // expected-error {{'nil' requires a contextual type}}
  _ = nil // expected-error {{'nil' requires a contextual type}}
  _ = (nil) // expected-error {{'nil' requires a contextual type}}
  _ = ((nil)) // expected-error {{'nil' requires a contextual type}}
  _ = (((nil))) // expected-error {{'nil' requires a contextual type}}
  _ = ((((((nil)))))) // expected-error {{'nil' requires a contextual type}}
  _ = (((((((((nil))))))))) // expected-error {{'nil' requires a contextual type}}

  func test_with_contextual_type_one() -> Int? {
    return (nil) // Ok
  }

  func test_with_contextual_type_many() -> Int? {
    return (((nil))) // Ok
  }
}

// rdar://75146811 - crash due to incorrect inout type
func rdar75146811() {
  func test(_: UnsafeMutablePointer<Double>) {}
  func test_tuple(_: UnsafeMutablePointer<Double>, x: Int) {}
  func test_named(x: UnsafeMutablePointer<Double>) {}

  var arr: [Double]! = []

  test(&arr) // expected-error {{cannot convert value of type '[Double]?' to expected argument type 'Double'}}
  test((&arr)) // expected-error {{'&' may only be used to pass an argument to inout parameter}}
  // expected-error@-1 {{cannot convert value of type '[Double]?' to expected argument type 'Double'}}
  test(&(arr)) // expected-error {{cannot convert value of type '[Double]?' to expected argument type 'Double'}}

  test_tuple(&arr, x: 0) // expected-error {{cannot convert value of type '[Double]?' to expected argument type 'Double'}}
  test_tuple((&arr), x: 0) // expected-error {{'&' may only be used to pass an argument to inout parameter}}
  // expected-error@-1 {{cannot convert value of type '[Double]?' to expected argument type 'Double'}}
  test_tuple(&(arr), x: 0) // expected-error {{cannot convert value of type '[Double]?' to expected argument type 'Double'}}

  test_named(x: &arr) // expected-error {{cannot convert value of type '[Double]?' to expected argument type 'Double'}}
  test_named(x: (&arr)) // expected-error {{'&' may only be used to pass an argument to inout parameter}}
  // expected-error@-1 {{cannot convert value of type '[Double]?' to expected argument type 'Double'}}
  test_named(x: &(arr)) // expected-error {{cannot convert value of type '[Double]?' to expected argument type 'Double'}}
}

// rdar://75514153 - Unable to produce a diagnostic for ambiguities related to use of `nil`
func rdar75514153() {
  func test_no_label(_: Int) {}    // expected-note 2 {{'nil' is not compatible with expected argument type 'Int' at position #1}}
  func test_no_label(_: String) {} // expected-note 2 {{'nil' is not compatible with expected argument type 'String' at position #1}}

  test_no_label(nil)   // expected-error {{no exact matches in call to local function 'test_no_label'}}
  test_no_label((nil)) // expected-error {{no exact matches in call to local function 'test_no_label'}}

  func test_labeled(_: Int, x: Int) {}    // expected-note 2 {{'nil' is not compatible with expected argument type 'Int' at position #2}}
  func test_labeled(_: Int, x: String) {} // expected-note 2 {{'nil' is not compatible with expected argument type 'String' at position #2}}

  test_labeled(42, x: nil)   // expected-error {{no exact matches in call to local function 'test_labeled'}}
  test_labeled(42, x: (nil)) // expected-error {{no exact matches in call to local function 'test_labeled'}}
}

// rdar://85166519 - Crash dereferencing Null Type In Bogus Expression
func rdar85166519() {
  var v: Int? = nil

  var _: [Int: AnyObject] = [ // expected-error {{dictionary of type '[Int : AnyObject]' cannot be initialized with array literal}}
    // expected-note@-1 {{did you mean to use a dictionary literal instead?}}
    v?.addingReportingOverflow(0)
  ]
}

// https://github.com/apple/swift/issues/58539
if let x = nil {} // expected-error{{'nil' requires a contextual type}}

// https://github.com/apple/swift/issues/56699
let singleOptionalClosure: (() -> Void)? = nil
let doubleOptionalClosure: (() -> Void)?? = nil
singleOptionalClosure()
// expected-error@-1 {{value of optional type}}
// expected-note@-2 {{use optional chaining to call this value of function type when optional is non-'nil'}} {{22-22=?}}
// expected-note@-3 {{coalesce}}
// expected-note@-4 {{force-unwrap}}

doubleOptionalClosure()
// expected-error@-1 {{value of optional type}}
// expected-note@-2 {{use optional chaining to call this value of function type when optional is non-'nil'}} {{22-22=??}}
// expected-note@-3 {{coalesce}}
// expected-note@-4 {{force-unwrap}}

doubleOptionalClosure?()
// expected-error@-1 {{value of optional type}}
// expected-note@-2 {{use optional chaining to call this value of function type when optional is non-'nil'}} {{23-23=?}}
// expected-note@-3 {{coalesce}}
// expected-note@-4 {{force-unwrap}}

doubleOptionalClosure!()
// expected-error@-1 {{value of optional type}}
// expected-note@-2 {{use optional chaining to call this value of function type when optional is non-'nil'}} {{23-23=?}}
// expected-note@-3 {{coalesce}}
// expected-note@-4 {{force-unwrap}}

struct FunctionContainer {
  func test() {}
}

func testFunctionContainerMethodCall(container: FunctionContainer?) {
  let fn = container?.test
   // expected-note@-1 {{short-circuit}}
   // expected-note@-2 {{coalesce}}
   // expected-note@-3 {{force-unwrap}}
  fn()
  // expected-error@-1 {{value of optional type}}
  // expected-note@-2 {{use optional chaining to call this value of function type when optional is non-'nil'}} {{5-5=?}}
  // expected-note@-3 {{coalesce}}
  // expected-note@-4 {{force-unwrap}}
}

// Test for https://github.com/apple/swift/issues/60730
// rdar://94037733
do {
  struct S: P {}
  func takesP(_: any P) {}
  func passOptional(value: (any P)?) {
    takesP(value)
    // expected-error@-1 {{value of optional type '(any P)?' must be unwrapped to a value of type 'any P'}}
    // expected-note@-2 {{coalesce using '??' to provide a default when the optional value contains 'nil'}}
    // expected-note@-3 {{force-unwrap using '!' to abort execution if the optional value contains 'nil'}}
  }
  func passLayersOfOptional(value: (any P)??) {
    // FIXME(diagnostics): Consider recording multiple ForceUnwrap fixes based on number of optionals 
    takesP(value)
    // expected-error@-1 {{value of optional type '(any P)??' must be unwrapped to a value of type '(any P)?}}
    // expected-note@-2 {{coalesce using '??' to provide a default when the optional value contains 'nil'}}
    // expected-note@-3 {{force-unwrap using '!' to abort execution if the optional value contains 'nil'}}
  }
  func passNonConformingValue(value: (any BinaryInteger)?){
    takesP(value)
    // expected-error@-1 {{argument type '(any BinaryInteger)?' does not conform to expected type 'P'}}
  }
}

// Diagnose extraneous force unwrap in ambiguous context
do {
  func test(_: Int) {} // expected-note {{candidate expects value of type 'Int' for parameter #1 (got 'Double')}}
  func test(_: String) {} // expected-note {{candidate expects value of type 'String' for parameter #1 (got 'Double')}}

  var x: Double = 42
  test(x!) // expected-error {{no exact matches in call to local function 'test'}}
  // expected-error@-1 {{cannot force unwrap value of non-optional type 'Double'}}
}
