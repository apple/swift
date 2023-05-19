// RUN: %target-typecheck-verify-swift

var t1 : Int
var t2 = 10
var t3 = 10, t4 = 20.0
var (t5, t6) = (10, 20.0)
var t7, t8 : Int
var t9, t10 = 20 // expected-error {{type annotation missing in pattern}}
var t11, t12 : Int = 20 // expected-error {{type annotation missing in pattern}}
var t13 = 2.0, t14 : Int
var (x = 123, // expected-error {{expected ',' separator}} {{7-7=,}} expected-error {{expected pattern}}
     y = 456) : (Int,Int)
var bfx : Int, bfy : Int

_ = 10

var self1 = self1
// expected-note@-1 2{{through reference here}}
// expected-error@-2 {{circular reference}}

var self2 : Int = self2
var (self3) : Int = self3
var (self4) : Int = self4

var self5 = self5 + self5
// expected-note@-1 2{{through reference here}}
// expected-error@-2 {{circular reference}}

var self6 = !self6
// expected-note@-1 2{{through reference here}}
// expected-error@-2 {{circular reference}}

var (self7a, self7b) = (self7b, self7a)
// expected-note@-1 2{{through reference here}}
// expected-error@-2 {{circular reference}}

var self8 = 0
func testShadowing() {
  var self8 = self8
  // expected-warning@-1 {{initialization of variable 'self8' was never used; consider replacing with assignment to '_' or removing it}}
}

var (paren) = 0
var paren2: Int = paren

struct Broken {
  var b : Bool = True // expected-error{{cannot find 'True' in scope}}
}

// rdar://16252090 - Warning when inferring empty tuple type for declarations
var emptyTuple = testShadowing()  // expected-warning {{variable 'emptyTuple' inferred to have type '()'}} \
                                  // expected-note {{add an explicit type annotation to silence this warning}} {{15-15=: ()}}

// rdar://15263687 - Diagnose variables inferenced to 'AnyObject'
var ao1 : AnyObject
var ao2 = ao1

var aot1 : AnyObject.Type
var aot2 = aot1          // expected-warning {{variable 'aot2' inferred to have type 'any AnyObject.Type', which may be unexpected}} \
                       // expected-note {{add an explicit type annotation to silence this warning}} {{9-9=: any AnyObject.Type}}


for item in [AnyObject]() {  // No warning in for-each loop.
  _ = item
}


// <rdar://problem/16574105> Type inference of _Nil very coherent but kind of useless
var ptr = nil // expected-error {{'nil' requires a contextual type}}

func testAnyObjectOptional() -> AnyObject? {
  let x = testAnyObjectOptional()
  return x
}

// https://github.com/apple/swift/issues/53912
// Warning for inferring an array of empty tuples

var arrayOfEmptyTuples = [""].map { print($0) } // expected-warning {{variable 'arrayOfEmptyTuples' inferred to have type '[()]'}} \
                                                // expected-note {{add an explicit type annotation to silence this warning}} {{23-23=: [()]}}

var maybeEmpty = Optional(arrayOfEmptyTuples) // expected-warning {{variable 'maybeEmpty' inferred to have type '[()]?'}} \
                                              // expected-note {{add an explicit type annotation to silence this warning}} {{15-15=: [()]?}}

var shouldWarnWithoutSugar = (arrayOfEmptyTuples as Array<()>) // expected-warning {{variable 'shouldWarnWithoutSugar' inferred to have type 'Array<()>'}} \
                                 // expected-note {{add an explicit type annotation to silence this warning}} {{27-27=: Array<()>}}

class SomeClass {}

// <rdar://problem/16877304> weak let's should be rejected
weak let V = SomeClass()  // expected-error {{'weak' must be a mutable variable, because it may change at runtime}}

let a = b ; let b = a
// expected-error@-1 {{circular reference}}
// expected-note@-2 {{through reference here}}
// expected-note@-3 {{through reference here}}
// expected-note@-4 {{through reference here}}
// expected-note@-5 {{through reference here}}
// expected-note@-6 {{through reference here}}

// <rdar://problem/17501765> Swift should warn about immutable default initialized values
let uselessValue : String?


func tuplePatternDestructuring(_ x : Int, y : Int) {
  let (b: _, a: h) = (b: x, a: y)
  _ = h
  
  // <rdar://problem/20392122> Destructuring tuple with labels doesn't work
  let (i, j) = (b: x, a: y)
  _ = i+j

  // <rdar://problem/20395243> QoI: type variable reconstruction failing for tuple types
  let (x: g1, a: h1) = (b: x, a: y)  // expected-error {{cannot convert value of type '(b: Int, a: Int)' to specified type '(x: Int, a: Int)'}}
}

// <rdar://problem/21057425> Crash while compiling attached test-app.
func test21057425() -> (Int, Int) {
  let x: Int = "not an int!", y = 0 // expected-error{{cannot convert value of type 'String' to specified type 'Int'}}
  return (x, y)
}

// rdar://problem/21081340
func test21081340() {
  func foo() { }
  let (x: a, y: b): () = foo() // expected-error{{tuple pattern has the wrong length for tuple type '()'}}
}

// <rdar://problem/22322266> Swift let late initialization in top level control flow statements
if true {
  let s : Int
  s = 42  // should be valid.
  _ = s
}

// ASTScope assertion
func patternBindingWithTwoEntries() {
  let x2 = 1, (_, _) = (1, 2)
  // expected-warning@-1 {{immutable value 'x2' was never used; consider replacing with '_' or removing it}}
}
