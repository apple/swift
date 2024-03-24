// RUN: %empty-directory(%t)
// RUN: %target-swift-frontend %s -typecheck -module-name Enums -enable-experimental-cxx-interop -clang-header-expose-decls=has-expose-attr -emit-clang-header-path %t/enums.h
// RUN: %FileCheck %s < %t/enums.h

// RUN: %check-interop-cxx-header-in-clang(%t/enums.h -Wno-unused-private-field -Wno-unused-function)

@_expose(Cxx)
public enum Tiny {
    case first
    case second
}

@_expose(Cxx)
public func makeTiny(_ x: Int) -> Tiny {
    return x >= 0 ? .first : .second
}

@_expose(Cxx)
public func printTiny(_ en: Tiny) {
    switch en {
    case .first:
        print("Tiny.first")
    case .second:
        print("Tiny.second")
    }
}

@_expose(Cxx)
public func passThroughTiny(_ en: Tiny) -> Tiny {
    return en
}

@_expose(Cxx)
public func inoutTiny(_ en: inout Tiny, _ x: Int) {
    if x >= 0 {
        en = .first
    } else {
        en = .second
    }
}

@_expose(Cxx)
public enum Small {
    case first(Int)
    case second(Double)
}

@_expose(Cxx)
public func makeSmall(_ x: Int) -> Small {
    return x >= 0 ? .first(x) : .second(Double(-x))
}

@_expose(Cxx)
public func printSmall(_ en: Small) {
    switch en {
    case let .first(x):
        print("Small.first(\(x))")
    case let .second(x):
        print("Small.second(\(x))")
    }
}

@_expose(Cxx)
public func passThroughSmall(_ en: Small) -> Small {
    return en
}

@_expose(Cxx)
public func inoutSmall(_ en: inout Small, _ x: Int) {
    if x >= 0 {
        en = .first(x)
    } else {
        en = .second(Double(x - 1))
    }
}

// CHECK: struct swift_interop_passStub_Enums_uint8_t_0_1 {
// CHECK-NEXT: uint8_t _1;
// CHECK-NEXT: };

// CHECK: static SWIFT_C_INLINE_THUNK struct swift_interop_passStub_Enums_uint8_t_0_1 swift_interop_passDirect_Enums_uint8_t_0_1(const char * _Nonnull value) {
// CHECK-NEXT: struct swift_interop_passStub_Enums_uint8_t_0_1 result;
// CHECK-NEXT: memcpy(&result._1, value + 0, 1);
// CHECK-NEXT: return result;
// CHECK-NEXT: }

// CHECK: SWIFT_EXTERN void $s5Enums10inoutSmallyyAA0C0Oz_SitF(void * _Nonnull en, ptrdiff_t x) SWIFT_NOEXCEPT SWIFT_CALL; // inoutSmall(_:_:)
// CHECK: SWIFT_EXTERN void $s5Enums9inoutTinyyyAA0C0Oz_SitF(void * _Nonnull en, ptrdiff_t x) SWIFT_NOEXCEPT SWIFT_CALL; // inoutTiny(_:_:)

// The check for generated stub is currently moved to small-enums-generated-stub-64bit.swift

// CHECK: SWIFT_EXTERN struct swift_interop_returnStub_Enums_[[Small:[0-9a-z_]+]] $s5Enums9makeSmallyAA0C0OSiF(ptrdiff_t x) SWIFT_NOEXCEPT SWIFT_CALL; // makeSmall(_:)

// CHECK:      struct swift_interop_returnStub_Enums_uint8_t_0_1 {
// CHECK-NEXT:   uint8_t _1;
// CHECK-NEXT: };

// CHECK:      static SWIFT_C_INLINE_THUNK void swift_interop_returnDirect_Enums_uint8_t_0_1(char * _Nonnull result, struct swift_interop_returnStub_Enums_uint8_t_0_1 value) {
// CHECK-NEXT:   memcpy(result + 0, &value._1, 1);
// CHECK-NEXT: }

// CHECK: SWIFT_EXTERN struct swift_interop_returnStub_Enums_uint8_t_0_1 $s5Enums8makeTinyyAA0C0OSiF(ptrdiff_t x) SWIFT_NOEXCEPT SWIFT_CALL; // makeTiny(_:)
// CHECK: SWIFT_EXTERN struct swift_interop_returnStub_Enums_[[Small]] $s5Enums16passThroughSmallyAA0D0OADF(struct swift_interop_passStub_Enums_[[Small]] en) SWIFT_NOEXCEPT SWIFT_CALL; // passThroughSmall(_:)

// CHECK: SWIFT_EXTERN struct swift_interop_returnStub_Enums_uint8_t_0_1 $s5Enums15passThroughTinyyAA0D0OADF(struct swift_interop_passStub_Enums_uint8_t_0_1 en) SWIFT_NOEXCEPT SWIFT_CALL; // passThroughTiny(_:)
// CHECK: SWIFT_EXTERN void $s5Enums10printSmallyyAA0C0OF(struct swift_interop_passStub_Enums_[[Small]] en) SWIFT_NOEXCEPT SWIFT_CALL; // printSmall(_:)
// CHECK: SWIFT_EXTERN void $s5Enums9printTinyyyAA0C0OF(struct swift_interop_passStub_Enums_uint8_t_0_1 en) SWIFT_NOEXCEPT SWIFT_CALL; // printTiny(_:)
// CHECK: class SWIFT_SYMBOL("s:5Enums5SmallO") Small final {
// CHECK: class SWIFT_SYMBOL("s:5Enums4TinyO") Tiny final {

// CHECK:      SWIFT_INLINE_THUNK void inoutSmall(Small& en, swift::Int x) noexcept SWIFT_SYMBOL("s:5Enums10inoutSmallyyAA0C0Oz_SitF") {
// CHECK-NEXT:   _impl::$s5Enums10inoutSmallyyAA0C0Oz_SitF(_impl::_impl_Small::getOpaquePointer(en), x);
// CHECK-NEXT: }

// CHECK:      SWIFT_INLINE_THUNK void inoutTiny(Tiny& en, swift::Int x) noexcept SWIFT_SYMBOL("s:5Enums9inoutTinyyyAA0C0Oz_SitF") {
// CHECK-NEXT:   _impl::$s5Enums9inoutTinyyyAA0C0Oz_SitF(_impl::_impl_Tiny::getOpaquePointer(en), x);
// CHECK-NEXT: }

// CHECK:      SWIFT_INLINE_THUNK Small makeSmall(swift::Int x) noexcept SWIFT_SYMBOL("s:5Enums9makeSmallyAA0C0OSiF") SWIFT_WARN_UNUSED_RESULT {
// CHECK-NEXT:   return _impl::_impl_Small::returnNewValue([&](char * _Nonnull result) SWIFT_INLINE_THUNK_ATTRIBUTES {
// CHECK-NEXT:     _impl::swift_interop_returnDirect_Enums_[[Small]](result, _impl::$s5Enums9makeSmallyAA0C0OSiF(x));
// CHECK-NEXT:   });
// CHECK-NEXT: }

// CHECK:      SWIFT_INLINE_THUNK Tiny makeTiny(swift::Int x) noexcept SWIFT_SYMBOL("s:5Enums8makeTinyyAA0C0OSiF") SWIFT_WARN_UNUSED_RESULT {
// CHECK-NEXT:   return _impl::_impl_Tiny::returnNewValue([&](char * _Nonnull result) SWIFT_INLINE_THUNK_ATTRIBUTES {
// CHECK-NEXT:     _impl::swift_interop_returnDirect_Enums_uint8_t_0_1(result, _impl::$s5Enums8makeTinyyAA0C0OSiF(x));
// CHECK-NEXT:   });
// CHECK-NEXT: }

// CHECK:      SWIFT_INLINE_THUNK Small passThroughSmall(const Small& en) noexcept SWIFT_SYMBOL("s:5Enums16passThroughSmallyAA0D0OADF") SWIFT_WARN_UNUSED_RESULT {
// CHECK-NEXT:   return _impl::_impl_Small::returnNewValue([&](char * _Nonnull result) SWIFT_INLINE_THUNK_ATTRIBUTES {
// CHECK-NEXT:     _impl::swift_interop_returnDirect_Enums_[[Small]](result, _impl::$s5Enums16passThroughSmallyAA0D0OADF(_impl::swift_interop_passDirect_Enums_[[Small]](_impl::_impl_Small::getOpaquePointer(en))));
// CHECK-NEXT:   });
// CHECK-NEXT: }

// CHECK:      SWIFT_INLINE_THUNK Tiny passThroughTiny(const Tiny& en) noexcept SWIFT_SYMBOL("s:5Enums15passThroughTinyyAA0D0OADF") SWIFT_WARN_UNUSED_RESULT {
// CHECK-NEXT:   return _impl::_impl_Tiny::returnNewValue([&](char * _Nonnull result) SWIFT_INLINE_THUNK_ATTRIBUTES {
// CHECK-NEXT:     _impl::swift_interop_returnDirect_Enums_uint8_t_0_1(result, _impl::$s5Enums15passThroughTinyyAA0D0OADF(_impl::swift_interop_passDirect_Enums_uint8_t_0_1(_impl::_impl_Tiny::getOpaquePointer(en))));
// CHECK-NEXT:   });
// CHECK-NEXT: }

// CHECK:      SWIFT_INLINE_THUNK void printSmall(const Small& en) noexcept SWIFT_SYMBOL("s:5Enums10printSmallyyAA0C0OF") {
// CHECK-NEXT:   _impl::$s5Enums10printSmallyyAA0C0OF(_impl::swift_interop_passDirect_Enums_[[Small]](_impl::_impl_Small::getOpaquePointer(en)));
// CHECK-NEXT: }

// CHECK:      SWIFT_INLINE_THUNK void printTiny(const Tiny& en) noexcept SWIFT_SYMBOL("s:5Enums9printTinyyyAA0C0OF") {
// CHECK-NEXT:   _impl::$s5Enums9printTinyyyAA0C0OF(_impl::swift_interop_passDirect_Enums_uint8_t_0_1(_impl::_impl_Tiny::getOpaquePointer(en)));
// CHECK-NEXT: }
