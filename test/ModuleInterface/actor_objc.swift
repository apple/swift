// RUN: %empty-directory(%t)
// RUN: %target-swift-frontend -emit-module -o %t/Test.swiftmodule -emit-module-interface-path %t/Test.swiftinterface -module-name Test -enable-experimental-concurrency %s
// RUN: %FileCheck %s < %t/Test.swiftinterface
// RUN: %target-swift-frontend -typecheck-module-from-interface -module-name Test %t/Test.swiftinterface

// RUN: %target-swift-frontend -emit-module -o /dev/null -merge-modules %t/Test.swiftmodule -disable-objc-attr-requires-foundation-module -emit-module-interface-path %t/TestFromModule.swiftinterface -module-name Test -enable-experimental-concurrency
// RUN: %FileCheck %s < %t/TestFromModule.swiftinterface
// RUN: %target-swift-frontend -typecheck-module-from-interface -module-name Test %t/TestFromModule.swiftinterface

// REQUIRES: concurrency
// REQUIRES: objc_interop

import Foundation

// CHECK-LABEL: @objc @_inheritsConvenienceInitializers public actor SomeActor : ObjectiveC.NSObject {
// CHECK: @objc override public init()
public actor SomeActor: NSObject {
}
