// RUN: %target-swift-frontend -typecheck %s -module-name themodule -enable-source-import -I %S/../decl/enum -sdk "" -verify -show-diagnostics-after-fatal -verify-ignore-unknown

// -verify-ignore-unknown is for
// <unknown>:0: error: unexpected note produced: did you forget to set an SDK using -sdk or SDKROOT?
// <unknown>:0: error: unexpected note produced: use "xcrun swiftc" to select the default macOS SDK installed with Xcode

import Swift
import nonexistentimport  // expected-error {{no such module 'nonexistentimport'}}

//===----------------------------------------------------------------------===//
// Test imported names
//===----------------------------------------------------------------------===//

// Imported from swift stdlib.
var importedtype : Int

// Imported from enumtest module.
import enum enumtest.unionSearchFlags
var importedunion: unionSearchFlags = .backwards


// This shouldn't be imported from data.
var notimported : MaybeInt // expected-error {{cannot find type 'MaybeInt' in scope}}

//===----------------------------------------------------------------------===//
// Name lookup stress test
//===----------------------------------------------------------------------===//

var callee1 : () -> (Int,Int,Int)           // Takes nothing, returns tuple.

func test_shadowing() {
  // Shadow Int.
  enum Int { case xyz; case abc }
  // We get the shadowed version of Int.
  var _ : Int = .abc
}

func unknown_member() {
  var error = Swift.nonexistent_member // expected-error {{module 'Swift' has no member named 'nonexistent_member'}}
}

//===----------------------------------------------------------------------===//
// varname Processing
//===----------------------------------------------------------------------===//

var varname1 : (a : Int, b : Int)

// Not very useful, but it is allowed.
var (varname2_a, varname2_b) : (a : Int, b : Int) = varname1


func test_varname_binding() {
  var c = (4, 5)
  var (d, e) = (c.1, c.0)
  var ((), (g1, g2), h) = ((), (e, d), e)
  var (j, k, l) = callee1()
  var (m, n) = callee1() // expected-error{{'(Int, Int, Int)' is not convertible to '(Int, Int)', tuples have a different number of elements}}
  var (o, p, q, r) = callee1() // expected-error{{'(Int, Int, Int)' is not convertible to '(Int, Int, Int, _)', tuples have a different number of elements}}
}

//===----------------------------------------------------------------------===//
// ForwardIndex referencing of types.
//===----------------------------------------------------------------------===//

// Lookup can find a decl declared later in the main module.
var x : x_ty
typealias x_ty = Int

// We allow name lookup to look forward past a function declaration (and other
// declarations which never have side-effects) in the main module.
func fy() -> y_ty { return 1 }
typealias y_ty = Int

// FIXME: Should reject this!
//typealias x = x

// FIXME: Should reject this (has infinite size or is tautological depend on
// how you look at it).
enum y {
  case y
  case Int
}

// We don't have a typeof, but this would also be an error somehow.
//var x : typeof(x)


//===----------------------------------------------------------------------===//
// ForwardIndex referencing of values.
//===----------------------------------------------------------------------===//

func func2() {
  func3()
}

func func3() {
  undefined_func() // expected-error {{cannot find 'undefined_func' in scope}}
}

//===----------------------------------------------------------------------===//
// Overloading
//===----------------------------------------------------------------------===//

struct a_struct { var x : Int }

infix operator *** : Starry
precedencegroup Starry {
  associativity: left
  higherThan: AssignmentPrecedence
  lowerThan: TernaryPrecedence
}

func ***(lhs: Int, rhs: Int) -> Int {
  return 4
}
func ***(lhs: a_struct, rhs: a_struct) {}
func ***(lhs: a_struct, rhs: (Int) -> Int) {}


func ov_fn_result() -> Int {}
func ov_fn_result() -> Double {}

func ov_fn_result2() -> (Int) -> (Int) -> Int {}
func ov_fn_result2() -> (Int) -> (a_struct) -> Int {}

func overloadtest(x: Int) {
  var _ : Int = ((ov_fn_result))()
  var _ : Double = ((ov_fn_result))()

  // Test overloaded operators.
  let s : a_struct
  _ = 4 *** 17     // Resolved to the *** operator that takes ints.
  s *** s     // Resolved to the *** operator that takes a_struct.
  s *** {$0 + 4}     // Closure obviously not a struct.

  _ = ov_fn_result2()(4)(4)  // picks the ov_fn_result2 taking an Int.
  _ = ov_fn_result2()(4)(s)  // picks the ov_fn_result2 taking a_struct.
}

func localtest() {
  func shadowbug() { 
    var Foo = 10
    Foo += 1
    print(Foo)
    
    func g() {
      struct S {
        // FIXME: Swap these two lines to crash our broken lookup.
        typealias Foo = Int
        var x : Foo
      }
    }
  }
  func scopebug() { 
    var Foo = 10
    struct S {
      typealias Foo = Int
    }
    Foo = 17
    _ = Foo
  }
  func scopebug2() { 
    struct S1 {}
    struct S2 {
      var x : S1
    }
  }
}

func f0() -> ThisTypeDoesNotExist { return 1 } // expected-error {{cannot find type 'ThisTypeDoesNotExist' in scope}}

for _ in [1] { }

//===----------------------------------------------------------------------===//
// Accessing names from our own module
//===----------------------------------------------------------------------===//
var qualifiedvalue : Int = themodule.importedtype
var qualifiedtype : themodule.x_ty = 5


prefix operator +++
postfix operator +++

prefix operator ++
postfix operator ++

prefix func +++(a: inout Int) { a += 2 }
postfix func +++(a: inout Int) { a += 2 }

var test = 0
+++test
test+++


//===----------------------------------------------------------------------===//
// Forward references to local variables.
//===----------------------------------------------------------------------===//

func forwardReference() {
  v = 0 // expected-error{{use of local variable 'v' before its declaration}}
  var v: Float = 0.0 // expected-note{{'v' declared here}}
}

func questionablyValidForwardReference() { print(qvfrVar, terminator: ""); }; var qvfrVar: Int = 0

// FIXME: This should warn too.
print(forwardReferenceVar, terminator: ""); var forwardReferenceVar: Int = 0



// <rdar://problem/23248290> Name lookup: "Cannot convert type 'Int' to expected argument type 'Int'" while trying to initialize ivar of generic type in class scope
// https://gist.github.com/erynofwales/61768899502b7ac83c6e
struct Matrix4<T: FloatingPoint> {
  static func size() -> Int {}
  
  private var data: Int = Matrix4.size()   // Ok: Matrix4<T>
  
  init() {
    data = Matrix4.size()  // Ok: Matrix4<T>
  }
}

// <rdar://problem/19558785> for-in collection/where expressions are parsed with pattern variables in scope
func r19558785() {
  let b = 10
  for b in 0...b {
    _ = b
  }
}

