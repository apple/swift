// RUN: %empty-directory(%t)
// RUN: %target-swift-frontend %s -typecheck -module-name Init -clang-header-expose-decls=all-public -emit-clang-header-path %t/inits.h
// RUN: %FileCheck --check-prefixes=CHECK,CHECK-NON_EVO %s < %t/inits.h
// RUN: %check-interop-cxx-header-in-clang(%t/inits.h)

// RUN: %target-swift-frontend %s -typecheck -module-name Init -clang-header-expose-decls=all-public -emit-clang-header-path %t/inits-evo.h -enable-library-evolution
// RUN: %FileCheck %s < %t/inits-evo.h
// RUN: %check-interop-cxx-header-in-clang(%t/inits-evo.h)

public final class AKlass {
    public init() {}
    deinit {
        print("destroy AKlass")
    }
    consuming public func takeKlass() {}
}

public struct SmallStruct {
    public let x: UInt32
}

public struct SmallStructNonTrivial {
    public let k: AKlass
    public let x: UInt32
}

public struct LargeStructNonTrivial {
    public let x1, x2, x3, x4, x5, x6: Int
    public let k: AKlass

    consuming public func takeMe() {}
}

public func createSmallStructNonTrivial(_ k: AKlass) -> SmallStructNonTrivial {
    return SmallStructNonTrivial(k: k, x: 0)
}

public enum EnumNonTrivial {
    case a(Int)
    case b(AKlass)
}

public struct InitFromSmall {
    public init(_ x : SmallStructNonTrivial) {
        self.x = 0
    }

    public func takeSmall(_ x: consuming SmallStruct) {
    }
    public func takeSmallLarge(_: consuming SmallStructNonTrivial,
                               _: consuming LargeStructNonTrivial) {
    }

    let x: Int
}

public struct InitFromLargeStructNonTrivial {
    public init(_ x : LargeStructNonTrivial) {
        self.x = 0
    }

    let x: Int
}

public struct InitFromKlass {
    public init(_ x : AKlass) {
        self.x = 0
    }

    let x: Int
}

public struct InitFromEnumNonTrivial {
    public init(_ x : EnumNonTrivial) {
        self.x = 0
    }

    let x: Int
}

public struct TheGenericContainer<T> {
    public init(_ x : T) { self.x = x }

    consuming public func takeGenericContainer() {}

    let x: T
}

public struct TheGenericContainerInitTriv {
    public init(_ x : TheGenericContainer<Int>) {
        self.x = 0
    }

    let x: Int
}

public struct TheGenericContainerInitNonTriv {
    public init(_ x : TheGenericContainer<AKlass>) {
        self.x = 0
    }

    let x: Int
}

// CHECK: SWIFT_INLINE_THUNK void AKlass::takeKlass() {
// CHECK-NEXT: alignas(alignof(AKlass)) char copyBuffer_consumedParamCopy_this[sizeof(AKlass)];
// CHECK-NEXT: auto &consumedParamCopy_this = *(new(copyBuffer_consumedParamCopy_this) AKlass(*this));
// CHECK-NEXT: swift::_impl::ConsumedValueStorageDestroyer<AKlass> storageGuard_consumedParamCopy_this(consumedParamCopy_this);
// CHECK-NEXT: _impl::$s4Init6AKlassC9takeKlassyyF(::swift::_impl::_impl_RefCountedClass::getOpaquePointer(consumedParamCopy_this));
// CHECK-NEXT: }

// CHECK: SWIFT_INLINE_THUNK InitFromEnumNonTrivial InitFromEnumNonTrivial::init(const EnumNonTrivial& x) {
// CHECK-NEXT: alignas(alignof(EnumNonTrivial)) char copyBuffer_consumedParamCopy_x[sizeof(EnumNonTrivial)];
// CHECK-NEXT: auto &consumedParamCopy_x = *(new(copyBuffer_consumedParamCopy_x) EnumNonTrivial(x));
// CHECK-NEXT: ConsumedValueStorageDestroyer<EnumNonTrivial> storageGuard_consumedParamCopy_x(consumedParamCopy_x);
// CHECK-NEXT: returnNewValue
// CHECK-NEXT: _impl::_impl_EnumNonTrivial::getOpaquePointer(consumedParamCopy_x)

// CHECK: SWIFT_INLINE_THUNK InitFromKlass InitFromKlass::init(const AKlass& x) {
// CHECK-NEXT: alignas(alignof(AKlass)) char copyBuffer_consumedParamCopy_x[sizeof(AKlass)];
// CHECK-NEXT: auto &consumedParamCopy_x = *(new(copyBuffer_consumedParamCopy_x) AKlass(x));
// CHECK-NEXT: swift::_impl::ConsumedValueStorageDestroyer<AKlass> storageGuard_consumedParamCopy_x(consumedParamCopy_x);
// CHECK-NEXT: returnNewValue
// CHECK-NEXT: swift::_impl::_impl_RefCountedClass::getOpaquePointer(consumedParamCopy_x)

// CHECK: SWIFT_INLINE_THUNK InitFromLargeStructNonTrivial InitFromLargeStructNonTrivial::init(const LargeStructNonTrivial& x) {
// CHECK-NEXT: alignas(alignof(LargeStructNonTrivial)) char copyBuffer_consumedParamCopy_x[sizeof(LargeStructNonTrivial)];
// CHECK-NEXT: auto &consumedParamCopy_x = *(new(copyBuffer_consumedParamCopy_x) LargeStructNonTrivial(x));
// CHECK-NEXT: swift::_impl::ConsumedValueStorageDestroyer<LargeStructNonTrivial> storageGuard_consumedParamCopy_x(consumedParamCopy_x);

// CHECK:  SWIFT_INLINE_THUNK InitFromSmall InitFromSmall::init(const SmallStructNonTrivial& x) {
// CHECK-NEXT: alignas(alignof(SmallStructNonTrivial)) char copyBuffer_consumedParamCopy_x[sizeof(SmallStructNonTrivial)];
// CHECK-NEXT: auto &consumedParamCopy_x = *(new(copyBuffer_consumedParamCopy_x) SmallStructNonTrivial(x));
// CHECK-NEXT: swift::_impl::ConsumedValueStorageDestroyer<SmallStructNonTrivial> storageGuard_consumedParamCopy_x(consumedParamCopy_x);

// CHECK: SWIFT_INLINE_THUNK void InitFromSmall::takeSmallLarge(const SmallStructNonTrivial& _1, const LargeStructNonTrivial& _2) const {
// CHECK-NEXT: alignas(alignof(SmallStructNonTrivial)) char copyBuffer_consumedParamCopy_1[sizeof(SmallStructNonTrivial)];
// CHECK-NEXT: auto &consumedParamCopy_1 = *(new(copyBuffer_consumedParamCopy_1) SmallStructNonTrivial(_1));
// CHECK-NEXT: swift::_impl::ConsumedValueStorageDestroyer<SmallStructNonTrivial> storageGuard_consumedParamCopy_1(consumedParamCopy_1);
// CHECK-NEXT: alignas(alignof(LargeStructNonTrivial)) char copyBuffer_consumedParamCopy_2[sizeof(LargeStructNonTrivial)];
// CHECK-NEXT: auto &consumedParamCopy_2 = *(new(copyBuffer_consumedParamCopy_2) LargeStructNonTrivial(_2));
// CHECK-NEXT: swift::_impl::ConsumedValueStorageDestroyer<LargeStructNonTrivial> storageGuard_consumedParamCopy_2(consumedParamCopy_2);
// CHECK-NON_EVO-NEXT: _impl::$s4Init0A9FromSmallV04takeC5LargeyyAA0C16StructNonTrivialVn_AA0efgH0VntF(_impl::swift_interop_passDirect_Init_{{.*}}(_impl::_impl_SmallStructNonTrivial::getOpaquePointer(consumedParamCopy_1)), _impl::_impl_LargeStructNonTrivial::getOpaquePointer(consumedParamCopy_2), _impl::swift_interop_passDirect_Init_{{.*}}(_getOpaquePointer()));

// CHECK: SWIFT_INLINE_THUNK void LargeStructNonTrivial::takeMe() const {
// CHECK-NEXT: alignas(alignof(LargeStructNonTrivial)) char copyBuffer_consumedParamCopy_this[sizeof(LargeStructNonTrivial)];
// CHECK-NEXT: auto &consumedParamCopy_this = *(new(copyBuffer_consumedParamCopy_this) LargeStructNonTrivial(*this));
// CHECK-NEXT: swift::_impl::ConsumedValueStorageDestroyer<LargeStructNonTrivial> storageGuard_consumedParamCopy_this(consumedParamCopy_this);
// CHECK-NEXT: _impl::$s4Init21LargeStructNonTrivialV6takeMeyyF(_impl::_impl_LargeStructNonTrivial::getOpaquePointer(consumedParamCopy_this));
// CHECK-NEXT: }

// CHECK: SWIFT_INLINE_THUNK TheGenericContainer<T_0_0> TheGenericContainer<T_0_0>::init(const T_0_0& x) {
//CHECK-NEXT:#ifndef __cpp_concepts
//CHECK-NEXT: static_assert
//CHECK-NEXT:#endif // __cpp_concepts
//CHECK-NEXT: alignas(alignof(T_0_0)) char copyBuffer_consumedParamCopy_x[sizeof(T_0_0)];
//CHECK-NEXT: auto &consumedParamCopy_x = *(new(copyBuffer_consumedParamCopy_x) T_0_0(x));
//CHECK-NEXT: swift::_impl::ConsumedValueStorageDestroyer<T_0_0> storageGuard_consumedParamCopy_x(consumedParamCopy_x);
//CHECK-NEXT: returnNewValue
//CHECK-NEXT: swift::_impl::getOpaquePointer(consumedParamCopy_x)

// CHECK: SWIFT_INLINE_THUNK void TheGenericContainer<T_0_0>::takeGenericContainer() const {
// CHECK-NEXT: #ifndef __cpp_concepts
// CHECK-NEXT: static_assert
// CHECK-NEXT: #endif // __cpp_concepts
// CHECK-NEXT: alignas(alignof(TheGenericContainer<T_0_0>)) char copyBuffer_consumedParamCopy_this[sizeof(TheGenericContainer<T_0_0>)];
// CHECK-NEXT: auto &consumedParamCopy_this = *(new(copyBuffer_consumedParamCopy_this) TheGenericContainer<T_0_0>(*this));
// CHECK-NEXT: swift::_impl::ConsumedValueStorageDestroyer<TheGenericContainer<T_0_0>> storageGuard_consumedParamCopy_this(consumedParamCopy_this);
// CHECK-NEXT: _impl::$s4Init19TheGenericContainerV04takecD0yyF(swift::TypeMetadataTrait<TheGenericContainer<T_0_0>>::getTypeMetadata(), _impl::_impl_TheGenericContainer<T_0_0>::getOpaquePointer(consumedParamCopy_this));
// CHECK-NEXT: }

// CHECK: SWIFT_INLINE_THUNK TheGenericContainerInitNonTriv TheGenericContainerInitNonTriv::init(const TheGenericContainer<AKlass>& x) {
// CHECK-NEXT: alignas(alignof(TheGenericContainer<AKlass>)) char copyBuffer_consumedParamCopy_x[sizeof(TheGenericContainer<AKlass>)];
// CHECK-NEXT: auto &consumedParamCopy_x = *(new(copyBuffer_consumedParamCopy_x) TheGenericContainer<AKlass>(x));
// CHECK-NEXT: swift::_impl::ConsumedValueStorageDestroyer<TheGenericContainer<AKlass>> storageGuard_consumedParamCopy_x(consumedParamCopy_x);

// CHECK: SWIFT_INLINE_THUNK TheGenericContainerInitTriv TheGenericContainerInitTriv::init(const TheGenericContainer<swift::Int>& x) {
// CHECK-NON_EVO-NEXT: return
