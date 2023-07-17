// RUN: %empty-directory(%t)
// RUN: %target-swift-frontend -scan-dependencies %/s %/S/Inputs/unicode_filёnamё.swift -o %t/deps.json

// Check the contents of the JSON output
// RUN: %validate-json %t/deps.json &>/dev/null
// RUN: %FileCheck %s < %t/deps.json

public func bar() {
    print(foo())
}

// CHECK:      "swift": "deps"
// CHECK-NEXT:    },
// CHECK-NEXT:    {
// CHECK-NEXT:      "modulePath": "deps.swiftmodule",
// CHECK-NEXT:      "sourceFiles": [
// CHECK-NEXT:        "{{.*}}ScanDependencies/unicode_filename.swift",
// CHECK-NEXT:        "{{.*}}ScanDependencies/Inputs/unicode_filёnamё.swift"
// CHECK-NEXT:      ],
