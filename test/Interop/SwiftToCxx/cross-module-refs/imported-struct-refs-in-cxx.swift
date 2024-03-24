// RUN: %empty-directory(%t)
// RUN: %target-swift-frontend %S/Inputs/structs.swift -module-name Structs -emit-module -emit-module-path %t/Structs.swiftmodule -clang-header-expose-decls=all-public -emit-clang-header-path %t/structs.h

// RUN: %target-swift-frontend %s -typecheck -module-name UsesStructs -I %t -clang-header-expose-decls=all-public -emit-clang-header-path %t/uses-structs.h -clang-header-expose-module Structs=structs.h

// RUN: %FileCheck %s < %t/uses-structs.h
// RUN: %check-interop-cxx-header-in-clang(-I %t %t/uses-structs.h)

// RUN: %target-swift-frontend %s -typecheck -module-name UsesStructs -I %t -cxx-interoperability-mode=default -emit-clang-header-path %t/uses-structs-default.h -clang-header-expose-module Structs=structs.h
// RUN: %check-interop-cxx-header-in-clang(-I %t %t/uses-structs-default.h)

import Structs

// CHECK: #include <structs.h>

public struct UsesStructsStruct {
    public func passThroughStructSeveralI64(_ y: StructSeveralI64) -> StructSeveralI64 {
        return y
    }

    public let x: StructSeveralI64
}

// CHECK:      SWIFT_INLINE_THUNK Structs::StructSeveralI64 passThroughStructSeveralI64(const Structs::StructSeveralI64& y) const SWIFT_SYMBOL("s:11UsesStructs0aB6StructV011passThroughC10SeveralI64y0B00cfG0VAGF");
// CHECK-NEXT: SWIFT_INLINE_THUNK Structs::StructSeveralI64 getX() const SWIFT_SYMBOL("s:11UsesStructs0aB6StructV1x0B00C10SeveralI64Vvp");


public func passThroughStructSeveralI64(_ x: StructSeveralI64) -> StructSeveralI64 {
    return Structs.passThroughStructSeveralI64(i: 0, x, j: 2)
}

public func inoutStructSeveralI64(_ s: inout StructSeveralI64) {
    return Structs.inoutStructSeveralI64(&s)
}

public func passThroughStructSmallDirect(_ x: SmallStructDirectPassing) -> SmallStructDirectPassing {
    return x
}

// CHECK:      SWIFT_INLINE_THUNK void inoutStructSeveralI64(Structs::StructSeveralI64& s) noexcept SWIFT_SYMBOL("s:11UsesStructs21inoutStructSeveralI64yy0B00deF0VzF") {
// CHECK-NEXT:   _impl::$s11UsesStructs21inoutStructSeveralI64yy0B00deF0VzF(Structs::_impl::_impl_StructSeveralI64::getOpaquePointer(s));
// CHECK-NEXT: }


// CHECK:      SWIFT_INLINE_THUNK Structs::StructSeveralI64 passThroughStructSeveralI64(const Structs::StructSeveralI64& x) noexcept SWIFT_SYMBOL("s:11UsesStructs27passThroughStructSeveralI64y0B00efG0VAEF") SWIFT_WARN_UNUSED_RESULT {
// CHECK-NEXT:  return Structs::_impl::_impl_StructSeveralI64::returnNewValue([&](char * _Nonnull result) SWIFT_INLINE_THUNK_ATTRIBUTES {
// CHECK-NEXT:    _impl::$s11UsesStructs27passThroughStructSeveralI64y0B00efG0VAEF(result, Structs::_impl::_impl_StructSeveralI64::getOpaquePointer(x));
// CHECK-NEXT:  });
// CHECK-NEXT: }

// CHECK:      SWIFT_INLINE_THUNK Structs::SmallStructDirectPassing passThroughStructSmallDirect(const Structs::SmallStructDirectPassing& x) noexcept SWIFT_SYMBOL("s:11UsesStructs28passThroughStructSmallDirecty0B00feG7PassingVAEF") SWIFT_WARN_UNUSED_RESULT {
// CHECK-NEXT:  return Structs::_impl::_impl_SmallStructDirectPassing::returnNewValue([&](char * _Nonnull result) SWIFT_INLINE_THUNK_ATTRIBUTES {
// CHECK-NEXT:    _impl::swift_interop_returnDirect_UsesStructs_uint32_t_0_4(result, _impl::$s11UsesStructs28passThroughStructSmallDirecty0B00feG7PassingVAEF(_impl::swift_interop_passDirect_UsesStructs_uint32_t_0_4(Structs::_impl::_impl_SmallStructDirectPassing::getOpaquePointer(x))));
// CHECK-NEXT:  });
// CHECK-NEXT: }

// CHECK: SWIFT_INLINE_THUNK Structs::StructSeveralI64 UsesStructsStruct::passThroughStructSeveralI64(const Structs::StructSeveralI64& y) const {
// CHECK-NEXT: return Structs::_impl::_impl_StructSeveralI64::returnNewValue([&](char * _Nonnull result) SWIFT_INLINE_THUNK_ATTRIBUTES {
// CHECK-NEXT:   _impl::$s11UsesStructs0aB6StructV011passThroughC10SeveralI64y0B00cfG0VAGF(result, Structs::_impl::_impl_StructSeveralI64::getOpaquePointer(y), _getOpaquePointer());
// CHECK-NEXT: });
// CHECK-NEXT: }

// CHECK: SWIFT_INLINE_THUNK Structs::StructSeveralI64 UsesStructsStruct::getX() const {
// CHECK-NEXT: return Structs::_impl::_impl_StructSeveralI64::returnNewValue([&](char * _Nonnull result) SWIFT_INLINE_THUNK_ATTRIBUTES {
// CHECK-NEXT:   _impl::$s11UsesStructs0aB6StructV1x0B00C10SeveralI64Vvg(result, _getOpaquePointer());
// CHECK-NEXT: });
// CHECK-NEXT: }
