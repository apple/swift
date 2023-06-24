// REQUIRES: OS=macosx
// RUN: %empty-directory(%t)
// RUN: %empty-directory(%t/includes)
// RUN: echo "[myProto]" > %t/protocols.json

// Build external Swift library/module to also check conformances to external protocols
// RUN: %target-build-swift -target %target-cpu-apple-macosx10.15 %S/../Reflection/Inputs/swiftmodules/testModB.swift -parse-as-library -emit-module -emit-library -module-name testModB -o %t/includes/testModB.o

// RUN: %target-swift-frontend -target %target-cpu-apple-macosx10.15 -typecheck -emit-const-values-path %t/ExtractOpaqueTypealias.swiftconstvalues -const-gather-protocols-file %t/protocols.json -primary-file %s -I %t/includes
// RUN: cat %t/ExtractOpaqueTypealias.swiftconstvalues 2>&1 | %FileCheck %s

import testModB

public protocol myProto {
    associatedtype PerformReturn
    func perform() -> PerformReturn
}
public protocol protoA<T> {
    associatedtype T
}
public protocol protoB<K> {
    associatedtype K
}

public struct Bar<M, N> : protoA, protoB, testModBProtocol {
    public typealias T = M
    public typealias K = N
}

public struct Foo : myProto {
    public func perform() -> some protoA<testModBStruct> & protoB<Float> & testModBProtocol { return baz() }
}

private func baz() -> some protoA<testModBStruct> & protoB<Float> & testModBProtocol { return Bar<testModBStruct, Float>() }


// CHECK: [
// CHECK-NEXT:  {
// CHECK-NEXT:    "typeName": "ExtractOpaqueTypealias.Foo",
// CHECK-NEXT:    "mangledTypeName": "22ExtractOpaqueTypealias3FooV",
// CHECK-NEXT:    "kind": "struct",
// CHECK-NEXT:    "file": "{{.*}}test{{/|\\\\}}ConstExtraction{{/|\\\\}}ExtractOpaqueTypealias.swift",
// CHECK-NEXT:    "line": 30,
// CHECK-NEXT:    "conformances": [
// CHECK-NEXT:      "ExtractOpaqueTypealias.myProto"
// CHECK-NEXT:    ],
// CHECK-NEXT:    "associatedTypeAliases": [
// CHECK-NEXT:      {
// CHECK-NEXT:        "typeAliasName": "PerformReturn",
// CHECK-NEXT:        "substitutedTypeName": "some ExtractOpaqueTypealias.protoA<testModB.testModBStruct> & ExtractOpaqueTypealias.protoB<Swift.Float> & testModB.testModBProtocol",
// CHECK-NEXT:        "substitutedMangledTypeName": "22ExtractOpaqueTypealias3FooV7performQryFQOy_Qo_",
// CHECK-NEXT:        "opaqueTypeProtocolRequirements": [
// CHECK-NEXT:          "ExtractOpaqueTypealias.protoA",
// CHECK-NEXT:          "ExtractOpaqueTypealias.protoB",
// CHECK-NEXT:          "testModB.testModBProtocol"
// CHECK-NEXT:        ],
// CHECK-NEXT:        "opaqueTypeSameTypeRequirements": [
// CHECK-NEXT:          {
// CHECK-NEXT:            "typeAliasName": "ExtractOpaqueTypealias.protoA.T",
// CHECK-NEXT:            "substitutedTypeName": "testModB.testModBStruct",
// CHECK-NEXT:            "substitutedMangledTypeName": "8testModB0aB7BStructV"
// CHECK-NEXT:          },
// CHECK-NEXT:          {
// CHECK-NEXT:            "typeAliasName": "ExtractOpaqueTypealias.protoB.K",
// CHECK-NEXT:            "substitutedTypeName": "Swift.Float",
// CHECK-NEXT:            "substitutedMangledTypeName": "Sf"
// CHECK-NEXT:          }
// CHECK-NEXT:        ]
// CHECK-NEXT:      }
// CHECK-NEXT:    ],
// CHECK-NEXT:    "properties": []
// CHECK-NEXT:  }
// CHECK-NEXT:]
