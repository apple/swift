// RUN: %empty-directory(%t)
// RUN: %target-swift-frontend %s -typecheck -module-name Enums -clang-header-expose-decls=all-public -emit-clang-header-path %t/enums.h
// RUN: %FileCheck %s < %t/enums.h

// RUN: %check-interop-cxx-header-in-clang(%t/enums.h -Wno-unused-private-field -Wno-unused-function)

public enum E {
    case a
    case b(Int)

    public init(_ b: Int) {
        self = .b(b)
    }

    public func takeParamA(_ a: Int) {}

    public static func takeParamB(_ b: Int) {}
}

// CHECK: static SWIFT_INLINE_THUNK E init(swift::Int b_)
// CHECK: SWIFT_INLINE_THUNK void takeParamA(swift::Int a_)
// CHECK: static SWIFT_INLINE_THUNK void takeParamB(swift::Int b_)

// CHECK: E E::init(swift::Int b_) {
// CHECK: _impl::$s5Enums1EOyACSicfC(b_)

// CHECK: void E::takeParamA(swift::Int a_) const {
// CHECK: _impl::$s5Enums1EO10takeParamAyySiF(a_,

// CHECK: void E::takeParamB(swift::Int b_) {
// CHECK: _impl::$s5Enums1EO10takeParamByySiFZ(b_);
