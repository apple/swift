// RUN: %empty-directory(%t)
// RUN: %target-swift-frontend %s -typecheck -module-name Properties -clang-header-expose-decls=all-public -emit-clang-header-path %t/properties.h
// RUN: %FileCheck %s < %t/properties.h

// RUN: %check-interop-cxx-header-in-clang(%t/properties.h)

public struct FirstSmallStruct {
    public var x: UInt32
}

// CHECK: class SWIFT_SYMBOL({{.*}}) FirstSmallStruct final {
// CHECK: public:
// CHECK:   SWIFT_INLINE_PRIVATE_HELPER FirstSmallStruct(FirstSmallStruct &&)
// CHECK: }
// CHECK-NEXT:   SWIFT_INLINE_THUNK uint32_t getX() const SWIFT_SYMBOL({{.*}});
// CHECK-NEXT:   SWIFT_INLINE_THUNK void setX(uint32_t value) SWIFT_SYMBOL({{.*}});
// CHECK-NEXT:   private:

public struct LargeStruct {
    public var x1, x2, x3, x4, x5, x6: Int

    private static var _statX: Int = 0
    public static var staticX: Int {
        get {
            return _statX
        }
        set {
            _statX = newValue
        }
    }
}

// CHECK: class SWIFT_SYMBOL({{.*}}) LargeStruct final {
// CHECK: public:
// CHECK: SWIFT_INLINE_PRIVATE_HELPER LargeStruct(LargeStruct &&)
// CHECK: }
// CHECK-NEXT: SWIFT_INLINE_THUNK swift::Int getX1() const SWIFT_SYMBOL({{.*}});
// CHECK-NEXT: SWIFT_INLINE_THUNK void setX1(swift::Int value) SWIFT_SYMBOL({{.*}});
// CHECK-NEXT: SWIFT_INLINE_THUNK swift::Int getX2() const SWIFT_SYMBOL({{.*}});
// CHECK-NEXT: SWIFT_INLINE_THUNK void setX2(swift::Int value) SWIFT_SYMBOL({{.*}});
// CHECK-NEXT: SWIFT_INLINE_THUNK swift::Int getX3() const SWIFT_SYMBOL({{.*}});
// CHECK-NEXT: SWIFT_INLINE_THUNK void setX3(swift::Int value) SWIFT_SYMBOL({{.*}});
// CHECK-NEXT: SWIFT_INLINE_THUNK swift::Int getX4() const SWIFT_SYMBOL({{.*}});
// CHECK-NEXT: SWIFT_INLINE_THUNK void setX4(swift::Int value) SWIFT_SYMBOL({{.*}});
// CHECK-NEXT: SWIFT_INLINE_THUNK swift::Int getX5() const SWIFT_SYMBOL({{.*}});
// CHECK-NEXT: SWIFT_INLINE_THUNK void setX5(swift::Int value) SWIFT_SYMBOL({{.*}});
// CHECK-NEXT: SWIFT_INLINE_THUNK swift::Int getX6() const SWIFT_SYMBOL({{.*}});
// CHECK-NEXT: SWIFT_INLINE_THUNK void setX6(swift::Int value) SWIFT_SYMBOL({{.*}});
// CHECK-NEXT: static SWIFT_INLINE_THUNK swift::Int getStaticX() SWIFT_SYMBOL({{.*}});
// CHECK-NEXT: static SWIFT_INLINE_THUNK void setStaticX(swift::Int newValue) SWIFT_SYMBOL({{.*}});
// CHECK-NEXT: private:

public struct LargeStructWithProps {
    public var storedLargeStruct: LargeStruct
    public var storedSmallStruct: FirstSmallStruct
}

// CHECK:      class SWIFT_SYMBOL({{.*}}) LargeStructWithProps final {
// CHECK-NEXT: public:
// CHECK:      SWIFT_INLINE_THUNK LargeStruct getStoredLargeStruct() const SWIFT_SYMBOL({{.*}});
// CHECK-NEXT: SWIFT_INLINE_THUNK void setStoredLargeStruct(const LargeStruct& value) SWIFT_SYMBOL({{.*}});
// CHECK-NEXT: SWIFT_INLINE_THUNK FirstSmallStruct getStoredSmallStruct() const SWIFT_SYMBOL({{.*}});
// CHECK-NEXT: SWIFT_INLINE_THUNK void setStoredSmallStruct(const FirstSmallStruct& value) SWIFT_SYMBOL({{.*}});

public final class PropertiesInClass {
    public var storedInt: Int32

    init(_ x: Int32) {
        storedInt = x
    }

    public var computedInt: Int {
        get {
            return Int(storedInt) + 2
        } set {
            storedInt = Int32(newValue - 2)
        }
    }
}

// CHECK: class SWIFT_SYMBOL({{.*}}) PropertiesInClass final : public swift::_impl::RefCountedClass {
// CHECK: using RefCountedClass::operator=;
// CHECK-NEXT: SWIFT_INLINE_THUNK int32_t getStoredInt() SWIFT_SYMBOL({{.*}});
// CHECK-NEXT: SWIFT_INLINE_THUNK void setStoredInt(int32_t value) SWIFT_SYMBOL({{.*}});
// CHECK-NEXT: SWIFT_INLINE_THUNK swift::Int getComputedInt() SWIFT_SYMBOL({{.*}});
// CHECK-NEXT: SWIFT_INLINE_THUNK void setComputedInt(swift::Int newValue) SWIFT_SYMBOL({{.*}});

public func createPropsInClass(_ x: Int32) -> PropertiesInClass {
    return PropertiesInClass(x)
}

public struct SmallStructWithProps {
    public var storedInt: UInt32
    public var computedInt: Int {
        get {
            return Int(storedInt) + 2
        } set {
            storedInt = UInt32(newValue - 2)
        }
    }

    public var largeStructWithProps: LargeStructWithProps {
        get {
            return LargeStructWithProps(storedLargeStruct: LargeStruct(x1: computedInt * 2, x2: 1, x3: 2, x4: 3, x5: 4, x6: 5),
                                        storedSmallStruct:FirstSmallStruct(x: 0xFAE))
        } set {
            print("SET: \(newValue.storedLargeStruct), \(newValue.storedSmallStruct)")
        }
    }
}

// CHECK: class SWIFT_SYMBOL({{.*}}) SmallStructWithProps final {
// CHECK: public:
// CHECK:   SWIFT_INLINE_PRIVATE_HELPER SmallStructWithProps(SmallStructWithProps &&)
// CHECK: }
// CHECK-NEXT:    SWIFT_INLINE_THUNK uint32_t getStoredInt() const SWIFT_SYMBOL({{.*}});
// CHECK-NEXT:    SWIFT_INLINE_THUNK void setStoredInt(uint32_t value) SWIFT_SYMBOL({{.*}});
// CHECK-NEXT:    SWIFT_INLINE_THUNK swift::Int getComputedInt() const SWIFT_SYMBOL({{.*}});
// CHECK-NEXT:    SWIFT_INLINE_THUNK void setComputedInt(swift::Int newValue) SWIFT_SYMBOL({{.*}});
// CHECK-NEXT:    SWIFT_INLINE_THUNK LargeStructWithProps getLargeStructWithProps() const SWIFT_SYMBOL({{.*}});
// CHECK-NEXT:    SWIFT_INLINE_THUNK void setLargeStructWithProps(const LargeStructWithProps& newValue) SWIFT_SYMBOL({{.*}});
// CHECK-NEXT: private:

public func createSmallStructWithProps() -> SmallStructWithProps {
    return SmallStructWithProps(storedInt: 21)
}

public func createFirstSmallStruct(_ x: UInt32) -> FirstSmallStruct {
    return FirstSmallStruct(x: x)
}

// CHECK: SWIFT_INLINE_THUNK uint32_t FirstSmallStruct::getX() const {
// CHECK-NEXT: return _impl::$s10Properties16FirstSmallStructV1xs6UInt32Vvg(_impl::swift_interop_passDirect_Properties_uint32_t_0_4(_getOpaquePointer()));
// CHECK-NEXT: }
// CHECK-NEXT: SWIFT_INLINE_THUNK void FirstSmallStruct::setX(uint32_t value) {
// CHECK-NEXT: return _impl::$s10Properties16FirstSmallStructV1xs6UInt32Vvs(value, _getOpaquePointer());
// CHECK-NEXT: }

// CHECK:        SWIFT_INLINE_THUNK swift::Int LargeStruct::getX1() const {
// CHECK-NEXT:   return _impl::$s10Properties11LargeStructV2x1Sivg(_getOpaquePointer());
// CHECK-NEXT:   }
// CHECK-NEXT:   SWIFT_INLINE_THUNK void LargeStruct::setX1(swift::Int value) {
// CHECK-NEXT:   return _impl::$s10Properties11LargeStructV2x1Sivs(value, _getOpaquePointer());
// CHECK-NEXT:   }

// CHECK:      SWIFT_INLINE_THUNK swift::Int LargeStruct::getStaticX() {
// CHECK-NEXT: return _impl::$s10Properties11LargeStructV7staticXSivgZ();
// CHECK-NEXT: }
// CHECK-NEXT: SWIFT_INLINE_THUNK void LargeStruct::setStaticX(swift::Int newValue) {
// CHECK-NEXT: return _impl::$s10Properties11LargeStructV7staticXSivsZ(newValue);
// CHECK-NEXT: }

// CHECK:        SWIFT_INLINE_THUNK LargeStruct LargeStructWithProps::getStoredLargeStruct() const {
// CHECK-NEXT:    return _impl::_impl_LargeStruct::returnNewValue([&](char * _Nonnull result) SWIFT_INLINE_THUNK_ATTRIBUTES {
// CHECK-NEXT:     _impl::$s10Properties20LargeStructWithPropsV06storedbC0AA0bC0Vvg(result, _getOpaquePointer());
// CHECK-NEXT:   });
// CHECK-NEXT:   }
// CHECK-NEXT:   SWIFT_INLINE_THUNK void LargeStructWithProps::setStoredLargeStruct(const LargeStruct& value) {
// CHECK-NEXT:   return _impl::$s10Properties20LargeStructWithPropsV06storedbC0AA0bC0Vvs(_impl::_impl_LargeStruct::getOpaquePointer(value), _getOpaquePointer());
// CHECK-NEXT:   }
// CHECK-NEXT:   SWIFT_INLINE_THUNK FirstSmallStruct LargeStructWithProps::getStoredSmallStruct() const {
// CHECK-NEXT:   return _impl::_impl_FirstSmallStruct::returnNewValue([&](char * _Nonnull result) SWIFT_INLINE_THUNK_ATTRIBUTES {
// CHECK-NEXT:     _impl::swift_interop_returnDirect_Properties_uint32_t_0_4(result, _impl::$s10Properties20LargeStructWithPropsV011storedSmallC0AA05FirstgC0Vvg(_getOpaquePointer()));
// CHECK-NEXT:   });
// CHECK-NEXT:   }
// CHECK-NEXT:   SWIFT_INLINE_THUNK void LargeStructWithProps::setStoredSmallStruct(const FirstSmallStruct& value) {
// CHECK-NEXT:   return _impl::$s10Properties20LargeStructWithPropsV011storedSmallC0AA05FirstgC0Vvs(_impl::swift_interop_passDirect_Properties_uint32_t_0_4(_impl::_impl_FirstSmallStruct::getOpaquePointer(value)), _getOpaquePointer());
// CHECK-NEXT:   }

// CHECK: SWIFT_INLINE_THUNK int32_t PropertiesInClass::getStoredInt() {
// CHECK-NEXT: return _impl::$s10Properties0A7InClassC9storedInts5Int32Vvg(::swift::_impl::_impl_RefCountedClass::getOpaquePointer(*this));
// CHECK-NEXT: }
// CHECK-NEXT: SWIFT_INLINE_THUNK void PropertiesInClass::setStoredInt(int32_t value) {
// CHECK-NEXT: return _impl::$s10Properties0A7InClassC9storedInts5Int32Vvs(value, ::swift::_impl::_impl_RefCountedClass::getOpaquePointer(*this));
// CHECK-NEXT: }
// CHECK-NEXT: SWIFT_INLINE_THUNK swift::Int PropertiesInClass::getComputedInt() {
// CHECK-NEXT: return _impl::$s10Properties0A7InClassC11computedIntSivg(::swift::_impl::_impl_RefCountedClass::getOpaquePointer(*this));
// CHECK-NEXT: }
// CHECK-NEXT: SWIFT_INLINE_THUNK void PropertiesInClass::setComputedInt(swift::Int newValue) {
// CHECK-NEXT: return _impl::$s10Properties0A7InClassC11computedIntSivs(newValue, ::swift::_impl::_impl_RefCountedClass::getOpaquePointer(*this));
// CHECK-NEXT: }

// CHECK:        SWIFT_INLINE_THUNK uint32_t SmallStructWithProps::getStoredInt() const {
// CHECK-NEXT:  return _impl::$s10Properties20SmallStructWithPropsV9storedInts6UInt32Vvg(_impl::swift_interop_passDirect_Properties_uint32_t_0_4(_getOpaquePointer()));
// CHECK-NEXT:  }
// CHECK-NEXT:  SWIFT_INLINE_THUNK void SmallStructWithProps::setStoredInt(uint32_t value) {
// CHECK-NEXT:  return _impl::$s10Properties20SmallStructWithPropsV9storedInts6UInt32Vvs(value, _getOpaquePointer());
// CHECK-NEXT:  }
// CHECK-NEXT:  SWIFT_INLINE_THUNK swift::Int SmallStructWithProps::getComputedInt() const {
// CHECK-NEXT:  return _impl::$s10Properties20SmallStructWithPropsV11computedIntSivg(_impl::swift_interop_passDirect_Properties_uint32_t_0_4(_getOpaquePointer()));
// CHECK-NEXT:  }
// CHECK-NEXT:  SWIFT_INLINE_THUNK void SmallStructWithProps::setComputedInt(swift::Int newValue) {
// CHECK-NEXT:  return _impl::$s10Properties20SmallStructWithPropsV11computedIntSivs(newValue, _getOpaquePointer());
// CHECK-NEXT:  }
// CHECK-NEXT:  SWIFT_INLINE_THUNK LargeStructWithProps SmallStructWithProps::getLargeStructWithProps() const {
// CHECK-NEXT:  return _impl::_impl_LargeStructWithProps::returnNewValue([&](char * _Nonnull result) SWIFT_INLINE_THUNK_ATTRIBUTES {
// CHECK-NEXT:    _impl::$s10Properties20SmallStructWithPropsV05largecdE0AA05LargecdE0Vvg(result, _impl::swift_interop_passDirect_Properties_uint32_t_0_4(_getOpaquePointer()));
// CHECK-NEXT:  });
// CHECK-NEXT:  }
// CHECK-NEXT:  SWIFT_INLINE_THUNK void SmallStructWithProps::setLargeStructWithProps(const LargeStructWithProps& newValue) {
// CHECK-NEXT:  return _impl::$s10Properties20SmallStructWithPropsV05largecdE0AA05LargecdE0Vvs(_impl::_impl_LargeStructWithProps::getOpaquePointer(newValue), _getOpaquePointer());
// CHECK-NEXT:  }
