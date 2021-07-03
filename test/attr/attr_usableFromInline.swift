// RUN: %target-typecheck-verify-swift -swift-version 5 -disable-objc-attr-requires-foundation-module -enable-objc-interop -enable-access-control-hacks
// RUN: %target-typecheck-verify-swift -swift-version 5 -disable-objc-attr-requires-foundation-module -enable-objc-interop -enable-testing -enable-access-control-hacks

@usableFromInline private func privateVersioned() {}
// expected-error@-1 {{'@usableFromInline' attribute can only be applied to internal declarations, but 'privateVersioned()' is private}}

@usableFromInline fileprivate func fileprivateVersioned() {}
// expected-error@-1 {{'@usableFromInline' attribute can only be applied to internal declarations, but 'fileprivateVersioned()' is fileprivate}}

@usableFromInline internal func internalVersioned() {}
// OK

@usableFromInline func implicitInternalVersioned() {}
// OK

@usableFromInline public func publicVersioned() {}
// expected-error@-1 {{'@usableFromInline' attribute can only be applied to internal declarations, but 'publicVersioned()' is public}}

internal class InternalClass {
  // expected-note@-1 2{{type declared here}}
  @usableFromInline public func publicVersioned() {}
  // expected-error@-1 {{'@usableFromInline' attribute can only be applied to internal declarations, but 'publicVersioned()' is public}}
}

fileprivate class filePrivateClass {
  @usableFromInline internal func internalVersioned() {}
}

@usableFromInline struct S {
  var x: Int
  @usableFromInline var y: Int
}

@usableFromInline extension S {}
// expected-error@-1 {{'@usableFromInline' attribute cannot be applied to this declaration}}

@usableFromInline
protocol VersionedProtocol {
  associatedtype T

  func requirement() -> T

  public func publicRequirement() -> T
  // expected-error@-1 {{'public' modifier cannot be used in protocols}}
  // expected-note@-2 {{protocol requirements implicitly have the same access as the protocol itself}}

  @usableFromInline func versionedRequirement() -> T
  // expected-error@-1 {{'@usableFromInline' attribute cannot be used in protocols}}
}

// Derived conformances had issues with @usableFromInline - rdar://problem/34342955
@usableFromInline
internal enum EqEnum {
  case foo
}

@usableFromInline
internal enum RawEnum : Int {
  case foo = 0
}

@inlinable
public func usesEqEnum() -> Bool {
  _ = (EqEnum.foo == .foo)
  _ = EqEnum.foo.hashValue

  _ = RawEnum.foo.rawValue
  _ = RawEnum(rawValue: 0)
}

public class DynamicMembers {
  @usableFromInline @objc dynamic init() {}
  @usableFromInline @objc dynamic func foo() {}
  @usableFromInline @objc dynamic var bar: Int = 0
}

internal struct InternalStruct {}
// expected-note@-1 9{{type declared here}}

@usableFromInline var globalInferred = InternalStruct()
// expected-error@-1 {{type referenced from a '@usableFromInline' variable with inferred type 'InternalStruct' must be '@usableFromInline' or public}}

@usableFromInline var globalDeclared: InternalStruct = InternalStruct()
// expected-error@-1 {{type referenced from a '@usableFromInline' variable must be '@usableFromInline' or public}}

@usableFromInline typealias BadAlias = InternalStruct
// expected-error@-1 {{type referenced from the underlying type of a '@usableFromInline' type alias must be '@usableFromInline' or public}}

protocol InternalProtocol {
  // expected-note@-1 * {{type declared here}}
  associatedtype T
}

@usableFromInline
struct BadStruct<T, U>
// expected-error@-1 {{type referenced from a generic requirement of a '@usableFromInline' generic struct must be '@usableFromInline' or public}}
where T : InternalProtocol,
      T : Sequence,
      T.Element == InternalStruct {
  @usableFromInline init(x: InternalStruct) {}
  // expected-error@-1 {{the parameter of a '@usableFromInline' initializer must be '@usableFromInline' or public}}

  @usableFromInline func foo(x: InternalStruct) -> InternalClass {}
  // expected-error@-1 {{the parameter of a '@usableFromInline' method must be '@usableFromInline' or public}}
  // expected-error@-2 {{the result of a '@usableFromInline' method must be '@usableFromInline' or public}}

  @usableFromInline var propertyInferred = InternalStruct()
  // expected-error@-1 {{type referenced from a '@usableFromInline' property with inferred type 'InternalStruct' must be '@usableFromInline' or public}}

  @usableFromInline var propertyDeclared: InternalStruct = InternalStruct()
  // expected-error@-1 {{type referenced from a '@usableFromInline' property must be '@usableFromInline' or public}}

  @usableFromInline subscript(x: InternalStruct) -> Int {
    // expected-error@-1 {{index type of a '@usableFromInline' subscript must be '@usableFromInline' or public}}
    get {}
    set {}
  }

  @usableFromInline subscript(x: Int) -> InternalStruct {
    // expected-error@-1 {{element type of a '@usableFromInline' subscript must be '@usableFromInline' or public}}
    get {}
    set {}
  }
}

@usableFromInline
protocol BadProtocol : InternalProtocol {
  // expected-error@-1 {{protocol refined by '@usableFromInline' protocol must be '@usableForInline' or public}}
  associatedtype X : InternalProtocol
  // expected-error@-1 {{type referenced from a requirement of an associated type in a '@usableFromInline' protocol must be '@usableFromInline' or public}}
  associatedtype Y = InternalStruct
  // expected-error@-1 {{type referenced from a default definition of an associated type in a '@usableFromInline' protocol must be '@usableFromInline' or public}}
}

@usableFromInline
protocol AnotherBadProtocol where Self.T : InternalProtocol {
  // expected-error@-1 {{protocol used by '@usableFromInline' protocol must be '@usableForInline' or public}}
  associatedtype T
}

@usableFromInline
enum BadEnum {
  case bad(InternalStruct)
  // expected-error@-1 {{type of enum case in '@usableFromInline' enum must be '@usableFromInline' or public}}
}

@usableFromInline
class BadClass : InternalClass {}
// expected-error@-1 {{type referenced from the superclass of a '@usableFromInline' class must be '@usableFromInline' or public}}

public struct TestGenericSubscripts {
  @usableFromInline subscript<T: InternalProtocol>(_: T) -> Int { return 0 } // expected-warning {{type referenced from a generic parameter of a '@usableFromInline' subscript should be '@usableFromInline' or public}}
  @usableFromInline subscript<T>(where _: T) -> Int where T: InternalProtocol { return 0 } // expected-warning {{type referenced from a generic requirement of a '@usableFromInline' subscript should be '@usableFromInline' or public}}
}

@usableFromInline typealias TestGenericAlias<T: InternalProtocol> = T // expected-warning {{type referenced from a generic parameter of a '@usableFromInline' type alias should be '@usableFromInline' or public}}
@usableFromInline typealias TestGenericAliasWhereClause<T> = T where T: InternalProtocol // expected-warning {{type referenced from a generic requirement of a '@usableFromInline' type alias should be '@usableFromInline' or public}}

@usableFromInline struct GenericStruct<T> {
  @usableFromInline struct Nested where T : InternalProtocol {}
  // expected-error@-1 {{type referenced from a generic requirement of a '@usableFromInline' struct must be '@usableFromInline' or public}}

  @usableFromInline func nonGenericWhereClause() where T : InternalProtocol {}
  // expected-error@-1 {{type referenced from a generic requirement of a '@usableFromInline' instance method must be '@usableFromInline' or public}}
}