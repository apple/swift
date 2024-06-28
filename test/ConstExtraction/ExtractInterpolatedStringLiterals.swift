// RUN: %empty-directory(%t)
// RUN: echo "[MyProto]" > %t/protocols.json

// RUN: %target-swift-frontend -typecheck -emit-const-values-path %t/ExtractInterpolatedStringLiterals.swiftconstvalues -const-gather-protocols-file %t/protocols.json -primary-file %s
// RUN: cat %t/ExtractInterpolatedStringLiterals.swiftconstvalues 2>&1 | %FileCheck %s

protocol MyProto {}

func generateString(input: String) -> String {
    return "function" + input
}

public struct Internal: MyProto {
    static var internalTitle: String = "Inner title"
}

public struct MyType {
    var nested: NestedOne

    struct NestedOne {
        var foo: String
    }
}

public struct External: MyProto {
    var interpolatedTitle = "Start Interpolation with Member Reference: \(Internal.internalTitle). Followed By Function Call: \(generateString(input: "test")). End with KeyPath: \(\MyType.nested.foo)."
}

// CHECK: [
// CHECK-NEXT:   {
// CHECK-NEXT:     "typeName": "ExtractInterpolatedStringLiterals.Internal",
// CHECK-NEXT:     "mangledTypeName": "33ExtractInterpolatedStringLiterals8InternalV",
// CHECK-NEXT:     "kind": "struct",
// CHECK-NEXT:     "file": "{{.*}}test{{/|\\\\}}ConstExtraction{{/|\\\\}}ExtractInterpolatedStringLiterals.swift",
// CHECK-NEXT:     "line": 13,
// CHECK-NEXT:     "conformances": [
// CHECK-NEXT:       "ExtractInterpolatedStringLiterals.MyProto"
// CHECK-NEXT:     ],
// CHECK-NEXT:     "associatedTypeAliases": [],
// CHECK-NEXT:     "properties": [
// CHECK-NEXT:       {
// CHECK-NEXT:         "label": "internalTitle",
// CHECK-NEXT:         "type": "Swift.String",
// CHECK-NEXT:         "mangledTypeName": "n/a - deprecated",
// CHECK-NEXT:         "isStatic": "true",
// CHECK-NEXT:         "isComputed": "false",
// CHECK-NEXT:         "file": "{{.*}}test{{/|\\\\}}ConstExtraction{{/|\\\\}}ExtractInterpolatedStringLiterals.swift",
// CHECK-NEXT:         "line": 14,
// CHECK-NEXT:         "valueKind": "RawLiteral",
// CHECK-NEXT:         "value": "Inner title"
// CHECK-NEXT:       }
// CHECK-NEXT:     ]
// CHECK-NEXT:   },
// CHECK-NEXT:   {
// CHECK-NEXT:     "typeName": "ExtractInterpolatedStringLiterals.External",
// CHECK-NEXT:     "mangledTypeName": "33ExtractInterpolatedStringLiterals8ExternalV",
// CHECK-NEXT:     "kind": "struct",
// CHECK-NEXT:     "file": "{{.*}}test{{/|\\\\}}ConstExtraction{{/|\\\\}}ExtractInterpolatedStringLiterals.swift",
// CHECK-NEXT:     "line": 25,
// CHECK-NEXT:     "conformances": [
// CHECK-NEXT:       "ExtractInterpolatedStringLiterals.MyProto"
// CHECK-NEXT:     ],
// CHECK-NEXT:     "associatedTypeAliases": [],
// CHECK-NEXT:     "properties": [
// CHECK-NEXT:       {
// CHECK-NEXT:         "label": "interpolatedTitle",
// CHECK-NEXT:         "type": "Swift.String",
// CHECK-NEXT:         "mangledTypeName": "n/a - deprecated",
// CHECK-NEXT:         "isStatic": "false",
// CHECK-NEXT:         "isComputed": "false",
// CHECK-NEXT:         "file": "{{.*}}test{{/|\\\\}}ConstExtraction{{/|\\\\}}ExtractInterpolatedStringLiterals.swift",
// CHECK-NEXT:         "line": 26,
// CHECK-NEXT:         "valueKind": "InterpolatedStringLiteral",
// CHECK-NEXT:         "value": {
// CHECK-NEXT:           "segments": [
// CHECK-NEXT:             {
// CHECK-NEXT:               "valueKind": "RawLiteral",
// CHECK-NEXT:               "value": "Start Interpolation with Member Reference: "
// CHECK-NEXT:             },
// CHECK-NEXT:             {
// CHECK-NEXT:               "valueKind": "MemberReference",
// CHECK-NEXT:               "value": {
// CHECK-NEXT:                 "baseType": "ExtractInterpolatedStringLiterals.Internal",
// CHECK-NEXT:                 "memberLabel": "internalTitle"
// CHECK-NEXT:               }
// CHECK-NEXT:             },
// CHECK-NEXT:             {
// CHECK-NEXT:               "valueKind": "RawLiteral",
// CHECK-NEXT:               "value": ". Followed By Function Call: "
// CHECK-NEXT:             },
// CHECK-NEXT:             {
// CHECK-NEXT:               "valueKind": "FunctionCall",
// CHECK-NEXT:               "value": {
// CHECK-NEXT:                 "name": "generateString",
// CHECK-NEXT:                 "arguments": [
// CHECK-NEXT:                   {
// CHECK-NEXT:                     "label": "input",
// CHECK-NEXT:                     "type": "Swift.String",
// CHECK-NEXT:                     "valueKind": "RawLiteral",
// CHECK-NEXT:                     "value": "test"
// CHECK-NEXT:                   }
// CHECK-NEXT:                 ]
// CHECK-NEXT:               }
// CHECK-NEXT:             },
// CHECK-NEXT:             {
// CHECK-NEXT:               "valueKind": "RawLiteral",
// CHECK-NEXT:               "value": ". End with KeyPath: "
// CHECK-NEXT:             },
// CHECK-NEXT:             {
// CHECK-NEXT:               "valueKind": "KeyPath",
// CHECK-NEXT:               "value": {
// CHECK-NEXT:                 "path": "nested.foo",
// CHECK-NEXT:                 "rootType": "ExtractInterpolatedStringLiterals.MyType",
// CHECK-NEXT:                 "components": [
// CHECK-NEXT:                   {
// CHECK-NEXT:                     "label": "nested",
// CHECK-NEXT:                     "type": "ExtractInterpolatedStringLiterals.MyType.NestedOne"
// CHECK-NEXT:                   },
// CHECK-NEXT:                   {
// CHECK-NEXT:                     "label": "foo",
// CHECK-NEXT:                     "type": "Swift.String"
// CHECK-NEXT:                   }
// CHECK-NEXT:                 ]
// CHECK-NEXT:               }
// CHECK-NEXT:             },
// CHECK-NEXT:             {
// CHECK-NEXT:               "valueKind": "RawLiteral",
// CHECK-NEXT:               "value": "."
// CHECK-NEXT:             }
// CHECK-NEXT:           ]
// CHECK-NEXT:         }
// CHECK-NEXT:       }
// CHECK-NEXT:     ]
// CHECK-NEXT:   }
// CHECK-NEXT: ]
