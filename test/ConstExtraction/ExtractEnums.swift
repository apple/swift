// RUN: %empty-directory(%t)
// RUN: echo "[MyProto]" > %t/protocols.json

// RUN: %target-swift-frontend -typecheck -emit-const-values-path %t/ExtractEnums.swiftconstvalues -const-gather-protocols-file %t/protocols.json -primary-file %s
// RUN: cat %t/ExtractEnums.swiftconstvalues 2>&1 | %FileCheck %s

protocol MyProto {}

public enum SimpleEnum: MyProto {
    case hello
    case world
}

public enum StringEnum: String, MyProto {
    case first, second
    case third = "bronzeMedal"
}

public enum AssociatedEnums: MyProto {
    case strings(title: String, subtitle: String)
    case ints(Int)
}

public struct Enums: MyProto {
  let enum1 = SimpleEnum.hello
  var enum2: StringEnum { .third }
  let enum3 = AssociatedEnums.strings(title: "the_title", subtitle: "the_subtitle")
  var enum4: AssociatedEnums { return .ints(42) }
}

// CHECK: [
// CHECK-NEXT:  {
// CHECK-NEXT:    "typeName": "ExtractEnums.SimpleEnum",
// CHECK-NEXT:    "mangledTypeName": "12ExtractEnums10SimpleEnumO",
// CHECK-NEXT:    "kind": "enum",
// CHECK-NEXT:    "file": "{{.*}}test{{/|\\\\}}ConstExtraction{{/|\\\\}}ExtractEnums.swift",
// CHECK-NEXT:    "line": 9,
// CHECK-NEXT:    "conformances": [
// CHECK-NEXT:      "Swift.Equatable",
// CHECK-NEXT:      "Swift.Hashable",
// CHECK-NEXT:      "ExtractEnums.MyProto"
// CHECK-NEXT:    ],
// CHECK-NEXT:    "associatedTypeAliases": [],
// CHECK-NEXT:    "properties": [
// CHECK-NEXT:      {
// CHECK-NEXT:        "label": "hashValue",
// CHECK-NEXT:        "type": "Swift.Int",
// CHECK-NEXT:        "mangledTypeName": "n/a - deprecated",
// CHECK-NEXT:        "isStatic": "false",
// CHECK-NEXT:        "isComputed": "true",
// CHECK-NEXT:        "valueKind": "Runtime"
// CHECK-NEXT:      }
// CHECK-NEXT:    ],
// CHECK-NEXT:    "cases": [
// CHECK-NEXT:      {
// CHECK-NEXT:        "name": "hello"
// CHECK-NEXT:      },
// CHECK-NEXT:      {
// CHECK-NEXT:        "name": "world"
// CHECK-NEXT:      }
// CHECK-NEXT:    ]
// CHECK-NEXT:  },
// CHECK-NEXT:  {
// CHECK-NEXT:    "typeName": "ExtractEnums.StringEnum",
// CHECK-NEXT:    "mangledTypeName": "12ExtractEnums10StringEnumO",
// CHECK-NEXT:    "kind": "enum",
// CHECK-NEXT:    "file": "{{.*}}test{{/|\\\\}}ConstExtraction{{/|\\\\}}ExtractEnums.swift",
// CHECK-NEXT:    "line": 14,
// CHECK-NEXT:    "conformances": [
// CHECK-NEXT:      "Swift.Equatable",
// CHECK-NEXT:      "Swift.Hashable",
// CHECK-NEXT:      "Swift.RawRepresentable",
// CHECK-NEXT:      "ExtractEnums.MyProto"
// CHECK-NEXT:    ],
// CHECK-NEXT:    "associatedTypeAliases": [
// CHECK-NEXT:      {
// CHECK-NEXT:        "typeAliasName": "RawValue",
// CHECK-NEXT:        "substitutedTypeName": "Swift.String",
// CHECK-NEXT:        "substitutedMangledTypeName": "SS"
// CHECK-NEXT:      }
// CHECK-NEXT:    ],    
// CHECK-NEXT:    "properties": [
// CHECK-NEXT:      {
// CHECK-NEXT:        "label": "rawValue",
// CHECK-NEXT:        "type": "Swift.String",
// CHECK-NEXT:        "mangledTypeName": "n/a - deprecated",        
// CHECK-NEXT:        "isStatic": "false",
// CHECK-NEXT:        "isComputed": "true",
// CHECK-NEXT:        "valueKind": "Runtime"
// CHECK-NEXT:      }
// CHECK-NEXT:    ],
// CHECK-NEXT:    "cases": [
// CHECK-NEXT:      {
// CHECK-NEXT:        "name": "first",
// CHECK-NEXT:        "rawValue": "first"
// CHECK-NEXT:      },
// CHECK-NEXT:      {
// CHECK-NEXT:        "name": "second",
// CHECK-NEXT:        "rawValue": "second"
// CHECK-NEXT:      },
// CHECK-NEXT:      {
// CHECK-NEXT:        "name": "third",
// CHECK-NEXT:        "rawValue": "bronzeMedal"
// CHECK-NEXT:      }
// CHECK-NEXT:    ]
// CHECK-NEXT:  },
// CHECK-NEXT:  {
// CHECK-NEXT:    "typeName": "ExtractEnums.AssociatedEnums",
// CHECK-NEXT:    "mangledTypeName": "12ExtractEnums010AssociatedB0O",    
// CHECK-NEXT:    "kind": "enum",
// CHECK-NEXT:    "file": "{{.*}}test{{/|\\\\}}ConstExtraction{{/|\\\\}}ExtractEnums.swift",
// CHECK-NEXT:    "line": 19,
// CHECK-NEXT:    "conformances": [
// CHECK-NEXT:      "ExtractEnums.MyProto"
// CHECK-NEXT:    ],
// CHECK-NEXT:    "associatedTypeAliases": [],    
// CHECK-NEXT:    "properties": [],
// CHECK-NEXT:    "cases": [
// CHECK-NEXT:      {
// CHECK-NEXT:        "name": "strings",
// CHECK-NEXT:        "parameters": [
// CHECK-NEXT:          {
// CHECK-NEXT:            "label": "title",
// CHECK-NEXT:            "type": "Swift.String"
// CHECK-NEXT:          },
// CHECK-NEXT:          {
// CHECK-NEXT:            "label": "subtitle",
// CHECK-NEXT:            "type": "Swift.String"
// CHECK-NEXT:          }
// CHECK-NEXT:        ]
// CHECK-NEXT:      },
// CHECK-NEXT:      {
// CHECK-NEXT:        "name": "ints",
// CHECK-NEXT:        "parameters": [
// CHECK-NEXT:          {
// CHECK-NEXT:            "type": "Swift.Int"
// CHECK-NEXT:          }
// CHECK-NEXT:        ]
// CHECK-NEXT:      }
// CHECK-NEXT:    ]
// CHECK-NEXT:  },
// CHECK-NEXT:  {
// CHECK-NEXT:    "typeName": "ExtractEnums.Enums",
// CHECK-NEXT:    "mangledTypeName": "12ExtractEnums0B0V",    
// CHECK-NEXT:    "kind": "struct",
// CHECK-NEXT:    "file": "{{.*}}test{{/|\\\\}}ConstExtraction{{/|\\\\}}ExtractEnums.swift",
// CHECK-NEXT:    "line": 24,
// CHECK-NEXT:    "conformances": [
// CHECK-NEXT:      "ExtractEnums.MyProto"
// CHECK-NEXT:    ],
// CHECK-NEXT:    "associatedTypeAliases": [],    
// CHECK-NEXT:    "properties": [
// CHECK-NEXT:      {
// CHECK-NEXT:        "label": "enum1",
// CHECK-NEXT:        "type": "ExtractEnums.SimpleEnum",
// CHECK-NEXT:        "mangledTypeName": "n/a - deprecated",        
// CHECK-NEXT:        "isStatic": "false",
// CHECK-NEXT:        "isComputed": "false",
// CHECK-NEXT:        "file": "{{.*}}test{{/|\\\\}}ConstExtraction{{/|\\\\}}ExtractEnums.swift",
// CHECK-NEXT:        "line": 25,
// CHECK-NEXT:        "valueKind": "Enum",
// CHECK-NEXT:        "value": {
// CHECK-NEXT:          "name": "hello"
// CHECK-NEXT:        }
// CHECK-NEXT:      },
// CHECK-NEXT:      {
// CHECK-NEXT:        "label": "enum3",
// CHECK-NEXT:        "type": "ExtractEnums.AssociatedEnums",
// CHECK-NEXT:        "mangledTypeName": "n/a - deprecated",        
// CHECK-NEXT:        "isStatic": "false",
// CHECK-NEXT:        "isComputed": "false",
// CHECK-NEXT:        "file": "{{.*}}test{{/|\\\\}}ConstExtraction{{/|\\\\}}ExtractEnums.swift",
// CHECK-NEXT:        "line": 27,
// CHECK-NEXT:        "valueKind": "Enum",
// CHECK-NEXT:        "value": {
// CHECK-NEXT:          "name": "strings",
// CHECK-NEXT:          "arguments": [
// CHECK-NEXT:            {
// CHECK-NEXT:              "label": "title",
// CHECK-NEXT:              "type": "Swift.String",
// CHECK-NEXT:              "valueKind": "RawLiteral",
// CHECK-NEXT:              "value": "the_title"
// CHECK-NEXT:            },
// CHECK-NEXT:            {
// CHECK-NEXT:              "label": "subtitle",
// CHECK-NEXT:              "type": "Swift.String",
// CHECK-NEXT:              "valueKind": "RawLiteral",
// CHECK-NEXT:              "value": "the_subtitle"
// CHECK-NEXT:            }
// CHECK-NEXT:          ]
// CHECK-NEXT:        }
// CHECK-NEXT:      },
// CHECK-NEXT:      {
// CHECK-NEXT:        "label": "enum2",
// CHECK-NEXT:        "type": "ExtractEnums.StringEnum",
// CHECK-NEXT:        "mangledTypeName": "n/a - deprecated",        
// CHECK-NEXT:        "isStatic": "false",
// CHECK-NEXT:        "isComputed": "true",
// CHECK-NEXT:        "file": "{{.*}}test{{/|\\\\}}ConstExtraction{{/|\\\\}}ExtractEnums.swift",
// CHECK-NEXT:        "line": 26,
// CHECK-NEXT:        "valueKind": "Enum",
// CHECK-NEXT:        "value": {
// CHECK-NEXT:          "name": "third"
// CHECK-NEXT:        }
// CHECK-NEXT:      },
// CHECK-NEXT:      {
// CHECK-NEXT:        "label": "enum4",
// CHECK-NEXT:        "type": "ExtractEnums.AssociatedEnums",
// CHECK-NEXT:        "mangledTypeName": "n/a - deprecated",        
// CHECK-NEXT:        "isStatic": "false",
// CHECK-NEXT:        "isComputed": "true",
// CHECK-NEXT:        "file": "{{.*}}test{{/|\\\\}}ConstExtraction{{/|\\\\}}ExtractEnums.swift",
// CHECK-NEXT:        "line": 28,
// CHECK-NEXT:        "valueKind": "Enum",
// CHECK-NEXT:        "value": {
// CHECK-NEXT:          "name": "ints",
// CHECK-NEXT:          "arguments": [
// CHECK-NEXT:            {
// CHECK-NEXT:              "label": "",
// CHECK-NEXT:              "type": "Swift.Int",
// CHECK-NEXT:              "valueKind": "RawLiteral",
// CHECK-NEXT:              "value": "42"
// CHECK-NEXT:            }
// CHECK-NEXT:          ]
// CHECK-NEXT:        }
// CHECK-NEXT:      }
// CHECK-NEXT:    ]
// CHECK-NEXT:  }
// CHECK-NEXT:]
