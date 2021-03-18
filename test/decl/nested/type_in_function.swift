// RUN: %target-typecheck-verify-swift -parse-as-library

// Generic class locally defined in non-generic function (rdar://problem/20116710)
func f3() {
  class B<T> {}

  class C : B<Int> {}

  _ = B<Int>()
  _ = C()
}

// Type defined inside a closure (rdar://problem/31803589)
func hasAClosure() {
  _ = {
    enum E<T> { case a(T) }

    let _ = E.a("hi")
    let _ = E<String>.a("hi")
    let _: E = .a("hi")
    let _: E<String> = .a("hi")
  }
}

protocol Racoon {
  associatedtype Stripes
}

// Types inside generic functions -- not supported yet

func outerGenericFunction<T>(_ t: T) {
  struct InnerNonGeneric { // expected-error{{type 'InnerNonGeneric' cannot be nested in generic function 'outerGenericFunction'}}
    func nonGenericMethod(_ t: T) {}
    func genericMethod<V>(_ t: T) -> V where V : Racoon, V.Stripes == T {}
  }

  struct InnerGeneric<U> { // expected-error{{type 'InnerGeneric' cannot be nested in generic function 'outerGenericFunction'}}
    func nonGenericMethod(_ t: T, u: U) {}
    func genericMethod<V>(_ t: T, u: U) -> V where V : Racoon, V.Stripes == T {}
  }

  _ = {
    struct ConcreteInClosure { // expected-error{{type 'ConcreteInClosure' cannot be nested in closure in generic context}}
    }

    struct GenericInClosure<U> { // expected-error{{type 'GenericInClosure' cannot be nested in closure in generic context}}
    }
  }
}

class OuterNonGenericClass {
  func genericFunction<T>(_ t: T) {
    class InnerNonGenericClass : OuterNonGenericClass { // expected-error {{type 'InnerNonGenericClass' cannot be nested in generic function 'genericFunction'}}
      let t: T

      init(t: T) { super.init(); self.t = t }
    }

    class InnerGenericClass<U> : OuterNonGenericClass // expected-error {{type 'InnerGenericClass' cannot be nested in generic function 'genericFunction'}}
        where U : Racoon, U.Stripes == T {
      let t: T

      init(t: T) { super.init(); self.t = t }
    }
  }
}

class OuterGenericClass<T> {
  func genericFunction<U>(_ t: U) {
    class InnerNonGenericClass1 : OuterGenericClass { // expected-error {{type 'InnerNonGenericClass1' cannot be nested in generic function 'genericFunction'}}
      let t: T

      init(t: T) { super.init(); self.t = t }
    }

    class InnerNonGenericClass2 : OuterGenericClass<Int> { // expected-error {{type 'InnerNonGenericClass2' cannot be nested in generic function 'genericFunction'}}
      let t: T

      init(t: T) { super.init(); self.t = t }
    }

    class InnerNonGenericClass3 : OuterGenericClass<T> { // expected-error {{type 'InnerNonGenericClass3' cannot be nested in generic function 'genericFunction'}}
      let t: T

      init(t: T) { super.init(); self.t = t }
    }

    class InnerGenericClass<U> : OuterGenericClass<U> // expected-error {{type 'InnerGenericClass' cannot be nested in generic function 'genericFunction'}}
      where U : Racoon, U.Stripes == T {
      let t: T

      init(t: T) { super.init(); self.t = t }
    }
  }
}

// Name lookup within local classes.
func f5<T, U>(x: T, y: U) {
  struct Local { // expected-error {{type 'Local' cannot be nested in generic function 'f5(x:y:)'}}
    func f() {
      _ = 17 as T // expected-error{{'Int' is not convertible to 'T'}} 
      // expected-note@-1{{did you mean to use 'as!' to force downcast?}} {{14-16=as!}}
      _ = 17 as U // okay: refers to 'U' declared within the local class
    }
    typealias U = Int
  }
}

// Issue with gatherAllSubstitutions().
struct OuterGenericStruct<A> {
  class MiddleNonGenericClass {
    func nonGenericFunction() {
      class InnerGenericClass<T> : MiddleNonGenericClass {
      // expected-error@-1 {{type 'InnerGenericClass' cannot be nested in generic function 'nonGenericFunction()'}}
        override init() { super.init() }
      }
    }
  }

  func middleFunction() {
    struct ConformingType : Racoon {
    // expected-error@-1 {{type 'ConformingType' cannot be nested in generic function 'middleFunction()'}}
      typealias Stripes = A
    }
  }
}

// Issue with diagnoseUnknownType().
func genericFunction<T>(t: T) {
  class First : Second<T>.UnknownType { }
  // expected-error@-1 {{type 'First' cannot be nested in generic function 'genericFunction(t:)'}}
  // expected-error@-2 {{'UnknownType' is not a member type of generic class 'type_in_function.Second<T>'}}
  class Second<T> : Second { } // expected-note{{'Second' declared here}}
  // expected-error@-1 {{type 'Second' cannot be nested in generic function 'genericFunction(t:)'}}
  // expected-error@-2 {{'Second' inherits from itself}}
}

// Spurious "Self or associated type requirements" diagnostic.
protocol ProtoWithAssocType {
  associatedtype T = Int
}

func freeFunction() {
  struct ConformingType : ProtoWithAssocType {
    typealias T = Int

    func method() -> ProtoWithAssocType {}
    // expected-error@-1 {{can only be used as a conformance constraint because it has Self or associated type requirements}}
  }
}

// Superclass lookup archetype vs interface type mixup
class Generic<T> {
  struct Nested {}

  func outerMethod() {
    class Inner : Generic<T> { // expected-error {{type 'Inner' cannot be nested in generic function 'outerMethod()'}}
      func innerMethod() -> Nested {}
    }
  }
}
