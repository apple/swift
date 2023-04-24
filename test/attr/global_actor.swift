// RUN: %target-swift-frontend -typecheck -verify %s  -disable-availability-checking -package-name myPkg
// REQUIRES: concurrency

actor SomeActor { }

// -----------------------------------------------------------------------
// @globalActor attribute itself.
// -----------------------------------------------------------------------

// Well-formed global actor.
@globalActor
struct GA1 {
  static let shared = SomeActor()
}

@globalActor
struct GenericGlobalActor<T> {
  static var shared: SomeActor { SomeActor() }
}

// Ill-formed global actors.
@globalActor
final class GA2 { // expected-error{{type 'GA2' does not conform to protocol 'GlobalActor'}}
}

@globalActor
struct GA3 { // expected-error{{type 'GA3' does not conform to protocol 'GlobalActor'}}
  let shared = SomeActor()
}

@globalActor
struct GA4 {
  private static let shared = SomeActor() // expected-error{{property 'shared' must be as accessible as its enclosing type because it matches a requirement in protocol 'GlobalActor'}}
  // expected-note@-1{{mark the static property as 'internal' to satisfy the requirement}}
}

@globalActor
open class GA5 { // expected-error{{non-final class 'GA5' cannot be a global actor}}
  static let shared = SomeActor() // expected-error{{property 'shared' must be declared public because it matches a requirement in public protocol 'GlobalActor'}}
  // expected-note@-1{{mark the static property as 'public' to satisfy the requirement}}
}

@globalActor
struct GA6<T> { // expected-error{{type 'GA6<T>' does not conform to protocol 'GlobalActor'}}
}

extension GA6 where T: Equatable {
  static var shared: SomeActor { SomeActor() }
}

@globalActor
final class GA7 { // expected-error{{type 'GA7' does not conform to protocol 'GlobalActor'}}
  static let shared = 5 // expected-note{{candidate would match and infer 'ActorType' = 'Int' if 'Int' conformed to 'Actor'}}
}

// -----------------------------------------------------------------------
// Applying global actors to entities.
// -----------------------------------------------------------------------
@globalActor
struct OtherGlobalActor {
  static let shared = SomeActor()
}

@GA1 func f() {
  @GA1 let x = 17 // expected-error{{local variable 'x' cannot have a global actor}}
  _ = x
}

@GA1 struct X {
  @GA1 var member: Int // expected-warning {{stored property 'member' within struct cannot have a global actor; this is an error in Swift 6}}
}

struct Y {
  @GA1 subscript(i: Int) -> Int { i }
}

@GA1 extension Y { }

@GA1 func g() { }

class SomeClass {
  @GA1 init() { }
  @GA1 deinit { }
}

@GA1 typealias Integer = Int // expected-error{{type alias cannot have a global actor}}

@GA1 actor ActorInTooManyPlaces { } // expected-error{{actor 'ActorInTooManyPlaces' cannot have a global actor}}

@GA1 @OtherGlobalActor func twoGlobalActors() { } // expected-error{{declaration can not have multiple global actor attributes ('OtherGlobalActor' and 'GA1')}}

struct Container {
  // FIXME: Diagnostic could be improved to show the generic arguments.
@GenericGlobalActor<Int> @GenericGlobalActor<String> func twoGenericGlobalActors() { } // expected-error{{declaration can not have multiple global actor attributes ('GenericGlobalActor' and 'GenericGlobalActor')}}
}

// -----------------------------------------------------------------------
// Redundant attributes
// -----------------------------------------------------------------------
extension SomeActor {
  @GA1 nonisolated func conflict1() { } // expected-error 3{{instance method 'conflict1()' has multiple actor-isolation attributes ('nonisolated' and 'GA1')}}
}


// -----------------------------------------------------------------------
// Access
// -----------------------------------------------------------------------

@globalActor
private struct PrivateGA { // expected-note 3 {{type declared here}}
  actor Actor {}
  static let shared = Actor()
}

@globalActor
internal struct InternalGA { // expected-note 2 {{type declared here}}
  actor Actor {}
  static let shared = Actor()
}

@globalActor
package struct PackageGA { // expected-note 1 {{type declared here}}
  package actor Actor {}
  package static let shared = Actor()
}

@globalActor
public struct PublicGA {
  public actor Actor {}
  public static let shared = Actor()
}

@PrivateGA private struct PrivateStructPrivateGA {}
@InternalGA private struct PrivateStructInternalGA {}
@PackageGA private struct PrivateStructPackageGA {}
@PublicGA private struct PrivateStructPublicGA {}

@PrivateGA internal struct InternalStructPrivateGA {} // expected-error {{internal struct 'InternalStructPrivateGA' cannot have private global actor 'PrivateGA'}}
@InternalGA internal struct InternalStructInternalGA {}
@PackageGA internal struct InternalStructPackageGA {}
@PublicGA internal struct InternalStructPublicGA {}

@PrivateGA package class PackageClassPrivateGA {} // expected-error {{package class 'PackageClassPrivateGA' cannot have private global actor 'PrivateGA'}}
@InternalGA package class PackageClassInternalGA {} // expected-error {{package class 'PackageClassInternalGA' cannot have internal global actor 'InternalGA'}}
@PackageGA package struct PackageClassPackageGA {}
@PublicGA package class PackageClassPublicGA {}

@PrivateGA open class OpenClassPrivateGA {} // expected-error {{open class 'OpenClassPrivateGA' cannot have private global actor 'PrivateGA'}}
@InternalGA open class OpenClassInternalGA {} // expected-error {{open class 'OpenClassInternalGA' cannot have internal global actor 'InternalGA'}}
@PackageGA open class OpenClassPackageGA {} // expected-error {{open class 'OpenClassPackageGA' cannot have package global actor 'PackageGA'}}
@PublicGA open class OpenClassPublicGA {}

// rdar://99281333 - no accessors/addressors/observers expect to 'get' are allowed to have a global actor attribute
do {
  class TestInvalidAccessors {
    var test1: Int {
      get { 42 }
      @GA1
      set { } // expected-warning {{setter cannot have a global actor}} {{158:7-11=}}
      // expected-note@-1 {{move global actor attribute to property 'test1'}} {{156:5-5=@GA1}}

      @GA1 _modify { fatalError() } // expected-warning {{_modify accessor cannot have a global actor}} {{7-12=}}
      // expected-note@-1 {{move global actor attribute to property 'test1'}} {{156:5-5=@GA1}}
    }

    func local() {
      var test: Bool {
        get { false }

        @GA1
        set { } // expected-warning {{setter cannot have a global actor}}
      }
    }

    @GA1 var testAlreadyWithGlobal: String {
      get { "" }
      @GA1 set { } // expected-warning {{setter cannot have a global actor}} {{7-12=}}
    }
  }

  struct TestStruct {
    var test1: Int {
      get { 42 }
      @GA1
      set { } // expected-warning {{setter cannot have a global actor}} {{184:7-11=}}
      // expected-note@-1 {{move global actor attribute to property 'test1'}} {{182:5-5=@GA1}}
      @GA1 _modify { fatalError() } // expected-warning {{_modify accessor cannot have a global actor}} {{7-12=}}
      // expected-note@-1 {{move global actor attribute to property 'test1'}} {{182:5-5=@GA1}}
    }

    var test2: Int {
      @GA1 willSet { // expected-warning {{willSet observer cannot have a global actor}} {{7-12=}}
        // expected-note@-1 {{move global actor attribute to property 'test2'}} {{191:5-5=@GA1}}
      }
    }

    subscript(x: Int) -> Bool {
      get { true }
      @GA1 set { } // expected-warning {{setter cannot have a global actor}} {{7-12=}}
      // expected-note@-1 {{move global actor attribute to subscript 'subscript(_:)'}} {{197:5-5=@GA1}}
    }

    @GA1 subscript(y: Bool) -> String {
      get { "" }
      @GA1 set { } // expected-warning {{setter cannot have a global actor}} {{7-12=}}
    }
  }
}
