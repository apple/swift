// RUN: %empty-directory(%t)
// RUN: %target-swift-frontend %s -typecheck -module-name Functions -clang-header-expose-decls=all-public -emit-clang-header-path %t/functions.h
// RUN: %FileCheck %s < %t/functions.h

// RUN: %check-interop-cxx-header-in-clang(%t/functions.h -Wno-unused-function)

// RUN: %target-swift-frontend %s -typecheck -module-name Functions -enable-library-evolution -clang-header-expose-decls=all-public -emit-clang-header-path %t/functions-evo.h
// RUN: %FileCheck %s < %t/functions-evo.h

// RUN: %check-interop-cxx-header-in-clang(%t/functions-evo.h -Wno-unused-function)

public func genericPrintFunctionTwoArg<T>(_ x: T, _ y: Int) {
    print("X:", x)
    print("Y:", y)
}

public func genericPrintFunction<T>(_ x: T) {
    print("\(T.self) value=\(x)")
}

public func genericPrintFunctionMultiGeneric<T1, T2>(_ x: Int, _ t1: T1, _ t1p: T1, _ y: Int, _ t2: T2) {
    print("\(T1.self) value 1=\(t1)")
    print("\(T1.self) value 2=\(t1p)")
    print("\(T2.self) value 1=\(t2)")
    print("other values=\(x),\(y)")
}

public func genericSwap<T>(_ x: inout T, _ y: inout T) {
  let t = x
  x = y
  y = t
}

public func genericRet<T>(_ x: T) -> T {
    return x
}

public func genericRequirementProtocol<T: Hashable>(_ x: T) {
}

public func genericRequirementClass<T>(_ x: T) where T: TestClass {
}

public class TestClass {
    let field: Int

    init() { field = 0 }
    deinit { print("deinit TestClass") }
}

public func createTestClass() -> TestClass { return TestClass() }

public struct TestLargeStruct {
    var x1, x2, x3, x4, x5, x6: Int

    init(_ x: Int) {
        x1 = x
        x2 = x+1
        x3 = x-1
        x4 = x
        x5 = x+2
        x6 = x-2
    }

    public mutating func mut() {
        x1 = -x1
        x6 = x5
    }
}

public func createTestLargeStruct(_ x: Int) -> TestLargeStruct {
    return TestLargeStruct(x)
}

@frozen
public struct TestSmallStruct {
    var x1: UInt32

    public mutating func mut() {
        x1 = ~x1
    }

    public func genericMethodPassThrough<T>(_ x: T) -> T {
        return x
    }

    public mutating func genericMethodMutTake<T>(_ x: T) {
        if let y = x as? UInt32 {
            x1 += y
        } else {
            x1 -= 1
        }
    }
}

public func createTestSmallStruct(_ x: UInt32) -> TestSmallStruct {
    return TestSmallStruct(x1: x)
}

// CHECK: SWIFT_EXTERN void $s9Functions15TestSmallStructV24genericMethodPassThroughyxxlF(SWIFT_INDIRECT_RESULT void * _Nonnull, const void * _Nonnull x, struct swift_interop_passStub_Functions_uint32_t_0_4 _self, void * _Nonnull ) SWIFT_NOEXCEPT SWIFT_CALL; // genericMethodPassThrough(_:)
// CHECK-NEXT: SWIFT_EXTERN void $s9Functions15TestSmallStructV20genericMethodMutTakeyyxlF(const void * _Nonnull x, void * _Nonnull , SWIFT_CONTEXT void * _Nonnull _self) SWIFT_NOEXCEPT SWIFT_CALL; // genericMethodMutTake(_:)

// CHECK: SWIFT_EXTERN void $s9Functions20genericPrintFunctionyyxlF(const void * _Nonnull x, void * _Nonnull ) SWIFT_NOEXCEPT SWIFT_CALL; // genericPrintFunction(_:)
// CHECK-NEXT: SWIFT_EXTERN void $s9Functions32genericPrintFunctionMultiGenericyySi_xxSiq_tr0_lF(ptrdiff_t x, const void * _Nonnull t1, const void * _Nonnull t1p, ptrdiff_t y, const void * _Nonnull t2, void * _Nonnull , void * _Nonnull ) SWIFT_NOEXCEPT SWIFT_CALL; // genericPrintFunctionMultiGeneric(_:_:_:_:_:)
// CHECK-NEXT: SWIFT_EXTERN void $s9Functions26genericPrintFunctionTwoArgyyx_SitlF(const void * _Nonnull x, ptrdiff_t y, void * _Nonnull ) SWIFT_NOEXCEPT SWIFT_CALL; // genericPrintFunctionTwoArg(_:_:)
// CHECK-NEXT: SWIFT_EXTERN void $s9Functions10genericRetyxxlF(SWIFT_INDIRECT_RESULT void * _Nonnull, const void * _Nonnull x, void * _Nonnull ) SWIFT_NOEXCEPT SWIFT_CALL; // genericRet(_:)
// CHECK-NEXT: SWIFT_EXTERN void $s9Functions11genericSwapyyxz_xztlF(void * _Nonnull x, void * _Nonnull y, void * _Nonnull ) SWIFT_NOEXCEPT SWIFT_CALL; // genericSwap(_:_:)

// CHECK-NOT: genericRequirement

// Skip templates in impl classes.
// CHECK: _impl_TestSmallStruct
// CHECK:      template<class T_0_0>
// CHECK-NEXT: #ifdef __cpp_concepts
// CHECK-NEXT: requires swift::isUsableInGenericContext<T_0_0>
// CHECK-NEXT: #endif
// CHECK-NEXT: SWIFT_INLINE_THUNK T_0_0 genericMethodPassThrough(const T_0_0& x) const SWIFT_SYMBOL("s:9Functions15TestSmallStructV24genericMethodPassThroughyxxlF");
// CHECK-NEXT: template<class T_0_0>
// CHECK-NEXT: #ifdef __cpp_concepts
// CHECK-NEXT: requires swift::isUsableInGenericContext<T_0_0>
// CHECK-NEXT: #endif
// CHECK-NEXT: SWIFT_INLINE_THUNK void genericMethodMutTake(const T_0_0& x) SWIFT_SYMBOL("s:9Functions15TestSmallStructV20genericMethodMutTakeyyxlF");
// CHECK:      template<class T>
// CHECK-NEXT: returnNewValue

// CHECK:      template<class T_0_0>
// CHECK-NEXT: #ifdef __cpp_concepts
// CHECK-NEXT: requires swift::isUsableInGenericContext<T_0_0>
// CHECK-NEXT: #endif
// CHECK-NEXT: SWIFT_INLINE_THUNK void genericPrintFunction(const T_0_0& x) noexcept SWIFT_SYMBOL("s:9Functions20genericPrintFunctionyyxlF") {
// CHECK-NEXT: #ifndef __cpp_concepts
// CHECK-NEXT: static_assert(swift::isUsableInGenericContext<T_0_0>, "type cannot be used in a Swift generic context");
// CHECK-NEXT: #endif
// CHECK-NEXT:   _impl::$s9Functions20genericPrintFunctionyyxlF(swift::_impl::getOpaquePointer(x), swift::TypeMetadataTrait<T_0_0>::getTypeMetadata());
// CHECK-NEXT: }


// CHECK:      template<class T_0_0, class T_0_1>
// CHECK-NEXT: #ifdef __cpp_concepts
// CHECK-NEXT: requires swift::isUsableInGenericContext<T_0_0> && swift::isUsableInGenericContext<T_0_1>
// CHECK-NEXT: #endif
// CHECK-NEXT: SWIFT_INLINE_THUNK void genericPrintFunctionMultiGeneric(swift::Int x, const T_0_0& t1, const T_0_0& t1p, swift::Int y, const T_0_1& t2) noexcept SWIFT_SYMBOL("s:9Functions32genericPrintFunctionMultiGenericyySi_xxSiq_tr0_lF") {
// CHECK-NEXT: #ifndef __cpp_concepts
// CHECK-NEXT: static_assert(swift::isUsableInGenericContext<T_0_0>, "type cannot be used in a Swift generic context");
// CHECK-NEXT: static_assert(swift::isUsableInGenericContext<T_0_1>, "type cannot be used in a Swift generic context");
// CHECK-NEXT: #endif
// CHECK-NEXT:   _impl::$s9Functions32genericPrintFunctionMultiGenericyySi_xxSiq_tr0_lF(x, swift::_impl::getOpaquePointer(t1), swift::_impl::getOpaquePointer(t1p), y, swift::_impl::getOpaquePointer(t2), swift::TypeMetadataTrait<T_0_0>::getTypeMetadata(), swift::TypeMetadataTrait<T_0_1>::getTypeMetadata());
// CHECK-NEXT: }


// CHECK:      template<class T_0_0>
// CHECK-NEXT: #ifdef __cpp_concepts
// CHECK-NEXT: requires swift::isUsableInGenericContext<T_0_0>
// CHECK-NEXT: #endif
// CHECK-NEXT: SWIFT_INLINE_THUNK void genericPrintFunctionTwoArg(const T_0_0& x, swift::Int y) noexcept SWIFT_SYMBOL("s:9Functions26genericPrintFunctionTwoArgyyx_SitlF") {
// CHECK-NEXT: #ifndef __cpp_concepts
// CHECK-NEXT: static_assert(swift::isUsableInGenericContext<T_0_0>, "type cannot be used in a Swift generic context");
// CHECK-NEXT: #endif
// CHECK-NEXT:   _impl::$s9Functions26genericPrintFunctionTwoArgyyx_SitlF(swift::_impl::getOpaquePointer(x), y, swift::TypeMetadataTrait<T_0_0>::getTypeMetadata());
// CHECK-NEXT: }

// CHECK:      template<class T_0_0>
// CHECK-NEXT: #ifdef __cpp_concepts
// CHECK-NEXT: requires swift::isUsableInGenericContext<T_0_0>
// CHECK-NEXT: #endif
// CHECK-NEXT: SWIFT_INLINE_THUNK T_0_0 genericRet(const T_0_0& x) noexcept SWIFT_SYMBOL("s:9Functions10genericRetyxxlF") SWIFT_WARN_UNUSED_RESULT {
// CHECK-NEXT: #ifndef __cpp_concepts
// CHECK-NEXT: static_assert(swift::isUsableInGenericContext<T_0_0>, "type cannot be used in a Swift generic context");
// CHECK-NEXT: #endif
// CHECK-NEXT: #pragma clang diagnostic push
// CHECK-NEXT: #pragma clang diagnostic ignored "-Wc++17-extensions"
// CHECK-NEXT:    if constexpr (std::is_base_of<::swift::_impl::RefCountedClass, T_0_0>::value) {
// CHECK-NEXT:    void *returnValue;
// CHECK-NEXT:    _impl::$s9Functions10genericRetyxxlF(reinterpret_cast<void *>(&returnValue), swift::_impl::getOpaquePointer(x), swift::TypeMetadataTrait<T_0_0>::getTypeMetadata());
// CHECK-NEXT:    return ::swift::_impl::implClassFor<T_0_0>::type::makeRetained(returnValue);
// CHECK-NEXT:    } else if constexpr (::swift::_impl::isValueType<T_0_0>) {
// CHECK-NEXT:    return ::swift::_impl::implClassFor<T_0_0>::type::returnNewValue([&](void * _Nonnull returnValue) SWIFT_INLINE_THUNK_ATTRIBUTES {
// CHECK-NEXT:    _impl::$s9Functions10genericRetyxxlF(returnValue, swift::_impl::getOpaquePointer(x), swift::TypeMetadataTrait<T_0_0>::getTypeMetadata());
// CHECK-NEXT:    });
// CHECK-NEXT:    } else if constexpr (::swift::_impl::isSwiftBridgedCxxRecord<T_0_0>) {
// CHECK-NEXT:    alignas(alignof(T_0_0)) char storage[sizeof(T_0_0)];
// CHECK-NEXT:    auto * _Nonnull storageObjectPtr = reinterpret_cast<T_0_0 *>(storage);
// CHECK-NEXT:    _impl::$s9Functions10genericRetyxxlF(storage, swift::_impl::getOpaquePointer(x), swift::TypeMetadataTrait<T_0_0>::getTypeMetadata());
// CHECK-NEXT:    T_0_0 result(static_cast<T_0_0 &&>(*storageObjectPtr));
// CHECK-NEXT:    storageObjectPtr->~T_0_0();
// CHECK-NEXT:    return result;
// CHECK-NEXT:    } else {
// CHECK-NEXT:    T_0_0 returnValue;
// CHECK-NEXT:    _impl::$s9Functions10genericRetyxxlF(reinterpret_cast<void *>(&returnValue), swift::_impl::getOpaquePointer(x), swift::TypeMetadataTrait<T_0_0>::getTypeMetadata());
// CHECK-NEXT:    return returnValue;
// CHECK-NEXT:    }
// CHECK-NEXT:  #pragma clang diagnostic pop
// CHECK-NEXT:  }

// CHECK:      template<class T_0_0>
// CHECK-NEXT: #ifdef __cpp_concepts
// CHECK-NEXT: requires swift::isUsableInGenericContext<T_0_0>
// CHECK-NEXT: #endif
// CHECK-NEXT: SWIFT_INLINE_THUNK void genericSwap(T_0_0& x, T_0_0& y) noexcept SWIFT_SYMBOL("s:9Functions11genericSwapyyxz_xztlF") {
// CHECK-NEXT: #ifndef __cpp_concepts
// CHECK-NEXT: static_assert(swift::isUsableInGenericContext<T_0_0>, "type cannot be used in a Swift generic context");
// CHECK-NEXT: #endif
// CHECK-NEXT:   _impl::$s9Functions11genericSwapyyxz_xztlF(swift::_impl::getOpaquePointer(x), swift::_impl::getOpaquePointer(y), swift::TypeMetadataTrait<T_0_0>::getTypeMetadata());
// CHECK-NEXT: }

// CHECK:      template<class T_0_0>
// CHECK-NEXT: #ifdef __cpp_concepts
// CHECK-NEXT: requires swift::isUsableInGenericContext<T_0_0>
// CHECK-NEXT: #endif
// CHECK-NEXT: SWIFT_INLINE_THUNK T_0_0 TestSmallStruct::genericMethodPassThrough(const T_0_0& x) const {
// CHECK-NEXT: #ifndef __cpp_concepts
// CHECK-NEXT: static_assert(swift::isUsableInGenericContext<T_0_0>, "type cannot be used in a Swift generic context");
// CHECK-NEXT: #endif
// CHECK-NEXT: #pragma clang diagnostic push
// CHECK-NEXT: #pragma clang diagnostic ignored "-Wc++17-extensions"
// CHECK-NEXT:   if constexpr (std::is_base_of<::swift::_impl::RefCountedClass, T_0_0>::value) {
// CHECK-NEXT:   void *returnValue;
// CHECK-NEXT:   _impl::$s9Functions15TestSmallStructV24genericMethodPassThroughyxxlF(reinterpret_cast<void *>(&returnValue), swift::_impl::getOpaquePointer(x), _impl::swift_interop_passDirect_Functions_uint32_t_0_4(_getOpaquePointer()), swift::TypeMetadataTrait<T_0_0>::getTypeMetadata());
// CHECK-NEXT:   return ::swift::_impl::implClassFor<T_0_0>::type::makeRetained(returnValue);
// CHECK-NEXT:   } else if constexpr (::swift::_impl::isValueType<T_0_0>) {
// CHECK-NEXT:   return ::swift::_impl::implClassFor<T_0_0>::type::returnNewValue([&](void * _Nonnull returnValue) SWIFT_INLINE_THUNK_ATTRIBUTES {
// CHECK-NEXT: _impl::$s9Functions15TestSmallStructV24genericMethodPassThroughyxxlF(returnValue, swift::_impl::getOpaquePointer(x), _impl::swift_interop_passDirect_Functions_uint32_t_0_4(_getOpaquePointer()), swift::TypeMetadataTrait<T_0_0>::getTypeMetadata());
// CHECK-NEXT:   });
// CHECK-NEXT:   } else if constexpr (::swift::_impl::isSwiftBridgedCxxRecord<T_0_0>) {
// CHECK-NEXT:    alignas(alignof(T_0_0)) char storage[sizeof(T_0_0)];
// CHECK-NEXT:    auto * _Nonnull storageObjectPtr = reinterpret_cast<T_0_0 *>(storage);
// CHECK-NEXT:    _impl::$s9Functions15TestSmallStructV24genericMethodPassThroughyxxlF(storage, swift::_impl::getOpaquePointer(x), _impl::swift_interop_passDirect_Functions_uint32_t_0_4(_getOpaquePointer()), swift::TypeMetadataTrait<T_0_0>::getTypeMetadata())
// CHECK-NEXT:    T_0_0 result(static_cast<T_0_0 &&>(*storageObjectPtr));
// CHECK-NEXT:    storageObjectPtr->~T_0_0();
// CHECK-NEXT:    return result;
// CHECK-NEXT:   } else {
// CHECK-NEXT:   T_0_0 returnValue;
// CHECK-NEXT: _impl::$s9Functions15TestSmallStructV24genericMethodPassThroughyxxlF(reinterpret_cast<void *>(&returnValue), swift::_impl::getOpaquePointer(x), _impl::swift_interop_passDirect_Functions_uint32_t_0_4(_getOpaquePointer()), swift::TypeMetadataTrait<T_0_0>::getTypeMetadata());
// CHECK-NEXT:   return returnValue;
// CHECK-NEXT:   }
// CHECK-NEXT:   #pragma clang diagnostic pop
// CHECK-NEXT:   }
// CHECK-NEXT:   template<class T_0_0>
// CHECK-NEXT: #ifdef __cpp_concepts
// CHECK-NEXT: requires swift::isUsableInGenericContext<T_0_0>
// CHECK-NEXT: #endif
// CHECK-NEXT: SWIFT_INLINE_THUNK void TestSmallStruct::genericMethodMutTake(const T_0_0& x) {
// CHECK-NEXT: #ifndef __cpp_concepts
// CHECK-NEXT: static_assert(swift::isUsableInGenericContext<T_0_0>, "type cannot be used in a Swift generic context");
// CHECK-NEXT: #endif
// CHECK-NEXT:   _impl::$s9Functions15TestSmallStructV20genericMethodMutTakeyyxlF(swift::_impl::getOpaquePointer(x), swift::TypeMetadataTrait<T_0_0>::getTypeMetadata(), _getOpaquePointer());
// CHECK-NEXT:   }
