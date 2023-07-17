// RUN: %empty-directory(%t)
// RUN: cp -r %S/Inputs/EmitWhileBuilding/EmitWhileBuilding.framework %t
// RUN: %target-swift-frontend(mock-sdk: %clang-importer-sdk) -enable-objc-interop -emit-module-path %t/EmitWhileBuilding.framework/Modules/EmitWhileBuilding.swiftmodule/%target-swiftmodule-name -import-underlying-module -F %t -module-name EmitWhileBuilding -disable-objc-attr-requires-foundation-module %s %S/Inputs/EmitWhileBuilding/Extra.swift -emit-symbol-graph -emit-symbol-graph-dir %t
// RUN: %validate-json %t/EmitWhileBuilding.symbols.json %t/EmitWhileBuilding.formatted.symbols.json
// RUN: %FileCheck %s --input-file %t/EmitWhileBuilding.formatted.symbols.json
// RUN: %FileCheck %s --input-file %t/EmitWhileBuilding.formatted.symbols.json --check-prefix HEADER
// RUN: %FileCheck %s --input-file %t/EmitWhileBuilding.formatted.symbols.json --check-prefix LOCATION

// REQUIRES: objc_interop

import Foundation

public enum SwiftEnum {}

// HEADER:       "precise": "c:@testVariable"

// CHECK:        "precise": "s:17EmitWhileBuilding9SwiftEnumO",
// CHECK:        "declarationFragments": [
// CHECK-NEXT:       {
// CHECK-NEXT:           "kind": "keyword",
// CHECK-NEXT:           "spelling": "enum"
// CHECK-NEXT:       },
// CHECK-NEXT:       {
// CHECK-NEXT:           "kind": "text",
// CHECK-NEXT:           "spelling": " "
// CHECK-NEXT:       },
// CHECK-NEXT:       {
// CHECK-NEXT:           "kind": "identifier",
// CHECK-NEXT:           "spelling": "SwiftEnum"
// CHECK-NEXT:       }
// CHECK-NEXT:   ],

// ensure that the only nodes with a "location" field are the ones that came from Swift
// LOCATION-COUNT-2: "location":
