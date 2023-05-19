// RUN: %target-typecheck-verify-swift

prefix operator +++ {} // expected-error {{operator should no longer be declared with body}} {{20-23=}}
postfix operator +++ {} // expected-error {{operator should no longer be declared with body}} {{21-24=}}
infix operator +++ {} // expected-error {{operator should no longer be declared with body}} {{19-22=}}
infix operator +++* { // expected-error {{operator should no longer be declared with body; use a precedence group instead}} {{none}}
  associativity right
}
infix operator +++*+ : A { } // expected-error {{operator should no longer be declared with body}} {{25-29=}}


prefix operator +++** : A { }
// expected-error@-1 {{only infix operators may declare a precedence}} {{23-27=}}
// expected-error@-2 {{operator should no longer be declared with body}} {{26-30=}}

prefix operator ++*++ : A
// expected-error@-1 {{only infix operators may declare a precedence}} {{23-26=}}

postfix operator ++*+* : A { }
// expected-error@-1 {{only infix operators may declare a precedence}} {{24-28=}}
// expected-error@-2 {{operator should no longer be declared with body}} {{27-31=}}

postfix operator ++**+ : A
// expected-error@-1 {{only infix operators may declare a precedence}} {{24-27=}}

operator ++*** : A
// expected-error@-1 {{operator must be declared as 'prefix', 'postfix', or 'infix'}}

operator +*+++ { }
// expected-error@-1 {{operator must be declared as 'prefix', 'postfix', or 'infix'}}
// expected-error@-2 {{operator should no longer be declared with body}} {{15-19=}}

operator +*++* : A { }
// expected-error@-1 {{operator must be declared as 'prefix', 'postfix', or 'infix'}}
// expected-error@-2 {{operator should no longer be declared with body}} {{19-23=}}

prefix operator // expected-error {{expected operator name in operator declaration}}

;
prefix operator %%+

prefix operator ??
postfix operator ?? // expected-error {{postfix operator names starting with '?' or '!' are disallowed to avoid collisions with built-in unwrapping operators}}
prefix operator !!
postfix operator !! // expected-error {{postfix operator names starting with '?' or '!' are disallowed to avoid collisions with built-in unwrapping operators}}
postfix operator ?$$
// expected-error@-1 {{postfix operator names starting with '?' or '!' are disallowed}}
// expected-error@-2 {{'$$' is considered an identifier}}

infix operator --aa // expected-error {{'aa' is considered an identifier and must not appear within an operator name}}
infix operator aa--: A // expected-error {{'aa' is considered an identifier and must not appear within an operator name}}
infix operator <<$$@< // expected-error {{'$$' is considered an identifier and must not appear within an operator name}}
infix operator !!@aa // expected-error {{'@' is not allowed in operator names}}
infix operator #++= // expected-error {{'#' is not allowed in operator names}}
infix operator ++=# // expected-error {{'#' is not allowed in operator names}}
infix operator -># // expected-error {{'#' is not allowed in operator names}}

// FIXME: Ideally, we shouldn't emit the «consistent whitespace» diagnostic
// where = cannot possibly mean an assignment.
infix operator =#=
// expected-error@-1 {{'#' is not allowed in operator names}}
// expected-error@-2 {{'=' must have consistent whitespace on both sides}}

infix operator +++=
infix operator *** : A
infix operator --- : ; // expected-error {{expected precedence group name after ':' in operator declaration}}

precedencegroup { // expected-error {{expected identifier after 'precedencegroup'}}
  associativity: right
}
precedencegroup A {
  associativity right // expected-error {{expected colon after attribute name in precedence group}}
}
precedencegroup B {
  precedence 123 // expected-error {{'precedence' is not a valid precedence group attribute}}
}
precedencegroup C {
  associativity: sinister // expected-error {{expected 'none', 'left', or 'right' after 'associativity'}}
}
precedencegroup D {
  assignment: no // expected-error {{expected 'true' or 'false' after 'assignment'}}
}
precedencegroup E {
  higherThan:
} // expected-error {{expected name of related precedence group after 'higherThan'}}

precedencegroup F {
  higherThan: A, B, C
}


precedencegroup BangBangBang {
  associativity: none
  associativity: left // expected-error{{'associativity' attribute for precedence group declared multiple times}}
}

precedencegroup CaretCaretCaret {
  assignment: true 
  assignment: false // expected-error{{'assignment' attribute for precedence group declared multiple times}}
}

class Foo {
  infix operator ||| // expected-error{{'operator' may only be declared at file scope}}
}

infix operator **<< : UndeclaredPrecedenceGroup
// expected-error@-1 {{unknown precedence group 'UndeclaredPrecedenceGroup'}}

protocol Proto {}
infix operator *<*< : F, Proto
// expected-error@-1 {{consecutive statements on a line must be separated by ';'}}
// expected-error@-2 {{expected expression}}

// https://github.com/apple/swift/issues/60932

// expected-error@+2 {{expected precedence group name after ':' in operator declaration}}
postfix operator ++: // expected-error {{only infix operators may declare a precedence}} {{20-21=}}
