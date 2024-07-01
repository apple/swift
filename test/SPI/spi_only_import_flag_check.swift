/// Test requiring the flag -experimental-spi-only-imports to use @_spiOnly.

// RUN: %empty-directory(%t)
// RUN: split-file %s %t

/// Build dummy dependency.
// RUN: %target-swift-frontend -emit-module %t/Empty.swift \
// RUN:   -module-name Empty -emit-module-path %t/Empty.swiftmodule

/// Attribute is rejected without the flag.
// RUN: %target-swift-frontend -emit-module %t/Client.swift -I %t \
// RUN:   -module-name Client -emit-module-path %t/Client.swiftmodule -verify

/// Attribute is accepted with the flag.
// RUN: %target-swift-frontend -emit-module %t/Client.swift -I %t \
// RUN:   -module-name Client -emit-module-path %t/Client.swiftmodule \
// RUN:   -experimental-spi-only-imports

/// Attribute is accepted without the flag in Swift 6.
// RUN: %target-swift-frontend -emit-module %t/Client.swift -I %t \
// RUN:   -module-name Client -emit-module-path %t/Client.swiftmodule \
// RUN:   -swift-version 6

/// Attribute is accepted without the flag when in a swiftinterface.
// RUN: %target-swift-typecheck-module-from-interface(%t/Client.private.swiftinterface) \
// RUN:   -I %t -module-name Client

//--- Empty.swift
//--- Client.swift

@_spiOnly import Empty // expected-error {{'@_spiOnly' requires setting the frontend flag '-experimental-spi-only-imports'}}

//--- Client.private.swiftinterface
// swift-interface-format-version: 1.0
// swift-compiler-version: Swift version 5.8-dev effective-4.1.50
// swift-module-flags: -swift-version 4 -module-name Client
import Swift
@_spiOnly import Empty
