// RUN: %empty-directory(%t)
// RUN: echo "[MyProto]" > %t/protocols.json

// RUN: %target-swift-frontend -typecheck -emit-const-values-path %t/ExtractAnnotations.swiftconstvalues -const-gather-protocols-file %t/protocols.json -primary-file %s
// RUN: cat %t/ExtractAnnotations.swiftconstvalues 2>&1 | %FileCheck %s

protocol MyProto {}

public struct Annotations: MyProto {
    @available(iOS 10.0, OSX 10.12, *)
    var available1: String { "available" }

    @available(iOS, deprecated: 10.0)
    @available(OSX, deprecated: 10.12)
    var deprecated1: String { "deprecated" }

    @available(*, unavailable, renamed: "available1", message: "Please use available1")
    var renamed1: String { "renamed" }

    @available(iOS, introduced: 10.0.0)
    var introduced1: String { "introduced" }
}

@available(iOS, deprecated: 10.0, renamed: "Annotations", message: "Please use Annotations")
@available(OSX, deprecated: 10.12, renamed: "Annotations", message: "Please use Annotations")
@available(tvOS, deprecated)
@available(*, unavailable)
public struct DeprecatedAnnotations: MyProto {}

// CHECK: [
// CHECK-NEXT:  {
// CHECK-NEXT:    "typeName": "ExtractAnnotations.Annotations",
// CHECK-NEXT:    "mangledTypeName": "18ExtractAnnotations0B0V",
// CHECK-NEXT:    "kind": "struct",
// CHECK-NEXT:    "file": "{{.*}}test{{/|\\\\}}ConstExtraction{{/|\\\\}}ExtractAnnotations.swift",
// CHECK-NEXT:    "line": 9,
// CHECK-NEXT:    "conformances": [
// CHECK-NEXT:      "ExtractAnnotations.MyProto"
// CHECK-NEXT:    ],
// CHECK-NEXT:    "associatedTypeAliases": [],
// CHECK-NEXT:    "properties": [
// CHECK-NEXT:      {
// CHECK-NEXT:        "label": "available1",
// CHECK-NEXT:        "type": "Swift.String",
// CHECK-NEXT:        "mangledTypeName": "n/a - deprecated",
// CHECK-NEXT:        "isStatic": "false",
// CHECK-NEXT:        "isComputed": "true",
// CHECK-NEXT:        "file": "{{.*}}test{{/|\\\\}}ConstExtraction{{/|\\\\}}ExtractAnnotations.swift",
// CHECK-NEXT:        "line": 11,
// CHECK-NEXT:        "valueKind": "RawLiteral",
// CHECK-NEXT:        "value": "available",
// CHECK-NEXT:        "availabilityAttributes": [
// CHECK-NEXT:          {
// CHECK-NEXT:            "platform": "macOS",
// CHECK-NEXT:            "introducedVersion": "10.12",
// CHECK-NEXT:            "isUnavailable": false,
// CHECK-NEXT:            "isDeprecated": false
// CHECK-NEXT:          },
// CHECK-NEXT:          {
// CHECK-NEXT:            "platform": "iOS",
// CHECK-NEXT:            "introducedVersion": "10.0",
// CHECK-NEXT:            "isUnavailable": false,
// CHECK-NEXT:            "isDeprecated": false
// CHECK-NEXT:          }
// CHECK-NEXT:        ]
// CHECK-NEXT:      },
// CHECK-NEXT:      {
// CHECK-NEXT:        "label": "deprecated1",
// CHECK-NEXT:        "type": "Swift.String",
// CHECK-NEXT:        "mangledTypeName": "n/a - deprecated",
// CHECK-NEXT:        "isStatic": "false",
// CHECK-NEXT:        "isComputed": "true",
// CHECK-NEXT:        "file": "{{.*}}test{{/|\\\\}}ConstExtraction{{/|\\\\}}ExtractAnnotations.swift",
// CHECK-NEXT:        "line": 15,
// CHECK-NEXT:        "valueKind": "RawLiteral",
// CHECK-NEXT:        "value": "deprecated",
// CHECK-NEXT:        "availabilityAttributes": [
// CHECK-NEXT:          {
// CHECK-NEXT:            "platform": "macOS",
// CHECK-NEXT:            "deprecatedVersion": "10.12",
// CHECK-NEXT:            "isUnavailable": false,
// CHECK-NEXT:            "isDeprecated": false
// CHECK-NEXT:          },
// CHECK-NEXT:          {
// CHECK-NEXT:            "platform": "iOS",
// CHECK-NEXT:            "deprecatedVersion": "10.0",
// CHECK-NEXT:            "isUnavailable": false,
// CHECK-NEXT:            "isDeprecated": false
// CHECK-NEXT:          }
// CHECK-NEXT:        ]
// CHECK-NEXT:      },
// CHECK-NEXT:      {
// CHECK-NEXT:        "label": "renamed1",
// CHECK-NEXT:        "type": "Swift.String",
// CHECK-NEXT:        "mangledTypeName": "n/a - deprecated",
// CHECK-NEXT:        "isStatic": "false",
// CHECK-NEXT:        "isComputed": "true",
// CHECK-NEXT:        "file": "{{.*}}test{{/|\\\\}}ConstExtraction{{/|\\\\}}ExtractAnnotations.swift",
// CHECK-NEXT:        "line": 18,
// CHECK-NEXT:        "valueKind": "RawLiteral",
// CHECK-NEXT:        "value": "renamed",
// CHECK-NEXT:        "availabilityAttributes": [
// CHECK-NEXT:          {
// CHECK-NEXT:            "platform": "*",
// CHECK-NEXT:            "message": "Please use available1",
// CHECK-NEXT:            "rename": "available1",
// CHECK-NEXT:            "isUnavailable": true,
// CHECK-NEXT:            "isDeprecated": false
// CHECK-NEXT:          }
// CHECK-NEXT:        ]
// CHECK-NEXT:      },
// CHECK-NEXT:      {
// CHECK-NEXT:        "label": "introduced1",
// CHECK-NEXT:        "type": "Swift.String",
// CHECK-NEXT:        "mangledTypeName": "n/a - deprecated",
// CHECK-NEXT:        "isStatic": "false",
// CHECK-NEXT:        "isComputed": "true",
// CHECK-NEXT:        "file": "{{.*}}test{{/|\\\\}}ConstExtraction{{/|\\\\}}ExtractAnnotations.swift",
// CHECK-NEXT:        "line": 21,
// CHECK-NEXT:        "valueKind": "RawLiteral",
// CHECK-NEXT:        "value": "introduced",
// CHECK-NEXT:        "availabilityAttributes": [
// CHECK-NEXT:          {
// CHECK-NEXT:            "platform": "iOS",
// CHECK-NEXT:            "introducedVersion": "10.0.0",
// CHECK-NEXT:            "isUnavailable": false,
// CHECK-NEXT:            "isDeprecated": false
// CHECK-NEXT:          }
// CHECK-NEXT:        ]
// CHECK-NEXT:      }
// CHECK-NEXT:    ]
// CHECK-NEXT:  },
// CHECK-NEXT:  {
// CHECK-NEXT:    "typeName": "ExtractAnnotations.DeprecatedAnnotations",
// CHECK-NEXT:    "mangledTypeName": "18ExtractAnnotations010DeprecatedB0V",
// CHECK-NEXT:    "kind": "struct",
// CHECK-NEXT:    "file": "{{.*}}test{{/|\\\\}}ConstExtraction{{/|\\\\}}ExtractAnnotations.swift",
// CHECK-NEXT:    "line": 28,
// CHECK-NEXT:    "conformances": [
// CHECK-NEXT:      "ExtractAnnotations.MyProto"
// CHECK-NEXT:    ],
// CHECK-NEXT:    "associatedTypeAliases": [],
// CHECK-NEXT:    "properties": [],
// CHECK-NEXT:    "availabilityAttributes": [
// CHECK-NEXT:      {
// CHECK-NEXT:        "platform": "*",
// CHECK-NEXT:        "isUnavailable": true,
// CHECK-NEXT:        "isDeprecated": false
// CHECK-NEXT:      },
// CHECK-NEXT:      {
// CHECK-NEXT:        "platform": "tvOS",
// CHECK-NEXT:        "isUnavailable": false,
// CHECK-NEXT:        "isDeprecated": true
// CHECK-NEXT:      },
// CHECK-NEXT:      {
// CHECK-NEXT:        "platform": "macOS",
// CHECK-NEXT:        "message": "Please use Annotations",
// CHECK-NEXT:        "rename": "Annotations",
// CHECK-NEXT:        "deprecatedVersion": "10.12",
// CHECK-NEXT:        "isUnavailable": false,
// CHECK-NEXT:        "isDeprecated": false
// CHECK-NEXT:      },
// CHECK-NEXT:      {
// CHECK-NEXT:        "platform": "iOS",
// CHECK-NEXT:        "message": "Please use Annotations",
// CHECK-NEXT:        "rename": "Annotations",
// CHECK-NEXT:        "deprecatedVersion": "10.0",
// CHECK-NEXT:        "isUnavailable": false,
// CHECK-NEXT:        "isDeprecated": false
// CHECK-NEXT:      }
// CHECK-NEXT:    ]
// CHECK-NEXT:  }
// CHECK-NEXT:]
