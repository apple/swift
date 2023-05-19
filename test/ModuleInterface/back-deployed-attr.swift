// RUN: %empty-directory(%t)
// RUN: %target-swift-emit-module-interface(%t/Test.swiftinterface) %s \
// RUN:   -define-availability "_macOS12_1:macOS 12.1" \
// RUN:   -define-availability "_myProject 1.0:macOS 12.1, iOS 15.1"
// RUN: %target-swift-typecheck-module-from-interface(%t/Test.swiftinterface)
// RUN: %FileCheck %s < %t/Test.swiftinterface

// REQUIRES: OS=macosx

public struct TopLevelStruct {
  // CHECK: @backDeployed(before: macOS 12.0)
  // CHECK: public func backDeployedFunc_SinglePlatform() -> Swift.Int { return 41 }
  @backDeployed(before: macOS 12.0)
  public func backDeployedFunc_SinglePlatform() -> Int { return 41 }
  
  // CHECK: @backDeployed(before: macOS 12.0, iOS 15.0)
  // CHECK: public func backDeployedFunc_MultiPlatform() -> Swift.Int { return 42 }
  @backDeployed(before: macOS 12.0, iOS 15.0)
  public func backDeployedFunc_MultiPlatform() -> Int { return 42 }

  // CHECK: @backDeployed(before: macOS 12.0, iOS 15.0, watchOS 8.0, tvOS 15.0)
  // CHECK: public func backDeployedFunc_MultiPlatformSeparate() -> Swift.Int { return 43 }
  @backDeployed(before: macOS 12.0)
  @backDeployed(before: iOS 15.0)
  @backDeployed(before: watchOS 8.0)
  @backDeployed(before: tvOS 15.0)
  public func backDeployedFunc_MultiPlatformSeparate() -> Int { return 43 }

  // CHECK: @backDeployed(before: macOS 12.0)
  // CHECK: public var backDeployedComputedProperty: Swift.Int {
  // CHECK-NEXT:   get { 44 }
  // CHECK-NEXT: }
  @backDeployed(before: macOS 12.0)
  public var backDeployedComputedProperty: Int { 44 }

  // CHECK: @backDeployed(before: macOS 12.0)
  // CHECK: public var backDeployedPropertyWithAccessors: Swift.Int {
  // CHECK-NEXT:   get { 45 }
  // CHECK-NEXT: }
  @backDeployed(before: macOS 12.0)
  public var backDeployedPropertyWithAccessors: Int {
    get { 45 }
  }

  // CHECK: @backDeployed(before: macOS 12.0)
  // CHECK: public subscript(index: Swift.Int) -> Swift.Int {
  // CHECK-NEXT:   get { 46 }
  // CHECK-NEXT: }
  @backDeployed(before: macOS 12.0)
  public subscript(index: Int) -> Int {
    get { 46 }
  }
}

// CHECK: @backDeployed(before: iOS 15.0)
// CHECK: public func backDeployedFunc_iOSOnly() -> Swift.Int
// CHECK-NOT: return 99
@backDeployed(before: iOS 15.0)
public func backDeployedFunc_iOSOnly() -> Int { return 99 }

// CHECK: @backDeployed(before: macOS 12.0)
// CHECK: public func backDeployTopLevelFunc_macOS() -> Swift.Int {
// CHECK-NEXT:   return 47
// CHECK-NEXT: }
// CHECK-NOT: fatalError()
@backDeployed(before: macOS 12.0)
public func backDeployTopLevelFunc_macOS() -> Int {
#if os(macOS)
  return 47
#else
  fatalError()
#endif
}

// MARK: - Availability macros

// CHECK: @backDeployed(before: macOS 12.1)
// CHECK: public func backDeployTopLevelFunc_macOS12_1() -> Swift.Int { return 48 }
@backDeployed(before: _macOS12_1)
public func backDeployTopLevelFunc_macOS12_1() -> Int { return 48 }

// CHECK: @backDeployed(before: macOS 12.1, iOS 15.1)
// CHECK: public func backDeployTopLevelFunc_myProject() -> Swift.Int { return 49 }
@backDeployed(before: _myProject 1.0)
public func backDeployTopLevelFunc_myProject() -> Int { return 49 }
