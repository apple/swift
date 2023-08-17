// RUN: %empty-directory(%t)
// RUN: echo "[MyProto]" > %t/protocols.json

// RUN: %target-swift-frontend -typecheck -emit-const-values-path %t/ExtractEnums.swiftconstvalues -const-gather-protocols-file %t/protocols.json -primary-file %s
// RUN: cat %t/ExtractEnums.swiftconstvalues 2>&1 | %FileCheck %s

protocol MyProto {}

public struct Foo {
    init(bar: Any) {
    }
}

public struct ArchetypalConformance<T>: MyProto {
    let baz: Foo = Foo(bar: T.self)
    public init() {}
}

// CHECK: [
// CHECK-NEXT:  {
// CHECK-NEXT:    "typeName": "ExtractArchetype.ArchetypalConformance<T>"
// CHECK:        "valueKind": "InitCall",
// CHECK-NEXT:        "value": {
// CHECK-NEXT:          "type": "ExtractArchetype.Foo",
// CHECK-NEXT:          "arguments": [
// CHECK-NEXT:            {
// CHECK-NEXT:              "label": "bar",
// CHECK-NEXT:              "type": "Any",
// CHECK-NEXT:              "valueKind": "Type",
// CHECK-NEXT:              "value": {
// CHECK-NEXT:                "type": "T",
// CHECK-NEXT:                "mangledName": "x"
// CHECK-NEXT:              }
// CHECK-NEXT:            }
// CHECK-NEXT:          ]
// CHECK-NEXT:        }
