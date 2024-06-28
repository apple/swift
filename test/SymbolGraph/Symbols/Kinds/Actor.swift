// RUN: %empty-directory(%t)
// RUN: %target-build-swift %s -module-name Actor -emit-module -emit-module-path %t/
// RUN: %target-swift-symbolgraph-extract -module-name Actor -I %t -pretty-print -output-dir %t
// RUN: %FileCheck %s --input-file %t/Actor.symbols.json

// CHECK: "identifier": "swift.class"
// CHECK-NEXT: "displayName": "Class"
// CHECK: pathComponents
// CHECK-NEXT: "Q"
// CHECK: "kind": "keyword"
// CHECK-NEXT: "spelling": "actor"
@available(macOS 12.0, iOS 15.0, watchOS 8.0, tvOS 15.0, *)
public actor Q {}
