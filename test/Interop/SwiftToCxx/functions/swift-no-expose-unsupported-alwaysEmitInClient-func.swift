// RUN: %empty-directory(%t)
// RUN: %target-swift-frontend %s -typecheck -module-name Functions -clang-header-expose-decls=all-public -emit-clang-header-path %t/functions.h
// RUN: %FileCheck %s < %t/functions.h

// RUN: %check-interop-cxx-header-in-clang(%t/functions.h)

// CHECK-NOT: SWIFT_EXTERN bool $s9Functions24alwaysEmitIntoClientFuncyS2bF(bool x) SWIFT_NOEXCEPT SWIFT_CALL; // alwaysEmitIntoClientFunc(_:)

// CHECK:       namespace Functions SWIFT_PRIVATE_ATTR SWIFT_SYMBOL_MODULE("Functions") {
// CHECK-EMPTY:
// CHECK-EMPTY:
// CHECK-NEXT: // Unavailable in C++: Swift global function 'alwaysEmitIntoClientFunc(_:)'.
// CHECK-EMPTY:
// CHECK-NEXT:  } // namespace Functions



@_alwaysEmitIntoClient
public func alwaysEmitIntoClientFunc(_ x: Bool) -> Bool { return !x }
