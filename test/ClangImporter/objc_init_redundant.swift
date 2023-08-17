// RUN: %target-swift-frontend(mock-sdk: %clang-importer-sdk -I %S/Inputs/custom-modules -F %S/Inputs/frameworks) -import-underlying-module -import-objc-header %S/Inputs/objc_init_redundant_bridging.h -emit-sil %s -verify
// RUN: not %target-swift-frontend(mock-sdk: %clang-importer-sdk -I %S/Inputs/custom-modules -F %S/Inputs/frameworks) -import-underlying-module -import-objc-header %S/Inputs/objc_init_redundant_bridging.h -emit-sil %s > %t.log 2>&1
// RUN: %FileCheck %s < %t.log

// REQUIRES: objc_interop

import Foundation
@_implementationOnly import objc_init_redundant_Private

// rdar://problem/17687082
extension NSObject {
  @objc convenience init() { self.init() } // expected-error{{initializer 'init()' with Objective-C selector 'init' conflicts with previous declaration with the same Objective-C selector}}
// CHECK: objc_init_redundant.swift:[[@LINE-1]]:21: error: initializer 'init()' with Objective-C selector 'init' conflicts
// CHECK: ObjectiveC.NSObject:{{.*}}note: 'init()' previously declared here
}

extension NSObject {
  @objc(class) func foo() { } // expected-error{{method 'foo()' with Objective-C selector 'class' conflicts with method 'class()' with the same Objective-C selector}}
// CHECK: objc_init_redundant.swift:[[@LINE-1]]:21: error: method 'foo()' with Objective-C selector 'class' conflicts
// CHECK: ObjectiveC.NSObjectProtocol:{{.*}}note: method 'class()' declared here
}

// rdar://96470068 - Don't want conflict diagnostics in the same module
extension MyObject {
  @objc func implementedInSwift() {}
}

// ...or the bridging header
extension MyBridgedObject {
  @objc func implementedInSwift() {}
}

// ...or the private module
extension MyPrivateObject {
  @objc func implementedInSwift() {}
}
