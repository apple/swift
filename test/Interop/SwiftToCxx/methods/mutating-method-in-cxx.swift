// RUN: %empty-directory(%t)
// RUN: %target-swift-frontend %s -typecheck -module-name Methods -clang-header-expose-decls=all-public -emit-clang-header-path %t/methods.h
// RUN: %FileCheck %s < %t/methods.h

// RUN: %check-interop-cxx-header-in-clang(%t/methods.h)

public struct LargeStruct {
    var x1, x2, x3, x4, x5, x6: Int

    public func dump() {
        print("\(x1), \(x2), \(x3), \(x4), \(x5), \(x6)")
    }

    public mutating func double() {
        x1 *= 2
        x2 *= 2
        x3 *= 2
        x4 *= 2
        x5 *= 2
        x6 *= 2
    }

    public mutating func scale(_ x: Int, _ y: Int) -> LargeStruct {
        x1 *= x
        x2 *= y
        x3 *= x
        x4 *= y
        x5 *= x
        x6 *= y
        return self
    }
}

public struct SmallStruct {
    var x: Float

    public func dump() {
        print("small x = \(x);")
    }

    public mutating func scale(_ y: Float) -> SmallStruct {
        x *= y
        return SmallStruct(x: x / y)
    }

    public mutating func invert() {
        x = -x
    }
}

// CHECK: SWIFT_EXTERN void $s7Methods11LargeStructV4dumpyyF(SWIFT_CONTEXT const void * _Nonnull _self) SWIFT_NOEXCEPT SWIFT_CALL; // dump()
// CHECK: SWIFT_EXTERN void $s7Methods11LargeStructV6doubleyyF(SWIFT_CONTEXT void * _Nonnull _self) SWIFT_NOEXCEPT SWIFT_CALL; // double()
// CHECK: SWIFT_EXTERN void $s7Methods11LargeStructV5scaleyACSi_SitF(SWIFT_INDIRECT_RESULT void * _Nonnull, ptrdiff_t x, ptrdiff_t y, SWIFT_CONTEXT void * _Nonnull _self) SWIFT_NOEXCEPT SWIFT_CALL; // scale(_:_:)

// CHECK: SWIFT_EXTERN void $s7Methods11SmallStructV4dumpyyF(struct swift_interop_passStub_Methods_float_0_4 _self) SWIFT_NOEXCEPT SWIFT_CALL; // dump()
// CHECK: SWIFT_EXTERN struct swift_interop_returnStub_Methods_float_0_4 $s7Methods11SmallStructV5scaleyACSfF(float y, SWIFT_CONTEXT void * _Nonnull _self) SWIFT_NOEXCEPT SWIFT_CALL; // scale(_:)
// CHECK: SWIFT_EXTERN void $s7Methods11SmallStructV6invertyyF(SWIFT_CONTEXT void * _Nonnull _self) SWIFT_NOEXCEPT SWIFT_CALL; // invert()

// CHECK: class SWIFT_SYMBOL("s:7Methods11LargeStructV") LargeStruct final {
// CHECK: SWIFT_INLINE_PRIVATE_HELPER LargeStruct(LargeStruct &&)
// CHECK: }
// CHECK-NEXT:   SWIFT_INLINE_THUNK void dump() const SWIFT_SYMBOL("s:7Methods11LargeStructV4dumpyyF");
// CHECK-NEXT:   SWIFT_INLINE_THUNK void double_() SWIFT_SYMBOL("s:7Methods11LargeStructV6doubleyyF");
// CHECK-NEXT:   SWIFT_INLINE_THUNK LargeStruct scale(swift::Int x, swift::Int y) SWIFT_SYMBOL("s:7Methods11LargeStructV5scaleyACSi_SitF");
// CHECK-NEXT: private

// CHECK: class SWIFT_SYMBOL("s:7Methods11SmallStructV") SmallStruct final {
// CHECK:   SWIFT_INLINE_PRIVATE_HELPER SmallStruct(SmallStruct &&)
// CHECK: }
// CHECK-NEXT:   SWIFT_INLINE_THUNK void dump() const SWIFT_SYMBOL("s:7Methods11SmallStructV4dumpyyF");
// CHECK-NEXT:   SWIFT_INLINE_THUNK SmallStruct scale(float y) SWIFT_SYMBOL("s:7Methods11SmallStructV5scaleyACSfF");
// CHECK-NEXT:   SWIFT_INLINE_THUNK void invert() SWIFT_SYMBOL("s:7Methods11SmallStructV6invertyyF");
// CHECK-NEXT: private:


public func createLargeStruct() -> LargeStruct {
    return LargeStruct(x1: 1, x2: -5, x3: 9, x4: 11, x5: 0xbeef, x6: -77)
}

public func createSmallStruct(x: Float) -> SmallStruct {
    return SmallStruct(x: x)
}

// CHECK:        SWIFT_INLINE_THUNK void LargeStruct::dump() const {
// CHECK-NEXT:   return _impl::$s7Methods11LargeStructV4dumpyyF(_getOpaquePointer());
// CHECK-NEXT:   }
// CHECK-NEXT:   SWIFT_INLINE_THUNK void LargeStruct::double_() {
// CHECK-NEXT:   return _impl::$s7Methods11LargeStructV6doubleyyF(_getOpaquePointer());
// CHECK-NEXT:   }
// CHECK-NEXT:   SWIFT_INLINE_THUNK LargeStruct LargeStruct::scale(swift::Int x, swift::Int y) {
// CHECK-NEXT:   return _impl::_impl_LargeStruct::returnNewValue([&](char * _Nonnull result) SWIFT_INLINE_THUNK_ATTRIBUTES {
// CHECK-NEXT:     _impl::$s7Methods11LargeStructV5scaleyACSi_SitF(result, x, y, _getOpaquePointer());
// CHECK-NEXT:   });
// CHECK-NEXT:   }

// CHECK:        SWIFT_INLINE_THUNK void SmallStruct::dump() const {
// CHECK-NEXT:   return _impl::$s7Methods11SmallStructV4dumpyyF(_impl::swift_interop_passDirect_Methods_float_0_4(_getOpaquePointer()));
// CHECK-NEXT:   }
// CHECK-NEXT:   SWIFT_INLINE_THUNK SmallStruct SmallStruct::scale(float y) {
// CHECK-NEXT:   return _impl::_impl_SmallStruct::returnNewValue([&](char * _Nonnull result) SWIFT_INLINE_THUNK_ATTRIBUTES {
// CHECK-NEXT:     _impl::swift_interop_returnDirect_Methods_float_0_4(result, _impl::$s7Methods11SmallStructV5scaleyACSfF(y, _getOpaquePointer()));
// CHECK-NEXT:   });
// CHECK-NEXT:   }
// CHECK-NEXT:   SWIFT_INLINE_THUNK void SmallStruct::invert() {
// CHECK-NEXT:   return _impl::$s7Methods11SmallStructV6invertyyF(_getOpaquePointer());
// CHECK-NEXT:   }
