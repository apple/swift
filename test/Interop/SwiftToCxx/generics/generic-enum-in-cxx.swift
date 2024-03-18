// RUN: %empty-directory(%t)
// RUN: %target-swift-frontend %s -typecheck -module-name Generics -clang-header-expose-decls=all-public -emit-clang-header-path %t/generics.h
// RUN: %FileCheck %s < %t/generics.h
// RUN: %check-interop-cxx-header-in-clang(%t/generics.h -Wno-reserved-identifier)

// RUN: %empty-directory(%t)
// RUN: %target-swift-frontend %s -enable-library-evolution -typecheck -module-name Generics -clang-header-expose-decls=all-public -emit-clang-header-path %t/generics.h
// RUN: %FileCheck %s < %t/generics.h
// RUN: %check-interop-cxx-header-in-clang(%t/generics.h -Wno-reserved-identifier)

// FIXME: remove the need for -Wno-reserved-identifier

public class TracksDeinit {
    init() {
        print("init-TracksDeinit")
    }
    deinit {
        print("destroy-TracksDeinit")
    }
}

public func constructTracksDeinit() -> TracksDeinit {
    return TracksDeinit()
}

@frozen
public struct StructForEnum {
    let x: TracksDeinit

    public init() {
        x = TracksDeinit()
    }
}

@frozen public enum GenericOpt<T> {
    case none
    case some(T)

    public func method() {
        let copyOfSelf = self
        print("GenericOpt<T>::testme::\(copyOfSelf);")
    }

    public mutating func reset() {
        self = .none
    }

    public func genericMethod<T>(_ x: T) -> T {
        print("GenericOpt<T>::genericMethod<T>::\(self),\(x);")
        return x
    }

    public var computedProp: Int {
        return 42
    }
}

public func makeGenericOpt<T>(_ x: T) -> GenericOpt<T> {
    return .some(x)
}

public func makeConcreteOpt(_ x: UInt16) -> GenericOpt<UInt16> {
    return GenericOpt<UInt16>.some(x)
}

public func takeGenericOpt<T>(_ x: GenericOpt<T>) {
    print(x)
}

public func takeConcreteOpt(_ x: GenericOpt<UInt16>) {
    print("CONCRETE opt:", x, ";")
}

public func inoutGenericOpt<T>(_ x: inout GenericOpt<T>, _ y: T) {
    switch (x) {
    case .some:
        x = .none
    case .none:
        x = .some(y)
    }
}

public func inoutConcreteOpt(_ x: inout GenericOpt<UInt16>) {
    switch (x) {
    case .some(let x_):
        x = .some(x_ * 3)
    case .none:
        break
    }
}

// CHECK: template<class T_0_0>
// CHECK-NEXT: #ifdef __cpp_concepts
// CHECK-NEXT: requires swift::isUsableInGenericContext<T_0_0>
// CHECK-NEXT: #endif
// CHECK-NEXT: class SWIFT_SYMBOL("s:8Generics10GenericOptO") GenericOpt;

// CHECK: template<class T_0_0>
// CHECK-NEXT: #ifdef __cpp_concepts
// CHECK-NEXT: requires swift::isUsableInGenericContext<T_0_0>
// CHECK-NEXT: #endif
// CHECK-NEXT: static inline const constexpr bool isUsableInGenericContext<Generics::GenericOpt<T_0_0>> = isUsableInGenericContext<T_0_0>;

// CHECK: template<class T_0_0>
// CHECK-NEXT: #ifdef __cpp_concepts
// CHECK-NEXT: requires swift::isUsableInGenericContext<T_0_0>
// CHECK-NEXT: #endif
// CHECK-NEXT: class _impl_GenericOpt;

// CHECK: SWIFT_EXTERN swift::_impl::MetadataResponseTy $s8Generics10GenericOptOMa(swift::_impl::MetadataRequestTy, void * _Nonnull) SWIFT_NOEXCEPT SWIFT_CALL;

// CHECK: template<class T_0_0>
// CHECK-NEXT: #ifdef __cpp_concepts
// CHECK-NEXT: requires swift::isUsableInGenericContext<T_0_0>
// CHECK-NEXT: #endif
// CHECK-NEXT: class SWIFT_SYMBOL("s:8Generics10GenericOptO") GenericOpt final {
// CHECK-NEXT: public:
// CHECK-NEXT: #ifndef __cpp_concepts
// CHECK-NEXT: static_assert(swift::isUsableInGenericContext<T_0_0>, "type cannot be used in a Swift generic context");
// CHECK-NEXT: #endif
// CHECK-NEXT: SWIFT_INLINE_THUNK ~GenericOpt() noexcept {
// CHECK-NEXT:   auto metadata = _impl::$s8Generics10GenericOptOMa(0, swift::TypeMetadataTrait<T_0_0>::getTypeMetadata());
// CHECK-NEXT:   auto *vwTableAddr = reinterpret_cast<swift::_impl::ValueWitnessTable **>(metadata._0) - 1;
// CHECK-NEXT: #ifdef __arm64e__
// CHECK-NEXT:   auto *vwTable = reinterpret_cast<swift::_impl::ValueWitnessTable *>(ptrauth_auth_data(reinterpret_cast<void *>(*vwTableAddr), ptrauth_key_process_independent_data, ptrauth_blend_discriminator(vwTableAddr, 11839)));
// CHECK-NEXT: #else
// CHECK-NEXT:   auto *vwTable = *vwTableAddr;
// CHECK-NEXT: #endif
// CHECK-NEXT:   vwTable->destroy(_getOpaquePointer(), metadata._0);
// CHECK-NEXT: }

// CHECK: enum class cases {
// CHECK-NEXT:  some SWIFT_SYMBOL("s:8Generics10GenericOptO4someyACyxGxcAEmlF"),
// CHECK-NEXT:  none SWIFT_SYMBOL("s:8Generics10GenericOptO4noneyACyxGAEmlF")
// CHECK-NEXT: };

// CHECK: SWIFT_INLINE_THUNK GenericOpt<T_0_0> operator()(const T_0_0& val) const;
// CHECK-NEXT: } some SWIFT_SYMBOL("s:8Generics10GenericOptO4someyACyxGxcAEmlF");
// CHECK: SWIFT_INLINE_THUNK GenericOpt<T_0_0> operator()() const;
// CHECK-NEXT: } none SWIFT_SYMBOL("s:8Generics10GenericOptO4noneyACyxGAEmlF");

// CHECK: SWIFT_INLINE_THUNK operator cases() const {
// CHECK-NEXT:   switch (_getEnumTag()) {
// CHECK-NEXT:     case 0: return cases::some;
// CHECK-NEXT:     case 1: return cases::none;
// CHECK-NEXT:     default: abort();
// CHECK-NEXT:   }
// CHECK-NEXT: }

// CHECK: SWIFT_INLINE_THUNK void method() const SWIFT_SYMBOL("s:8Generics10GenericOptO6methodyyF");
// CHECK-NEXT: SWIFT_INLINE_THUNK void reset() SWIFT_SYMBOL("s:8Generics10GenericOptO5resetyyF");
// CHECK-NEXT: template<class T_1_0>
// CHECK-NEXT: #ifdef __cpp_concepts
// CHECK-NEXT: requires swift::isUsableInGenericContext<T_1_0>
// CHECK-NEXT: #endif
// CHECK-NEXT: SWIFT_INLINE_THUNK T_1_0 genericMethod(const T_1_0& x) const SWIFT_SYMBOL("s:8Generics10GenericOptO13genericMethodyqd__qd__lF");
// CHECK-NEXT: SWIFT_INLINE_THUNK swift::Int getComputedProp() const SWIFT_SYMBOL("s:8Generics10GenericOptO12computedPropSivp");


// CHECK: SWIFT_INLINE_THUNK void inoutConcreteOpt(GenericOpt<uint16_t>& x) noexcept SWIFT_SYMBOL("s:8Generics16inoutConcreteOptyyAA07GenericD0Oys6UInt16VGzF") {
// CHECK-NEXT:   _impl::$s8Generics16inoutConcreteOptyyAA07GenericD0Oys6UInt16VGzF(_impl::_impl_GenericOpt<uint16_t>::getOpaquePointer(x));
// CHECK-NEXT: }


// CHECK: template<class T_0_0>
// CHECK-NEXT: #ifdef __cpp_concepts
// CHECK-NEXT: requires swift::isUsableInGenericContext<T_0_0>
// CHECK-NEXT: #endif
// CHECK-NEXT: SWIFT_INLINE_THUNK void inoutGenericOpt(GenericOpt<T_0_0>& x, const T_0_0& y) noexcept SWIFT_SYMBOL("s:8Generics15inoutGenericOptyyAA0cD0OyxGz_xtlF") {
// CHECK-NEXT: #ifndef __cpp_concepts
// CHECK-NEXT: static_assert(swift::isUsableInGenericContext<T_0_0>, "type cannot be used in a Swift generic context");
// CHECK-NEXT: #endif
// CHECK-NEXT:   _impl::$s8Generics15inoutGenericOptyyAA0cD0OyxGz_xtlF(_impl::_impl_GenericOpt<T_0_0>::getOpaquePointer(x), swift::_impl::getOpaquePointer(y), swift::TypeMetadataTrait<T_0_0>::getTypeMetadata());
// CHECK-NEXT: }


// CHECK: SWIFT_INLINE_THUNK GenericOpt<uint16_t> makeConcreteOpt(uint16_t x) noexcept SWIFT_SYMBOL("s:8Generics15makeConcreteOptyAA07GenericD0Oys6UInt16VGAFF") SWIFT_WARN_UNUSED_RESULT {
// CHECK-NEXT:   return _impl::_impl_GenericOpt<uint16_t>::returnNewValue([&](char * _Nonnull result) SWIFT_INLINE_THUNK_ATTRIBUTES {
// CHECK-NEXT:     _impl::swift_interop_returnDirect_Generics_uint32_t_0_4(result, _impl::$s8Generics15makeConcreteOptyAA07GenericD0Oys6UInt16VGAFF(x));
// CHECK-NEXT:   });
// CHECK-NEXT: }


// CHECK: template<class T_0_0>
// CHECK-NEXT: #ifdef __cpp_concepts
// CHECK-NEXT: requires swift::isUsableInGenericContext<T_0_0>
// CHECK-NEXT: #endif
// CHECK-NEXT: SWIFT_INLINE_THUNK GenericOpt<T_0_0> makeGenericOpt(const T_0_0& x) noexcept SWIFT_SYMBOL("s:8Generics14makeGenericOptyAA0cD0OyxGxlF") SWIFT_WARN_UNUSED_RESULT {
// CHECK-NEXT: #ifndef __cpp_concepts
// CHECK-NEXT: static_assert(swift::isUsableInGenericContext<T_0_0>, "type cannot be used in a Swift generic context");
// CHECK-NEXT: #endif
// CHECK-NEXT:   return _impl::_impl_GenericOpt<T_0_0>::returnNewValue([&](char * _Nonnull result) SWIFT_INLINE_THUNK_ATTRIBUTES {
// CHECK-NEXT:     _impl::$s8Generics14makeGenericOptyAA0cD0OyxGxlF(result, swift::_impl::getOpaquePointer(x), swift::TypeMetadataTrait<T_0_0>::getTypeMetadata());
// CHECK-NEXT:   });
// CHECK-NEXT: }


// CHECK: SWIFT_INLINE_THUNK void takeConcreteOpt(const GenericOpt<uint16_t>& x) noexcept SWIFT_SYMBOL("s:8Generics15takeConcreteOptyyAA07GenericD0Oys6UInt16VGF") {
// CHECK-NEXT:   _impl::$s8Generics15takeConcreteOptyyAA07GenericD0Oys6UInt16VGF(_impl::swift_interop_passDirect_Generics_uint32_t_0_4(_impl::_impl_GenericOpt<uint16_t>::getOpaquePointer(x)));
// CHECK-NEXT: }


// CHECK: template<class T_0_0>
// CHECK-NEXT: #ifdef __cpp_concepts
// CHECK-NEXT: requires swift::isUsableInGenericContext<T_0_0>
// CHECK-NEXT: #endif
// CHECK-NEXT: SWIFT_INLINE_THUNK void takeGenericOpt(const GenericOpt<T_0_0>& x) noexcept SWIFT_SYMBOL("s:8Generics14takeGenericOptyyAA0cD0OyxGlF") {
// CHECK-NEXT: #ifndef __cpp_concepts
// CHECK-NEXT: static_assert(swift::isUsableInGenericContext<T_0_0>, "type cannot be used in a Swift generic context");
// CHECK-NEXT: #endif
// CHECK-NEXT:   _impl::$s8Generics14takeGenericOptyyAA0cD0OyxGlF(_impl::_impl_GenericOpt<T_0_0>::getOpaquePointer(x), swift::TypeMetadataTrait<T_0_0>::getTypeMetadata());
// CHECK-NEXT: }

// CHECK: template<class T_0_0>
// CHECK-NEXT: #ifdef __cpp_concepts
// CHECK-NEXT: requires swift::isUsableInGenericContext<T_0_0>
// CHECK-NEXT: #endif // __cpp_concepts
// CHECK-NEXT:   SWIFT_INLINE_THUNK GenericOpt<T_0_0> GenericOpt<T_0_0>::_impl_some::operator()(const T_0_0& val) const {
// CHECK-NEXT:     auto result = GenericOpt<T_0_0>::_make();
// CHECK-NEXT: #pragma clang diagnostic push
// CHECK-NEXT: #pragma clang diagnostic ignored "-Wc++17-extensions"
// CHECK-NEXT: if constexpr (std::is_base_of<::swift::_impl::RefCountedClass, T_0_0>::value) {
// CHECK-NEXT:     void *ptr = ::swift::_impl::_impl_RefCountedClass::copyOpaquePointer(val);
// CHECK-NEXT:     memcpy(result._getOpaquePointer(), &ptr, sizeof(ptr));
// CHECK-NEXT: } else if constexpr (::swift::_impl::isValueType<T_0_0>) {
// CHECK-NEXT:     alignas(T_0_0) unsigned char buffer[sizeof(T_0_0)];
// CHECK-NEXT:     auto *valCopy = new(buffer) T_0_0(val);
// CHECK-NEXT:     swift::_impl::implClassFor<T_0_0>::type::initializeWithTake(result._getOpaquePointer(), swift::_impl::implClassFor<T_0_0>::type::getOpaquePointer(*valCopy));
// CHECK-NEXT: } else {
// CHECK-NEXT:     memcpy(result._getOpaquePointer(), &val, sizeof(val));
// CHECK-NEXT: }
// CHECK-NEXT: #pragma clang diagnostic pop
// CHECK-NEXT:     result._destructiveInjectEnumTag(0);
// CHECK-NEXT:     return result;
// CHECK-NEXT:   }
// CHECK-NEXT: template<class T_0_0>
// CHECK-NEXT: #ifdef __cpp_concepts
// CHECK-NEXT: requires swift::isUsableInGenericContext<T_0_0>
// CHECK-NEXT: #endif
// CHECK-NEXT:   SWIFT_INLINE_THUNK bool GenericOpt<T_0_0>::isSome() const {
// CHECK-NEXT:     return *this == GenericOpt<T_0_0>::some;
// CHECK-NEXT:   }
// CHECK-NEXT:   template<class T_0_0>
// CHECK-NEXT:   #ifdef __cpp_concepts
// CHECK-NEXT:   requires swift::isUsableInGenericContext<T_0_0>
// CHECK-NEXT:   #endif
// CHECK-NEXT:     SWIFT_INLINE_THUNK T_0_0 GenericOpt<T_0_0>::getSome() const {
// CHECK-NEXT:       if (!isSome()) abort();
// CHECK-NEXT:       alignas(GenericOpt) unsigned char buffer[sizeof(GenericOpt)];
// CHECK-NEXT:       auto *thisCopy = new(buffer) GenericOpt(*this);
// CHECK-NEXT:       char * _Nonnull payloadFromDestruction = thisCopy->_destructiveProjectEnumData();
// CHECK-NEXT:     #pragma clang diagnostic push
// CHECK-NEXT:     #pragma clang diagnostic ignored "-Wc++17-extensions"
// CHECK-NEXT:     if constexpr (std::is_base_of<::swift::_impl::RefCountedClass, T_0_0>::value) {
// CHECK-NEXT:     void *returnValue;
// CHECK-NEXT:     returnValue = *reinterpret_cast<void **>(payloadFromDestruction);
// CHECK-NEXT:     return ::swift::_impl::implClassFor<T_0_0>::type::makeRetained(returnValue);
// CHECK-NEXT:     } else if constexpr (::swift::_impl::isValueType<T_0_0>) {
// CHECK-NEXT:     return ::swift::_impl::implClassFor<T_0_0>::type::returnNewValue([&](void * _Nonnull returnValue) SWIFT_INLINE_THUNK_ATTRIBUTES {
// CHECK-NEXT:    return ::swift::_impl::implClassFor<T_0_0>::type::initializeWithTake(reinterpret_cast<char * _Nonnull>(returnValue), payloadFromDestruction);
// CHECK-NEXT:     });
// CHECK-NEXT:     } else if constexpr (::swift::_impl::isSwiftBridgedCxxRecord<T_0_0>) {
// CHECK-NEXT:   abort();
// CHECK-NEXT:     } else {
// CHECK-NEXT:     T_0_0 returnValue;
// CHECK-NEXT:   memcpy(&returnValue, payloadFromDestruction, sizeof(returnValue));
// CHECK-NEXT:     return returnValue;
// CHECK-NEXT:     }
// CHECK-NEXT:     #pragma clang diagnostic pop
// CHECK-NEXT:   }
// CHECK-NEXT: template<class T_0_0>
// CHECK-NEXT: #ifdef __cpp_concepts
// CHECK-NEXT: requires swift::isUsableInGenericContext<T_0_0>
// CHECK-NEXT: #endif // __cpp_concepts
// CHECK-NEXT:   SWIFT_INLINE_THUNK GenericOpt<T_0_0> GenericOpt<T_0_0>::_impl_none::operator()() const {
// CHECK-NEXT:     auto result = GenericOpt<T_0_0>::_make();
// CHECK-NEXT:     result._destructiveInjectEnumTag(1);
// CHECK-NEXT:     return result;
// CHECK-NEXT:   }
// CHECK-NEXT: template<class T_0_0>
// CHECK-NEXT: #ifdef __cpp_concepts
// CHECK-NEXT: requires swift::isUsableInGenericContext<T_0_0>
// CHECK-NEXT: #endif
// CHECK-NEXT:   SWIFT_INLINE_THUNK bool GenericOpt<T_0_0>::isNone() const {
// CHECK-NEXT:     return *this == GenericOpt<T_0_0>::none;
// CHECK-NEXT:   }

// CHECK: template<class T_0_0>
// CHECK-NEXT: #ifdef __cpp_concepts
// CHECK-NEXT: requires swift::isUsableInGenericContext<T_0_0>
// CHECK-NEXT: #endif
// CHECK-NEXT: SWIFT_INLINE_THUNK void GenericOpt<T_0_0>::method() const {
// CHECK-NEXT: #ifndef __cpp_concepts
// CHECK-NEXT: static_assert(swift::isUsableInGenericContext<T_0_0>, "type cannot be used in a Swift generic context");
// CHECK-NEXT: #endif
// CHECK-NEXT: _impl::$s8Generics10GenericOptO6methodyyF(swift::TypeMetadataTrait<GenericOpt<T_0_0>>::getTypeMetadata(), _getOpaquePointer());
// CHECK-NEXT: }

// CHECK: template<class T_0_0>
// CHECK-NEXT: #ifdef __cpp_concepts
// CHECK-NEXT: requires swift::isUsableInGenericContext<T_0_0>
// CHECK-NEXT: #endif
// CHECK-NEXT: void GenericOpt<T_0_0>::reset()

// CHECK: template<class T_0_0>
// CHECK-NEXT: #ifdef __cpp_concepts
// CHECK-NEXT: requires swift::isUsableInGenericContext<T_0_0>
// CHECK-NEXT: #endif
// CHECK-NEXT: template<class T_1_0>
// CHECK-NEXT: #ifdef __cpp_concepts
// CHECK-NEXT: requires swift::isUsableInGenericContext<T_1_0>
// CHECK-NEXT: #endif
// CHECK-NEXT: SWIFT_INLINE_THUNK T_1_0 GenericOpt<T_0_0>::genericMethod(const T_1_0& x) const {

// CHECK: template<class T_0_0>
// CHECK-NEXT: #ifdef __cpp_concepts
// CHECK-NEXT: requires swift::isUsableInGenericContext<T_0_0>
// CHECK-NEXT: #endif
// CHECK-NEXT: SWIFT_INLINE_THUNK swift::Int GenericOpt<T_0_0>::getComputedProp() const {
// CHECK-NEXT: #ifndef __cpp_concepts
// CHECK-NEXT: static_assert(swift::isUsableInGenericContext<T_0_0>, "type cannot be used in a Swift generic context");
// CHECK-NEXT: #endif
// CHECK-NEXT: return _impl::$s8Generics10GenericOptO12computedPropSivg(swift::TypeMetadataTrait<GenericOpt<T_0_0>>::getTypeMetadata(), _getOpaquePointer());
// CHECK-NEXT: }
