// RUN: %empty-directory(%t)
// RUN: %target-swift-frontend %s -typecheck -module-name Structs -clang-header-expose-decls=all-public -emit-clang-header-path %t/structs.h
// RUN: %FileCheck %s < %t/structs.h

// CHECK: namespace Structs SWIFT_PRIVATE_ATTR SWIFT_SYMBOL_MODULE("Structs") {

// CHECK: class SWIFT_SYMBOL({{.*}}) ZeroSizedStruct final {
// CHECK: alignas(1) char _storage[1];
// CHECK-NEXT:   friend class
public struct ZeroSizedStruct {}

// CHECK: } // namespace Structs
