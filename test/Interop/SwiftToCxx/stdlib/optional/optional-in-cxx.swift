// RUN: %empty-directory(%t)

// RUN: %target-swift-frontend -typecheck %s -typecheck -module-name UseOptional -enable-experimental-cxx-interop -clang-header-expose-decls=has-expose-attr -emit-clang-header-path %t/useopt.h

// RUN: %FileCheck %s < %t/useopt.h

@_expose(Cxx)
public struct SmallStruct {
    let x: Int16
}

@_expose(Cxx)
public class Klass {
    let x: Int16

    init(_ x: Int16) {
        self.x = x
    }
}

@_expose(Cxx)
public func createCIntOpt(_ val: CInt) -> Optional<CInt> {
    return val
}

@_expose(Cxx)
public func takeCIntOpt(_ val: Optional<CInt>) {
    print(String(describing: val))
}

@_expose(Cxx)
public func createSmallStructOpt(_ val: Int16) -> SmallStruct? {
    return SmallStruct(x: val)
}

@_expose(Cxx)
public func takeSmallStructOpt(_ val: Optional<SmallStruct>) {
    print(String(describing: val))
}

@_expose(Cxx)
public func createKlassOpt(_ val: Int16) -> Klass? {
    return Klass(val)
}

@_expose(Cxx)
public func takeKlassOpt(_ val: Klass?) {
    print(String(describing: val))
}

@_expose(Cxx)
public func resetOpt<T>(_ val: inout Optional<T>) {
    val = .none
}


// CHECK: SWIFT_INLINE_THUNK swift::Optional<int> createCIntOpt(int val) noexcept SWIFT_SYMBOL("s:11UseOptional13createCIntOptys5Int32VSgADF") SWIFT_WARN_UNUSED_RESULT {
 // CHECK-NEXT: return swift::_impl::_impl_Optional<int>::returnNewValue([&](char * _Nonnull result) SWIFT_INLINE_THUNK_ATTRIBUTES {
// CHECK-NEXT:   _impl::swift_interop_returnDirect_UseOptional_[[CINTENC:[a-z0-9_]+]](result, _impl::$s11UseOptional13createCIntOptys5Int32VSgADF(val));
// CHECK-NEXT: });
// CHECK-NEXT: }


// CHECK: SWIFT_INLINE_THUNK swift::Optional<Klass> createKlassOpt(int16_t val) noexcept SWIFT_SYMBOL("s:11UseOptional14createKlassOptyAA0D0CSgs5Int16VF") SWIFT_WARN_UNUSED_RESULT {
// CHECK-NEXT:   return swift::_impl::_impl_Optional<Klass>::returnNewValue([&](char * _Nonnull result) SWIFT_INLINE_THUNK_ATTRIBUTES {
// CHECK-NEXT: _impl::swift_interop_returnDirect_UseOptional_[[CLASSENC:[a-z0-9_]+]](result, _impl::$s11UseOptional14createKlassOptyAA0D0CSgs5Int16VF(val));
// CHECK-NEXT:   });
// CHECK-NEXT: }

// CHECK: SWIFT_INLINE_THUNK swift::Optional<SmallStruct> createSmallStructOpt(int16_t val) noexcept SWIFT_SYMBOL("s:11UseOptional20createSmallStructOptyAA0dE0VSgs5Int16VF") SWIFT_WARN_UNUSED_RESULT {
// CHECK-NEXT:   return swift::_impl::_impl_Optional<SmallStruct>::returnNewValue([&](char * _Nonnull result) SWIFT_INLINE_THUNK_ATTRIBUTES {
// CHECK-NEXT:     _impl::swift_interop_returnDirect_UseOptional_uint32_t_0_4(result, _impl::$s11UseOptional20createSmallStructOptyAA0dE0VSgs5Int16VF(val));
// CHECK-NEXT:   });
// CHECK-NEXT: }


// CHECK: template<class T_0_0>
// CHECK-NEXT: #ifdef __cpp_concepts
// CHECK-NEXT: requires swift::isUsableInGenericContext<T_0_0>
// CHECK-NEXT: #endif
// CHECK-NEXT: SWIFT_INLINE_THUNK void resetOpt(swift::Optional<T_0_0>& val) noexcept SWIFT_SYMBOL("s:11UseOptional8resetOptyyxSgzlF") {
// CHECK-NEXT: #ifndef __cpp_concepts
// CHECK-NEXT: static_assert(swift::isUsableInGenericContext<T_0_0>, "type cannot be used in a Swift generic context");
// CHECK-NEXT: #endif
// CHECK-NEXT:   _impl::$s11UseOptional8resetOptyyxSgzlF(swift::_impl::_impl_Optional<T_0_0>::getOpaquePointer(val), swift::TypeMetadataTrait<T_0_0>::getTypeMetadata());
// CHECK-NEXT: }


// CHECK: SWIFT_INLINE_THUNK void takeCIntOpt(const swift::Optional<int>& val) noexcept SWIFT_SYMBOL("s:11UseOptional11takeCIntOptyys5Int32VSgF") {
// CHECK-NEXT:  _impl::$s11UseOptional11takeCIntOptyys5Int32VSgF(_impl::swift_interop_passDirect_UseOptional_[[CINTENC]](swift::_impl::_impl_Optional<int>::getOpaquePointer(val)));
// CHECK-NEXT: }


// CHECK: SWIFT_INLINE_THUNK void takeKlassOpt(const swift::Optional<Klass>& val) noexcept SWIFT_SYMBOL("s:11UseOptional12takeKlassOptyyAA0D0CSgF") {
// CHECK-NEXT:   _impl::$s11UseOptional12takeKlassOptyyAA0D0CSgF(_impl::swift_interop_passDirect_UseOptional_[[CLASSENC]](swift::_impl::_impl_Optional<Klass>::getOpaquePointer(val)));
// CHECK-NEXT: }


// CHECK: SWIFT_INLINE_THUNK void takeSmallStructOpt(const swift::Optional<SmallStruct>& val) noexcept SWIFT_SYMBOL("s:11UseOptional18takeSmallStructOptyyAA0dE0VSgF") {
// CHECK-NEXT:  _impl::$s11UseOptional18takeSmallStructOptyyAA0dE0VSgF(_impl::swift_interop_passDirect_UseOptional_uint32_t_0_4(swift::_impl::_impl_Optional<SmallStruct>::getOpaquePointer(val)));
// CHECK-NEXT: }
