// '-enable-experimental-feature Macros' requires an asserts build.
// REQUIRES: swift_swift_parser, asserts

// RUN: %empty-directory(%t)
// RUN: %host-build-swift -swift-version 5 -emit-library -o %t/%target-library-name(MacroDefinition) -module-name=MacroDefinition %S/Inputs/stringify_macro.swift -g -no-toolchain-stdlib-rpath -swift-version 5
// RUN: %target-swift-frontend -load-plugin-library %t/%target-library-name(MacroDefinition) %s -module-name Macros -emit-module -emit-module-path %t/Macros.swiftmodule -emit-symbol-graph -emit-symbol-graph-dir %t/
// RUN: %{python} -m json.tool %t/Macros.symbols.json %t/Macros.formatted.symbols.json

// Make sure that the `= #externalMacro(...)` doesn't show up in declaration fragments and in names fragments.

// RUN: %FileCheck %s --input-file %t/Macros.formatted.symbols.json
// RUN: %FileCheck %s --input-file %t/Macros.formatted.symbols.json --check-prefix NAMES


@freestanding(expression) public macro stringify<T>(_ value: T) -> (T, String) = #externalMacro(module: "MacroDefinition", type: "StringifyMacro")

// CHECK:      "declarationFragments": [
// CHECK-NEXT:     {
// CHECK-NEXT:         "kind": "attribute",
// CHECK-NEXT:         "spelling": "@freestanding"
// CHECK-NEXT:     },
// CHECK-NEXT:     {
// CHECK-NEXT:         "kind": "text",
// CHECK-NEXT:         "spelling": "(expression) "
// CHECK-NEXT:     },
// CHECK-NEXT:     {
// CHECK-NEXT:         "kind": "keyword",
// CHECK-NEXT:         "spelling": "macro"
// CHECK-NEXT:     },
// CHECK-NEXT:     {
// CHECK-NEXT:         "kind": "text",
// CHECK-NEXT:         "spelling": " "
// CHECK-NEXT:     },
// CHECK-NEXT:     {
// CHECK-NEXT:         "kind": "identifier",
// CHECK-NEXT:         "spelling": "stringify"
// CHECK-NEXT:     },
// CHECK-NEXT:     {
// CHECK-NEXT:         "kind": "text",
// CHECK-NEXT:         "spelling": "<"
// CHECK-NEXT:     },
// CHECK-NEXT:     {
// CHECK-NEXT:         "kind": "genericParameter",
// CHECK-NEXT:         "spelling": "T"
// CHECK-NEXT:     },
// CHECK-NEXT:     {
// CHECK-NEXT:         "kind": "text",
// CHECK-NEXT:         "spelling": ">("
// CHECK-NEXT:     },
// CHECK-NEXT:     {
// CHECK-NEXT:         "kind": "externalParam",
// CHECK-NEXT:         "spelling": "_"
// CHECK-NEXT:     },
// CHECK-NEXT:     {
// CHECK-NEXT:         "kind": "text",
// CHECK-NEXT:         "spelling": " "
// CHECK-NEXT:     },
// CHECK-NEXT:     {
// CHECK-NEXT:         "kind": "internalParam",
// CHECK-NEXT:         "spelling": "value"
// CHECK-NEXT:     },
// CHECK-NEXT:     {
// CHECK-NEXT:         "kind": "text",
// CHECK-NEXT:         "spelling": ": "
// CHECK-NEXT:     },
// CHECK-NEXT:     {
// CHECK-NEXT:         "kind": "typeIdentifier",
// CHECK-NEXT:         "spelling": "T",
// CHECK-NEXT:         "preciseIdentifier": "s:6Macros1TL_xmfp"
// CHECK-NEXT:     },
// CHECK-NEXT:     {
// CHECK-NEXT:         "kind": "text",
// CHECK-NEXT:         "spelling": ") -> ("
// CHECK-NEXT:     },
// CHECK-NEXT:     {
// CHECK-NEXT:         "kind": "typeIdentifier",
// CHECK-NEXT:         "spelling": "T",
// CHECK-NEXT:         "preciseIdentifier": "s:6Macros1TL_xmfp"
// CHECK-NEXT:     },
// CHECK-NEXT:     {
// CHECK-NEXT:         "kind": "text",
// CHECK-NEXT:         "spelling": ", "
// CHECK-NEXT:     },
// CHECK-NEXT:     {
// CHECK-NEXT:         "kind": "typeIdentifier",
// CHECK-NEXT:         "spelling": "String",
// CHECK-NEXT:         "preciseIdentifier": "s:SS"
// CHECK-NEXT:     },
// CHECK-NEXT:     {
// CHECK-NEXT:         "kind": "text",
// CHECK-NEXT:         "spelling": ")"
// CHECK-NEXT:     }
// CHECK-NEXT: ],

// NAMES:      "names": {
// NAMES-NEXT:     "title": "stringify(_:)",
// NAMES-NEXT:     "subHeading": [
// NAMES-NEXT:         {
// NAMES-NEXT:             "kind": "keyword",
// NAMES-NEXT:             "spelling": "macro"
// NAMES-NEXT:         },
// NAMES-NEXT:         {
// NAMES-NEXT:             "kind": "text",
// NAMES-NEXT:             "spelling": " "
// NAMES-NEXT:         },
// NAMES-NEXT:         {
// NAMES-NEXT:             "kind": "identifier",
// NAMES-NEXT:             "spelling": "stringify"
// NAMES-NEXT:         },
// NAMES-NEXT:         {
// NAMES-NEXT:             "kind": "text",
// NAMES-NEXT:             "spelling": "<"
// NAMES-NEXT:         },
// NAMES-NEXT:         {
// NAMES-NEXT:             "kind": "genericParameter",
// NAMES-NEXT:             "spelling": "T"
// NAMES-NEXT:         },
// NAMES-NEXT:         {
// NAMES-NEXT:             "kind": "text",
// NAMES-NEXT:             "spelling": ">("
// NAMES-NEXT:         },
// NAMES-NEXT:         {
// NAMES-NEXT:             "kind": "typeIdentifier",
// NAMES-NEXT:             "spelling": "T",
// NAMES-NEXT:             "preciseIdentifier": "s:6Macros1TL_xmfp"
// NAMES-NEXT:         },
// NAMES-NEXT:         {
// NAMES-NEXT:             "kind": "text",
// NAMES-NEXT:             "spelling": ") -> ("
// NAMES-NEXT:         },
// NAMES-NEXT:         {
// NAMES-NEXT:             "kind": "typeIdentifier",
// NAMES-NEXT:             "spelling": "T",
// NAMES-NEXT:             "preciseIdentifier": "s:6Macros1TL_xmfp"
// NAMES-NEXT:         },
// NAMES-NEXT:         {
// NAMES-NEXT:             "kind": "text",
// NAMES-NEXT:             "spelling": ", "
// NAMES-NEXT:         },
// NAMES-NEXT:         {
// NAMES-NEXT:             "kind": "typeIdentifier",
// NAMES-NEXT:             "spelling": "String",
// NAMES-NEXT:             "preciseIdentifier": "s:SS"
// NAMES-NEXT:         },
// NAMES-NEXT:         {
// NAMES-NEXT:             "kind": "text",
// NAMES-NEXT:             "spelling": ")"
// NAMES-NEXT:         }
// NAMES-NEXT:     ]
// NAMES-NEXT: },
