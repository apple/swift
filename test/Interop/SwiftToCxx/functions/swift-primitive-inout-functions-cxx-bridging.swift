// RUN: %empty-directory(%t)
// RUN: %target-swift-frontend %s -typecheck -module-name Functions -clang-header-expose-decls=all-public -emit-clang-header-path %t/functions.h
// RUN: %FileCheck %s < %t/functions.h

// RUN: %check-interop-cxx-header-in-clang(%t/functions.h)

// CHECK: SWIFT_EXTERN void $s9Functions8inOutIntyySizF(ptrdiff_t * _Nonnull x) SWIFT_NOEXCEPT SWIFT_CALL; // inOutInt(_:)
// CHECK: SWIFT_EXTERN void $s9Functions11inOutTwoIntyySiz_SiztF(ptrdiff_t * _Nonnull x, ptrdiff_t * _Nonnull y) SWIFT_NOEXCEPT SWIFT_CALL; // inOutTwoInt(_:_:)
// CHECK: SWIFT_EXTERN void $s9Functions13inOutTwoParamyySbz_SdztF(bool * _Nonnull x, double * _Nonnull y) SWIFT_NOEXCEPT SWIFT_CALL; // inOutTwoParam(_:_:)
// CHECK: SWIFT_EXTERN void $s9Functions24inoutTypeWithNullabilityyySVzF(void const * _Nonnull * _Nonnull x) SWIFT_NOEXCEPT SWIFT_CALL; // inoutTypeWithNullability(_:)
// CHECK: SWIFT_EXTERN void $s9Functions25inoutUnsafeGenericPointeryySPys5Int32VGzF(int32_t const * _Nonnull * _Nonnull x) SWIFT_NOEXCEPT SWIFT_CALL; // inoutUnsafeGenericPointer(_:)

// CHECK:      SWIFT_INLINE_THUNK void inOutInt(swift::Int & x) noexcept SWIFT_SYMBOL({{.*}}) {
// CHECK-NEXT:   _impl::$s9Functions8inOutIntyySizF(&x);
// CHECK-NEXT: }

// CHECK:      SWIFT_INLINE_THUNK void inOutTwoInt(swift::Int & x, swift::Int & y) noexcept SWIFT_SYMBOL({{.*}}) {
// CHECK-NEXT:   _impl::$s9Functions11inOutTwoIntyySiz_SiztF(&x, &y);
// CHECK-NEXT: }

// CHECK:      SWIFT_INLINE_THUNK void inOutTwoParam(bool & x, double & y) noexcept SWIFT_SYMBOL({{.*}}) {
// CHECK-NEXT:   _impl::$s9Functions13inOutTwoParamyySbz_SdztF(&x, &y);
// CHECK-NEXT: }

// CHECK:      SWIFT_INLINE_THUNK void inoutTypeWithNullability(void const * _Nonnull & x) noexcept SWIFT_SYMBOL({{.*}}) {
// CHECK-NEXT:   _impl::$s9Functions24inoutTypeWithNullabilityyySVzF(&x);
// CHECK-NEXT: }

// CHECK:      SWIFT_INLINE_THUNK void inoutUnsafeGenericPointer(int32_t const * _Nonnull & x) noexcept SWIFT_SYMBOL({{.*}}) {
// CHECK-NEXT:   _impl::$s9Functions25inoutUnsafeGenericPointeryySPys5Int32VGzF(&x);
// CHECK-NEXT: }

public func inOutInt(_ x: inout Int) { x = Int() }

public func inOutTwoInt(_ x: inout Int, _ y: inout Int) {
    x += y
    y -= 2 * x
}

public func inOutTwoParam(_ x: inout Bool, _ y: inout Double) {
    y = 3.14
    x = !x
}

public func inoutTypeWithNullability(_ x: inout UnsafeRawPointer) {
    x += 1
}

public func inoutUnsafeGenericPointer(_ x: inout UnsafePointer<Int32>) {
    x += 1
}
