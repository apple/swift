// RUN: %empty-directory(%t)

// RUN: %target-swift-emit-module-interface(%t/main.swiftinterface) %s
// RUN: %target-swift-typecheck-module-from-interface(%t/main.swiftinterface)
// RUN: %FileCheck %s < %t/main.swiftinterface

// CHECK: import Swift

// CHECK: public let MyClosureVar: (Swift.Int) -> Swift.Int
public let MyClosureVar: (Int) -> Int = { $0 }

// CHECK: public let MyOtherClosureVar: (_ x: Swift.Int) -> Swift.Int
public let MyOtherClosureVar: (_ x: Int) -> Int = { x in x }
