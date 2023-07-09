/// Test that the least restricting restricted import takes priority in
/// exportability checks.

// RUN: %empty-directory(%t)
// RUN: split-file %s %t

/// Generate dependencies.
// RUN: %target-swift-frontend -emit-module %t/LibA.swift \
// RUN:   -module-name LibA -emit-module-path %t/LibA.swiftmodule \
// RUN:   -swift-version 5 -enable-library-evolution
// RUN: %target-swift-frontend -emit-module %t/LibB.swift \
// RUN:   -module-name LibB -emit-module-path %t/LibB.swiftmodule \
// RUN:   -swift-version 5 -enable-library-evolution -I %t
// RUN: %target-swift-frontend -emit-module %t/LibC.swift \
// RUN:   -module-name LibC -emit-module-path %t/LibC.swiftmodule \
// RUN:   -swift-version 5 -enable-library-evolution -I %t

// RUN: %target-swift-frontend -typecheck %t/TwoIOI.swift -I %t -verify \
// RUN:   -swift-version 5 -enable-library-evolution \
// RUN:   -experimental-spi-only-imports -verify
// RUN: %target-swift-frontend -typecheck %t/SPIOnlyAndIOI1.swift -I %t -verify \
// RUN:   -swift-version 5 -enable-library-evolution \
// RUN:   -experimental-spi-only-imports -verify
// RUN: %target-swift-frontend -typecheck %t/SPIOnlyAndIOI2.swift -I %t -verify \
// RUN:   -swift-version 5 -enable-library-evolution \
// RUN:   -experimental-spi-only-imports -verify
// RUN: %target-swift-frontend -typecheck %t/TwoSPIOnly.swift -I %t -verify \
// RUN:   -swift-version 5 -enable-library-evolution \
// RUN:   -experimental-spi-only-imports -verify
// RUN: %target-swift-frontend -typecheck %t/OneSPIOnly1.swift -I %t -verify \
// RUN:   -swift-version 5 -enable-library-evolution \
// RUN:   -experimental-spi-only-imports -verify
// RUN: %target-swift-frontend -typecheck %t/OneSPIOnly2.swift -I %t -verify \
// RUN:   -swift-version 5 -enable-library-evolution \
// RUN:   -experimental-spi-only-imports -verify

/// Setup 2 indirect imports of LibA, allowing the same LibA to be imported
/// via 2 different imports from the same file.
//--- LibA.swift
public struct LibAStruct {}
//--- LibB.swift
@_exported import LibA
//--- LibC.swift
@_exported import LibA

//--- TwoIOI.swift
@_implementationOnly import LibB
@_implementationOnly import LibC

public func foo(a: LibAStruct) {} // expected-error {{cannot use struct 'LibAStruct' here; 'LibA' has been imported as implementation-only}}

//--- SPIOnlyAndIOI1.swift
@_spiOnly import LibB
@_implementationOnly import LibC

public func foo(a: LibAStruct) {} // expected-error {{cannot use struct 'LibAStruct' here; 'LibA' was imported for SPI only}}

//--- SPIOnlyAndIOI2.swift
@_implementationOnly import LibB
@_spiOnly import LibC

public func foo(a: LibAStruct) {} // expected-error {{cannot use struct 'LibAStruct' here; 'LibA' was imported for SPI only}}

//--- TwoSPIOnly.swift
@_spiOnly import LibB
@_spiOnly import LibC

public func foo(a: LibAStruct) {} // expected-error {{cannot use struct 'LibAStruct' here; 'LibA' was imported for SPI only}}

//--- OneSPIOnly1.swift
import LibB
@_spiOnly import LibC

public func foo(a: LibAStruct) {}

//--- OneSPIOnly2.swift
import LibB
@_spiOnly import LibC

public func foo(a: LibAStruct) {}
