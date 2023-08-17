// RUN: %empty-directory(%t)
// RUN: echo "[MyProto]" > %t/protocols.json

// RUN: %target-swift-frontend -typecheck -emit-const-values-path %t/ExtractLiterals.swiftconstvalues -const-gather-protocols-file %t/protocols.json -primary-file %s
// RUN: cat %t/ExtractLiterals.swiftconstvalues 2>&1 | %FileCheck %s

protocol MyProto {}

public struct Bools : MyProto {
    let bool1: Bool = false
    let bool2: Bool? = nil
}

public struct Ints : MyProto {
    static _const let int1: Int = 1
    var int2: Int { 2 }
    static var int3: Int { return 3 }
}

public struct Floats : MyProto {
    static let float1: Float = 42.2
    static var float2: Float { return (6) }
    static var float3: Float { return (1 + 2) }
}

public struct Strings : MyProto {
    let string1: String =
    """
    First "string"
    Second \\ string
    """

    let string2: String = (("Hi"))
    var string3: String { ("Hey") }
}

public struct PropertyWrappers : MyProto {
    @Buffered
    var propertyWrapper1: String = "Hello"

    @Clamping(min: 0, max: 255)
    var propertyWrapper2: Int = 128

    @Buffered @Clamping(min: 0, max: 255)
    var propertyWrapper3: Int = 128
}

@propertyWrapper
 struct Clamping<V: Comparable> {
     var value: V
     let min: V
     let max: V

     init(wrappedValue: V, min: V, max: V) {
         self.value = wrappedValue
         self.min = min
         self.max = max
     }

     var wrappedValue: V {
         get { return self.value }
         set {
             if newValue < self.min {
                 self.value = self.min
             } else if newValue > self.max {
                 self.value = self.max
             } else {
                 self.value = newValue
             }
         }
     }
 }

 @propertyWrapper
 struct Buffered<V> {
     var value: V
     var lastValue: V?

     init(wrappedValue: V) {
         self.value = wrappedValue
     }

     var wrappedValue: V {
         get { return value }
         set {
             self.lastValue = self.value
             self.value = newValue
         }
     }

     var projectedValue: (V, V?) { (self.value, self.lastValue) }
}

// CHECK: [
// CHECK-NEXT:  {
// CHECK-NEXT:    "typeName": "ExtractLiterals.Bools",
// CHECK-NEXT:    "mangledTypeName": "15ExtractLiterals5BoolsV",
// CHECK-NEXT:    "kind": "struct",
// CHECK-NEXT:    "file": "{{.*}}test{{/|\\\\}}ConstExtraction{{/|\\\\}}ExtractLiterals.swift",
// CHECK-NEXT:    "line": 9,
// CHECK-NEXT:    "conformances": [
// CHECK-NEXT:      "ExtractLiterals.MyProto"
// CHECK-NEXT:    ],
// CHECK-NEXT:    "associatedTypeAliases": [],
// CHECK-NEXT:    "properties": [
// CHECK-NEXT:      {
// CHECK-NEXT:        "label": "bool1",
// CHECK-NEXT:        "type": "Swift.Bool",
// CHECK-NEXT:        "mangledTypeName": "n/a - deprecated",
// CHECK-NEXT:        "isStatic": "false",
// CHECK-NEXT:        "isComputed": "false",
// CHECK-NEXT:        "file": "{{.*}}test{{/|\\\\}}ConstExtraction{{/|\\\\}}ExtractLiterals.swift",
// CHECK-NEXT:        "line": 10,
// CHECK-NEXT:        "valueKind": "RawLiteral",
// CHECK-NEXT:        "value": "false"
// CHECK-NEXT:      },
// CHECK-NEXT:      {
// CHECK-NEXT:        "label": "bool2",
// CHECK-NEXT:        "type": "Swift.Optional<Swift.Bool>",
// CHECK-NEXT:        "mangledTypeName": "n/a - deprecated",
// CHECK-NEXT:        "isStatic": "false",
// CHECK-NEXT:        "isComputed": "false",
// CHECK-NEXT:        "file": "{{.*}}test{{/|\\\\}}ConstExtraction{{/|\\\\}}ExtractLiterals.swift",
// CHECK-NEXT:        "line": 11,
// CHECK-NEXT:        "valueKind": "RawLiteral",
// CHECK-NEXT:        "value": "nil"
// CHECK-NEXT:      }
// CHECK-NEXT:    ]
// CHECK-NEXT:  },
// CHECK-NEXT:  {
// CHECK-NEXT:    "typeName": "ExtractLiterals.Ints",
// CHECK-NEXT:    "mangledTypeName": "15ExtractLiterals4IntsV",
// CHECK-NEXT:    "kind": "struct",
// CHECK-NEXT:    "file": "{{.*}}test{{/|\\\\}}ConstExtraction{{/|\\\\}}ExtractLiterals.swift",
// CHECK-NEXT:    "line": 14,
// CHECK-NEXT:    "conformances": [
// CHECK-NEXT:      "ExtractLiterals.MyProto"
// CHECK-NEXT:    ],
// CHECK-NEXT:    "associatedTypeAliases": [],
// CHECK-NEXT:    "properties": [
// CHECK-NEXT:      {
// CHECK-NEXT:        "label": "int1",
// CHECK-NEXT:        "type": "Swift.Int",
// CHECK-NEXT:        "mangledTypeName": "n/a - deprecated",
// CHECK-NEXT:        "isStatic": "true",
// CHECK-NEXT:        "isComputed": "false",
// CHECK-NEXT:        "file": "{{.*}}test{{/|\\\\}}ConstExtraction{{/|\\\\}}ExtractLiterals.swift",
// CHECK-NEXT:        "line": 15,
// CHECK-NEXT:        "valueKind": "RawLiteral",
// CHECK-NEXT:        "value": "1"
// CHECK-NEXT:      },
// CHECK-NEXT:      {
// CHECK-NEXT:        "label": "int2",
// CHECK-NEXT:        "type": "Swift.Int",
// CHECK-NEXT:        "mangledTypeName": "n/a - deprecated",
// CHECK-NEXT:        "isStatic": "false",
// CHECK-NEXT:        "isComputed": "true",
// CHECK-NEXT:        "file": "{{.*}}test{{/|\\\\}}ConstExtraction{{/|\\\\}}ExtractLiterals.swift",
// CHECK-NEXT:        "line": 16,
// CHECK-NEXT:        "valueKind": "RawLiteral",
// CHECK-NEXT:        "value": "2"
// CHECK-NEXT:      },
// CHECK-NEXT:      {
// CHECK-NEXT:        "label": "int3",
// CHECK-NEXT:        "type": "Swift.Int",
// CHECK-NEXT:        "mangledTypeName": "n/a - deprecated",
// CHECK-NEXT:        "isStatic": "true",
// CHECK-NEXT:        "isComputed": "true",
// CHECK-NEXT:        "file": "{{.*}}test{{/|\\\\}}ConstExtraction{{/|\\\\}}ExtractLiterals.swift",
// CHECK-NEXT:        "line": 17,
// CHECK-NEXT:        "valueKind": "RawLiteral",
// CHECK-NEXT:        "value": "3"
// CHECK-NEXT:      }
// CHECK-NEXT:    ]
// CHECK-NEXT:  },
// CHECK-NEXT:  {
// CHECK-NEXT:    "typeName": "ExtractLiterals.Floats",
// CHECK-NEXT:    "mangledTypeName": "15ExtractLiterals6FloatsV",
// CHECK-NEXT:    "kind": "struct",
// CHECK-NEXT:    "file": "{{.*}}test{{/|\\\\}}ConstExtraction{{/|\\\\}}ExtractLiterals.swift",
// CHECK-NEXT:    "line": 20,
// CHECK-NEXT:    "conformances": [
// CHECK-NEXT:      "ExtractLiterals.MyProto"
// CHECK-NEXT:    ],
// CHECK-NEXT:    "associatedTypeAliases": [],
// CHECK-NEXT:    "properties": [
// CHECK-NEXT:      {
// CHECK-NEXT:        "label": "float1",
// CHECK-NEXT:        "type": "Swift.Float",
// CHECK-NEXT:        "mangledTypeName": "n/a - deprecated",
// CHECK-NEXT:        "isStatic": "true",
// CHECK-NEXT:        "isComputed": "false",
// CHECK-NEXT:        "file": "{{.*}}test{{/|\\\\}}ConstExtraction{{/|\\\\}}ExtractLiterals.swift",
// CHECK-NEXT:        "line": 21,
// CHECK-NEXT:        "valueKind": "RawLiteral",
// CHECK-NEXT:        "value": "42.2"
// CHECK-NEXT:      },
// CHECK-NEXT:      {
// CHECK-NEXT:        "label": "float2",
// CHECK-NEXT:        "type": "Swift.Float",
// CHECK-NEXT:        "mangledTypeName": "n/a - deprecated",
// CHECK-NEXT:        "isStatic": "true",
// CHECK-NEXT:        "isComputed": "true",
// CHECK-NEXT:        "file": "{{.*}}test{{/|\\\\}}ConstExtraction{{/|\\\\}}ExtractLiterals.swift",
// CHECK-NEXT:        "line": 22,
// CHECK-NEXT:        "valueKind": "RawLiteral",
// CHECK-NEXT:        "value": "6"
// CHECK-NEXT:      },
// CHECK-NEXT:      {
// CHECK-NEXT:        "label": "float3",
// CHECK-NEXT:        "type": "Swift.Float",
// CHECK-NEXT:        "mangledTypeName": "n/a - deprecated",
// CHECK-NEXT:        "isStatic": "true",
// CHECK-NEXT:        "isComputed": "true",
// CHECK-NEXT:        "file": "{{.*}}test{{/|\\\\}}ConstExtraction{{/|\\\\}}ExtractLiterals.swift",
// CHECK-NEXT:        "line": 23,
// CHECK-NEXT:        "valueKind": "Runtime"
// CHECK-NEXT:      }
// CHECK-NEXT:    ]
// CHECK-NEXT:  },
// CHECK-NEXT:  {
// CHECK-NEXT:    "typeName": "ExtractLiterals.Strings",
// CHECK-NEXT:    "mangledTypeName": "15ExtractLiterals7StringsV",
// CHECK-NEXT:    "kind": "struct",
// CHECK-NEXT:    "file": "{{.*}}test{{/|\\\\}}ConstExtraction{{/|\\\\}}ExtractLiterals.swift",
// CHECK-NEXT:    "line": 26,
// CHECK-NEXT:    "conformances": [
// CHECK-NEXT:      "ExtractLiterals.MyProto"
// CHECK-NEXT:    ],
// CHECK-NEXT:    "associatedTypeAliases": [],
// CHECK-NEXT:    "properties": [
// CHECK-NEXT:      {
// CHECK-NEXT:        "label": "string1",
// CHECK-NEXT:        "type": "Swift.String",
// CHECK-NEXT:        "mangledTypeName": "n/a - deprecated",
// CHECK-NEXT:        "isStatic": "false",
// CHECK-NEXT:        "isComputed": "false",
// CHECK-NEXT:        "file": "{{.*}}test{{/|\\\\}}ConstExtraction{{/|\\\\}}ExtractLiterals.swift",
// CHECK-NEXT:        "line": 27,
// CHECK-NEXT:        "valueKind": "RawLiteral",
// CHECK-NEXT:        "value": "First \"string\"\nSecond \\ string"
// CHECK-NEXT:      },
// CHECK-NEXT:      {
// CHECK-NEXT:        "label": "string2",
// CHECK-NEXT:        "type": "Swift.String",
// CHECK-NEXT:        "mangledTypeName": "n/a - deprecated",
// CHECK-NEXT:        "isStatic": "false",
// CHECK-NEXT:        "isComputed": "false",
// CHECK-NEXT:        "file": "{{.*}}test{{/|\\\\}}ConstExtraction{{/|\\\\}}ExtractLiterals.swift",
// CHECK-NEXT:        "line": 33,
// CHECK-NEXT:        "valueKind": "RawLiteral",
// CHECK-NEXT:        "value": "Hi"
// CHECK-NEXT:      },
// CHECK-NEXT:      {
// CHECK-NEXT:        "label": "string3",
// CHECK-NEXT:        "type": "Swift.String",
// CHECK-NEXT:        "mangledTypeName": "n/a - deprecated",
// CHECK-NEXT:        "isStatic": "false",
// CHECK-NEXT:        "isComputed": "true",
// CHECK-NEXT:        "file": "{{.*}}test{{/|\\\\}}ConstExtraction{{/|\\\\}}ExtractLiterals.swift",
// CHECK-NEXT:        "line": 34,
// CHECK-NEXT:        "valueKind": "RawLiteral",
// CHECK-NEXT:        "value": "Hey"
// CHECK-NEXT:      }
// CHECK-NEXT:    ]
// CHECK-NEXT:  },
// CHECK-NEXT:  {
// CHECK-NEXT:    "typeName": "ExtractLiterals.PropertyWrappers",
// CHECK-NEXT:    "mangledTypeName": "15ExtractLiterals16PropertyWrappersV",
// CHECK-NEXT:    "kind": "struct",
// CHECK-NEXT:    "file": "{{.*}}test{{/|\\\\}}ConstExtraction{{/|\\\\}}ExtractLiterals.swift",
// CHECK-NEXT:    "line": 37,
// CHECK-NEXT:    "conformances": [
// CHECK-NEXT:      "ExtractLiterals.MyProto"
// CHECK-NEXT:    ],
// CHECK-NEXT:    "associatedTypeAliases": [],
// CHECK-NEXT:    "properties": [
// CHECK-NEXT:      {
// CHECK-NEXT:        "label": "_propertyWrapper1",
// CHECK-NEXT:        "type": "ExtractLiterals.Buffered<Swift.String>",
// CHECK-NEXT:        "mangledTypeName": "n/a - deprecated",
// CHECK-NEXT:        "isStatic": "false",
// CHECK-NEXT:        "isComputed": "false",
// CHECK-NEXT:        "file": "{{.*}}test{{/|\\\\}}ConstExtraction{{/|\\\\}}ExtractLiterals.swift",
// CHECK-NEXT:        "line": 39,
// CHECK-NEXT:        "valueKind": "InitCall",
// CHECK-NEXT:        "value": {
// CHECK-NEXT:          "type": "ExtractLiterals.Buffered<Swift.String>",
// CHECK-NEXT:          "arguments": [
// CHECK-NEXT:            {
// CHECK-NEXT:              "label": "wrappedValue",
// CHECK-NEXT:              "type": "Swift.String",
// CHECK-NEXT:              "valueKind": "RawLiteral",
// CHECK-NEXT:              "value": "Hello"
// CHECK-NEXT:            }
// CHECK-NEXT:          ]
// CHECK-NEXT:        }
// CHECK-NEXT:      },
// CHECK-NEXT:      {
// CHECK-NEXT:        "label": "_propertyWrapper2",
// CHECK-NEXT:        "type": "ExtractLiterals.Clamping<Swift.Int>",
// CHECK-NEXT:        "mangledTypeName": "n/a - deprecated",
// CHECK-NEXT:        "isStatic": "false",
// CHECK-NEXT:        "isComputed": "false",
// CHECK-NEXT:        "file": "{{.*}}test{{/|\\\\}}ConstExtraction{{/|\\\\}}ExtractLiterals.swift",
// CHECK-NEXT:        "line": 42,
// CHECK-NEXT:        "valueKind": "InitCall",
// CHECK-NEXT:        "value": {
// CHECK-NEXT:          "type": "ExtractLiterals.Clamping<Swift.Int>",
// CHECK-NEXT:          "arguments": [
// CHECK-NEXT:            {
// CHECK-NEXT:              "label": "wrappedValue",
// CHECK-NEXT:              "type": "Swift.Int",
// CHECK-NEXT:              "valueKind": "RawLiteral",
// CHECK-NEXT:              "value": "128"
// CHECK-NEXT:            },
// CHECK-NEXT:            {
// CHECK-NEXT:              "label": "min",
// CHECK-NEXT:              "type": "Swift.Int",
// CHECK-NEXT:              "valueKind": "RawLiteral",
// CHECK-NEXT:              "value": "0"
// CHECK-NEXT:            },
// CHECK-NEXT:            {
// CHECK-NEXT:              "label": "max",
// CHECK-NEXT:              "type": "Swift.Int",
// CHECK-NEXT:              "valueKind": "RawLiteral",
// CHECK-NEXT:              "value": "255"
// CHECK-NEXT:            }
// CHECK-NEXT:          ]
// CHECK-NEXT:        }
// CHECK-NEXT:      },
// CHECK-NEXT:      {
// CHECK-NEXT:        "label": "_propertyWrapper3",
// CHECK-NEXT:        "type": "ExtractLiterals.Buffered<ExtractLiterals.Clamping<Swift.Int>>",
// CHECK-NEXT:        "mangledTypeName": "n/a - deprecated",
// CHECK-NEXT:        "isStatic": "false",
// CHECK-NEXT:        "isComputed": "false",
// CHECK-NEXT:        "file": "{{.*}}test{{/|\\\\}}ConstExtraction{{/|\\\\}}ExtractLiterals.swift",
// CHECK-NEXT:        "line": 45,
// CHECK-NEXT:        "valueKind": "InitCall",
// CHECK-NEXT:        "value": {
// CHECK-NEXT:          "type": "ExtractLiterals.Buffered<ExtractLiterals.Clamping<Swift.Int>>",
// CHECK-NEXT:          "arguments": [
// CHECK-NEXT:            {
// CHECK-NEXT:              "label": "wrappedValue",
// CHECK-NEXT:              "type": "ExtractLiterals.Clamping<Swift.Int>",
// CHECK-NEXT:              "valueKind": "InitCall",
// CHECK-NEXT:              "value": {
// CHECK-NEXT:                "type": "ExtractLiterals.Clamping<Swift.Int>",
// CHECK-NEXT:                "arguments": [
// CHECK-NEXT:                  {
// CHECK-NEXT:                    "label": "wrappedValue",
// CHECK-NEXT:                    "type": "Swift.Int",
// CHECK-NEXT:                    "valueKind": "RawLiteral"
// CHECK-NEXT:                    "value": "128"
// CHECK-NEXT:                  },
// CHECK-NEXT:                  {
// CHECK-NEXT:                    "label": "min",
// CHECK-NEXT:                    "type": "Swift.Int",
// CHECK-NEXT:                    "valueKind": "RawLiteral",
// CHECK-NEXT:                    "value": "0"
// CHECK-NEXT:                  },
// CHECK-NEXT:                  {
// CHECK-NEXT:                    "label": "max",
// CHECK-NEXT:                    "type": "Swift.Int",
// CHECK-NEXT:                    "valueKind": "RawLiteral",
// CHECK-NEXT:                    "value": "255"
// CHECK-NEXT:                  }
// CHECK-NEXT:                ]
// CHECK-NEXT:              }
// CHECK-NEXT:            }
// CHECK-NEXT:          ]
// CHECK-NEXT:        }
// CHECK-NEXT:      },
// CHECK-NEXT:      {
// CHECK-NEXT:        "label": "propertyWrapper1",
// CHECK-NEXT:        "type": "Swift.String",
// CHECK-NEXT:        "mangledTypeName": "n/a - deprecated",
// CHECK-NEXT:        "isStatic": "false",
// CHECK-NEXT:        "isComputed": "true",
// CHECK-NEXT:        "file": "{{.*}}test{{/|\\\\}}ConstExtraction{{/|\\\\}}ExtractLiterals.swift",
// CHECK-NEXT:        "line": 39,
// CHECK-NEXT:        "valueKind": "InitCall",
// CHECK-NEXT:        "value": {
// CHECK-NEXT:          "type": "ExtractLiterals.Buffered<Swift.String>",
// CHECK-NEXT:          "arguments": [
// CHECK-NEXT:            {
// CHECK-NEXT:              "label": "wrappedValue",
// CHECK-NEXT:              "type": "Swift.String",
// CHECK-NEXT:              "valueKind": "RawLiteral",
// CHECK-NEXT:              "value": "Hello"
// CHECK-NEXT:            }
// CHECK-NEXT:          ]
// CHECK-NEXT:        },
// CHECK-NEXT:        "propertyWrappers": [
// CHECK-NEXT:          {
// CHECK-NEXT:            "type": "ExtractLiterals.Buffered",
// CHECK-NEXT:            "file": "{{.*}}test{{/|\\\\}}ConstExtraction{{/|\\\\}}ExtractLiterals.swift",
// CHECK-NEXT:            "line": 38,
// CHECK-NEXT:            "arguments": []
// CHECK-NEXT:          }
// CHECK-NEXT:        ]
// CHECK-NEXT:      },
// CHECK-NEXT:      {
// CHECK-NEXT:        "label": "$propertyWrapper1",
// CHECK-NEXT:        "type": "(Swift.String, Swift.Optional<Swift.String>)",
// CHECK-NEXT:        "mangledTypeName": "n/a - deprecated",
// CHECK-NEXT:        "isStatic": "false",
// CHECK-NEXT:        "isComputed": "true",
// CHECK-NEXT:        "file": "{{.*}}test{{/|\\\\}}ConstExtraction{{/|\\\\}}ExtractLiterals.swift",
// CHECK-NEXT:        "line": 39,
// CHECK-NEXT:        "valueKind": "Runtime"
// CHECK-NEXT:      },
// CHECK-NEXT:      {
// CHECK-NEXT:        "label": "propertyWrapper2",
// CHECK-NEXT:        "type": "Swift.Int",
// CHECK-NEXT:        "mangledTypeName": "n/a - deprecated",
// CHECK-NEXT:        "isStatic": "false",
// CHECK-NEXT:        "isComputed": "true",
// CHECK-NEXT:        "file": "{{.*}}test{{/|\\\\}}ConstExtraction{{/|\\\\}}ExtractLiterals.swift",
// CHECK-NEXT:        "line": 42,
// CHECK-NEXT:        "valueKind": "InitCall",
// CHECK-NEXT:        "value": {
// CHECK-NEXT:          "type": "ExtractLiterals.Clamping<Swift.Int>",
// CHECK-NEXT:          "arguments": [
// CHECK-NEXT:            {
// CHECK-NEXT:              "label": "wrappedValue",
// CHECK-NEXT:              "type": "Swift.Int",
// CHECK-NEXT:              "valueKind": "RawLiteral",
// CHECK-NEXT:              "value": "128"
// CHECK-NEXT:            },
// CHECK-NEXT:            {
// CHECK-NEXT:              "label": "min",
// CHECK-NEXT:              "type": "Swift.Int",
// CHECK-NEXT:              "valueKind": "RawLiteral",
// CHECK-NEXT:              "value": "0"
// CHECK-NEXT:            },
// CHECK-NEXT:            {
// CHECK-NEXT:              "label": "max",
// CHECK-NEXT:              "type": "Swift.Int",
// CHECK-NEXT:              "valueKind": "RawLiteral",
// CHECK-NEXT:              "value": "255"
// CHECK-NEXT:            }
// CHECK-NEXT:          ]
// CHECK-NEXT:        },
// CHECK-NEXT:        "propertyWrappers": [
// CHECK-NEXT:          {
// CHECK-NEXT:            "type": "ExtractLiterals.Clamping",
// CHECK-NEXT:            "file": "{{.*}}test{{/|\\\\}}ConstExtraction{{/|\\\\}}ExtractLiterals.swift",
// CHECK-NEXT:            "line": 41,
// CHECK-NEXT:            "arguments": [
// CHECK-NEXT:              {
// CHECK-NEXT:                "label": "min",
// CHECK-NEXT:                "type": "Swift.Int",
// CHECK-NEXT:                "valueKind": "RawLiteral",
// CHECK-NEXT:                "value": "0"
// CHECK-NEXT:              },
// CHECK-NEXT:              {
// CHECK-NEXT:                "label": "max",
// CHECK-NEXT:                "type": "Swift.Int",
// CHECK-NEXT:                "valueKind": "RawLiteral",
// CHECK-NEXT:                "value": "255"
// CHECK-NEXT:              }
// CHECK-NEXT:            ]
// CHECK-NEXT:          }
// CHECK-NEXT:        ]
// CHECK-NEXT:      },
// CHECK-NEXT:      {
// CHECK-NEXT:        "label": "propertyWrapper3",
// CHECK-NEXT:        "type": "Swift.Int",
// CHECK-NEXT:        "mangledTypeName": "n/a - deprecated",
// CHECK-NEXT:        "isStatic": "false",
// CHECK-NEXT:        "isComputed": "true",
// CHECK-NEXT:        "file": "{{.*}}test{{/|\\\\}}ConstExtraction{{/|\\\\}}ExtractLiterals.swift",
// CHECK-NEXT:        "line": 45,
// CHECK-NEXT:        "valueKind": "InitCall",
// CHECK-NEXT:        "value": {
// CHECK-NEXT:          "type": "ExtractLiterals.Buffered<ExtractLiterals.Clamping<Swift.Int>>",
// CHECK-NEXT:          "arguments": [
// CHECK-NEXT:            {
// CHECK-NEXT:              "label": "wrappedValue",
// CHECK-NEXT:              "type": "ExtractLiterals.Clamping<Swift.Int>",
// CHECK-NEXT:              "valueKind": "InitCall",
// CHECK-NEXT:              "value": {
// CHECK-NEXT:                "type": "ExtractLiterals.Clamping<Swift.Int>",
// CHECK-NEXT:                "arguments": [
// CHECK-NEXT:                  {
// CHECK-NEXT:                    "label": "wrappedValue",
// CHECK-NEXT:                    "type": "Swift.Int",
// CHECK-NEXT:                    "valueKind": "RawLiteral",
// CHECK-NEXT:                    "value": "128"
// CHECK-NEXT:                  },
// CHECK-NEXT:                  {
// CHECK-NEXT:                    "label": "min",
// CHECK-NEXT:                    "type": "Swift.Int",
// CHECK-NEXT:                    "valueKind": "RawLiteral",
// CHECK-NEXT:                    "value": "0"
// CHECK-NEXT:                  },
// CHECK-NEXT:                  {
// CHECK-NEXT:                    "label": "max",
// CHECK-NEXT:                    "type": "Swift.Int",
// CHECK-NEXT:                    "valueKind": "RawLiteral",
// CHECK-NEXT:                    "value": "255"
// CHECK-NEXT:                  }
// CHECK-NEXT:                ]
// CHECK-NEXT:              }
// CHECK-NEXT:            }
// CHECK-NEXT:          ]
// CHECK-NEXT:        },
// CHECK-NEXT:        "propertyWrappers": [
// CHECK-NEXT:          {
// CHECK-NEXT:            "type": "ExtractLiterals.Buffered",
// CHECK-NEXT:            "file": "{{.*}}test{{/|\\\\}}ConstExtraction{{/|\\\\}}ExtractLiterals.swift",
// CHECK-NEXT:            "line": 44,
// CHECK-NEXT:            "arguments": []
// CHECK-NEXT:          },
// CHECK-NEXT:          {
// CHECK-NEXT:            "type": "ExtractLiterals.Clamping",
// CHECK-NEXT:            "file": "{{.*}}test{{/|\\\\}}ConstExtraction{{/|\\\\}}ExtractLiterals.swift",
// CHECK-NEXT:            "line": 44,
// CHECK-NEXT:            "arguments": [
// CHECK-NEXT:              {
// CHECK-NEXT:                "label": "min",
// CHECK-NEXT:                "type": "Swift.Int",
// CHECK-NEXT:                "valueKind": "RawLiteral",
// CHECK-NEXT:                "value": "0"
// CHECK-NEXT:              },
// CHECK-NEXT:              {
// CHECK-NEXT:                "label": "max",
// CHECK-NEXT:                "type": "Swift.Int",
// CHECK-NEXT:                "valueKind": "RawLiteral",
// CHECK-NEXT:                "value": "255"
// CHECK-NEXT:              }
// CHECK-NEXT:            ]
// CHECK-NEXT:          }
// CHECK-NEXT:        ]
// CHECK-NEXT:      },
// CHECK-NEXT:      {
// CHECK-NEXT:        "label": "$propertyWrapper3",
// CHECK-NEXT:        "type": "(ExtractLiterals.Clamping<Swift.Int>, Swift.Optional<ExtractLiterals.Clamping<Swift.Int>>)",
// CHECK-NEXT:        "mangledTypeName": "n/a - deprecated",
// CHECK-NEXT:        "isStatic": "false",
// CHECK-NEXT:        "isComputed": "true",
// CHECK-NEXT:        "file": "{{.*}}test{{/|\\\\}}ConstExtraction{{/|\\\\}}ExtractLiterals.swift",
// CHECK-NEXT:        "line": 45,
// CHECK-NEXT:        "valueKind": "Runtime"
// CHECK-NEXT:      }
// CHECK-NEXT:    ]
// CHECK-NEXT:  }
// CHECK-NEXT:]
