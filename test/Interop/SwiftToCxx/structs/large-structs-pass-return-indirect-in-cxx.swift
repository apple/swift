// RUN: %empty-directory(%t)
// RUN: %target-swift-frontend %s -typecheck -module-name Structs -clang-header-expose-decls=all-public -emit-clang-header-path %t/structs.h
// RUN: %FileCheck %s < %t/structs.h

// RUN: %check-interop-cxx-header-in-clang(%t/structs.h)

public struct StructSeveralI64 {
    var x1, x2, x3, x4, x5: Int64
}

// CHECK: SWIFT_EXTERN void $s7Structs21inoutStructSeveralI64yyAA0cdE0VzF(void * _Nonnull s) SWIFT_NOEXCEPT SWIFT_CALL; // inoutStructSeveralI64(_:)
// CHECK: class SWIFT_SYMBOL("s:7Structs16StructSeveralI64V") StructSeveralI64 final {

public func returnNewStructSeveralI64(i: Int64) -> StructSeveralI64 {
    return StructSeveralI64(x1: i, x2: 0, x3: -17, x4: 12345612, x5: -0xFFFF)
}

public func passThroughStructSeveralI64(i: Int64, _ x: StructSeveralI64, j: Float) -> StructSeveralI64 {
    return StructSeveralI64(x1: x.x1, x2: x.x2 + i, x3: x.x3, x4: -x.x4, x5: x.x5 + Int64(j))
}

public func printStructSeveralI64(_ x: StructSeveralI64) {
    print("StructSeveralI64.1 = \(x.x1), .2 = \(x.x2), .3 = \(x.x3), .4 = \(x.x4), .5 = \(x.x5)")
}

public func inoutStructSeveralI64(_ s: inout StructSeveralI64) {
    s.x1 = -1
    s.x2 = -2
    s.x3 = -3
    s.x4 = -4
    s.x5 = -5
}

// CHECK:      SWIFT_INLINE_THUNK void inoutStructSeveralI64(StructSeveralI64& s) noexcept SWIFT_SYMBOL("s:7Structs21inoutStructSeveralI64yyAA0cdE0VzF") {
// CHECK-NEXT:   _impl::$s7Structs21inoutStructSeveralI64yyAA0cdE0VzF(_impl::_impl_StructSeveralI64::getOpaquePointer(s));
// CHECK-NEXT: }


// CHECK: SWIFT_INLINE_THUNK StructSeveralI64 passThroughStructSeveralI64(int64_t i, const StructSeveralI64& x, float j) noexcept SWIFT_SYMBOL("s:7Structs27passThroughStructSeveralI641i_1jAA0deF0Vs5Int64V_AFSftF") SWIFT_WARN_UNUSED_RESULT {
// CHECK-NEXT:  return _impl::_impl_StructSeveralI64::returnNewValue([&](char * _Nonnull result) SWIFT_INLINE_THUNK_ATTRIBUTES {
// CHECK-NEXT:    _impl::$s7Structs27passThroughStructSeveralI641i_1jAA0deF0Vs5Int64V_AFSftF(result, i, _impl::_impl_StructSeveralI64::getOpaquePointer(x), j);
// CHECK-NEXT:  });
// CHECK-NEXT: }


// CHECK: SWIFT_INLINE_THUNK void printStructSeveralI64(const StructSeveralI64& x) noexcept SWIFT_SYMBOL("s:7Structs21printStructSeveralI64yyAA0cdE0VF") {
// CHECK-NEXT:  _impl::$s7Structs21printStructSeveralI64yyAA0cdE0VF(_impl::_impl_StructSeveralI64::getOpaquePointer(x));
// CHECK-NEXT: }


// CHECK: SWIFT_INLINE_THUNK StructSeveralI64 returnNewStructSeveralI64(int64_t i) noexcept SWIFT_SYMBOL("s:7Structs25returnNewStructSeveralI641iAA0deF0Vs5Int64V_tF") SWIFT_WARN_UNUSED_RESULT {
// CHECK-NEXT:  return _impl::_impl_StructSeveralI64::returnNewValue([&](char * _Nonnull result) SWIFT_INLINE_THUNK_ATTRIBUTES {
// CHECK-NEXT:    _impl::$s7Structs25returnNewStructSeveralI641iAA0deF0Vs5Int64V_tF(result, i);
// CHECK-NEXT:  });
// CHECK-NEXT: }
