// RUN: %empty-directory(%t)
// RUN: split-file %s %t

// RUN: %target-swift-frontend -emit-module %t/Lib.swift -I %t \
// RUN:   -enable-library-evolution \
// RUN:   -module-name Lib -emit-module-path %t/Lib.swiftmodule \
// RUN:   -swift-version 5

/// Use of IOI types in SPI signatures is an error with -experimental-spi-only-imports
// RUN: %target-swift-frontend -emit-module %t/ClientSPIOnlyMode.swift -I %t \
// RUN:   -enable-library-evolution \
// RUN:   -swift-version 5 -verify \
// RUN:   -experimental-spi-only-imports

/// Use of IOI types in SPI signatures is a warning without -experimental-spi-only-imports
// RUN: %target-swift-frontend -emit-module %t/ClientDefaultMode.swift -I %t \
// RUN:   -enable-library-evolution \
// RUN:   -swift-version 5 -verify

//--- Lib.swift

public struct IOIStruct {
    public init() {}
}

//--- ClientSPIOnlyMode.swift

@_implementationOnly import Lib

@_spi(X) public func spiClient(s: IOIStruct) -> IOIStruct { // expected-error 2 {{cannot use struct 'IOIStruct' here; 'Lib' has been imported as implementation-only}}
    return IOIStruct()
}

@_spi(X) @inlinable public func inlinableClient(s: IOIStruct) -> IOIStruct { // expected-error 2 {{struct 'IOIStruct' cannot be used in an '@inlinable' function because 'Lib' was imported implementation-only}}
    return IOIStruct() // expected-error {{struct 'IOIStruct' cannot be used in an '@inlinable' function because 'Lib' was imported implementation-only}}
    // expected-error @-1 {{initializer 'init()' cannot be used in an '@inlinable' function because 'Lib' was imported implementation-only}}
}

//--- ClientDefaultMode.swift

@_implementationOnly import Lib

@_spi(X) public func spiClient(s: IOIStruct) -> IOIStruct { // expected-warning 2 {{cannot use struct 'IOIStruct' here; 'Lib' has been imported as implementation-only}}
    return IOIStruct()
}

@_spi(X) @inlinable public func inlinableClient(s: IOIStruct) -> IOIStruct { // expected-error 2 {{struct 'IOIStruct' cannot be used in an '@inlinable' function because 'Lib' was imported implementation-only}}
    return IOIStruct() // expected-error {{struct 'IOIStruct' cannot be used in an '@inlinable' function because 'Lib' was imported implementation-only}}
    // expected-error @-1 {{initializer 'init()' cannot be used in an '@inlinable' function because 'Lib' was imported implementation-only}}
}
