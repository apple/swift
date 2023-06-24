// RUN: %target-swift-frontend(mock-sdk: %clang-importer-sdk) -emit-ir %s | %FileCheck %s
// REQUIRES: objc_interop

import Foundation

open class AllSwift {}

open class Mixed: NSObject {}

// CHECK-LABEL: define{{.*@.*}}14typeOfAllSwift
public func typeOfAllSwift(_ x: AllSwift) -> AllSwift.Type {
  // CHECK: [[ISA:%.*]] = load ptr
  // CHECK: ret ptr [[ISA]]
  return type(of: x)
}

// CHECK-LABEL: define{{.*@.*}}11typeOfMixed
public func typeOfMixed(_ x: Mixed) -> Mixed.Type {
  // CHECK: [[ISA:%.*]] = call ptr @swift_getObjectType
  // CHECK: ret ptr [[ISA]]
  return type(of: x)
}

// CHECK-LABEL: define{{.*@.*}}14typeOfNSObject
public func typeOfNSObject(_ x: NSObject) -> NSObject.Type {
  // CHECK: [[ISA:%.*]] = call ptr @swift_getObjectType
  // CHECK: ret ptr [[ISA]]
  return type(of: x)
}

// CHECK-LABEL: define{{.*@.*}}13typeOfUnknown
public func typeOfUnknown(_ x: AnyObject) -> AnyObject.Type {
  // CHECK: [[ISA:%.*]] = call ptr @swift_getObjectType
  // CHECK: ret ptr [[ISA]]
  return type(of: x)
}
