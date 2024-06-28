// RUN: %empty-directory(%t)
// RUN: %target-swift-frontend %s -typecheck -module-name Operators -clang-header-expose-decls=all-public -emit-clang-header-path %t/operators.h
// RUN: %FileCheck %s < %t/operators.h

// RUN: %check-interop-cxx-header-in-clang(%t/operators.h)

// CHECK-LABEL: namespace Operators SWIFT_PRIVATE_ATTR SWIFT_SYMBOL_MODULE("Operators") {

// CHECK-LABEL: namespace _impl {

// CHECK: SWIFT_EXTERN bool $s9Operators2eeoiySbAA6IntBoxV_ADtF(struct swift_interop_passStub_Operators_uint32_t_0_4 lhs, struct swift_interop_passStub_Operators_uint32_t_0_4 rhs) SWIFT_NOEXCEPT SWIFT_CALL; // ==(_:_:)

// CHECK: }

public struct IntBox { var x: CInt }

public func -(lhs: IntBox, rhs: IntBox) -> CInt {
  return lhs.x - rhs.x
}

// CHECK: SWIFT_INLINE_THUNK int operator-(const IntBox& lhs, const IntBox& rhs) noexcept SWIFT_SYMBOL("s:9Operators1soiys5Int32VAA6IntBoxV_AFtF") SWIFT_WARN_UNUSED_RESULT {
// CHECK:   return _impl::$s9Operators1soiys5Int32VAA6IntBoxV_AFtF(_impl::swift_interop_passDirect_Operators_uint32_t_0_4(_impl::_impl_IntBox::getOpaquePointer(lhs)), _impl::swift_interop_passDirect_Operators_uint32_t_0_4(_impl::_impl_IntBox::getOpaquePointer(rhs)));
// CHECK: }

public func ==(lhs: IntBox, rhs: IntBox) -> Bool {
  return lhs.x == rhs.x
}

// CHECK: SWIFT_INLINE_THUNK bool operator==(const IntBox& lhs, const IntBox& rhs) noexcept SWIFT_SYMBOL("s:9Operators2eeoiySbAA6IntBoxV_ADtF") SWIFT_WARN_UNUSED_RESULT {
// CHECK:   return _impl::$s9Operators2eeoiySbAA6IntBoxV_ADtF(_impl::swift_interop_passDirect_Operators_uint32_t_0_4(_impl::_impl_IntBox::getOpaquePointer(lhs)), _impl::swift_interop_passDirect_Operators_uint32_t_0_4(_impl::_impl_IntBox::getOpaquePointer(rhs)));
// CHECK: }
