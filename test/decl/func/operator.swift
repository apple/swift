// RUN: %target-typecheck-verify-swift

infix operator %%%
infix operator %%%%

func %%%() {} // expected-error {{operators must have one or two non-default arguments}}
func %%%%(a: Int, b: Int, c: Int) {} // expected-error {{operators must have one or two non-default arguments}}

struct X {}
struct Y {}

func +(lhs: X, rhs: X) -> X {} // okay

func <=>(lhs: X, rhs: X) -> X {} // expected-error {{operator implementation without matching operator declaration}}{{1-1=infix operator <=> : <# Precedence Group #>\n}}

extension X {
    static func <=>(lhs: X, rhs: X) -> X {} // expected-error {{operator implementation without matching operator declaration}}{{-1:1-1=infix operator <=> : <# Precedence Group #>\n}}
}

extension X {
    struct Z {
        static func <=> (lhs: Z, rhs: Z) -> Z {} // expected-error {{operator implementation without matching operator declaration}}{{-2:1-1=infix operator <=> : <# Precedence Group #>\n}}
    }
}

extension X {
    static prefix func <=>(lhs: X) -> X {} // expected-error {{operator implementation without matching operator declaration}}{{-1:1-1=prefix operator <=> : <# Precedence Group #>\n}}
}

extension X {
    struct ZZ {
        static prefix func <=>(lhs: ZZ) -> ZZ {} // expected-error {{operator implementation without matching operator declaration}}{{-2:1-1=prefix operator <=> : <# Precedence Group #>\n}}
    }
}

// #60268: Make sure we insert at the start of the attributes.
@discardableResult
internal
func ^^^ (lhs: Int, rhs: Int) -> Int {} // expected-error {{operator implementation without matching operator declaration}} {{37:1-1=infix operator ^^^ : <# Precedence Group #>\n}}

@discardableResult
internal
prefix func ^^^ (rhs: Int) -> Int {} // expected-error {{operator implementation without matching operator declaration}} {{41:1-1=prefix operator ^^^ : <# Precedence Group #>\n}}

@frozen
public
struct Z {
  struct Y {
    static func ^^^ (lhs: Y, rhs: Y) {} // expected-error {{operator implementation without matching operator declaration}} {{45:1-1=infix operator ^^^ : <# Precedence Group #>\n}}
  }
}

_ = {
  func ^^^ (lhs: Int, rhs: Int) {}
  // expected-error@-1 {{operator functions can only be declared at global or in type scope}}
  // expected-error@-2 {{operator implementation without matching operator declaration}} {{53:1-1=infix operator ^^^ : <# Precedence Group #>\n}}
}

infix operator ++++ : ReallyHighPrecedence
precedencegroup ReallyHighPrecedence {
  higherThan: BitwiseShiftPrecedence
  associativity: left
}

infix func fn_binary(_ lhs: Int, rhs: Int) {}  // expected-error {{'infix' modifier is not required or allowed on func declarations}}


func ++++(lhs: X, rhs: X) -> X {}
func ++++(lhs: Y, rhs: Y) -> Y {} // okay

func useInt(_ x: Int) {}
func test() {
  var x : Int  
  let y : Int = 42
  // Produce a diagnostic for using the result of an assignment as a value.
  // rdar://12961094
  useInt(x = y)  // expected-error{{cannot convert value of type '()' to expected argument type 'Int'}}
  _ = x
}

prefix operator ~~
postfix operator ~~
infix operator ~~

postfix func foo(_ x: Int) {} // expected-error {{'postfix' requires a function with an operator identifier}}
postfix func ~~(x: Int) -> Float { return Float(x) }
postfix func ~~(x: Int, y: Int) {} // expected-error {{'postfix' requires a function with one argument}}
prefix func ~~(x: Float) {}
func test_postfix(_ x: Int) {
  ~~x~~
}

prefix operator ~~~ // expected-note 2{{prefix operator found here}}

// Unary operators require a prefix or postfix attribute
func ~~~(x: Float) {} // expected-error{{prefix unary operator missing 'prefix' modifier}}{{1-1=prefix }}

protocol P {
  static func ~~~(x: Self) // expected-error{{prefix unary operator missing 'prefix' modifier}}{{10-10=prefix }}
}

prefix func +// this should be a comment, not an operator
(arg: Int) -> Int { return arg }

prefix func -/* this also should be a comment, not an operator */
(arg: Int) -> Int { return arg }

func +*/ () {}   // expected-error {{expected identifier in function declaration}} expected-error {{unexpected end of block comment}}
func errors() {
  */    // expected-error {{unexpected end of block comment}}
  
  // rdar://12962712 - reject */ in an operator as it should end a block comment.
  */+    // expected-error {{unexpected end of block comment}}
}

prefix operator ...

prefix func ... (arg: Int) -> Int { return arg }
func resyncParser() {}

// Operator decl refs (<op>)

infix operator +-+
prefix operator +-+

prefix operator -+-
postfix operator -+-

infix operator +-+=

infix func +-+ (x: Int, y: Int) -> Int {} // expected-error {{'infix' modifier is not required or allowed on func declarations}} {{1-7=}}
prefix func +-+ (x: Int) -> Int {}

prefix func -+- (y: inout Int) -> Int {} // expected-note 2{{found this candidate}}
postfix func -+- (x: inout Int) -> Int {} // expected-note 2{{found this candidate}}

infix func +-+= (x: inout Int, y: Int) -> Int {} // expected-error {{'infix' modifier is not required or allowed on func declarations}} {{1-7=}}

var n = 0

// Infix by context
_ = (+-+)(1, 2)
// Prefix by context
_ = (+-+)(1)

// Ambiguous -- could be prefix or postfix
(-+-)(&n) // expected-error{{ambiguous use of operator '-+-'}}

// Assignment operator refs become inout functions
_ = (+-+=)(&n, 12)
(+-+=)(n, 12)   // expected-error {{passing value of type 'Int' to an inout parameter requires explicit '&'}} {{8-8=&}}

var f1 : (Int, Int) -> Int = (+-+)
var f2 : (Int) -> Int = (+-+)
var f3 : (inout Int) -> Int = (-+-) // expected-error{{ambiguous use of operator '-+-'}}
var f4 : (inout Int, Int) -> Int = (+-+=)
var r5 : (a : (Int, Int) -> Int, b : (Int, Int) -> Int) = (+, -)
var r6 : (a : (Int, Int) -> Int, b : (Int, Int) -> Int) = (b : +, a : -) // expected-warning {{expression shuffles the elements of this tuple; this behavior is deprecated}}

struct f6_S {
  subscript(op : (Int, Int) -> Int) -> Int {
    return 42
  }
}
var f6_s : f6_S
var junk = f6_s[+]

// Unicode operator names
infix operator ☃
infix operator ☃⃠ // Operators can contain (but not start with) combining characters

func ☃(x: Int, y: Int) -> Bool { return x == y }
func ☃⃠(x: Int, y: Int) -> Bool { return x != y }

var x, y : Int
_ = x☃y
_ = x☃⃠y

// rdar://14705150 - crash on invalid
func test_14705150() {
  let a = 4
  var b! = a  // expected-error {{type annotation missing in pattern}}
  // expected-error @-1 {{consecutive statements on a line must be separated by ';'}} {{8-8=;}}
  // expected-error @-2 {{expected expression}}

}

postfix operator ++
prefix operator ++

prefix postfix func ++(x: Int) {} // expected-error {{'postfix' contradicts previous modifier 'prefix'}} {{8-16=}}
postfix prefix func ++(x: Float) {} // expected-error {{'prefix' contradicts previous modifier 'postfix'}} {{9-16=}}
postfix prefix infix func ++(x: Double) {} // expected-error {{'prefix' contradicts previous modifier 'postfix'}} {{9-16=}} expected-error {{'infix' contradicts previous modifier 'postfix'}} {{16-22=}}
infix prefix func +-+(x: Double, y: Double) {} // expected-error {{'infix' modifier is not required or allowed on func declarations}} {{1-7=}} expected-error{{'prefix' contradicts previous modifier 'infix'}} {{7-14=}}

// Don't allow one to define a postfix '!'; it's built into the
// language. Also illegal to have any postfix operator starting with '!'.
postfix operator !  // expected-error {{cannot declare a custom postfix '!' operator}} expected-error {{postfix operator names starting with '?' or '!' are disallowed}}
prefix operator &  // expected-error {{cannot declare a custom prefix '&' operator}}

// <rdar://problem/14607026> Restrict use of '<' and '>' as prefix/postfix operator names
postfix operator >  // expected-error {{cannot declare a custom postfix '>' operator}}
prefix operator <  // expected-error {{cannot declare a custom prefix '<' operator}}

infix operator =  // expected-error {{cannot declare a custom infix '=' operator}}
infix operator ->  // expected-error {{cannot declare a custom infix '->' operator}}

postfix func !(x: Int) { } // expected-error{{cannot declare a custom postfix '!' operator}}
postfix func!(x: Int8) { } // expected-error{{cannot declare a custom postfix '!' operator}}
prefix func & (x: Int) {} // expected-error {{cannot declare a custom prefix '&' operator}}

// Only allow operators at global scope:
func operator_in_func_bad () {
    prefix func + (input: String) -> String { return "+" + input } // expected-error {{operator functions can only be declared at global or in type scope}}
}

infix operator ?  // expected-error {{cannot declare a custom infix '?' operator}}
prefix operator ?  // expected-error {{cannot declare a custom prefix '?' operator}}

infix operator ??=

func ??= <T>(result : inout T?, rhs : Int) {  // ok
}



// <rdar://problem/14296004> [QoI] Poor diagnostic/recovery when two operators (e.g., == and -) are adjacent without spaces.
_ = n*-4       // expected-error {{missing whitespace between '*' and '-' operators}} {{6-6= }} {{7-7= }}
if n==-1 {}    // expected-error {{missing whitespace between '==' and '-' operators}} {{5-5= }} {{7-7= }}

prefix operator ☃⃠
prefix func☃⃠(a : Int) -> Int { return a }
postfix operator ☃⃠
postfix func☃⃠(a : Int) -> Int { return a }

_ = n☃⃠ ☃⃠ n   // Ok.
_ = n ☃⃠ ☃⃠n   // Ok.
_ = n ☃⃠☃⃠ n   // expected-error {{cannot find operator '☃⃠☃⃠' in scope}}
_ = n☃⃠☃⃠n     // expected-error {{ambiguous missing whitespace between unary and binary operators}}
// expected-note @-1 {{could be binary '☃⃠' and prefix '☃⃠'}} {{12-12= }} {{18-18= }}
// expected-note @-2 {{could be postfix '☃⃠' and binary '☃⃠'}} {{6-6= }} {{12-12= }}

_ = n☃⃠☃⃠ // expected-error {{unary operators must not be juxtaposed; parenthesize inner expression}}
_ = ~!n  // expected-error {{unary operators must not be juxtaposed; parenthesize inner expression}}
_ = -+n  // expected-error {{unary operators must not be juxtaposed; parenthesize inner expression}}
_ = -++n // expected-error {{unary operators must not be juxtaposed; parenthesize inner expression}}

// <rdar://problem/16230507> Cannot use a negative constant as the second operator of ... operator
_ = 3...-5  // expected-error {{ambiguous missing whitespace between unary and binary operators}} expected-note {{could be postfix '...' and binary '-'}} expected-note {{could be binary '...' and prefix '-'}}


protocol P0 {
  static func %%%(lhs: Self, rhs: Self) -> Self
}

protocol P1 {
  func %%%(lhs: Self, rhs: Self) -> Self // expected-error{{operator '%%%' declared in protocol must be 'static'}}{{3-3=static }}
}

struct S0 {
  static func %%%(lhs: S0, rhs: S0) -> S0 { return lhs }
}

extension S0 {
  static func %%%%(lhs: S0, rhs: S0) -> S0 { return lhs }
}

struct S1 {
  func %%%(lhs: S1, rhs: S1) -> S1 { return lhs } // expected-error{{operator '%%%' declared in type 'S1' must be 'static'}}{{3-3=static }}
}

extension S1 {
  func %%%%(lhs: S1, rhs: S1) -> S1 { return lhs } // expected-error{{operator '%%%%' declared in extension of 'S1' must be 'static'}}{{3-3=static }}
}

class C0 {
  static func %%%(lhs: C0, rhs: C0) -> C0 { return lhs }
}

class C1 {
  final func %%%(lhs: C1, rhs: C1) -> C1 { return lhs } // expected-error{{operator '%%%' declared in type 'C1' must be 'static'}}{{3-3=static }}
}

final class C2 {
  class func %%%(lhs: C2, rhs: C2) -> C2 { return lhs }
}

class C3 {
  class func %%%(lhs: C3, rhs: C3) -> C3 { return lhs } // expected-error{{operator '%%%' declared in non-final class 'C3' must be 'final'}}{{3-3=final }}
}

class C4 {
  func %%%(lhs: C4, rhs: C4) -> C4 { return lhs } // expected-error{{operator '%%%' declared in type 'C4' must be 'static'}}{{3-3=static }}
}

struct Unrelated { }

struct S2 {
  static func %%%(lhs: Unrelated, rhs: Unrelated) -> Unrelated { }
  // expected-error@-1{{member operator '%%%' must have at least one argument of type 'S2'}}

  static func %%%(lhs: Unrelated, rhs: Unrelated) -> S2 { }
  // expected-error@-1{{member operator '%%%' must have at least one argument of type 'S2'}}

  static func %%%(lhs: Unrelated, rhs: Unrelated) -> S2.Type { }
  // expected-error@-1{{member operator '%%%' must have at least one argument of type 'S2'}}

  // Okay: refers to S2
  static func %%%(lhs: S2, rhs: Unrelated) -> Unrelated { }
  static func %%%(lhs: inout S2, rhs: Unrelated) -> Unrelated { }
  static func %%%(lhs: S2.Type, rhs: Unrelated) -> Unrelated { }
  static func %%%(lhs: inout S2.Type, rhs: Unrelated) -> Unrelated { }
  static func %%%(lhs: Unrelated, rhs: S2) -> Unrelated { }
  static func %%%(lhs: Unrelated, rhs: inout S2) -> Unrelated { }
  static func %%%(lhs: Unrelated, rhs: S2.Type) -> Unrelated { }
  static func %%%(lhs: Unrelated, rhs: inout S2.Type) -> Unrelated { }
}

extension S2 {
  static func %%%%(lhs: Unrelated, rhs: Unrelated) -> Unrelated { }
  // expected-error@-1{{member operator '%%%%' must have at least one argument of type 'S2'}}

  static func %%%%(lhs: Unrelated, rhs: Unrelated) -> S2 { }
  // expected-error@-1{{member operator '%%%%' must have at least one argument of type 'S2'}}

  static func %%%%(lhs: Unrelated, rhs: Unrelated) -> S2.Type { }
  // expected-error@-1{{member operator '%%%%' must have at least one argument of type 'S2'}}

  // Okay: refers to S2
  static func %%%%(lhs: S2, rhs: Unrelated) -> Unrelated { }
  static func %%%%(lhs: inout S2, rhs: Unrelated) -> Unrelated { }
  static func %%%%(lhs: S2.Type, rhs: Unrelated) -> Unrelated { }
  static func %%%%(lhs: inout S2.Type, rhs: Unrelated) -> Unrelated { }
  static func %%%%(lhs: Unrelated, rhs: S2) -> Unrelated { }
  static func %%%%(lhs: Unrelated, rhs: inout S2) -> Unrelated { }
  static func %%%%(lhs: Unrelated, rhs: S2.Type) -> Unrelated { }
  static func %%%%(lhs: Unrelated, rhs: inout S2.Type) -> Unrelated { }
}

protocol P2 {
  static func %%%(lhs: Unrelated, rhs: Unrelated) -> Unrelated
  // expected-error@-1{{member operator '%%%' of protocol 'P2' must have at least one argument of type 'Self'}}

  static func %%%(lhs: Unrelated, rhs: Unrelated) -> Self
  // expected-error@-1{{member operator '%%%' of protocol 'P2' must have at least one argument of type 'Self'}}

  static func %%%(lhs: Unrelated, rhs: Unrelated) -> Self.Type
  // expected-error@-1{{member operator '%%%' of protocol 'P2' must have at least one argument of type 'Self'}}

  // Okay: refers to Self
  static func %%%(lhs: Self, rhs: Unrelated) -> Unrelated
  static func %%%(lhs: inout Self, rhs: Unrelated) -> Unrelated
  static func %%%(lhs: Self.Type, rhs: Unrelated) -> Unrelated
  static func %%%(lhs: inout Self.Type, rhs: Unrelated) -> Unrelated
  static func %%%(lhs: Unrelated, rhs: Self) -> Unrelated
  static func %%%(lhs: Unrelated, rhs: inout Self) -> Unrelated
  static func %%%(lhs: Unrelated, rhs: Self.Type) -> Unrelated
  static func %%%(lhs: Unrelated, rhs: inout Self.Type) -> Unrelated
}

extension P2 {
  static func %%%%(lhs: Unrelated, rhs: Unrelated) -> Unrelated { }
  // expected-error@-1{{member operator '%%%%' of protocol 'P2' must have at least one argument of type 'Self'}}

  static func %%%%(lhs: Unrelated, rhs: Unrelated) -> Self { }
  // expected-error@-1{{member operator '%%%%' of protocol 'P2' must have at least one argument of type 'Self'}}

  static func %%%%(lhs: Unrelated, rhs: Unrelated) -> Self.Type { }
  // expected-error@-1{{member operator '%%%%' of protocol 'P2' must have at least one argument of type 'Self'}}

  // Okay: refers to Self
  static func %%%%(lhs: Self, rhs: Unrelated) -> Unrelated { }
  static func %%%%(lhs: inout Self, rhs: Unrelated) -> Unrelated { }
  static func %%%%(lhs: Self.Type, rhs: Unrelated) -> Unrelated { }
  static func %%%%(lhs: inout Self.Type, rhs: Unrelated) -> Unrelated { }
  static func %%%%(lhs: Unrelated, rhs: Self) -> Unrelated { }
  static func %%%%(lhs: Unrelated, rhs: inout Self) -> Unrelated { }
  static func %%%%(lhs: Unrelated, rhs: Self.Type) -> Unrelated { }
  static func %%%%(lhs: Unrelated, rhs: inout Self.Type) -> Unrelated { }
}

protocol P3 {
  // Not allowed: there's no way to infer 'Self' from this interface type
  static func %%%(lhs: P3, rhs: Unrelated) -> Unrelated
  // expected-error@-1 {{member operator '%%%' of protocol 'P3' must have at least one argument of type 'Self'}}
}

extension P3 {
  // Not allowed: there's no way to infer 'Self' from this interface type
  static func %%%%(lhs: P3, rhs: Unrelated) -> Unrelated { }
  // expected-error@-1 {{member operator '%%%%' of protocol 'P3' must have at least one argument of type 'Self'}}
}

// rdar://problem/27940842 - recovery with a non-static '=='.
class C5 {
  func == (lhs: C5, rhs: C5) -> Bool { return false } // expected-error{{operator '==' declared in type 'C5' must be 'static'}}

  func test1(x: C5) {
    _ = x == x
  }
}

class C6 {
  static func == (lhs: C6, rhs: C6) -> Bool { return false }

  func test1(x: C6) {
    if x == x && x = x { } // expected-error{{use of '=' in a boolean context, did you mean '=='?}} {{20-21===}}
    // expected-error@-1 {{cannot convert value of type 'C6' to expected argument type 'Bool'}}
  }
}

prefix operator ∫

prefix func ∫(arg: (Int, Int)) {}

func testPrefixOperatorOnTuple() {

  let foo = (1, 2)
  _ = ∫foo
  _ = (∫)foo
  // expected-error@-1 {{consecutive statements on a line must be separated by ';'}}
  // expected-warning@-2 {{expression of type '(Int, Int)' is unused}}
  (∫)(foo)
  _ = ∫(1, 2)
  _ = (∫)(1, 2) // expected-error {{operator function '∫' expects a single parameter of type '(Int, Int)'}}
  (∫)((1, 2))
}

postfix operator §

postfix func §<T, U>(arg: (T, (U, U), T)) {} // expected-note {{in call to operator '§'}}

func testPostfixOperatorOnTuple<A, B>(a: A, b: B) {

  let foo = (a, (b, b), a)
  _ = foo§

  // FIX-ME: "...could not be inferred" is irrelevant
  _ = (§)foo
  // expected-error@-1 {{consecutive statements on a line must be separated by ';'}}
  // expected-error@-2 {{generic parameter 'T' could not be inferred}}
  // expected-error@-3 {{generic parameter 'U' could not be inferred}}
  // expected-warning@-4 {{expression of type '(A, (B, B), A)' is unused}}
  (§)(foo)
  _ = (a, (b, b), a)§
  _ = (§)(a, (b, b), a) // expected-error {{operator function '§' expects a single parameter of type '(T, (U, U), T)'}}
  (§)((a, (b, b), a))
  _ = (a, ((), (b, (a, a), b)§), a)§
}

func testNonexistentPowerOperatorWithPowFunctionOutOfScope() {
  func a(_ value: Double) { }
  let x: Double = 3.0
  let y: Double = 3.0
  let z: Double = x**y // expected-error {{cannot find operator '**' in scope}}
  let w: Double = a(x**2.0) // expected-error {{cannot find operator '**' in scope}} expected-error {{cannot convert value of type '()' to specified type 'Double'}}
}
