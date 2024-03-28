// Test doesn't pass on all platforms (rdar://101543397)
// REQUIRES: OS=macosx
// UNSUPPORTED: use_os_stdlib
// UNSUPPORTED: back_deployment_runtime

//
// Build objc_implementation.framework
//
// RUN: %empty-directory(%t)
// RUN: %empty-directory(%t/frameworks)
// RUN: %empty-directory(%t/frameworks/objc_implementation.framework/Modules/objc_implementation.swiftmodule)
// RUN: %empty-directory(%t/frameworks/objc_implementation.framework/Headers)
// RUN: cp %S/Inputs/objc_implementation.modulemap %t/frameworks/objc_implementation.framework/Modules/module.modulemap
// RUN: cp %S/Inputs/objc_implementation.h %t/frameworks/objc_implementation.framework/Headers
// RUN: %target-build-swift-dylib(%t/frameworks/objc_implementation.framework/objc_implementation) -enable-experimental-feature CImplementation -emit-module-path %t/frameworks/objc_implementation.framework/Modules/objc_implementation.swiftmodule/%module-target-triple.swiftmodule -module-name objc_implementation -F %t/frameworks -import-underlying-module -Xlinker -install_name -Xlinker %t/frameworks/objc_implementation.framework/objc_implementation %S/objc_implementation.swift

//
// Execute this file
//
// RUN: %empty-directory(%t/swiftmod)
// RUN: %target-build-swift %s -module-cache-path %t/swiftmod/mcp -F %t/frameworks -o %t/swiftmod/a.out -module-name main
// RUN: %target-codesign %t/swiftmod/a.out
// RUN: %target-run %t/swiftmod/a.out | %FileCheck %s

//
// Execute again, without the swiftmodule this time
//
// RUN: mv %t/frameworks/objc_implementation.framework/Modules/objc_implementation.swiftmodule %t/frameworks/objc_implementation.framework/Modules/objc_implementation.swiftmodule.disabled
// RUN: %empty-directory(%t/clangmod)
// RUN: %target-build-swift %s -module-cache-path %t/clangmod/mcp -F %t/frameworks -o %t/clangmod/a.out -module-name main
// RUN: %target-codesign %t/clangmod/a.out
// RUN: %target-run %t/clangmod/a.out | %FileCheck %s

// REQUIRES: executable_test
// REQUIRES: objc_interop

import Foundation
import objc_implementation

ImplClass.runTests()
// CHECK: someMethod = ImplClass.someMethod()
// CHECK: implProperty = 0
// CHECK: implProperty = 42
// CHECK: someMethod = SwiftSubclass.someMethod()
// CHECK: implProperty = 0
// CHECK: implProperty = 42
// CHECK: otherProperty = 1
// CHECK: otherProperty = 13
// CHECK: implProperty = 42

let impl = ImplClass()
print(impl.someMethod(), impl.implProperty)
// CHECK: ImplClass.someMethod() 0

class SwiftClientSubclass: ImplClass {
  override init() {}
  var otherProperty = 2
  override func someMethod() -> String { "SwiftClientSubclass.someMethod()" }
}

let swiftClientSub = SwiftClientSubclass()
print(swiftClientSub.someMethod())
// CHECK: SwiftClientSubclass.someMethod()
print(swiftClientSub.implProperty, swiftClientSub.otherProperty)
// CHECK: 0 2
swiftClientSub.implProperty = 3
swiftClientSub.otherProperty = 9
print(swiftClientSub.implProperty, swiftClientSub.otherProperty)
// CHECK: 3 9

