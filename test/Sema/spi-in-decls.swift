/// SPI variant of implementation-only-import-in-decls with the "Bad"
/// declarations defined as local SPI.

// RUN: %empty-directory(%t)
// RUN: %target-swift-frontend -emit-module -o %t/NormalLibrary.swiftmodule %S/Inputs/implementation-only-import-in-decls-public-helper.swift

// RUN: %target-typecheck-verify-swift -I %t

import NormalLibrary

@_spi(X)
extension NormalStruct: @retroactive NormalProto {
  public typealias Assoc = Int
}
@_spi(X)
extension GenericStruct: @retroactive NormalProto {
  public typealias Assoc = Int
}
@_spi(X)
extension NormalClass: @retroactive NormalProto {
  public typealias Assoc = Int
}

@_spi(X)
public struct BadStruct {} // expected-note 34 {{type declared here}}
@_spi(X)
public protocol BadProto {} // expected-note 20 {{type declared here}}
@_spi(X)
open class BadClass {} // expected-note 2 {{type declared here}}

@_spi(X)
public struct IntLike: ExpressibleByIntegerLiteral, Equatable { // expected-note {{type declared here}}
  public init(integerLiteral: Int) {}
}

@_spi(X)
@propertyWrapper
public struct BadWrapper { // expected-note {{type declared here}}
    public var wrappedValue: Int
    public init(wrappedValue: Int) {
        self.wrappedValue = wrappedValue
    }
}

// FIXME SPI precedencegroups are not yet accepted.
//@_spi(X)
//precedencegroup BadPrecedence {}

public struct TestConformance: BadProto {} // expected-error {{cannot use protocol 'BadProto' in a public or '@usableFromInline' conformance; it is SPI}}

@usableFromInline struct TestConformanceUFI: BadProto {} // expected-error {{cannot use protocol 'BadProto' in a public or '@usableFromInline' conformance; it is SPI}}

struct TestConformanceOkay: BadProto {} // ok

public class TestConformanceClass: BadProto {} // expected-error {{cannot use protocol 'BadProto' in a public or '@usableFromInline' conformance; it is SPI}}
public enum TestConformanceEnum: BadProto {} // expected-error {{cannot use protocol 'BadProto' in a public or '@usableFromInline' conformance; it is SPI}}


public struct TestGenericParams<T: BadProto> {} // expected-error {{cannot use protocol 'BadProto' here; it is SPI}}

public struct TestGenericParamsWhereClause<T> where T: BadProto {} // expected-error {{cannot use protocol 'BadProto' here; it is SPI}}

public enum TestCase {
  case x(BadStruct) // expected-error {{cannot use struct 'BadStruct' here; it is SPI}}
  case y(Int, BadStruct) // expected-error {{cannot use struct 'BadStruct' here; it is SPI}}
}

public func testGenericParams<T: BadProto>(_: T) {} // expected-error {{cannot use protocol 'BadProto' here; it is SPI}}
public func testGenericParamsWhereClause<T>(_: T) where T: BadProto {} // expected-error {{cannot use protocol 'BadProto' here; it is SPI}}
public func testParams(_: BadStruct, _: BadProto) {}
// expected-error@-1 {{cannot use struct 'BadStruct' here; it is SPI}}
// expected-error@-2 {{cannot use protocol 'BadProto' here; it is SPI}}
public func testResult() -> BadStruct? { return nil } // expected-error {{cannot use struct 'BadStruct' here; it is SPI}}

public struct TestSubscript {
  public subscript<T: BadProto>(_: T) -> Int { return 0 } // expected-error {{cannot use protocol 'BadProto' here; it is SPI}}
  public subscript<T>(where _: T) -> Int where T: BadProto { return 0 } // expected-error {{cannot use protocol 'BadProto' here; it is SPI}}
  public subscript(_: BadStruct) -> Int { return 0 } // expected-error {{cannot use struct 'BadStruct' here; it is SPI}}
  public subscript(_: Int) -> BadStruct? { return nil } // expected-error {{cannot use struct 'BadStruct' here; it is SPI}}
}

public struct TestInit {
  public init(_: BadStruct) {} // expected-error {{cannot use struct 'BadStruct' here; it is SPI}}
  public init<T: BadProto>(_: T) {} // expected-error {{cannot use protocol 'BadProto' here; it is SPI}}
  public init<T>(where _: T) where T: BadProto {} // expected-error {{cannot use protocol 'BadProto' here; it is SPI}}
}

public struct TestPropertyWrapper {
  @BadWrapper public var BadProperty: Int // expected-error {{cannot use struct 'BadWrapper' as property wrapper here; it is SPI}}
}

public protocol TestInherited: BadProto {} // expected-error {{cannot use protocol 'BadProto' here; it is SPI}}

public protocol TestConstraintBase {
  associatedtype Assoc
}
public protocol TestConstraint: TestConstraintBase where Assoc: BadProto {} // expected-error {{cannot use protocol 'BadProto' here; it is SPI}}
public protocol TestConstraintEq: TestConstraintBase where Assoc == BadStruct {} // expected-error {{cannot use struct 'BadStruct' here; it is SPI}}

public protocol TestAssocType {
  associatedtype Assoc: BadProto // expected-error {{cannot use protocol 'BadProto' here; it is SPI}}
}

public protocol TestAssocTypeWhereClause {
  associatedtype Assoc: Collection where Assoc.Element: BadProto // expected-error {{cannot use protocol 'BadProto' here; it is SPI}}
}

public enum TestRawType: IntLike { // expected-error {{cannot use struct 'IntLike' in a public or '@usableFromInline' conformance; it is SPI}}
  case x = 1
}

public class TestSubclass: BadClass { // expected-error {{cannot use class 'BadClass' in a public or '@usableFromInline' conformance; it is SPI}}
}

public typealias TestUnderlying = BadStruct // expected-error {{cannot use struct 'BadStruct' here; it is SPI}}
public typealias TestGenericParamsAliasWhereClause<T> = T where T: BadProto // expected-error {{cannot use protocol 'BadProto' here; it is SPI}}

public typealias TestGenericParamsAlias<T: BadProto> = T // expected-error {{cannot use protocol 'BadProto' here; it is SPI}}

public var testBadType: BadStruct? = nil // expected-error {{cannot use struct 'BadStruct' here; it is SPI}}
public var testBadTypeInferred = Optional<BadStruct>.none // expected-error {{cannot use struct 'BadStruct' here; it is SPI}}
public var testBadTypePartiallyInferred: Optional = Optional<BadStruct>.none // expected-error {{cannot use struct 'BadStruct' here; it is SPI}}
public var (testBadTypeTuple1, testBadTypeTuple2): (BadStruct?, BadClass?) = (nil, nil)
// expected-error@-1 {{cannot use struct 'BadStruct' here; it is SPI}}
// expected-error@-2 {{cannot use class 'BadClass' here; it is SPI}}
public var (testBadTypeTuplePartlyInferred1, testBadTypeTuplePartlyInferred2): (Optional, Optional) = (Optional<Int>.none, Optional<BadStruct>.none) // expected-error {{cannot use struct 'BadStruct' here; it is SPI}}
public var (testBadTypeTuplePartlyInferred3, testBadTypeTuplePartlyInferred4): (Optional, Optional) = (Optional<BadStruct>.none, Optional<Int>.none) // expected-error {{cannot use struct 'BadStruct' here; it is SPI}}
public var testMultipleBindings1: Int? = nil, testMultipleBindings2: BadStruct? = nil // expected-error {{cannot use struct 'BadStruct' here; it is SPI}}
public var testMultipleBindings3: BadStruct? = nil, testMultipleBindings4: Int? = nil // expected-error {{cannot use struct 'BadStruct' here; it is SPI}}

extension BadStruct {
  public func testExtensionOfBadType() {}
  public var testExtensionVarBad: Int { 0 }
  public subscript(bad _: Int) -> Int { 0 }

  func testExtensionOfOkayType() {}
  var testExtensionVarOkay: Int { 0 }
  subscript(okay _: Int) -> Int { 0 }
}

extension Array where Element == BadStruct { // expected-error {{cannot use struct 'BadStruct' in an extension with public or '@usableFromInline' members; it is SPI}}
  public func testExtensionWithBadRequirement() {}
  public var testExtensionVarBad: Int { 0 }
  public subscript(bad _: Int) -> Int { 0 }
}

extension Array where Element == BadStruct {
  func testExtensionWithOkayRequirement() {} // okay
  var testExtensionVarOkay: Int { 0 } // okay
  subscript(okay _: Int) -> Int { 0 } // okay
}

extension Int: BadProto {} // expected-error {{cannot use protocol 'BadProto' in a public or '@usableFromInline' conformance; it is SPI}}
struct TestExtensionConformanceOkay {}
extension TestExtensionConformanceOkay: BadProto {} // okay

public protocol TestConstrainedExtensionProto {}
extension Array: TestConstrainedExtensionProto where Element == BadStruct { // expected-error {{cannot use struct 'BadStruct' in an extension with conditional conformances; it is SPI}}
}


// FIXME Support SPI precedencegroup?
//infix operator !!!!!!: BadPrecedence
//
//precedencegroup TestLowerThan {
//  lowerThan: BadPrecedence
//}
//precedencegroup TestHigherThan {
//  higherThan: BadPrecedence
//}

@frozen public struct PublicStructStoredProperties {
  public var publiclyBad: BadStruct? // expected-error {{cannot use struct 'BadStruct' here; it is SPI}}
  internal var internallyBad: BadStruct? // expected-error {{cannot use struct 'BadStruct' here; it is SPI}}
  private var privatelyBad: BadStruct? // expected-error {{cannot use struct 'BadStruct' here; it is SPI}}
  private let letIsLikeVar = [BadStruct]() // expected-error {{cannot use struct 'BadStruct' here; it is SPI}}
  // expected-error@-1 {{struct 'BadStruct' cannot be used in a property initializer in a '@frozen' type because it is SPI}}

  private var computedIsOkay: BadStruct? { return nil } // okay
  private static var staticIsOkay: BadStruct? // okay
  @usableFromInline internal var computedUFIIsNot: BadStruct? { return nil } // expected-error {{cannot use struct 'BadStruct' here; it is SPI}}
}

@frozen @usableFromInline internal struct UFIStructStoredProperties {
  @usableFromInline var publiclyBad: BadStruct? // expected-error {{cannot use struct 'BadStruct' here; it is SPI}}
  internal var internallyBad: BadStruct? // expected-error {{cannot use struct 'BadStruct' here; it is SPI}}
  private var privatelyBad: BadStruct? // expected-error {{cannot use struct 'BadStruct' here; it is SPI}}
  private let letIsLikeVar = [BadStruct]() // expected-error {{cannot use struct 'BadStruct' here; it is SPI}}
  // expected-error@-1 {{struct 'BadStruct' cannot be used in a property initializer in a '@frozen' type because it is SPI}}

  private var computedIsOkay: BadStruct? { return nil } // okay
  private static var staticIsOkay: BadStruct? // okay
  @usableFromInline internal var computedUFIIsNot: BadStruct? { return nil } // expected-error {{cannot use struct 'BadStruct' here; it is SPI}}
}

@_fixed_layout public class PublicClassStoredProperties {
  public var publiclyBad: BadStruct? // expected-error {{cannot use struct 'BadStruct' here; it is SPI}}
  internal var internallyBad: BadStruct? // expected-error {{cannot use struct 'BadStruct' here; it is SPI}}
  private var privatelyBad: BadStruct? // expected-error {{cannot use struct 'BadStruct' here; it is SPI}}
  private let letIsLikeVar = [BadStruct]() // expected-error {{cannot use struct 'BadStruct' here; it is SPI}}
  // expected-error@-1 {{struct 'BadStruct' cannot be used in a property initializer in a '@frozen' type because it is SPI}}

  private var computedIsOkay: BadStruct? { return nil } // okay
  private static var staticIsOkay: BadStruct? // okay
  @usableFromInline internal var computedUFIIsNot: BadStruct? { return nil } // expected-error {{cannot use struct 'BadStruct' here; it is SPI}}
}

public typealias NormalProtoAssoc<T: NormalProto> = T.Assoc
public func testConformanceInTypealias(_: NormalProtoAssoc<NormalStruct>) {} // expected-error {{cannot use conformance of 'NormalStruct' to 'NormalProto' here; the conformance is declared as SPI}}

public struct NormalProtoAssocHolder<T: NormalProto> {
  public var value: T.Assoc
}
public func testConformanceInBoundGeneric(_: NormalProtoAssocHolder<NormalStruct>) {} // expected-error {{cannot use conformance of 'NormalStruct' to 'NormalProto' here; the conformance is declared as SPI}}

public struct OuterGenericHolder<T> {
  public struct Nested where T : NormalProto {
    public var value: T.Assoc
  }
}
public func testConformanceInNestedNonGeneric(_: OuterGenericHolder<NormalStruct>.Nested) {} // expected-error {{cannot use conformance of 'NormalStruct' to 'NormalProto' here; the conformance is declared as SPI}}

public class SubclassOfNormalClass: NormalClass {}

public func testInheritedConformance(_: NormalProtoAssocHolder<SubclassOfNormalClass>) {} // expected-error {{cannot use conformance of 'NormalClass' to 'NormalProto' here; the conformance is declared as SPI}}
public func testSpecializedConformance(_: NormalProtoAssocHolder<GenericStruct<Int>>) {} // expected-error {{cannot use conformance of 'GenericStruct<T>' to 'NormalProto' here; the conformance is declared as SPI}}

extension Array where Element == NormalProtoAssocHolder<NormalStruct> { // expected-error {{cannot use conformance of 'NormalStruct' to 'NormalProto' in an extension with public or '@usableFromInline' members; the conformance is declared as SPI}}
  public func testConstrainedExtensionUsingBadConformance() {}
}

public struct ConditionalGenericStruct<T> {}
extension ConditionalGenericStruct: NormalProto where T: NormalProto {
  public typealias Assoc = Int
}
public func testConditionalGeneric(_: NormalProtoAssocHolder<ConditionalGenericStruct<NormalStruct>>) {} // expected-error {{cannot use conformance of 'NormalStruct' to 'NormalProto' here; the conformance is declared as SPI}}

public protocol PublicAssociatedTypeProto {
  associatedtype Assoc: NormalProto
  func takesAssoc(_: Assoc)
}
@usableFromInline protocol UFIAssociatedTypeProto {
  associatedtype Assoc: NormalProto
  func takesAssoc(_: Assoc)
}
protocol InternalAssociatedTypeProto {
  associatedtype Assoc: NormalProto
  func takesAssoc(_: Assoc)
}

public struct PublicInferredAssociatedTypeImpl {
  public func takesAssoc(_: NormalStruct) {}
}
extension PublicInferredAssociatedTypeImpl: PublicAssociatedTypeProto {} // expected-error {{cannot use conformance of 'NormalStruct' to 'NormalProto' here; the conformance is declared as SPI}}
// expected-note@-1 {{in associated type 'Self.Assoc' (inferred as 'NormalStruct')}}

extension PublicInferredAssociatedTypeImpl: UFIAssociatedTypeProto {} // expected-error {{cannot use conformance of 'NormalStruct' to 'NormalProto' here; the conformance is declared as SPI}}
// expected-note@-1 {{in associated type 'Self.Assoc' (inferred as 'NormalStruct')}}

extension PublicInferredAssociatedTypeImpl: InternalAssociatedTypeProto {} // okay

@usableFromInline struct UFIInferredAssociatedTypeImpl {
  public func takesAssoc(_: NormalStruct) {}
}

extension UFIInferredAssociatedTypeImpl: PublicAssociatedTypeProto {} // expected-error {{cannot use conformance of 'NormalStruct' to 'NormalProto' here; the conformance is declared as SPI}}
// expected-note@-1 {{in associated type 'Self.Assoc' (inferred as 'NormalStruct')}}

extension UFIInferredAssociatedTypeImpl: UFIAssociatedTypeProto {} // expected-error {{cannot use conformance of 'NormalStruct' to 'NormalProto' here; the conformance is declared as SPI}}
// expected-note@-1 {{in associated type 'Self.Assoc' (inferred as 'NormalStruct')}}

extension UFIInferredAssociatedTypeImpl: InternalAssociatedTypeProto {} // okay

struct InternalInferredAssociatedTypeImpl {
  public func takesAssoc(_: NormalStruct) {}
}
extension InternalInferredAssociatedTypeImpl: PublicAssociatedTypeProto {} // okay
extension InternalInferredAssociatedTypeImpl: UFIAssociatedTypeProto {} // okay
extension InternalInferredAssociatedTypeImpl: InternalAssociatedTypeProto {} // okay

public struct PublicExplicitAssociatedTypeImpl {
  public typealias Assoc = NormalStruct
  public func takesAssoc(_: NormalStruct) {}
}
extension PublicExplicitAssociatedTypeImpl: PublicAssociatedTypeProto {} // expected-error {{cannot use conformance of 'NormalStruct' to 'NormalProto' here; the conformance is declared as SPI}}
// expected-note@-1 {{in associated type 'Self.Assoc' (inferred as 'NormalStruct')}}

extension PublicExplicitAssociatedTypeImpl: UFIAssociatedTypeProto {} // expected-error {{cannot use conformance of 'NormalStruct' to 'NormalProto' here; the conformance is declared as SPI}}
// expected-note@-1 {{in associated type 'Self.Assoc'}}

extension PublicExplicitAssociatedTypeImpl: InternalAssociatedTypeProto {} // okay


public protocol BaseProtoWithNoRequirement {
  associatedtype Assoc
  func takesAssoc(_: Assoc)
}
public protocol RefinedProto: BaseProtoWithNoRequirement where Assoc: NormalProto {
}

public struct RefinedProtoImpl: RefinedProto { // expected-error {{cannot use conformance of 'NormalStruct' to 'NormalProto' here; the conformance is declared as SPI}}
// expected-note@-1 {{in associated type 'Self.Assoc' (inferred as 'NormalStruct')}}
  public func takesAssoc(_: NormalStruct) {}
}

public protocol RefinedSelfProto where Self: NormalProto {}
extension NormalStruct: RefinedSelfProto {} // expected-error {{cannot use conformance of 'NormalStruct' to 'NormalProto' here; the conformance is declared as SPI}}

public protocol RefinedSelfProtoInheritance: NormalProto {}
extension NormalStruct: RefinedSelfProtoInheritance {} // expected-error {{cannot use conformance of 'NormalStruct' to 'NormalProto' here; the conformance is declared as SPI}}


public protocol SlightlyMoreComplicatedRequirement {
  associatedtype Assoc: Collection where Assoc.Element: NormalProto
  func takesAssoc(_: Assoc)
}
public struct SlightlyMoreComplicatedRequirementImpl: SlightlyMoreComplicatedRequirement { // expected-error {{cannot use conformance of 'NormalStruct' to 'NormalProto' here; the conformance is declared as SPI}}
// expected-note@-1 {{in associated type 'Self.Assoc.Element' (inferred as 'NormalStruct')}}
  public func takesAssoc(_: [NormalStruct]) {}
}
public struct RequirementsHandleSubclassesToo: SlightlyMoreComplicatedRequirement { // expected-error {{cannot use conformance of 'NormalClass' to 'NormalProto' here; the conformance is declared as SPI}}
// expected-note@-1 {{in associated type 'Self.Assoc.Element' (inferred as 'SubclassOfNormalClass')}}
  public func takesAssoc(_: [SubclassOfNormalClass]) {}
}

public struct RequirementsHandleSpecializationsToo: SlightlyMoreComplicatedRequirement { // expected-error {{cannot use conformance of 'NormalStruct' to 'NormalProto' here; the conformance is declared as SPI}}
// expected-note@-1 {{in associated type 'Self.Assoc.Element' (inferred as 'ConditionalGenericStruct<NormalStruct>')}}

  public func takesAssoc(_: [ConditionalGenericStruct<NormalStruct>]) {}
}

public struct ClassConstrainedGenericArg<T: NormalClass>: PublicAssociatedTypeProto { // expected-error {{cannot use conformance of 'NormalClass' to 'NormalProto' here; the conformance is declared as SPI}}
// expected-note@-1 {{in associated type 'Self.Assoc' (inferred as 'T')}}

  public func takesAssoc(_: T) {}
}
