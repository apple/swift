// RUN: %empty-directory(%t)
// RUN: %target-swift-frontend %s -typecheck -module-name Properties -clang-header-expose-decls=all-public -emit-clang-header-path %t/properties.h
// RUN: %FileCheck %s < %t/properties.h

// RUN: %check-interop-cxx-header-in-clang(%t/properties.h)

public struct FirstSmallStruct {
    public let x: UInt32
}

// CHECK: class SWIFT_SYMBOL({{.*}}) FirstSmallStruct final {
// CHECK: public:
// CHECK:   SWIFT_INLINE_PRIVATE_HELPER FirstSmallStruct(FirstSmallStruct &&)
// CHECK: }
// CHECK-NEXT:   SWIFT_INLINE_THUNK uint32_t getX() const SWIFT_SYMBOL({{.*}});
// CHECK-NEXT:   private:

public struct LargeStruct {
    public let x1, x2, x3, x4, x5, x6: Int

    public var anotherLargeStruct: LargeStruct {
        return LargeStruct(x1: 11, x2: 42, x3: -0xFFF, x4: 0xbad, x5: 5, x6: 0)
    }

    public var firstSmallStruct: FirstSmallStruct {
        return FirstSmallStruct(x: 65)
    }

    static public var staticX: Int {
        return -402
    }

    static public var staticSmallStruct: FirstSmallStruct {
        return FirstSmallStruct(x: 789)
    }
}

// CHECK: class SWIFT_SYMBOL({{.*}}) LargeStruct final {
// CHECK: public:
// CHECK: SWIFT_INLINE_PRIVATE_HELPER LargeStruct(LargeStruct &&)
// CHECK: }
// CHECK-NEXT: SWIFT_INLINE_THUNK swift::Int getX1() const SWIFT_SYMBOL({{.*}});
// CHECK-NEXT: SWIFT_INLINE_THUNK swift::Int getX2() const SWIFT_SYMBOL({{.*}});
// CHECK-NEXT: SWIFT_INLINE_THUNK swift::Int getX3() const SWIFT_SYMBOL({{.*}});
// CHECK-NEXT: SWIFT_INLINE_THUNK swift::Int getX4() const SWIFT_SYMBOL({{.*}});
// CHECK-NEXT: SWIFT_INLINE_THUNK swift::Int getX5() const SWIFT_SYMBOL({{.*}});
// CHECK-NEXT: SWIFT_INLINE_THUNK swift::Int getX6() const SWIFT_SYMBOL({{.*}});
// CHECK-NEXT: SWIFT_INLINE_THUNK LargeStruct getAnotherLargeStruct() const SWIFT_SYMBOL({{.*}});
// CHECK-NEXT: SWIFT_INLINE_THUNK FirstSmallStruct getFirstSmallStruct() const SWIFT_SYMBOL({{.*}});
// CHECK-NEXT: static SWIFT_INLINE_THUNK swift::Int getStaticX() SWIFT_SYMBOL({{.*}});
// CHECK-NEXT: static SWIFT_INLINE_THUNK FirstSmallStruct getStaticSmallStruct() SWIFT_SYMBOL({{.*}});
// CHECK-NEXT: private:

public final class PropertiesInClass {
    public let storedInt: Int32

    init(_ x: Int32) {
        storedInt = x
    }

    public var computedInt: Int {
        return Int(storedInt) - 1
    }

    public var smallStruct: FirstSmallStruct {
        return FirstSmallStruct(x: UInt32(-storedInt));
    }
}

// CHECK: class SWIFT_SYMBOL({{.*}}) PropertiesInClass final : public swift::_impl::RefCountedClass {
// CHECK: using RefCountedClass::operator=;
// CHECK-NEXT: SWIFT_INLINE_THUNK int32_t getStoredInt() SWIFT_SYMBOL({{.*}});
// CHECK-NEXT: SWIFT_INLINE_THUNK swift::Int getComputedInt() SWIFT_SYMBOL({{.*}});
// CHECK-NEXT: SWIFT_INLINE_THUNK FirstSmallStruct getSmallStruct() SWIFT_SYMBOL({{.*}});

public func createPropsInClass(_ x: Int32) -> PropertiesInClass {
    return PropertiesInClass(x)
}

public struct SmallStructWithGetters {
    public let storedInt: UInt32
    public var computedInt: Int {
        return Int(storedInt) + 2
    }

    public var largeStruct: LargeStruct {
        return LargeStruct(x1: computedInt * 2, x2: 1, x3: 2, x4: 3, x5: 4, x6: 5)
    }

    public var smallStruct: SmallStructWithGetters {
        return SmallStructWithGetters(storedInt: 0xFAE);
    }
}

// CHECK: class SWIFT_SYMBOL({{.*}}) SmallStructWithGetters final {
// CHECK: public:
// CHECK:   SWIFT_INLINE_PRIVATE_HELPER SmallStructWithGetters(SmallStructWithGetters &&)
// CHECK: }
// CHECK-NEXT:  SWIFT_INLINE_THUNK uint32_t getStoredInt() const SWIFT_SYMBOL({{.*}});
// CHECK-NEXT:  SWIFT_INLINE_THUNK swift::Int getComputedInt() const SWIFT_SYMBOL({{.*}});
// CHECK-NEXT:  SWIFT_INLINE_THUNK LargeStruct getLargeStruct() const SWIFT_SYMBOL({{.*}});
// CHECK-NEXT:  SWIFT_INLINE_THUNK SmallStructWithGetters getSmallStruct() const SWIFT_SYMBOL({{.*}});
// CHECK-NEXT: private:

public func createSmallStructWithGetter() -> SmallStructWithGetters {
    return SmallStructWithGetters(storedInt: 21)
}

private class RefCountedClass {
    let x: Int

    init(x: Int) {
        self.x = x
        print("create RefCountedClass \(x)")
    }
    deinit {
        print("destroy RefCountedClass \(x)")
    }
}

public struct StructWithRefCountStoredProp {
    private let storedRef: RefCountedClass

    internal init(x: Int) {
        storedRef = RefCountedClass(x: x)
    }

    public var another: StructWithRefCountStoredProp {
        return StructWithRefCountStoredProp(x: 1)
    }
}

public func createStructWithRefCountStoredProp() -> StructWithRefCountStoredProp {
    return StructWithRefCountStoredProp(x: 0)
}

// CHECK: SWIFT_INLINE_THUNK uint32_t FirstSmallStruct::getX() const {
// CHECK-NEXT: return _impl::$s10Properties16FirstSmallStructV1xs6UInt32Vvg(_impl::swift_interop_passDirect_Properties_uint32_t_0_4(_getOpaquePointer()));
// CHECK-NEXT: }

// CHECK:      SWIFT_INLINE_THUNK swift::Int LargeStruct::getX1() const {
// CHECK-NEXT: return _impl::$s10Properties11LargeStructV2x1Sivg(_getOpaquePointer());
// CHECK-NEXT: }
// CHECK-NEXT: SWIFT_INLINE_THUNK swift::Int LargeStruct::getX2() const {
// CHECK-NEXT: return _impl::$s10Properties11LargeStructV2x2Sivg(_getOpaquePointer());
// CHECK-NEXT: }
// CHECK-NEXT: SWIFT_INLINE_THUNK swift::Int LargeStruct::getX3() const {
// CHECK-NEXT: return _impl::$s10Properties11LargeStructV2x3Sivg(_getOpaquePointer());
// CHECK-NEXT: }
// CHECK-NEXT: SWIFT_INLINE_THUNK swift::Int LargeStruct::getX4() const {
// CHECK-NEXT: return _impl::$s10Properties11LargeStructV2x4Sivg(_getOpaquePointer());
// CHECK-NEXT: }
// CHECK-NEXT: SWIFT_INLINE_THUNK swift::Int LargeStruct::getX5() const {
// CHECK-NEXT: return _impl::$s10Properties11LargeStructV2x5Sivg(_getOpaquePointer());
// CHECK-NEXT: }
// CHECK-NEXT: SWIFT_INLINE_THUNK swift::Int LargeStruct::getX6() const {
// CHECK-NEXT: return _impl::$s10Properties11LargeStructV2x6Sivg(_getOpaquePointer());
// CHECK-NEXT: }
// CHECK-NEXT: SWIFT_INLINE_THUNK LargeStruct LargeStruct::getAnotherLargeStruct() const {
// CHECK-NEXT: return _impl::_impl_LargeStruct::returnNewValue([&](char * _Nonnull result) SWIFT_INLINE_THUNK_ATTRIBUTES {
// CHECK-NEXT:   _impl::$s10Properties11LargeStructV07anotherbC0ACvg(result, _getOpaquePointer());
// CHECK-NEXT: });
// CHECK-NEXT: }
// CHECK-NEXT: SWIFT_INLINE_THUNK FirstSmallStruct LargeStruct::getFirstSmallStruct() const {
// CHECK-NEXT: return _impl::_impl_FirstSmallStruct::returnNewValue([&](char * _Nonnull result) SWIFT_INLINE_THUNK_ATTRIBUTES {
// CHECK-NEXT:   _impl::swift_interop_returnDirect_Properties_uint32_t_0_4(result, _impl::$s10Properties11LargeStructV010firstSmallC0AA05FirsteC0Vvg(_getOpaquePointer()));
// CHECK-NEXT: });
// CHECK-NEXT: }
// CHECK-NEXT: SWIFT_INLINE_THUNK swift::Int LargeStruct::getStaticX() {
// CHECK-NEXT: return _impl::$s10Properties11LargeStructV7staticXSivgZ();
// CHECK-NEXT: }
// CHECK-NEXT: SWIFT_INLINE_THUNK FirstSmallStruct LargeStruct::getStaticSmallStruct() {
// CHECK-NEXT: return _impl::_impl_FirstSmallStruct::returnNewValue([&](char * _Nonnull result) SWIFT_INLINE_THUNK_ATTRIBUTES {
// CHECK-NEXT:   _impl::swift_interop_returnDirect_Properties_uint32_t_0_4(result, _impl::$s10Properties11LargeStructV011staticSmallC0AA05FirsteC0VvgZ());
// CHECK-NEXT: });
// CHECK-NEXT: }

// CHECK: SWIFT_INLINE_THUNK int32_t PropertiesInClass::getStoredInt() {
// CHECK-NEXT: return _impl::$s10Properties0A7InClassC9storedInts5Int32Vvg(::swift::_impl::_impl_RefCountedClass::getOpaquePointer(*this));
// CHECK-NEXT: }
// CHECK-NEXT: SWIFT_INLINE_THUNK swift::Int PropertiesInClass::getComputedInt() {
// CHECK-NEXT: return _impl::$s10Properties0A7InClassC11computedIntSivg(::swift::_impl::_impl_RefCountedClass::getOpaquePointer(*this));
// CHECK-NEXT: }
// CHECK-NEXT: SWIFT_INLINE_THUNK FirstSmallStruct PropertiesInClass::getSmallStruct() {
// CHECK-NEXT: return _impl::_impl_FirstSmallStruct::returnNewValue([&](char * _Nonnull result) SWIFT_INLINE_THUNK_ATTRIBUTES {
// CHECK-NEXT:   _impl::swift_interop_returnDirect_Properties_uint32_t_0_4(result, _impl::$s10Properties0A7InClassC11smallStructAA010FirstSmallE0Vvg(::swift::_impl::_impl_RefCountedClass::getOpaquePointer(*this)));
// CHECK-NEXT: });
// CHECK-NEXT: }

// CHECK:      SWIFT_INLINE_THUNK uint32_t SmallStructWithGetters::getStoredInt() const {
// CHECK-NEXT: return _impl::$s10Properties22SmallStructWithGettersV9storedInts6UInt32Vvg(_impl::swift_interop_passDirect_Properties_uint32_t_0_4(_getOpaquePointer()));
// CHECK-NEXT: }
// CHECK-NEXT: SWIFT_INLINE_THUNK swift::Int SmallStructWithGetters::getComputedInt() const {
// CHECK-NEXT: return _impl::$s10Properties22SmallStructWithGettersV11computedIntSivg(_impl::swift_interop_passDirect_Properties_uint32_t_0_4(_getOpaquePointer()));
// CHECK-NEXT: }
// CHECK-NEXT: SWIFT_INLINE_THUNK LargeStruct SmallStructWithGetters::getLargeStruct() const {
// CHECK-NEXT: return _impl::_impl_LargeStruct::returnNewValue([&](char * _Nonnull result) SWIFT_INLINE_THUNK_ATTRIBUTES {
// CHECK-NEXT:   _impl::$s10Properties22SmallStructWithGettersV05largeC0AA05LargeC0Vvg(result, _impl::swift_interop_passDirect_Properties_uint32_t_0_4(_getOpaquePointer()));
// CHECK-NEXT: });
// CHECK-NEXT: }
// CHECK-NEXT: SWIFT_INLINE_THUNK SmallStructWithGetters SmallStructWithGetters::getSmallStruct() const {
// CHECK-NEXT: return _impl::_impl_SmallStructWithGetters::returnNewValue([&](char * _Nonnull result) SWIFT_INLINE_THUNK_ATTRIBUTES {
// CHECK-NEXT:   _impl::swift_interop_returnDirect_Properties_uint32_t_0_4(result, _impl::$s10Properties22SmallStructWithGettersV05smallC0ACvg(_impl::swift_interop_passDirect_Properties_uint32_t_0_4(_getOpaquePointer())));
// CHECK-NEXT: });
// CHECK-NEXT: }
