// RUN: %empty-directory(%t)

// RUN: %target-swift-frontend -typecheck %s -typecheck -module-name UseOptional -enable-experimental-cxx-interop -emit-clang-header-path %t/stdlib.h
// RUN: %FileCheck %s < %t/stdlib.h

// RUN: %target-swift-frontend -typecheck %s -typecheck -module-name UseOptional -enable-experimental-cxx-interop -clang-header-expose-decls=has-expose-attr-or-stdlib -emit-clang-header-path %t/stdlib_expose_flag.h
// RUN: %FileCheck %s < %t/stdlib_expose_flag.h

// RUN: %target-swift-frontend -typecheck %s -typecheck -module-name UseOptional2 -enable-experimental-cxx-interop -emit-clang-header-path %t/stdlib2.h

// RUN: cat %t/stdlib.h %t/stdlib2.h > %t/two_includes.h

// RUN: %check-interop-cxx-header-in-clang(-DSWIFT_CXX_INTEROP_HIDE_STL_OVERLAY %t/two_includes.h -Wno-unused-private-field -Wno-unused-function)

@_expose(Cxx)
public func test() -> String {
    return ""
}

@_expose(Cxx)
public func testIntArray() -> [Int] {
    return []
}

// CHECK: namespace swift SWIFT_PRIVATE_ATTR SWIFT_SYMBOL_MODULE("swift") {
// CHECK: class SWIFT_SYMBOL("{{.*}}") String final {
