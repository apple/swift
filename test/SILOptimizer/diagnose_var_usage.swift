// RUN: %target-swift-frontend -emit-sil %s -verify

// <rdar://problem/20872721> QoI: warn about unused variables
// <rdar://problem/15975935> warning that you can use 'let' not 'var'
// <rdar://problem/18876585> Compiler should warn me if I set a parameter as 'var' but never modify it

func basicTests() -> Int {
  let x = 42 // expected-warning {{immutable value 'x' was never used; consider replacing with assignment to '_' or removing it}} {{3-8=_}}
  var y = 12 // expected-warning {{variable 'y' was never mutated; consider changing to 'let' constant}} {{3-6=let}}
  _ = 42 // ok
  _ = 42 // ok
  return y
}

func testUnderscoreAssignment() {
  var a: Int {
    get { return 0 }
    set { print(newValue) }
  }
  _ = a
  
  var b = 0 // expected-warning {{variable 'b' was never mutated; consider changing to 'let' constant}}
  _ = b
  
  let c = 0
  _ = c + 1
  
  let d = 0
  _ = d
}

struct X {
  var value = 1
  func f() {}
  mutating func g() {}
}

class TestClass {
  
  func f() {
    
    takeClosure { [weak self] in  // self is mutable but never mutated.  Ok because it is weak
      self?.f()
    }
  }
}

func testStruct() {
  let a = X()
  a.f()
  
  var b = X()
  b.g()
  
  var c = X() // expected-warning {{variable 'c' was never mutated; consider changing to 'let' constant}} {{3-6=let}}
  c.f()
  
  var d = X() // expected-warning {{initialization of variable 'd' was never used; consider replacing with assignment to '_' or removing it}}
  
  let e = X() // expected-warning {{initialization of immutable value 'e' was never used; consider replacing with assignment to '_' or removing it}}
  
  var f = X() // expected-warning {{variable 'f' was written to, but never read}}
  f = X()
  
  let g = X()
  var h = X() // expected-warning {{variable 'h' was written to, but never read}}
  h = g
  
  var i = X()
  i.value = 2
  
  var j = X() // expected-warning {{variable 'j' was never mutated; consider changing to 'let' constant}} {{3-6=let}}
  markUsed(j.value)
  
  var k = X()
  k.value += 1
  
  let l = X()
  markUsed(l.value)
  
  _ = X()
}

func testDuplicateStructReference() {
  // Variables reference the same value but should only track their own uses
  let a = X() // expected-note {{initially referenced here}}
  let b = a // expected-warning {{immutable value referenced by 'b' is already referenced by 'a'; consider removing it and replacing all uses of 'b' with 'a'}}
  var c = a // expected-warning {{initialization of variable 'c' was never used; consider replacing with assignment to '_' or removing it}}
  markUsed(a)
  
  let d = X() // expected-note {{initially referenced here}}
  let e = d // expected-warning {{immutable value referenced by 'e' is already referenced by 'd'; consider removing it and replacing all uses of 'e' with 'd'}}
  var f = e // expected-warning {{initialization of variable 'f' was never used; consider replacing with assignment to '_' or removing it}}
  markUsed(e)
  
  let g = X()
  var h = g // expected-warning {{initialization of variable 'h' was never used; consider replacing with assignment to '_' or removing it}}
}

func testDuplicateReferenceStructParam(a: X) -> X { // expected-note {{initially referenced here}}
  let b = a // expected-warning {{immutable value referenced by 'b' is already referenced by 'a'; consider removing it and replacing all uses of 'b' with 'a'}}
  return b
}

func testNotDuplicateReferenceStructParam(a: X) -> X {
  var b = a // expected-warning {{variable 'b' was never mutated; consider changing to 'let' constant}}
  return b
}

func testDuplicateIntReference() {
  // Variables reference the same value but should only track their own uses
  let a = 42 // expected-note {{initially referenced here}}
  let b = a // expected-warning {{immutable value referenced by 'b' is already referenced by 'a'; consider removing it and replacing all uses of 'b' with 'a'}}
  var c = a // expected-warning {{initialization of variable 'c' was never used; consider replacing with assignment to '_' or removing it}}
  markUsed(a)
  
  let d = 42 // expected-note {{initially referenced here}}
  let e = d // expected-warning {{immutable value referenced by 'e' is already referenced by 'd'; consider removing it and replacing all uses of 'e' with 'd'}}
  var f = e // expected-warning {{initialization of variable 'f' was never used; consider replacing with assignment to '_' or removing it}}
  markUsed(e)
  
  let g = 42
  var h = g // expected-warning {{initialization of variable 'h' was never used; consider replacing with assignment to '_' or removing it}}
}

func testDuplicateReferenceIntParam(a: Int) -> Int { // expected-note {{initially referenced here}}
  let b = a // expected-warning {{immutable value referenced by 'b' is already referenced by 'a'; consider removing it and replacing all uses of 'b' with 'a'}}
  return b
}

func testNotDuplicateReferenceIntParam(a: Int) -> Int {
  let b = a + 1
  var c = a // expected-warning {{initialization of variable 'c' was never used; consider replacing with assignment to '_' or removing it}}
  return b
}

func testDuplicateClassReference() {
  // Variables reference the same value but should only track their own uses
  let a = TestClass()
  let b = a // expected-warning {{initialization of immutable value 'b' was never used; consider replacing with assignment to '_' or removing it}}
  var c = a // expected-warning {{initialization of variable 'c' was never used; consider replacing with assignment to '_' or removing it}}
  markUsed(a)
  
  let d = TestClass()
  let e = d
  var f = e // expected-warning {{initialization of variable 'f' was never used; consider replacing with assignment to '_' or removing it}}
  markUsed(e)
  
  let g = TestClass()
  var h = g // expected-warning {{initialization of variable 'h' was never used; consider replacing with assignment to '_' or removing it}}
}

func testDuplicateReferenceClassParam(a: TestClass) -> TestClass {
  let b = a
  return b
}

func testNotDuplicateReferenceClassParam(a: TestClass) -> TestClass {
  var b = a // expected-warning {{initialization of variable 'b' was never used; consider replacing with assignment to '_' or removing it}}
  return a
}

func takeClosure(_ fn : () -> ()) {}

enum TestEnum {
  case Test(Int, Int, Int)
}

func testEnumNotMutated() -> Int {
  let ev = TestEnum.Test(5, 6, 7)
  switch ev {
  case .Test(var i, var j, var k): // expected-warning {{variable 'i' was never mutated; consider changing to 'let' constant}} {{14-17=let}}
                                   // expected-warning@-1 {{variable 'j' was never mutated; consider changing to 'let' constant}} {{21-24=let}}
                                   // expected-warning@-2 {{variable 'k' was never mutated; consider changing to 'let' constant}} {{28-31=let}}
    return i + j + k
  default: // expected-warning {{default will never be executed}}
    return 0
  }
}

func testEnum() -> Int {
  let ev = TestEnum.Test(5, 6, 7)
  switch ev {
  case .Test(let i, let j, let k):
    return i + j + k
  default: // expected-warning {{default will never be executed}}
    return 0
  }
}

func nestedFunction() -> Int {
  var x = 42  // No warning about being never-set.
  
  func g() {
    x = 97
    var q = 27  // expected-warning {{variable 'q' was never used}} {{5-10=_}}
  }
  g()
  
  return x
}

func neverRead() {
  var x = 42  // expected-warning {{variable 'x' was written to, but never read}}
  
  x += 97
  x = 123
}

func property() -> Int {
  var p : Int {  // everything ok
    return 42
  }
  return p
}


func testInOut(_ x : inout Int) {  // Ok.
}

func testInOutNoRead() {
  var x = 12 // expected-warning {{variable 'x' was written to, but never read}}
  testInOut(&x)
}

struct TestStruct {
  var property = 42

  var mutatingProperty: Int {
    mutating get { return 0 }
    mutating set {}
  }
  var nonmutatingProperty: Int {
    nonmutating get { return 0 }
    nonmutating set {}
  }
  subscript(mutating index: Int) -> Int {
    mutating get { return 0 }
    mutating set {}
  }
  subscript(nonmutating index: Int) -> Int {
    nonmutating get { return 0 }
    nonmutating set {}
  }
}

func testStructMember() -> TestStruct {
  var x = TestStruct()  // ok
  x.property = 17
  return x
}

func testMutatingProperty_get() -> TestStruct {
  var x = TestStruct()  // ok
  _ = x.mutatingProperty
  return x
}

func testMutatingProperty_set() -> TestStruct {
  var x = TestStruct()  // ok
  x.mutatingProperty = 17
  return x
}

func testNonmutatingProperty_get() -> TestStruct {
  var x = TestStruct()  // expected-warning {{variable 'x' was never mutated; consider changing to 'let' constant}}
  _ = x.nonmutatingProperty
  return x
}

func testNonmutatingProperty_set() -> TestStruct {
  var x = TestStruct()  // expected-warning {{variable 'x' was never mutated; consider changing to 'let' constant}}
  x.nonmutatingProperty = 17
  return x
}

func testMutatingSubscript_get() -> TestStruct {
  var x = TestStruct()  // ok
  _ = x[mutating: 4]
  return x
}

func testMutatingSubscript_set() -> TestStruct {
  var x = TestStruct()  // ok
  x[mutating: 4] = 17
  return x
}

func testNonmutatingSubscript_get() -> TestStruct {
  var x = TestStruct()  // expected-warning {{variable 'x' was never mutated; consider changing to 'let' constant}}
  _ = x[nonmutating: 4]
  return x
}

func testNonmutatingSubscript_set() -> TestStruct {
  var x = TestStruct()  // expected-warning {{variable 'x' was never mutated; consider changing to 'let' constant}}
  x[nonmutating: 4] = 17
  return x
}

func testSubscript() -> [Int] {
  var x = [1,2,3] // ok
  x[1] = 27
  return x
}

func testSubscriptNeverMutated() -> Int {
  var x = [1,2,3] // expected-warning {{variable 'x' was never mutated; consider changing to 'let' constant}}
  return x[1]
}

func testTuple(_ x : Int) -> Int {
  var x = x
  var y : Int  // Ok, stored by a tuple
  
  (x, y) = (1,2)
  _ = x
  _ = y
  return y
}

struct TestComputedPropertyStruct {
  var x : Int {
    get {} // expected-error {{missing return in a function expected to return 'Int'}}
    nonmutating set {}
  }
}

func testComputedProperty() {
  let v = TestComputedPropertyStruct()
  v.x = 42
  
  var v2 = TestComputedPropertyStruct()  // expected-warning {{variable 'v2' was never mutated; consider changing to 'let' constant}} {{3-6=let}}
  v2.x = 42
}

func testComputedProperty2() {
  // expected-warning @+1 {{variable 'dest' was never mutated; consider changing to 'let' constant}} {{3-6=let}}
  var dest = UnsafeMutablePointer<Int>(bitPattern: 0)!

  dest[0] = 0
}

func testTuple() {
  var tup : (x:Int, y:Int)  // expected-warning {{variable 'tup' was written to, but never read}}
  tup.x = 1
  
  var tupUnused : (x:Int, y:Int)  // expected-warning {{variable 'tupUnused' was never used; consider replacing with '_' or removing it}}
  
  let tupUnused2 : (x:Int, y:Int)  // expected-warning {{immutable value 'tupUnused2' was never used; consider replacing with '_' or removing it}}

  // <rdar://problem/20927707> QoI: 'variable was never mutated' noisy when only part of a destructured tuple is mutated
  // don't warn about tupB being changeable to a 'let'.
  var (tupA, tupB) = (1,2)  // expected-warning {{variable 'tupA' was written to, but never read}}
  tupA += tupB

}

func halfUsedTuple() -> (Int, Int) {
  var (a, b) = (1, 2) // expected-warning {{variable 'b' was never used; consider replacing with '_' or removing it}}
  a += 1
  let (c, d) = (1, 2) // expected-warning {{immutable value 'd' was never used; consider replacing with '_' or removing it}}
  return (a, c)
}

/// <rdar://problem/20911927> False positive in the "variable was never mutated" warning with IUO
func testForceValueExpr() {
  var a: X! = nil  // no warning, mutated through the !
  a!.g()
}

//// <rdar://problem/20894455> "variable was never mutated" diagnostic does not take #if into account
func markUsed<T>(_ t: T) {}
func testBuildConfigsTrue() {
  let abc = 42
  var mut = 18 // expected-warning {{variable 'mut' was written to, but never read}}
  var read = 24
  #if true
    mut = abc
    read = 1
    markUsed(read)
  #endif
}

func testBuildConfigsFalse() {
  let abc = 42 // expected-warning {{initialization of immutable value 'abc' was never used; consider replacing with assignment to '_' or removing it}}
  var mut = 18 // expected-warning {{initialization of variable 'mut' was never used; consider replacing with assignment to '_' or removing it}}
  var read = 24 // expected-warning {{initialization of variable 'read' was never used; consider replacing with assignment to '_' or removing it}}
  #if false // false IfConfigs don't appear in SIL, so we can't track this
    mut = abc
    read = 1
    markUsed(read)
  #endif
}

func testGuardWithPoundIfTrue(x: Int?) {
  guard let x = x else { return }
  
  #if true
    markUsed(x)
  #endif
}

func testGuardWithPoundIfFalse(x: Int?) {
  guard let x = x else { return } // expected-warning {{value 'x' was defined but never used; consider replacing with boolean test}}
  
  #if false
    markUsed(x)
  #endif
}

// <rdar://problem/21091625> Bogus 'never mutated' warning when protocol variable is mutated only by mutating method
protocol Fooable {
  mutating func mutFoo()
  func immutFoo()
  var mutatingProperty: Int { mutating get mutating set }
  var nonmutatingProperty: Int { nonmutating get nonmutating set }
}

func testOpenExistential(_ x: Fooable,
                         y: Fooable) {
  var x = x
  let y = y
  x.mutFoo()
  y.immutFoo()
}

func testOpenExistential_mutatingProperty_get(p: Fooable) {
  var x = p
  _ = x.mutatingProperty
}

func testOpenExistential_mutatingProperty_set(p: Fooable) {
  var x = p
  x.mutatingProperty = 4
}

func testOpenExistential_nonmutatingProperty_get(p: Fooable) {
  var x = p // expected-warning {{variable 'x' was never mutated; consider changing to 'let' constant}}
  _ = x.nonmutatingProperty
}

func testOpenExistential_nonmutatingProperty_set(p: Fooable) {
  var x = p // expected-warning {{variable 'x' was never mutated; consider changing to 'let' constant}}
  x.nonmutatingProperty = 4
}

func couldThrow() throws {}

func testFixitsInStatementsWithPatterns(_ a : Int?) {
  if var b = a,    // expected-warning {{variable 'b' was never mutated; consider changing to 'let' constant}} {{6-9=let}}
      var b2 = a {   // expected-warning {{variable 'b2' was never mutated; consider changing to 'let' constant}} {{7-10=let}}
    _ = b
    _ = b2
  }
  
  for var b in [42] { // expected-warning {{variable 'b' was never mutated; consider removing 'var' to make it constant}} {{7-11=}}
    _ = b
  }

  do {
    try couldThrow()
  } catch var err {  // expected-warning {{variable 'err' was never mutated; consider changing to 'let' constant}} {{11-14=let}}
    _ = err
  }

  switch a {
    case var b: _ = b  // expected-warning {{variable 'b' was never mutated; consider changing to 'let' constant}} {{10-13=let}}
  }
}

// <rdar://22774938> QoI: "never used" in an "if let" should rewrite expression to use != nil
func testIfLets(_ a : Int?, b : Any) {
  
  if let x = a {
    markUsed(x)
  }
  
  if let x = a { // expected-warning {{value 'x' was defined but never used; consider replacing with boolean test}}
    markUsed(a)
  }
  
  if true == true, let x = a { // expected-warning {{immutable value 'x' was never used; consider replacing with '_' or removing it}} {{20-25=_}}
  }
  
  var z: Int // expected-warning {{variable 'z' was written to, but never read}}
  if let x = a, let y = a {  // expected-warning {{immutable value 'x' was never used; consider replacing with '_' or removing it}} {{6-11=_}}
    z = y
  }
  
  if let x = a, let y = a {  // expected-warning {{immutable value 'x' was never used; consider replacing with '_' or removing it}} {{6-11=_}}
    markUsed(y)
  }

  // Simple case, insert a comparison with nil.
  if let x = a {  // expected-warning {{value 'x' was defined but never used; consider replacing with boolean test}} {{6-14=}} {{15-15= != nil}}
  }

  // General case, need to insert parentheses.
  if let x = a ?? a {}  // expected-warning {{value 'x' was defined but never used; consider replacing with boolean test}} {{6-14=(}} {{20-20=) != nil}}
  
  // Special case, we can turn this into an 'is' test.
  if let x = b as? Int {  // expected-warning {{value 'x' was defined but never used; consider replacing with boolean test}} {{6-14=}} {{16-19=is}}
  }

  // SR-1112

  let xxx: Int? = 0

  if let yyy = xxx { } // expected-warning{{with boolean test}} {{6-16=}} {{19-19= != nil}}

  var zzz: Int? = 0
  zzz = 1

  if let yyy = zzz { } // expected-warning{{with boolean test}} {{6-16=}} {{19-19= != nil}}

  if let yyy = zzz ?? xxx { } // expected-warning{{with boolean test}} {{6-16=(}} {{26-26=) != nil}}

}

func testOptionalWrite() {
  let a: Int? // expected-note {{constant defined here}}
  // expected-warning@-1 {{immutable value 'a' was never used; consider removing it}}
  a? = 0 // expected-error {{constant 'a' used before being initialized}}
  
  var b: Int? // expected-warning {{variable 'b' was written to, but never read}}
  b? = 0
  
  var c: Int? = 1 // expected-warning {{variable 'c' was written to, but never read}}
  c? = 0
  
  let d: Int? // expected-note {{constant defined here}}
  // expected-warning@-1 {{immutable value 'd' was never used; consider removing it}}
  d! = 0 // expected-error {{constant 'd' used before being initialized}}
  
  var e: Int? // expected-warning {{variable 'e' was written to, but never read}}
  e! = 0
  
  var f: Int? = 1 // expected-warning {{variable 'f' was written to, but never read}}
  f! = 0
}


func testPatterns() {
  let a = 4 // expected-warning {{initialization of immutable value 'a' was never used; consider replacing with assignment to '_' or removing it}} {{3-8=_}}
  var ( b ) = 6 // expected-warning {{initialization of variable 'b' was never used; consider replacing with assignment to '_' or removing it}} {{3-12=_}}
  var c: Int = 4 // expected-warning {{variable 'c' was never used; consider replacing with '_' or removing it}} {{7-8=_}}
  let (d): Int = 9 // expected-warning {{immutable value 'd' was never used; consider replacing with '_' or removing it}} {{8-9=_}}
}

let optionalString: String? = "check"
if let string = optionalString {}  // expected-warning {{value 'string' was defined but never used; consider replacing with boolean test}} {{4-17=}} {{31-31= != nil}}

let optionalAny: Any? = "check"
if let string = optionalAny as? String {} // expected-warning {{value 'string' was defined but never used; consider replacing with boolean test}} {{4-17=(}} {{39-39=) != nil}}

// Due to the complexities of global variable tracing, these will not generate warnings
let unusedVariable = ""
var unNeededVar = false
if unNeededVar {}

for i in 0..<10 { // expected-warning {{immutable value 'i' was never used; consider replacing with '_' or removing it}} {{5-6=_}}
   print("")
}

// Tests fix to SR-2421
func sr2421() {
  let x: Int // expected-warning {{immutable value 'x' was never used; consider removing it}}
  x = 42
}

func sr2421Continued(a: Bool) {
  let x: Int // expected-warning {{immutable value 'x' was never used; consider removing it}}
  if a {
    x = 42
  } else {
    x = 12
  }
  
  let y: Int // expected-warning {{immutable value 'y' was never used; consider removing it}}
  switch a {
  case true:
    y = 1
  default:
    break
  }
  
  let z: Int
  z = 0
  markUsed(z)
}

// Tests fix to SR-964
func sr964() {
  var noOpSetter: String {
    get { return "" }
    set { } // No warning
  }
  var suspiciousSetter: String {
    get { return "" }
    set {
      print(suspiciousSetter) // expected-warning {{setter argument 'newValue' was never used, but the property was accessed}} expected-note {{did you mean to use 'newValue' instead of accessing the property's current value?}} {{13-29=newValue}}
    }
  }
  struct MemberGetterStruct {
    var suspiciousSetter: String {
      get { return "" }
      set {
        print(suspiciousSetter) // expected-warning {{setter argument 'newValue' was never used, but the property was accessed}} expected-note {{did you mean to use 'newValue' instead of accessing the property's current value?}} {{15-31=newValue}}
      }
    }
  }
  class MemberGetterClass {
    var suspiciousSetter: String {
      get { return "" }
      set {
        print(suspiciousSetter) // expected-warning {{setter argument 'newValue' was never used, but the property was accessed}} expected-note {{did you mean to use 'newValue' instead of accessing the property's current value?}} {{15-31=newValue}}
      }
    }
  }
  var namedSuspiciousSetter: String {
    get { return "" }
    set(parameter) {
      print(namedSuspiciousSetter) // expected-warning {{setter argument 'parameter' was never used, but the property was accessed}} expected-note {{did you mean to use 'parameter' instead of accessing the property's current value?}} {{13-34=parameter}}
    }
  }
  var okSetter: String {
    get { return "" }
    set { print(newValue) } // No warning
  }
  var multiTriggerSetter: String {
    get { return "" }
    set {
      print(multiTriggerSetter) // expected-warning {{setter argument 'newValue' was never used, but the property was accessed}} expected-note {{did you mean to use 'newValue' instead of accessing the property's current value?}} {{13-31=newValue}}
      print(multiTriggerSetter)
    }
  }
}
struct MemberGetterExtension {}
extension MemberGetterExtension {
  var suspiciousSetter: String {
    get { return "" }
    set {
      print(suspiciousSetter) // expected-warning {{setter argument 'newValue' was never used, but the property was accessed}} expected-note {{did you mean to use 'newValue' instead of accessing the property's current value?}} {{13-29=newValue}}
    }
  }
}

func testLocalFuncNotCalled() {
  var unusedVar = 0
  // expected-warning@-1 {{initialization of variable 'unusedVar' was never used; consider replacing with assignment to '_' or removing it}}

  var notMutatedVar = 0
  // expected-warning@-1 {{initialization of variable 'notMutatedVar' was never used; consider replacing with assignment to '_' or removing it}}

  var mutatedVar = 0
  // expected-warning@-1 {{initialization of variable 'mutatedVar' was never used; consider replacing with assignment to '_' or removing it}}

  func localFunc() {
    _ = notMutatedVar
    mutatedVar = 1 // TODO: Mark the unused capture
  }
}

func testLocalFunc() {
  var unusedVar = 0
  // expected-warning@-1 {{initialization of variable 'unusedVar' was never used; consider replacing with assignment to '_' or removing it}}
  
  var notMutatedVar = 0
  // expected-warning@-1 {{variable 'notMutatedVar' was never mutated; consider changing to 'let' constant}}
  
  var mutatedVar = 0
  // expected-warning@-1 {{variable 'mutatedVar' was written to, but never read}}
  
  func localFunc() {
    _ = notMutatedVar
    mutatedVar = 1
  }
  localFunc()
}

// False positive "'var' was never mutated" warning - rdar://60563962
func testForwardReferenceCapture() {
  func innerFunc() {
    x = 10
  }

  var x: Int = 1
  innerFunc()
  print(x)
}

// rdar://47240768 Expand the definition of "simple" pattern to variables bound in patterns
enum StringB {
  case simple(b: Bool)
  case tuple(b: (Bool, Bool))
  case optional(b: Bool?)
}
func testVariablesBoundInPatterns(stringB: StringB) {

  // Because Swift enables all kinds of creative binding forms, make sure that
  // variable patterns occuring directly under a `let` or `var` have that
  // introducer stripped by the fixit. All other cases are currently too
  // complicated for the VarDeclUsageChecker.
  switch stringB {
  case .simple(b: let b) where false:
    // expected-warning@-1 {{immutable value 'b' was never used; consider replacing with '_' or removing it}} {{19-24=_}}
    // expected-note@-2 {{condition always evaluates to false}}
    // expected-warning@-3 {{will never be executed}}
    break
  case .simple(b: var b) where false:
    // expected-warning@-1 {{variable 'b' was never used; consider replacing with '_' or removing it}} {{19-24=_}}
    // expected-note@-2 {{condition always evaluates to false}}
    // expected-warning@-3 {{will never be executed}}
    break
  case var .simple(b: b):
    // expected-warning@-1 {{variable 'b' was never used; consider replacing with '_' or removing it}} {{23-24=_}}
    break
  case .tuple(b: let (b1, b2)) where false:
    // expected-warning@-1 {{immutable value 'b1' was never used; consider replacing with '_' or removing it}} {{23-25=_}}
    // expected-warning@-2 {{immutable value 'b2' was never used; consider replacing with '_' or removing it}} {{27-29=_}}
    // expected-note@-3 {{condition always evaluates to false}}
    // expected-warning@-4 {{will never be executed}}
    break
  case .tuple(b: (let b1, let b2)) where false:
    // expected-warning@-1 {{immutable value 'b1' was never used; consider replacing with '_' or removing it}} {{19-25=_}}
    // expected-warning@-2 {{immutable value 'b2' was never used; consider replacing with '_' or removing it}} {{27-33=_}}
    // expected-note@-3 {{condition always evaluates to false}}
    // expected-warning@-4 {{will never be executed}}
    break
  case .tuple(b: (var b1, var b2)) where false:
    // expected-warning@-1 {{variable 'b1' was never used; consider replacing with '_' or removing it}} {{19-25=_}}
    // expected-warning@-2 {{variable 'b2' was never used; consider replacing with '_' or removing it}} {{27-33=_}}
    // expected-note@-3 {{condition always evaluates to false}}
    // expected-warning@-4 {{will never be executed}}
    break
  case var .tuple(b: (b1, b2)) where false:
    // expected-warning@-1 {{variable 'b1' was never used; consider replacing with '_' or removing it}} {{23-25=_}}
    // expected-warning@-2 {{variable 'b2' was never used; consider replacing with '_' or removing it}} {{27-29=_}}
    // expected-note@-3 {{condition always evaluates to false}}
    // expected-warning@-4 {{will never be executed}}
    break
  case .tuple(b: let b):
    // expected-warning@-1 {{immutable value 'b' was never used; consider replacing with '_' or removing it}} {{18-23=_}}
    break
  case .optional(b: let x?) where false:
    // expected-warning@-1 {{immutable value 'x' was never used; consider replacing with '_' or removing it}} {{25-26=_}}
    // expected-note@-2 {{condition always evaluates to false}}
    // expected-warning@-3 {{will never be executed}}
    break
  case .optional(b: let .some(x)) where false:
    // expected-warning@-1 {{immutable value 'x' was never used; consider replacing with '_' or removing it}} {{31-32=_}}
    // expected-note@-2 {{condition always evaluates to false}}
    // expected-warning@-3 {{will never be executed}}
    break
  case let .optional(b: x?):
    // expected-warning@-1 {{immutable value 'x' was never used; consider replacing with '_' or removing it}} {{25-26=_}}
    break
  case let .optional(b: .none):
    // expected-warning@-1 {{'let' pattern has no effect; sub-pattern didn't bind any variables}} {{8-12=}}
    break
  }
}

enum ExampleNestedEnum {
  case a(ExampleEnum), b
}

enum ExampleEnum {
  case a(Int), b(Int), c(String), d
}

func testSwitchUsage(e: ExampleEnum) {
  
  switch e {
  case .a:
    break
  case .b:
    break
  case .c:
    break
  case .d:
    break
  }
  
  switch e {
  case .a(let i):
    // expected-warning@-1 {{immutable value 'i' was never used; consider replacing with '_' or removing it}}
    break
  case .b(let i):
    // expected-warning@-1 {{immutable value 'i' was never used; consider replacing with '_' or removing it}}
    break
  case .c(let s):
    // expected-warning@-1 {{immutable value 's' was never used; consider replacing with '_' or removing it}}
    break
  case .d:
    break
  }
  
  switch e {
  case .a(let i):
    // expected-warning@-1 {{immutable value 'i' was never used; consider replacing with '_' or removing it}}
    break
  case .b(let i):
    print(i)
    break
  case .c(let s):
    // expected-warning@-1 {{immutable value 's' was never used; consider replacing with '_' or removing it}}
    break
  case .d:
    break
  }
  
  switch e {
  case .a(let i):
    // expected-warning@-1 {{immutable value 'i' was never used; consider replacing with '_' or removing it}}
    break
  case .b(var i):
    // expected-warning@-1 {{variable 'i' was never mutated; consider changing to 'let' constant}}
    print(i)
    break
  case .c(let s):
    // expected-warning@-1 {{immutable value 's' was never used; consider replacing with '_' or removing it}}
    break
  case .d:
    break
  }
}

func testSwitchUsage(e: ExampleNestedEnum) {
  switch(e) {
  case .a(let e2):
    switch (e2) {
    case .a:
      break
    default:
      break
    }
  default:
    break
  }
  
  switch(e) {
  case .a(var e2):
    switch (e2) {
    case .a:
      e2 = .a(2)
      break
    default:
      break
    }
  default:
    break
  }
}

func testTupleValueSwitch() -> Int {
  let x = true
  let y = false
  switch (x, y) {
  case (false, false):
    return 0
  case (false, true):
    return 1
  case (true, false):
    return 1
  case (true, true):
    return 2
  }
}

func testBindingTupleValueSwitch() -> Int {
  let x = true
  let y = false
  switch (x, y) {
  case (false, let a):
    // expected-warning@-1 {{immutable value 'a' was never used; consider replacing with '_' or removing it}}
    return 0
  case (true, let b):
    return b ? 0 : 1
  }
}

// TODO: product more narrow warnings and fixits for unused conditional bindings in switch
func testSwitchBindingCondition(a: Int) {
  
  // Example where x is not bound within the case block
  switch a {
  case let x where x > 0:
    break
  case let x where x < 0:
    break
  default:
    break
  }
  
  // Should provide more narrow warnings and fixits
  // to deduce here:
  switch a {
  case _ where a > 0:
    break
  case _ where a < 0:
    break
  default:
    break
  }
}

var closure_var_unused: () -> Int = {
  var unused = 42 // expected-warning {{initialization of variable 'unused' was never used; consider replacing with assignment to '_' or removing it}}
  return 12
}

var closure_let_unused: () -> Int = {
  let unused = 42 // expected-warning {{initialization of immutable value 'unused' was never used; consider replacing with assignment to '_' or removing it}}
  return 12
}

var closure_var_never_mutated: () -> Int = {
  var unmutated = 42 // expected-warning {{variable 'unmutated' was never mutated; consider changing to 'let' constant}}
  return unmutated
}

func nested_closures() {
  var _: () -> Int = {
    var unused = 42 // expected-warning {{initialization of variable 'unused' was never used; consider replacing with assignment to '_' or removing it}}
    return 12
  }
  
  var _: () -> Int = {
    let unused = 42 // expected-warning {{initialization of immutable value 'unused' was never used; consider replacing with assignment to '_' or removing it}}
    return 12
  }
  
  var _: () -> Int = {
    var unmutated = 42 // expected-warning {{variable 'unmutated' was never mutated; consider changing to 'let' constant}}
    return unmutated
  }
}

class A {
  lazy var lazyvar_var_unused: Int = {
    var unused = 42 // expected-warning {{initialization of variable 'unused' was never used; consider replacing with assignment to '_' or removing it}}
    return 12
  }()
  
  lazy var lazyvar_let_unused: Int = {
    let unused = 42 // expected-warning {{initialization of immutable value 'unused' was never used; consider replacing with assignment to '_' or removing it}}
    return 12
  }()
  
  lazy var lazyvar_var_never_mutated: Int = {
    var unmutated = 42 // expected-warning {{variable 'unmutated' was never mutated; consider changing to 'let' constant}}
    return unmutated
  }()
  
  var closure_var_unused: () -> Int = {
    var unused = 42 // expected-warning {{initialization of variable 'unused' was never used; consider replacing with assignment to '_' or removing it}}
    return 12
  }
  
  var closure_let_unused: () -> Int = {
    let unused = 42 // expected-warning {{initialization of immutable value 'unused' was never used; consider replacing with assignment to '_' or removing it}}
    return 12
  }
  
  var closure_var_never_mutated: () -> Int = {
    var unmutated = 42 // expected-warning {{variable 'unmutated' was never mutated; consider changing to 'let' constant}}
    return unmutated
  }
  
  func nested_closures() {
    var _: () -> Int = {
      var unused = 42 // expected-warning {{initialization of variable 'unused' was never used; consider replacing with assignment to '_' or removing it}}
      return 12
    }
    
    var _: () -> Int = {
      let unused = 42 // expected-warning {{initialization of immutable value 'unused' was never used; consider replacing with assignment to '_' or removing it}}
      return 12
    }
    
    var _: () -> Int = {
      var unmutated = 42 // expected-warning {{variable 'unmutated' was never mutated; consider changing to 'let' constant}}
      return unmutated
    }
  }
}

struct Y {
  var v: Int
}

class Z {
  let a: Int
  init(y: Y) {
    a = y.v
  }
}

func testPassingLetStructClassReturn() -> Z {
  let y = Y(v: 3)
  let z = Z(y: y)
  return z
}

func testPassingLetStructNoClassReturn() {
  let y = Y(v: 3)
  let z = Z(y: y) // expected-warning {{initialization of immutable value 'z' was never used; consider replacing with assignment to '_' or removing it}}
}

func testPassingVarStructClassReturn() -> Z {
  var y = Y(v: 3) // expected-warning {{variable 'y' was never mutated; consider changing to 'let' constant}}
  let z = Z(y: y)
  return z
}

func testPassingVarStructNoClassReturn() {
  var y = Y(v: 3) // expected-warning {{variable 'y' was never mutated; consider changing to 'let' constant}}
  let z = Z(y: y) // expected-warning {{initialization of immutable value 'z' was never used; consider replacing with assignment to '_' or removing it}}
}

struct IfTester {
  var v: Int
  func doSomething() { }
  func doSomethingElse() { }
}

func testIfAvailable(x: IfTester?) {
  let unwrappedX = x!
  if #available(OSX 10.13, iOS 11.0, tvOS 11.0, watchOS 4.0, *) {
    unwrappedX.doSomething()
  } else {
    unwrappedX.doSomethingElse()
  }
}

struct LetPropertyInGetter {
  var values: [Any?] = [1]
  public var property: Int? {
    get {
      let value = values[0]
      return value as? Int
    }
    set {
      values[0] = newValue as Any
    }
  }
}

struct VarPropertyInGetter {
  var values: [Any?] = [1]
  public var property: Int? {
    get {
      var value = values[0] // expected-warning {{variable 'value' was never mutated; consider changing to 'let' constant}}
      return value as? Int
    }
    set {
      values[0] = newValue as Any
    }
  }
}

struct IfLetInGetter {
  struct Nested {
    var string: String
  }
  
  var x: Nested?
  
  public var letRead: String {
    if let u = x {
      return u.string
    } else {
      return ""
    }
  }
  
  public var varRead: String {
    if var u = x {
      u.string += "."
      return ""
    } else {
      return ""
    }
  }
  
  public var varReadModified: String {
    if var u = x { // expected-warning {{variable 'u' was never mutated; consider changing to 'let' constant}}
      return u.string
    } else {
      return ""
    }
  }
}

struct ThrowInFunc {
  
  var structError: Error?
  
  public func useThrow() throws -> Bool {
    let error = structError
    if let e = error {
      throw e
    } else {
      return true
    }
  }
}

func substringReturn() -> Substring {
  let s = "hello"
  return s[...]
}


func elementReturn() -> Int {
  let i = [1]
  return i[0]
}

struct AssignInIf {
  
  var a: Bool
  var b: Bool
  
  func test() -> String {
    let result: String
    if (a) {
      result = "a"
    } else if (b) {
      result = "b"
    } else {
      result = "c"
    }
    return result
  }
}

struct MutableThrow {
  mutating func mutate() throws { }
}

func testTryMutate() throws {
  var x = MutableThrow()
  try x.mutate()
}

class GuardPropertyAssign {
  var value: Int
  init?(_ input: Any) {
    guard let integer = input as? Int else {
      return nil
    }
    self.value = integer
  }
}

class Object {
  func foo() { }
}

class GuardClassTest {
  
  var object: Object?
  
  func invalidate() {
    guard let x = self.object else { return }
    x.foo()
  }
}

class ACast { }

struct IfLetEqualityTester {
  
  var x: Any
  
  func unbox(_ value: Any) -> Bool? {
    
    if let number = value as? ACast {
      if number === x as! ACast {
        return true
      }
    }
    return false
  }
}

func testClosureUnread() {
  let body = { () -> Void in // expected-warning {{initialization of immutable value 'body' was never used; consider replacing with assignment to '_' or removing it}}
    print("")
  }
}

func testClosureRead() {
  let body = { () -> Void in
    print("")
  }
  
  body()
}

func testClosureUnreadUnmodified() {
  var body = { () -> Void in // expected-warning {{initialization of variable 'body' was never used; consider replacing with assignment to '_' or removing it}}
    print("")
  }
}

func testClosureUnmodified() {
  var body = { () -> Void in // expected-warning {{variable 'body' was never mutated; consider changing to 'let'}}
    print("")
  }
  body()
}

func testReturnInTuple() -> (Bool, Int) {
  let i = 20
  return (true, i)
}

func testTupleAssignUsed() -> Int {
  let (a, b) = (1, true)
  if b {
    return a
  }
}

func testTupleAssignUsedModified() -> Int {
  var (a, b) = (1, true)
  if b {
    a += 1
    return a
  }
}

func testDeclareInIf() {
  var a = [1, 2] // expected-warning {{variable 'a' was written to, but never read}}
  #if arch(arm64)
  let i = 0
  #else
  let i = 1
  #endif
  a[i] = 0
}

enum Switchable {
  case a, b
}

func testSwitch(y: ((Switchable) -> ()) -> ()) -> Int {
  
  do {
    var x = 0
    y { (v) -> () in
      switch v {
      case .a:
        break
      case .b:
        x += 1
      }
    }
    return x
  }
}

struct TestPointerStruct {
  
  typealias Counters = (UInt32, UInt32)
  
  var counters: Counters = (UInt32(0), UInt32(0))
  
  func testPointer(_ index: Int) -> UInt32 {
    var tmpCounters = counters
    
    let counter: UInt32 = withUnsafePointer(to: &tmpCounters) { ptr in
      return ptr.withMemoryRebound(to: UInt32.self, capacity: 64) { buf in
        return buf[index]
      }
    }
    return counter
  }
}

public func testSwitchGeneric<T>(first: (T?, Bool), next: @escaping (T) -> T?) -> T? {
  switch first {
  case (let value, true):
    return value
  case (let value?, _):
    let nextValue = next(value)
    return nextValue
  case (nil, _):
    return nil
  }
}


func trailClosure(c: (Int) -> (Bool, Int)) -> (Bool, Int) {
  return c(5)
}

func TestTupleMutate() -> (Bool, Int) {
  var (a, b): (Bool, Int) = trailClosure { i in
    return (true, i)
  }
  a = false
  return (a, b)
}

// ASTScope assertion
func patternBindingWithTwoEntries() {
  let x2 = 1, (_, _) = (1, 2)
  // expected-warning@-1 {{immutable value 'x2' was never used; consider replacing with '_' or removing it}}
}

var implicitGet: Int {
  var zzz = 0
  // expected-warning@-1 {{initialization of variable 'zzz' was never used; consider replacing with assignment to '_' or removing it}}
  // For the purpose of this test, any other function attribute work as well.
  @inline(__always)
  func foo() {}
  return 0
}

class SelfRefProperties {
  var setter: Int {
    get {
      return 42
    }
    set {
      markUsed(setter)  // expected-warning {{setter argument 'newValue' was never used, but the property was accessed}} expected-note {{did you mean to use 'newValue' instead of accessing the property's current value?}}
      var unused = setter + setter // expected-warning {{initialization of variable 'unused' was never used; consider replacing with assignment to '_' or removing it}} {{7-17=_}}
    }
  }
}

func takeTrailingClosure(_ fn: () -> ()) -> Int { return 0 }
func disambiguateGetSet4Attr() {
  func set(_ x: Int, fn: () -> ()) {}
  var newValue: Int = 0
  // expected-warning@-1 {{variable 'newValue' was never mutated; consider changing to 'let' constant}}
  var a: Int = takeTrailingClosure {
    @inline(__always)
    func foo() {}
    set(newValue) {}
  }
  // Check that the property is read-write.
  a = a + 42
}


// FIXME: Moved from associated_types

protocol TestAssocType {
  associatedtype AssocType
}

struct TestAssocTypeStruct : TestAssocType {
  typealias AssocType = Int
  func blah() {
    var a : AssocType // expected-warning {{variable 'a' was never used; consider replacing with '_' or removing it}} {{9-10=_}}
  }
}

@dynamicCallable
struct Callable {
  func dynamicallyCall(withArguments arguments: [Int]) -> Int {
    return arguments.count
  }
}

@dynamicCallable
struct Throwing {
  func dynamicallyCall(withArguments arguments: [String]) throws -> Int {
    return arguments.count
  }
}

func testCallable(a: Callable, b: Throwing) {
  _ = a()
  let a1 = a(1, 2, 3, 4) // expected-warning {{initialization of immutable value 'a1' was never used}}
  
  _ = try? b()
  let b1 = try! b("hello", "world") // expected-warning {{initialization of immutable value 'b1' was never used}}
}

// SR-4082
func foo2() {
  let x = 5
  if x < 0, let x = Optional(1) { }
  // expected-warning@-1 {{immutable value 'x' was never used; consider replacing with '_' or removing it}}
  // expected-warning@-2 {{will never be executed}}
  // expected-note@-3 {{condition always evaluates to false}}
}

class ForwardReference {
  var x: Int = 0
  
  func test() {
    x = 0
    var x: Float = 0.0 // expected-warning{{variable 'x' was never used; consider replacing with '_' or removing it}}
  }
}

func localtest() {
  func shadowbug() { 
    var Foo = 10
    // expected-warning@-1 {{initialization of variable 'Foo' was never used; consider replacing with assignment to '_' or removing it}}
    func g() {
      struct S {
        // FIXME: Swap these two lines to crash our broken lookup.
        typealias Foo = Int
        var x : Foo
      }
    }
  }
}

enum EnumA {
  case A(Int)
  case B(Bool)
  case C
  case D
}

enum EnumB {
  case A
  case B
}

func s(a: EnumA, b: EnumB) {
  switch (a, b) {
  case (.A(_), .A):
    break
  case (.A(_), .B):
    break
    
  case (.B(_), let b):
    // expected-warning@-1 {{immutable value 'b' was never used; consider replacing with '_' or removing it}}
    break
    
  case (.C, _), (.D, _):
    break
  }
}

func fullNameTest() {
  let x = 123 // expected-warning {{never used}}
  func x() {}
}

// SR-6726
var sr6726var: Int?

func sr6726() {
  guard let bar = sr6726var else {
    return
  }
  let sr6726var = String(bar) // expected-warning {{initialization of immutable value 'sr6726var' was never used; consider replacing with assignment to '_' or removing it}}
}


func nested_scope_2() {
  do {
    let x = 11// expected-warning {{initialization of immutable value 'x' was never used; consider replacing with assignment to '_' or removing it}}
    do {
      let _ = x
      let x = 111 // expected-warning {{initialization of immutable value 'x' was never used; consider replacing with assignment to '_' or removing it}}
    }
  }
  let x = 1  // expected-warning {{initialization of immutable value 'x' was never used; consider replacing with assignment to '_' or removing it}}
}

func nested_scope_3() {
  let x = 1 // expected-warning {{initialization of immutable value 'x' was never used; consider replacing with assignment to '_' or removing it}}
  do {
    do {
      let x = 111 // expected-warning {{initialization of immutable value 'x' was never used; consider replacing with assignment to '_' or removing it}}
    }
    let x = 11 // expected-warning {{initialization of immutable value 'x' was never used; consider replacing with assignment to '_' or removing it}}
  }
}

class Ty {
  var v : Int?
  
  func fn() {
    let _ = v
    let v = 1 // expected-warning {{initialization of immutable value 'v' was never used; consider replacing with assignment to '_' or removing it}}
  }
}

let g = 0
func file_scope_1() {
  let _ = g
  let g = 1 // expected-warning {{initialization of immutable value 'g' was never used; consider replacing with assignment to '_' or removing it}}
}

func module_scope_1() {
  let _ = print // Legal use of func print declared in Swift Standard Library
  let print = "something" // expected-warning {{initialization of immutable value 'print' was never used; consider replacing with assignment to '_' or removing it}}
}

// SR-7660
class TestC {
  var variable: Int?
  func f() {
    guard let _ = variable else { return }
    let variable = 1 // expected-warning {{initialization of immutable value 'variable' was never used; consider replacing with assignment to '_' or removing it}}
  }
}

func test_lambda() {
  // A simple closure.
  var a = { (value: Int) -> () in markUsed(value+1) }
  // expected-warning@-1 {{initialization of variable 'a' was never used; consider replacing with assignment to '_' or removing it}}
  
  // A recursive lambda.
  var fib = { (n: Int) -> Int in
    // expected-warning@-1 {{variable 'fib' was never mutated; consider changing to 'let' constant}}
    if (n < 2) {
      return n
    }
    
    return fib(n-1)+fib(n-2)
  }
}

var nestedSelfRef = {
  var recursive = { nestedSelfRef() }
  // expected-warning@-1 {{variable 'recursive' was never mutated; consider changing to 'let' constant}}
  recursive()
}

class Writer {}

class MyCls {
  func something() {}
  
  func test() {
    // expected-warning @+1 {{initialization of immutable value 'self' was never used}}
    let `self` = Writer() // Even if `self` is shadowed,
    something() // this should still refer `MyCls.something`.
  }
}

func barFunc() {
  var x : () = { () -> () in
    // expected-warning@-1 {{variable 'x' was never used; consider replacing with '_' or removing it}}
    return
  } ()
  
  var y : () = { () -> () in
    // expected-warning@-1 {{variable 'y' was never used; consider replacing with '_' or removing it}}
    return
  } ()
}

@discardableResult
func calls(_ arg: (Int) -> Int, _ x: Int) -> Int {
  return arg(x)
}

func doStuff(_ fn : @escaping () -> Int) {}
func foo() {
  let i = 42 // expected-warning {{initialization of immutable value 'i' was never used; consider replacing with assignment to '_' or removing it}}
  doStuff { [weak i] in i! }
  // expected-warning@-1 {{variable 'i' was never mutated; consider changing to 'let' constant}}
  // expected-error@-2 {{'weak' may only be applied to class and class-bound protocol types, not 'Int'}}
}

func capture_list_no_captures_param(x: Int) {
  calls({ [x] in $0 }, 0) // expected-warning {{capture 'x' was never used}}
}

func capture_list_no_captures() {
  let x = 100
  calls({ [x] in $0 }, 0) // expected-warning {{capture 'x' was never used}}
}

func capture_list_multiple_captures() {
  let a = 100
  calls({ [a] in $0 }, 0) // expected-warning {{capture 'a' was never used}}
  calls({ [a] in a + $0 }, 0)
  
  let b = 100
  calls({ [b] in b + $0 }, 0)
  calls({ [b] in $0 }, 0) // expected-warning {{capture 'b' was never used}}
  calls({ [b] in b + $0 }, 0)
  
  let c = 1
  let d = 0
  calls({ [c, d] in c + $0 }, 0) // expected-warning {{capture 'd' was never used}}
  calls({ [c, d] in d + $0 }, 0) // expected-warning {{capture 'c' was never used}}
  
}

func capture_list_param_used(x: Int) {
  calls({ [x] in x + $0 }, 0)
}

func capture_list() {
  let x = 100
  calls({ [x] in x + $0 }, 0)
  
  let y = 10
  calls({ [x, y] in x + $0 }, 0) // expected-warning {{capture 'y' was never used}}
  
  let z = 1
  calls({ [x, y, z] in x + $0 }, 0) // expected-warning {{capture 'y' was never used}} expected-warning {{capture 'z' was never used}}
  calls({ [x, y, z] in y + $0 }, 0) // expected-warning {{capture 'x' was never used}} expected-warning {{capture 'z' was never used}}
  calls({ [x, y, z] in z + $0 }, 0) // expected-warning {{capture 'x' was never used}} expected-warning {{capture 'y' was never used}}
}

func testUnusedInFor() {
  for _ in 0..<10 {
    var y = 300 // expected-warning {{variable 'y' was written to, but never read}}
    y += 1
  }
}

// <rdar://problem/19382878> Introduce new x? pattern
switch Optional(42) {
case let x?: break // expected-warning{{immutable value 'x' was never used; consider replacing with '_' or removing it}} {{10-11=_}}
case nil: break
}

for (var x) in 0...100 {} // expected-warning{{variable 'x' was never used; consider replacing with '_' or removing it}}
for var x in 0...100 {}  // rdar://20167543 expected-warning{{variable 'x' was never used; consider replacing with '_' or removing it}}

// rdar://problem/32390726 - Bad Diagnostic: Don't suggest `var` to `let` when binding inside for-statement
for var i in 0..<10 { // expected-warning {{variable 'i' was never mutated; consider removing 'var' to make it constant}} {{5-9=}}
  _ = i + 1
}

func takes_closure(_ fn: () -> ()) {}
func testVarReadInClosure() {
  var b1 = 0 // expected-warning {{variable 'b1' was never mutated; consider changing to 'let' constant}}
  takes_closure {
    markUsed(b1)
  }
}
