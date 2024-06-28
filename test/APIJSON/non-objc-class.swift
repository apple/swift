// REQUIRES: objc_interop, OS=macosx
// RUN: %empty-directory(%t)
// RUN: %empty-directory(%t/ModuleCache)
// RUN: %target-swift-frontend(mock-sdk: %clang-importer-sdk-nosource -I %t) %s -typecheck -parse-as-library -emit-module-interface-path %t/MyModule.swiftinterface -enable-library-evolution -module-name MyModule -swift-version 5
// RUN: %target-swift-frontend(mock-sdk: %clang-importer-sdk-nosource -I %t) %s -typecheck -parse-as-library -emit-module-interface-path %t/MyModule.swiftinterface -enable-library-evolution -module-name MyModule -swift-version 5 -emit-api-descriptor-path %t/api.json
// RUN: %validate-json %t/api.json | %FileCheck %s

import Foundation

// Need objc data for objc compatible methods.
public class NonObjC {
  @objc public init() {}
  @objc public func testMethod() {}
}

public class NonObjC1 {
  @objc public func testMethod() {}
}

public class NonObjC2 {
  @objc public func testMethod() {}
  public init() {}
}

public class NonObjC3 {
  @objc public init() {}
  public init(fromInt: Int) {}
}

// CHECK:      "interfaces": [
// CHECK-NEXT:   {
// CHECK-NEXT:     "name": "_TtC8MyModule7NonObjC",
// CHECK-NEXT:     "access": "public",
// CHECK-NEXT:     "file": "SOURCE_DIR/test/APIJSON/non-objc-class.swift",
// CHECK-NEXT:     "linkage": "internal",
// CHECK-NEXT:     "super": "",
// CHECK-NEXT:     "instanceMethods": [
// CHECK-NEXT:       {
// CHECK-NEXT:         "name": "init",
// CHECK-NEXT:         "access": "public",
// CHECK-NEXT:         "file": "SOURCE_DIR/test/APIJSON/non-objc-class.swift"
// CHECK-NEXT:       },
// CHECK-NEXT:       {
// CHECK-NEXT:         "name": "testMethod",
// CHECK-NEXT:         "access": "public",
// CHECK-NEXT:         "file": "SOURCE_DIR/test/APIJSON/non-objc-class.swift"
// CHECK-NEXT:       }
// CHECK-NEXT:     ],
// CHECK-NEXT:     "classMethods": []
// CHECK-NEXT:   },
// CHECK-NEXT:   {
// CHECK-NEXT:     "name": "_TtC8MyModule8NonObjC1",
// CHECK-NEXT:     "access": "public",
// CHECK-NEXT:     "file": "SOURCE_DIR/test/APIJSON/non-objc-class.swift",
// CHECK-NEXT:     "linkage": "internal",
// CHECK-NEXT:     "super": "",
// CHECK-NEXT:     "instanceMethods": [
// CHECK-NEXT:       {
// CHECK-NEXT:         "name": "testMethod",
// CHECK-NEXT:         "access": "public",
// CHECK-NEXT:         "file": "SOURCE_DIR/test/APIJSON/non-objc-class.swift"
// CHECK-NEXT:       }
// CHECK-NEXT:     ],
// CHECK-NEXT:     "classMethods": []
// CHECK-NEXT:   },
// CHECK-NEXT:   {
// CHECK-NEXT:     "name": "_TtC8MyModule8NonObjC2",
// CHECK-NEXT:     "access": "public",
// CHECK-NEXT:     "file": "SOURCE_DIR/test/APIJSON/non-objc-class.swift",
// CHECK-NEXT:     "linkage": "internal",
// CHECK-NEXT:     "super": "",
// CHECK-NEXT:     "instanceMethods": [
// CHECK-NEXT:       {
// CHECK-NEXT:         "name": "testMethod",
// CHECK-NEXT:         "access": "public",
// CHECK-NEXT:         "file": "SOURCE_DIR/test/APIJSON/non-objc-class.swift"
// CHECK-NEXT:       }
// CHECK-NEXT:     ],
// CHECK-NEXT:     "classMethods": []
// CHECK-NEXT:   },
// CHECK-NEXT:   {
// CHECK-NEXT:     "name": "_TtC8MyModule8NonObjC3",
// CHECK-NEXT:     "access": "public",
// CHECK-NEXT:     "file": "SOURCE_DIR/test/APIJSON/non-objc-class.swift",
// CHECK-NEXT:     "linkage": "internal",
// CHECK-NEXT:     "super": "",
// CHECK-NEXT:     "instanceMethods": [
// CHECK-NEXT:       {
// CHECK-NEXT:         "name": "init",
// CHECK-NEXT:         "access": "public",
// CHECK-NEXT:         "file": "SOURCE_DIR/test/APIJSON/non-objc-class.swift"
// CHECK-NEXT:       }
// CHECK-NEXT:     ],
// CHECK-NEXT:     "classMethods": []
// CHECK-NEXT:   }
// CHECK-NEXT: ]

