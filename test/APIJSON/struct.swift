// REQUIRES: objc_interop, OS=macosx
// RUN: %empty-directory(%t)
// RUN: %empty-directory(%t/ModuleCache)
// RUN: %target-swift-frontend %s -typecheck -parse-as-library -emit-module-interface-path %t/MyModule.swiftinterface -enable-library-evolution -module-name MyModule -swift-version 5
// RUN: %target-swift-frontend %s -typecheck -parse-as-library -emit-module-interface-path %t/MyModule.swiftinterface -enable-library-evolution -module-name MyModule -swift-version 5 -emit-api-descriptor-path %t/api.json
// RUN: %validate-json %t/api.json | %FileCheck %s

// Struct has no objc data.
@available(macOS 10.13, *)
public struct TestStruct {
  public init() {}

  @available(macOS 10.14, *)
  public func testMethod() {}
}

// CHECK:      {
// CHECK-NEXT:   "target":
// CHECK-NEXT:   "globals": [
// CHECK-NEXT:     {
// CHECK-NEXT:       "name": "_$s8MyModule10TestStructV10testMethodyyF",
// CHECK-NEXT:       "access": "public",
// CHECK-NEXT:       "file": "SOURCE_DIR/test/APIJSON/struct.swift",
// CHECK-NEXT:       "linkage": "exported",
// CHECK-NEXT:       "introduced": "10.14"
// CHECK-NEXT:     },
// CHECK-NEXT:     {
// CHECK-NEXT:       "name": "_$s8MyModule10TestStructVACycfC",
// CHECK-NEXT:       "access": "public",
// CHECK-NEXT:       "file": "SOURCE_DIR/test/APIJSON/struct.swift",
// CHECK-NEXT:       "linkage": "exported"
// CHECK-NEXT:     },
// CHECK-NEXT:     {
// CHECK-NEXT:       "name": "_$s8MyModule10TestStructVMa",
// CHECK-NEXT:       "access": "public",
// CHECK-NEXT:       "file": "SOURCE_DIR/test/APIJSON/struct.swift",
// CHECK-NEXT:       "linkage": "exported",
// CHECK-NEXT:       "introduced": "10.13"
// CHECK-NEXT:     },
// CHECK-NEXT:     {
// CHECK-NEXT:       "name": "_$s8MyModule10TestStructVMn",
// CHECK-NEXT:       "access": "public",
// CHECK-NEXT:       "file": "SOURCE_DIR/test/APIJSON/struct.swift",
// CHECK-NEXT:       "linkage": "exported",
// CHECK-NEXT:       "introduced": "10.13"
// CHECK-NEXT:     },
// CHECK-NEXT:     {
// CHECK-NEXT:       "name": "_$s8MyModule10TestStructVN",
// CHECK-NEXT:       "access": "public",
// CHECK-NEXT:       "file": "SOURCE_DIR/test/APIJSON/struct.swift",
// CHECK-NEXT:       "linkage": "exported",
// CHECK-NEXT:       "introduced": "10.13"
// CHECK-NEXT:     }
// CHECK-NEXT:   ],
// CHECK-NEXT:   "interfaces": [],
// CHECK-NEXT:   "categories": [],
// CHECK-NEXT:   "version": "1.0"
// CHECK-NEXT: }
