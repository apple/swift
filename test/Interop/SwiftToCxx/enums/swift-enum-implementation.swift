// RUN: %empty-directory(%t)
// RUN: %target-swift-frontend %s -typecheck -module-name Enums -clang-header-expose-decls=all-public -emit-clang-header-path %t/enums.h
// RUN: %FileCheck %s < %t/enums.h

// RUN: %check-interop-cxx-header-in-clang(%t/enums.h -Wno-unused-private-field -Wno-unused-function)

// RUN: %target-swift-frontend %s -typecheck -module-name Enums -enable-experimental-cxx-interop -emit-clang-header-path %t/enums-default.h
// RUN: %FileCheck %s < %t/enums-default.h

public enum E {
    case x(Double)
    case y(UnsafeRawPointer?)
    case z(S)
    case w(i: Int)
    case auto(UnsafeMutableRawPointer)
    case foobar

    public init() {
        self = .foobar
    }

    public var ten: Int {
        return 10
    }

    public func printSelf() {
        print("self")
    }
}

public enum E2 {
    case foobar
    case baz
}

public struct S {
    public var x: Int64
    
    public init(x: Int64) {
        print("S.init()")
        self.x = x
    }
}

// CHECK:      class SWIFT_SYMBOL("s:5Enums1EO") E final {
// CHECK:        enum class cases {
// CHECK-NEXT:     x SWIFT_SYMBOL("s:5Enums1EO1xyACSdcACmF"),
// CHECK-NEXT:     y SWIFT_SYMBOL("s:5Enums1EO1yyACSVSgcACmF"),
// CHECK-NEXT:     z SWIFT_SYMBOL("s:5Enums1EO1zyAcA1SVcACmF"),
// CHECK-NEXT:     w SWIFT_SYMBOL("s:5Enums1EO1wyACSi_tcACmF"),
// CHECK-NEXT:     auto_ SWIFT_SYMBOL("s:5Enums1EO4autoyACSvcACmF"),
// CHECK-NEXT:     foobar SWIFT_SYMBOL("s:5Enums1EO6foobaryA2CmF")
// CHECK-NEXT:   };
// CHECK-EMPTY:
// CHECK-NEXT: #pragma clang diagnostic push
// CHECK-NEXT: #pragma clang diagnostic ignored "-Wc++17-extensions" // allow use of inline static data member
// CHECK-NEXT:   inline const static struct _impl_x {  // impl struct for case x
// CHECK-NEXT:     SWIFT_INLINE_THUNK constexpr operator cases() const {
// CHECK-NEXT:       return cases::x;
// CHECK-NEXT:     }
// CHECK-NEXT:     SWIFT_INLINE_THUNK E operator()(double val) const;
// CHECK-NEXT:   } x SWIFT_SYMBOL("s:5Enums1EO1xyACSdcACmF");
// CHECK-NEXT:   SWIFT_INLINE_THUNK bool isX() const;
// CHECK-NEXT:   SWIFT_INLINE_THUNK double getX() const;
// CHECK-EMPTY:
// CHECK-NEXT:   inline const static struct _impl_y {  // impl struct for case y
// CHECK-NEXT:     SWIFT_INLINE_THUNK constexpr operator cases() const {
// CHECK-NEXT:       return cases::y;
// CHECK-NEXT:     }
// CHECK-NEXT:     SWIFT_INLINE_THUNK E operator()(void const * _Nullable val) const;
// CHECK-NEXT:   } y SWIFT_SYMBOL("s:5Enums1EO1yyACSVSgcACmF");
// CHECK-NEXT:   SWIFT_INLINE_THUNK bool isY() const;
// CHECK-NEXT:   SWIFT_INLINE_THUNK void const * _Nullable getY() const;
// CHECK-EMPTY:
// CHECK-NEXT:   inline const static struct _impl_z {  // impl struct for case z
// CHECK-NEXT:     SWIFT_INLINE_THUNK constexpr operator cases() const {
// CHECK-NEXT:       return cases::z;
// CHECK-NEXT:     }
// CHECK-NEXT:     SWIFT_INLINE_THUNK E operator()(const S& val) const;
// CHECK-NEXT:   } z SWIFT_SYMBOL("s:5Enums1EO1zyAcA1SVcACmF");
// CHECK-NEXT:   SWIFT_INLINE_THUNK bool isZ() const;
// CHECK-NEXT:   SWIFT_INLINE_THUNK S getZ() const;
// CHECK-EMPTY:
// CHECK-NEXT:   inline const static struct _impl_w {  // impl struct for case w
// CHECK-NEXT:     SWIFT_INLINE_THUNK constexpr operator cases() const {
// CHECK-NEXT:       return cases::w;
// CHECK-NEXT:     }
// CHECK-NEXT:     SWIFT_INLINE_THUNK E operator()(swift::Int val) const;
// CHECK-NEXT:   } w SWIFT_SYMBOL("s:5Enums1EO1wyACSi_tcACmF");
// CHECK-NEXT:   SWIFT_INLINE_THUNK bool isW() const;
// CHECK-NEXT:   SWIFT_INLINE_THUNK swift::Int getW() const;
// CHECK-EMPTY:
// CHECK-NEXT:   inline const static struct _impl_auto {  // impl struct for case auto
// CHECK-NEXT:     SWIFT_INLINE_THUNK constexpr operator cases() const {
// CHECK-NEXT:       return cases::auto_;
// CHECK-NEXT:     }
// CHECK-NEXT:     SWIFT_INLINE_THUNK E operator()(void * _Nonnull val) const;
// CHECK-NEXT:   } auto_ SWIFT_SYMBOL("s:5Enums1EO4autoyACSvcACmF");
// CHECK-NEXT:   SWIFT_INLINE_THUNK bool isAuto_() const;
// CHECK-NEXT:   SWIFT_INLINE_THUNK void * _Nonnull getAuto_() const;
// CHECK-EMPTY:
// CHECK-NEXT:   inline const static struct _impl_foobar {  // impl struct for case foobar
// CHECK-NEXT:     SWIFT_INLINE_THUNK constexpr operator cases() const {
// CHECK-NEXT:       return cases::foobar;
// CHECK-NEXT:     }
// CHECK-NEXT:     SWIFT_INLINE_THUNK E operator()() const;
// CHECK-NEXT:   } foobar SWIFT_SYMBOL("s:5Enums1EO6foobaryA2CmF");
// CHECK-NEXT:   SWIFT_INLINE_THUNK bool isFoobar() const;
// CHECK-EMPTY:
// CHECK-NEXT: #pragma clang diagnostic pop
// CHECK-NEXT:   SWIFT_INLINE_THUNK operator cases() const {
// CHECK-NEXT:     switch (_getEnumTag()) {
// CHECK-NEXT:       case 0: return cases::x;
// CHECK-NEXT:       case 1: return cases::y;
// CHECK-NEXT:       case 2: return cases::z;
// CHECK-NEXT:       case 3: return cases::w;
// CHECK-NEXT:       case 4: return cases::auto_;
// CHECK-NEXT:       case 5: return cases::foobar;
// CHECK-NEXT:       default: abort();
// CHECK-NEXT:     }
// CHECK-NEXT:   }
// CHECK-EMPTY:
// CHECK-NEXT:   static SWIFT_INLINE_THUNK E init() SWIFT_SYMBOL("s:5Enums1EOACycfc");
// CHECK-NEXT:   SWIFT_INLINE_THUNK swift::Int getTen() const SWIFT_SYMBOL("s:5Enums1EO3tenSivp");
// CHECK-NEXT:   SWIFT_INLINE_THUNK void printSelf() const SWIFT_SYMBOL("s:5Enums1EO9printSelfyyF");
// CHECK-NEXT: private:
// CHECK:        SWIFT_INLINE_THUNK char * _Nonnull _destructiveProjectEnumData() noexcept {
// CHECK-NEXT:     auto metadata = _impl::$s5Enums1EOMa(0);
// CHECK-NEXT:     auto *vwTableAddr = reinterpret_cast<swift::_impl::ValueWitnessTable **>(metadata._0) - 1;
// CHECK-NEXT: #ifdef __arm64e__
// CHECK-NEXT:     auto *vwTable = reinterpret_cast<swift::_impl::ValueWitnessTable *>(ptrauth_auth_data(reinterpret_cast<void *>(*vwTableAddr), ptrauth_key_process_independent_data, ptrauth_blend_discriminator(vwTableAddr, 11839)));
// CHECK-NEXT: #else
// CHECK-NEXT:     auto *vwTable = *vwTableAddr;
// CHECK-NEXT: #endif
// CHECK-NEXT:     const auto *enumVWTable = reinterpret_cast<swift::_impl::EnumValueWitnessTable *>(vwTable);
// CHECK-NEXT:     enumVWTable->destructiveProjectEnumData(_getOpaquePointer(), metadata._0);
// CHECK-NEXT:     return _getOpaquePointer();
// CHECK-NEXT:   }
// CHECK-NEXT:   SWIFT_INLINE_THUNK void _destructiveInjectEnumTag(unsigned tag) noexcept {
// CHECK-NEXT:     auto metadata = _impl::$s5Enums1EOMa(0);
// CHECK-NEXT:     auto *vwTableAddr = reinterpret_cast<swift::_impl::ValueWitnessTable **>(metadata._0) - 1;
// CHECK-NEXT: #ifdef __arm64e__
// CHECK-NEXT:     auto *vwTable = reinterpret_cast<swift::_impl::ValueWitnessTable *>(ptrauth_auth_data(reinterpret_cast<void *>(*vwTableAddr), ptrauth_key_process_independent_data, ptrauth_blend_discriminator(vwTableAddr, 11839)));
// CHECK-NEXT: #else
// CHECK-NEXT:     auto *vwTable = *vwTableAddr;
// CHECK-NEXT: #endif
// CHECK-NEXT:     const auto *enumVWTable = reinterpret_cast<swift::_impl::EnumValueWitnessTable *>(vwTable);
// CHECK-NEXT:     enumVWTable->destructiveInjectEnumTag(_getOpaquePointer(), tag, metadata._0);
// CHECK-NEXT:   }
// CHECK-NEXT:   SWIFT_INLINE_THUNK unsigned _getEnumTag() const noexcept {
// CHECK-NEXT:     auto metadata = _impl::$s5Enums1EOMa(0);
// CHECK-NEXT:     auto *vwTableAddr = reinterpret_cast<swift::_impl::ValueWitnessTable **>(metadata._0) - 1;
// CHECK-NEXT: #ifdef __arm64e__
// CHECK-NEXT:     auto *vwTable = reinterpret_cast<swift::_impl::ValueWitnessTable *>(ptrauth_auth_data(reinterpret_cast<void *>(*vwTableAddr), ptrauth_key_process_independent_data, ptrauth_blend_discriminator(vwTableAddr, 11839)));
// CHECK-NEXT: #else
// CHECK-NEXT:     auto *vwTable = *vwTableAddr;
// CHECK-NEXT: #endif
// CHECK-NEXT:     const auto *enumVWTable = reinterpret_cast<swift::_impl::EnumValueWitnessTable *>(vwTable);
// CHECK-NEXT:     return enumVWTable->getEnumTag(_getOpaquePointer(), metadata._0);
// CHECK-NEXT:   }
// CHECK:      };
// CHECK-EMPTY:
// CHECK-NEXT: namespace _impl {
// CHECK-EMPTY:
// CHECK-NEXT: class _impl_E {
// CHECK-NEXT: public:
// CHECK:        static SWIFT_INLINE_THUNK void initializeWithTake(char * _Nonnull destStorage, char * _Nonnull srcStorage) {
// CHECK-NEXT:     auto metadata = _impl::$s5Enums1EOMa(0);
// CHECK-NEXT:     auto *vwTableAddr = reinterpret_cast<swift::_impl::ValueWitnessTable **>(metadata._0) - 1;
// CHECK-NEXT: #ifdef __arm64e__
// CHECK-NEXT:     auto *vwTable = reinterpret_cast<swift::_impl::ValueWitnessTable *>(ptrauth_auth_data(reinterpret_cast<void *>(*vwTableAddr), ptrauth_key_process_independent_data, ptrauth_blend_discriminator(vwTableAddr, 11839)));
// CHECK-NEXT: #else
// CHECK-NEXT:     auto *vwTable = *vwTableAddr;
// CHECK-NEXT: #endif
// CHECK-NEXT:     vwTable->initializeWithTake(destStorage, srcStorage, metadata._0);
// CHECK-NEXT:   }

// CHECK: class SWIFT_SYMBOL({{.*}}) E2 final {
// CHECK: SWIFT_INLINE_THUNK operator cases() const {
// CHECK: }
// CHECK-NEXT: }
// CHECK-EMPTY:
// CHECK-NEXT: SWIFT_INLINE_THUNK swift::Int getHashValue() const SWIFT_SYMBOL({{.*}});
// CHECK-NEXT: private:

// CHECK:      namespace Enums SWIFT_PRIVATE_ATTR SWIFT_SYMBOL_MODULE("Enums") {
// CHECK:        SWIFT_INLINE_THUNK E E::_impl_x::operator()(double val) const {
// CHECK-NEXT:     auto result = E::_make();
// CHECK-NEXT:     memcpy(result._getOpaquePointer(), &val, sizeof(val));
// CHECK-NEXT:     result._destructiveInjectEnumTag(0);
// CHECK-NEXT:     return result;
// CHECK-NEXT:   }
// CHECK-NEXT:   SWIFT_INLINE_THUNK bool E::isX() const {
// CHECK-NEXT:     return *this == E::x;
// CHECK-NEXT:   }
// CHECK-NEXT:   SWIFT_INLINE_THUNK double E::getX() const {
// CHECK-NEXT:     if (!isX()) abort();
// CHECK-NEXT:     alignas(E) unsigned char buffer[sizeof(E)];
// CHECK-NEXT:     auto *thisCopy = new(buffer) E(*this);
// CHECK-NEXT:     char * _Nonnull payloadFromDestruction = thisCopy->_destructiveProjectEnumData();
// CHECK-NEXT:     double result;
// CHECK-NEXT:     memcpy(&result, payloadFromDestruction, sizeof(result));
// CHECK-NEXT:     return result;
// CHECK-NEXT:   }
// CHECK-NEXT:   SWIFT_INLINE_THUNK E E::_impl_y::operator()(void const * _Nullable val) const {
// CHECK-NEXT:     auto result = E::_make();
// CHECK-NEXT:     memcpy(result._getOpaquePointer(), &val, sizeof(val));
// CHECK-NEXT:     result._destructiveInjectEnumTag(1);
// CHECK-NEXT:     return result;
// CHECK-NEXT:   }
// CHECK-NEXT:   SWIFT_INLINE_THUNK bool E::isY() const {
// CHECK-NEXT:     return *this == E::y;
// CHECK-NEXT:   }
// CHECK-NEXT:   SWIFT_INLINE_THUNK void const * _Nullable E::getY() const {
// CHECK-NEXT:     if (!isY()) abort();
// CHECK-NEXT:     alignas(E) unsigned char buffer[sizeof(E)];
// CHECK-NEXT:     auto *thisCopy = new(buffer) E(*this);
// CHECK-NEXT:     char * _Nonnull payloadFromDestruction = thisCopy->_destructiveProjectEnumData();
// CHECK-NEXT:     void const * _Nullable result;
// CHECK-NEXT:     memcpy(&result, payloadFromDestruction, sizeof(result));
// CHECK-NEXT:     return result;
// CHECK-NEXT:   }
// CHECK-NEXT:   SWIFT_INLINE_THUNK E E::_impl_z::operator()(const S& val) const {
// CHECK-NEXT:     auto result = E::_make();
// CHECK-NEXT:     alignas(S) unsigned char buffer[sizeof(S)];
// CHECK-NEXT:     auto *valCopy = new(buffer) S(val);
// CHECK-NEXT:     swift::_impl::implClassFor<S>::type::initializeWithTake(result._getOpaquePointer(), swift::_impl::implClassFor<S>::type::getOpaquePointer(*valCopy));
// CHECK-NEXT:     result._destructiveInjectEnumTag(2);
// CHECK-NEXT:     return result;
// CHECK-NEXT:   }
// CHECK-NEXT:   SWIFT_INLINE_THUNK bool E::isZ() const {
// CHECK-NEXT:     return *this == E::z;
// CHECK-NEXT:   }
// CHECK-NEXT:   SWIFT_INLINE_THUNK S E::getZ() const {
// CHECK-NEXT:     if (!isZ()) abort();
// CHECK-NEXT:     alignas(E) unsigned char buffer[sizeof(E)];
// CHECK-NEXT:     auto *thisCopy = new(buffer) E(*this);
// CHECK-NEXT:     char * _Nonnull payloadFromDestruction = thisCopy->_destructiveProjectEnumData();
// CHECK-NEXT:     return swift::_impl::implClassFor<S>::type::returnNewValue([&](char * _Nonnull result) SWIFT_INLINE_THUNK_ATTRIBUTES {
// CHECK-NEXT:       swift::_impl::implClassFor<S>::type::initializeWithTake(result, payloadFromDestruction);
// CHECK-NEXT:     });
// CHECK-NEXT:   }
// CHECK-NEXT:   SWIFT_INLINE_THUNK E E::_impl_w::operator()(swift::Int val) const {
// CHECK-NEXT:     auto result = E::_make();
// CHECK-NEXT:     memcpy(result._getOpaquePointer(), &val, sizeof(val));
// CHECK-NEXT:     result._destructiveInjectEnumTag(3);
// CHECK-NEXT:     return result;
// CHECK-NEXT:   }
// CHECK-NEXT:   SWIFT_INLINE_THUNK bool E::isW() const {
// CHECK-NEXT:     return *this == E::w;
// CHECK-NEXT:   }
// CHECK-NEXT:   SWIFT_INLINE_THUNK swift::Int E::getW() const {
// CHECK-NEXT:     if (!isW()) abort();
// CHECK-NEXT:     alignas(E) unsigned char buffer[sizeof(E)];
// CHECK-NEXT:     auto *thisCopy = new(buffer) E(*this);
// CHECK-NEXT:     char * _Nonnull payloadFromDestruction = thisCopy->_destructiveProjectEnumData();
// CHECK-NEXT:     swift::Int result;
// CHECK-NEXT:     memcpy(&result, payloadFromDestruction, sizeof(result));
// CHECK-NEXT:     return result;
// CHECK-NEXT:   }
// CHECK-NEXT:   SWIFT_INLINE_THUNK E E::_impl_auto::operator()(void * _Nonnull val) const {
// CHECK-NEXT:     auto result = E::_make();
// CHECK-NEXT:     memcpy(result._getOpaquePointer(), &val, sizeof(val));
// CHECK-NEXT:     result._destructiveInjectEnumTag(4);
// CHECK-NEXT:     return result;
// CHECK-NEXT:   }
// CHECK-NEXT:   SWIFT_INLINE_THUNK bool E::isAuto_() const {
// CHECK-NEXT:     return *this == E::auto_;
// CHECK-NEXT:   }
// CHECK-NEXT:   SWIFT_INLINE_THUNK void * _Nonnull E::getAuto_() const {
// CHECK-NEXT:     if (!isAuto_()) abort();
// CHECK-NEXT:     alignas(E) unsigned char buffer[sizeof(E)];
// CHECK-NEXT:     auto *thisCopy = new(buffer) E(*this);
// CHECK-NEXT:     char * _Nonnull payloadFromDestruction = thisCopy->_destructiveProjectEnumData();
// CHECK-NEXT:     void * _Nonnull result;
// CHECK-NEXT:     memcpy(&result, payloadFromDestruction, sizeof(result));
// CHECK-NEXT:     return result;
// CHECK-NEXT:   }
// CHECK-NEXT:   SWIFT_INLINE_THUNK E E::_impl_foobar::operator()() const {
// CHECK-NEXT:     auto result = E::_make();
// CHECK-NEXT:     result._destructiveInjectEnumTag(5);
// CHECK-NEXT:     return result;
// CHECK-NEXT:   }
// CHECK-NEXT:   SWIFT_INLINE_THUNK bool E::isFoobar() const {
// CHECK-NEXT:     return *this == E::foobar;
// CHECK-NEXT:   }
// CHECK-NEXT:   SWIFT_INLINE_THUNK E E::init() {
// CHECK-NEXT:     return _impl::_impl_E::returnNewValue([&](char * _Nonnull result) SWIFT_INLINE_THUNK_ATTRIBUTES {
// CHECK-NEXT:       _impl::swift_interop_returnDirect_Enums[[ENUMENCODING:[a-z0-9_]+]](result, _impl::$s5Enums1EOACycfC());
// CHECK-NEXT:     });
// CHECK-NEXT:   }
// CHECK-NEXT:   SWIFT_INLINE_THUNK swift::Int E::getTen() const {
// CHECK-NEXT:     return _impl::$s5Enums1EO3tenSivg(_impl::swift_interop_passDirect_Enums[[ENUMENCODING]](_getOpaquePointer()));
// CHECK-NEXT:   }
// CHECK-NEXT:   SWIFT_INLINE_THUNK void E::printSelf() const {
// CHECK-NEXT:     _impl::$s5Enums1EO9printSelfyyF(_impl::swift_interop_passDirect_Enums[[ENUMENCODING]](_getOpaquePointer()));
// CHECK-NEXT:   }
