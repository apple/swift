// RUN: %target-typecheck-verify-swift

// https://bugs.swift.org/browse/SR-10477

protocol Brew { // expected-note {{in declaration of 'Brew'}}
  tripel() -> Int // expected-error {{expected 'func' keyword in instance method declaration}} {{3-3=func }}

  quadrupel: Int { get } // expected-error {{expected 'var' keyword in property declaration}} {{3-3=var }}

  static + (lhs: Self, rhs: Self) -> Self // expected-error {{expected 'func' keyword in operator function declaration}} {{10-10=func }}

  * (lhs: Self, rhs: Self) -> Self // expected-error {{expected 'func' keyword in operator function declaration}} {{3-3=func }}
                                   // expected-error @-1 {{operator '*' declared in protocol must be 'static'}} {{3-3=static }}

  ipa: Int { get }; apa: Float { get }
  // expected-error @-1 {{expected 'var' keyword in property declaration}} {{3-3=var }}
  // expected-error @-2 {{expected 'var' keyword in property declaration}} {{21-21=var }}

  stout: Int { get } porter: Float { get }
  // expected-error @-1 {{expected 'var' keyword in property declaration}} {{3-3=var }}
  // expected-error @-2 {{expected declaration}}
  // expected-error @-3 {{consecutive declarations on a line must be separated by ';'}}
}

infix operator %%

struct Bar {
  fisr = 0x5F3759DF // expected-error {{expected 'var' keyword in property declaration}} {{3-3=var }}

  %%<T: Brew> (lhs: T, rhs: T) -> T { // expected-error {{expected 'func' keyword in operator function declaration}} {{3-3=func }}
                                      // expected-error @-1 {{operator '%%' declared in type 'Bar' must be 'static'}}
                                      // expected-error @-2 {{member operator '%%' must have at least one argument of type 'Bar'}}
    lhs + lhs + rhs + rhs
  }

  _: Int = 42 // expected-error {{expected 'var' keyword in property declaration}} {{3-3=var }}
              // expected-error @-1 {{property declaration does not bind any variables}}

  (light, dark) = (100, 200)// expected-error {{expected 'var' keyword in property declaration}} {{3-3=var }}
  
  a, b: Int // expected-error {{expected 'var' keyword in property declaration}} {{3-3=var }}
}

class Baz {

  instanceMethod() {} // expected-error {{expected 'func' keyword in instance method declaration}} {{3-3=func }}

  static staticMethod() {} // expected-error {{expected 'func' keyword in static method declaration}} {{10-10=func }}

  instanceProperty: Int { 0 } // expected-error {{expected 'var' keyword in property declaration}} {{3-3=var }}

  static staticProperty: Int { 0 } // expected-error {{expected 'var' keyword in static property declaration}} {{10-10=var }}
}

class C1 {
  class classMethod() {} // expected-error {{expected '{' in class}}
}

class C2 {
  class classProperty: Int { 0 } // expected-error {{class type cannot inherit from non-protocol, non-class type 'Int'}}
                                 // expected-note @-1 {{in declaration of 'classProperty'}}
                                 // expected-error @-2 {{expected declaration}}
}
