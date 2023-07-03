// REQUIRES: OS=macosx
// RUN: %empty-directory(%t)
// RUN: %empty-directory(%t/includes)
// RUN: echo "[myProto]" > %t/protocols.json

// RUN: %target-swift-frontend -target %target-cpu-apple-macosx10.15 -typecheck -emit-const-values-path %t/ExtractOpaqueGenericTypealias.swiftconstvalues -const-gather-protocols-file %t/protocols.json -primary-file %s -I %t/includes
// RUN: cat %t/ExtractOpaqueGenericTypealias.swiftconstvalues 2>&1 | %FileCheck %s

protocol myProto {
    associatedtype T
    func foo() -> T
}

protocol protoA<T> {
    associatedtype T
}

struct A<K> : protoA {
    typealias T = K
}

struct Foo<L : Hashable> : myProto {
    func foo() -> some protoA<Int> { return A() }
    func bar(_ param : L) {}
}

// CHECK: [
// CHECK-NEXT:   {
// CHECK-NEXT:     "typeName": "ExtractOpaqueGenericTypealias.Foo<L>",
// CHECK-NEXT:     "mangledTypeName": "29ExtractOpaqueGenericTypealias3FooVyxG",
// CHECK-NEXT:     "kind": "generic struct",
// CHECK-NEXT:     "file": "{{.*}}test{{/|\\\\}}ConstExtraction{{/|\\\\}}ExtractOpaqueGenericTypealias.swift",
// CHECK-NEXT:     "line": 22,
// CHECK-NEXT:     "conformances": [
// CHECK-NEXT:       "ExtractOpaqueGenericTypealias.myProto",
// CHECK-NEXT:       "Swift.Sendable"
// CHECK-NEXT:     ],
// CHECK-NEXT:     "associatedTypeAliases": [
// CHECK-NEXT:       {
// CHECK-NEXT:         "typeAliasName": "T",
// CHECK-NEXT:         "substitutedTypeName": "some ExtractOpaqueGenericTypealias.protoA<Swift.Int>",
// CHECK-NEXT:         "substitutedMangledTypeName": "29ExtractOpaqueGenericTypealias3FooV3fooQryFQOyx_Qo_",
// CHECK-NEXT:         "opaqueTypeProtocolRequirements": [
// CHECK-NEXT:           "ExtractOpaqueGenericTypealias.protoA"
// CHECK-NEXT:         ],
// CHECK-NEXT:         "opaqueTypeSameTypeRequirements": [
// CHECK-NEXT:           {
// CHECK-NEXT:             "typeAliasName": "ExtractOpaqueGenericTypealias.protoA.T",
// CHECK-NEXT:             "substitutedTypeName": "Swift.Int",
// CHECK-NEXT:             "substitutedMangledTypeName": "Si"
// CHECK-NEXT:           }
// CHECK-NEXT:         ]
// CHECK-NEXT:       }
// CHECK-NEXT:     ],
// CHECK-NEXT:     "properties": []
// CHECK-NEXT:   }
// CHECK-NEXT: ]
