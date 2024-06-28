// RUN: %empty-directory(%t)
// RUN: split-file %s %t

// RUN: %target-swift-emit-module-interface(%t/Original.swiftinterface) %t/Original.swift
// RUN: %target-swift-typecheck-module-from-interface(%t/Original.swiftinterface)

// RUN: %target-swift-emit-module-interface(%t/Aliases.swiftinterface) %t/Aliases.swift -I %t
// RUN: %target-swift-typecheck-module-from-interface(%t/Aliases.swiftinterface) -I %t

// RUN: %target-swift-frontend -typecheck -verify %t/UsesAliasesNoImport.swift -enable-library-evolution -I %t
// RUN: %target-swift-frontend -typecheck -verify %t/UsesAliasesImplementationOnlyImport.swift -enable-library-evolution -I %t
// RUN: %target-swift-frontend -typecheck -verify %t/UsesAliasesSPIOnlyImport.swift -enable-library-evolution -I %t -experimental-spi-only-imports
// RUN: %target-swift-frontend -typecheck -verify %t/UsesAliasesWithImport.swift -enable-library-evolution -I %t

/// With library evolution disabled UsesAliasesNoImport.swift should compile without diagnostics.
// RUN: %target-swift-frontend -typecheck %t/UsesAliasesNoImport.swift -I %t | %FileCheck %t/UsesAliasesNoImport.swift --check-prefix=CHECK-NON-RESILIENT --allow-empty

/// The swiftinterface is broken by the missing import without the workaround.
// RUN: %target-swift-emit-module-interface(%t/UsesAliasesNoImport.swiftinterface) %t/UsesAliasesNoImport.swift -I %t \
// RUN:   -disable-print-missing-imports-in-module-interface
// RUN: not %target-swift-typecheck-module-from-interface(%t/UsesAliasesNoImport.swiftinterface) -I %t

/// The swiftinterface parses fine with the workaround adding the missing imports.
// RUN: %target-swift-emit-module-interface(%t/UsesAliasesNoImportFixed.swiftinterface) %t/UsesAliasesNoImport.swift -I %t
// RUN: %target-swift-typecheck-module-from-interface(%t/UsesAliasesNoImportFixed.swiftinterface) -I %t

/// The module with an implementation-only import is not affected by the workaround and remains broken.
// RUN: %target-swift-emit-module-interface(%t/UsesAliasesImplementationOnlyImport.swiftinterface) %t/UsesAliasesImplementationOnlyImport.swift -I %t \
// RUN:   -disable-print-missing-imports-in-module-interface
// RUN: not %target-swift-typecheck-module-from-interface(%t/UsesAliasesImplementationOnlyImport.swiftinterface) -I %t

//--- Original.swift

open class Clazz {}

public protocol Proto {
  func requirement()
}

public struct Struct {
  public init() {}
}

@propertyWrapper
public struct Wrapper<T> {
  public var wrappedValue: T
  public init(wrappedValue: T) {
    self.wrappedValue = wrappedValue
  }
}


//--- Aliases.swift

import Original

public typealias ClazzAlias = Clazz
public typealias ProtoAlias = Proto
public typealias StructAlias = Struct
public typealias WrapperAlias = Wrapper


//--- UsesAliasesNoImport.swift

import Aliases

// CHECK-NON-RESILIENT-NOT: was not imported by this file

// expected-warning@+2 {{'ClazzAlias' aliases 'Original.Clazz' and cannot be used in a public or '@usableFromInline' conformance because 'Original' was not imported by this file; this is an error in the Swift 6 language mode}}
// expected-note@+1 {{The missing import of module 'Original' will be added implicitly}}
public class InheritsFromClazzAlias: ClazzAlias {}

@inlinable public func inlinableFunc() {
  // expected-warning@+2 {{'StructAlias' aliases 'Original.Struct' and cannot be used in an '@inlinable' function because 'Original' was not imported by this file; this is an error in the Swift 6 language mode}}
  // expected-note@+1 {{The missing import of module 'Original' will be added implicitly}}
  _ = StructAlias.self
}

// expected-warning@+2 {{'ProtoAlias' aliases 'Original.Proto' and cannot be used here because 'Original' was not imported by this file; this is an error in the Swift 6 language mode}}
// expected-note@+1 {{The missing import of module 'Original' will be added implicitly}}
public func takesGeneric<T: ProtoAlias>(_ t: T) {}

public struct HasMembers {
  // expected-warning@+2 {{'WrapperAlias' aliases 'Original.Wrapper' and cannot be used as property wrapper here because 'Original' was not imported by this file; this is an error in the Swift 6 language mode}}
 // expected-note@+1 {{The missing import of module 'Original' will be added implicitly}}
  @WrapperAlias public var wrapped: Int
}

// expected-warning@+2 {{'StructAlias' aliases 'Original.Struct' and cannot be used in an extension with public or '@usableFromInline' members because 'Original' was not imported by this file; this is an error in the Swift 6 language mode}}
// expected-note@+1 {{The missing import of module 'Original' will be added implicitly}}
extension StructAlias {
  public func someFunc() {}
}


//--- UsesAliasesImplementationOnlyImport.swift

import Aliases
@_implementationOnly import Original
// expected-warning @-1 {{'@_implementationOnly' is deprecated, use 'internal import' instead}}

@inlinable public func inlinableFunc() {
  // expected-warning@+1 {{'StructAlias' aliases 'Original.Struct' and cannot be used in an '@inlinable' function because 'Original' has been imported as implementation-only; this is an error in the Swift 6 language mode}}
  _ = StructAlias.self
}

// expected-warning@+1 {{'ProtoAlias' aliases 'Original.Proto' and cannot be used here because 'Original' has been imported as implementation-only; this is an error in the Swift 6 language mode}}
public func takesGeneric<T: ProtoAlias>(_ t: T) {}


//--- UsesAliasesSPIOnlyImport.swift

import Aliases
@_spiOnly import Original

@inlinable public func inlinableFunc() {
  // expected-error@+1 {{'StructAlias' aliases 'Original.Struct' and cannot be used in an '@inlinable' function because 'Original' was imported for SPI only}}
  _ = StructAlias.self
}

// expected-error@+1 {{'ProtoAlias' aliases 'Original.Proto' and cannot be used here because 'Original' was imported for SPI only}}
public func takesGeneric<T: ProtoAlias>(_ t: T) {}


//--- UsesAliasesWithImport.swift

import Aliases
import Original

@inlinable public func inlinableFunc() {
  _ = StructAlias.self
}

public func takesGeneric<T: ProtoAlias>(_ t: T) {}
