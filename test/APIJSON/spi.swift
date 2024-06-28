// REQUIRES: objc_interop, OS=macosx
// RUN: %empty-directory(%t)
// RUN: %empty-directory(%t/ModuleCache)

// RUN: %target-swift-frontend(mock-sdk: %clang-importer-sdk-nosource -I %t) %s -parse-as-library  -emit-module -emit-module-path %t/MyModule.swiftmodule -enable-library-evolution -module-name MyModule -swift-version 5
// RUN: %target-swift-frontend(mock-sdk: %clang-importer-sdk-nosource -I %t) %s -parse-as-library  -emit-module -emit-module-path %t/MyModule.swiftmodule -enable-library-evolution -module-name MyModule -swift-version 5 -emit-api-descriptor-path %t/api.json
// RUN: %validate-json %t/api.json | %FileCheck %s --check-prefixes=CHECK-SPI,CHECK-SPI-EMIT

import Foundation

@_spi(Experimental) public func newUnprovenFunc() {}
@_spi(Experimental) public class MyClass : NSObject {
  @objc public func spiMethod() {}
}

public class MyClass2 : NSObject {
  @_spi(Experimental) @objc public func spiMethod() {}

  @_spi_available(macOS 10.10, tvOS 14.0, *)
  @available(iOS 8.0, *)
  @objc public func spiAvailableMethod() {}
}

@_spi_available(macOS 10.10, tvOS 14.0, *)
@available(iOS 8.0, *)
public func spiAvailableFunc() {}

// CHECK:      {
// CHECK-NEXT:   "target":
// CHECK-NEXT:   "globals": [
// CHECK-NEXT:     {
// CHECK-NEXT:       "name": "_$s8MyModule0A6Class2C18spiAvailableMethodyyFTj",
// CHECK-NEXT:       "access": "public",
// CHECK-NEXT:       "file": "/@input/MyModule.swiftinterface",
// CHECK-NEXT:       "linkage": "exported",
// CHECK-NEXT:       "unavailable": true
// CHECK-NEXT:     },
// CHECK-NEXT:     {
// CHECK-NEXT:       "name": "_$s8MyModule0A6Class2C18spiAvailableMethodyyFTq",
// CHECK-NEXT:       "access": "public",
// CHECK-NEXT:       "file": "/@input/MyModule.swiftinterface",
// CHECK-NEXT:       "linkage": "exported",
// CHECK-NEXT:       "unavailable": true
// CHECK-NEXT:     },
// CHECK-NEXT:     {
// CHECK-NEXT:       "name": "_$s8MyModule0A6Class2CACycfC",
// CHECK-NEXT:       "access": "public",
// CHECK-NEXT:       "file": "/@input/MyModule.swiftinterface",
// CHECK-NEXT:       "linkage": "exported"
// CHECK-NEXT:     },
// CHECK-NEXT:     {
// CHECK-NEXT:       "name": "_$s8MyModule0A6Class2CACycfc",
// CHECK-NEXT:       "access": "public",
// CHECK-NEXT:       "file": "/@input/MyModule.swiftinterface",
// CHECK-NEXT:       "linkage": "exported"
// CHECK-NEXT:     },
// CHECK-NEXT:     {
// CHECK-NEXT:       "name": "_$s8MyModule0A6Class2CMa",
// CHECK-NEXT:       "access": "public",
// CHECK-NEXT:       "file": "/@input/MyModule.swiftinterface",
// CHECK-NEXT:       "linkage": "exported"
// CHECK-NEXT:     },
// CHECK-NEXT:     {
// CHECK-NEXT:       "name": "_$s8MyModule0A6Class2CMn",
// CHECK-NEXT:       "access": "public",
// CHECK-NEXT:       "file": "/@input/MyModule.swiftinterface",
// CHECK-NEXT:       "linkage": "exported"
// CHECK-NEXT:     },
// CHECK-NEXT:     {
// CHECK-NEXT:       "name": "_$s8MyModule0A6Class2CMo",
// CHECK-NEXT:       "access": "public",
// CHECK-NEXT:       "file": "/@input/MyModule.swiftinterface",
// CHECK-NEXT:       "linkage": "exported"
// CHECK-NEXT:     },
// CHECK-NEXT:     {
// CHECK-NEXT:       "name": "_$s8MyModule0A6Class2CMu",
// CHECK-NEXT:       "access": "public",
// CHECK-NEXT:       "file": "/@input/MyModule.swiftinterface",
// CHECK-NEXT:       "linkage": "exported"
// CHECK-NEXT:     },
// CHECK-NEXT:     {
// CHECK-NEXT:       "name": "_$s8MyModule0A6Class2CN",
// CHECK-NEXT:       "access": "public",
// CHECK-NEXT:       "file": "/@input/MyModule.swiftinterface",
// CHECK-NEXT:       "linkage": "exported"
// CHECK-NEXT:     },
// CHECK-NEXT:     {
// CHECK-NEXT:       "name": "_$s8MyModule0A6Class2CfD",
// CHECK-NEXT:       "access": "public",
// CHECK-NEXT:       "file": "/@input/MyModule.swiftinterface",
// CHECK-NEXT:       "linkage": "exported"
// CHECK-NEXT:     },
// CHECK-NEXT:     {
// CHECK-NEXT:       "name": "_$s8MyModule16spiAvailableFuncyyF",
// CHECK-NEXT:       "access": "public",
// CHECK-NEXT:       "file": "/@input/MyModule.swiftinterface",
// CHECK-NEXT:       "linkage": "exported",
// CHECK-NEXT:       "unavailable": true
// CHECK-NEXT:     }
// CHECK-NEXT:   ],
// CHECK-NEXT:   "interfaces": [
// CHECK-NEXT:     {
// CHECK-NEXT:       "name": "_TtC8MyModule8MyClass2",
// CHECK-NEXT:       "access": "public",
// CHECK-NEXT:       "file": "/@input/MyModule.swiftinterface",
// CHECK-NEXT:       "linkage": "exported",
// CHECK-NEXT:       "super": "NSObject",
// CHECK-NEXT:       "instanceMethods": [
// CHECK-NEXT:         {
// CHECK-NEXT:           "name": "spiAvailableMethod",
// CHECK-NEXT:           "access": "public",
// CHECK-NEXT:           "file": "/@input/MyModule.swiftinterface",
// CHECK-NEXT:           "unavailable": true
// CHECK-NEXT:         },
// CHECK-NEXT:         {
// CHECK-NEXT:           "name": "init",
// CHECK-NEXT:           "access": "public",
// CHECK-NEXT:           "file": "/@input/MyModule.swiftinterface"
// CHECK-NEXT:         }
// CHECK-NEXT:       ],
// CHECK-NEXT:       "classMethods": []
// CHECK-NEXT:     }
// CHECK-NEXT:   ],
// CHECK-NEXT:   "categories": [],
// CHECK-NEXT:   "version": "1.0"
// CHECK-NEXT: }

// CHECK-SPI:      {
// CHECK-SPI-NEXT:   "target":
// CHECK-SPI-NEXT:   "globals": [
// CHECK-SPI-NEXT:     {
// CHECK-SPI-NEXT:       "name": "_$s8MyModule0A5ClassC9spiMethodyyFTj",
// CHECK-SPI-NEXT:       "access": "private",
// CHECK-SPI-EXTRACT-NEXT:  "file": "/@input/MyModule.swiftmodule",
// CHECK-SPI-EMIT-NEXT:     "file": "SOURCE_DIR/test/APIJSON/spi.swift",
// CHECK-SPI-NEXT:       "linkage": "exported"
// CHECK-SPI-NEXT:     },
// CHECK-SPI-NEXT:     {
// CHECK-SPI-NEXT:       "name": "_$s8MyModule0A5ClassC9spiMethodyyFTq",
// CHECK-SPI-NEXT:       "access": "private",
// CHECK-SPI-EXTRACT-NEXT:  "file": "/@input/MyModule.swiftmodule",
// CHECK-SPI-EMIT-NEXT:     "file": "SOURCE_DIR/test/APIJSON/spi.swift",
// CHECK-SPI-NEXT:       "linkage": "exported"
// CHECK-SPI-NEXT:     },
// CHECK-SPI-NEXT:     {
// CHECK-SPI-NEXT:       "name": "_$s8MyModule0A5ClassCACycfC",
// CHECK-SPI-NEXT:       "access": "private",
// CHECK-SPI-EXTRACT-NEXT:  "file": "/@input/MyModule.swiftmodule",
// CHECK-SPI-EMIT-NEXT:     "file": "SOURCE_DIR/test/APIJSON/spi.swift",
// CHECK-SPI-NEXT:       "linkage": "exported"
// CHECK-SPI-NEXT:     },
// CHECK-SPI-NEXT:     {
// CHECK-SPI-NEXT:       "name": "_$s8MyModule0A5ClassCACycfc",
// CHECK-SPI-NEXT:       "access": "private",
// CHECK-SPI-EXTRACT-NEXT:  "file": "/@input/MyModule.swiftmodule",
// CHECK-SPI-EMIT-NEXT:     "file": "SOURCE_DIR/test/APIJSON/spi.swift",
// CHECK-SPI-NEXT:       "linkage": "exported"
// CHECK-SPI-NEXT:     },
// CHECK-SPI-NEXT:     {
// CHECK-SPI-NEXT:       "name": "_$s8MyModule0A5ClassCMa",
// CHECK-SPI-NEXT:       "access": "private",
// CHECK-SPI-EXTRACT-NEXT:  "file": "/@input/MyModule.swiftmodule",
// CHECK-SPI-EMIT-NEXT:     "file": "SOURCE_DIR/test/APIJSON/spi.swift",
// CHECK-SPI-NEXT:       "linkage": "exported"
// CHECK-SPI-NEXT:     },
// CHECK-SPI-NEXT:     {
// CHECK-SPI-NEXT:       "name": "_$s8MyModule0A5ClassCMn",
// CHECK-SPI-NEXT:       "access": "private",
// CHECK-SPI-EXTRACT-NEXT:  "file": "/@input/MyModule.swiftmodule",
// CHECK-SPI-EMIT-NEXT:     "file": "SOURCE_DIR/test/APIJSON/spi.swift",
// CHECK-SPI-NEXT:       "linkage": "exported"
// CHECK-SPI-NEXT:     },
// CHECK-SPI-NEXT:     {
// CHECK-SPI-NEXT:       "name": "_$s8MyModule0A5ClassCMo",
// CHECK-SPI-NEXT:       "access": "private",
// CHECK-SPI-EXTRACT-NEXT:  "file": "/@input/MyModule.swiftmodule",
// CHECK-SPI-EMIT-NEXT:     "file": "SOURCE_DIR/test/APIJSON/spi.swift",
// CHECK-SPI-NEXT:       "linkage": "exported"
// CHECK-SPI-NEXT:     },
// CHECK-SPI-NEXT:     {
// CHECK-SPI-NEXT:       "name": "_$s8MyModule0A5ClassCMu",
// CHECK-SPI-NEXT:       "access": "private",
// CHECK-SPI-EXTRACT-NEXT:  "file": "/@input/MyModule.swiftmodule",
// CHECK-SPI-EMIT-NEXT:     "file": "SOURCE_DIR/test/APIJSON/spi.swift",
// CHECK-SPI-NEXT:       "linkage": "exported"
// CHECK-SPI-NEXT:     },
// CHECK-SPI-NEXT:     {
// CHECK-SPI-NEXT:       "name": "_$s8MyModule0A5ClassCN",
// CHECK-SPI-NEXT:       "access": "private",
// CHECK-SPI-EXTRACT-NEXT:  "file": "/@input/MyModule.swiftmodule",
// CHECK-SPI-EMIT-NEXT:     "file": "SOURCE_DIR/test/APIJSON/spi.swift",
// CHECK-SPI-NEXT:       "linkage": "exported"
// CHECK-SPI-NEXT:     },
// CHECK-SPI-NEXT:     {
// CHECK-SPI-NEXT:       "name": "_$s8MyModule0A5ClassCfD",
// CHECK-SPI-NEXT:       "access": "private",
// CHECK-SPI-EXTRACT-NEXT:  "file": "/@input/MyModule.swiftmodule",
// CHECK-SPI-EMIT-NEXT:     "file": "SOURCE_DIR/test/APIJSON/spi.swift",
// CHECK-SPI-NEXT:       "linkage": "exported"
// CHECK-SPI-NEXT:     },
// CHECK-SPI-NEXT:     {
// CHECK-SPI-NEXT:       "name": "_$s8MyModule0A6Class2C18spiAvailableMethodyyFTj",
// CHECK-SPI-NEXT:       "access": "private",
// CHECK-SPI-EXTRACT-NEXT:  "file": "/@input/MyModule.swiftmodule",
// CHECK-SPI-EMIT-NEXT:     "file": "SOURCE_DIR/test/APIJSON/spi.swift",
// CHECK-SPI-NEXT:       "linkage": "exported",
// CHECK-SPI-NEXT:       "introduced": "10.10"
// CHECK-SPI-NEXT:     },
// CHECK-SPI-NEXT:     {
// CHECK-SPI-NEXT:       "name": "_$s8MyModule0A6Class2C18spiAvailableMethodyyFTq",
// CHECK-SPI-NEXT:       "access": "private",
// CHECK-SPI-EXTRACT-NEXT:  "file": "/@input/MyModule.swiftmodule",
// CHECK-SPI-EMIT-NEXT:     "file": "SOURCE_DIR/test/APIJSON/spi.swift",
// CHECK-SPI-NEXT:       "linkage": "exported",
// CHECK-SPI-NEXT:       "introduced": "10.10"
// CHECK-SPI-NEXT:     },
// CHECK-SPI-NEXT:     {
// CHECK-SPI-NEXT:       "name": "_$s8MyModule0A6Class2C9spiMethodyyFTj",
// CHECK-SPI-NEXT:       "access": "private",
// CHECK-SPI-EXTRACT-NEXT:  "file": "/@input/MyModule.swiftmodule",
// CHECK-SPI-EMIT-NEXT:     "file": "SOURCE_DIR/test/APIJSON/spi.swift",
// CHECK-SPI-NEXT:       "linkage": "exported"
// CHECK-SPI-NEXT:     },
// CHECK-SPI-NEXT:     {
// CHECK-SPI-NEXT:       "name": "_$s8MyModule0A6Class2C9spiMethodyyFTq",
// CHECK-SPI-NEXT:       "access": "private",
// CHECK-SPI-EXTRACT-NEXT:  "file": "/@input/MyModule.swiftmodule",
// CHECK-SPI-EMIT-NEXT:     "file": "SOURCE_DIR/test/APIJSON/spi.swift",
// CHECK-SPI-NEXT:       "linkage": "exported"
// CHECK-SPI-NEXT:     },
// CHECK-SPI-NEXT:     {
// CHECK-SPI-NEXT:       "name": "_$s8MyModule0A6Class2CACycfC",
// CHECK-SPI-NEXT:       "access": "public",
// CHECK-SPI-EXTRACT-NEXT:  "file": "/@input/MyModule.swiftmodule",
// CHECK-SPI-EMIT-NEXT:     "file": "SOURCE_DIR/test/APIJSON/spi.swift",
// CHECK-SPI-NEXT:       "linkage": "exported"
// CHECK-SPI-NEXT:     },
// CHECK-SPI-NEXT:     {
// CHECK-SPI-NEXT:       "name": "_$s8MyModule0A6Class2CACycfc",
// CHECK-SPI-NEXT:       "access": "public",
// CHECK-SPI-EXTRACT-NEXT:  "file": "/@input/MyModule.swiftmodule",
// CHECK-SPI-EMIT-NEXT:     "file": "SOURCE_DIR/test/APIJSON/spi.swift",
// CHECK-SPI-NEXT:       "linkage": "exported"
// CHECK-SPI-NEXT:     },
// CHECK-SPI-NEXT:     {
// CHECK-SPI-NEXT:       "name": "_$s8MyModule0A6Class2CMa",
// CHECK-SPI-NEXT:       "access": "public",
// CHECK-SPI-EXTRACT-NEXT:  "file": "/@input/MyModule.swiftmodule",
// CHECK-SPI-EMIT-NEXT:     "file": "SOURCE_DIR/test/APIJSON/spi.swift",
// CHECK-SPI-NEXT:       "linkage": "exported"
// CHECK-SPI-NEXT:     },
// CHECK-SPI-NEXT:     {
// CHECK-SPI-NEXT:       "name": "_$s8MyModule0A6Class2CMn",
// CHECK-SPI-NEXT:       "access": "public",
// CHECK-SPI-EXTRACT-NEXT:  "file": "/@input/MyModule.swiftmodule",
// CHECK-SPI-EMIT-NEXT:     "file": "SOURCE_DIR/test/APIJSON/spi.swift",
// CHECK-SPI-NEXT:       "linkage": "exported"
// CHECK-SPI-NEXT:     },
// CHECK-SPI-NEXT:     {
// CHECK-SPI-NEXT:       "name": "_$s8MyModule0A6Class2CMo",
// CHECK-SPI-NEXT:       "access": "public",
// CHECK-SPI-EXTRACT-NEXT:  "file": "/@input/MyModule.swiftmodule",
// CHECK-SPI-EMIT-NEXT:     "file": "SOURCE_DIR/test/APIJSON/spi.swift",
// CHECK-SPI-NEXT:       "linkage": "exported"
// CHECK-SPI-NEXT:     },
// CHECK-SPI-NEXT:     {
// CHECK-SPI-NEXT:       "name": "_$s8MyModule0A6Class2CMu",
// CHECK-SPI-NEXT:       "access": "public",
// CHECK-SPI-EXTRACT-NEXT:  "file": "/@input/MyModule.swiftmodule",
// CHECK-SPI-EMIT-NEXT:     "file": "SOURCE_DIR/test/APIJSON/spi.swift",
// CHECK-SPI-NEXT:       "linkage": "exported"
// CHECK-SPI-NEXT:     },
// CHECK-SPI-NEXT:     {
// CHECK-SPI-NEXT:       "name": "_$s8MyModule0A6Class2CN",
// CHECK-SPI-NEXT:       "access": "public",
// CHECK-SPI-EXTRACT-NEXT:  "file": "/@input/MyModule.swiftmodule",
// CHECK-SPI-EMIT-NEXT:     "file": "SOURCE_DIR/test/APIJSON/spi.swift",
// CHECK-SPI-NEXT:       "linkage": "exported"
// CHECK-SPI-NEXT:     },
// CHECK-SPI-NEXT:     {
// CHECK-SPI-NEXT:       "name": "_$s8MyModule0A6Class2CfD",
// CHECK-SPI-NEXT:       "access": "public",
// CHECK-SPI-EXTRACT-NEXT:  "file": "/@input/MyModule.swiftmodule",
// CHECK-SPI-EMIT-NEXT:     "file": "SOURCE_DIR/test/APIJSON/spi.swift",
// CHECK-SPI-NEXT:       "linkage": "exported"
// CHECK-SPI-NEXT:     },
// CHECK-SPI-NEXT:     {
// CHECK-SPI-NEXT:       "name": "_$s8MyModule15newUnprovenFuncyyF",
// CHECK-SPI-NEXT:       "access": "private",
// CHECK-SPI-EXTRACT-NEXT:  "file": "/@input/MyModule.swiftmodule",
// CHECK-SPI-EMIT-NEXT:     "file": "SOURCE_DIR/test/APIJSON/spi.swift",
// CHECK-SPI-NEXT:       "linkage": "exported"
// CHECK-SPI-NEXT:     },
// CHECK-SPI-NEXT:    {
// CHECK-SPI-NEXT:       "name": "_$s8MyModule16spiAvailableFuncyyF",
// CHECK-SPI-NEXT:       "access": "private",
// CHECK-SPI-EXTRACT-NEXT:  "file": "/@input/MyModule.swiftmodule",
// CHECK-SPI-EMIT-NEXT:     "file": "SOURCE_DIR/test/APIJSON/spi.swift",
// CHECK-SPI-NEXT:       "linkage": "exported",
// CHECK-SPI-NEXT:       "introduced": "10.10"
// CHECK-SPI-NEXT:     }
// CHECK-SPI-NEXT:   ],
// CHECK-SPI-NEXT:   "interfaces": [
// CHECK-SPI-NEXT:     {
// CHECK-SPI-NEXT:       "name": "_TtC8MyModule7MyClass",
// CHECK-SPI-NEXT:       "access": "private",
// CHECK-SPI-EXTRACT-NEXT:  "file": "/@input/MyModule.swiftmodule",
// CHECK-SPI-EMIT-NEXT:     "file": "SOURCE_DIR/test/APIJSON/spi.swift",
// CHECK-SPI-NEXT:       "linkage": "exported",
// CHECK-SPI-NEXT:       "super": "NSObject",
// CHECK-SPI-NEXT:       "instanceMethods": [
// CHECK-SPI-NEXT:         {
// CHECK-SPI-NEXT:           "name": "spiMethod",
// CHECK-SPI-NEXT:           "access": "private",
// CHECK-SPI-EXTRACT-NEXT:   "file": "/@input/MyModule.swiftmodule"
// CHECK-SPI-EMIT-NEXT:      "file": "SOURCE_DIR/test/APIJSON/spi.swift"
// CHECK-SPI-NEXT:         },
// CHECK-SPI-NEXT:         {
// CHECK-SPI-NEXT:           "name": "init",
// CHECK-SPI-NEXT:           "access": "private",
// CHECK-SPI-EXTRACT-NEXT:   "file": "/@input/MyModule.swiftmodule"
// CHECK-SPI-EMIT-NEXT:      "file": "SOURCE_DIR/test/APIJSON/spi.swift"
// CHECK-SPI-NEXT:         }
// CHECK-SPI-NEXT:       ],
// CHECK-SPI-NEXT:       "classMethods": []
// CHECK-SPI-NEXT:     },
// CHECK-SPI-NEXT:     {
// CHECK-SPI-NEXT:       "name": "_TtC8MyModule8MyClass2",
// CHECK-SPI-NEXT:       "access": "public",
// CHECK-SPI-EXTRACT-NEXT:  "file": "/@input/MyModule.swiftmodule",
// CHECK-SPI-EMIT-NEXT:     "file": "SOURCE_DIR/test/APIJSON/spi.swift",
// CHECK-SPI-NEXT:       "linkage": "exported",
// CHECK-SPI-NEXT:       "super": "NSObject",
// CHECK-SPI-NEXT:       "instanceMethods": [
// CHECK-SPI-NEXT:         {
// CHECK-SPI-NEXT:           "name": "spiMethod",
// CHECK-SPI-NEXT:           "access": "private",
// CHECK-SPI-EXTRACT-NEXT:   "file": "/@input/MyModule.swiftmodule"
// CHECK-SPI-EMIT-NEXT:      "file": "SOURCE_DIR/test/APIJSON/spi.swift"
// CHECK-SPI-NEXT:         },
// CHECK-SPI-NEXT:         {
// CHECK-SPI-NEXT:           "name": "spiAvailableMethod",
// CHECK-SPI-NEXT:           "access": "private",
// CHECK-SPI-EXTRACT-NEXT:   "file": "/@input/MyModule.swiftmodule"
// CHECK-SPI-EMIT-NEXT:      "file": "SOURCE_DIR/test/APIJSON/spi.swift",
// CHECK-SPI-NEXT:           "introduced": "10.10"
// CHECK-SPI-NEXT:         },
// CHECK-SPI-NEXT:         {
// CHECK-SPI-NEXT:           "name": "init",
// CHECK-SPI-NEXT:           "access": "public",
// CHECK-SPI-EXTRACT-NEXT:   "file": "/@input/MyModule.swiftmodule"
// CHECK-SPI-EMIT-NEXT:      "file": "SOURCE_DIR/test/APIJSON/spi.swift"
// CHECK-SPI-NEXT:         }
// CHECK-SPI-NEXT:       ],
// CHECK-SPI-NEXT:       "classMethods": []
// CHECK-SPI-NEXT:     }
// CHECK-SPI-NEXT:   ],
 // CHECK-SPI-NEXT:  "categories": [],
// CHECK-SPI-NEXT:   "version": "1.0"
// CHECK-SPI-NEXT: }
