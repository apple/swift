// RUN: %empty-directory(%t)
// RUN: %target-swift-frontend %S/Inputs/enums.swift -module-name Enums -emit-module -emit-module-path %t/Enums.swiftmodule -clang-header-expose-decls=all-public -emit-clang-header-path %t/enums.h

// RUN: %target-swift-frontend %s -typecheck -module-name UsesEnums -I %t -clang-header-expose-decls=all-public -emit-clang-header-path %t/uses-enums.h -clang-header-expose-module Enums=enums.h

// RUN: %FileCheck %s < %t/uses-enums.h

// RUN: %check-interop-cxx-header-in-clang(-I %t %t/uses-enums.h)

import Enums

public struct UsesEnumsLargeEnum {
    public func passThroughStructSeveralI64(_ y: LargeEnum) -> LargeEnum {
        return y
    }

    public let x: LargeEnum
}

public func inoutLargeEnum(_ s: inout LargeEnum) {
    return s = LargeEnum.B
}

// CHECK: SWIFT_INLINE_THUNK void inoutLargeEnum(Enums::LargeEnum& s) noexcept SWIFT_SYMBOL("s:9UsesEnums14inoutLargeEnumyy0B00dE0OzF") {
// CHECK-NEXT: _impl::$s9UsesEnums14inoutLargeEnumyy0B00dE0OzF(Enums::_impl::_impl_LargeEnum::getOpaquePointer(s));
// CHECK-NEXT: }

// CHECK: SWIFT_INLINE_THUNK Enums::LargeEnum UsesEnumsLargeEnum::passThroughStructSeveralI64(const Enums::LargeEnum& y) const {
// CHECK-NEXT: return Enums::_impl::_impl_LargeEnum::returnNewValue([&](char * _Nonnull result) SWIFT_INLINE_THUNK_ATTRIBUTES {
// CHECK-NEXT:   _impl::$s9UsesEnums0aB9LargeEnumV27passThroughStructSeveralI64y0B00cD0OAGF(result, Enums::_impl::_impl_LargeEnum::getOpaquePointer(y), _getOpaquePointer());
// CHECK-NEXT: });
// CHECK-NEXT: }
// CHECK-NEXT: SWIFT_INLINE_THUNK Enums::LargeEnum UsesEnumsLargeEnum::getX() const {
// CHECK-NEXT: return Enums::_impl::_impl_LargeEnum::returnNewValue([&](char * _Nonnull result) SWIFT_INLINE_THUNK_ATTRIBUTES {
// CHECK-NEXT:   _impl::$s9UsesEnums0aB9LargeEnumV1x0B00cD0Ovg(result, _getOpaquePointer());
// CHECK-NEXT: });
// CHECK-NEXT: }
