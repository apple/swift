// RUN: %empty-directory(%t)
// RUN: split-file --leading-lines %s %t

/// Build the libraries.
// RUN: %target-swift-frontend -emit-module %t/PublicLib.swift -o %t \
// RUN:   -enable-library-evolution
// RUN: %target-swift-frontend -emit-module %t/PackageLib.swift -o %t \
// RUN:   -enable-library-evolution
// RUN: %target-swift-frontend -emit-module %t/InternalLib.swift -o %t \
// RUN:   -enable-library-evolution
// RUN: %target-swift-frontend -emit-module %t/FileprivateLib.swift -o %t \
// RUN:   -enable-library-evolution
// RUN: %target-swift-frontend -emit-module %t/PrivateLib.swift -o %t \
// RUN:   -enable-library-evolution

/// Check diagnostics.
// RUN: %target-swift-frontend -typecheck %t/Client.swift -I %t \
// RUN:   -package-name TestPackage -swift-version 5 \
// RUN:   -enable-experimental-feature AccessLevelOnImport -verify

/// Check diagnostics with library-evolution.
// RUN: %target-swift-frontend -typecheck %t/Client.swift -I %t \
// RUN:   -package-name TestPackage -swift-version 5 \
// RUN:   -enable-library-evolution \
// RUN:   -enable-experimental-feature AccessLevelOnImport -verify

//--- PublicLib.swift
public struct PublicImportType {
    public init() {}
}

//--- PackageLib.swift
public struct PackageImportType {
    public init() {}
}

//--- InternalLib.swift
public struct InternalImportType {
    public init() {}
}

//--- FileprivateLib.swift
public struct FileprivateImportType {
    public init() {}
}

//--- PrivateLib.swift
public struct PrivateImportType {
    public init() {}
}

//--- Client.swift
public import PublicLib
package import PackageLib // expected-note 2 {{struct 'PackageImportType' imported as 'package' from 'PackageLib' here}}
internal import InternalLib // expected-note 2 {{struct 'InternalImportType' imported as 'internal' from 'InternalLib' here}}
fileprivate import FileprivateLib // expected-note 2 {{struct 'FileprivateImportType' imported as 'fileprivate' from 'FileprivateLib' here}}
private import PrivateLib // expected-note 2 {{struct 'PrivateImportType' imported as 'private' from 'PrivateLib' here}}

public protocol PublicConstrainedExtensionProto {}
extension Array: PublicConstrainedExtensionProto where Element == PublicImportType {}
extension PublicImportType {
    public func publicMethod() {}
}

package protocol PublicConstrainedExtensionProtoInPackage {}
extension Array: PublicConstrainedExtensionProtoInPackage where Element == PackageImportType {}
extension PublicImportType {
    package func packageMethod() {}
}

internal protocol PublicConstrainedExtensionProtoInInternal {}
extension Array: PublicConstrainedExtensionProtoInInternal where Element == PublicImportType {}
extension PublicImportType {
    internal func internalMethod() {}
}

fileprivate protocol PublicConstrainedExtensionProtoInFileprivate {}
extension Array: PublicConstrainedExtensionProtoInFileprivate where Element == PublicImportType {}
extension PublicImportType {
    fileprivate func fileprivateMethod() {}
}

private protocol PublicConstrainedExtensionProtoInPrivate {}
extension Array: PublicConstrainedExtensionProtoInPrivate where Element == PublicImportType {}
extension PublicImportType {
    private func privateMethod() {}
}

public protocol PackageConstrainedExtensionProto {}
extension Array: PackageConstrainedExtensionProto where Element == PackageImportType {} // expected-error {{cannot use struct 'PackageImportType' in an extension with conditional conformances; 'PackageLib' was not imported publicly}}
extension PackageImportType { // expected-error {{cannot use struct 'PackageImportType' in an extension with public or '@usableFromInline' members; 'PackageLib' was not imported publicly}}
    public func publicMethod() {}
}

package protocol PackageConstrainedExtensionProtoInPackage {}
extension Array: PackageConstrainedExtensionProtoInPackage where Element == PackageImportType {}
extension PackageImportType {
    package func packageMethod() {}
}

internal protocol PackageConstrainedExtensionProtoInInternal {}
extension Array: PackageConstrainedExtensionProtoInInternal where Element == PackageImportType {}
extension PackageImportType {
    internal func internalMethod() {}
}

fileprivate protocol PackageConstrainedExtensionProtoInFileprivate {}
extension Array: PackageConstrainedExtensionProtoInFileprivate where Element == PackageImportType {}
extension PackageImportType {
    fileprivate func fileprivateMethod() {}
}

private protocol PackageConstrainedExtensionProtoInPrivate {}
extension Array: PackageConstrainedExtensionProtoInPrivate where Element == PackageImportType {}
extension PackageImportType {
    private func privateMethod() {}
}

public protocol InternalConstrainedExtensionProto {}
extension Array: InternalConstrainedExtensionProto where Element == InternalImportType {} // expected-error {{cannot use struct 'InternalImportType' in an extension with conditional conformances; 'InternalLib' was not imported publicly}}
extension InternalImportType { // expected-error {{cannot use struct 'InternalImportType' in an extension with public or '@usableFromInline' members; 'InternalLib' was not imported publicly}}
    public func foo() {}
}

package protocol InternalConstrainedExtensionProtoInPackage {}
extension Array: InternalConstrainedExtensionProtoInPackage where Element == InternalImportType {}
extension InternalImportType {
    package func packageMethod() {}
}

internal protocol InternalConstrainedExtensionProtoInInternal {}
extension Array: InternalConstrainedExtensionProtoInInternal where Element == InternalImportType {}
extension InternalImportType {
    internal func internalMethod() {}
}

fileprivate protocol InternalConstrainedExtensionProtoInFileprivate {}
extension Array: InternalConstrainedExtensionProtoInFileprivate where Element == InternalImportType {}
extension InternalImportType {
    fileprivate func fileprivateMethod() {}
}

private protocol InternalConstrainedExtensionProtoInPrivate {}
extension Array: InternalConstrainedExtensionProtoInPrivate where Element == InternalImportType {}
extension InternalImportType {
    private func privateMethod() {}
}

public protocol FileprivateConstrainedExtensionProto {}
extension Array: FileprivateConstrainedExtensionProto where Element == FileprivateImportType {} // expected-error {{cannot use struct 'FileprivateImportType' in an extension with conditional conformances; 'FileprivateLib' was not imported publicly}}
extension FileprivateImportType { // expected-error {{cannot use struct 'FileprivateImportType' in an extension with public or '@usableFromInline' members; 'FileprivateLib' was not imported publicly}}
    public func foo() {}
}

package protocol FileprivateConstrainedExtensionProtoInPackage {}
extension Array: FileprivateConstrainedExtensionProtoInPackage where Element == FileprivateImportType {}
extension FileprivateImportType {
    package func packageMethod() {}
}

internal protocol FileprivateConstrainedExtensionProtoInInternal {}
extension Array: FileprivateConstrainedExtensionProtoInInternal where Element == FileprivateImportType {}
extension FileprivateImportType {
    internal func internalMethod() {}
}

fileprivate protocol FileprivateConstrainedExtensionProtoInFileprivate {}
extension Array: FileprivateConstrainedExtensionProtoInFileprivate where Element == FileprivateImportType {}
extension FileprivateImportType {
    fileprivate func fileprivateMethod() {}
}

private protocol FileprivateConstrainedExtensionProtoInPrivate {}
extension Array: FileprivateConstrainedExtensionProtoInPrivate where Element == FileprivateImportType {}
extension FileprivateImportType {
    private func privateMethod() {}
}

public protocol PrivateConstrainedExtensionProto {}
extension Array: PrivateConstrainedExtensionProto where Element == PrivateImportType {} // expected-error {{cannot use struct 'PrivateImportType' in an extension with conditional conformances; 'PrivateLib' was not imported publicly}}
extension PrivateImportType { // expected-error {{cannot use struct 'PrivateImportType' in an extension with public or '@usableFromInline' members; 'PrivateLib' was not imported publicly}}
    public func foo() {}
}

package protocol PrivateConstrainedExtensionProtoInPackage {}
extension Array: PrivateConstrainedExtensionProtoInPackage where Element == PrivateImportType {}
extension PrivateImportType {
    package func packageMethod() {}
}

internal protocol PrivateConstrainedExtensionProtoInInternal {}
extension Array: PrivateConstrainedExtensionProtoInInternal where Element == PrivateImportType {}
extension PrivateImportType {
    internal func internalMethod() {}
}

fileprivate protocol PrivateConstrainedExtensionProtoInFileprivate {}
extension Array: PrivateConstrainedExtensionProtoInFileprivate where Element == PrivateImportType {}
extension PrivateImportType {
    fileprivate func fileprivateMethod() {}
}

private protocol PrivateConstrainedExtensionProtoInPrivate {}
extension Array: PrivateConstrainedExtensionProtoInPrivate where Element == PrivateImportType {}
extension PrivateImportType {
    private func privateMethod() {}
}
