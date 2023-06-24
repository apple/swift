// RUN: %empty-directory(%t)
// RUN: echo "[MyProto]" > %t/protocols.json

// RUN: %target-swift-frontend -typecheck -emit-const-values-path %t/ExtractGroups.swiftconstvalues -const-gather-protocols-file %t/protocols.json -primary-file %s
// RUN: cat %t/ExtractGroups.swiftconstvalues 2>&1 | %FileCheck %s

protocol MyProto {}

public struct Arrays : MyProto {
    let array1: [Int] = [1, 2, 3]
    let array2: [Foo] = [Bar(), 1, "hi"]
    let array3: [Bar] = [Bar()]
}

public struct Dictionaries : MyProto {
    let dict1: [String: Int] = ["One": 1, "Two": 2, "Three": 3]
    let dict2: [Int: [String]] = [
        1: ["a", "b", "c"],
        2: ["z"]
    ]
    let dict3: [String: Foo] = [
        "Bar": Bar(),
        "Int": 42
    ]
}

public struct Tuples : MyProto {
    let tuple1: (String, Bar) = ("foo", Bar())
    let tuple2: (lat: Float, lng: Float) = (lat: 42.7, lng: -73.9)
    let tuple3: Void = ()
}

public protocol Foo {}
public struct Bar: Foo {}
extension Int: Foo {}
extension String: Foo {}

// CHECK: [
// CHECK-NEXT:  {
// CHECK-NEXT:    "typeName": "ExtractGroups.Arrays",
// CHECK-NEXT:    "mangledTypeName": "13ExtractGroups6ArraysV",
// CHECK-NEXT:    "kind": "struct",
// CHECK-NEXT:    "file": "{{.*}}test{{/|\\\\}}ConstExtraction{{/|\\\\}}ExtractGroups.swift",
// CHECK-NEXT:    "line": 9,
// CHECK-NEXT:    "conformances": [
// CHECK-NEXT:      "ExtractGroups.MyProto"
// CHECK-NEXT:    ],
// CHECK-NEXT:    "associatedTypeAliases": [],
// CHECK-NEXT:    "properties": [
// CHECK-NEXT:      {
// CHECK-NEXT:        "label": "array1",
// CHECK-NEXT:        "type": "Swift.Array<Swift.Int>",
// CHECK-NEXT:        "mangledTypeName": "SaySiG",
// CHECK-NEXT:        "isStatic": "false",
// CHECK-NEXT:        "isComputed": "false",
// CHECK-NEXT:        "file": "{{.*}}test{{/|\\\\}}ConstExtraction{{/|\\\\}}ExtractGroups.swift",
// CHECK-NEXT:        "line": 10,
// CHECK-NEXT:        "valueKind": "Array",
// CHECK-NEXT:        "value": [
// CHECK-NEXT:          {
// CHECK-NEXT:            "valueKind": "RawLiteral",
// CHECK-NEXT:            "value": "1"
// CHECK-NEXT:          },
// CHECK-NEXT:          {
// CHECK-NEXT:            "valueKind": "RawLiteral",
// CHECK-NEXT:            "value": "2"
// CHECK-NEXT:          },
// CHECK-NEXT:          {
// CHECK-NEXT:            "valueKind": "RawLiteral",
// CHECK-NEXT:            "value": "3"
// CHECK-NEXT:          }
// CHECK-NEXT:        ]
// CHECK-NEXT:      },
// CHECK-NEXT:      {
// CHECK-NEXT:        "label": "array2",
// CHECK-NEXT:        "type": "Swift.Array<any ExtractGroups.Foo>",
// CHECK-NEXT:        "mangledTypeName": "Say13ExtractGroups3Foo_pG",
// CHECK-NEXT:        "isStatic": "false",
// CHECK-NEXT:        "isComputed": "false",
// CHECK-NEXT:        "file": "{{.*}}test{{/|\\\\}}ConstExtraction{{/|\\\\}}ExtractGroups.swift",
// CHECK-NEXT:        "line": 11,
// CHECK-NEXT:        "valueKind": "Array",
// CHECK-NEXT:        "value": [
// CHECK-NEXT:          {
// CHECK-NEXT:            "valueKind": "InitCall",
// CHECK-NEXT:            "value": {
// CHECK-NEXT:              "type": "ExtractGroups.Bar",
// CHECK-NEXT:              "arguments": []
// CHECK-NEXT:            }
// CHECK-NEXT:          },
// CHECK-NEXT:          {
// CHECK-NEXT:            "valueKind": "RawLiteral",
// CHECK-NEXT:            "value": "1"
// CHECK-NEXT:          },
// CHECK-NEXT:          {
// CHECK-NEXT:            "valueKind": "RawLiteral",
// CHECK-NEXT:            "value": "hi"
// CHECK-NEXT:          }
// CHECK-NEXT:        ]
// CHECK-NEXT:      },
// CHECK-NEXT:      {
// CHECK-NEXT:        "label": "array3",
// CHECK-NEXT:        "type": "Swift.Array<ExtractGroups.Bar>",
// CHECK-NEXT:        "mangledTypeName": "Say13ExtractGroups3BarVG",
// CHECK-NEXT:        "isStatic": "false",
// CHECK-NEXT:        "isComputed": "false",
// CHECK-NEXT:        "file": "{{.*}}test{{/|\\\\}}ConstExtraction{{/|\\\\}}ExtractGroups.swift",
// CHECK-NEXT:        "line": 12,
// CHECK-NEXT:        "valueKind": "Array",
// CHECK-NEXT:        "value": [
// CHECK-NEXT:          {
// CHECK-NEXT:            "valueKind": "InitCall",
// CHECK-NEXT:            "value": {
// CHECK-NEXT:              "type": "ExtractGroups.Bar",
// CHECK-NEXT:              "arguments": []
// CHECK-NEXT:            }
// CHECK-NEXT:          }
// CHECK-NEXT:        ]
// CHECK-NEXT:      }
// CHECK-NEXT:    ]
// CHECK-NEXT:  },
// CHECK-NEXT:  {
// CHECK-NEXT:    "typeName": "ExtractGroups.Dictionaries",
// CHECK-NEXT:    "mangledTypeName": "13ExtractGroups12DictionariesV",
// CHECK-NEXT:    "kind": "struct",
// CHECK-NEXT:    "file": "{{.*}}test{{/|\\\\}}ConstExtraction{{/|\\\\}}ExtractGroups.swift",
// CHECK-NEXT:    "line": 15,
// CHECK-NEXT:    "conformances": [
// CHECK-NEXT:      "ExtractGroups.MyProto"
// CHECK-NEXT:    ],
// CHECK-NEXT:    "associatedTypeAliases": [],    
// CHECK-NEXT:    "properties": [
// CHECK-NEXT:      {
// CHECK-NEXT:        "label": "dict1",
// CHECK-NEXT:        "type": "Swift.Dictionary<Swift.String, Swift.Int>",
// CHECK-NEXT:        "mangledTypeName": "SDySSSiG",
// CHECK-NEXT:        "isStatic": "false",
// CHECK-NEXT:        "isComputed": "false",
// CHECK-NEXT:        "file": "{{.*}}test{{/|\\\\}}ConstExtraction{{/|\\\\}}ExtractGroups.swift",
// CHECK-NEXT:        "line": 16,
// CHECK-NEXT:        "valueKind": "Dictionary",
// CHECK-NEXT:        "value": [
// CHECK-NEXT:          {
// CHECK-NEXT:            "key": {
// CHECK-NEXT:              "valueKind": "RawLiteral",
// CHECK-NEXT:              "value": "One"
// CHECK-NEXT:            },
// CHECK-NEXT:            "value": {
// CHECK-NEXT:              "valueKind": "RawLiteral",
// CHECK-NEXT:              "value": "1"
// CHECK-NEXT:            }
// CHECK-NEXT:          },
// CHECK-NEXT:          {
// CHECK-NEXT:            "key": {
// CHECK-NEXT:              "valueKind": "RawLiteral",
// CHECK-NEXT:              "value": "Two"
// CHECK-NEXT:            },
// CHECK-NEXT:            "value": {
// CHECK-NEXT:              "valueKind": "RawLiteral",
// CHECK-NEXT:              "value": "2"
// CHECK-NEXT:            }
// CHECK-NEXT:          },
// CHECK-NEXT:          {
// CHECK-NEXT:            "key": {
// CHECK-NEXT:              "valueKind": "RawLiteral",
// CHECK-NEXT:              "value": "Three"
// CHECK-NEXT:            },
// CHECK-NEXT:            "value": {
// CHECK-NEXT:              "valueKind": "RawLiteral",
// CHECK-NEXT:              "value": "3"
// CHECK-NEXT:            }
// CHECK-NEXT:          }
// CHECK-NEXT:        ]
// CHECK-NEXT:      },
// CHECK-NEXT:      {
// CHECK-NEXT:        "label": "dict2",
// CHECK-NEXT:        "type": "Swift.Dictionary<Swift.Int, Swift.Array<Swift.String>>",
// CHECK-NEXT:        "mangledTypeName": "SDySiSaySSGG",
// CHECK-NEXT:        "isStatic": "false",
// CHECK-NEXT:        "isComputed": "false",
// CHECK-NEXT:        "file": "{{.*}}test{{/|\\\\}}ConstExtraction{{/|\\\\}}ExtractGroups.swift",
// CHECK-NEXT:        "line": 17,
// CHECK-NEXT:        "valueKind": "Dictionary",
// CHECK-NEXT:        "value": [
// CHECK-NEXT:          {
// CHECK-NEXT:            "key": {
// CHECK-NEXT:              "valueKind": "RawLiteral",
// CHECK-NEXT:              "value": "1"
// CHECK-NEXT:            },
// CHECK-NEXT:            "value": {
// CHECK-NEXT:              "valueKind": "Array",
// CHECK-NEXT:              "value": [
// CHECK-NEXT:                {
// CHECK-NEXT:                  "valueKind": "RawLiteral",
// CHECK-NEXT:                  "value": "a"
// CHECK-NEXT:                },
// CHECK-NEXT:                {
// CHECK-NEXT:                  "valueKind": "RawLiteral",
// CHECK-NEXT:                  "value": "b"
// CHECK-NEXT:                },
// CHECK-NEXT:                {
// CHECK-NEXT:                  "valueKind": "RawLiteral",
// CHECK-NEXT:                  "value": "c"
// CHECK-NEXT:                }
// CHECK-NEXT:              ]
// CHECK-NEXT:            }
// CHECK-NEXT:          },
// CHECK-NEXT:          {
// CHECK-NEXT:            "key": {
// CHECK-NEXT:              "valueKind": "RawLiteral",
// CHECK-NEXT:              "value": "2"
// CHECK-NEXT:            },
// CHECK-NEXT:            "value": {
// CHECK-NEXT:              "valueKind": "Array",
// CHECK-NEXT:              "value": [
// CHECK-NEXT:                {
// CHECK-NEXT:                  "valueKind": "RawLiteral",
// CHECK-NEXT:                  "value": "z"
// CHECK-NEXT:                }
// CHECK-NEXT:              ]
// CHECK-NEXT:            }
// CHECK-NEXT:          }
// CHECK-NEXT:        ]
// CHECK-NEXT:      },
// CHECK-NEXT:      {
// CHECK-NEXT:        "label": "dict3",
// CHECK-NEXT:        "type": "Swift.Dictionary<Swift.String, any ExtractGroups.Foo>",
// CHECK-NEXT:        "mangledTypeName": "SDySS13ExtractGroups3Foo_pG",
// CHECK-NEXT:        "isStatic": "false",
// CHECK-NEXT:        "isComputed": "false",
// CHECK-NEXT:        "file": "{{.*}}test{{/|\\\\}}ConstExtraction{{/|\\\\}}ExtractGroups.swift",
// CHECK-NEXT:        "line": 21,
// CHECK-NEXT:        "valueKind": "Dictionary",
// CHECK-NEXT:        "value": [
// CHECK-NEXT:          {
// CHECK-NEXT:            "key": {
// CHECK-NEXT:              "valueKind": "RawLiteral",
// CHECK-NEXT:              "value": "Bar"
// CHECK-NEXT:            },
// CHECK-NEXT:            "value": {
// CHECK-NEXT:              "valueKind": "InitCall",
// CHECK-NEXT:              "value": {
// CHECK-NEXT:                "type": "ExtractGroups.Bar",
// CHECK-NEXT:                "arguments": []
// CHECK-NEXT:              }
// CHECK-NEXT:            }
// CHECK-NEXT:          },
// CHECK-NEXT:          {
// CHECK-NEXT:            "key": {
// CHECK-NEXT:              "valueKind": "RawLiteral",
// CHECK-NEXT:              "value": "Int"
// CHECK-NEXT:            },
// CHECK-NEXT:            "value": {
// CHECK-NEXT:              "valueKind": "RawLiteral",
// CHECK-NEXT:              "value": "42"
// CHECK-NEXT:            }
// CHECK-NEXT:          }
// CHECK-NEXT:        ]
// CHECK-NEXT:      }
// CHECK-NEXT:    ]
// CHECK-NEXT:  },
// CHECK-NEXT:  {
// CHECK-NEXT:    "typeName": "ExtractGroups.Tuples",
// CHECK-NEXT:    "mangledTypeName": "13ExtractGroups6TuplesV",
// CHECK-NEXT:    "kind": "struct",
// CHECK-NEXT:    "file": "{{.*}}test{{/|\\\\}}ConstExtraction{{/|\\\\}}ExtractGroups.swift",
// CHECK-NEXT:    "line": 27,
// CHECK-NEXT:    "conformances": [
// CHECK-NEXT:      "ExtractGroups.MyProto"
// CHECK-NEXT:    ],
// CHECK-NEXT:    "associatedTypeAliases": [],
// CHECK-NEXT:    "properties": [
// CHECK-NEXT:      {
// CHECK-NEXT:        "label": "tuple1",
// CHECK-NEXT:        "type": "(Swift.String, ExtractGroups.Bar)",
// CHECK-NEXT:        "mangledTypeName": "SS_13ExtractGroups3BarVt",        
// CHECK-NEXT:        "isStatic": "false",
// CHECK-NEXT:        "isComputed": "false",
// CHECK-NEXT:        "file": "{{.*}}test{{/|\\\\}}ConstExtraction{{/|\\\\}}ExtractGroups.swift",
// CHECK-NEXT:        "line": 28,
// CHECK-NEXT:        "valueKind": "Tuple",
// CHECK-NEXT:        "value": [
// CHECK-NEXT:          {
// CHECK-NEXT:            "type": "Swift.String",
// CHECK-NEXT:            "valueKind": "RawLiteral",
// CHECK-NEXT:            "value": "foo"
// CHECK-NEXT:          },
// CHECK-NEXT:          {
// CHECK-NEXT:            "type": "ExtractGroups.Bar",
// CHECK-NEXT:            "valueKind": "InitCall",
// CHECK-NEXT:            "value": {
// CHECK-NEXT:              "type": "ExtractGroups.Bar",
// CHECK-NEXT:              "arguments": []
// CHECK-NEXT:            }
// CHECK-NEXT:          }
// CHECK-NEXT:        ]
// CHECK-NEXT:      },
// CHECK-NEXT:      {
// CHECK-NEXT:        "label": "tuple2",
// CHECK-NEXT:        "type": "(lat: Swift.Float, lng: Swift.Float)",
// CHECK-NEXT:        "mangledTypeName": "Sf3lat_Sf3lngt",        
// CHECK-NEXT:        "isStatic": "false",
// CHECK-NEXT:        "isComputed": "false",
// CHECK-NEXT:        "file": "{{.*}}test{{/|\\\\}}ConstExtraction{{/|\\\\}}ExtractGroups.swift",
// CHECK-NEXT:        "line": 29,
// CHECK-NEXT:        "valueKind": "Tuple",
// CHECK-NEXT:        "value": [
// CHECK-NEXT:          {
// CHECK-NEXT:            "label": "lat",
// CHECK-NEXT:            "type": "Swift.Float",
// CHECK-NEXT:            "valueKind": "RawLiteral",
// CHECK-NEXT:            "value": "42.7"
// CHECK-NEXT:          },
// CHECK-NEXT:          {
// CHECK-NEXT:            "label": "lng",
// CHECK-NEXT:            "type": "Swift.Float",
// CHECK-NEXT:            "valueKind": "RawLiteral",
// CHECK-NEXT:            "value": "-73.9"
// CHECK-NEXT:          }
// CHECK-NEXT:        ]
// CHECK-NEXT:      },
// CHECK-NEXT:      {
// CHECK-NEXT:        "label": "tuple3",
// CHECK-NEXT:        "type": "Swift.Void",
// CHECK-NEXT:        "mangledTypeName": "yt",
// CHECK-NEXT:        "isStatic": "false",
// CHECK-NEXT:        "isComputed": "false",
// CHECK-NEXT:        "file": "{{.*}}test{{/|\\\\}}ConstExtraction{{/|\\\\}}ExtractGroups.swift",
// CHECK-NEXT:        "line": 30,
// CHECK-NEXT:        "valueKind": "Tuple",
// CHECK-NEXT:        "value": []
// CHECK-NEXT:      }
// CHECK-NEXT:    ]
// CHECK-NEXT:  }
// CHECK-NEXT:]
