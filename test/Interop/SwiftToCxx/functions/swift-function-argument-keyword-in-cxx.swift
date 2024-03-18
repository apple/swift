// RUN: %empty-directory(%t)
// RUN: %target-swift-frontend %s -typecheck -module-name Functions -clang-header-expose-decls=all-public -emit-clang-header-path %t/functions.h
// RUN: %FileCheck %s < %t/functions.h

// RUN: %check-interop-cxx-header-in-clang(%t/functions.h)

// CHECK: SWIFT_INLINE_THUNK void testKeywordArgument(swift::Int register_) noexcept
// CHECK-NEXT: _impl::$s9Functions19testKeywordArgument8registerySi_tF(register_);
// CHECK-NEXT: }

public func testKeywordArgument(register: Int) {
}
