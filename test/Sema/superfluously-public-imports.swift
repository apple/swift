// RUN: %empty-directory(%t)
// RUN: split-file --leading-lines %s %t

/// Build the libraries.
// RUN: %target-swift-frontend -emit-module %t/DepUsedFromInlinableCode.swift -o %t
// RUN: %target-swift-frontend -emit-module %t/DepUsedInSignature.swift -o %t
// RUN: %target-swift-frontend -emit-module %t/Exportee.swift -o %t
// RUN: %target-swift-frontend -emit-module %t/Exporter.swift -o %t -I %t
// RUN: %target-swift-frontend -emit-module %t/ConformanceBaseTypes.swift -o %t
// RUN: %target-swift-frontend -emit-module %t/ConformanceDefinition.swift -o %t -I %t
// RUN: %target-swift-frontend -emit-module %t/AliasesBase.swift -o %t
// RUN: %target-swift-frontend -emit-module %t/Aliases.swift -o %t -I %t
// RUN: %target-swift-frontend -emit-module %t/ExtensionA.swift -o %t -I %t
// RUN: %target-swift-frontend -emit-module %t/ExtensionB.swift -o %t -I %t
// RUN: %target-swift-frontend -emit-module %t/PropertyWrapper.swift -o %t -I %t
// RUN: %target-swift-frontend -emit-module %t/ExtendedDefinitionPublic.swift -o %t -I %t
// RUN: %target-swift-frontend -emit-module %t/ExtendedDefinitionNonPublic.swift -o %t -I %t
// RUN: %target-swift-frontend -emit-module %t/UnusedImport.swift -o %t -I %t
// RUN: %target-swift-frontend -emit-module %t/UnusedPackageImport.swift -o %t -I %t
// RUN: %target-swift-frontend -emit-module %t/ExtendedPackageTypeImport.swift -o %t -I %t
// RUN: %target-swift-frontend -emit-module %t/ImportNotUseFromAPI.swift -o %t -I %t
// RUN: %target-swift-frontend -emit-module %t/ImportUsedInPackage.swift -o %t -I %t
// RUN: %target-swift-frontend -emit-module %t/ExportedUnused.swift -o %t -I %t
// RUN: %target-swift-frontend -emit-module %t/SPIOnlyUsedInSPI.swift -o %t -I %t
// RUN: %target-swift-frontend -emit-module %t/RetroactiveConformance.swift -o %t -I %t

/// Check diagnostics.
// RUN: %target-swift-frontend -typecheck %t/Client.swift -I %t \
// RUN:   -package-name pkg -Rmodule-api-import \
// RUN:   -enable-upcoming-feature InternalImportsByDefault -verify \
// RUN:   -experimental-spi-only-imports
// RUN: %target-swift-frontend -typecheck %t/ClientOfClangModules.swift -I %t \
// RUN:   -package-name pkg -Rmodule-api-import \
// RUN:   -enable-upcoming-feature InternalImportsByDefault -verify
// RUN: %target-swift-frontend -typecheck %t/Client_Swift5.swift -I %t \
// RUN:   -swift-version 5 -verify

//--- DepUsedFromInlinableCode.swift
public struct TypeUsedFromInlinableCode {}
public func funcUsedFromInlinableCode() {}

public func funcUsedFromDefaultValue() -> Int { 42 }

//--- DepUsedInSignature.swift
public struct TypeUsedInSignature {}
public protocol ComposedProtoA {}
public protocol ComposedProtoB {}

//--- Exportee.swift
public struct ExportedType {}

//--- Exporter.swift
@_exported import Exportee

//--- ConformanceBaseTypes.swift
public protocol Proto {}
public struct ConformingType {
    public init () {}
}

//--- ConformanceDefinition.swift
import ConformanceBaseTypes
extension ConformingType : Proto  {}

//--- AliasesBase.swift
open class Clazz {}

//--- Aliases.swift
import AliasesBase
public typealias ClazzAlias = Clazz

//--- ExtensionA.swift
import ConformanceBaseTypes
extension ConformingType {
    public func extFuncA() {}
}

//--- ExtensionB.swift
import ConformanceBaseTypes
extension ConformingType {
    public func extFuncB() {}
}

//--- PropertyWrapper.swift
@propertyWrapper
public struct MyPropertyWrapper<T> {
  public var wrappedValue: T

  public init(wrappedValue value: T) { self.wrappedValue = value }
  public init(_ value: T) { self.wrappedValue = value }
}

//--- ExtendedDefinitionPublic.swift
public struct PublicExtendedType {}

//--- ExtendedDefinitionNonPublic.swift
public struct NonPublicExtendedType {}

//--- UnusedImport.swift

//--- UnusedPackageImport.swift
//--- ExtendedPackageTypeImport.swift

public struct ExtendedPackageType {}

//--- ImportNotUseFromAPI.swift
public struct NotAnAPIType {}
public func notAnAPIFunc() -> NotAnAPIType { return NotAnAPIType() }

//--- ImportUsedInPackage.swift
public struct PackageType {}
public func packageFunc() -> PackageType { return PackageType() }

//--- ExportedUnused.swift

//--- SPIOnlyUsedInSPI.swift
public struct ToUseFromSPI {}

//--- RetroactiveConformance.swift
public struct Extended {
  public var count: Int { 42 }
}

//--- Client_Swift5.swift
/// No diagnostics should be raised on the implicit access level.
import UnusedImport
public import UnusedImport // expected-warning {{public import of 'UnusedImport' was not used in public declarations or inlinable code}} {{1-7=internal}}

//--- Client.swift
public import DepUsedFromInlinableCode
public import DepUsedInSignature
public import Exporter
public import ConformanceBaseTypes
public import ConformanceDefinition
public import AliasesBase
public import Aliases
public import ExtensionA
public import ExtensionB
public import PropertyWrapper
public import ExtendedDefinitionPublic
public import ExtendedDefinitionNonPublic // expected-warning {{public import of 'ExtendedDefinitionNonPublic' was not used in public declarations or inlinable code}} {{1-8=}}

/// Repeat some imports to make sure we report all of them.
public import UnusedImport // expected-warning {{public import of 'UnusedImport' was not used in public declarations or inlinable code}} {{1-8=}}
// expected-note @-1 {{imported 'public' here}}
public import UnusedImport // expected-warning {{public import of 'UnusedImport' was not used in public declarations or inlinable code}} {{1-8=}}
package import UnusedImport // expected-warning {{package import of 'UnusedImport' was not used in package declarations}} {{1-9=}}
// expected-warning @-1 {{module 'UnusedImport' is imported as 'public' from the same file; this 'package' access level will be ignored}}

package import UnusedPackageImport // expected-warning {{package import of 'UnusedPackageImport' was not used in package declarations}} {{1-9=}}
package import ExtendedPackageTypeImport
public import ImportNotUseFromAPI // expected-warning {{public import of 'ImportNotUseFromAPI' was not used in public declarations or inlinable code}} {{1-8=}}
public import ImportUsedInPackage // expected-warning {{public import of 'ImportUsedInPackage' was not used in public declarations or inlinable code}} {{1-7=package}}

@_exported public import ExportedUnused
@_spiOnly public import SPIOnlyUsedInSPI
public import RetroactiveConformance

public func useInSignature(_ a: TypeUsedInSignature) {} // expected-remark {{struct 'TypeUsedInSignature' is imported via 'DepUsedInSignature'}}
public func exportedTypeUseInSignature(_ a: ExportedType) {} // expected-remark {{struct 'ExportedType' is imported via 'Exporter', which reexports definition from 'Exportee'}}

public func useInDefaultValue(_ a: Int = funcUsedFromDefaultValue()) {}
// expected-remark @-1 {{struct 'Int' is imported via 'Swift'}}
// expected-remark @-2 {{global function 'funcUsedFromDefaultValue()' is imported via 'DepUsedFromInlinableCode'}}

public func genericType(_ a: Array<TypeUsedInSignature>) {}
// expected-remark @-1 {{generic struct 'Array' is imported via 'Swift'}}
// expected-remark @-2 {{struct 'TypeUsedInSignature' is imported via 'DepUsedInSignature'}}

public func protocolComposition(_ a: any ComposedProtoA & ComposedProtoB) {}
// expected-remark @-1 {{protocol 'ComposedProtoA' is imported via 'DepUsedInSignature'}}
// expected-remark @-2 {{protocol 'ComposedProtoB' is imported via 'DepUsedInSignature'}}

public func useConformance(_ a: any Proto = ConformingType()) {}
// expected-remark @-1 {{protocol 'Proto' is imported via 'ConformanceBaseTypes'}}
// expected-remark @-2 {{conformance of 'ConformingType' to protocol 'Proto' used here is imported via 'ConformanceDefinition'}}
// expected-remark @-3 {{struct 'ConformingType' is imported via 'ConformanceBaseTypes'}}
// expected-remark @-4 {{initializer 'init()' is imported via 'ConformanceBaseTypes'}}

@usableFromInline internal func usableFromInlineFunc(_ a: TypeUsedInSignature) {}

@inlinable
public func publicFuncUsesPrivate() {
  let _: TypeUsedFromInlinableCode // expected-remark {{struct 'TypeUsedFromInlinableCode' is imported via 'DepUsedFromInlinableCode'}}
  let _: ExportedType // expected-remark {{struct 'ExportedType' is imported via 'Exporter', which reexports definition from 'Exportee'}}
  funcUsedFromInlinableCode() // expected-remark {{global function 'funcUsedFromInlinableCode()' is imported via 'DepUsedFromInlinableCode'}}

  let _: Array<TypeUsedInSignature>
  // expected-remark @-1 {{generic struct 'Array' is imported via 'Swift'}}
  // expected-remark @-2 {{struct 'TypeUsedInSignature' is imported via 'DepUsedInSignature'}}

  let _: any ComposedProtoA & ComposedProtoB
  // expected-remark @-1 {{protocol 'ComposedProtoA' is imported via 'DepUsedInSignature'}}
  // expected-remark @-2 {{protocol 'ComposedProtoB' is imported via 'DepUsedInSignature'}}

  let _: any Proto = ConformingType()
  // expected-remark @-1 {{protocol 'Proto' is imported via 'ConformanceBaseTypes'}}
  // expected-remark @-2 {{conformance of 'ConformingType' to protocol 'Proto' used here is imported via 'ConformanceDefinition'}}
  // expected-remark @-3 {{struct 'ConformingType' is imported via 'ConformanceBaseTypes'}}
  // expected-remark @-4 {{initializer 'init()' is imported via 'ConformanceBaseTypes'}}

  let _: ClazzAlias
  // expected-remark @-1 {{type alias 'ClazzAlias' is imported via 'Aliases'}}
  // expected-remark @-2 2 {{typealias underlying type class 'Clazz' is imported via 'AliasesBase'}}

  let x = ConformingType()
  // expected-remark @-1 {{struct 'ConformingType' is imported via 'ConformanceBaseTypes'}}
  // expected-remark @-2 {{initializer 'init()' is imported via 'ConformanceBaseTypes'}}
  x.extFuncA() // expected-remark {{instance method 'extFuncA()' is imported via 'ExtensionA'}}
  x.extFuncB() // expected-remark {{instance method 'extFuncB()' is imported via 'ExtensionB'}}
}

public struct StructUsingPropertyWrapper {
  @MyPropertyWrapper(42) public var wrapped: Any // expected-remark 2 {{generic struct 'MyPropertyWrapper' is imported via 'PropertyWrapper'}}
}

extension PublicExtendedType { // expected-remark 2 {{struct 'PublicExtendedType' is imported via 'ExtendedDefinitionPublic'}}
    public func foo() {}
}

extension NonPublicExtendedType {
    func foo() {}
}

public struct Struct { // expected-remark {{implicitly used struct 'Int' is imported via 'Swift'}}
  public var propWithInferredIntType = 42
  public var propWithExplicitType: String = "Text" // expected-remark {{struct 'String' is imported via 'Swift'}}
}

public func publicFunction() {
    let _: NotAnAPIType = notAnAPIFunc()
}

internal func internalFunc(a: NotAnAPIType = notAnAPIFunc()) {}
func implicitlyInternalFunc(a: NotAnAPIType = notAnAPIFunc()) {}

// For package decls we only remark on types used in signatures, not for inlinable code.
package func packageFunc(a: PackageType = packageFunc()) {} // expected-remark {{struct 'PackageType' is imported via 'ImportUsedInPackage'}}

@_spi(X)
public func spiFunc(a: ToUseFromSPI) {} // expected-remark {{struct 'ToUseFromSPI' is imported via 'SPIOnlyUsedInSPI'}}

public protocol Countable {
  var count: Int { get } // expected-remark {{struct 'Int' is imported via 'Swift'}}
}

extension Extended: Countable { // expected-remark {{struct 'Extended' is imported via 'RetroactiveConformance'}}
}

extension ExtendedPackageType { // expected-remark {{struct 'ExtendedPackageType' is imported via 'ExtendedPackageTypeImport'}}
  package func useExtendedPackageType() { }
}

/// Tests for imports of clang modules.
//--- module.modulemap
module ClangSimpleUnused {
    header "ClangSimpleUnused.h"
}
module ClangSimple {
    header "ClangSimple.h"
}

module ClangSubmodule {
    header "ClangSubmodule.h"

    module ClangSubmoduleSubmodule {
      header "ClangSubmoduleSubmodule.h"
    }
}

module ClangSubmoduleUnused {
    header "ClangSubmoduleUnused.h"

    module ClangSubmoduleUnsuedSubmodule {
      header "ClangSubmoduleUnusedSubmodule.h"
    }
}

module ClangTopModule {
  header "ClangTopModule.h"
  module ClangTopModuleSubmodule {
    header "ClangTopModuleSubmodule.h"
  }
}

//--- ClangSimpleUnused.h
//--- ClangSimple.h
struct ClangSimpleType {};

//--- ClangSubmodule.h
//--- ClangSubmoduleSubmodule.h
struct ClangSubmoduleSubmoduleType {};

//--- ClangSubmoduleUnused.h
//--- ClangSubmoduleUnusedSubmodule.h

//--- ClangTopModule.h
struct ClangTopModuleType {};
//--- ClangTopModuleSubmodule.h

//--- ClientOfClangModules.swift
public import ClangSimple
public import ClangSimpleUnused // expected-warning {{public import of 'ClangSimpleUnused' was not used in public declarations or inlinable code}}
public import ClangSubmodule.ClangSubmoduleSubmodule
public import ClangSubmoduleUnused.ClangSubmoduleUnsuedSubmodule // expected-warning {{public import of 'ClangSubmoduleUnused' was not used in public declarations or inlinable code}}

// Only the top-level module is used, but we can't detect whether the submodule was used or not.
public import ClangTopModule.ClangTopModuleSubmodule

public func clangUser(a: ClangSimpleType) {} // expected-remark {{struct 'ClangSimpleType' is imported via 'ClangSimple'}}
public func clangUser(a: ClangSubmoduleSubmoduleType) {} // expected-remark {{struct 'ClangSubmoduleSubmoduleType' is imported via 'ClangSubmodule'}}
public func clangUser(a: ClangTopModuleType) {} // expected-remark {{struct 'ClangTopModuleType' is imported via 'ClangTopModule'}}
