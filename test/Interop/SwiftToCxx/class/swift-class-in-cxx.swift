// RUN: %empty-directory(%t)
// RUN: %target-swift-frontend %s -typecheck -module-name Class -clang-header-expose-decls=all-public -emit-clang-header-path %t/class.h
// RUN: %FileCheck %s < %t/class.h

// RUN: %check-interop-cxx-header-in-clang(%t/class.h)

// RUN: %target-swift-frontend %s -typecheck -module-name Class -enable-library-evolution -clang-header-expose-decls=all-public -emit-clang-header-path %t/class-evo.h
// RUN: %FileCheck %s < %t/class-evo.h

// RUN: %check-interop-cxx-header-in-clang(%t/class-evo.h)

public final class ClassWithIntField {
  var field: Int64

  init() {
    field = 0
    print("init ClassWithIntField")
  }
  deinit {
    print("destroy ClassWithIntField")
  }
}

// CHECK: namespace Class SWIFT_PRIVATE_ATTR SWIFT_SYMBOL_MODULE("Class") {

// CHECK: SWIFT_EXTERN void * _Nonnull $s5Class011passThroughA12WithIntFieldyAA0adeF0CADF(void * _Nonnull x) SWIFT_NOEXCEPT SWIFT_CALL; // passThroughClassWithIntField(_:)
// CHECK-NEXT: SWIFT_EXTERN void * _Nonnull $s5Class06returnA12WithIntFieldAA0acdE0CyF(void) SWIFT_NOEXCEPT SWIFT_CALL; // returnClassWithIntField()
// CHECK-NEXT: SWIFT_EXTERN void $s5Class04takeA12WithIntFieldyyAA0acdE0CF(void * _Nonnull x) SWIFT_NOEXCEPT SWIFT_CALL; // takeClassWithIntField(_:)
// CHECK-NEXT: SWIFT_EXTERN void $s5Class04takeA17WithIntFieldInoutyyAA0acdE0CzF(void * _Nonnull * _Nonnull x) SWIFT_NOEXCEPT SWIFT_CALL; // takeClassWithIntFieldInout(_:)

// CHECK: namespace Class SWIFT_PRIVATE_ATTR SWIFT_SYMBOL_MODULE("Class") {

// CHECK: class SWIFT_SYMBOL("s:5Class0A12WithIntFieldC") ClassWithIntField;
// CHECK-NEXT: } // end namespace

// CHECK: namespace
// CHECK-SAME: swift SWIFT_PRIVATE_ATTR {
// CHECK-NEXT: #pragma clang diagnostic push
// CHECK-NEXT: #pragma clang diagnostic ignored "-Wc++17-extensions"
// CHECK-NEXT: template<>
// CHECK-NEXT: static inline const constexpr bool isUsableInGenericContext<Class::ClassWithIntField> = true;
// CHECK-NEXT: #pragma clang diagnostic pop
// CHECK-NEXT: } // namespace swift

// CHECK: namespace
// CHECK-SAME: Class SWIFT_PRIVATE_ATTR SWIFT_SYMBOL_MODULE("Class") {

// CHECK: namespace
// CHECK-SAME: _impl {
// CHECK-EMPTY:
// CHECK-NEXT: class _impl_ClassWithIntField;
// CHECK-NEXT: // Type metadata accessor for ClassWithIntField
// CHECK-NEXT: SWIFT_EXTERN swift::_impl::MetadataResponseTy $s5Class0A12WithIntFieldCMa(swift::_impl::MetadataRequestTy) SWIFT_NOEXCEPT SWIFT_CALL;
// CHECK-EMPTY:
// CHECK-EMPTY:
// CHECK-NEXT: } // namespace _impl
// CHECK-EMPTY:
// CHECK-NEXT: class SWIFT_SYMBOL("s:5Class0A12WithIntFieldC") ClassWithIntField final : public swift::_impl::RefCountedClass {
// CHECK-NEXT: public:
// CHECK-NEXT:   using RefCountedClass::RefCountedClass;
// CHECK-NEXT:   using RefCountedClass::operator=;
// CHECK-NEXT: protected:
// CHECK-NEXT:   SWIFT_INLINE_THUNK ClassWithIntField(void * _Nonnull ptr) noexcept : RefCountedClass(ptr) {}
// CHECK-NEXT: private:
// CHECK-NEXT:   friend class _impl::_impl_ClassWithIntField;
// CHECK-NEXT: #pragma clang diagnostic push
// CHECK-NEXT: #pragma clang diagnostic ignored "-Wc++17-extensions"
// CHECK-NEXT: #pragma clang diagnostic push
// CHECK-NEXT: #pragma clang diagnostic ignored "-Wreserved-identifier"
// CHECK-NEXT:   typedef char $s5Class0A12WithIntFieldCD;
// CHECK-NEXT:   static inline constexpr $s5Class0A12WithIntFieldCD __swift_mangled_name = 0;
// CHECK-NEXT: #pragma clang diagnostic pop
// CHECK-NEXT: #pragma clang diagnostic pop
// CHECK-NEXT: };
// CHECK-EMPTY:
// CHECK-NEXT:namespace _impl {
// CHECK-EMPTY:
// CHECK-NEXT:class _impl_ClassWithIntField {
// CHECK-NEXT:public:
// CHECK-NEXT:static SWIFT_INLINE_THUNK ClassWithIntField makeRetained(void * _Nonnull ptr) noexcept { return ClassWithIntField(ptr); }
// CHECK-NEXT:};
// CHECK-EMPTY:
// CHECK-NEXT:} // namespace _impl
// CHECK-EMPTY:
// CHECK-NEXT: } // end namespace
// CHECK-EMPTY:
// CHECK-NEXT: namespace swift SWIFT_PRIVATE_ATTR {
// CHECK-NEXT: #pragma clang diagnostic push
// CHECK-NEXT: #pragma clang diagnostic ignored "-Wc++17-extensions"
// CHECK-NEXT: template<>
// CHECK-NEXT: struct TypeMetadataTrait<Class::ClassWithIntField> {
// CHECK-NEXT:   SWIFT_INLINE_PRIVATE_HELPER void * _Nonnull getTypeMetadata() {
// CHECK-NEXT:     return Class::_impl::$s5Class0A12WithIntFieldCMa(0)._0;
// CHECK-NEXT:   }
// CHECK-NEXT: };
// CHECK-NEXT: namespace _impl{
// CHECK-NEXT: template<>
// CHECK-NEXT: struct implClassFor<Class::ClassWithIntField> { using type = Class::_impl::_impl_ClassWithIntField; };
// CHECK-NEXT: } // namespace
// CHECK-NEXT: #pragma clang diagnostic pop
// CHECK-NEXT: } // namespace swift
// CHECK-EMPTY:
// CHECK-NEXT: namespace Class SWIFT_PRIVATE_ATTR SWIFT_SYMBOL_MODULE("Class") {

// CHECK: SWIFT_INLINE_THUNK ClassWithIntField passThroughClassWithIntField(const ClassWithIntField& x) noexcept SWIFT_SYMBOL("s:5Class011passThroughA12WithIntFieldyAA0adeF0CADF") SWIFT_WARN_UNUSED_RESULT {
// CHECK-NEXT:  return _impl::_impl_ClassWithIntField::makeRetained(_impl::$s5Class011passThroughA12WithIntFieldyAA0adeF0CADF(::swift::_impl::_impl_RefCountedClass::getOpaquePointer(x)));
// CHECK-NEXT: }

public final class register { }

// CHECK: class SWIFT_SYMBOL("s:5Class8registerC") register_ final : public swift::_impl::RefCountedClass {

public func returnClassWithIntField() -> ClassWithIntField {
  return ClassWithIntField()
}


public func passThroughClassWithIntField(_ x: ClassWithIntField) -> ClassWithIntField {
  x.field = 42
  return x
}

public func takeClassWithIntField(_ x: ClassWithIntField) {
  print("ClassWithIntField: \(x.field);")
}

public func takeClassWithIntFieldInout(_ x: inout ClassWithIntField) {
  x = ClassWithIntField()
  x.field = -11
}

// CHECK: SWIFT_INLINE_THUNK ClassWithIntField returnClassWithIntField() noexcept SWIFT_SYMBOL("s:5Class06returnA12WithIntFieldAA0acdE0CyF") SWIFT_WARN_UNUSED_RESULT {
// CHECK-NEXT:   return _impl::_impl_ClassWithIntField::makeRetained(_impl::$s5Class06returnA12WithIntFieldAA0acdE0CyF());
// CHECK-NEXT: }

// CHECK: SWIFT_INLINE_THUNK void takeClassWithIntField(const ClassWithIntField& x) noexcept SWIFT_SYMBOL("s:5Class04takeA12WithIntFieldyyAA0acdE0CF") {
// CHECK-NEXT:  _impl::$s5Class04takeA12WithIntFieldyyAA0acdE0CF(::swift::_impl::_impl_RefCountedClass::getOpaquePointer(x));
// CHECK-NEXT: }

// CHECK: SWIFT_INLINE_THUNK void takeClassWithIntFieldInout(ClassWithIntField& x) noexcept SWIFT_SYMBOL("s:5Class04takeA17WithIntFieldInoutyyAA0acdE0CzF") {
// CHECK-NEXT:    _impl::$s5Class04takeA17WithIntFieldInoutyyAA0acdE0CzF(&::swift::_impl::_impl_RefCountedClass::getOpaquePointerRef(x));
// CHECK-NEXT:  }
