// RUN: %empty-directory(%t)
// RUN: %target-swift-frontend %s -typecheck -module-name Functions -clang-header-expose-decls=all-public -emit-clang-header-path %t/apis.h
// RUN: %FileCheck %s < %t/apis.h

internal func takeFloat(_ x: Float) {}

private struct PrivateStruct { let x: Int }

class InternalClass {
    let x: Int
    init() { self.x = 0 }
}

protocol InternalProto {}

// CHECK: namespace Functions SWIFT_PRIVATE_ATTR SWIFT_SYMBOL_MODULE("Functions") {
// CHECK-EMPTY:
// CHECK-EMPTY:
// CHECK-NEXT: } // namespace Functions
