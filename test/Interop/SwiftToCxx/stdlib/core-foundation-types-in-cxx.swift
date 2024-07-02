// RUN: %empty-directory(%t)

// RUN: %target-swift-frontend -typecheck %s -typecheck -module-name UseCoreFoundation -enable-experimental-cxx-interop -clang-header-expose-decls=all-public -emit-clang-header-path %t/UseCoreFoundation.h
// RUN: %FileCheck %s < %t/UseCoreFoundation.h

// REQUIRES: objc_interop

import CoreFoundation

public func foobar(_ a: CFData) -> Bool {
    true
}

// CHECK: SWIFT_EXTERN bool $s17UseCoreFoundation6foobarySbSo9CFDataRefaF(CFDataRef _Nonnull a) SWIFT_NOEXCEPT SWIFT_CALL; // foobar(_:)
