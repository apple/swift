// RUN: %empty-directory(%t)
// RUN: %target-swift-frontend -emit-module -o %t/NormalLibrary.swiftmodule %S/Inputs/implementation-only-import-in-decls-public-helper.swift \
// RUN:  -enable-library-evolution -swift-version 5
// RUN: %target-swift-frontend -emit-module -o %t/BADLibrary.swiftmodule %S/Inputs/implementation-only-import-in-decls-helper.swift -I %t \
// RUN:  -enable-library-evolution -swift-version 5

// RUN: %target-typecheck-verify-swift -I %t \
// RUN:  -enable-library-evolution -swift-version 5

import NormalLibrary
@_implementationOnly import BADLibrary

public struct TestConformance: BadProto {} // expected-error {{cannot use protocol 'BadProto' here; 'BADLibrary' has been imported as implementation-only}}

@usableFromInline struct TestConformanceUFI: BadProto {} // expected-error {{cannot use protocol 'BadProto' here; 'BADLibrary' has been imported as implementation-only}}

struct TestConformanceOkay: BadProto {} // ok

public class TestConformanceClass: BadProto {} // expected-error {{cannot use protocol 'BadProto' here; 'BADLibrary' has been imported as implementation-only}}
public enum TestConformanceEnum: BadProto {} // expected-error {{cannot use protocol 'BadProto' here; 'BADLibrary' has been imported as implementation-only}}


public struct TestGenericParams<T: BadProto> {} // expected-error {{cannot use protocol 'BadProto' here; 'BADLibrary' has been imported as implementation-only}}

public struct TestGenericParamsWhereClause<T> where T: BadProto {} // expected-error {{cannot use protocol 'BadProto' here; 'BADLibrary' has been imported as implementation-only}}

public struct TestGenericParamsWithOuter<T> {
  public func nonGenericWhereClause() where T : BadProto {} // expected-error {{cannot use protocol 'BadProto' here; 'BADLibrary' has been imported as implementation-only}}
  public struct Inner where T : BadProto {} // expected-error {{cannot use protocol 'BadProto' here; 'BADLibrary' has been imported as implementation-only}}
}

public enum TestCase {
  case x(BadStruct) // expected-error {{cannot use struct 'BadStruct' here; 'BADLibrary' has been imported as implementation-only}}
  case y(Int, BadStruct) // expected-error {{cannot use struct 'BadStruct' here; 'BADLibrary' has been imported as implementation-only}}
}

public func testGenericParams<T: BadProto>(_: T) {} // expected-error {{cannot use protocol 'BadProto' here; 'BADLibrary' has been imported as implementation-only}}
public func testGenericParamsWhereClause<T>(_: T) where T: BadProto {} // expected-error {{cannot use protocol 'BadProto' here; 'BADLibrary' has been imported as implementation-only}}
public func testParams(_: BadStruct, _: BadProto) {}
// expected-error@-1 {{cannot use struct 'BadStruct' here; 'BADLibrary' has been imported as implementation-only}}
// expected-error@-2 {{cannot use protocol 'BadProto' here; 'BADLibrary' has been imported as implementation-only}}
public func testResult() -> BadStruct? { return nil } // expected-error {{cannot use struct 'BadStruct' here; 'BADLibrary' has been imported as implementation-only}}

public struct TestSubscript {
  public subscript<T: BadProto>(_: T) -> Int { return 0 } // expected-error {{cannot use protocol 'BadProto' here; 'BADLibrary' has been imported as implementation-only}}
  public subscript<T>(where _: T) -> Int where T: BadProto { return 0 } // expected-error {{cannot use protocol 'BadProto' here; 'BADLibrary' has been imported as implementation-only}}
  public subscript(_: BadStruct) -> Int { return 0 } // expected-error {{cannot use struct 'BadStruct' here; 'BADLibrary' has been imported as implementation-only}}
  public subscript(_: Int) -> BadStruct? { return nil } // expected-error {{cannot use struct 'BadStruct' here; 'BADLibrary' has been imported as implementation-only}}
}

public struct TestInit {
  public init(_: BadStruct) {} // expected-error {{cannot use struct 'BadStruct' here; 'BADLibrary' has been imported as implementation-only}}
  public init<T: BadProto>(_: T) {} // expected-error {{cannot use protocol 'BadProto' here; 'BADLibrary' has been imported as implementation-only}}
  public init<T>(where _: T) where T: BadProto {} // expected-error {{cannot use protocol 'BadProto' here; 'BADLibrary' has been imported as implementation-only}}
}

public struct TestPropertyWrapper {
  @BadWrapper public var BadProperty: Int // expected-error {{cannot use struct 'BadWrapper' as property wrapper here; 'BADLibrary' has been imported as implementation-only}}
}

public protocol TestInherited: BadProto {} // expected-error {{cannot use protocol 'BadProto' here; 'BADLibrary' has been imported as implementation-only}}

public protocol TestConstraintBase {
  associatedtype Assoc
}
public protocol TestConstraint: TestConstraintBase where Assoc: BadProto {} // expected-error {{cannot use protocol 'BadProto' here; 'BADLibrary' has been imported as implementation-only}}
public protocol TestConstraintEq: TestConstraintBase where Assoc == BadStruct {} // expected-error {{cannot use struct 'BadStruct' here; 'BADLibrary' has been imported as implementation-only}}

public protocol TestAssocType {
  associatedtype Assoc: BadProto // expected-error {{cannot use protocol 'BadProto' here; 'BADLibrary' has been imported as implementation-only}}
}

public protocol TestAssocTypeWhereClause {
  associatedtype Assoc: Collection where Assoc.Element: BadProto // expected-error {{cannot use protocol 'BadProto' here; 'BADLibrary' has been imported as implementation-only}}
}

public enum TestRawType: IntLike { // expected-error {{cannot use struct 'IntLike' here; 'BADLibrary' has been imported as implementation-only}}
  case x = 1
}

public class TestSubclass: BadClass { // expected-error {{cannot use class 'BadClass' here; 'BADLibrary' has been imported as implementation-only}}
}

public typealias TestUnderlying = BadStruct // expected-error {{cannot use struct 'BadStruct' here; 'BADLibrary' has been imported as implementation-only}}
public typealias TestGenericParamsAliasWhereClause<T> = T where T: BadProto // expected-error {{cannot use protocol 'BadProto' here; 'BADLibrary' has been imported as implementation-only}}

public typealias TestGenericParamsAlias<T: BadProto> = T // expected-error {{cannot use protocol 'BadProto' here; 'BADLibrary' has been imported as implementation-only}}

public var testBadType: BadStruct? = nil // expected-error {{cannot use struct 'BadStruct' here; 'BADLibrary' has been imported as implementation-only}}
public var testBadTypeInferred = Optional<BadStruct>.none // expected-error {{cannot use struct 'BadStruct' here; 'BADLibrary' has been imported as implementation-only}}
public var testBadTypePartiallyInferred: Optional = Optional<BadStruct>.none // expected-error {{cannot use struct 'BadStruct' here; 'BADLibrary' has been imported as implementation-only}}
public var (testBadTypeTuple1, testBadTypeTuple2): (BadStruct?, BadClass?) = (nil, nil)
// expected-error@-1 {{cannot use struct 'BadStruct' here; 'BADLibrary' has been imported as implementation-only}}
// expected-error@-2 {{cannot use class 'BadClass' here; 'BADLibrary' has been imported as implementation-only}}
public var (testBadTypeTuplePartlyInferred1, testBadTypeTuplePartlyInferred2): (Optional, Optional) = (Optional<Int>.none, Optional<BadStruct>.none) // expected-error {{cannot use struct 'BadStruct' here; 'BADLibrary' has been imported as implementation-only}}
public var (testBadTypeTuplePartlyInferred3, testBadTypeTuplePartlyInferred4): (Optional, Optional) = (Optional<BadStruct>.none, Optional<Int>.none) // expected-error {{cannot use struct 'BadStruct' here; 'BADLibrary' has been imported as implementation-only}}
public var testMultipleBindings1: Int? = nil, testMultipleBindings2: BadStruct? = nil // expected-error {{cannot use struct 'BadStruct' here; 'BADLibrary' has been imported as implementation-only}}
public var testMultipleBindings3: BadStruct? = nil, testMultipleBindings4: Int? = nil // expected-error {{cannot use struct 'BadStruct' here; 'BADLibrary' has been imported as implementation-only}}

extension BadStruct { // expected-error {{cannot use struct 'BadStruct' in an extension with public or '@usableFromInline' members; 'BADLibrary' has been imported as implementation-only}}
  public func testExtensionOfBadType() {}
  public var testExtensionVarBad: Int { 0 }
  public subscript(bad _: Int) -> Int { 0 }
}
extension BadStruct {
  func testExtensionOfOkayType() {}
  var testExtensionVarOkay: Int { 0 }
  subscript(okay _: Int) -> Int { 0 }
}

extension Array where Element == BadStruct { // expected-error {{cannot use struct 'BadStruct' in an extension with public or '@usableFromInline' members; 'BADLibrary' has been imported as implementation-only}}
  public func testExtensionWithBadRequirement() {}
  public var testExtensionVarBad: Int { 0 }
  public subscript(bad _: Int) -> Int { 0 }
}

extension Array where Element == BadStruct {
  func testExtensionWithOkayRequirement() {} // okay
  var testExtensionVarOkay: Int { 0 } // okay
  subscript(okay _: Int) -> Int { 0 } // okay
}

extension Int: BadProto {} // expected-error {{cannot use protocol 'BadProto' here; 'BADLibrary' has been imported as implementation-only}}
struct TestExtensionConformanceOkay {}
extension TestExtensionConformanceOkay: BadProto {} // okay

public protocol TestConstrainedExtensionProto {}
extension Array: TestConstrainedExtensionProto where Element == BadStruct { // expected-error {{cannot use struct 'BadStruct' in an extension with conditional conformances; 'BADLibrary' has been imported as implementation-only}}
}


infix operator !!!!!!: BadPrecedence // expected-error {{cannot use precedence group 'BadPrecedence' here; 'BADLibrary' has been imported as implementation-only}}

precedencegroup TestLowerThan {
  lowerThan: BadPrecedence // expected-error {{cannot use precedence group 'BadPrecedence' here; 'BADLibrary' has been imported as implementation-only}}
}
precedencegroup TestHigherThan {
  higherThan: BadPrecedence // expected-error {{cannot use precedence group 'BadPrecedence' here; 'BADLibrary' has been imported as implementation-only}}
}

@frozen public struct PublicStructStoredProperties {
  public var publiclyBad: BadStruct? // expected-error {{cannot use struct 'BadStruct' here; 'BADLibrary' has been imported as implementation-only}}
  internal var internallyBad: BadStruct? // expected-error {{cannot use struct 'BadStruct' here; 'BADLibrary' has been imported as implementation-only}}
  private var privatelyBad: BadStruct? // expected-error {{cannot use struct 'BadStruct' here; 'BADLibrary' has been imported as implementation-only}}
  private let letIsLikeVar = [BadStruct]() // expected-error {{cannot use struct 'BadStruct' here; 'BADLibrary' has been imported as implementation-only}}
  // expected-error@-1 {{struct 'BadStruct' cannot be used in a property initializer in a '@frozen' type because 'BADLibrary' was imported implementation-only}}

  private var computedIsOkay: BadStruct? { return nil } // okay
  private static var staticIsOkay: BadStruct? // okay
  @usableFromInline internal var computedUFIIsNot: BadStruct? { return nil } // expected-error {{cannot use struct 'BadStruct' here; 'BADLibrary' has been imported as implementation-only}}
}

@frozen @usableFromInline internal struct UFIStructStoredProperties {
  @usableFromInline var publiclyBad: BadStruct? // expected-error {{cannot use struct 'BadStruct' here; 'BADLibrary' has been imported as implementation-only}}
  internal var internallyBad: BadStruct? // expected-error {{cannot use struct 'BadStruct' here; 'BADLibrary' has been imported as implementation-only}}
  private var privatelyBad: BadStruct? // expected-error {{cannot use struct 'BadStruct' here; 'BADLibrary' has been imported as implementation-only}}
  private let letIsLikeVar = [BadStruct]() // expected-error {{cannot use struct 'BadStruct' here; 'BADLibrary' has been imported as implementation-only}}
  // expected-error@-1 {{struct 'BadStruct' cannot be used in a property initializer in a '@frozen' type because 'BADLibrary' was imported implementation-only}}

  private var computedIsOkay: BadStruct? { return nil } // okay
  private static var staticIsOkay: BadStruct? // okay
  @usableFromInline internal var computedUFIIsNot: BadStruct? { return nil } // expected-error {{cannot use struct 'BadStruct' here; 'BADLibrary' has been imported as implementation-only}}
}

@_fixed_layout public class PublicClassStoredProperties {
  public var publiclyBad: BadStruct? // expected-error {{cannot use struct 'BadStruct' here; 'BADLibrary' has been imported as implementation-only}}
  internal var internallyBad: BadStruct? // expected-error {{cannot use struct 'BadStruct' here; 'BADLibrary' has been imported as implementation-only}}
  private var privatelyBad: BadStruct? // expected-error {{cannot use struct 'BadStruct' here; 'BADLibrary' has been imported as implementation-only}}
  private let letIsLikeVar = [BadStruct]() // expected-error {{cannot use struct 'BadStruct' here; 'BADLibrary' has been imported as implementation-only}}
  // expected-error@-1 {{struct 'BadStruct' cannot be used in a property initializer in a '@frozen' type because 'BADLibrary' was imported implementation-only}}

  private var computedIsOkay: BadStruct? { return nil } // okay
  private static var staticIsOkay: BadStruct? // okay
  @usableFromInline internal var computedUFIIsNot: BadStruct? { return nil } // expected-error {{cannot use struct 'BadStruct' here; 'BADLibrary' has been imported as implementation-only}}
}

public typealias NormalProtoAssoc<T: NormalProto> = T.Assoc
public func testConformanceInTypealias(_: NormalProtoAssoc<NormalStruct>) {} // expected-error {{cannot use conformance of 'NormalStruct' to 'NormalProto' here; 'BADLibrary' has been imported as implementation-only}}

public struct NormalProtoAssocHolder<T: NormalProto> {
  public var value: T.Assoc
}
public func testConformanceInBoundGeneric(_: NormalProtoAssocHolder<NormalStruct>) {} // expected-error {{cannot use conformance of 'NormalStruct' to 'NormalProto' here; 'BADLibrary' has been imported as implementation-only}}

public struct OuterGenericHolder<T> {
  public struct Nested where T : NormalProto {
    public var value: T.Assoc
  }
}
public func testConformanceInNestedNonGeneric(_: OuterGenericHolder<NormalStruct>.Nested) {} // expected-error {{cannot use conformance of 'NormalStruct' to 'NormalProto' here; 'BADLibrary' has been imported as implementation-only}}

public class SubclassOfNormalClass: NormalClass {}

public func testInheritedConformance(_: NormalProtoAssocHolder<SubclassOfNormalClass>) {} // expected-error {{cannot use conformance of 'NormalClass' to 'NormalProto' here; 'BADLibrary' has been imported as implementation-only}}
public func testSpecializedConformance(_: NormalProtoAssocHolder<GenericStruct<Int>>) {} // expected-error {{cannot use conformance of 'GenericStruct<T>' to 'NormalProto' here; 'BADLibrary' has been imported as implementation-only}}

extension Array where Element == NormalProtoAssocHolder<NormalStruct> { // expected-error {{cannot use conformance of 'NormalStruct' to 'NormalProto' in an extension with public or '@usableFromInline' members; 'BADLibrary' has been imported as implementation-only}}
  public func testConstrainedExtensionUsingBadConformance() {}
}

public struct ConditionalGenericStruct<T> {}
extension ConditionalGenericStruct: NormalProto where T: NormalProto {
  public typealias Assoc = Int
}
public func testConditionalGeneric(_: NormalProtoAssocHolder<ConditionalGenericStruct<NormalStruct>>) {} // expected-error {{cannot use conformance of 'NormalStruct' to 'NormalProto' here; 'BADLibrary' has been imported as implementation-only}}

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
extension PublicInferredAssociatedTypeImpl: PublicAssociatedTypeProto {} // expected-error {{cannot use conformance of 'NormalStruct' to 'NormalProto' here; 'BADLibrary' has been imported as implementation-only}}
// expected-note@-1 {{in associated type 'Self.Assoc' (inferred as 'NormalStruct')}}

extension PublicInferredAssociatedTypeImpl: UFIAssociatedTypeProto {} // expected-error {{cannot use conformance of 'NormalStruct' to 'NormalProto' here; 'BADLibrary' has been imported as implementation-only}}
// expected-note@-1 {{in associated type 'Self.Assoc' (inferred as 'NormalStruct')}}

extension PublicInferredAssociatedTypeImpl: InternalAssociatedTypeProto {} // okay

@usableFromInline struct UFIInferredAssociatedTypeImpl {
  public func takesAssoc(_: NormalStruct) {}
}
extension UFIInferredAssociatedTypeImpl: PublicAssociatedTypeProto {} // expected-error {{cannot use conformance of 'NormalStruct' to 'NormalProto' here; 'BADLibrary' has been imported as implementation-only}}
// expected-note@-1 {{in associated type 'Self.Assoc' (inferred as 'NormalStruct')}}

extension UFIInferredAssociatedTypeImpl: UFIAssociatedTypeProto {} // expected-error {{cannot use conformance of 'NormalStruct' to 'NormalProto' here; 'BADLibrary' has been imported as implementation-only}}
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
extension PublicExplicitAssociatedTypeImpl: PublicAssociatedTypeProto {} // expected-error {{cannot use conformance of 'NormalStruct' to 'NormalProto' here; 'BADLibrary' has been imported as implementation-only}}
// expected-note@-1 {{in associated type 'Self.Assoc' (inferred as 'NormalStruct')}}

extension PublicExplicitAssociatedTypeImpl: UFIAssociatedTypeProto {} // expected-error {{cannot use conformance of 'NormalStruct' to 'NormalProto' here; 'BADLibrary' has been imported as implementation-only}}
// expected-note@-1 {{in associated type 'Self.Assoc' (inferred as 'NormalStruct')}}

extension PublicExplicitAssociatedTypeImpl: InternalAssociatedTypeProto {} // okay


public protocol BaseProtoWithNoRequirement {
  associatedtype Assoc
  func takesAssoc(_: Assoc)
}
public protocol RefinedProto: BaseProtoWithNoRequirement where Assoc: NormalProto {
}

public struct RefinedProtoImpl: RefinedProto { // expected-error {{cannot use conformance of 'NormalStruct' to 'NormalProto' here; 'BADLibrary' has been imported as implementation-only}}
// expected-note@-1 {{in associated type 'Self.Assoc' (inferred as 'NormalStruct')}}
  public func takesAssoc(_: NormalStruct) {}
}

public protocol RefinedSelfProto where Self: NormalProto {}
extension NormalStruct: RefinedSelfProto {} // expected-error {{cannot use conformance of 'NormalStruct' to 'NormalProto' here; 'BADLibrary' has been imported as implementation-only}}

public protocol RefinedSelfProtoInheritance: NormalProto {}
extension NormalStruct: RefinedSelfProtoInheritance {} // expected-error {{cannot use conformance of 'NormalStruct' to 'NormalProto' here; 'BADLibrary' has been imported as implementation-only}}


public protocol SlightlyMoreComplicatedRequirement {
  associatedtype Assoc: Collection where Assoc.Element: NormalProto
  func takesAssoc(_: Assoc)
}
public struct SlightlyMoreComplicatedRequirementImpl: SlightlyMoreComplicatedRequirement { // expected-error {{cannot use conformance of 'NormalStruct' to 'NormalProto' here; 'BADLibrary' has been imported as implementation-only}}
// expected-note@-1 {{in associated type 'Self.Assoc.Element' (inferred as 'NormalStruct')}}

  public func takesAssoc(_: [NormalStruct]) {}
}
public struct RequirementsHandleSubclassesToo: SlightlyMoreComplicatedRequirement { // expected-error {{cannot use conformance of 'NormalClass' to 'NormalProto' here; 'BADLibrary' has been imported as implementation-only}}
// expected-note@-1 {{in associated type 'Self.Assoc.Element' (inferred as 'SubclassOfNormalClass')}}

  public func takesAssoc(_: [SubclassOfNormalClass]) {}
}

public struct RequirementsHandleSpecializationsToo: SlightlyMoreComplicatedRequirement { // expected-error {{cannot use conformance of 'NormalStruct' to 'NormalProto' here; 'BADLibrary' has been imported as implementation-only}}
// expected-note@-1 {{in associated type 'Self.Assoc.Element' (inferred as 'ConditionalGenericStruct<NormalStruct>')}}

  public func takesAssoc(_: [ConditionalGenericStruct<NormalStruct>]) {}
}

public struct ClassConstrainedGenericArg<T: NormalClass>: PublicAssociatedTypeProto { // expected-error {{cannot use conformance of 'NormalClass' to 'NormalProto' here; 'BADLibrary' has been imported as implementation-only}}
// expected-note@-1 {{in associated type 'Self.Assoc' (inferred as 'T')}}

  public func takesAssoc(_: T) {}
}


public protocol RecursiveRequirements {
  associatedtype Other: RecursiveRequirements
}
extension GenericStruct: RecursiveRequirements {
  public typealias Other = GenericStruct<T>
}
public struct RecursiveRequirementsHolder<T: RecursiveRequirements> {}
public func makeSureRecursiveRequirementsDontBreakEverything(_: RecursiveRequirementsHolder<GenericStruct<Int>>) {}
