// RUN: %empty-directory(%t)
// RUN: %target-swift-frontend %s -D RESILIENT_MODULE -module-name Class -emit-module -emit-module-path %t/Class.swiftmodule -enable-library-evolution -clang-header-expose-decls=all-public -emit-clang-header-path %t/class.h

// RUN: %target-swift-frontend %s -I %t -typecheck -module-name UseClass -clang-header-expose-decls=all-public -emit-clang-header-path %t/useclass.h

// RUN: %FileCheck %s < %t/useclass.h

// FIXME: add import automatically?
// RUN: echo '#include "class.h"' > %t/fixed-useclass.h
// RUN: cat %t/useclass.h     >> %t/fixed-useclass.h

// RUN: %check-interop-cxx-header-in-clang(%t/fixed-useclass.h)

// rdar://105396625
// UNSUPPORTED: CPU=arm64e

#if RESILIENT_MODULE

open class BaseClass {
  var field: Int64

  public init() {
    field = 0
  }
  open func virtualMethod() {
    print("BaseClass.virtualMethod")
  }
  open var computedProp: Int {
    return 11
  }
}

#else

import Class

public class CrossModuleDerivedClass: BaseClass {
  override public init() {}
  override public func virtualMethod() {
    print("CrossModuleDerivedClass.virtualMethod")
  }
  override public var computedProp: Int {
    return -56
  }

  public func virtualMethodInDerived() {
    print("CrossModuleDerivedClass.virtualMethodInDerived")
  }
  public var derivedComputedProp: Int {
    return 23
  }
  public func virtualMethod2InDerived() {
    print("CrossModuleDerivedClass.virtualMethod2InDerived")
  }
}

public class CrossModuleDerivedDerivedClass: CrossModuleDerivedClass {
  override public init() {}
  override public func virtualMethodInDerived() {
    print("CrossModuleDerivedDerivedClass.virtualMethodInDerived")
  }
  override public var derivedComputedProp: Int {
    return -95
  }
  override public final func virtualMethod2InDerived() {
    print("CrossModuleDerivedDerivedClass.virtualMethod2InDerived")
  }
}

public func createCrossModuleDerivedClass() -> CrossModuleDerivedClass {
    return CrossModuleDerivedClass()
}

public func createCrossModuleDerivedDerivedClass() -> CrossModuleDerivedDerivedClass {
    return CrossModuleDerivedDerivedClass()
}

#endif

// CHECK: SWIFT_EXTERN void $s8UseClass018CrossModuleDerivedB0C015virtualMethodInE0yyF(SWIFT_CONTEXT void * _Nonnull _self) SWIFT_NOEXCEPT SWIFT_CALL; // virtualMethodInDerived()
// CHECK-NEXT: SWIFT_EXTERN uint[[#NUM:]]_t $s8UseClass018CrossModuleDerivedB0CMo; // class metadata base offset
// CHECK-NEXT: SWIFT_EXTERN ptrdiff_t $s8UseClass018CrossModuleDerivedB0C19derivedComputedPropSivg(SWIFT_CONTEXT void * _Nonnull _self) SWIFT_NOEXCEPT SWIFT_CALL;
// CHECK-NEXT: SWIFT_EXTERN void $s8UseClass018CrossModuleDerivedB0C016virtualMethod2InE0yyF(SWIFT_CONTEXT void * _Nonnull _self) SWIFT_NOEXCEPT SWIFT_CALL; // virtualMethod2InDerived()


// CHECK:      void CrossModuleDerivedClass::virtualMethod() {
// CHECK-NEXT: _impl::$s5Class04BaseA0C13virtualMethodyyFTj(::swift::_impl::_impl_RefCountedClass::getOpaquePointer(*this));

// CHECK:      void CrossModuleDerivedClass::virtualMethodInDerived() {
// CHECK-NEXT: void ***selfPtr_ = reinterpret_cast<void ***>( ::swift::_impl::_impl_RefCountedClass::getOpaquePointer(*this));
// CHECK-NEXT: #ifdef __arm64e__
// CHECK-NEXT: void **vtable_ = ptrauth_auth_data(*selfPtr_, ptrauth_key_process_independent_data, ptrauth_blend_discriminator(selfPtr_,27361));
// CHECK-NEXT: #else
// CHECK-NEXT: void **vtable_ = *selfPtr_;
// CHECK-NEXT: #endif
// CHECK-NEXT: struct FTypeAddress {
// CHECK-NEXT: decltype(_impl::$s8UseClass018CrossModuleDerivedB0C015virtualMethodInE0yyF) * func;
// CHECK-NEXT: };
// CHECK-NEXT: FTypeAddress *fptrptr_ = reinterpret_cast<FTypeAddress *>(vtable_ + (_impl::$s8UseClass018CrossModuleDerivedB0CMo + 0) / sizeof(void *));
// CHECK-NEXT: (* fptrptr_->func)(::swift::_impl::_impl_RefCountedClass::getOpaquePointer(*this));
// CHECK-NEXT: }

// CHECK:      swift::Int CrossModuleDerivedClass::getDerivedComputedProp() {
// CHECK:      FTypeAddress *fptrptr_ = reinterpret_cast<FTypeAddress *>(vtable_ + (_impl::$s8UseClass018CrossModuleDerivedB0CMo + [[#VM1:]]) / sizeof(void *));

// CHECK:      void CrossModuleDerivedClass::virtualMethod2InDerived() {
// CHECK:      FTypeAddress *fptrptr_ = reinterpret_cast<FTypeAddress *>(vtable_ + (_impl::$s8UseClass018CrossModuleDerivedB0CMo + [[#VM2:]]) / sizeof(void *));

// CHECK:      void CrossModuleDerivedDerivedClass::virtualMethodInDerived() {
// CHECK:      FTypeAddress *fptrptr_ = reinterpret_cast<FTypeAddress *>(vtable_ + (_impl::$s8UseClass018CrossModuleDerivedB0CMo + 0) / sizeof(void *));

// CHECK:      swift::Int CrossModuleDerivedDerivedClass::getDerivedComputedProp() {
// CHECK:      FTypeAddress *fptrptr_ = reinterpret_cast<FTypeAddress *>(vtable_ + (_impl::$s8UseClass018CrossModuleDerivedB0CMo + [[#VM1]]) / sizeof(void *));

// CHECK:      void CrossModuleDerivedDerivedClass::virtualMethod2InDerived() {
// CHECK-NEXT: _impl::$s8UseClass018CrossModuleDerivedeB0C016virtualMethod2InE0yyF(::swift::_impl::_impl_RefCountedClass::getOpaquePointer(*this));
