// RUN: %empty-directory(%t)
// RUN: %target-swift-frontend %s -typecheck -module-name Enums -clang-header-expose-decls=all-public -emit-clang-header-path %t/enums.h
// RUN: %FileCheck %s < %t/enums.h

public enum EmptyEnum {}
public enum SingleCaseEnum { case first }

// CHECK: namespace Enums SWIFT_PRIVATE_ATTR SWIFT_SYMBOL_MODULE("Enums") {

// CHECK-NOT: EmptyEnum final {

// CHECK: class SWIFT_SYMBOL({{.*}}) SingleCaseEnum final {
// CHECK: SWIFT_INLINE_THUNK operator cases() const {
// CHECK: }
// CHECK-NEXT: }
// CHECK-EMPTY:
// CHECK-NEXT: SWIFT_INLINE_THUNK swift::Int getHashValue() const SWIFT_SYMBOL({{.*}});
// CHECK-NEXT: private:

// CHECK: } // namespace Enums

