// RUN: %empty-directory(%t)
// RUN: split-file %s %t

// RUN: %target-swift-frontend -emit-module %t/Lib.swift -I %t \
// RUN:   -module-name Lib -emit-module-path %t/Lib.swiftmodule \
// RUN:   -swift-version 5
// RUN: %target-swift-frontend -emit-module %t/APILib.swift -I %t \
// RUN:   -swift-version 5 -verify \
// RUN:   -experimental-spi-only-imports \
// RUN:   -library-level api \
// RUN:   -require-explicit-availability=ignore
// RUN: %target-swift-frontend -emit-module %t/SPILib.swift -I %t \
// RUN:   -swift-version 5 -verify \
// RUN:   -experimental-spi-only-imports \
// RUN:   -library-level spi
// RUN: %target-swift-frontend -emit-module %t/OtherLib.swift -I %t \
// RUN:   -swift-version 5 -verify \
// RUN:   -experimental-spi-only-imports

//--- Lib.swift

public struct LibStruct {}

//--- APILib.swift

@_spiOnly import Lib

public func publicClient() -> LibStruct { fatalError() } // expected-error {{cannot use struct 'LibStruct' here; 'Lib' was imported for SPI only}}
@_spi(X) public func spiClient() -> LibStruct { fatalError() }

//--- SPILib.swift

@_spiOnly import Lib

public func publicClient() -> LibStruct { fatalError() }
@_spi(X) public func spiClient() -> LibStruct { fatalError() }

//--- OtherLib.swift

@_spiOnly import Lib

public func publicClient() -> LibStruct { fatalError() } // expected-error {{cannot use struct 'LibStruct' here; 'Lib' was imported for SPI only}}
@_spi(X) public func spiClient() -> LibStruct { fatalError() }
