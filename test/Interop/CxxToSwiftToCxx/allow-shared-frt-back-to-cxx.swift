// RUN: %empty-directory(%t)
// RUN: split-file %s %t

// RUN: %target-swift-frontend -typecheck %t/use-cxx-types.swift -typecheck -module-name UseCxxTy -emit-clang-header-path %t/UseCxxTy.h -I %t -enable-experimental-cxx-interop -clang-header-expose-decls=all-public -disable-availability-checking

// RUN: %FileCheck %s < %t/UseCxxTy.h

//--- header.h

class SharedFRT {
public:
    int x;
} __attribute__((swift_attr("import_reference")))
__attribute__((swift_attr("retain:retainShared")))
__attribute__((swift_attr("release:releaseShared")));

inline void retainShared(SharedFRT *r) { }
inline void releaseShared(SharedFRT *r) { }

//--- module.modulemap
module CxxTest {
    header "header.h"
    requires cplusplus
}

//--- use-cxx-types.swift
import CxxTest

public func consumeSharedFRT(_ x: consuming SharedFRT) {}
public func takeSharedFRT(_ x: SharedFRT) {}

// CHECK: SWIFT_EXTERN void $s8UseCxxTy16consumeSharedFRTyySo0eF0VnF(SharedFRT *_Nonnull x) SWIFT_NOEXCEPT SWIFT_CALL; // consumeSharedFRT(_:)

// CHECK: SWIFT_EXTERN void $s8UseCxxTy13takeSharedFRTyySo0eF0VF(SharedFRT *_Nonnull x) SWIFT_NOEXCEPT SWIFT_CALL; // takeSharedFRT(_:)
