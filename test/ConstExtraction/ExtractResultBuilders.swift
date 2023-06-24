// RUN: %empty-directory(%t)
// RUN: %empty-directory(%t/inputs)
// RUN: echo "[FooProvider]" > %t/inputs/protocols.json

// RUN: %target-swift-frontend -typecheck -emit-const-values-path %t/ExtractResultBuilders.swiftconstvalues -const-gather-protocols-file %t/inputs/protocols.json -primary-file %s
// RUN: cat %t/ExtractResultBuilders.swiftconstvalues 2>&1 | %FileCheck %s

public struct Foo {
    let name: String
}

@resultBuilder
public enum FooBuilder {
    public static func buildExpression(_ component: Foo) -> Foo {
        component
    }

    public static func buildBlock(_ components: Foo...) -> [Foo] {
        components
    }
}

public protocol FooProvider {
    @FooBuilder
    static var foos: [Foo] { get }
}

public struct MyFooProvider: FooProvider {
    @FooBuilder
    public static var foos: [Foo] {
        Foo(name: "AAA")
        Foo(name: "BBB")
    }

    @FooBuilder
    public static var fooTwo: [Foo] {
        Foo(name: "111")
        Foo(name: "222")
    }
}

public struct MyFooProviderInferred: FooProvider {
    public static var foos: [Foo] {
        Foo(name: "CCC")
        Foo(name: "DDD")
    }
}

// CHECK: [
// CHECK-NEXT:    {
// CHECK-NEXT:      "typeName": "ExtractResultBuilders.MyFooProvider",
// CHECK-NEXT:      "mangledTypeName": "21ExtractResultBuilders13MyFooProviderV",
// CHECK-NEXT:      "kind": "struct",
// CHECK-NEXT:      "file": "{{.*}}test{{/|\\\\}}ConstExtraction{{/|\\\\}}ExtractResultBuilders.swift",
// CHECK-NEXT:      "line": 28,
// CHECK-NEXT:    "conformances": [
// CHECK-NEXT:      "ExtractResultBuilders.FooProvider"
// CHECK-NEXT:    ],
// CHECK-NEXT:    "associatedTypeAliases": [],
// CHECK-NEXT:      "properties": [
// CHECK-NEXT:        {
// CHECK-NEXT:          "label": "foos",
// CHECK-NEXT:          "type": "Swift.Array<ExtractResultBuilders.Foo>",
// CHECK-NEXT:          "mangledTypeName": "Say21ExtractResultBuilders3FooVG",
// CHECK-NEXT:          "isStatic": "true",
// CHECK-NEXT:          "isComputed": "true",
// CHECK-NEXT:          "file": "{{.*}}test{{/|\\\\}}ConstExtraction{{/|\\\\}}ExtractResultBuilders.swift",
// CHECK-NEXT:          "line": 30,
// CHECK-NEXT:          "valueKind": "Runtime",
// CHECK-NEXT:          "resultBuilder": {
// CHECK-NEXT:            "type": "ExtractResultBuilders.FooBuilder"
// CHECK-NEXT:          }
// CHECK-NEXT:        },
// CHECK-NEXT:        {
// CHECK-NEXT:          "label": "fooTwo",
// CHECK-NEXT:          "type": "Swift.Array<ExtractResultBuilders.Foo>",
// CHECK-NEXT:          "mangledTypeName": "Say21ExtractResultBuilders3FooVG",
// CHECK-NEXT:          "isStatic": "true",
// CHECK-NEXT:          "isComputed": "true",
// CHECK-NEXT:          "file": "{{.*}}test{{/|\\\\}}ConstExtraction{{/|\\\\}}ExtractResultBuilders.swift",
// CHECK-NEXT:          "line": 36,
// CHECK-NEXT:          "valueKind": "Runtime",
// CHECK-NEXT:          "resultBuilder": {
// CHECK-NEXT:            "type": "ExtractResultBuilders.FooBuilder"
// CHECK-NEXT:          }
// CHECK-NEXT:        }
// CHECK-NEXT:      ]
// CHECK-NEXT:    },
// CHECK-NEXT:    {
// CHECK-NEXT:      "typeName": "ExtractResultBuilders.MyFooProviderInferred",
// CHECK-NEXT:      "mangledTypeName": "21ExtractResultBuilders21MyFooProviderInferredV",
// CHECK-NEXT:      "kind": "struct",
// CHECK-NEXT:      "file": "{{.*}}test{{/|\\\\}}ConstExtraction{{/|\\\\}}ExtractResultBuilders.swift",
// CHECK-NEXT:      "line": 42,
// CHECK-NEXT:    "conformances": [
// CHECK-NEXT:      "ExtractResultBuilders.FooProvider"
// CHECK-NEXT:    ],
// CHECK-NEXT:    "associatedTypeAliases": [],
// CHECK-NEXT:      "properties": [
// CHECK-NEXT:        {
// CHECK-NEXT:          "label": "foos",
// CHECK-NEXT:          "type": "Swift.Array<ExtractResultBuilders.Foo>",
// CHECK-NEXT:          "mangledTypeName": "Say21ExtractResultBuilders3FooVG",
// CHECK-NEXT:          "isStatic": "true",
// CHECK-NEXT:          "isComputed": "true",
// CHECK-NEXT:          "file": "{{.*}}test{{/|\\\\}}ConstExtraction{{/|\\\\}}ExtractResultBuilders.swift",
// CHECK-NEXT:          "line": 43,
// CHECK-NEXT:          "valueKind": "Runtime",
// CHECK-NEXT:          "resultBuilder": {
// CHECK-NEXT:            "type": "ExtractResultBuilders.FooBuilder"
// CHECK-NEXT:          }
// CHECK-NEXT:        }
// CHECK-NEXT:      ]
// CHECK-NEXT:    }
// CHECK-NEXT:  ]
