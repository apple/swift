// RUN: %empty-directory(%t)
// RUN: %target-swift-frontend %s -typecheck -module-name Core -clang-header-expose-decls=all-public -emit-clang-header-path %t/core.h
// RUN: %FileCheck %s < %t/core.h

// RUN: %check-interop-cxx-header-in-clang(%t/core.h)

// REQUIRES: PTRSIZE=64
// REQUIRES: OS=macosx

// CHECK: // type metadata address for Bool.
// CHECK-NEXT: SWIFT_IMPORT_STDLIB_SYMBOL extern size_t $sSbN;
// CHECK-NEXT: // type metadata address for Int8.
// CHECK-NEXT: SWIFT_IMPORT_STDLIB_SYMBOL extern size_t $ss4Int8VN;
// CHECK-NEXT: // type metadata address for UInt8.
// CHECK-NEXT: SWIFT_IMPORT_STDLIB_SYMBOL extern size_t $ss5UInt8VN;
// CHECK-NEXT: // type metadata address for Int16.
// CHECK-NEXT: SWIFT_IMPORT_STDLIB_SYMBOL extern size_t $ss5Int16VN;
// CHECK-NEXT: // type metadata address for UInt16.
// CHECK-NEXT: SWIFT_IMPORT_STDLIB_SYMBOL extern size_t $ss6UInt16VN;
// CHECK-NEXT: // type metadata address for Int32.
// CHECK-NEXT: SWIFT_IMPORT_STDLIB_SYMBOL extern size_t $ss5Int32VN;
// CHECK-NEXT: // type metadata address for UInt32.
// CHECK-NEXT: SWIFT_IMPORT_STDLIB_SYMBOL extern size_t $ss6UInt32VN;
// CHECK-NEXT: // type metadata address for Int64.
// CHECK-NEXT: SWIFT_IMPORT_STDLIB_SYMBOL extern size_t $ss5Int64VN;
// CHECK-NEXT: // type metadata address for UInt64.
// CHECK-NEXT: SWIFT_IMPORT_STDLIB_SYMBOL extern size_t $ss6UInt64VN;
// CHECK-NEXT: // type metadata address for Float.
// CHECK-NEXT: SWIFT_IMPORT_STDLIB_SYMBOL extern size_t $sSfN;
// CHECK-NEXT: // type metadata address for Double.
// CHECK-NEXT: SWIFT_IMPORT_STDLIB_SYMBOL extern size_t $sSdN;
// CHECK-NEXT: // type metadata address for OpaquePointer.
// CHECK-NEXT: SWIFT_IMPORT_STDLIB_SYMBOL extern size_t $ss13OpaquePointerVN;
// CHECK-NEXT: // type metadata address for Int.
// CHECK-NEXT: SWIFT_IMPORT_STDLIB_SYMBOL extern size_t $sSiN;
// CHECK-NEXT: // type metadata address for UInt.
// CHECK-NEXT: SWIFT_IMPORT_STDLIB_SYMBOL extern size_t $sSuN;
// CHECK-EMPTY:
// CHECK-NEXT: #ifdef __cplusplus
// CHECK-NEXT: }
// CHECK-NEXT: #endif

// CHECK: static inline const constexpr bool isUsableInGenericContext<void *> = true;
// CHECK-EMPTY:
// CHECK-NEXT: template<>
// CHECK-NEXT: struct TypeMetadataTrait<void *> {
// CHECK-NEXT:   static SWIFT_INLINE_THUNK void * _Nonnull getTypeMetadata() {
// CHECK-NEXT:     return &_impl::$ss13OpaquePointerVN;
// CHECK-NEXT:   }
// CHECK-NEXT: };
// CHECK-EMPTY:
// CHECK-NEXT: template<>
// CHECK-NEXT: static inline const constexpr bool isUsableInGenericContext<swift::Int> = true;
// CHECK-EMPTY:
// CHECK-NEXT: template<>
// CHECK-NEXT: struct TypeMetadataTrait<swift::Int> {
// CHECK-NEXT:   static SWIFT_INLINE_THUNK void * _Nonnull getTypeMetadata() {
// CHECK-NEXT:     return &_impl::$sSiN;
// CHECK-NEXT:   }
// CHECK-NEXT: };
// CHECK-EMPTY:
// CHECK-NEXT: template<>
// CHECK-NEXT: static inline const constexpr bool isUsableInGenericContext<swift::UInt> = true;
// CHECK-EMPTY:
// CHECK-NEXT: template<>
// CHECK-NEXT: struct TypeMetadataTrait<swift::UInt> {
// CHECK-NEXT:   static SWIFT_INLINE_THUNK void * _Nonnull getTypeMetadata() {
// CHECK-NEXT:     return &_impl::$sSuN;
// CHECK-NEXT:   }
// CHECK-NEXT: };
// CHECK-EMPTY:
// CHECK-NEXT: #pragma clang diagnostic pop
// CHECK-EMPTY:
// CHECK-NEXT: } // namespace swift

