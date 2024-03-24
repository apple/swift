// RUN: %empty-directory(%t)
// RUN: %target-swift-frontend %S/swift-class-virtual-method-dispatch.swift -typecheck -module-name Class -clang-header-expose-decls=all-public -emit-clang-header-path %t/class.h -enable-library-evolution
// RUN: %FileCheck %s < %t/class.h

// RUN: %check-interop-cxx-header-in-clang(%t/class.h)

// note: implemented in swift-class-virtual-method-dispatch.swift

// CHECK: SWIFT_EXTERN void $s5Class04BaseA0C13virtualMethodyyF(SWIFT_CONTEXT void * _Nonnull _self) SWIFT_NOEXCEPT SWIFT_CALL; // virtualMethod()
// CHECK-NEXT: SWIFT_EXTERN void $s5Class04BaseA0C13virtualMethodyyFTj(SWIFT_CONTEXT void * _Nonnull _self) SWIFT_NOEXCEPT SWIFT_CALL; // dispatch thunk for virtualMethod()

// CHECK: SWIFT_EXTERN ptrdiff_t $s5Class04BaseA0C19virtualComputedPropSivg(SWIFT_CONTEXT void * _Nonnull _self) SWIFT_NOEXCEPT SWIFT_CALL;
// CHECK-NEXT: SWIFT_EXTERN ptrdiff_t $s5Class04BaseA0C19virtualComputedPropSivgTj(SWIFT_CONTEXT void * _Nonnull _self) SWIFT_NOEXCEPT SWIFT_CALL; // dispatch thunk for

// CHECK:      void BaseClass::virtualMethod() {
// CHECK-NEXT: _impl::$s5Class04BaseA0C13virtualMethodyyFTj(::swift::_impl::_impl_RefCountedClass::getOpaquePointer(*this));
// CHECK-NEXT:  }

// CHECK:      swift::Int BaseClass::virtualMethodIntInt(swift::Int x) {
// CHECK-NEXT: return _impl::$s5Class04BaseA0C016virtualMethodIntE0yS2iFTj(x, ::swift::_impl::_impl_RefCountedClass::getOpaquePointer(*this));
// CHECK-NEXT:   }

// CHECK:        swift::Int BaseClass::finalMethodInBase(swift::Int x) {
// CHECK-NEXT:   return _impl::$s5Class04BaseA0C013finalMethodInB0yS2iF(x, ::swift::_impl::_impl_RefCountedClass::getOpaquePointer(*this));
// CHECK-NEXT:   }

// CHECK:         swift::Int BaseClass::getVirtualComputedProp() {
// CHECK-NEXT:    _impl::$s5Class04BaseA0C19virtualComputedPropSivgTj(::swift::_impl::_impl_RefCountedClass::getOpaquePointer(*this));

// CHECK:         int64_t BaseClass::getVirtualComputedGetSet() {
// CHECK-NEXT:    _impl::$s5Class04BaseA0C21virtualComputedGetSets5Int64VvgTj(::swift::_impl::_impl_RefCountedClass::getOpaquePointer(*this));

// CHECK:         void BaseClass::setVirtualComputedGetSet(int64_t newValue) {
// CHECK-NEXT:    _impl::$s5Class04BaseA0C21virtualComputedGetSets5Int64VvsTj(newValue, ::swift::_impl::_impl_RefCountedClass::getOpaquePointer(*this));

// CHECK:         swift::Int BaseClass::getStoredProp() {
// CHECK-NEXT:    _impl::$s5Class04BaseA0C10storedPropSivgTj(::swift::_impl::_impl_RefCountedClass::getOpaquePointer(*this));

// CHECK:         void BaseClass::setStoredProp(swift::Int value) {
// CHECK-NEXT:    _impl::$s5Class04BaseA0C10storedPropSivsTj(value, ::swift::_impl::_impl_RefCountedClass::getOpaquePointer(*this));

// CHECK:           swift::Int BaseClass::operator [](swift::Int i) const
// CHECK-NEXT:      return _impl::$s5Class04BaseA0CyS2icigTj(i, ::swift::_impl::_impl_RefCountedClass::getOpaquePointer(*this));

// CHECK:        void DerivedClass::virtualMethod() {
// CHECK-NEXT:   _impl::$s5Class04BaseA0C13virtualMethodyyFTj(::swift::_impl::_impl_RefCountedClass::getOpaquePointer(*this));
// CHECK-NEXT:   }

// CHECK:        swift::Int DerivedClass::virtualMethodIntInt(swift::Int x) {
// CHECK-NEXT:   return _impl::$s5Class04BaseA0C016virtualMethodIntE0yS2iFTj(x, ::swift::_impl::_impl_RefCountedClass::getOpaquePointer(*this));
// CHECK-NEXT:   }

// CHECK:        BaseClass DerivedClass::virtualMethodInDerived(const BaseClass& x) {
// CHECK-NEXT:   return _impl::_impl_BaseClass::makeRetained(_impl::$s5Class07DerivedA0C015virtualMethodInB0yAA04BaseA0CAFFTj(::swift::_impl::_impl_RefCountedClass::getOpaquePointer(x), ::swift::_impl::_impl_RefCountedClass::getOpaquePointer(*this)));
// CHECK-NEXT:   }

// CHECK:         swift::Int DerivedClass::getVirtualComputedProp() {
// CHECK-NEXT:   _impl::$s5Class04BaseA0C19virtualComputedPropSivgTj(::swift::_impl::_impl_RefCountedClass::getOpaquePointer(*this));


// CHECK:        void DerivedDerivedClass::virtualMethod() {
// CHECK-NEXT:     _impl::$s5Class07DerivedbA0C13virtualMethodyyF(::swift::_impl::_impl_RefCountedClass::getOpaquePointer(*this));
// CHECK-NEXT:     }

// CHECK:        BaseClass DerivedDerivedClass::virtualMethodInDerived(const BaseClass& x) {
// CHECK-NEXT:     return _impl::_impl_BaseClass::makeRetained(_impl::$s5Class07DerivedbA0C015virtualMethodInB0yAA04BaseA0CAFF(::swift::_impl::_impl_RefCountedClass::getOpaquePointer(x), ::swift::_impl::_impl_RefCountedClass::getOpaquePointer(*this)));
// CHECK-NEXT:     }

// CHECK:        void DerivedDerivedClass::methodInDerivedDerived() {
// CHECK-NEXT:     _impl::$s5Class07DerivedbA0C08methodInbB0yyF(::swift::_impl::_impl_RefCountedClass::getOpaquePointer(*this));
// CHECK-NEXT:     }

// CHECK:         swift::Int DerivedDerivedClass::getStoredProp() {
// CHECK-NEXT:     return _impl::$s5Class07DerivedbA0C10storedPropSivg(::swift::_impl::_impl_RefCountedClass::getOpaquePointer(*this));

// CHECK:          swift::Int DerivedDerivedClass::getComputedPropInDerivedDerived() {
// CHECK-NEXT:     return _impl::$s5Class07DerivedbA0C014computedPropInbB0Sivg(::swift::_impl::_impl_RefCountedClass::getOpaquePointer(*this));
