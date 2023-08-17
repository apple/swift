/// An `@_private` import opens access to all SPIs of the imported module.
/// Exports of SPI in API are still reported.

// RUN: %empty-directory(%t)
// RUN: split-file %s %t

/// Build the library.
// RUN: %target-swift-frontend -emit-module %t/Lib_FileA.swift %t/Lib_FileB.swift \
// RUN:   -module-name Lib -emit-module-path %t/Lib.swiftmodule \
// RUN:   -enable-library-evolution -swift-version 5 \
// RUN:   -enable-private-imports

/// Typecheck a @_private client.
// RUN: %target-swift-frontend -typecheck -verify -I %t %t/PrivateClient.swift

/// Typecheck a regular client building against the same Lib with private imports enabled.
// RUN: %target-swift-frontend -typecheck -verify -I %t %t/RegularClient.swift

//--- Lib_FileA.swift
@_spi(S) public func spiFuncA() {}
@_spi(S) public struct SPITypeA {}

//--- Lib_FileB.swift
@_spi(S) public func spiFuncB() {}
@_spi(S) public struct SPITypeB {}

//--- PrivateClient.swift
@_private(sourceFile: "Lib_FileA.swift") import Lib

func useOnly(a: SPITypeA, b: SPITypeB) {
  spiFuncA()
  spiFuncB()
}

public func export(a: SPITypeA, b: SPITypeB) { // expected-error {{cannot use struct 'SPITypeA' here; it is an SPI imported from 'Lib'}}
                                           // expected-error @-1 {{cannot use struct 'SPITypeB' here; it is an SPI imported from 'Lib'}}
  spiFuncA()
  spiFuncB()
}

@inlinable
public func inlinableExport(a: SPITypeA, b: SPITypeB) { // expected-error {{struct 'SPITypeA' cannot be used in an '@inlinable' function because it is an SPI imported from 'Lib'}}
                                                    // expected-error @-1 {{struct 'SPITypeB' cannot be used in an '@inlinable' function because it is an SPI imported from 'Lib'}}
  spiFuncA() // expected-error {{global function 'spiFuncA()' cannot be used in an '@inlinable' function because it is an SPI imported from 'Lib'}}
  spiFuncB() // expected-error {{global function 'spiFuncB()' cannot be used in an '@inlinable' function because it is an SPI imported from 'Lib'}}
}

//--- RegularClient.swift
import Lib

func useOnly(a: SPITypeA, b: SPITypeB) { // expected-error {{cannot find type 'SPITypeA' in scope}}
                                     // expected-error @-1 {{cannot find type 'SPITypeB' in scope}}
  spiFuncA() // expected-error {{cannot find 'spiFuncA' in scope}}
  spiFuncB() // expected-error {{cannot find 'spiFuncB' in scope}}
}

public func export(a: SPITypeA, b: SPITypeB) { // expected-error {{cannot find type 'SPITypeA' in scope}}
                                           // expected-error @-1 {{cannot find type 'SPITypeB' in scope}}
  spiFuncA() // expected-error {{cannot find 'spiFuncA' in scope}}
  spiFuncB() // expected-error {{cannot find 'spiFuncB' in scope}}
}

@inlinable
public func inlinableExport(a: SPITypeA, b: SPITypeB) { // expected-error {{cannot find type 'SPITypeA' in scope}}
                                                    // expected-error @-1 {{cannot find type 'SPITypeB' in scope}}
  spiFuncA() // expected-error {{cannot find 'spiFuncA' in scope}}
  spiFuncB() // expected-error {{cannot find 'spiFuncB' in scope}}
}
