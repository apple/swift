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

package import BADLibrary // expected-note 8 {{protocol 'BadProto' imported as 'package' from 'BADLibrary' here}}
// expected-note @-1 {{struct 'IntLike' imported as 'package' from 'BADLibrary' here}}
// expected-note @-2 {{class 'BadClass' imported as 'package' from 'BADLibrary' here}}

public protocol LocalProto {}

public struct TestConformance: BadProto {} // expected-error {{cannot use protocol 'BadProto' in a public or '@usableFromInline' conformance; 'BADLibrary' was not imported publicly}}
public struct TestConformanceComposition: LocalProto & BadProto {} // expected-error {{cannot use protocol 'BadProto' in a public or '@usableFromInline' conformance; 'BADLibrary' was not imported publicly}}

@usableFromInline struct TestConformanceUFI: BadProto {} // expected-error {{cannot use protocol 'BadProto' in a public or '@usableFromInline' conformance; 'BADLibrary' was not imported publicly}}

public class TestConformanceClass: BadProto {} // expected-error {{cannot use protocol 'BadProto' in a public or '@usableFromInline' conformance; 'BADLibrary' was not imported publicly}}
public enum TestConformanceEnum: BadProto {} // expected-error {{cannot use protocol 'BadProto' in a public or '@usableFromInline' conformance; 'BADLibrary' was not imported publicly}}

public struct TestExtensionStruct {}
extension TestExtensionStruct: BadProto {} // expected-error {{cannot use protocol 'BadProto' in a public or '@usableFromInline' conformance; 'BADLibrary' was not imported publicly}}

package struct TestConformancePackage: BadProto {}
package struct TestConformanceCompositionPackage: LocalProto & BadProto {}

@usableFromInline struct TestConformanceUFIPackage: BadProto {} // expected-error {{cannot use protocol 'BadProto' in a public or '@usableFromInline' conformance; 'BADLibrary' was not imported publicly}}

package class TestConformanceClassPackage: BadProto {}
package enum TestConformanceEnumPackage: BADLibrary.BadProto {}

package struct TestExtensionStructPackage {}
extension TestExtensionStructPackage: BadProto {} // FIXME-error {{cannot use protocol 'BadProto' in a public or '@usableFromInline' conformance; 'BADLibrary' was not imported publicly}}

/// Other inheritance types are covered by the classic access-level check.

public class TestSubclass: BadClass { // expected-error {{class cannot be declared public because its superclass is package}}
// expected-note @-1 {{class 'BadClass' is imported by this file as 'package' from 'BADLibrary'}}
}

public enum TestRawType: IntLike { // expected-error {{enum cannot be declared public because its raw type uses a package type}}
// expected-note @-1 {{struct 'IntLike' is imported by this file as 'package' from 'BADLibrary'}}
  case x = 1
}

public protocol TestRefinedProto: BadProto { // expected-error {{public protocol cannot refine a package protocol}}
// expected-note @-1 {{protocol 'BadProto' is imported by this file as 'package' from 'BADLibrary'}}
}

package class TestSubclassPackage: BadClass {
}

package enum TestRawTypePackage: IntLike {
  case x = 1
}

package protocol TestRefinedProtoPackage: BadProto {
}
