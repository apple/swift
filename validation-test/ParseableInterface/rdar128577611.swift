// RUN: %empty-directory(%t)
// RUN: %target-swift-emit-module-interface(%t/Library.swiftinterface) %s -module-name Library
// RUN: %target-swift-typecheck-module-from-interface(%t/Library.swiftinterface) -module-name Library
// RUN: %FileCheck %s < %t/Library.swiftinterface

struct InternalStruct {}
extension [Int: InternalStruct]: Sendable {}

// CHECK: @available(*, unavailable)
// CHECK-NEXT: extension Swift.Dictionary : Swift.Copyable where Key : _ConstraintThatIsNotPartOfTheAPIOfThisLibrary {}

// CHECK: @available(*, unavailable)
// CHECK-NEXT: extension Swift.Dictionary : Swift.Escapable where Key : _ConstraintThatIsNotPartOfTheAPIOfThisLibrary {}

// CHECK: @available(*, unavailable)
// CHECK-NEXT: extension Swift.Dictionary : Swift.Sendable where Key : _ConstraintThatIsNotPartOfTheAPIOfThisLibrary {}
