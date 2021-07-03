// RUN: %target-typecheck-verify-swift -swift-version 4 -disable-objc-attr-requires-foundation-module -enable-objc-interop -enable-access-control-hacks
// RUN: %target-typecheck-verify-swift -enable-testing -swift-version 4 -disable-objc-attr-requires-foundation-module -enable-objc-interop -enable-access-control-hacks

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

internal struct InternalStruct {}

@usableFromInline var globalInferred = InternalStruct()

@usableFromInline var globalDeclared: InternalStruct = InternalStruct()

@usableFromInline typealias BadAlias = InternalStruct

protocol InternalProtocol {
  associatedtype T
}

@usableFromInline
struct BadStruct<T, U>
where T : InternalProtocol,
      T : Sequence,
      T.Element == InternalStruct {
  @usableFromInline init(x: InternalStruct) {}

  @usableFromInline func foo(x: InternalStruct) -> InternalClass {}

  @usableFromInline var propertyInferred = InternalStruct()

  @usableFromInline var propertyDeclared: InternalStruct = InternalStruct()

  @usableFromInline subscript(x: InternalStruct) -> Int {
    get {}
    set {}
  }

  @usableFromInline subscript(x: Int) -> InternalStruct {
    get {}
    set {}
  }
}

@usableFromInline
protocol BadProtocol : InternalProtocol {
  associatedtype X : InternalProtocol
  associatedtype Y = InternalStruct
}

@usableFromInline
protocol AnotherBadProtocol where Self.T : InternalProtocol {
  associatedtype T
}

@usableFromInline
enum BadEnum {
  case bad(InternalStruct)
}

@usableFromInline
class BadClass : InternalClass {}

public class DynamicMembers {
  @usableFromInline @objc dynamic init() {}
  @usableFromInline @objc dynamic func foo() {}
  @usableFromInline @objc dynamic var bar: Int = 0
}
