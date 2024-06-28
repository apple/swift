// REQUIRES: objc_interop, OS=macosx
// RUN: %empty-directory(%t)
// RUN: %empty-directory(%t/ModuleCache)
// RUN: %target-swift-frontend(mock-sdk: %clang-importer-sdk-nosource -I %t) %s -typecheck -parse-as-library -emit-module-interface-path %t/MyModule.swiftinterface -enable-library-evolution -module-name MyModule -swift-version 5
// RUN: %target-swift-frontend(mock-sdk: %clang-importer-sdk-nosource -I %t) %s -typecheck -parse-as-library -emit-module-interface-path %t/MyModule.swiftinterface -enable-library-evolution -module-name MyModule -swift-version 5 -emit-api-descriptor-path %t/api.json
// RUN: %validate-json %t/api.json | %FileCheck %s

import Foundation

// This should create an ObjC Category and a method with custom name.
extension NSDictionary {
  @objc
  public subscript(key: Any) -> Any? {
    @objc(__custom_name:)
    get { return nil }
  }
}

// This shouldn't create an interface.
public class A {}

// This shouldn't create a category.
extension A {
  public func run() {}
}

// This creates an interface.
public class B: NSObject {}

// This creates a category.
@objc
extension B {
  public func run() {}
}

// This shouldn't create a category.
extension B {
  public func noop() {}
}

// This creates a category with index 1.
@objc
extension B {
  public func fun() {}
}

// This creates a category with a custom name.
@objc(CustomCategoryName)
extension B {
  public func doIt() {}
}

// CHECK:      "interfaces": [
// CHECK-NEXT:    {
// CHECK-NEXT:      "name": "_TtC8MyModule1B",
// CHECK-NEXT:      "access": "public",
// CHECK-NEXT:      "file": "SOURCE_DIR/test/APIJSON/extension.swift",
// CHECK-NEXT:      "linkage": "exported",
// CHECK-NEXT:      "super": "NSObject",
// CHECK-NEXT:      "instanceMethods": [
// CHECK-NEXT:        {
// CHECK-NEXT:          "name": "init",
// CHECK-NEXT:          "access": "public",
// CHECK-NEXT:          "file": "SOURCE_DIR/test/APIJSON/extension.swift"
// CHECK-NEXT:        }
// CHECK-NEXT:      ],
// CHECK-NEXT:      "classMethods": []
// CHECK-NEXT:    }
// CHECK-NEXT:  ],
// CHECK-NEXT:  "categories": [
// CHECK-NEXT:    {
// CHECK-NEXT:      "name": "CustomCategoryName",
// CHECK-NEXT:      "access": "public",
// CHECK-NEXT:      "file": "SOURCE_DIR/test/APIJSON/extension.swift",
// CHECK-NEXT:      "linkage": "exported",
// CHECK-NEXT:      "interface": "_TtC8MyModule1B",
// CHECK-NEXT:      "instanceMethods": [
// CHECK-NEXT:        {
// CHECK-NEXT:          "name": "doIt",
// CHECK-NEXT:          "access": "public",
// CHECK-NEXT:          "file": "SOURCE_DIR/test/APIJSON/extension.swift"
// CHECK-NEXT:        }
// CHECK-NEXT:      ],
// CHECK-NEXT:      "classMethods": []
// CHECK-NEXT:    },
// CHECK-NEXT:    {
// CHECK-NEXT:      "name": "MyModule",
// CHECK-NEXT:      "access": "public",
// CHECK-NEXT:      "file": "SOURCE_DIR/test/APIJSON/extension.swift",
// CHECK-NEXT:      "linkage": "exported",
// CHECK-NEXT:      "interface": "NSDictionary",
// CHECK-NEXT:      "instanceMethods": [
// CHECK-NEXT:        {
// CHECK-NEXT:          "name": "__custom_name:",
// CHECK-NEXT:          "access": "public",
// CHECK-NEXT:          "file": "SOURCE_DIR/test/APIJSON/extension.swift"
// CHECK-NEXT:        }
// CHECK-NEXT:      ],
// CHECK-NEXT:      "classMethods": []
// CHECK-NEXT:    },
// CHECK-NEXT:    {
// CHECK-NEXT:      "name": "MyModule",
// CHECK-NEXT:      "access": "public",
// CHECK-NEXT:      "file": "SOURCE_DIR/test/APIJSON/extension.swift",
// CHECK-NEXT:      "linkage": "exported",
// CHECK-NEXT:      "interface": "_TtC8MyModule1B",
// CHECK-NEXT:      "instanceMethods": [
// CHECK-NEXT:        {
// CHECK-NEXT:          "name": "run",
// CHECK-NEXT:          "access": "public",
// CHECK-NEXT:          "file": "SOURCE_DIR/test/APIJSON/extension.swift"
// CHECK-NEXT:        }
// CHECK-NEXT:      ],
// CHECK-NEXT:      "classMethods": []
// CHECK-NEXT:    },
// CHECK-NEXT:    {
// CHECK-NEXT:      "name": "MyModule1",
// CHECK-NEXT:      "access": "public",
// CHECK-NEXT:      "file": "SOURCE_DIR/test/APIJSON/extension.swift",
// CHECK-NEXT:      "linkage": "exported",
// CHECK-NEXT:      "interface": "_TtC8MyModule1B",
// CHECK-NEXT:      "instanceMethods": [
// CHECK-NEXT:        {
// CHECK-NEXT:          "name": "fun",
// CHECK-NEXT:          "access": "public",
// CHECK-NEXT:          "file": "SOURCE_DIR/test/APIJSON/extension.swift"
// CHECK-NEXT:        }
// CHECK-NEXT:      ],
// CHECK-NEXT:      "classMethods": []
// CHECK-NEXT:    }
// CHECK-NEXT:  ],
