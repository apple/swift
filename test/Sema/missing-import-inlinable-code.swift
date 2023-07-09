/// Report the use in API of indirectly or implicitly imported decls.

// RUN: %empty-directory(%t)
// RUN: %{python} %utils/split_file.py -o %t %s

// RUN: %target-swift-frontend -emit-module %t/empty.swift -module-name empty -o %t/empty.swiftmodule \
// RUN:   -enable-library-evolution
// RUN: %target-swift-frontend -emit-module %t/libA.swift -module-name libA -o %t/libA.swiftmodule \
// RUN:   -enable-library-evolution
// RUN: %target-swift-frontend -emit-module %t/libB.swift -module-name libB -o %t/libB.swiftmodule -I %t \
// RUN:   -enable-library-evolution

/// In pre-Swift 6, this is a warning where there's no implementation-only import present.
// RUN: %target-swift-frontend -emit-module %t/clientFileA-Swift5.swift %t/clientFileB.swift -module-name client -o %t/client.swiftmodule -I %t -verify \
// RUN:   -enable-library-evolution

/// In pre-Swift 6, this remains an error when there's an implementation-only import present.
// RUN: %target-swift-frontend -emit-module %t/clientFileA-OldCheck.swift %t/clientFileB.swift -module-name client -o %t/client.swiftmodule -I %t -verify \
// RUN:   -enable-library-evolution

/// In Swift 6, it's an error.
// RUN: %target-swift-frontend -emit-module %t/clientFileA-Swift6.swift %t/clientFileB.swift -module-name client -o %t/client.swiftmodule -I %t -verify -swift-version 6 \
// RUN:   -enable-library-evolution

/// The swiftinterface is broken by the missing import without the workaround.
// RUN: %target-swift-emit-module-interface(%t/ClientBroken.swiftinterface)  %t/clientFileA-Swift5.swift %t/clientFileB.swift -I %t -disable-print-missing-imports-in-module-interface
// RUN: not %target-swift-typecheck-module-from-interface(%t/ClientBroken.swiftinterface) -I %t

/// The swiftinterface parses fine with the workaround adding the missing imports.
// RUN: %target-swift-emit-module-interface(%t/ClientFixed.swiftinterface)  %t/clientFileA-Swift5.swift %t/clientFileB.swift -I %t
// RUN: %target-swift-typecheck-module-from-interface(%t/ClientFixed.swiftinterface) -I %t

/// The inserted missing imports should be aliased.
// RUN: %target-swift-emit-module-interface(%t/ClientFixed.swiftinterface) %t/clientFileA-Swift5.swift %t/clientFileB.swift -I %t -alias-module-names-in-module-interface
// RUN: %target-swift-typecheck-module-from-interface(%t/ClientFixed.swiftinterface) -I %t
// RUN: cat %t/ClientFixed.swiftinterface | %FileCheck -check-prefix ALIASED %s
// ALIASED: import Module___libB

// REQUIRES: asserts

// BEGIN empty.swift

// BEGIN libA.swift
public struct ImportedType {
    public init() {}
}

// Test exportability of conformance uses
public protocol SomeProtocol {}
public func conformanceUse(_ a: SomeProtocol) {}

// BEGIN libB.swift
import libA

extension ImportedType {
    public func implicitlyImportedMethod() {}
}

extension ImportedType : SomeProtocol {}

/// Client module
// BEGIN clientFileA-Swift5.swift
import libA

@inlinable public func bar() {
  let a = ImportedType()
  a.implicitlyImportedMethod() // expected-warning {{instance method 'implicitlyImportedMethod()' cannot be used in an '@inlinable' function because 'libB' was not imported by this file; this is an error in Swift 6}}
  // expected-note@-1 {{The missing import of module 'libB' will be added implicitly}}

  // Expected implicit imports are still fine
  a.localModuleMethod()

  conformanceUse(a) // expected-warning {{cannot use conformance of 'ImportedType' to 'SomeProtocol' here; 'libB' was not imported by this file; this is an error in Swift 6}}
  // expected-note@-1 {{The missing import of module 'libB' will be added implicitly}}
}

// BEGIN clientFileA-OldCheck.swift
import libA
@_implementationOnly import empty

@inlinable public func bar() {
  let a = ImportedType()
  a.implicitlyImportedMethod() // expected-error {{instance method 'implicitlyImportedMethod()' cannot be used in an '@inlinable' function because 'libB' was not imported by this file}}

  // Expected implicit imports are still fine
  a.localModuleMethod()

  conformanceUse(a) // expected-warning {{cannot use conformance of 'ImportedType' to 'SomeProtocol' here; 'libB' was not imported by this file; this is an error in Swift 6}}
  // expected-note@-1 {{The missing import of module 'libB' will be added implicitly}}
}

// BEGIN clientFileA-Swift6.swift
import libA

@inlinable public func bar() {
  let a = ImportedType()
  a.implicitlyImportedMethod() // expected-error {{instance method 'implicitlyImportedMethod()' cannot be used in an '@inlinable' function because 'libB' was not imported by this file}}

  // Expected implicit imports are still fine
  a.localModuleMethod()

  conformanceUse(a) // expected-error {{cannot use conformance of 'ImportedType' to 'SomeProtocol' here; 'libB' was not imported by this file}}
}

// BEGIN clientFileB.swift
@_implementationOnly import libB
import libA
extension ImportedType {
    public func localModuleMethod() {}
}

