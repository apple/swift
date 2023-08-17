// RUN: %empty-directory(%t)
// RUN: echo "[MyProto]" > %t/protocols.json

// RUN: %target-swift-frontend -typecheck -emit-const-values-path %t/ExtractCalls.swiftconstvalues -const-gather-protocols-file %t/protocols.json -primary-file %s
// RUN: cat %t/ExtractCalls.swiftconstvalues 2>&1 | %FileCheck %s

protocol MyProto {}

public struct Foo : MyProto {
    let init1 = Bar()
    let init2: Bat = .init()
    let init3 = Bat(buz: "hello", fuz: adder(2, 3))
    static var init4: Bar? = Bar()

    let func1: Int = adder(2, 3)
}

extension Foo {
    struct Boo {}

    var ext1: Boo { Boo() }
}

func adder(_ x: Int, _ y: Int) -> Int {
    x + y
}

public struct Bar {}
public struct Bat {
    let buz: String
    let fuz: Int

    init(buz: String = "", fuz: Int = 0) {
        self.buz = buz
        self.fuz = fuz
    }
}

// CHECK: [
// CHECK-NEXT:  {
// CHECK-NEXT:    "typeName": "ExtractCalls.Foo",
// CHECK-NEXT:    "mangledTypeName": "12ExtractCalls3FooV",
// CHECK-NEXT:    "kind": "struct",
// CHECK-NEXT:    "file": "{{.*}}test{{/|\\\\}}ConstExtraction{{/|\\\\}}ExtractCalls.swift",
// CHECK-NEXT:    "line": 9,
// CHECK-NEXT:    "conformances": [
// CHECK-NEXT:      "ExtractCalls.MyProto"
// CHECK-NEXT:    ],
// CHECK-NEXT:    "associatedTypeAliases": [],
// CHECK-NEXT:    "properties": [
// CHECK-NEXT:      {
// CHECK-NEXT:        "label": "init1",
// CHECK-NEXT:        "type": "ExtractCalls.Bar",
// CHECK-NEXT:        "mangledTypeName": "n/a - deprecated",
// CHECK-NEXT:        "isStatic": "false",
// CHECK-NEXT:        "isComputed": "false",
// CHECK-NEXT:        "file": "{{.*}}test{{/|\\\\}}ConstExtraction{{/|\\\\}}ExtractCalls.swift",
// CHECK-NEXT:        "line": 10,
// CHECK-NEXT:        "valueKind": "InitCall",
// CHECK-NEXT:        "value": {
// CHECK-NEXT:          "type": "ExtractCalls.Bar",
// CHECK-NEXT:          "arguments": []
// CHECK-NEXT:        }
// CHECK-NEXT:      },
// CHECK-NEXT:      {
// CHECK-NEXT:        "label": "init2",
// CHECK-NEXT:        "type": "ExtractCalls.Bat",
// CHECK-NEXT:        "mangledTypeName": "n/a - deprecated",        
// CHECK-NEXT:        "isStatic": "false",
// CHECK-NEXT:        "isComputed": "false",
// CHECK-NEXT:        "file": "{{.*}}test{{/|\\\\}}ConstExtraction{{/|\\\\}}ExtractCalls.swift",
// CHECK-NEXT:        "line": 11,
// CHECK-NEXT:        "valueKind": "InitCall",
// CHECK-NEXT:        "value": {
// CHECK-NEXT:          "type": "ExtractCalls.Bat",
// CHECK-NEXT:          "arguments": [
// CHECK-NEXT:            {
// CHECK-NEXT:              "label": "buz",
// CHECK-NEXT:              "type": "Swift.String",
// CHECK-NEXT:              "valueKind": "RawLiteral",
// CHECK-NEXT:              "value": ""
// CHECK-NEXT:            },
// CHECK-NEXT:            {
// CHECK-NEXT:              "label": "fuz",
// CHECK-NEXT:              "type": "Swift.Int",
// CHECK-NEXT:              "valueKind": "RawLiteral",
// CHECK-NEXT:              "value": "0"
// CHECK-NEXT:            }
// CHECK-NEXT:          ]
// CHECK-NEXT:        }
// CHECK-NEXT:      },
// CHECK-NEXT:      {
// CHECK-NEXT:        "label": "init3",
// CHECK-NEXT:        "type": "ExtractCalls.Bat",
// CHECK-NEXT:        "mangledTypeName": "n/a - deprecated",        
// CHECK-NEXT:        "isStatic": "false",
// CHECK-NEXT:        "isComputed": "false",
// CHECK-NEXT:        "file": "{{.*}}test{{/|\\\\}}ConstExtraction{{/|\\\\}}ExtractCalls.swift",
// CHECK-NEXT:        "line": 12,
// CHECK-NEXT:        "valueKind": "InitCall",
// CHECK-NEXT:        "value": {
// CHECK-NEXT:          "type": "ExtractCalls.Bat",
// CHECK-NEXT:          "arguments": [
// CHECK-NEXT:            {
// CHECK-NEXT:              "label": "buz",
// CHECK-NEXT:              "type": "Swift.String",
// CHECK-NEXT:              "valueKind": "RawLiteral",
// CHECK-NEXT:              "value": "hello"
// CHECK-NEXT:            },
// CHECK-NEXT:            {
// CHECK-NEXT:              "label": "fuz",
// CHECK-NEXT:              "type": "Swift.Int",
// CHECK-NEXT:              "valueKind": "Runtime"
// CHECK-NEXT:            }
// CHECK-NEXT:          ]
// CHECK-NEXT:        }
// CHECK-NEXT:      },
// CHECK-NEXT:      {
// CHECK-NEXT:        "label": "func1",
// CHECK-NEXT:        "type": "Swift.Int",
// CHECK-NEXT:        "mangledTypeName": "n/a - deprecated",        
// CHECK-NEXT:        "isStatic": "false",
// CHECK-NEXT:        "isComputed": "false",
// CHECK-NEXT:        "file": "{{.*}}test{{/|\\\\}}ConstExtraction{{/|\\\\}}ExtractCalls.swift",
// CHECK-NEXT:        "line": 15,
// CHECK-NEXT:        "valueKind": "Runtime"
// CHECK-NEXT:      },
// CHECK-NEXT:      {
// CHECK-NEXT:        "label": "init4",
// CHECK-NEXT:        "type": "Swift.Optional<ExtractCalls.Bar>",
// CHECK-NEXT:        "mangledTypeName": "n/a - deprecated",
// CHECK-NEXT:        "isStatic": "true",
// CHECK-NEXT:        "isComputed": "false",
// CHECK-NEXT:        "file": "{{.*}}test{{/|\\\\}}ConstExtraction{{/|\\\\}}ExtractCalls.swift",
// CHECK-NEXT:        "line": 13,
// CHECK-NEXT:        "valueKind": "InitCall",
// CHECK-NEXT:        "value": {
// CHECK-NEXT:          "type": "ExtractCalls.Bar",
// CHECK-NEXT:          "arguments": []
// CHECK-NEXT:        }
// CHECK-NEXT:      },
// CHECK-NEXT:      {
// CHECK-NEXT:        "label": "ext1",
// CHECK-NEXT:        "type": "ExtractCalls.Foo.Boo",
// CHECK-NEXT:        "mangledTypeName": "n/a - deprecated",        
// CHECK-NEXT:        "isStatic": "false",
// CHECK-NEXT:        "isComputed": "true",
// CHECK-NEXT:        "file": "{{.*}}test{{/|\\\\}}ConstExtraction{{/|\\\\}}ExtractCalls.swift",
// CHECK-NEXT:        "line": 21,
// CHECK-NEXT:        "valueKind": "InitCall",
// CHECK-NEXT:        "value": {
// CHECK-NEXT:          "type": "ExtractCalls.Foo.Boo",
// CHECK-NEXT:          "arguments": []
// CHECK-NEXT:        }
// CHECK-NEXT:      }
// CHECK-NEXT:    ]
// CHECK-NEXT:  }
// CHECK-NEXT:]
