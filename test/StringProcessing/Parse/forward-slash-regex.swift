// RUN: %target-typecheck-verify-swift -enable-bare-slash-regex -disable-availability-checking -typo-correction-limit 0
// REQUIRES: swift_in_compiler
// REQUIRES: concurrency

prefix operator /
prefix operator ^/
prefix operator /^/

prefix func ^/ <T> (_ x: T) -> T { x }

prefix operator !!
prefix func !! <T>(_ x: T) -> T { x }

prefix operator ^^
prefix func ^^ <T>(_ x: T) -> T { x }

precedencegroup P {
  associativity: left
}

// The divisions in the body of the below operators make sure we don't try and
// consider them to be ending delimiters of a regex.
infix operator /^/ : P
func /^/ (lhs: Int, rhs: Int) -> Int { 1 / 2 }

infix operator /^ : P
func /^ (lhs: Int, rhs: Int) -> Int { 1 / 2 }

infix operator ^^/ : P
func ^^/ (lhs: Int, rhs: Int) -> Int { 1 / 2 }

let i = 0 /^/ 1/^/3

let x = /abc/
_ = /abc/
_ = /x/.self
_ = /\//
_ = /\\/

// This is just here to appease typo correction.
let y = 0

// These unfortunately become prefix `=` and infix `=/` respectively. We could
// likely improve the diagnostic though.
do {
  let z=/0/
  // expected-error@-1 {{type annotation missing in pattern}}
  // expected-error@-2 {{consecutive statements on a line must be separated by ';'}}
  // expected-error@-3 {{expected expression}}
}
do {
  _=/0/
  // expected-error@-1 {{'_' can only appear in a pattern or on the left side of an assignment}}
  // expected-error@-2 {{cannot find operator '=/' in scope}}
  // expected-error@-3 {{'/' is not a postfix unary operator}}
}

// No closing '/' so a prefix operator.
_ = /x
// expected-error@-1 {{'/' is not a prefix unary operator}}

_ = !/x/
// expected-error@-1 {{cannot convert value of type 'Regex<Substring>' to expected argument type 'Bool'}}

_ = (!/x/)
// expected-error@-1 {{cannot convert value of type 'Regex<Substring>' to expected argument type 'Bool'}}

_ = !/ /
// expected-error@-1 {{regex literal may not start with space; add backslash to escape}}
// expected-error@-2 {{cannot convert value of type 'Regex<Substring>' to expected argument type 'Bool'}}

_ = !!/ /
// expected-error@-1 {{regex literal may not start with space; add backslash to escape}}

_ = !!/x/
_ = (!!/x/)

_ = /^)
// expected-error@-1 {{unterminated regex literal}}
// expected-error@-2 {{closing ')' does not balance any groups openings}}

_ = /x/! // expected-error {{cannot force unwrap value of non-optional type 'Regex<Substring>'}}
_ = /x/ + /y/ // expected-error {{binary operator '+' cannot be applied to two 'Regex<Substring>' operands}}

_ = /x/+/y/
// expected-error@-1 {{cannot find operator '+/' in scope}}
// expected-error@-2 {{'/' is not a postfix unary operator}}

_ = /x/?.blah
// expected-error@-1 {{cannot use optional chaining on non-optional value of type 'Regex<Substring>'}}
// expected-error@-2 {{value of type 'Regex<Substring>' has no member 'blah'}}
_ = /x/!.blah
// expected-error@-1 {{cannot force unwrap value of non-optional type 'Regex<Substring>'}}
// expected-error@-2 {{value of type 'Regex<Substring>' has no member 'blah'}}

do {
  // expected-error@+2 {{cannot find operator '/?' in scope}}
  // expected-error@+1 {{'/' is not a prefix unary operator}}
  _ = /x /?
    .blah
  // expected-error@-1 {{cannot infer contextual base in reference to member 'blah'}}
}
_ = /x/? // expected-error {{cannot use optional chaining on non-optional value of type 'Regex<Substring>'}}
  .blah // expected-error {{value of type 'Regex<Substring>' has no member 'blah'}}

_ = 0; /x/ // expected-warning {{regular expression literal is unused}}

do {
  _ = 0; /x /
} // expected-error {{expected expression after operator}}

_ = /x/ ? 0 : 1 // expected-error {{cannot convert value of type 'Regex<Substring>' to expected condition type 'Bool'}}
do {
  _ = /x / ? 0 : 1 // expected-error@:12 {{expected expression after operator}}
}
_ = .random() ? /x/ : .blah // expected-error {{type 'Regex<Substring>' has no member 'blah'}}

_ = /x/ ?? /x/ // expected-warning {{left side of nil coalescing operator '??' has non-optional type 'Regex<Substring>', so the right side is never used}}
do {
  _ = /x / ?? /x / // expected-error {{unary operator cannot be separated from its operand}}
} // expected-error {{expected expression after operator}}

_ = /x/??/x/ // expected-error {{'/' is not a postfix unary operator}}
// expected-error@-1 2 {{cannot use optional chaining on non-optional value of type 'Regex<Substring>'}}

_ = /x/ ... /y/ // expected-error {{referencing operator function '...' on 'Comparable' requires that 'Regex<Substring>' conform to 'Comparable'}}

_ = /x/.../y/
// expected-error@-1 {{missing whitespace between '...' and '/' operators}}
// expected-error@-2 {{'/' is not a postfix unary operator}}

_ = /x/...
// expected-error@-1 {{unary operator '...' cannot be applied to an operand of type 'Regex<Substring>'}}

do {
  _ = /x /...
  // expected-error@-1 {{'/' is not a prefix unary operator}}
  // expected-error@-2 {{consecutive statements on a line must be separated by ';'}}
  // expected-error@-3 {{operator with postfix spacing cannot start a subexpression}}
} // expected-error {{expected expression}}

do {
  _ = true / false /; // expected-error {{expected expression after operator}}
}

_ = "\(/x/)"

func defaulted(x: Regex<Substring> = /x/) {}

func foo<T>(_ x: T, y: T) {} // expected-note {{'foo(_:y:)' declared here}}
foo(/abc/, y: /abc/)

// TODO: The parser ought to have better recovery in cases where a binary
// operator chain is missing an operand, currently we throw everything away.
foo(/abc/, y: /abc /)
// expected-error@-1:21 {{expected expression after operator}}
// expected-error@-2 {{missing argument for parameter 'y' in call}}

func bar<T>(_ x: inout T) {}
bar(&/x/) // expected-error {{cannot pass immutable value as inout argument: literals are not mutable}}

struct S {
  subscript(x: Regex<Substring>) -> Void { () } // expected-note {{'subscript(_:)' declared here}}
  subscript(fn: (Int, Int) -> Int) -> Int { 0 }
}

func testSubscript(_ x: S) {
  x[/x/]
  x[/x /]
  // expected-error@-1:9 {{expected expression after operator}}
  // expected-error@-2 {{missing argument for parameter #1 in subscript}}

  _ = x[/] / 2
}

func testReturn() -> Regex<Substring> {
  if .random() {
    return /x/
  }
  return /x /
} // expected-error {{expected expression after operator}}

func testThrow() throws {
  throw /x/ // expected-error {{thrown expression type 'Regex<Substring>' does not conform to 'Error'}}
}

do {
  _ = [/abc/, /abc /] // expected-error@:21 {{expected expression after operator}}
}
do {
  _ = [/abc /: /abc /]
  // expected-error@-1:14 {{expected expression after operator}}
}
_ = [/abc/:/abc/] // expected-error {{generic struct 'Dictionary' requires that 'Regex<Substring>' conform to 'Hashable'}}
_ = [/abc/ : /abc/] // expected-error {{generic struct 'Dictionary' requires that 'Regex<Substring>' conform to 'Hashable'}}
_ = [/abc/:/abc/] // expected-error {{generic struct 'Dictionary' requires that 'Regex<Substring>' conform to 'Hashable'}}
_ = [/abc/: /abc/] // expected-error {{generic struct 'Dictionary' requires that 'Regex<Substring>' conform to 'Hashable'}}
_ = (/abc/, /abc/)
_ = ((/abc/))

do {
  _ = ((/abc /))
  // expected-error@-1:15 {{expected expression after operator}}
}

_ = { /abc/ }
_ = {
  /abc/
}

let _: () -> Int = {
  0
  / 1 /
  2
}

let _: () -> Int = {
  0
  /1 / // expected-error {{'/' is not a prefix unary operator}}
  2
}

_ = {
  0 // expected-warning {{integer literal is unused}}
  /1/ // expected-warning {{regular expression literal is unused}}
  2 // expected-warning {{integer literal is unused}}
}

// Operator chain, as a regex literal may not start with space.
_ = 2
/ 1 / .bitWidth

_ = 2
/1/ .bitWidth // expected-error {{value of type 'Regex<Substring>' has no member 'bitWidth'}}

_ = 2
/ 1 /
  .bitWidth

_ = 2
/1 /
  .bitWidth
// expected-error@-2 {{'/' is not a prefix unary operator}}

_ = !!/1/ .bitWidth // expected-error {{value of type 'Regex<Substring>' has no member 'bitWidth'}}
_ = !!/1 / .bitWidth // expected-error {{cannot find operator '!!/' in scope}}

let z =
/y/

// While '.' is technically an operator character, it seems more likely that
// the user hasn't written the member name yet.
_ = 0. / 1 / 2 // expected-error {{expected member name following '.'}}
_ = 0 . / 1 / 2 // expected-error {{expected member name following '.'}}

switch "" {
case _ where /x/:
  // expected-error@-1 {{cannot convert value of type 'Regex<Substring>' to expected condition type 'Bool'}}
  break
default:
  break
}

do {} catch /x/ {}
// expected-error@-1 {{expression pattern of type 'Regex<Substring>' cannot match values of type 'any Error'}}
// expected-warning@-2 {{'catch' block is unreachable because no errors are thrown in 'do' block}}

switch /x/ {
default:
  break
}

if /x/ {} // expected-error {{cannot convert value of type 'Regex<Substring>' to expected condition type 'Bool'}}
if /x/.smth {} // expected-error {{value of type 'Regex<Substring>' has no member 'smth'}}

func testGuard() {
  guard /x/ else { return } // expected-error {{cannot convert value of type 'Regex<Substring>' to expected condition type 'Bool'}}
}

for x in [0] where /x/ {} // expected-error {{cannot convert value of type 'Regex<Substring>' to expected condition type 'Bool'}}

typealias Magic<T> = T
_ = /x/ as Magic
_ = /x/ as! String // expected-warning {{cast from 'Regex<Substring>' to unrelated type 'String' always fails}}

_ = type(of: /x/)

do {
  let /x/ // expected-error {{expected pattern}}
}

do {
  _ = try /x/; _ = try /x /
  // expected-warning@-1 {{no calls to throwing functions occur within 'try' expression}}
} // expected-error {{expected expression after operator}}
do {
  _ = try? /x/; _ = try? /x /
  // expected-warning@-1 {{no calls to throwing functions occur within 'try' expression}}
} // expected-error {{expected expression after operator}}
do {
  _ = try! /x/; _ = try! /x /
  // expected-warning@-1 {{no calls to throwing functions occur within 'try' expression}}
} // expected-error {{expected expression after operator}}

_ = await /x/ // expected-warning {{no 'async' operations occur within 'await' expression}}

/x/ = 0 // expected-error {{cannot assign to value: literals are not mutable}}
/x/() // expected-error {{cannot call value of non-function type 'Regex<Substring>'}}

// We treat the following as comments, as it seems more likely the user has
// written a comment and is still in the middle of writing the characters before
// it.
_ = /x// comment
// expected-error@-1 {{'/' is not a prefix unary operator}}

_ = /x // comment
// expected-error@-1 {{'/' is not a prefix unary operator}}

_ = /x/*comment*/
// expected-error@-1 {{'/' is not a prefix unary operator}}

// MARK: Unapplied operators

// These become regex literals, unless last character is space, or are surrounded in parens.
func baz(_ x: (Int, Int) -> Int, _ y: (Int, Int) -> Int) {} // expected-note 3{{'baz' declared here}}
baz(/, /)
baz(/,/)
// expected-error@-1 {{cannot convert value of type 'Regex<Substring>' to expected argument type '(Int, Int) -> Int'}}
// expected-error@-2 {{missing argument for parameter #2 in call}}

baz((/), /)

baz(/^, /)
baz(/^,/)
// expected-error@-1 {{cannot convert value of type 'Regex<Substring>' to expected argument type '(Int, Int) -> Int'}}
// expected-error@-2 {{missing argument for parameter #2 in call}}

baz((/^), /)

baz(^^/, /)
baz(^^/,/) // expected-error {{missing argument for parameter #2 in call}}
baz((^^/), /)

func bazbaz(_ x: (Int, Int) -> Int, _ y: Int) {}
bazbaz(/, 0)
bazbaz(^^/, 0)

func qux<T>(_ x: (Int, Int) -> Int, _ y: T) -> Int { 0 }
_ = qux(/, 1) / 2
do {
  _ = qux(/, "(") / 2

  _ = qux(/, "(")/2
  // expected-error@-1 {{cannot convert value of type 'Regex<(Substring, Substring)>' to expected argument type '(Int, Int) -> Int'}}
  // expected-error@-2:19 {{expected ',' separator}}
}
_ = qux((/), "(") / 2
_ = qux(/, 1) // this comment tests to make sure we don't try and end the regex on the starting '/' of '//'.
_ = qux(/, 1) /* same thing with a block comment */

@discardableResult
func quxqux(_ x: (Int, Int) -> Int) -> Int { 0 }
quxqux(/^/) // expected-error {{cannot convert value of type 'Regex<Substring>' to expected argument type '(Int, Int) -> Int'}}
quxqux((/^/)) // expected-error {{cannot convert value of type 'Regex<Substring>' to expected argument type '(Int, Int) -> Int'}}
quxqux({ $0 /^/ $1 })

quxqux(!/^/)
// expected-error@-1 {{cannot convert value of type 'Bool' to expected argument type '(Int, Int) -> Int'}}
// expected-error@-2 {{cannot convert value of type 'Regex<Substring>' to expected argument type 'Bool'}}

quxqux(/^)
_ = quxqux(/^) / 1

let arr: [Double] = [2, 3, 4]
_ = arr.reduce(1, /) / 3
_ = arr.reduce(1, /) + arr.reduce(1, /)

// MARK: ')' disambiguation behavior

_ = (/x)
// expected-error@-1 {{'/' is not a prefix unary operator}}

_ = (/x)/
// expected-error@-1 {{'/' is not a prefix unary operator}}
// expected-error@-2 {{'/' is not a postfix unary operator}}

_ = (/[(0)])/
// expected-error@-1 {{'/' is not a prefix unary operator}}
// expected-error@-2 {{'/' is not a postfix unary operator}}

_ = /[(0)]/
_ = /(x)/
_ = /[)]/
_ = /[a\])]/
_ = /([)])/
_ = /]]][)]/

_ = /
// expected-error@-1 {{unterminated regex literal}}

_ = /)
// expected-error@-1 {{unterminated regex literal}}
// expected-error@-2 {{closing ')' does not balance any groups openings}}

let fn: (Int, Int) -> Int = (/)

_ = /\()/
// expected-error@-1 {{'/' is not a prefix unary operator}}
// expected-error@-2 {{'/' is not a postfix unary operator}}
// expected-error@-3 {{invalid component of Swift key path}}

do {
  let _: Regex = (/whatever\)/
  // expected-note@-1 {{to match this opening '('}}
} // expected-error {{expected ')' in expression list}}
do {
  _ = /(()()))/
  // expected-error@-1 {{'/' is not a prefix unary operator}}
  // expected-error@-2 {{consecutive statements on a line must be separated by ';'}}
  // expected-error@-3 {{expected expression}}
  // expected-error@-4 {{cannot call value of non-function type '()'}}
}
do {
  _ = /[x])/
  // expected-error@-1 {{'/' is not a prefix unary operator}}
  // expected-error@-2 {{consecutive statements on a line must be separated by ';'}}
  // expected-error@-3 {{expected expression}}
}
do {
  _ = /[\]])/
  // expected-error@-1 {{expected expression path in Swift key path}}
}

_ = ^/x/
// expected-error@-1 {{'^' is not a prefix unary operator}}

_ = (^/x)/
// expected-error@-1 {{'/' is not a postfix unary operator}}

_ = (!!/x/)

_ = ^/"/"
// expected-error@-1 {{'^' is not a prefix unary operator}}
// expected-error@-2 {{unterminated string literal}}

_ = ^/"[/"
// expected-error@-1 {{'^' is not a prefix unary operator}}
// expected-error@-2 {{unterminated string literal}}
// expected-error@-3 {{expected custom character class members}}

_ = (^/)("/")

// MARK: Starting characters

// Fine.
_ = /./

// You need to escape if you want a regex literal to start with these characters.
_ = /\ /
_ = / / // expected-error {{regex literal may not start with space; add backslash to escape}} {{6-6=\}}
_ = /  /
// expected-error@-1 {{regex literal may not start with space; add backslash to escape}} {{6-6=\}}
// expected-error@-2 {{regex literal may not end with space; use extended literal instead}} {{5-5=#}} {{9-9=#}}
_ = #/  /#
_ = /x\ /
_ = /\ \ /

// There are intentionally trailing spaces here
_ = /      
// expected-error@-1 {{unterminated regex literal}}
// expected-error@-2 {{regex literal may not start with space; add backslash to escape}} {{6-6=\}}

// There are intentionally trailing spaces here
_ = /^        
// expected-error@-1 {{unterminated regex literal}}

_ = /\)/
_ = /)/ // expected-error {{closing ')' does not balance any groups openings}}

_ = /,/
_ = /}/
_ = /]/
_ = /:/
_ = /;/

// Don't emit diagnostics here, as we re-lex.
_ = /0xG/
_ = /0oG/
_ = /"/
_ = /'/
_ = /<#placeholder#>/

_ = ^^/0xG/
_ = ^^/0oG/
_ = ^^/"/
_ = ^^/'/
_ = ^^/<#placeholder#>/

_ = (^^/0xG/)
_ = (^^/0oG/)
_ = (^^/"/)
_ = (^^/'/)
_ = (^^/<#placeholder#>/)
