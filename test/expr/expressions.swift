// RUN: %target-typecheck-verify-swift

//===----------------------------------------------------------------------===//
// Tests and samples.
//===----------------------------------------------------------------------===//

// Comment.  With unicode characters: ¡ç®åz¥!

func markUsed<T>(_: T) {}

// Various function types.
var func1 : () -> ()    // No input, no output.
var func2 : (Int) -> Int
var func3 : () -> () -> ()                   // Takes nothing, returns a fn.
var func3a : () -> (() -> ())                // same as func3
var func6 : (_ fn : (Int,Int) -> Int) -> ()    // Takes a fn, returns nothing.
var func7 : () -> (Int,Int,Int)              // Takes nothing, returns tuple.

// Top-Level expressions.  These are 'main' content.
func1()
_ = 4+7

var bind_test1 : () -> () = func1
var bind_test2 : Int = 4; func1 // expected-warning {{variable is unused}}

(func1, func2) // expected-warning {{variable is unused}}

func basictest() {
  // Simple integer variables.
  var x : Int
  var x2 = 4              // Simple Type inference.
  var x3 = 4+x*(4+x2)/97  // Basic Expressions.

  // Declaring a variable Void, aka (), is fine too.
  var v : Void

  var x4 : Bool = true
  var x5 : Bool =
        4 // expected-error {{cannot convert value of type 'Int' to specified type 'Bool'}}

  //var x6 : Float = 4+5

  var x7 = 4; 5 // expected-warning {{integer literal is unused}}

  // Test implicit conversion of integer literal to non-Int64 type.
  var x8 : Int8 = 4
  x8 = x8 + 1
  _ = x8 + 1
  _ = 0 + x8
  1.0 + x8 // expected-error{{binary operator '+' cannot be applied to operands of type 'Double' and 'Int8'}}
  // expected-note @-1 {{overloads for '+' exist with these partially matching parameter lists:}}


  var x9 : Int16 = x8 + 1 // expected-error {{cannot convert value of type 'Int8' to specified type 'Int16'}}

  // Various tuple types.
  var tuple1 : ()
  var tuple2 : (Int)
  var tuple3 : (Int, Int, ())
  var tuple2a : (a : Int) // expected-error{{cannot create a single-element tuple with an element label}}{{18-22=}}
  var tuple3a : (a : Int, b : Int, c : ())

  var tuple4 = (1, 2)        // Tuple literal.
  var tuple5 = (1, 2, 3, 4)  // Tuple literal.
  var tuple6 = (1 2)  // expected-error {{expected ',' separator}} {{18-18=,}}

  // Brace expressions.
  var brace3 = {
    var brace2 = 42  // variable shadowing.
    _ = brace2+7
  }

  // Function calls.
  var call1 : () = func1()
  var call2 = func2(1)
  var call3 : () = func3()()

  // Cannot call an integer.
  bind_test2() // expected-error {{cannot call value of non-function type 'Int'}}{{13-15=}}
}

// https://github.com/apple/swift/issues/46110
// Test unused literals.
do {
  42 // expected-warning {{integer literal is unused}}
  2.71828 // expected-warning {{floating-point literal is unused}}
  true // expected-warning {{boolean literal is unused}}
  false // expected-warning {{boolean literal is unused}}
  "Hello" // expected-warning {{string literal is unused}}
  "Hello \(42)" // expected-warning {{string literal is unused}}
  #file // expected-warning {{#file literal is unused}}
  (#line) // expected-warning {{#line literal is unused}}
  #column // expected-warning {{#column literal is unused}}
  #function // expected-warning {{#function literal is unused}}
  #dsohandle // expected-warning {{#dsohandle literal is unused}}
  __FILE__ // expected-error {{cannot find '__FILE__' in scope}}
  __LINE__ // expected-error {{cannot find '__LINE__' in scope}}
  __COLUMN__ // expected-error {{cannot find '__COLUMN__' in scope}}
  __FUNCTION__ // expected-error {{cannot find '__FUNCTION__' in scope}}
  __DSO_HANDLE__ // expected-error {{cannot find '__DSO_HANDLE__' in scope}}

  nil // expected-error {{'nil' requires a contextual type}}
  #fileLiteral(resourceName: "what.txt") // expected-error {{could not infer type of file reference literal}} expected-note * {{}}
  #imageLiteral(resourceName: "hello.png") // expected-error {{could not infer type of image literal}} expected-note * {{}}
  #colorLiteral(red: 1, green: 0, blue: 0, alpha: 1) // expected-error {{could not infer type of color literal}} expected-note * {{}}
}

// Infix operators and attribute lists.
infix operator %% : MinPrecedence
precedencegroup MinPrecedence {
  associativity: left
  lowerThan: AssignmentPrecedence
}

func %%(a: Int, b: Int) -> () {}
var infixtest : () = 4 % 2 + 27 %% 123



// The 'func' keyword gives a nice simplification for function definitions.
func funcdecl1(_ a: Int, _ y: Int) {}
func funcdecl2() {
  return funcdecl1(4, 2)
}
func funcdecl3() -> Int {
  return 12
}
func funcdecl4(_ a: ((Int) -> Int), b: Int) {}
func signal(_ sig: Int, f: (Int) -> Void) -> (Int) -> Void {}

// Doing fun things with named arguments.  Basic stuff first.
func funcdecl6(_ a: Int, b: Int) -> Int { return a+b }

// Can dive into tuples, 'b' is a reference to a whole tuple, c and d are
// fields in one.  Cannot dive into functions or through aliases.
func funcdecl7(_ a: Int, b: (c: Int, d: Int), third: (c: Int, d: Int)) -> Int {
  _ = a + b.0 + b.c + third.0 + third.1
  b.foo // expected-error {{value of tuple type '(c: Int, d: Int)' has no member 'foo'}}
}

// Error recovery.
func testfunc2 (_: ((), Int) -> Int) -> Int {}
func makeTuple() -> (String, Int) { return ("foo", 42) }
func errorRecovery() {
  testfunc2({ $0 + 1 }) // expected-error {{contextual closure type '((), Int) -> Int' expects 2 arguments, but 1 was used in closure body}}
  // expected-error@-1 {{cannot convert value of type '()' to expected argument type 'Int'}}

  enum union1 {
    case bar
    case baz
  }
  var a: Int = .hello // expected-error {{type 'Int' has no member 'hello'}}
  var b: union1 = .bar // ok
  var c: union1 = .xyz  // expected-error {{type 'union1' has no member 'xyz'}}
  var d: (Int,Int,Int) = (1,2) // expected-error {{'(Int, Int)' is not convertible to '(Int, Int, Int)', tuples have a different number of elements}}
  var e: (Int,Int) = (1, 2, 3) // expected-error {{'(Int, Int, Int)' is not convertible to '(Int, Int)', tuples have a different number of elements}}
  var f: (Int,Int) = (1, 2, f : 3) // expected-error {{'(Int, Int, f: Int)' is not convertible to '(Int, Int)', tuples have a different number of elements}}
  
  // <rdar://problem/22426860> CrashTracer: [USER] swift at …mous_namespace::ConstraintGenerator::getTypeForPattern + 698
  var (g1, g2, g3) = (1, 2) // expected-error {{'(Int, Int)' is not convertible to '(Int, Int, _)', tuples have a different number of elements}}
  var (h1, h2) = (1, 2, 3) // expected-error {{'(Int, Int, Int)' is not convertible to '(Int, Int)', tuples have a different number of elements}}
  var i: (Bool, Bool) = makeTuple() // expected-error {{cannot convert value of type '(String, Int)' to specified type '(Bool, Bool)'}}
}

func acceptsInt(_ x: Int) {}
acceptsInt(unknown_var) // expected-error {{cannot find 'unknown_var' in scope}}



var test1a: (Int) -> (Int) -> Int = { { $0 } } // expected-error{{contextual type for closure argument list expects 1 argument, which cannot be implicitly ignored}} {{38-38= _ in}}
var test1b = { 42 }
var test1c = { { 42 } }
var test1d = { { { 42 } } }

func test2(_ a: Int, b: Int) -> (c: Int) { // expected-error{{cannot create a single-element tuple with an element label}}
 _ = a+b
 a+b+c // expected-error{{cannot find 'c' in scope}}
 return a+b
}


func test3(_ arg1: Int, arg2: Int) -> Int {
  return 4
}

func test4() -> ((_ arg1: Int, _ arg2: Int) -> Int) {
  return test3
}

func test5() {
  let a: (Int, Int) = (1,2)
  var
     _: ((Int) -> Int, Int) = a  // expected-error {{cannot convert value of type '(Int, Int)' to specified type '((Int) -> Int, Int)'}}


  let c: (a: Int, b: Int) = (1,2)
  let _: (b: Int, a: Int) = c  // expected-warning {{expression shuffles the elements of this tuple; this behavior is deprecated}}
}


// Functions can obviously take and return values.
func w3(_ a: Int) -> Int { return a }
func w4(_: Int) -> Int { return 4 }



func b1() {}

func foo1(_ a: Int, b: Int) -> Int {}
func foo2(_ a: Int) -> (_ b: Int) -> Int {}
func foo3(_ a: Int = 2, b: Int = 3) {}

prefix operator ^^

prefix func ^^(a: Int) -> Int {
  return a + 1
}

func test_unary1() {
  var x: Int

  x = ^^(^^x)
  x = *x      // expected-error {{'*' is not a prefix unary operator}}
  x = x*      // expected-error {{'*' is not a postfix unary operator}}
  x = +(-x)
  x = + -x // expected-error {{unary operator cannot be separated from its operand}} {{8-9=}}
}
func test_unary2() {
  var x: Int
  // FIXME: second diagnostic is redundant.
  x = &; // expected-error {{expected expression after unary operator}} expected-error {{expected expression in assignment}}
}
func test_unary3() {
  var x: Int
  // FIXME: second diagnostic is redundant.
  x = &, // expected-error {{expected expression after unary operator}} expected-error {{expected expression in assignment}}
}

func test_as_1() {
  var _: Int
}
func test_as_2() {
  let x: Int = 1
  x as [] // expected-error {{expected element type}} {{9-9= <#type#>}}
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

func test_lambda2() {
  { () -> protocol<Int> in
    // expected-error @-1 {{'protocol<...>' composition syntax has been removed and is not needed here}} {{11-24=Int}}
    // expected-error @-2 {{non-protocol, non-class type 'Int' cannot be used within a protocol-constrained type}}
    return 1
  }()
}

func test_floating_point() {
  _ = 0.0
  _ = 100.1
  var _: Float = 0.0
  var _: Double = 0.0
}

func test_nonassoc(_ x: Int, y: Int) -> Bool {
  // FIXME: the second error and note here should arguably disappear
  return x == y == x // expected-error {{adjacent operators are in non-associative precedence group 'ComparisonPrecedence'}}  expected-error {{binary operator '==' cannot be applied to operands of type 'Bool' and 'Int'}}
  // expected-note@-1 {{overloads for '==' exist with these partially matching parameter lists: (Bool, Bool), (Int, Int)}}
}

// More realistic examples.

func fib(_ n: Int) -> Int {
  if (n < 2) {
    return n
  }

  return fib(n-2) + fib(n-1)
}

//===----------------------------------------------------------------------===//
// Integer Literals
//===----------------------------------------------------------------------===//

// FIXME: Should warn about integer constants being too large <rdar://problem/14070127>
var
   il_a: Bool = 4  // expected-error {{cannot convert value of type 'Int' to specified type 'Bool'}}
var il_b: Int8
   = 123123
var il_c: Int8 = 4  // ok

struct int_test4 : ExpressibleByIntegerLiteral {
  typealias IntegerLiteralType = Int
  init(integerLiteral value: Int) {} // user type.
}

var il_g: int_test4 = 4



// This just barely fits in Int64.
var il_i: Int64  = 18446744073709551615

// This constant is too large to fit in an Int64, but it is fine for Int128.
// FIXME: Should warn about the first. <rdar://problem/14070127>
var il_j: Int64  = 18446744073709551616
// var il_k: Int128 = 18446744073709551616

var bin_literal: Int64 = 0b100101
var hex_literal: Int64 = 0x100101
var oct_literal: Int64 = 0o100101

// verify that we're not using C rules
var oct_literal_test: Int64 = 0123
assert(oct_literal_test == 123)

// ensure that we swallow random invalid chars after the first invalid char
var invalid_num_literal: Int64 = 0QWERTY  // expected-error{{'Q' is not a valid digit in integer literal}}
var invalid_bin_literal: Int64 = 0bQWERTY // expected-error{{'Q' is not a valid binary digit (0 or 1) in integer literal}}
var invalid_hex_literal: Int64 = 0xQWERTY // expected-error{{'Q' is not a valid hexadecimal digit (0-9, A-F) in integer literal}}
var invalid_oct_literal: Int64 = 0oQWERTY // expected-error{{'Q' is not a valid octal digit (0-7) in integer literal}}
var invalid_exp_literal: Double = 1.0e+QWERTY // expected-error{{'Q' is not a valid digit in floating point exponent}}
var invalid_fp_exp_literal: Double = 0x1p+QWERTY // expected-error{{'Q' is not a valid digit in floating point exponent}}

// don't emit a partial integer literal if the invalid char is valid for identifiers.
var invalid_num_literal_prefix: Int64 = 0a1234567 // expected-error{{'a' is not a valid digit in integer literal}}
var invalid_num_literal_middle: Int64 = 0123A5678 // expected-error{{'A' is not a valid digit in integer literal}}
var invalid_bin_literal_middle: Int64 = 0b1020101 // expected-error{{'2' is not a valid binary digit (0 or 1) in integer literal}}
var invalid_oct_literal_middle: Int64 = 0o1357864 // expected-error{{'8' is not a valid octal digit (0-7) in integer literal}}
var invalid_hex_literal_middle: Int64 = 0x147ADG0 // expected-error{{'G' is not a valid hexadecimal digit (0-9, A-F) in integer literal}}

var invalid_hex_literal_exponent_ = 0xffp+12abc // expected-error{{'a' is not a valid digit in floating point exponent}}
var invalid_float_literal_exponent = 12e1abc // expected-error{{'a' is not a valid digit in floating point exponent}}

// rdar://11088443
var negative_int32: Int32 = -1

// <rdar://problem/11287167>
var tupleelemvar = 1
markUsed((tupleelemvar, tupleelemvar).1)

func int_literals() {
  // Fits exactly in 64-bits - rdar://11297273
  _ = 1239123123123123
  // Overly large integer.
  // FIXME: Should warn about it. <rdar://problem/14070127>
  _ = 123912312312312312312
  
}

// <rdar://problem/12830375>
func tuple_of_rvalues(_ a:Int, b:Int) -> Int {
  return (a, b).1
}

extension Int {
  func testLexingMethodAfterIntLiteral() {}
  func _0() {}
  // Hex letters
  func ffa() {}
  // Hex letters + non hex.
  func describe() {}
  // Hex letters + 'p'.
  func eap() {}
  // Hex letters + 'p' + non hex.
  func fpValue() {}
}

123.testLexingMethodAfterIntLiteral()
0b101.testLexingMethodAfterIntLiteral()
0o123.testLexingMethodAfterIntLiteral()
0x1FFF.testLexingMethodAfterIntLiteral()

123._0()
0b101._0()
0o123._0()
0x1FFF._0()

0x1fff.ffa()
0x1FFF.describe()
0x1FFF.eap()
0x1FFF.fpValue()

var separator1: Int = 1_
var separator2: Int = 1_000
var separator4: Int = 0b1111_0000_
var separator5: Int = 0b1111_0000
var separator6: Int = 0o127_777_
var separator7: Int = 0o127_777
var separator8: Int = 0x12FF_FFFF
var separator9: Int = 0x12FF_FFFF_

//===----------------------------------------------------------------------===//
// Float Literals
//===----------------------------------------------------------------------===//

var fl_a = 0.0
var fl_b: Double = 1.0
var fl_c: Float = 2.0
// FIXME: crummy diagnostic
var fl_d: Float = 2.0.0 // expected-error {{expected named member of numeric literal}}
var fl_e: Float = 1.0e42
var fl_f: Float = 1.0e+  // expected-error {{expected a digit in floating point exponent}} 
var fl_g: Float = 1.0E+42
var fl_h: Float = 2e-42
var vl_i: Float = -.45   // expected-error {{'.45' is not a valid floating point literal; it must be written '0.45'}} {{20-20=0}}
var fl_j: Float = 0x1p0
var fl_k: Float = 0x1.0p0
var fl_l: Float = 0x1.0 // expected-error {{hexadecimal floating point literal must end with an exponent}}
var fl_m: Float = 0x1.FFFFFEP-2
var fl_n: Float = 0x1.fffffep+2
var fl_o: Float = 0x1.fffffep+ // expected-error {{expected a digit in floating point exponent}}
var fl_p: Float = 0x1p // expected-error {{expected a digit in floating point exponent}}
var fl_q: Float = 0x1p+ // expected-error {{expected a digit in floating point exponent}}
var fl_r: Float = 0x1.0fp // expected-error {{expected a digit in floating point exponent}}
var fl_s: Float = 0x1.0fp+ // expected-error {{expected a digit in floating point exponent}}
var fl_t: Float = 0x1.p // expected-error {{value of type 'Int' has no member 'p'}}
var fl_u: Float = 0x1.p2 // expected-error {{value of type 'Int' has no member 'p2'}}
var fl_v: Float = 0x1.p+ // expected-error {{'+' is not a postfix unary operator}} expected-error {{value of type 'Int' has no member 'p'}}
var fl_w: Float = 0x1.p+2 // expected-error {{value of type 'Int' has no member 'p'}}

var if1: Double = 1.0 + 4  // integer literal ok as double.
var if2: Float = 1.0 + 4  // integer literal ok as float.

var fl_separator1: Double = 1_.2_
var fl_separator2: Double = 1_000.2_
var fl_separator3: Double = 1_000.200_001
var fl_separator4: Double = 1_000.200_001e1_
var fl_separator5: Double = 1_000.200_001e1_000
var fl_separator6: Double = 1_000.200_001e1_000
var fl_separator7: Double = 0x1_.0FFF_p1_
var fl_separator8: Double = 0x1_0000.0FFF_ABCDp10_001

var fl_bad_separator1: Double = 1e_ // expected-error {{'_' is not a valid first character in floating point exponent}}
var fl_bad_separator2: Double = 0x1p_ // expected-error {{'_' is not a valid first character in floating point exponent}}

//===----------------------------------------------------------------------===//
// String Literals
//===----------------------------------------------------------------------===//

var st_a = ""
var st_b: String = ""
var st_c = "asdfasd    // expected-error {{unterminated string literal}}

var st_d = " \t\n\r\"\'\\  "  // Valid simple escapes
var st_e = " \u{12}\u{0012}\u{00000078} "  // Valid unicode escapes
var st_u1 = " \u{1} "
var st_u2 = " \u{123} "
var st_u3 = " \u{1234567} " // expected-error {{invalid unicode scalar}}
var st_u4 = " \q "  // expected-error {{invalid escape sequence in literal}}

var st_u5 = " \u{FFFFFFFF} "  // expected-error {{invalid unicode scalar}}
var st_u6 = " \u{D7FF} \u{E000} "  // Fencepost UTF-16 surrogate pairs.
var st_u7 = " \u{D800} "  // expected-error {{invalid unicode scalar}}
var st_u8 = " \u{DFFF} "  // expected-error {{invalid unicode scalar}}
var st_u10 = " \u{0010FFFD} "  // Last valid codepoint, 0xFFFE and 0xFFFF are reserved in each plane
var st_u11 = " \u{00110000} "  // expected-error {{invalid unicode scalar}}

func stringliterals(_ d: [String: Int]) {

  // rdar://11385385
  let x = 4
  "Hello \(x+1) world"  // expected-warning {{string literal is unused}}
  
  // expected-error @+1 {{unterminated string literal}}
  "Error: \(x+1"; // expected-error {{cannot find ')' to match opening '(' in string interpolation}}

  // expected-error @+1 {{unterminated string literal}}
  "Error: \(x+1   // expected-error {{cannot find ')' to match opening '(' in string interpolation}}
  ;    // expected-error {{';' statements are not allowed}}

  // rdar://14050788 [DF] String Interpolations can't contain quotes
  "test \("nested")"
  "test \("\("doubly nested")")"
  "test \(d["hi"])"
  "test \("quoted-paren )")"
  "test \("quoted-paren (")"
  "test \("\\")"
  "test \("\n")"
  "test \("\")" // expected-error {{cannot find ')' to match opening '(' in string interpolation}} expected-error {{unterminated string literal}}

  "test \
  // expected-error @-1 {{unterminated string literal}} expected-error @-1 {{invalid escape sequence in literal}}
  "test \("\
  // expected-error @-1 {{cannot find ')' to match opening '(' in string interpolation}} expected-error @-1 {{unterminated string literal}}
  "test newline \("something" +
    "something else")"
  // expected-error @-2 {{cannot find ')' to match opening '(' in string interpolation}}
  // expected-error @-2 {{unterminated string literal}} expected-error @-3 {{unterminated string literal}}

  // expected-warning @+2 {{variable 'x2' was never used; consider replacing with '_' or removing it}}
  // expected-error @+1 {{unterminated string literal}}
  var x2 : () = ("hello" + "
  ;
}

func testSingleQuoteStringLiterals() {
  _ = 'abc' // expected-error{{single-quoted string literal found, use '"'}}{{7-12="abc"}}
  _ = 'abc' + "def" // expected-error{{single-quoted string literal found, use '"'}}{{7-12="abc"}}

  _ = 'ab\nc' // expected-error{{single-quoted string literal found, use '"'}}{{7-14="ab\\nc"}}

  _ = "abc\('def')" // expected-error{{single-quoted string literal found, use '"'}}{{13-18="def"}}
  _ = 'ab\("c")' // expected-error{{single-quoted string literal found, use '"'}}{{7-17="ab\\("c")"}}
  _ = 'a\('b')c' // expected-error{{single-quoted string literal found, use '"'}}{{7-17="a\\('b')c"}}
                 // expected-error@-1{{single-quoted string literal found, use '"'}}{{11-14="b"}}

  _ = "abc' // expected-error{{unterminated string literal}}
  _ = 'abc" // expected-error{{unterminated string literal}}
  _ = "a'c"

  _ = 'ab\'c' // expected-error{{single-quoted string literal found, use '"'}}{{7-14="ab'c"}}

  _ = 'ab"c' // expected-error{{single-quoted string literal found, use '"'}}{{7-13="ab\\"c"}}
  _ = 'ab\"c' // expected-error{{single-quoted string literal found, use '"'}}{{7-14="ab\\"c"}}
  _ = 'ab\\"c' // expected-error{{single-quoted string literal found, use '"'}}{{7-15="ab\\\\\\"c"}}
}

// <rdar://problem/17128913>
var s = ""
s.append(contentsOf: ["x"])

//===----------------------------------------------------------------------===//
// InOut arguments
//===----------------------------------------------------------------------===//

func takesInt(_ x: Int) {}
func takesExplicitInt(_ x: inout Int) { }

func testInOut(_ arg: inout Int) {
  var x: Int
  takesExplicitInt(x) // expected-error{{passing value of type 'Int' to an inout parameter requires explicit '&'}} {{20-20=&}}
  takesExplicitInt(&x)
  takesInt(&x) // expected-error{{'&' used with non-inout argument of type 'Int'}}
  var y = &x //expected-error {{'&' may only be used to pass an argument to inout parameter}}
  var z = &arg //expected-error {{'&' may only be used to pass an argument to inout parameter}}

  takesExplicitInt(5) // expected-error {{cannot pass immutable value as inout argument: literals are not mutable}}
}

//===----------------------------------------------------------------------===//
// Conversions
//===----------------------------------------------------------------------===//

var pi_f: Float
var pi_d: Double

struct SpecialPi {} // Type with no implicit construction.

var pi_s: SpecialPi

func getPi() -> Float {}  // expected-note 3 {{found this candidate}}
func getPi() -> Double {} // expected-note 3 {{found this candidate}}
func getPi() -> SpecialPi {}

enum Empty { }

extension Empty {
  init(_ f: Float) { }
}

func conversionTest(_ a: inout Double, b: inout Int) {
  var f: Float
  var d: Double
  a = Double(b)
  a = Double(f)
  a = Double(d) // no-warning
  b = Int(a)
  f = Float(b)

  var pi_f1 = Float(pi_f)
  var pi_d1 = Double(pi_d)
  var pi_s1 = SpecialPi(pi_s) // expected-error {{argument passed to call that takes no arguments}}

  var pi_f2 = Float(getPi()) // expected-error {{ambiguous use of 'getPi()'}}
  var pi_d2 = Double(getPi()) // expected-error {{ambiguous use of 'getPi()'}}
  var pi_s2: SpecialPi = getPi() // no-warning
  
  var float = Float.self
  var pi_f3 = float.init(getPi()) // expected-error {{ambiguous use of 'getPi()'}}
  var pi_f4 = float.init(pi_f)

  var e = Empty(f) // expected-warning {{variable 'e' inferred to have type 'Empty', which is an enum with no cases}} expected-note {{add an explicit type annotation to silence this warning}}  {{8-8=: Empty}}
  var e2 = Empty(d) // expected-error{{cannot convert value of type 'Double' to expected argument type 'Float'}}
  var e3 = Empty(Float(d)) // expected-warning {{variable 'e3' inferred to have type 'Empty', which is an enum with no cases}} expected-note {{add an explicit type annotation to silence this warning}}  {{9-9=: Empty}}
}

// FIXME(diagnostics): This note is pointing to a synthesized init
struct Rule { // expected-note {{'init(target:dependencies:)' declared here}}
  var target: String
  var dependencies: String
}

var ruleVar: Rule
// FIXME(diagnostics): To be able to suggest different candidates here we need to teach the solver how to figure out to which parameter
// does argument belong to in this case. If the `target` was of a different type, we currently suggest to add an argument for `dependencies:`
// which is incorrect.
ruleVar = Rule("a") // expected-error {{missing argument label 'target:' in call}}
// expected-error@-1 {{missing argument for parameter 'dependencies' in call}}

class C {
  var x: C?
  init(other: C?) { x = other }

  func method() {}
}

_ = C(3) // expected-error {{missing argument label 'other:' in call}}
// expected-error@-1 {{cannot convert value of type 'Int' to expected argument type 'C?'}}
_ = C(other: 3) // expected-error {{cannot convert value of type 'Int' to expected argument type 'C?'}}

//===----------------------------------------------------------------------===//
// Unary Operators
//===----------------------------------------------------------------------===//

func unaryOps(_ i8: inout Int8, i64: inout Int64) {
  i8 = ~i8
  i64 += 1
  i8 -= 1

  Int64(5) += 1 // expected-error{{left side of mutating operator has immutable type 'Int64'}}
  
  // <rdar://problem/17691565> attempt to modify a 'let' variable with ++ results in typecheck error not being able to apply ++ to Float
  let a = i8 // expected-note {{change 'let' to 'var' to make it mutable}} {{3-6=var}}
  a += 1 // expected-error {{left side of mutating operator isn't mutable: 'a' is a 'let' constant}}
  
  var b : Int { get { }}
  b += 1  // expected-error {{left side of mutating operator isn't mutable: 'b' is a get-only property}}
}

//===----------------------------------------------------------------------===//
// Iteration
//===----------------------------------------------------------------------===//

func..<(x: Double, y: Double) -> Double {
   return x + y
}

func iterators() {
  _ = 0..<42
  _ = 0.0..<42.0
}

//===----------------------------------------------------------------------===//
// Magic literal expressions
//===----------------------------------------------------------------------===//

func magic_literals() {
  _ = #file
  _ = #line + #column
  var _: UInt8 = #line + #column
}

//===----------------------------------------------------------------------===//
// lvalue processing
//===----------------------------------------------------------------------===//


infix operator +-+=
@discardableResult
func +-+= (x: inout Int, y: Int) -> Int { return 0}

func lvalue_processing() {
  var i = 0
  i += 1   // obviously ok

  var fn = (+-+=)

  var n = 42
  fn(n, 12)  // expected-error {{passing value of type 'Int' to an inout parameter requires explicit '&'}} {{6-6=&}}
  fn(&n, 12) // expected-warning {{result of call to function returning 'Int' is unused}}

  n +-+= 12

  (+-+=)(&n, 12)  // ok.
  (+-+=)(n, 12) // expected-error {{passing value of type 'Int' to an inout parameter requires explicit '&'}} {{10-10=&}}
}

struct Foo {
  func method() {}
  mutating func mutatingMethod() {}
}

func test() {
  var x = Foo()
  let y = Foo()

  // rdar://15708430
  (&x).method()  // expected-error {{'&' may only be used to pass an argument to inout parameter}}
  (&x).mutatingMethod() // expected-error {{'&' may only be used to pass an argument to inout parameter}}
}


// Unused results.
func unusedExpressionResults() {
  // Unused l-value
  _ // expected-error{{'_' can only appear in a pattern or on the left side of an assignment}}

  // <rdar://problem/20749592> Conditional Optional binding hides compiler error
  let optionalc:C? = nil
  optionalc?.method()  // ok
  optionalc?.method  // expected-error {{function is unused}}
}




//===----------------------------------------------------------------------===//
// Collection Literals
//===----------------------------------------------------------------------===//

func arrayLiterals() { 
  let _ = [1,2,3]
  let _ : [Int] = []
  let _ = []  // expected-error {{empty collection literal requires an explicit type}}
}

func dictionaryLiterals() {
  let _ = [1 : "foo",2 : "bar",3 : "baz"]
  let _: Dictionary<Int, String> = [:]
  let _ = [:]  // expected-error {{empty collection literal requires an explicit type}}
}

func invalidDictionaryLiteral() {
  // FIXME: lots of unnecessary diagnostics.

  var a = [1: ; // expected-error {{expected value in dictionary literal}}
  var b = [1: ;] // expected-error {{expected value in dictionary literal}}
  var c = [1: "one" ;] // expected-error {{expected key expression in dictionary literal}} expected-error {{expected ',' separator}} {{20-20=,}}
  var d = [1: "one", ;] // expected-error {{expected key expression in dictionary literal}}
  var e = [1: "one", 2] // expected-error {{expected ':' in dictionary literal}}
  var f = [1: "one", 2 ;] // expected-error {{expected ':' in dictionary literal}}
  var g = [1: "one", 2: ;] // expected-error {{expected value in dictionary literal}}
}


[4].joined(separator: [1])
// expected-error@-1 {{no exact matches in call to instance method 'joined'}}
// There is one more note here - candidate requires that 'Int' conform to 'Sequence' (requirement specified as 'Self.Element' : 'Sequence') pointing to Sequence extension

[4].joined(separator: [[[1]]])
// expected-error@-1 {{cannot convert value of type 'Int' to expected element type 'String'}}
// expected-error@-2 {{cannot convert value of type '[[[Int]]]' to expected argument type 'String'}}

//===----------------------------------------------------------------------===//
// nil/metatype comparisons
//===----------------------------------------------------------------------===//
_ = Int.self == nil  // expected-warning {{comparing non-optional value of type 'any Any.Type' to 'nil' always returns false}}
_ = nil == Int.self  // expected-warning {{comparing non-optional value of type 'any Any.Type' to 'nil' always returns false}}
_ = Int.self != nil  // expected-warning {{comparing non-optional value of type 'any Any.Type' to 'nil' always returns true}}
_ = nil != Int.self  // expected-warning {{comparing non-optional value of type 'any Any.Type' to 'nil' always returns true}}

_ = Int.self == .none  // expected-warning {{comparing non-optional value of type 'any Any.Type' to 'Optional.none' always returns false}}
_ = .none == Int.self  // expected-warning {{comparing non-optional value of type 'any Any.Type' to 'Optional.none' always returns false}}
_ = Int.self != .none  // expected-warning {{comparing non-optional value of type 'any Any.Type' to 'Optional.none' always returns true}}
_ = .none != Int.self  // expected-warning {{comparing non-optional value of type 'any Any.Type' to 'Optional.none' always returns true}}

// <rdar://problem/19032294> Disallow postfix ? when not chaining
func testOptionalChaining(_ a : Int?, b : Int!, c : Int??) {
  _ = a?    // expected-error {{optional chain has no effect, expression already produces 'Int?'}} {{8-9=}}
  _ = a?.customMirror

  _ = b?   // expected-error {{optional chain has no effect, expression already produces 'Int?'}}
  _ = b?.customMirror

  var _: Int? = c?   // expected-error {{'?' must be followed by a call, member lookup, or subscript}}
}


// <rdar://problem/19657458> Nil Coalescing operator (??) should have a higher precedence
func testNilCoalescePrecedence(cond: Bool, a: Int?, r: ClosedRange<Int>?) {
  // ?? should have higher precedence than logical operators like || and comparisons.
  if cond || (a ?? 42 > 0) {}  // Ok.
  if (cond || a) ?? 42 > 0 {}  // expected-error {{cannot be used as a boolean}} {{15-15=(}} {{16-16= != nil)}}
  // expected-error@-1 {{binary operator '>' cannot be applied to operands of type 'Bool' and 'Int'}}  expected-note@-1 {{overloads for '>' exist with these partially matching parameter list}}
  // expected-error@-2 {{binary operator '??' cannot be applied to operands of type 'Bool' and 'Int'}}
  if (cond || a) ?? (42 > 0) {}  // expected-error {{cannot be used as a boolean}} {{15-15=(}} {{16-16= != nil)}}

  if cond || a ?? 42 > 0 {}    // Parses as the first one, not the others.


  // ?? should have lower precedence than range and arithmetic operators.
  let r1 = r ?? (0...42) // ok
  let r2 = (r ?? 0)...42 // not ok: expected-error {{binary operator '??' cannot be applied to operands of type 'ClosedRange<Int>?' and 'Int'}}
  let r3 = r ?? 0...42 // parses as the first one, not the second.
  
  
  // <rdar://problem/27457457> [Type checker] Diagnose unsavory optional injections
  // Accidental optional injection for ??.
  let i = 42
  _ = i ?? 17 // expected-warning {{left side of nil coalescing operator '??' has non-optional type 'Int', so the right side is never used}} {{8-14=}}
}

// <rdar://problem/19772570> Parsing of as and ?? regressed
func testOptionalTypeParsing(_ a : AnyObject) -> String {
  return a as? String ?? "default name string here"
}

func testParenExprInTheWay() {
  let x = 42
  
  if x & 4.0 {}  // expected-error {{cannot convert value of type 'Double' to expected argument type 'Int'}}
  // expected-error@-1 {{type 'Int' cannot be used as a boolean; test for '!= 0' instead}}
  if (x & 4.0) {}   // expected-error {{cannot convert value of type 'Double' to expected argument type 'Int'}}
  // expected-error@-1 {{type 'Int' cannot be used as a boolean; test for '!= 0' instead}}
  if !(x & 4.0) {}  // expected-error {{cannot convert value of type 'Double' to expected argument type 'Int'}}
  // expected-error@-1 {{type 'Int' cannot be used as a boolean; test for '== 0' instead}} {{6-7=}}{{7-7=(}}{{16-16= == 0)}}

  if x & x {} // expected-error {{type 'Int' cannot be used as a boolean; test for '!= 0' instead}}
}

// <rdar://problem/21352576> Mixed method/property overload groups can cause a crash during constraint optimization
public struct TestPropMethodOverloadGroup {
    public typealias Hello = String
    public let apply:(Hello) -> Int
    public func apply(_ input:Hello) -> Int {
        return apply(input)
    }
}


// <rdar://problem/18496742> Passing ternary operator expression as inout crashes Swift compiler
func inoutTests(_ arr: inout Int) {
  var x = 1, y = 2
  (true ? &x : &y) // expected-error {{'&' may only be used to pass an argument to inout parameter}}
  let a = (true ? &x : &y) // expected-error {{'&' may only be used to pass an argument to inout parameter}}

  inoutTests(true ? &x : &y) // expected-error {{'&' may only be used to pass an argument to inout parameter}}

  &_ // expected-error {{'&' may only be used to pass an argument to inout parameter}}

  inoutTests((&x, 24).0) // expected-error {{'&' may only be used to pass an argument to inout parameter}}

  inoutTests((&x)) // expected-error {{'&' may only be used to pass an argument to inout parameter}} {{15-16=(}} {{14-15=&}}
  inoutTests(&x)
  
  // <rdar://problem/17489894> inout not rejected as operand to assignment operator
  &x += y  // expected-error {{'&' may only be used to pass an argument to inout parameter}}

  // <rdar://problem/23249098>
  func takeAny(_ x: Any) {}
  takeAny(&x) // expected-error{{'&' used with non-inout argument of type 'Any'}}
  func takeManyAny(_ x: Any...) {}
  takeManyAny(&x) // expected-error{{'&' used with non-inout argument of type 'Any'}}
  takeManyAny(1, &x) // expected-error{{'&' used with non-inout argument of type 'Any'}}
  func takeIntAndAny(_ x: Int, _ y: Any) {}
  takeIntAndAny(1, &x) // expected-error{{'&' used with non-inout argument of type 'Any'}}
}


// <rdar://problem/20802757> Compiler crash in default argument & inout expr
var g20802757 = 2
func r20802757(_ z: inout Int = &g20802757) { // expected-error {{cannot provide default value to inout parameter 'z'}}
  // expected-error@-1 {{'&' may only be used to pass an argument to inout parameter}}
  print(z)
}

_ = _.foo // expected-error {{type placeholder not allowed here}} expected-error {{could not infer type for placeholder}}

// <rdar://problem/22211854> wrong arg list crashing sourcekit
func r22211854() {
    func f(_ x: Bool, _ y: Int, _ z: String = "") {} // expected-note 2 {{'f' declared here}}
    func g<T>(_ x: T, _ y: T, _ z: String = "") {} // expected-note 2 {{'g' declared here}}

    f(false) // expected-error{{missing argument for parameter #2 in call}}
    g(1) // expected-error{{missing argument for parameter #2 in call}}
    func h() -> Int { return 1 }
    f(h() == 1) // expected-error{{missing argument for parameter #2 in call}}
    g(h() == 1) // expected-error{{missing argument for parameter #2 in call}}
}

// <rdar://problem/22348394> Compiler crash on invoking function with labeled defaulted param with non-labeled argument
func r22348394() {
  func f(x: Int = 0) { }
  f(Int(3)) // expected-error{{missing argument label 'x:' in call}}
}

// <rdar://problem/23185177> Compiler crashes in Assertion failed: ((AllowOverwrite || !E->hasLValueAccessKind()) && "l-value access kind has already been set"), function visit
protocol Proto { var y: String? { get } }
func r23185177(_ x: Proto?) -> [String] {
  return x?.y // expected-error{{cannot convert return expression of type 'String?' to return type '[String]'}}
}

// <rdar://problem/22913570> Miscompile: wrong argument parsing when calling a function in swift2.0
func r22913570() {
  func f(_ from: Int = 0, to: Int) {} // expected-note {{'f(_:to:)' declared here}}
  f(1 + 1) // expected-error{{missing argument for parameter 'to' in call}}
}

// https://github.com/apple/swift/issues/43245
// Mixing lvalues and rvalues in tuple expression
do {
  var x = 0
  var y = 1
  let _ = (x, x + 1).0
  let _ = (x, 3).1
  (x,y) = (2,3)
  (x,4) = (1,2) // expected-error {{cannot assign to value: literals are not mutable}}
  (x,y).1 = 7 // expected-error {{cannot assign to immutable expression of type 'Int'}}
  x = (x,(3,y)).1.1
}

// https://github.com/apple/swift/issues/46027
// Subscripting with pound expressions
do {
  class B {
    init() {}
    subscript(x: Int) -> Int { return x }
    subscript(x: String) -> String { return x }

    func foo() {
      _ = self[#line] // Ok.
    }
  }
  class C : B {
    func bar() {
      _ = super[#file] // Ok.
    }
  }

  let obj = C();
  _ = obj[#column] // Ok.
}

// rdar://problem/23672697 - No way to express literal integers larger than Int without using type ascription
let _: Int64 = 0xFFF_FFFF_FFFF_FFFF
let _: Int64 = Int64(0xFFF_FFFF_FFFF_FFFF)
let _: Int64 = 0xFFF_FFFF_FFFF_FFFF as Int64
let _ = Int64(0xFFF_FFFF_FFFF_FFFF)
let _ = 0xFFF_FFFF_FFFF_FFFF as Int64

// rdar://problem/20289969 - string interpolation with comment containing ')' or '"'
let _ = "foo \(42 /* ) " ) */)"
let _ = "foo \(foo // )  " // expected-error {{cannot find ')' to match opening '(' in string interpolation}} expected-error {{unterminated string literal}}
let _ = "foo \(42 /*
                   * multiline comment
                   */)end"
// expected-error @-3 {{cannot find ')' to match opening '(' in string interpolation}} expected-error @-3 {{unterminated string literal}}
// expected-error @-2 {{expected expression}}
// expected-error @-3 {{unterminated string literal}}

// https://github.com/apple/swift/issues/66192
func I66192(_: Int) {}
I66192(true ? "yes" : "no") // expected-error{{cannot convert value of type 'String' to expected argument type 'Int'}}
