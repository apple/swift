// RUN: %target-typecheck-verify-swift

// https://github.com/apple/swift/issues/52877

// RUN: %target-typecheck-verify-swift

struct SR13098 { // expected-note {{in declaration of 'SR13098'}}
  var srValue = 0xABABABAB // expected-note {{'srValue' previously declared here}}
  var a: Int 

  srValue = 0xCDCDCDCD // expected-error {{invalid redeclaration of 'srValue'}}
                       // expected-error @-1 {{expected 'var' keyword in property declaration}}
                        
  
  // ensure that the id.id pattern is not interpreted as a function
  a.foo = 345 // expected-error {{expected declaration}}

  // note that the expected declaration blocks further diagnostics up to 
  // the closing right brace.
  srValue.bar = 345
}
 

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
  class classProperty: Int { 0 } // expected-error {{inheritance from non-protocol, non-class type 'Int'}}
                                 // expected-note @-1 {{in declaration of 'classProperty'}}
                                 // expected-error @-2 {{expected declaration}}
}
