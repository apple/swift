// REQUIRES: objc_interop

// RUN: %empty-directory(%t)
// RUN: %empty-directory(%t/SwiftAttr)
// RUN: split-file %s %t

// RUN: %target-swift-frontend(mock-sdk: %clang-importer-sdk) -emit-module -emit-module-path %t/SwiftAttrExtension.swiftmodule -enable-objc-interop -I %t/SwiftAttr -module-name SwiftAttrExtension -emit-symbol-graph -emit-symbol-graph-dir %t %t/Extension.swift
// RUN: %FileCheck %s --input-file %t/SwiftAttrExtension@SwiftAttr.symbols.json

//--- SwiftAttr/module.modulemap
module SwiftAttr {
  header "SwiftAttr.h"
}

//--- SwiftAttr/SwiftAttr.h
@import Foundation;

__attribute__((swift_attr("@_nonSendable(_assumed)")))
@interface MyObjcClass : NSObject
@end

//--- Extension.swift
import SwiftAttr

extension MyObjcClass {
  // CHECK: s:So11MyObjcClassC18SwiftAttrExtensionE8someFuncyyF
  public func someFunc() {}
}
