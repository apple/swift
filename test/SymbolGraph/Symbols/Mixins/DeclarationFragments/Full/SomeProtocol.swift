// RUN: %empty-directory(%t)
// RUN: %target-build-swift %s -module-name SomeProtocol -emit-module -emit-module-path %t/
// RUN: %target-swift-symbolgraph-extract -module-name SomeProtocol -I %t -pretty-print -output-dir %t
// RUN: %FileCheck %s --input-file %t/SomeProtocol.symbols.json
// RUN: %FileCheck %s --input-file %t/SomeProtocol.symbols.json --check-prefix MULTI

// RUN: %empty-directory(%t)
// RUN: %target-build-swift %s -module-name SomeProtocol -emit-module -emit-module-path %t/SomeProtocol.swiftmodule -emit-symbol-graph -emit-symbol-graph-dir %t/
// RUN: %validate-json %t/SomeProtocol.symbols.json %t/SomeProtocol.formatted.symbols.json
// RUN: %FileCheck %s --input-file %t/SomeProtocol.formatted.symbols.json
// RUN: %FileCheck %s --input-file %t/SomeProtocol.formatted.symbols.json --check-prefix MULTI

// Make sure that `some MyProtocol` parameters don't crash swift-symbolgraph-extract, and that it
// properly renders `some MyProtocol & OtherProtocol` parameters with multiple protocols listed.

public protocol SomeProtocol {}

public func doSomething(with param: some SomeProtocol) {}

public protocol OtherProtocol {}

public func doSomethingElse(with param: some SomeProtocol & OtherProtocol) {}

// CHECK-LABEL:        "precise": "s:12SomeProtocol11doSomething4withyx_tA2ARzlF",

// the functionSignature fragments come before the full fragments, so skip those
// CHECK:      "functionSignature": {
// CHECK:      "declarationFragments": [

// CHECK:      "declarationFragments": [
// CHECK-NEXT:        {
// CHECK-NEXT:          "kind": "keyword",
// CHECK-NEXT:          "spelling": "func"
// CHECK-NEXT:        },
// CHECK-NEXT:        {
// CHECK-NEXT:          "kind": "text",
// CHECK-NEXT:          "spelling": " "
// CHECK-NEXT:        },
// CHECK-NEXT:        {
// CHECK-NEXT:          "kind": "identifier",
// CHECK-NEXT:          "spelling": "doSomething"
// CHECK-NEXT:        },
// CHECK-NEXT:        {
// CHECK-NEXT:          "kind": "text",
// CHECK-NEXT:          "spelling": "("
// CHECK-NEXT:        },
// CHECK-NEXT:        {
// CHECK-NEXT:          "kind": "externalParam",
// CHECK-NEXT:          "spelling": "with"
// CHECK-NEXT:        },
// CHECK-NEXT:        {
// CHECK-NEXT:          "kind": "text",
// CHECK-NEXT:          "spelling": " "
// CHECK-NEXT:        },
// CHECK-NEXT:        {
// CHECK-NEXT:          "kind": "internalParam",
// CHECK-NEXT:          "spelling": "param"
// CHECK-NEXT:        },
// CHECK-NEXT:        {
// CHECK-NEXT:          "kind": "text",
// CHECK-NEXT:          "spelling": ": some "
// CHECK-NEXT:        },
// CHECK-NEXT:        {
// CHECK-NEXT:          "kind": "typeIdentifier",
// CHECK-NEXT:          "spelling": "SomeProtocol",
// CHECK-NEXT:          "preciseIdentifier": "s:12SomeProtocolAAP"
// CHECK-NEXT:        },
// CHECK-NEXT:        {
// CHECK-NEXT:          "kind": "text",
// CHECK-NEXT:          "spelling": ")"
// CHECK-NEXT:        }
// CHECK-NEXT:      ],

// MULTI-LABEL:        "precise": "s:12SomeProtocol15doSomethingElse4withyx_tAA05OtherB0RzA2ARzlF",

// the functionSignature fragments come before the full fragments, so skip those
// MULTI:      "functionSignature": {
// MULTI:      "declarationFragments": [

// MULTI:      "declarationFragments": [
// MULTI-NEXT:        {
// MULTI-NEXT:          "kind": "keyword",
// MULTI-NEXT:          "spelling": "func"
// MULTI-NEXT:        },
// MULTI-NEXT:        {
// MULTI-NEXT:          "kind": "text",
// MULTI-NEXT:          "spelling": " "
// MULTI-NEXT:        },
// MULTI-NEXT:        {
// MULTI-NEXT:          "kind": "identifier",
// MULTI-NEXT:          "spelling": "doSomethingElse"
// MULTI-NEXT:        },
// MULTI-NEXT:        {
// MULTI-NEXT:          "kind": "text",
// MULTI-NEXT:          "spelling": "("
// MULTI-NEXT:        },
// MULTI-NEXT:        {
// MULTI-NEXT:          "kind": "externalParam",
// MULTI-NEXT:          "spelling": "with"
// MULTI-NEXT:        },
// MULTI-NEXT:        {
// MULTI-NEXT:          "kind": "text",
// MULTI-NEXT:          "spelling": " "
// MULTI-NEXT:        },
// MULTI-NEXT:        {
// MULTI-NEXT:          "kind": "internalParam",
// MULTI-NEXT:          "spelling": "param"
// MULTI-NEXT:        },
// MULTI-NEXT:        {
// MULTI-NEXT:          "kind": "text",
// MULTI-NEXT:          "spelling": ": some "
// MULTI-NEXT:        },
// MULTI-NEXT:        {
// MULTI-NEXT:          "kind": "typeIdentifier",
// MULTI-NEXT:          "spelling": "OtherProtocol",
// MULTI-NEXT:          "preciseIdentifier": "s:12SomeProtocol05OtherB0P"
// MULTI-NEXT:        },
// MULTI-NEXT:        {
// MULTI-NEXT:          "kind": "text",
// MULTI-NEXT:          "spelling": " & "
// MULTI-NEXT:        },
// MULTI-NEXT:        {
// MULTI-NEXT:          "kind": "typeIdentifier",
// MULTI-NEXT:          "spelling": "SomeProtocol",
// MULTI-NEXT:          "preciseIdentifier": "s:12SomeProtocolAAP"
// MULTI-NEXT:        },
// MULTI-NEXT:        {
// MULTI-NEXT:          "kind": "text",
// MULTI-NEXT:          "spelling": ")"
// MULTI-NEXT:        }
// MULTI-NEXT:      ],
