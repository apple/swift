// RUN: %target-typecheck-verify-swift
// REQUIRES: swift_swift_parser
// FIXME: Swift parser is not enabled on Linux CI yet.
// REQUIRES: OS=macosx

// rdar://15946844
func test1(inout var x : Int) {}  // expected-warning {{'var' in this position is interpreted as an argument label}} {{18-21=`var`}}
// expected-error @-1 {{'inout' before a parameter name is not allowed, place it before the parameter type instead}} {{12-17=}} {{26-26=inout }}
func test2(inout let x : Int) {}  // expected-warning {{'let' in this position is interpreted as an argument label}} {{18-21=`let`}}
// expected-error @-1 {{'inout' before a parameter name is not allowed, place it before the parameter type instead}} {{12-17=}} {{26-26=inout }}
func test3(f : (inout _ x : Int) -> Void) {} // expected-error {{'inout' before a parameter name is not allowed, place it before the parameter type instead}}

func test3() {
  undeclared_func( // expected-error {{cannot find 'undeclared_func' in scope}}
} // expected-error {{expected expression in list of expressions}}

func runAction() {} // expected-note {{did you mean 'runAction'?}}

// rdar://16601779
func foo() {
  // expected-error@+3 {{argument passed to call that takes no arguments}}
  // expected-error@+2 {{cannot find 'SKAction' in scope}}
  // expected-error@+1 {{expected ',' separator}}
  runAction(SKAction.sequence()

    skview!
    // expected-error @-1 {{cannot find 'skview' in scope}}
}

super.init() // expected-error {{'super' cannot be used outside of class members}}

switch state { // expected-error {{cannot find 'state' in scope}}
  let duration : Int = 0 // expected-error {{all statements inside a switch must be covered by a 'case' or 'default'}}
  case 1:
    break
}

func testNotCoveredCase(x: Int) {
  switch x {
    let y = "foo" // expected-error {{all statements inside a switch must be covered by a 'case' or 'default'}}
    switch y {
      case "bar":
        blah blah // ignored
    }
  case "baz": // expected-error {{expression pattern of type 'String' cannot match values of type 'Int'}}
    break
  case 1:
    break
  default:
    break
  }
}

// rdar://18926814
func test4() {
  let abc = 123
  _ = " >> \( abc } ) << " // expected-error {{expected ',' separator}} {{18-18=,}}  expected-error {{expected expression in list of expressions}}

}

// rdar://problem/18507467
func d(_ b: String -> <T>() -> T) {} // expected-error {{expected type for function result}}


// <rdar://problem/22143680> QoI: terrible diagnostic when trying to form a generic protocol
protocol Animal<Food> {  // expected-error {{an associated type named 'Food' must be declared in the protocol 'Animal' or a protocol it inherits}}
  func feed(_ food: Food) // expected-error {{cannot find type 'Food' in scope}}
}


// https://github.com/apple/swift/issues/43190
// Crash with invalid parameter declaration
do {
  class Starfish {}
  struct Salmon {}
  func f(s Starfish,  // expected-error {{expected ':' following argument label and parameter name}}
            _ ss: Salmon) -> [Int] {}
  func g() { f(Starfish(), Salmon()) }
}

do {
  func f(_ a: Int, b: Int) {}
  f(1, b: 2,)
}

// https://github.com/apple/swift/issues/43591
// Two inout crash compiler

func f1_43591(a : inout inout Int) {}  // expected-error {{parameter may have at most one of the 'inout', 'borrowing', or 'consuming' specifiers}} {{19-25=}}
func f2_43591(inout inout b: Int) {} // expected-error {{inout' before a parameter name is not allowed, place it before the parameter type instead}} {{15-20=}} {{30-30=inout }}
// expected-error@-1 {{parameter may have at most one of the 'inout', 'borrowing', or 'consuming' specifiers}} {{21-27=}}
func f3_43591(let let a: Int) {} // expected-warning {{'let' in this position is interpreted as an argument label}} {{15-18=`let`}}
// expected-error @-1 {{expected ',' separator}} {{22-22=,}}
// expected-error @-2 {{expected ':' following argument label and parameter name}}
// expected-warning @-3 {{extraneous duplicate parameter name; 'let' already has an argument label}} {{15-19=}}
func f4_43591(inout x: inout String) {} // expected-error {{parameter may have at most one of the 'inout', 'borrowing', or 'consuming' specifiers}}
func f5_43591(inout i: inout Int) {} // expected-error {{parameter may have at most one of the 'inout', 'borrowing', or 'consuming' specifiers}} {{15-20=}}

func repeat() {}
// expected-error @-1 {{keyword 'repeat' cannot be used as an identifier here}}
// expected-note @-2 {{if this name is unavoidable, use backticks to escape it}} {{6-12=`repeat`}}

let for = 2
// expected-error @-1 {{keyword 'for' cannot be used as an identifier here}}
// expected-note @-2 {{if this name is unavoidable, use backticks to escape it}} {{5-8=`for`}}

func dog cow() {} // expected-error {{found an unexpected second identifier in function declaration; is there an accidental break?}}
// expected-note@-1 {{join the identifiers together}} {{6-13=dogcow}}
// expected-note@-2 {{join the identifiers together with camel-case}} {{6-13=dogCow}}
func cat Mouse() {} // expected-error {{found an unexpected second identifier in function declaration; is there an accidental break?}}
// expected-note@-1 {{join the identifiers together}} {{6-15=catMouse}}
func friend ship<T>(x: T) {} // expected-error {{found an unexpected second identifier in function declaration; is there an accidental break?}}
// expected-note@-1 {{join the identifiers together}} {{6-17=friendship}}
// expected-note@-2 {{join the identifiers together with camel-case}} {{6-17=friendShip}}
func were
wolf() {} // expected-error {{found an unexpected second identifier in function declaration; is there an accidental break?}}
// expected-note@-1 {{join the identifiers together}} {{-1:6-+0:5=werewolf}}
// expected-note@-2 {{join the identifiers together with camel-case}} {{-1:6-+0:5=wereWolf}}
func hammer
leavings<T>(x: T) {} // expected-error {{found an unexpected second identifier in function declaration; is there an accidental break?}}
// expected-note@-1 {{join the identifiers together}} {{-1:6-+0:9=hammerleavings}}
// expected-note@-2 {{join the identifiers together with camel-case}} {{-1:6-+0:9=hammerLeavings}}

prefix operator %
prefix func %<T>(x: T) -> T { return x } // No error expected - the < is considered an identifier but is peeled off by the parser.

struct Weak<T: class> { // expected-error {{'class' constraint can only appear on protocol declarations}}
  // expected-note@-1 {{did you mean to write an 'AnyObject' constraint?}} {{16-21=AnyObject}}
  weak let value: T // expected-error {{'weak' must be a mutable variable, because it may change at runtime}} expected-error {{'weak' variable should have optional type 'T?'}} expected-error {{'weak' must not be applied to non-class-bound 'T'; consider adding a protocol conformance that has a class bound}}
}

let x: () = ()
!() // expected-error {{cannot convert value of type '()' to expected argument type 'Bool'}}
!(()) // expected-error {{cannot convert value of type '()' to expected argument type 'Bool'}}
!(x) // expected-error {{cannot convert value of type '()' to expected argument type 'Bool'}}
!x // expected-error {{cannot convert value of type '()' to expected argument type 'Bool'}}

// https://github.com/apple/swift/issues/50734

func f1_50734(@NSApplicationMain x: Int) {} // expected-error {{@NSApplicationMain may only be used on 'class' declarations}}
func f2_50734(@available(iOS, deprecated: 0) x: Int) {} // expected-error {{'@available' attribute cannot be applied to this declaration}}
func f3_50734(@discardableResult x: Int) {} // expected-error {{'@discardableResult' attribute cannot be applied to this declaration}}
func f4_50734(@objcMembers x: String) {} // expected-error {{@objcMembers may only be used on 'class' declarations}}
func f5_50734(@weak x: String) {} // expected-error {{'weak' is a declaration modifier, not an attribute}} expected-error {{'weak' may only be used on 'var' declarations}}

class C_50734<@NSApplicationMain T: AnyObject> {} // expected-error {{@NSApplicationMain may only be used on 'class' declarations}}
func f6_50734<@discardableResult T>(x: T) {} // expected-error {{'@discardableResult' attribute cannot be applied to this declaration}}
enum E_50734<@indirect T> {} // expected-error {{'indirect' is a declaration modifier, not an attribute}} expected-error {{'indirect' modifier cannot be applied to this declaration}}
protocol P {
  @available(swift, introduced: 4) associatedtype Assoc
}
