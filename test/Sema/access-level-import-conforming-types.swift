// RUN: %empty-directory(%t)

// RUN: %target-swift-frontend -emit-module -o %t/NormalLibrary.swiftmodule \
// RUN:  %S/Inputs/implementation-only-import-in-decls-public-helper.swift \
// RUN:  -enable-library-evolution -swift-version 5

// RUN: %target-swift-frontend -emit-module -o %t/BADLibrary.swiftmodule \
// RUN:  %S/Inputs/implementation-only-import-in-decls-helper.swift -I %t \
// RUN:  -enable-library-evolution -swift-version 5

// RUN: %target-typecheck-verify-swift -I %t \
// RUN:  -swift-version 5 -package-name pkg -enable-library-evolution
// RUN: %target-typecheck-verify-swift -I %t \
// RUN:  -swift-version 5 -package-name pkg

internal import BADLibrary // expected-note 9 {{protocol 'BadProto' imported as 'internal' from 'BADLibrary' here}}
// expected-note @-1 2 {{struct 'IntLike' imported as 'internal' from 'BADLibrary' here}}
// expected-note @-2 2 {{class 'BadClass' imported as 'internal' from 'BADLibrary' here}}

public protocol LocalProto {}

public struct TestConformance: BadProto {} // expected-error {{cannot use protocol 'BadProto' in a public or '@usableFromInline' conformance; 'BADLibrary' was not imported publicly}}
public struct TestConformanceComposition: LocalProto & BadProto {} // expected-error {{cannot use protocol 'BadProto' in a public or '@usableFromInline' conformance; 'BADLibrary' was not imported publicly}}

@usableFromInline struct TestConformanceUFI: BadProto {} // expected-error {{cannot use protocol 'BadProto' in a public or '@usableFromInline' conformance; 'BADLibrary' was not imported publicly}}

public class TestConformanceClass: BadProto {} // expected-error {{cannot use protocol 'BadProto' in a public or '@usableFromInline' conformance; 'BADLibrary' was not imported publicly}}
public enum TestConformanceEnum: BadProto {} // expected-error {{cannot use protocol 'BadProto' in a public or '@usableFromInline' conformance; 'BADLibrary' was not imported publicly}}

public struct TestExtensionStruct {}
extension TestExtensionStruct: BadProto {} // expected-error {{cannot use protocol 'BadProto' in a public or '@usableFromInline' conformance; 'BADLibrary' was not imported publicly}}

package struct TestConformancePackage: BadProto {} // FIXME-error {{cannot use protocol 'BadProto' in a public or '@usableFromInline' conformance; 'BADLibrary' was not imported publicly}}
package struct TestConformanceCompositionPackage: LocalProto & BadProto {} // FIXME-error {{cannot use protocol 'BadProto' in a public or '@usableFromInline' conformance; 'BADLibrary' was not imported publicly}}

@usableFromInline struct TestConformanceUFIPackage: BadProto {} // expected-error {{cannot use protocol 'BadProto' in a public or '@usableFromInline' conformance; 'BADLibrary' was not imported publicly}}

package class TestConformanceClassPackage: BadProto {} // FIXME-error {{cannot use protocol 'BadProto' in a public or '@usableFromInline' conformance; 'BADLibrary' was not imported publicly}}
package enum TestConformanceEnumPackage: BADLibrary.BadProto {} // FIXME-error {{cannot use protocol 'BadProto' in a public or '@usableFromInline' conformance; 'BADLibrary' was not imported publicly}}

package struct TestExtensionStructPackage {}
extension TestExtensionStructPackage: BadProto {} // FIXME-error {{cannot use protocol 'BadProto' in a public or '@usableFromInline' conformance; 'BADLibrary' was not imported publicly}}

/// Other inheritance types are covered by the classic access-level check.

public class TestSubclass: BadClass { // expected-error {{class cannot be declared public because its superclass is internal}}
// expected-note @-1 {{class 'BadClass' is imported by this file as 'internal' from 'BADLibrary'}}
}

public enum TestRawType: IntLike { // expected-error {{enum cannot be declared public because its raw type uses an internal type}}
// expected-note @-1 {{struct 'IntLike' is imported by this file as 'internal' from 'BADLibrary'}}
  case x = 1
}

public protocol TestRefinedProto: BadProto { // expected-error {{public protocol cannot refine an internal protocol}}
// expected-note @-1 {{protocol 'BadProto' is imported by this file as 'internal' from 'BADLibrary'}}
}

package class TestSubclassPackage: BadClass { // expected-error {{class cannot be declared package because its superclass is internal}}
// expected-note @-1 {{class 'BadClass' is imported by this file as 'internal' from 'BADLibrary'}}
}

package enum TestRawTypePackage: IntLike { // expected-error {{enum cannot be declared package because its raw type uses an internal type}}
// expected-note @-1 {{struct 'IntLike' is imported by this file as 'internal' from 'BADLibrary'}}
  case x = 1
}

package protocol TestRefinedProtoPackage: BadProto { // expected-error {{package protocol cannot refine an internal protocol}}
// expected-note @-1 {{protocol 'BadProto' is imported by this file as 'internal' from 'BADLibrary'}}
}
