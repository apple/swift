// RUN: %empty-directory(%t)
// RUN: %target-swift-frontend %s -typecheck -module-name Class -clang-header-expose-decls=all-public -emit-clang-header-path %t/class.h
// RUN: %FileCheck %s < %t/class.h

// RUN: %check-interop-cxx-header-in-clang(%t/class.h)

// UNSUPPORTED: CPU=arm64e

public class BaseClass {
  var field: Int64

  init() {
    field = 0
  }
  public func virtualMethod() {
    print("BaseClass.virtualMethod")
  }
  public func virtualMethodIntInt(_ x: Int) -> Int {
    return x
  }
  public final func finalMethodInBase(_ x: Int) -> Int {
    return x * 2
  }

  public var virtualComputedProp: Int {
    return 21
  }
  public var virtualComputedGetSet: Int64 {
      get {
          return field
      }
      set {
          field = newValue
      }
  }
  public var storedProp: Int = 0

  public subscript(_ i: Int) -> Int {
    return i
  }
}

public class DerivedClass: BaseClass {
  override init() {
    super.init()
  }
  override public func virtualMethod() {
    print("DerivedClass.virtualMethod")
  }
  override public func virtualMethodIntInt(_ x: Int) -> Int {
    return -x
  }
  public func virtualMethodInDerived(_ x: BaseClass) -> BaseClass {
    return x
  }

  override public var virtualComputedProp: Int {
    return -75
  }
  override public var virtualComputedGetSet: Int64 {
    get {
      super.virtualComputedGetSet
    }
    set {
      super.virtualComputedGetSet = newValue * 2
    }
  }
  override public var storedProp: Int {
    get {
      return -super.storedProp
    }
    set {
      super.storedProp = newValue
    }
  }

  override public subscript(_ i: Int) -> Int {
    return i * 2
  }
}

public final class DerivedDerivedClass: DerivedClass {
  override init() {
    super.init()
  }
  override public func virtualMethod() {
    print("DerivedDerivedClass.virtualMethod")
  }
  override public func virtualMethodInDerived(_ x: BaseClass) -> BaseClass {
    return self
  }
  public func methodInDerivedDerived() {
    print("DerivedDerivedClass.methodInDerivedDerived")
  }
  override public var storedProp: Int {
    get {
      return -super.storedProp
    }
    set {
      super.storedProp = newValue + 1
    }
  }
  public var storedPropInDerivedDerived: Int = 0
  public var computedPropInDerivedDerived: Int { return 11 }
}

public func returnBaseClass() -> BaseClass {
  return BaseClass()
}

public func returnDerivedClass() -> DerivedClass {
  return DerivedClass()
}

public func returnDerivedDerivedClass() -> DerivedDerivedClass {
  return DerivedDerivedClass()
}

// CHECK:      void BaseClass::virtualMethod() {
// CHECK-NEXT: void ***selfPtr_ = reinterpret_cast<void ***>( ::swift::_impl::_impl_RefCountedClass::getOpaquePointer(*this));
// CHECK-NEXT: #ifdef __arm64e__
// CHECK-NEXT: void **vtable_ = ptrauth_auth_data(*selfPtr_, ptrauth_key_process_independent_data, ptrauth_blend_discriminator(selfPtr_,27361));
// CHECK-NEXT: #else
// CHECK-NEXT: void **vtable_ = *selfPtr_;
// CHECK-NEXT: #endif
// CHECK-NEXT: struct FTypeAddress {
// CHECK-NEXT: decltype(_impl::$s5Class04BaseA0C13virtualMethodyyF) * func;
// CHECK-NEXT: };
// CHECK-NEXT: FTypeAddress *fptrptr_ = reinterpret_cast<FTypeAddress *>(vtable_ + [[#VM1:]] / sizeof(void *));
// CHECK-NEXT: (* fptrptr_->func)(::swift::_impl::_impl_RefCountedClass::getOpaquePointer(*this));
// CHECK-NEXT: }

// CHECK:      swift::Int BaseClass::virtualMethodIntInt(swift::Int x) {
// CHECK-NEXT: void ***selfPtr_ = reinterpret_cast<void ***>( ::swift::_impl::_impl_RefCountedClass::getOpaquePointer(*this));
// CHECK-NEXT: #ifdef __arm64e__
// CHECK-NEXT: void **vtable_ = ptrauth_auth_data(*selfPtr_, ptrauth_key_process_independent_data, ptrauth_blend_discriminator(selfPtr_,27361));
// CHECK-NEXT: #else
// CHECK-NEXT: void **vtable_ = *selfPtr_;
// CHECK-NEXT: #endif
// CHECK-NEXT: struct FTypeAddress {
// CHECK-NEXT: decltype(_impl::$s5Class04BaseA0C016virtualMethodIntE0yS2iF) * func;
// CHECK-NEXT: };
// CHECK-NEXT: FTypeAddress *fptrptr_ = reinterpret_cast<FTypeAddress *>(vtable_ + [[#VM2:]] / sizeof(void *));
// CHECK-NEXT:   return (* fptrptr_->func)(x, ::swift::_impl::_impl_RefCountedClass::getOpaquePointer(*this));
// CHECK-NEXT:   }

// CHECK:        swift::Int BaseClass::finalMethodInBase(swift::Int x) {
// CHECK-NEXT:   return _impl::$s5Class04BaseA0C013finalMethodInB0yS2iF(x, ::swift::_impl::_impl_RefCountedClass::getOpaquePointer(*this));
// CHECK-NEXT:   }

// CHECK:        swift::Int BaseClass::getVirtualComputedProp() {
// CHECK-NEXT:   void ***selfPtr_ = reinterpret_cast<void ***>( ::swift::_impl::_impl_RefCountedClass::getOpaquePointer(*this));
// CHECK-NEXT:   #ifdef __arm64e__
// CHECK-NEXT:   void **vtable_ = ptrauth_auth_data(*selfPtr_, ptrauth_key_process_independent_data, ptrauth_blend_discriminator(selfPtr_,27361));
// CHECK-NEXT:   #else
// CHECK-NEXT:   void **vtable_ = *selfPtr_;
// CHECK-NEXT:   #endif
// CHECK-NEXT:   struct FTypeAddress {
// CHECK-NEXT:   decltype(_impl::$s5Class04BaseA0C19virtualComputedPropSivg) * func;
// CHECK-NEXT:   };
// CHECK-NEXT:   FTypeAddress *fptrptr_ = reinterpret_cast<FTypeAddress *>(vtable_ + [[#VM3:]] / sizeof(void *));
// CHECK-NEXT:     return (* fptrptr_->func)(::swift::_impl::_impl_RefCountedClass::getOpaquePointer(*this));
// CHECK-NEXT:   }

// CHECK:        int64_t BaseClass::getVirtualComputedGetSet() {
// CHECK-NEXT:   void ***selfPtr_ = reinterpret_cast<void ***>( ::swift::_impl::_impl_RefCountedClass::getOpaquePointer(*this));
// CHECK-NEXT:   #ifdef __arm64e__
// CHECK-NEXT:   void **vtable_ = ptrauth_auth_data(*selfPtr_, ptrauth_key_process_independent_data, ptrauth_blend_discriminator(selfPtr_,27361));
// CHECK-NEXT:   #else
// CHECK-NEXT:   void **vtable_ = *selfPtr_;
// CHECK-NEXT:   #endif
// CHECK-NEXT:   struct FTypeAddress {
// CHECK-NEXT:   decltype(_impl::$s5Class04BaseA0C21virtualComputedGetSets5Int64Vvg) * func;
// CHECK-NEXT:   };
// CHECK-NEXT:   FTypeAddress *fptrptr_ = reinterpret_cast<FTypeAddress *>(vtable_ + [[#VM4:]] / sizeof(void *));
// CHECK-NEXT:     return (* fptrptr_->func)(::swift::_impl::_impl_RefCountedClass::getOpaquePointer(*this));
// CHECK-NEXT:   }

// CHECK:        void BaseClass::setVirtualComputedGetSet(int64_t newValue) {
// CHECK-NEXT:   void ***selfPtr_ = reinterpret_cast<void ***>( ::swift::_impl::_impl_RefCountedClass::getOpaquePointer(*this));
// CHECK-NEXT:   #ifdef __arm64e__
// CHECK-NEXT:   void **vtable_ = ptrauth_auth_data(*selfPtr_, ptrauth_key_process_independent_data, ptrauth_blend_discriminator(selfPtr_,27361));
// CHECK-NEXT:   #else
// CHECK-NEXT:   void **vtable_ = *selfPtr_;
// CHECK-NEXT:   #endif
// CHECK-NEXT:   struct FTypeAddress {
// CHECK-NEXT:   decltype(_impl::$s5Class04BaseA0C21virtualComputedGetSets5Int64Vvs) * func;
// CHECK-NEXT:   };
// CHECK-NEXT:   FTypeAddress *fptrptr_ = reinterpret_cast<FTypeAddress *>(vtable_ + [[#VM5:]] / sizeof(void *));
// CHECK-NEXT:   (* fptrptr_->func)(newValue, ::swift::_impl::_impl_RefCountedClass::getOpaquePointer(*this));
// CHECK-NEXT:   }

// CHECK:        swift::Int BaseClass::getStoredProp() {
// CHECK-NEXT:   void ***selfPtr_ = reinterpret_cast<void ***>( ::swift::_impl::_impl_RefCountedClass::getOpaquePointer(*this));
// CHECK-NEXT:   #ifdef __arm64e__
// CHECK-NEXT:   void **vtable_ = ptrauth_auth_data(*selfPtr_, ptrauth_key_process_independent_data, ptrauth_blend_discriminator(selfPtr_,27361));
// CHECK-NEXT:   #else
// CHECK-NEXT:   void **vtable_ = *selfPtr_;
// CHECK-NEXT:   #endif
// CHECK-NEXT:   struct FTypeAddress {
// CHECK-NEXT:   decltype(_impl::$s5Class04BaseA0C10storedPropSivg) * func;
// CHECK-NEXT:   };
// CHECK-NEXT:   FTypeAddress *fptrptr_ = reinterpret_cast<FTypeAddress *>(vtable_ + [[#VM7:]] / sizeof(void *));
// CHECK-NEXT:   return (* fptrptr_->func)(::swift::_impl::_impl_RefCountedClass::getOpaquePointer(*this));
// CHECK-NEXT:   }

// CHECK:        void BaseClass::setStoredProp(swift::Int value) {
// CHECK-NEXT:   void ***selfPtr_ = reinterpret_cast<void ***>( ::swift::_impl::_impl_RefCountedClass::getOpaquePointer(*this));
// CHECK-NEXT:   #ifdef __arm64e__
// CHECK-NEXT:   void **vtable_ = ptrauth_auth_data(*selfPtr_, ptrauth_key_process_independent_data, ptrauth_blend_discriminator(selfPtr_,27361));
// CHECK-NEXT:   #else
// CHECK-NEXT:   void **vtable_ = *selfPtr_;
// CHECK-NEXT:   #endif
// CHECK-NEXT:   struct FTypeAddress {
// CHECK-NEXT:   decltype(_impl::$s5Class04BaseA0C10storedPropSivs) * func;
// CHECK-NEXT:   };
// CHECK-NEXT:   FTypeAddress *fptrptr_ = reinterpret_cast<FTypeAddress *>(vtable_ + [[#VM8:]] / sizeof(void *));
// CHECK-NEXT:     (* fptrptr_->func)(value, ::swift::_impl::_impl_RefCountedClass::getOpaquePointer(*this));
// CHECK-NEXT:   }

// CHECK:         swift::Int BaseClass::operator [](swift::Int i) const
// CHECK-NEXT:    void ***selfPtr_ = reinterpret_cast<void ***>( ::swift::_impl::_impl_RefCountedClass::getOpaquePointer(*this));
// CHECK-NEXT:    #ifdef __arm64e__
// CHECK-NEXT:    void **vtable_ = ptrauth_auth_data(*selfPtr_, ptrauth_key_process_independent_data, ptrauth_blend_discriminator(selfPtr_,27361));
// CHECK-NEXT:    #else
// CHECK-NEXT:    void **vtable_ = *selfPtr_;
// CHECK-NEXT:    #endif
// CHECK-NEXT:    struct FTypeAddress {
// CHECK-NEXT:    decltype(_impl::$s5Class04BaseA0CyS2icig) * func;
// CHECK-NEXT:    };
// CHECK-NEXT:    FTypeAddress *fptrptr_ = reinterpret_cast<FTypeAddress *>(vtable_ + [[#VM10:]] / sizeof(void *));
// CHECK-NEXT:    return (* fptrptr_->func)(i, ::swift::_impl::_impl_RefCountedClass::getOpaquePointer(*this));
// CHECK-NEXT:    }

// CHECK:        void DerivedClass::virtualMethod() {
// CHECK-NEXT:   void ***selfPtr_ = reinterpret_cast<void ***>( ::swift::_impl::_impl_RefCountedClass::getOpaquePointer(*this));
// CHECK-NEXT:   #ifdef __arm64e__
// CHECK-NEXT:   void **vtable_ = ptrauth_auth_data(*selfPtr_, ptrauth_key_process_independent_data, ptrauth_blend_discriminator(selfPtr_,27361));
// CHECK-NEXT:   #else
// CHECK-NEXT:   void **vtable_ = *selfPtr_;
// CHECK-NEXT:   #endif
// CHECK-NEXT:   struct FTypeAddress {
// CHECK-NEXT:   decltype(_impl::$s5Class07DerivedA0C13virtualMethodyyF) * func;
// CHECK-NEXT:   };
// CHECK-NEXT:   FTypeAddress *fptrptr_ = reinterpret_cast<FTypeAddress *>(vtable_ + [[#VM1]] / sizeof(void *));
// CHECK-NEXT:   (* fptrptr_->func)(::swift::_impl::_impl_RefCountedClass::getOpaquePointer(*this));
// CHECK-NEXT:   }

// CHECK:        swift::Int DerivedClass::virtualMethodIntInt(swift::Int x) {
// CHECK-NEXT:     void ***selfPtr_ = reinterpret_cast<void ***>( ::swift::_impl::_impl_RefCountedClass::getOpaquePointer(*this));
// CHECK-NEXT:     #ifdef __arm64e__
// CHECK-NEXT:     void **vtable_ = ptrauth_auth_data(*selfPtr_, ptrauth_key_process_independent_data, ptrauth_blend_discriminator(selfPtr_,27361));
// CHECK-NEXT:     #else
// CHECK-NEXT:     void **vtable_ = *selfPtr_;
// CHECK-NEXT:     #endif
// CHECK-NEXT:     struct FTypeAddress {
// CHECK-NEXT:     decltype(_impl::$s5Class07DerivedA0C016virtualMethodIntE0yS2iF) * func;
// CHECK-NEXT:     };
// CHECK-NEXT:     FTypeAddress *fptrptr_ = reinterpret_cast<FTypeAddress *>(vtable_ + [[#VM2]] / sizeof(void *));
// CHECK-NEXT:     return (* fptrptr_->func)(x, ::swift::_impl::_impl_RefCountedClass::getOpaquePointer(*this));
// CHECK-NEXT:     }

// CHECK:        BaseClass DerivedClass::virtualMethodInDerived(const BaseClass& x) {
// CHECK-NEXT:   void ***selfPtr_ = reinterpret_cast<void ***>( ::swift::_impl::_impl_RefCountedClass::getOpaquePointer(*this));
// CHECK-NEXT:   #ifdef __arm64e__
// CHECK-NEXT:   void **vtable_ = ptrauth_auth_data(*selfPtr_, ptrauth_key_process_independent_data, ptrauth_blend_discriminator(selfPtr_,27361));
// CHECK-NEXT:   #else
// CHECK-NEXT:   void **vtable_ = *selfPtr_;
// CHECK-NEXT:   #endif
// CHECK-NEXT:   struct FTypeAddress {
// CHECK-NEXT:   decltype(_impl::$s5Class07DerivedA0C015virtualMethodInB0yAA04BaseA0CAFF) * func;
// CHECK-NEXT:   };
// CHECK-NEXT:   FTypeAddress *fptrptr_ = reinterpret_cast<FTypeAddress *>(vtable_ + [[#VM11:]] / sizeof(void *));
// CHECK-NEXT:   return _impl::_impl_BaseClass::makeRetained((* fptrptr_->func)(::swift::_impl::_impl_RefCountedClass::getOpaquePointer(x), ::swift::_impl::_impl_RefCountedClass::getOpaquePointer(*this)));
// CHECK-NEXT:   }

// CHECK:         swift::Int DerivedClass::getVirtualComputedProp() {
// CHECK-NEXT:     void ***selfPtr_ = reinterpret_cast<void ***>( ::swift::_impl::_impl_RefCountedClass::getOpaquePointer(*this));
// CHECK-NEXT:     #ifdef __arm64e__
// CHECK-NEXT:     void **vtable_ = ptrauth_auth_data(*selfPtr_, ptrauth_key_process_independent_data, ptrauth_blend_discriminator(selfPtr_,27361));
// CHECK-NEXT:     #else
// CHECK-NEXT:     void **vtable_ = *selfPtr_;
// CHECK-NEXT:     #endif
// CHECK-NEXT:     struct FTypeAddress {
// CHECK-NEXT:     decltype(_impl::$s5Class07DerivedA0C19virtualComputedPropSivg) * func;
// CHECK-NEXT:     };
// CHECK-NEXT:     FTypeAddress *fptrptr_ = reinterpret_cast<FTypeAddress *>(vtable_ + [[#VM3]] / sizeof(void *));
// CHECK-NEXT:       return (* fptrptr_->func)(::swift::_impl::_impl_RefCountedClass::getOpaquePointer(*this));
// CHECK-NEXT:       }

// CHECK:         int64_t DerivedClass::getVirtualComputedGetSet() {
// CHECK-NEXT:     void ***selfPtr_ = reinterpret_cast<void ***>( ::swift::_impl::_impl_RefCountedClass::getOpaquePointer(*this));
// CHECK-NEXT:     #ifdef __arm64e__
// CHECK-NEXT:     void **vtable_ = ptrauth_auth_data(*selfPtr_, ptrauth_key_process_independent_data, ptrauth_blend_discriminator(selfPtr_,27361));
// CHECK-NEXT:     #else
// CHECK-NEXT:     void **vtable_ = *selfPtr_;
// CHECK-NEXT:     #endif
// CHECK-NEXT:     struct FTypeAddress {
// CHECK-NEXT:     decltype(_impl::$s5Class07DerivedA0C21virtualComputedGetSets5Int64Vvg) * func;
// CHECK-NEXT:     };
// CHECK-NEXT:     FTypeAddress *fptrptr_ = reinterpret_cast<FTypeAddress *>(vtable_ + [[#VM4]] / sizeof(void *));
// CHECK-NEXT:     return (* fptrptr_->func)(::swift::_impl::_impl_RefCountedClass::getOpaquePointer(*this));
// CHECK-NEXT:     }

// CHECK:         void DerivedClass::setVirtualComputedGetSet(int64_t newValue) {
// CHECK-NEXT:     void ***selfPtr_ = reinterpret_cast<void ***>( ::swift::_impl::_impl_RefCountedClass::getOpaquePointer(*this));
// CHECK-NEXT:     #ifdef __arm64e__
// CHECK-NEXT:     void **vtable_ = ptrauth_auth_data(*selfPtr_, ptrauth_key_process_independent_data, ptrauth_blend_discriminator(selfPtr_,27361));
// CHECK-NEXT:     #else
// CHECK-NEXT:     void **vtable_ = *selfPtr_;
// CHECK-NEXT:     #endif
// CHECK-NEXT:     struct FTypeAddress {
// CHECK-NEXT:     decltype(_impl::$s5Class07DerivedA0C21virtualComputedGetSets5Int64Vvs) * func;
// CHECK-NEXT:     };
// CHECK-NEXT:     FTypeAddress *fptrptr_ = reinterpret_cast<FTypeAddress *>(vtable_ + [[#VM5]] / sizeof(void *));
// CHECK-NEXT:     (* fptrptr_->func)(newValue, ::swift::_impl::_impl_RefCountedClass::getOpaquePointer(*this));
// CHECK-NEXT:     }

// CHECK:         swift::Int DerivedClass::getStoredProp() {
// CHECK-NEXT:     void ***selfPtr_ = reinterpret_cast<void ***>( ::swift::_impl::_impl_RefCountedClass::getOpaquePointer(*this));
// CHECK-NEXT:     #ifdef __arm64e__
// CHECK-NEXT:     void **vtable_ = ptrauth_auth_data(*selfPtr_, ptrauth_key_process_independent_data, ptrauth_blend_discriminator(selfPtr_,27361));
// CHECK-NEXT:     #else
// CHECK-NEXT:     void **vtable_ = *selfPtr_;
// CHECK-NEXT:     #endif
// CHECK-NEXT:     struct FTypeAddress {
// CHECK-NEXT:     decltype(_impl::$s5Class07DerivedA0C10storedPropSivg) * func;
// CHECK-NEXT:     };
// CHECK-NEXT:     FTypeAddress *fptrptr_ = reinterpret_cast<FTypeAddress *>(vtable_ + [[#VM7]] / sizeof(void *));
// CHECK-NEXT:       return (* fptrptr_->func)(::swift::_impl::_impl_RefCountedClass::getOpaquePointer(*this));
// CHECK-NEXT:       }

// CHECK:         void DerivedClass::setStoredProp(swift::Int newValue) {
// CHECK-NEXT:     void ***selfPtr_ = reinterpret_cast<void ***>( ::swift::_impl::_impl_RefCountedClass::getOpaquePointer(*this));
// CHECK-NEXT:     #ifdef __arm64e__
// CHECK-NEXT:     void **vtable_ = ptrauth_auth_data(*selfPtr_, ptrauth_key_process_independent_data, ptrauth_blend_discriminator(selfPtr_,27361));
// CHECK-NEXT:     #else
// CHECK-NEXT:     void **vtable_ = *selfPtr_;
// CHECK-NEXT:     #endif
// CHECK-NEXT:     struct FTypeAddress {
// CHECK-NEXT:     decltype(_impl::$s5Class07DerivedA0C10storedPropSivs) * func;
// CHECK-NEXT:     };
// CHECK-NEXT:     FTypeAddress *fptrptr_ = reinterpret_cast<FTypeAddress *>(vtable_ + [[#VM8]] / sizeof(void *));
// CHECK-NEXT:     (* fptrptr_->func)(newValue, ::swift::_impl::_impl_RefCountedClass::getOpaquePointer(*this));
// CHECK-NEXT:     }

// CHECK:         swift::Int DerivedClass::operator [](swift::Int i) const
// CHECK-NEXT:    void ***selfPtr_ = reinterpret_cast<void ***>( ::swift::_impl::_impl_RefCountedClass::getOpaquePointer(*this));
// CHECK-NEXT:    #ifdef __arm64e__
// CHECK-NEXT:    void **vtable_ = ptrauth_auth_data(*selfPtr_, ptrauth_key_process_independent_data, ptrauth_blend_discriminator(selfPtr_,27361));
// CHECK-NEXT:    #else
// CHECK-NEXT:    void **vtable_ = *selfPtr_;
// CHECK-NEXT:    #endif
// CHECK-NEXT:    struct FTypeAddress {
// CHECK-NEXT:    decltype(_impl::$s5Class07DerivedA0CyS2icig) * func;
// CHECK-NEXT:    };
// CHECK-NEXT:    FTypeAddress *fptrptr_ = reinterpret_cast<FTypeAddress *>(vtable_ + [[#VM10]] / sizeof(void *));
// CHECK-NEXT:      return (* fptrptr_->func)(i, ::swift::_impl::_impl_RefCountedClass::getOpaquePointer(*this));

// CHECK:        void DerivedDerivedClass::virtualMethod() {
// CHECK-NEXT:     _impl::$s5Class07DerivedbA0C13virtualMethodyyF(::swift::_impl::_impl_RefCountedClass::getOpaquePointer(*this));
// CHECK-NEXT:     }

// CHECK:        BaseClass DerivedDerivedClass::virtualMethodInDerived(const BaseClass& x) {
// CHECK-NEXT:     return _impl::_impl_BaseClass::makeRetained(_impl::$s5Class07DerivedbA0C015virtualMethodInB0yAA04BaseA0CAFF(::swift::_impl::_impl_RefCountedClass::getOpaquePointer(x), ::swift::_impl::_impl_RefCountedClass::getOpaquePointer(*this)));
// CHECK-NEXT:     }

// CHECK:        void DerivedDerivedClass::methodInDerivedDerived() {
// CHECK-NEXT:     _impl::$s5Class07DerivedbA0C08methodInbB0yyF(::swift::_impl::_impl_RefCountedClass::getOpaquePointer(*this));
// CHECK-NEXT:     }

// CHECK:          swift::Int DerivedDerivedClass::getStoredProp() {
// CHECK-NEXT:     return _impl::$s5Class07DerivedbA0C10storedPropSivg(::swift::_impl::_impl_RefCountedClass::getOpaquePointer(*this));
// CHECK-NEXT:     }

// CHECK:          swift::Int DerivedDerivedClass::getComputedPropInDerivedDerived() {
// CHECK-NEXT:     return _impl::$s5Class07DerivedbA0C014computedPropInbB0Sivg(::swift::_impl::_impl_RefCountedClass::getOpaquePointer(*this));
// CHECK-NEXT:     }
