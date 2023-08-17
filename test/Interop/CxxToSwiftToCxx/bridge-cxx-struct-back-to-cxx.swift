// RUN: %empty-directory(%t)
// RUN: split-file %s %t

// RUN: %target-swift-frontend -typecheck %t/use-cxx-types.swift -typecheck -module-name UseCxxTy -emit-clang-header-path %t/UseCxxTy.h -I %t -enable-experimental-cxx-interop -clang-header-expose-decls=all-public -disable-availability-checking

// RUN: %FileCheck %s < %t/UseCxxTy.h

// RUN: %target-swift-frontend -typecheck %t/use-cxx-types.swift -typecheck -module-name UseCxxTy -emit-clang-header-path %t/UseCxxTyExposeOnly.h -I %t -enable-experimental-cxx-interop -clang-header-expose-decls=has-expose-attr -disable-availability-checking

// RUN: %FileCheck %s < %t/UseCxxTyExposeOnly.h

// FIXME: remove once https://github.com/apple/swift/pull/60971 lands.
// RUN: echo "#include \"header.h\"" > %t/full-cxx-swift-cxx-bridging.h
// RUN: cat %t/UseCxxTy.h >> %t/full-cxx-swift-cxx-bridging.h

// RUN: %check-interop-cxx-header-in-clang(%t/full-cxx-swift-cxx-bridging.h -Wno-reserved-identifier)

// Check that the generated header can be
// built with Clang modules enabled in C++.

// RUN: %target-interop-build-clangxx -fsyntax-only -x c++-header %t/full-cxx-swift-cxx-bridging.h -std=gnu++20 -c -fmodules -fcxx-modules -I %t

//--- header.h

struct Trivial {
    short x, y;
};

namespace ns {

    struct TrivialinNS {
        short x, y;
    };

    template<class T>
    struct NonTrivialTemplate {
        T x;

        NonTrivialTemplate();
        NonTrivialTemplate(const NonTrivialTemplate<T> &other) : x(other.x) {}
        NonTrivialTemplate(NonTrivialTemplate<T> &&) = default;
        ~NonTrivialTemplate() {}
    };

    using TypeAlias = NonTrivialTemplate<TrivialinNS>;

    struct NonTrivialImplicitMove {
        NonTrivialTemplate<int> member;

        NonTrivialImplicitMove(const NonTrivialImplicitMove &other) : member(other.member) {}
    };

    #define IMMORTAL_REF                                \
         __attribute__((swift_attr("import_as_ref")))   \
         __attribute__((swift_attr("retain:immortal"))) \
         __attribute__((swift_attr("release:immortal")))
    struct IMMORTAL_REF Immortal {
    public:
    };

    inline Immortal *makeNewImmortal() {
        return new Immortal;
    }

    template<class T>
    struct IMMORTAL_REF ImmortalTemplate {
    public:
    };

    inline ImmortalTemplate<int> *makeNewImmortalInt() {
        return new ImmortalTemplate<int>;
    }

    using ImmortalCInt = ImmortalTemplate<int>;

    using NonTrivialTemplateInt = NonTrivialTemplate<int>;

    using NonTrivialTemplateTrivial = NonTrivialTemplate<TrivialinNS>;
}

using SimpleTypedef = int;

typedef struct { float column; } anonymousStruct;

namespace ns {

using anonStructInNS = struct { float row; };

}

//--- module.modulemap
module CxxTest {
    header "header.h"
    requires cplusplus
}

//--- use-cxx-types.swift
import CxxTest

@_expose(Cxx)
public func retImmortal() -> ns.Immortal {
    return ns.makeNewImmortal()
}

@_expose(Cxx)
public func retImmortalTemplate() -> ns.ImmortalCInt {
    return ns.makeNewImmortalInt()
}

@_expose(Cxx)
public func retNonTrivial() -> ns.NonTrivialTemplateInt {
    return ns.NonTrivialTemplateInt()
}

@_expose(Cxx)
public func retNonTrivial2() -> ns.NonTrivialTemplateTrivial {
    return ns.NonTrivialTemplateTrivial()
}

@_expose(Cxx)
public func retNonTrivialImplicitMove() -> ns.NonTrivialImplicitMove {
    return ns.NonTrivialImplicitMove()
}

@_expose(Cxx)
public func retNonTrivialTypeAlias() -> ns.TypeAlias {
    return ns.TypeAlias()
}

@_expose(Cxx)
public func retSimpleTypedef() -> SimpleTypedef {
    return SimpleTypedef()
}

@_expose(Cxx)
public func retTrivial() -> Trivial {
    return Trivial()
}

@_expose(Cxx)
public func takeImmortal(_ x: ns.Immortal) {
}

@_expose(Cxx)
public func takeImmortalTemplate(_ x: ns.ImmortalCInt) {
}

@_expose(Cxx)
public func takeNonTrivial2(_ x: ns.NonTrivialTemplateTrivial) {
}

@_expose(Cxx)
public func takeTrivial(_ x: Trivial) {
}

@_expose(Cxx)
public func takeTrivialInout(_ x: inout Trivial) {
}

@_expose(Cxx)
public struct Strct {
    public let transform: anonymousStruct
    public let transform2: ns.anonStructInNS
}

// CHECK: #if __has_feature(objc_modules)
// CHECK: #if __has_feature(objc_modules)
// CHECK-NEXT: #if __has_warning("-Watimport-in-framework-header")
// CHECK-NEXT: #pragma clang diagnostic ignored "-Watimport-in-framework-header"
// CHECK-NEXT:#endif
// CHECK-NEXT: #pragma clang module import CxxTest
// CHECK-NEXT: #endif


// CHECK: SWIFT_EXTERN void $s8UseCxxTy13retNonTrivialSo2nsO0031NonTrivialTemplateInt32_fbGJhubVyF(SWIFT_INDIRECT_RESULT void * _Nonnull) SWIFT_NOEXCEPT SWIFT_CALL; // retNonTrivial()
// CHECK: SWIFT_EXTERN struct swift_interop_returnStub_UseCxxTy_uint32_t_0_4 $s8UseCxxTy10retTrivialSo0E0VyF(void) SWIFT_NOEXCEPT SWIFT_CALL; // retTrivial()

// CHECK: ns::Immortal *_Nonnull retImmortal() noexcept SWIFT_SYMBOL({{.*}}) SWIFT_WARN_UNUSED_RESULT {
// CHECK-NEXT: return _impl::$s8UseCxxTy11retImmortalSo2nsO0E0VyF();
// CHECK-NEXT: }

// CHECK:  ns::ImmortalTemplate<int> *_Nonnull retImmortalTemplate() noexcept SWIFT_SYMBOL({{.*}}) SWIFT_WARN_UNUSED_RESULT {
// CHECK-NEXT: return _impl::$s8UseCxxTy19retImmortalTemplateSo2nsO0029ImmortalTemplateInt32_hEFAhqbVyF();
// CHECK-NEXT: }

// CHECK: } // end namespace
// CHECK-EMPTY:
// CHECK-NEXT: namespace swift SWIFT_PRIVATE_ATTR {
// CHECK-NEXT: namespace _impl {
// CHECK-EMPTY:
// CHECK-NEXT: // Type metadata accessor for NonTrivialTemplateInt
// CHECK-NEXT: SWIFT_EXTERN swift::_impl::MetadataResponseTy $sSo2nsO0031NonTrivialTemplateInt32_fbGJhubVMa(swift::_impl::MetadataRequestTy) SWIFT_NOEXCEPT SWIFT_CALL;
// CHECK-EMPTY:
// CHECK-EMPTY:
// CHECK-NEXT: } // namespace _impl
// CHECK-EMPTY:
// CHECK-NEXT: #pragma clang diagnostic push
// CHECK-NEXT: #pragma clang diagnostic ignored "-Wc++17-extensions"
// CHECK-NEXT: template<>
// CHECK-NEXT: static inline const constexpr bool isUsableInGenericContext<ns::NonTrivialTemplateInt> = true;
// CHECK-NEXT: template<>
// CHECK-NEXT: struct TypeMetadataTrait<ns::NonTrivialTemplateInt> {
// CHECK-NEXT:   static SWIFT_INLINE_THUNK void * _Nonnull getTypeMetadata() {
// CHECK-NEXT:     return _impl::$sSo2nsO0031NonTrivialTemplateInt32_fbGJhubVMa(0)._0;
// CHECK-NEXT:   }
// CHECK-NEXT: };
// CHECK-NEXT: namespace _impl{
// CHECK-NEXT: template<>
// CHECK-NEXT: static inline const constexpr bool isSwiftBridgedCxxRecord<ns::NonTrivialTemplateInt> = true;
// CHECK-NEXT: } // namespace
// CHECK-NEXT: #pragma clang diagnostic pop
// CHECK-NEXT: } // namespace swift
// CHECK-EMPTY:
// CHECK-NEXT: namespace UseCxxTy SWIFT_PRIVATE_ATTR SWIFT_SYMBOL_MODULE("UseCxxTy") {

// CHECK: SWIFT_INLINE_THUNK ns::NonTrivialTemplate<int> retNonTrivial() noexcept SWIFT_SYMBOL({{.*}}) SWIFT_WARN_UNUSED_RESULT {
// CHECK-NEXT: alignas(alignof(ns::NonTrivialTemplate<int>)) char storage[sizeof(ns::NonTrivialTemplate<int>)];
// CHECK-NEXT: auto * _Nonnull storageObjectPtr = reinterpret_cast<ns::NonTrivialTemplate<int> *>(storage);
// CHECK-NEXT: _impl::$s8UseCxxTy13retNonTrivialSo2nsO0031NonTrivialTemplateInt32_fbGJhubVyF(storage);
// CHECK-NEXT: ns::NonTrivialTemplate<int> result(static_cast<ns::NonTrivialTemplate<int> &&>(*storageObjectPtr));
// CHECK-NEXT: storageObjectPtr->~NonTrivialTemplate();
// CHECK-NEXT: return result;
// CHECK-NEXT: }
// CHECK-EMPTY:
// CHECK-NEXT: } // end namespace
// CHECK-EMPTY:
// CHECK-NEXT: namespace swift SWIFT_PRIVATE_ATTR {
// CHECK-NEXT: namespace _impl {
// CHECK-EMPTY:
// CHECK-NEXT: // Type metadata accessor for NonTrivialTemplateTrivial
// CHECK-NEXT: SWIFT_EXTERN swift::_impl::MetadataResponseTy $sSo2nsO0037NonTrivialTemplateTrivialinNS_CsGGkdcVMa(swift::_impl::MetadataRequestTy) SWIFT_NOEXCEPT SWIFT_CALL;
// CHECK-EMPTY:
// CHECK-EMPTY:
// CHECK-NEXT: } // namespace _impl
// CHECK-EMPTY:
// CHECK-NEXT: #pragma clang diagnostic push
// CHECK-NEXT: #pragma clang diagnostic ignored "-Wc++17-extensions"
// CHECK-NEXT: template<>
// CHECK-NEXT: static inline const constexpr bool isUsableInGenericContext<ns::NonTrivialTemplateTrivial> = true;
// CHECK-NEXT: template<>
// CHECK-NEXT: struct TypeMetadataTrait<ns::NonTrivialTemplateTrivial> {
// CHECK-NEXT:   static SWIFT_INLINE_THUNK void * _Nonnull getTypeMetadata() {
// CHECK-NEXT:     return _impl::$sSo2nsO0037NonTrivialTemplateTrivialinNS_CsGGkdcVMa(0)._0;
// CHECK-NEXT:   }
// CHECK-NEXT: };
// CHECK-NEXT: namespace _impl{
// CHECK-NEXT: template<>
// CHECK-NEXT: static inline const constexpr bool isSwiftBridgedCxxRecord<ns::NonTrivialTemplateTrivial> = true;
// CHECK-NEXT: } // namespace
// CHECK-NEXT: #pragma clang diagnostic pop
// CHECK-NEXT: } // namespace swift
// CHECK-EMPTY:
// CHECK-NEXT: namespace UseCxxTy SWIFT_PRIVATE_ATTR SWIFT_SYMBOL_MODULE("UseCxxTy") {
// CHECK-EMPTY:
// CHECK-NEXT: SWIFT_INLINE_THUNK ns::NonTrivialTemplate<ns::TrivialinNS> retNonTrivial2() noexcept SWIFT_SYMBOL({{.*}}) SWIFT_WARN_UNUSED_RESULT {
// CHECK-NEXT: alignas(alignof(ns::NonTrivialTemplate<ns::TrivialinNS>)) char storage[sizeof(ns::NonTrivialTemplate<ns::TrivialinNS>)];
// CHECK-NEXT: auto * _Nonnull storageObjectPtr = reinterpret_cast<ns::NonTrivialTemplate<ns::TrivialinNS> *>(storage);
// CHECK-NEXT: _impl::$s8UseCxxTy14retNonTrivial2So2nsO0037NonTrivialTemplateTrivialinNS_CsGGkdcVyF(storage);
// CHECK-NEXT: ns::NonTrivialTemplate<ns::TrivialinNS> result(static_cast<ns::NonTrivialTemplate<ns::TrivialinNS> &&>(*storageObjectPtr));
// CHECK-NEXT: storageObjectPtr->~NonTrivialTemplate();
// CHECK-NEXT: return result;
// CHECK-NEXT: }

// CHECK: SWIFT_INLINE_THUNK ns::NonTrivialImplicitMove retNonTrivialImplicitMove() noexcept SWIFT_SYMBOL({{.*}}) SWIFT_WARN_UNUSED_RESULT {
// CHECK-NEXT: alignas(alignof(ns::NonTrivialImplicitMove)) char storage[sizeof(ns::NonTrivialImplicitMove)];
// CHECK-NEXT: auto * _Nonnull storageObjectPtr = reinterpret_cast<ns::NonTrivialImplicitMove *>(storage);
// CHECK-NEXT: _impl::$s8UseCxxTy25retNonTrivialImplicitMoveSo2nsO0efgH0VyF(storage);
// CHECK-NEXT: ns::NonTrivialImplicitMove result(static_cast<ns::NonTrivialImplicitMove &&>(*storageObjectPtr));
// CHECK-NEXT: storageObjectPtr->~NonTrivialImplicitMove();
// CHECK-NEXT: return result;
// CHECK-NEXT: }

// CHECK: ns::NonTrivialTemplate<ns::TrivialinNS> retNonTrivialTypeAlias() noexcept SWIFT_SYMBOL({{.*}}) SWIFT_WARN_UNUSED_RESULT {

// CHECK: SWIFT_INLINE_THUNK Trivial retTrivial() noexcept SWIFT_SYMBOL({{.*}}) SWIFT_WARN_UNUSED_RESULT {
// CHECK-NEXT: alignas(alignof(Trivial)) char storage[sizeof(Trivial)];
// CHECK-NEXT: auto * _Nonnull storageObjectPtr = reinterpret_cast<Trivial *>(storage);
// CHECK-NEXT: _impl::swift_interop_returnDirect_UseCxxTy_uint32_t_0_4(storage, _impl::$s8UseCxxTy10retTrivialSo0E0VyF());
// CHECK-NEXT: return *storageObjectPtr;
// CHECK-NEXT: }

// CHECK: void takeImmortal(ns::Immortal *_Nonnull x) noexcept SWIFT_SYMBOL({{.*}}) {
// CHECK-NEXT: return _impl::$s8UseCxxTy12takeImmortalyySo2nsO0E0VF(x);
// CHECK-NEXT: }

// CHECK: void takeImmortalTemplate(ns::ImmortalTemplate<int> *_Nonnull x) noexcept SWIFT_SYMBOL({{.*}}) {
// CHECK-NEXT:   return _impl::$s8UseCxxTy20takeImmortalTemplateyySo2nsO0029ImmortalTemplateInt32_hEFAhqbVF(x);
// CHECK-NEXT: }

// CHECK: SWIFT_INLINE_THUNK void takeNonTrivial2(const ns::NonTrivialTemplate<ns::TrivialinNS>& x) noexcept SWIFT_SYMBOL({{.*}}) {
// CHECK-NEXT:   return _impl::$s8UseCxxTy15takeNonTrivial2yySo2nsO0037NonTrivialTemplateTrivialinNS_CsGGkdcVF(swift::_impl::getOpaquePointer(x));
// CHECK-NEXT: }

// CHECK: SWIFT_INLINE_THUNK void takeTrivial(const Trivial& x) noexcept SWIFT_SYMBOL({{.*}}) {
// CHECK-NEXT:   return _impl::$s8UseCxxTy11takeTrivialyySo0E0VF(_impl::swift_interop_passDirect_UseCxxTy_uint32_t_0_4(reinterpret_cast<const char *>(swift::_impl::getOpaquePointer(x))));
// CHECK-NEXT: }

// CHECK: SWIFT_INLINE_THUNK void takeTrivialInout(Trivial& x) noexcept SWIFT_SYMBOL({{.*}}) {
// CHECK-NEXT:   return _impl::$s8UseCxxTy16takeTrivialInoutyySo0E0VzF(swift::_impl::getOpaquePointer(x));
// CHECK-NEXT: }

// CHECK: SWIFT_INLINE_THUNK anonymousStruct Strct::getTransform() const {
// CHECK-NEXT: alignas(alignof(anonymousStruct)) char storage[sizeof(anonymousStruct)];

// CHECK: SWIFT_INLINE_THUNK ns::anonStructInNS Strct::getTransform2() const {
// CHECK-NEXT: alignas(alignof(ns::anonStructInNS)) char storage[sizeof(ns::anonStructInNS)];
