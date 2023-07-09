/// Test duplicate and conflicting imports from the same file.

// RUN: %empty-directory(%t)
// RUN: split-file %s %t

/// Generate dependencies.
// RUN: %target-swift-frontend -emit-module %t/Lib.swift \
// RUN:   -module-name Lib -emit-module-path %t/Lib.swiftmodule \
// RUN:   -swift-version 5 -enable-library-evolution

/// Build clients.
// RUN: %target-swift-frontend -typecheck %t/SPIOnly_Default.swift -I %t -verify \
// RUN:   -swift-version 5 -enable-library-evolution \
// RUN:   -experimental-spi-only-imports -verify
// RUN: %target-swift-frontend -typecheck %t/Default_SPIOnly.swift -I %t -verify \
// RUN:   -swift-version 5 -enable-library-evolution \
// RUN:   -experimental-spi-only-imports -verify
// RUN: %target-swift-frontend -typecheck %t/SPIOnly_Exported.swift -I %t -verify \
// RUN:   -swift-version 5 -enable-library-evolution \
// RUN:   -experimental-spi-only-imports -verify
// RUN: %target-swift-frontend -typecheck %t/Exported_SPIOnly.swift -I %t -verify \
// RUN:   -swift-version 5 -enable-library-evolution \
// RUN:   -experimental-spi-only-imports -verify
// RUN: %target-swift-frontend -typecheck %t/SPIOnly_IOI.swift -I %t -verify \
// RUN:   -swift-version 5 -enable-library-evolution \
// RUN:   -experimental-spi-only-imports -verify
// RUN: %target-swift-frontend -typecheck %t/SPIOnly_IOI_Exported_Default.swift -I %t -verify \
// RUN:   -swift-version 5 -enable-library-evolution \
// RUN:   -experimental-spi-only-imports -verify
// RUN: %target-swift-frontend -typecheck -primary-file %t/SPIOnly_Default_FileA.swift \
// RUN:   -swift-version 5 -enable-library-evolution \
// RUN:   %t/SPIOnly_Default_FileB.swift -I %t -verify \
// RUN:   -experimental-spi-only-imports -verify
// RUN: %target-swift-frontend -typecheck -primary-file %t/IOI_Default_FileA.swift \
// RUN:   -swift-version 5 -enable-library-evolution \
// RUN:   %t/IOI_Default_FileB.swift -I %t -verify \
// RUN:   -experimental-spi-only-imports -verify

//--- Lib.swift
// Empty source file for import.

//--- SPIOnly_Default.swift
@_spiOnly import Lib // expected-note {{imported for SPI only here}}
import Lib // expected-error {{'Lib' inconsistently imported for SPI only}}

//--- Default_SPIOnly.swift
import Lib // expected-error {{'Lib' inconsistently imported for SPI only}}
@_spiOnly import Lib // expected-note {{imported for SPI only here}}

//--- SPIOnly_Exported.swift
@_spiOnly import Lib // expected-note {{imported for SPI only here}}
@_exported import Lib // expected-error {{'Lib' inconsistently imported for SPI only}}

//--- Exported_SPIOnly.swift
@_exported import Lib // expected-error {{'Lib' inconsistently imported for SPI only}}
@_spiOnly import Lib // expected-note {{imported for SPI only here}}

//--- SPIOnly_IOI.swift
@_spiOnly import Lib // expected-note {{imported for SPI only here}}
// expected-warning @-1 {{'Lib' inconsistently imported as implementation-only}}
@_implementationOnly import Lib // expected-error {{'Lib' inconsistently imported for SPI only}}
// expected-note @-1 {{imported as implementation-only here}}

/// Many confliciting imports lead to many diagnostics.
//--- SPIOnly_IOI_Exported_Default.swift
@_spiOnly import Lib // expected-note 3 {{imported for SPI only here}}
// expected-warning @-1 {{'Lib' inconsistently imported as implementation-only}}
@_implementationOnly import Lib // expected-error {{'Lib' inconsistently imported for SPI only}}
// expected-note @-1 3 {{imported as implementation-only here}}
@_exported import Lib // expected-error {{'Lib' inconsistently imported for SPI only}}
// expected-warning @-1 {{'Lib' inconsistently imported as implementation-only}}
import Lib // expected-error {{'Lib' inconsistently imported for SPI only}}
// expected-warning @-1 {{'Lib' inconsistently imported as implementation-only}}

/// Different SPI only imports in different files of the same module are accepted.
//--- SPIOnly_Default_FileA.swift
@_spiOnly import Lib

//--- SPIOnly_Default_FileB.swift
import Lib

/// Different IOI in different files of the same module are still rejected.
//--- IOI_Default_FileA.swift
@_implementationOnly import Lib // expected-note {{imported as implementation-only here}}

//--- IOI_Default_FileB.swift
import Lib // expected-warning {{'Lib' inconsistently imported as implementation-only}}
