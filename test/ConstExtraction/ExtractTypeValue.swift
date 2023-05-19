// RUN: %empty-directory(%t)
// RUN: echo "[MyProto]" > %t/protocols.json

// RUN: %target-swift-frontend -typecheck -emit-const-values-path %t/ExtractLiterals.swiftconstvalues -const-gather-protocols-file %t/protocols.json -primary-file %s
// RUN: cat %t/ExtractLiterals.swiftconstvalues 2>&1 | %FileCheck %s

protocol MyProto {}

protocol Bird {}
struct Warbler<T> : Bird {}
struct Avocet : Bird {}
struct RainbowLorikeet : Bird {}

struct TypeValuePropertyStruct : MyProto {
    var birdTypes: [any Bird.Type] = [Warbler<String>.self, Avocet.self, RainbowLorikeet.self]
}

// CHECK:             "label": "birdTypes",
// CHECK-NEXT:        "type": "Swift.Array<any ExtractTypeValue.Bird.Type>",
// CHECK-NEXT:        "isStatic": "false",
// CHECK-NEXT:        "isComputed": "false",
// CHECK-NEXT:        "file": "{{.*}}ExtractTypeValue.swift",
// CHECK-NEXT:        "line": 15,
// CHECK-NEXT:        "valueKind": "Array",
// CHECK-NEXT:        "value": [
// CHECK-NEXT:          {
// CHECK-NEXT:            "valueKind": "Type",
// CHECK-NEXT:            "value": {
// CHECK-NEXT:              "type": "ExtractTypeValue.Warbler<Swift.String>",
// CHECK-NEXT:              "mangledName": "16ExtractTypeValue7WarblerVySSG"
// CHECK-NEXT:            }
// CHECK-NEXT:          },
// CHECK-NEXT:          {
// CHECK-NEXT:            "valueKind": "Type",
// CHECK-NEXT:            "value": {
// CHECK-NEXT:              "type": "ExtractTypeValue.Avocet",
// CHECK-NEXT:              "mangledName": "16ExtractTypeValue6AvocetV"
// CHECK-NEXT:            }
// CHECK-NEXT:          },
// CHECK-NEXT:          {
// CHECK-NEXT:            "valueKind": "Type",
// CHECK-NEXT:            "value": {
// CHECK-NEXT:              "type": "ExtractTypeValue.RainbowLorikeet",
// CHECK-NEXT:              "mangledName": "16ExtractTypeValue15RainbowLorikeetV"
// CHECK-NEXT:            }
// CHECK-NEXT:          }
// CHECK-NEXT:        ]
