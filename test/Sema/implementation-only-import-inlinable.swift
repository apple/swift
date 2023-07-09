// RUN: %empty-directory(%t)
// RUN: %target-swift-frontend -emit-module -o %t/indirects.swiftmodule %S/Inputs/implementation-only-imports/indirects.swift \
// RUN:   -enable-library-evolution -swift-version 5
// RUN: %target-swift-frontend -emit-module -o %t/directs.swiftmodule -I %t %S/Inputs/implementation-only-imports/directs.swift \
// RUN:   -enable-library-evolution -swift-version 5

// RUN: %target-swift-frontend -typecheck -verify %s -I %t \
// RUN:   -enable-library-evolution -swift-version 5

@_implementationOnly import directs
import indirects

// Types

@inlinable
public func testStructFromDirect() {
  _ = StructFromDirect() // expected-error {{struct 'StructFromDirect' cannot be used in an '@inlinable' function because 'directs' was imported implementation-only}}
  // expected-error@-1 {{initializer 'init()' cannot be used in an '@inlinable' function because 'directs' was imported implementation-only}}
}

@inlinable
public func testStructFromIndirect() {
  _ = StructFromIndirect()
}

@inlinable
public func testAliasFromDirect() {
  _ = AliasFromDirect() // expected-error {{type alias 'AliasFromDirect' cannot be used in an '@inlinable' function because 'directs' was imported implementation-only}}
  // expected-error@-1 {{initializer 'init()' cannot be used in an '@inlinable' function because 'directs' was imported implementation-only}}
}

@inlinable
public func testAliasFromIndirect() {
  _ = AliasFromIndirect()
}

@inlinable
public func testGenericAliasFromDirect() {
  _ = GenericAliasFromDirect<Int>.self // expected-error {{type alias 'GenericAliasFromDirect' cannot be used in an '@inlinable' function because 'directs' was imported implementation-only}}
}

@inlinable
public func testGenericAliasFromIndirect() {
  let tmp: GenericAliasFromIndirect<Int>?
  _ = tmp
}

// Functions

@inlinable
public func testFunctionFromDirect() {
  globalFunctionFromDirect() // expected-error {{global function 'globalFunctionFromDirect()' cannot be used in an '@inlinable' function because 'directs' was imported implementation-only}}
}

@inlinable
public func testFunctionFromIndirect() {
  globalFunctionFromIndirect()
}

// Variables

@inlinable
public func testVariableFromDirect_get() {
  _ = globalVariableFromDirect // expected-error {{var 'globalVariableFromDirect' cannot be used in an '@inlinable' function because 'directs' was imported implementation-only}}
}

@inlinable
public func testVariableFromIndirect_get() {
  _ = globalVariableFromIndirect
}

@inlinable
public func testVariableFromDirect_set() {
  globalVariableFromDirect = 5 // expected-error {{var 'globalVariableFromDirect' cannot be used in an '@inlinable' function because 'directs' was imported implementation-only}}
}

@inlinable
public func testVariableFromIndirect_set() {
  globalVariableFromIndirect = 5
}

// Extensions

@inlinable
public func testExtensionMethod(s: inout StructFromIndirect) {
  s.extensionMethodFromDirect() // expected-error {{instance method 'extensionMethodFromDirect()' cannot be used in an '@inlinable' function because 'directs' was imported implementation-only}}
}

@inlinable
public func testExtensionProperty_get(s: inout StructFromIndirect) {
  _ = s.extensionPropertyFromDirect // expected-error {{property 'extensionPropertyFromDirect' cannot be used in an '@inlinable' function because 'directs' was imported implementation-only}}
}

@inlinable
public func testExtensionProperty_set(s: inout StructFromIndirect) {
  s.extensionPropertyFromDirect = 5 // expected-error {{property 'extensionPropertyFromDirect' cannot be used in an '@inlinable' function because 'directs' was imported implementation-only}}
}

@inlinable
public func testExtensionSubscript_get(s: inout StructFromIndirect) {
  _ = s[extensionSubscript: 0] // expected-error {{cannot be used in an '@inlinable' function because 'directs' was imported implementation-only}}
}

@inlinable
public func testExtensionSubscript_set(s: inout StructFromIndirect) {
  s[extensionSubscript: 0] = 5 // expected-error {{cannot be used in an '@inlinable' function because 'directs' was imported implementation-only}}
}
