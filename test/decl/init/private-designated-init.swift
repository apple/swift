// RUN: %target-typecheck-verify-swift

class Base {
  private init(x: Int) {}
  convenience init(y: Int) { self.init(x: y) }
}

class Derived: Base {}
// expected-error@-1 {{class 'Derived' has no initializers}}

class RequiredBase {
  private init(x: Int) {}
  required convenience init(y: Int) { self.init(x: y) }
  // expected-note@-1 {{'required' initializer is declared in superclass here}}
}

class RequiredDerived: RequiredBase {}
// expected-error@-1 {{class 'RequiredDerived' has no initializers}}
// expected-error@-1 {{'required' initializer 'init(y:)' must be provided by subclass of 'RequiredBase'}}