// RUN: %target-typecheck-verify-swift

func f1() -> Int { }
func f2(_: Int = 5) -> Int { }
func f3(_: Int...) -> Int { }

class A { }
class B : A { 
  func iAmAB() {}
  func createB() -> B { return B() }
}

func f4() -> B { }
func f5(_ a: A) { }
func f6(_ a: A, _: Int) { }

func createB() -> B { }
func createB(_ i: Int) -> B { }

func f7(_ a: A, _: @escaping () -> Int) -> B { }
func f7(_ a: A, _: Int) -> Int { }

// Forgot the '()' to call a function.
func forgotCall() {
  // Simple cases
  var x: Int
  x = f1 // expected-error{{function produces expected type 'Int'; did you mean to call it with '()'?}}{{9-9=()}}
  x = f2 // expected-error{{cannot assign value of type '(Int) -> Int' to type 'Int'}}
  x = f3 // expected-error{{cannot assign value of type '(Int...) -> Int' to type 'Int'}}

  // With a supertype conversion
  var a = A()
  a = f4 // expected-error{{function produces expected type 'B'; did you mean to call it with '()'?}}{{9-9=()}}

  // As a call
  f5(f4) // expected-error{{function produces expected type 'B'; did you mean to call it with '()'?}}{{8-8=()}}
  f6(f4, f2) // expected-error{{function produces expected type 'B'; did you mean to call it with '()'?}}{{8-8=()}}
  // expected-error@-1 {{function produces expected type 'Int'; did you mean to call it with '()'?}} {{12-12=()}}

  // With overloading: only one succeeds.
  a = createB // expected-error{{function produces expected type 'B'; did you mean to call it with '()'?}}

  let _: A = createB // expected-error{{function produces expected type 'B'; did you mean to call it with '()'?}} {{21-21=()}}
  let _: B = createB // expected-error{{function produces expected type 'B'; did you mean to call it with '()'?}} {{21-21=()}}

  // With overloading, pick the fewest number of fixes.
  var b = f7(f4, f1) // expected-error{{function produces expected type 'B'; did you mean to call it with '()'?}}
  b.iAmAB()
}

/// Forgot the '!' to unwrap an optional.
func parseInt() -> Int? { }

func <(lhs: A, rhs: A) -> A? { return nil }

func forgotOptionalBang(_ a: A, obj: AnyObject) {
  var i: Int = parseInt() // expected-error{{value of optional type 'Int?' must be unwrapped to a value of type 'Int'}}
  // expected-note@-1{{coalesce using '??' to provide a default when the optional value contains 'nil'}}{{26-26= ?? <#default value#>}}
  // expected-note@-2{{force-unwrap using '!' to abort execution if the optional value contains 'nil'}}{{26-26=!}}

  var a = A(), b = B()
  b = a as? B  // expected-error{{value of optional type 'B?' must be unwrapped to a value of type 'B'}}
  // expected-note@-1{{coalesce using '??' to provide a default when the optional value contains 'nil'}}{{14-14= ?? <#default value#>}}
  // expected-note@-2{{force-unwrap using '!' to abort execution if the optional value contains 'nil'}}{{7-7=(}}{{14-14=)!}}

  a = a < a // expected-error{{value of optional type 'A?' must be unwrapped to a value of type 'A'}}
  // expected-note@-1{{coalesce using '??' to provide a default when the optional value contains 'nil'}}{{7-7=(}}{{12-12=) ?? <#default value#>}}
  // expected-note@-2{{force-unwrap using '!' to abort execution if the optional value contains 'nil'}}{{7-7=(}}{{12-12=)!}}
  
  // rdar://problem/20377684 -- take care that the '!' doesn't fall into an
  // optional evaluation context
  let bo: B? = b
  let b2: B = bo?.createB() // expected-error{{value of optional type 'B?' must be unwrapped to a value of type 'B'}}
  // expected-note@-1{{coalesce using '??' to provide a default when the optional value contains 'nil'}}
  // expected-note@-2{{force-unwrap using '!' to abort execution if the optional value contains 'nil'}}
}

func forgotAnyObjectBang(_ obj: AnyObject) {
  var a = A()
  a = obj // expected-error{{'AnyObject' is not convertible to 'A'}}
  //expected-note@-1 {{did you mean to use 'as!' to force downcast?}} {{10-10= as! A}}
  _ = a
}

func increment(_ x: inout Int) { }

func forgotAmpersand() {
  var i = 5
  increment(i) // expected-error{{passing value of type 'Int' to an inout parameter requires explicit '&'}}{{13-13=&}}

  var array = [1,2,3]
  increment(array[1]) // expected-error{{passing value of type 'Int' to an inout parameter requires explicit '&'}}{{13-13=&}}
}

func maybeFn() -> ((Int) -> Int)? { }

func extraCall() {
  var i = 7
  i = i() // expected-error{{cannot call value of non-function type 'Int'}}{{8-10=}}

  maybeFn()(5) // expected-error{{value of optional type '((Int) -> Int)?' must be unwrapped to a value of type '(Int) -> Int'}}
  // expected-note@-1{{coalesce using '??' to provide a default when the optional value contains 'nil'}}
  // expected-note@-2{{force-unwrap using '!' to abort execution if the optional value contains 'nil'}}
}

class U {
    var prop1 = 0
}

class T {
    func m1() {
      // <rdar://problem/17741575>
      let l = self.m2!.prop1
      // expected-error@-1 {{method 'm2' was used as a property; add () to call it}}  {{22-22=()}}
    }

    func m2() -> U! {
      return U()
    }
}

// Used an optional in a conditional expression
class C {
  var a: Int = 1
}
var co: C? = nil
var ciuo: C! = nil 

if co {} // expected-error{{optional type 'C?' cannot be used as a boolean; test for '!= nil' instead}}{{4-4=(}} {{6-6= != nil)}}
if ciuo {} // expected-error{{optional type 'C?' cannot be used as a boolean; test for '!= nil' instead}}{{4-4=(}} {{8-8= != nil)}}
co ? true : false // expected-error{{optional type 'C?' cannot be used as a boolean; test for '!= nil' instead}}{{1-1=(}} {{3-3= != nil)}}
!co ? false : true // expected-error{{optional type 'C?' cannot be used as a boolean; test for '== nil' instead}} {{1-2=}} {{2-2=(}} {{4-4= == nil)}}
ciuo ? true : false // expected-error{{optional type 'C?' cannot be used as a boolean; test for '!= nil' instead}}{{1-1=(}} {{5-5= != nil)}}
!ciuo ? false : true // expected-error{{optional type 'C?' cannot be used as a boolean; test for '== nil' instead}} {{1-2=}} {{2-2=(}} {{6-6= == nil)}}
!co // expected-error{{optional type 'C?' cannot be used as a boolean; test for '== nil' instead}} {{1-2=}} {{2-2=(}} {{4-4= == nil)}}
!ciuo // expected-error{{optional type 'C?' cannot be used as a boolean; test for '== nil' instead}} {{1-2=}} {{2-2=(}} {{6-6= == nil)}}

// Used an integer in a conditional expression
var n1: Int = 1
var n2: UInt8 = 0
var n3: Int16 = 2

if n1 {} // expected-error {{type 'Int' cannot be used as a boolean; test for '!= 0' instead}} {{4-4=(}} {{6-6= != 0)}}
if !n1 {} // expected-error {{type 'Int' cannot be used as a boolean; test for '== 0' instead}} {{4-5=}} {{5-5=(}} {{7-7= == 0)}}
n1 ? true : false // expected-error {{type 'Int' cannot be used as a boolean; test for '!= 0' instead}} {{1-1=(}} {{3-3= != 0)}}
!n1 ? false : true // expected-error {{type 'Int' cannot be used as a boolean; test for '== 0' instead}} {{1-2=}} {{2-2=(}} {{4-4= == 0)}}
!n1 // expected-error {{type 'Int' cannot be used as a boolean; test for '== 0' instead}} {{1-2=}} {{2-2=(}} {{4-4= == 0)}}
if n2 {} // expected-error {{type 'UInt8' cannot be used as a boolean; test for '!= 0' instead}} {{4-4=(}} {{6-6= != 0)}}
!n3 // expected-error {{type 'Int16' cannot be used as a boolean; test for '== 0' instead}} {{1-2=}} {{2-2=(}} {{4-4= == 0)}}

// Forgotten ! or ?
var someInt = co.a // expected-error{{value of optional type 'C?' must be unwrapped to refer to member 'a' of wrapped base type 'C'}}
// expected-note@-1{{chain the optional using '?' to access member 'a' only for non-'nil' base values}}{{17-17=?}}
// expected-note@-2{{force-unwrap using '!' to abort execution if the optional value contains 'nil'}}{{17-17=!}}

// https://github.com/apple/swift/issues/43451
struct Q {
  let s: String?
}
let q = Q(s: nil)
let a: Int? = q.s.utf8 // expected-error{{value of optional type 'String?' must be unwrapped to refer to member 'utf8' of wrapped base type 'String'}}
// expected-error@-1 {{cannot convert value of type 'String.UTF8View?' to specified type 'Int?'}}
// expected-note@-2{{chain the optional using '?'}}{{18-18=?}}
let b: Int = q.s.utf8 // expected-error{{value of optional type 'String?' must be unwrapped to refer to member 'utf8' of wrapped base type 'String'}}
// expected-error@-1 {{cannot convert value of type 'String.UTF8View' to specified type 'Int'}}
// expected-note@-2{{chain the optional using '?'}}{{17-17=?}}
// expected-note@-3{{force-unwrap using '!'}}{{17-17=!}}
let d: Int! = q.s.utf8 // expected-error{{value of optional type 'String?' must be unwrapped to refer to member 'utf8' of wrapped base type 'String'}}
// expected-error@-1 {{cannot convert value of type 'String.UTF8View?' to specified type 'Int?'}}
// expected-note@-2{{chain the optional using '?'}}{{18-18=?}}
let c = q.s.utf8 // expected-error{{value of optional type 'String?' must be unwrapped to refer to member 'utf8' of wrapped base type 'String'}}
// expected-note@-1{{chain the optional using '?' to access member 'utf8' only for non-'nil' base values}}{{12-12=?}}
// expected-note@-2{{force-unwrap using '!' to abort execution if the optional value contains 'nil'}}{{12-12=!}}

// https://github.com/apple/swift/issues/43729
do {
  struct S {
    var s: Int?
  }

  let x: [S] = []
  var y = Set(1...10).subtracting(x.map({ $0.s })) // expected-error {{value of optional type 'Int?' must be unwrapped to a value of type 'Int'}}
  // expected-note@-1{{coalesce using '??' to provide a default when the optional value contains 'nil'}} {{47-47= ?? <#default value#>}}
  // expected-note@-2{{force-unwrap using '!' to abort execution if the optional value contains 'nil'}} {{47-47=!}}
}

func makeArray<T>(_ x: T) -> [T] { [x] }

// https://github.com/apple/swift/issues/54837
func f_54837(_ x: Int?) {
  _ = Set(0...10).subtracting(makeArray(x)) // expected-error {{value of optional type 'Int?' must be unwrapped to a value of type 'Int'}}
  // expected-note@-1{{coalesce using '??' to provide a default when the optional value contains 'nil'}} {{42-42= ?? <#default value#>}}
  // expected-note@-2{{force-unwrap using '!' to abort execution if the optional value contains 'nil'}} {{42-42=!}}
}

func moreComplexUnwrapFixes() {
  struct S {
    let value: Int
    let optValue: Int? = nil
  }
  struct T {
    let s: S
    let optS: S?
  }
  func takeNon(_ x: Int) -> Void {}
  func takeOpt(_ x: Int?) -> Void {}

  let s = S(value: 0)
  let t: T? = T(s: s, optS: nil)
  let os: S? = s

  takeOpt(os.value) // expected-error{{value of optional type 'S?' must be unwrapped to refer to member 'value' of wrapped base type 'S'}}
  // expected-note@-1{{chain the optional using '?'}}{{13-13=?}}
  takeNon(os.value) // expected-error{{value of optional type 'S?' must be unwrapped to refer to member 'value' of wrapped base type 'S'}}
  // expected-note@-1{{chain the optional using '?'}}{{13-13=?}}
  // expected-note@-2{{force-unwrap using '!'}}{{13-13=!}}

  // FIXME: Ideally we'd recurse evaluating chaining fixits instead of only offering just the unwrap of t
  takeOpt(t.s.value) // expected-error{{value of optional type 'T?' must be unwrapped to refer to member 's' of wrapped base type 'T'}}
  // expected-note@-1{{chain the optional using '?'}}{{12-12=?}}
  // expected-note@-2{{force-unwrap using '!'}}{{12-12=!}}

  takeOpt(t.optS.value) // expected-error{{value of optional type 'T?' must be unwrapped to refer to member 'optS' of wrapped base type 'T'}}
  // expected-note@-1{{chain the optional using '?'}}{{12-12=?}}
  // expected-error@-2{{value of optional type 'S?' must be unwrapped to refer to member 'value' of wrapped base type 'S'}}
  // expected-note@-3{{chain the optional using '?'}}{{17-17=?}}

  takeNon(t.optS.value) // expected-error{{value of optional type 'T?' must be unwrapped to refer to member 'optS' of wrapped base type 'T'}}
  // expected-note@-1{{chain the optional using '?'}}{{12-12=?}}
  // expected-error@-2{{value of optional type 'S?' must be unwrapped to refer to member 'value' of wrapped base type 'S'}}
  // expected-note@-3{{chain the optional using '?'}}{{17-17=?}}
  // expected-note@-4{{force-unwrap using '!'}}{{17-17=!}}

  takeNon(os?.value) // expected-error{{value of optional type 'Int?' must be unwrapped to a value of type 'Int'}}
  // expected-note@-1{{force-unwrap using '!'}}{{13-14=!}}
  // expected-note@-2{{coalesce}}
  takeNon(os?.optValue) // expected-error{{value of optional type 'Int?' must be unwrapped to a value of type 'Int'}}
  // expected-note@-1{{force-unwrap using '!'}}{{11-11=(}} {{23-23=)!}}
  // expected-note@-2{{coalesce}}

  func sample(a: Int?, b: Int!) {
    let aa = a
    // expected-note@-1{{short-circuit using 'guard'}}{{5-5=guard }} {{15-15= else \{ return \}}}
    // expected-note@-2{{force-unwrap}}
    // expected-note@-3{{coalesce}}

    let bb = b // expected-note{{value inferred to be type 'Int?' when initialized with an implicitly unwrapped value}}
    // expected-note@-1{{short-circuit using 'guard'}}{{5-5=guard }} {{15-15= else \{ return \}}}
    // expected-note@-2{{force-unwrap}}
    // expected-note@-3{{coalesce}}

    let cc = a

    takeNon(aa) // expected-error{{value of optional type 'Int?' must be unwrapped to a value of type 'Int'}}
    // expected-note@-1{{force-unwrap}} expected-note@-1{{coalesce}}
    takeNon(bb) // expected-error{{value of optional type 'Int?' must be unwrapped to a value of type 'Int'}}
    // expected-note@-1{{force-unwrap}} expected-note@-1{{coalesce}}

    _ = [].map { takeNon(cc) } // expected-error{{value of optional type 'Int?' must be unwrapped to a value of type 'Int'}}
    // expected-note@-1{{force-unwrap}} expected-note@-1{{coalesce}}

    takeOpt(cc)
  }

  func sample2(a: Int?) -> Int {
    let aa = a
    // expected-note@-1{{short-circuit using 'guard'}}{{5-5=guard }} {{15-15= else \{ return <#default value#> \}}}
    // expected-note@-2{{force-unwrap}}
    // expected-note@-3{{coalesce}}

    return aa // expected-error{{value of optional type 'Int?' must be unwrapped to a value of type 'Int'}}
    // expected-note@-1{{force-unwrap}} expected-note@-1{{coalesce}}

  }
}

struct FooStruct {
  func a() -> Int?? { return 10 }

  var b: Int?? {
    return 15
  }

  func c() -> Int??? { return 20 }

  var d: Int??? {
    return 25
  }

  let e: BarStruct? = BarStruct()

  func f() -> Optional<Optional<Int>> { return 29 }
}

struct BarStruct {
  func a() -> Int? { return 30 }
  var b: Int?? {
    return 35
  }
}

let thing: FooStruct? = FooStruct()

let _: Int? = thing?.a() // expected-error {{value of optional type 'Int??' must be unwrapped to a value of type 'Int?'}}
// expected-note@-1{{coalesce}}
// expected-note@-2{{force-unwrap}}
let _: Int? = thing?.b // expected-error {{value of optional type 'Int??' must be unwrapped to a value of type 'Int?'}}
// expected-note@-1{{coalesce}}
// expected-note@-2{{force-unwrap}}
let _: Int?? = thing?.c() // expected-error {{value of optional type 'Int???' must be unwrapped to a value of type 'Int??'}}
// expected-note@-1{{coalesce}}
// expected-note@-2{{force-unwrap}}
let _: Int?? = thing?.d // expected-error {{value of optional type 'Int???' must be unwrapped to a value of type 'Int??'}}
// expected-note@-1{{coalesce}}
// expected-note@-2{{force-unwrap}}
let _: Int = thing?.e?.a() // expected-error {{value of optional type 'Int?' must be unwrapped to a value of type 'Int'}}
// expected-note@-1{{coalesce}}
// expected-note@-2{{force-unwrap}}
let _: Int? = thing?.e?.b // expected-error {{value of optional type 'Int??' must be unwrapped to a value of type 'Int?'}}
// expected-note@-1{{coalesce}}
// expected-note@-2{{force-unwrap}}
let _: Int? = thing?.f() // expected-error {{value of optional type 'Int??' must be unwrapped to a value of type 'Int?'}}
// expected-note@-1{{coalesce}}
// expected-note@-2{{force-unwrap}}

// https://github.com/apple/swift/issues/52262
func coalesceWithParensRootExprFix() {
  let optionalBool: Bool? = false
  if !optionalBool { }  // expected-error{{value of optional type 'Bool?' must be unwrapped to a value of type 'Bool'}}
  // expected-note@-1{{coalesce using '??' to provide a default when the optional value contains 'nil'}}{{7-7=(}}{{19-19= ?? <#default value#>)}}
  // expected-note@-2{{force-unwrap using '!' to abort execution if the optional value contains 'nil'}}
}

func test_explicit_call_with_overloads() {
  func foo(_: Int) {}

  struct S {
    func foo(_: Int) -> Int { return 0 }
    func foo(_: Int = 32, _: String = "hello") -> Int {
      return 42
    }
  }

  foo(S().foo)
  // expected-error@-1 {{function produces expected type 'Int'; did you mean to call it with '()'?}} {{14-14=()}}
}

// https://github.com/apple/swift/issues/53876
func testKeyPathSubscriptArgFixes(_ fn: @escaping () -> Int) {
  struct S {
    subscript(x: Int) -> Int { x }
  }

  var i: Int?
  _ = \S.[i] // expected-error {{value of optional type 'Int?' must be unwrapped to a value of type 'Int'}}
  // expected-note@-1{{coalesce using '??' to provide a default when the optional value contains 'nil'}}{{12-12= ?? <#default value#>}}
  // expected-note@-2{{force-unwrap using '!' to abort execution if the optional value contains 'nil'}}{{12-12=!}}

  _ = \S.[nil] // expected-error {{'nil' is not compatible with expected argument type 'Int'}}
  _ = \S.[fn] // expected-error {{function produces expected type 'Int'; did you mean to call it with '()'?}} {{13-13=()}}
}

// https://github.com/apple/swift/issues/54865
func f_54865(a: Any, _ str: String?) {
  a == str // expected-error {{binary operator '==' cannot be applied to operands of type 'Any' and 'String?'}}
}
