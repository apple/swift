// RUN: %empty-directory(%t)
// RUN: split-file %s %t

// RUN: %target-swift-frontend -typecheck %t/use-objc-types.swift -typecheck -module-name UseObjCTy -emit-clang-header-path %t/UseObjCTy.h -I %t -enable-experimental-cxx-interop -clang-header-expose-decls=all-public

// RUN: %FileCheck %s < %t/UseObjCTy.h

// RUN: %target-swift-frontend -typecheck %t/use-objc-types.swift -typecheck -module-name UseObjCTy -emit-clang-header-path %t/UseObjCTyExposeOnly.h -I %t -enable-experimental-cxx-interop -clang-header-expose-decls=has-expose-attr

// RUN: %FileCheck %s < %t/UseObjCTyExposeOnly.h

// FIXME: remove once https://github.com/apple/swift/pull/60971 lands.
// RUN: echo "#include \"header.h\"" > %t/full-header.h
// RUN: cat %t/UseObjCTy.h >> %t/full-header.h

// RUN: %target-interop-build-clangxx -std=gnu++20 -fobjc-arc -c -x objective-c++-header %t/full-header.h -o %t/o.o

// REQUIRES: objc_interop

//--- header.h

@interface ObjCKlass
-(ObjCKlass * _Nonnull) init;
@end

//--- module.modulemap
module ObjCTest {
    header "header.h"
}

//--- use-objc-types.swift
import ObjCTest

@_expose(Cxx)
public func retObjClass() -> ObjCKlass {
    return ObjCKlass()
}

@_expose(Cxx)
public func takeObjCClass(_ x: ObjCKlass) {
}

@_expose(Cxx)
public func takeObjCClassInout(_ x: inout ObjCKlass) {
}

@_expose(Cxx)
public func takeObjCClassNullable(_ x: ObjCKlass?) {
}

@_expose(Cxx)
public func retObjClassNullable() -> ObjCKlass? {
    return nil
}

// CHECK: ObjCKlass *_Nonnull $s9UseObjCTy03retB5ClassSo0B6CKlassCyF(void) SWIFT_NOEXCEPT SWIFT_CALL;
// CHECK-NEXT: ObjCKlass *_Nullable $s9UseObjCTy03retB13ClassNullableSo0B6CKlassCSgyF(void) SWIFT_NOEXCEPT SWIFT_CALL;
// CHECK-NEXT: void $s9UseObjCTy04takeB6CClassyySo0B6CKlassCF(ObjCKlass *_Nonnull x) SWIFT_NOEXCEPT SWIFT_CALL;
// CHECK-NEXT: void $s9UseObjCTy04takeB11CClassInoutyySo0B6CKlassCzF(ObjCKlass *_Nonnull __strong * _Nonnull x) SWIFT_NOEXCEPT SWIFT_CALL;
// CHECK-NEXT: void $s9UseObjCTy04takeB14CClassNullableyySo0B6CKlassCSgF(ObjCKlass *_Nullable x) SWIFT_NOEXCEPT SWIFT_CALL;

// CHECK: SWIFT_INLINE_THUNK ObjCKlass *_Nonnull retObjClass() noexcept SWIFT_SYMBOL({{.*}}) SWIFT_WARN_UNUSED_RESULT {
// CHECK-NEXT: return (__bridge_transfer ObjCKlass *)(__bridge void *)_impl::$s9UseObjCTy03retB5ClassSo0B6CKlassCyF();

// CHECK: SWIFT_INLINE_THUNK ObjCKlass *_Nullable retObjClassNullable() noexcept SWIFT_SYMBOL({{.*}}) SWIFT_WARN_UNUSED_RESULT {
// CHECK-NEXT: return (__bridge_transfer ObjCKlass *)(__bridge void *)_impl::$s9UseObjCTy03retB13ClassNullableSo0B6CKlassCSgyF();

// CHECK: void takeObjCClass(ObjCKlass *_Nonnull x) noexcept SWIFT_SYMBOL({{.*}}) {
// CHECK-NEXT: _impl::$s9UseObjCTy04takeB6CClassyySo0B6CKlassCF(x);

// CHECK: SWIFT_INLINE_THUNK void takeObjCClassInout(ObjCKlass *_Nonnull __strong & x) noexcept SWIFT_SYMBOL({{.*}}) {
// CHECK-NEXT: _impl::$s9UseObjCTy04takeB11CClassInoutyySo0B6CKlassCzF(&x);

// CHECK: SWIFT_INLINE_THUNK void takeObjCClassNullable(ObjCKlass *_Nullable x) noexcept SWIFT_SYMBOL({{.*}}) {
// CHECK-NEXT: _impl::$s9UseObjCTy04takeB14CClassNullableyySo0B6CKlassCSgF(x);
