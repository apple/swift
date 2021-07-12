// RUN: %target-typecheck-verify-swift -enable-experimental-concurrency

// REQUIRES: concurrency

actor MyActor { }

class MyActorSubclass1: MyActor { }
// expected-error@-1{{actor types do not support inheritance}}
// expected-error@-2{{type 'MyActorSubclass1' cannot conform to the 'Actor' protocol}}
// expected-error@-3{{non-final class 'MyActorSubclass1' cannot conform to `Sendable`; use `@unchecked Sendable`}}

actor MyActorSubclass2: MyActor { } // expected-error{{actor types do not support inheritance}}

// expected-error@+1{{keyword 'class' cannot be used as an identifier here}}
actor class MyActorClass { }

class NonActor { } // expected-note{{overridden declaration is here}}

actor NonActorSubclass : NonActor { } // expected-error{{actor types do not support inheritance}}
// expected-error@-1{{actor-isolated initializer 'init()' has different actor isolation from nonisolated overridden declaration}}

// expected-error@+1{{keyword 'class' cannot be used as an identifier here}}
public actor class BobHope {}
// expected-error@+1{{keyword 'public' cannot be used as an identifier here}}
actor public class BarbraStreisand {}
// expected-error@+1{{keyword 'struct' cannot be used as an identifier here}}
public actor struct JulieAndrews {}
// expected-error@+1{{keyword 'public' cannot be used as an identifier here}}
actor public enum TomHanks {}

open actor A1 { } // expected-error{{only classes and overridable class members can be declared 'open'; use 'public'}}

actor A2 {
  required init() { } // expected-error{{'required' initializer in non-class type 'A2'}}
  open func f() { } // expected-error{{only classes and overridable class members can be declared 'open'; use 'public'}}

  final func g() { } // okay for now
  class func h() { } // expected-error{{class methods are only allowed within classes; use 'static' to declare a static method}}
  static func i() { } // okay

  class var someProp: Int { 0 } // expected-error{{class properties are only allowed within classes; use 'static' to declare a static property}}
}

extension A2 {
  class func h2() { } // expected-error{{class methods are only allowed within classes; use 'static' to declare a static method}}
  static func i2() { } // okay

  class subscript(i: Int) -> Int { i } // expected-error{{class subscripts are only allowed within classes; use 'static' to declare a static subscript}}
  static subscript(s: String) -> String { s }
}
