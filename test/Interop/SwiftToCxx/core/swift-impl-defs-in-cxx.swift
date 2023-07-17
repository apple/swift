// RUN: %empty-directory(%t)
// RUN: %target-swift-frontend %s -typecheck -module-name Core -clang-header-expose-decls=all-public -emit-clang-header-path %t/core.h
// RUN: %FileCheck %s < %t/core.h

// RUN: %check-interop-cxx-header-in-clang(%t/core.h)


// CHECK:      #ifndef SWIFT_PRINTED_CORE
// CHECK-NEXT: #define SWIFT_PRINTED_CORE
// CHECK-NEXT: namespace swift SWIFT_PRIVATE_ATTR {
// CHECK-EMPTY:
// CHECK-NEXT: namespace _impl {
// CHECK-EMPTY:
// CHECK-NEXT: #ifdef __cplusplus
// CHECK-NEXT: extern "C" {
// CHECK-NEXT: #endif
// CHECK-EMPTY:
// CHECK-NEXT: // Swift type metadata response type.
// CHECK-NEXT: struct MetadataResponseTy {
// CHECK-NEXT:   void * _Null_unspecified _0;
// CHECK-NEXT:   uint{{.*}}_t _1;
// CHECK-NEXT: };
// CHECK-NEXT: // Swift type metadata request type.
// CHECK-NEXT: typedef uint{{.*}}_t MetadataRequestTy;
// CHECK-EMPTY:
// CHECK-NEXT: #if __cplusplus > 201402L
// CHECK-NEXT: #  define SWIFT_NOEXCEPT_FUNCTION_PTR noexcept
// CHECK-NEXT: #else
// CHECK-NEXT: #  define SWIFT_NOEXCEPT_FUNCTION_PTR
// CHECK-NEXT: #endif
// CHECK-EMPTY:
// CHECK-NEXT: using ValueWitnessInitializeBufferWithCopyOfBufferTy = void * _Nonnull(* __ptrauth_swift_value_witness_function_pointer(55882))(void * _Nonnull, void * _Nonnull, void * _Nonnull) SWIFT_NOEXCEPT_FUNCTION_PTR;
// CHECK-NEXT: using ValueWitnessDestroyTy = void(* __ptrauth_swift_value_witness_function_pointer(1272))(void * _Nonnull, void * _Nonnull) SWIFT_NOEXCEPT_FUNCTION_PTR;
// CHECK-NEXT: using ValueWitnessInitializeWithCopyTy = void * _Nonnull(* __ptrauth_swift_value_witness_function_pointer(58298))(void * _Nonnull, void * _Nonnull, void * _Nonnull) SWIFT_NOEXCEPT_FUNCTION_PTR;
// CHECK-NEXT: using ValueWitnessAssignWithCopyTy = void * _Nonnull(* __ptrauth_swift_value_witness_function_pointer(34641))(void * _Nonnull, void * _Nonnull, void * _Nonnull) SWIFT_NOEXCEPT_FUNCTION_PTR;
// CHECK-NEXT: using ValueWitnessInitializeWithTakeTy = void * _Nonnull(* __ptrauth_swift_value_witness_function_pointer(18648))(void * _Nonnull, void * _Nonnull, void * _Nonnull) SWIFT_NOEXCEPT_FUNCTION_PTR;
// CHECK-NEXT: using ValueWitnessAssignWithTakeTy = void * _Nonnull(* __ptrauth_swift_value_witness_function_pointer(61402))(void * _Nonnull, void * _Nonnull, void * _Nonnull) SWIFT_NOEXCEPT_FUNCTION_PTR;
// CHECK-NEXT: using ValueWitnessGetEnumTagSinglePayloadTy = unsigned(* __ptrauth_swift_value_witness_function_pointer(24816))(const void * _Nonnull, unsigned, void * _Nonnull) SWIFT_NOEXCEPT_FUNCTION_PTR;
// CHECK-NEXT: using ValueWitnessStoreEnumTagSinglePayloadTy = void(* __ptrauth_swift_value_witness_function_pointer(41169))(void * _Nonnull, unsigned, unsigned, void * _Nonnull) SWIFT_NOEXCEPT_FUNCTION_PTR;
// CHECK-EMPTY:
// CHECK-NEXT: struct ValueWitnessTable {
// CHECK-NEXT:  ValueWitnessInitializeBufferWithCopyOfBufferTy _Nonnull initializeBufferWithCopyOfBuffer;
// CHECK-NEXT:  ValueWitnessDestroyTy _Nonnull destroy;
// CHECK-NEXT:  ValueWitnessInitializeWithCopyTy _Nonnull initializeWithCopy;
// CHECK-NEXT:  ValueWitnessAssignWithCopyTy _Nonnull assignWithCopy;
// CHECK-NEXT:  ValueWitnessInitializeWithTakeTy _Nonnull initializeWithTake;
// CHECK-NEXT:  ValueWitnessAssignWithTakeTy _Nonnull assignWithTake;
// CHECK-NEXT:  ValueWitnessGetEnumTagSinglePayloadTy _Nonnull getEnumTagSinglePayload;
// CHECK-NEXT:  ValueWitnessStoreEnumTagSinglePayloadTy _Nonnull storeEnumTagSinglePayload;
// CHECK-NEXT:  size_t size;
// CHECK-NEXT:  size_t stride;
// CHECK-NEXT:  unsigned flags;
// CHECK-NEXT:  unsigned extraInhabitantCount;
// CHECK-EMPTY:
// CHECK-NEXT:  constexpr size_t getAlignment() const { return (flags & 255) + 1; }
// CHECK-NEXT: };
// CHECK-EMPTY:
// CHECK-NEXT: using EnumValueWitnessGetEnumTagTy = unsigned(* __ptrauth_swift_value_witness_function_pointer(41909))(const void * _Nonnull, void * _Nonnull) SWIFT_NOEXCEPT_FUNCTION_PTR;
// CHECK-NEXT: using EnumValueWitnessDestructiveProjectEnumDataTy = void(* __ptrauth_swift_value_witness_function_pointer(1053))(void * _Nonnull, void * _Nonnull) SWIFT_NOEXCEPT_FUNCTION_PTR;
// CHECK-NEXT: using EnumValueWitnessDestructiveInjectEnumTagTy = void(* __ptrauth_swift_value_witness_function_pointer(45796))(void * _Nonnull, unsigned, void * _Nonnull) SWIFT_NOEXCEPT_FUNCTION_PTR;
// CHECK-EMPTY:
// CHECK-NEXT: struct EnumValueWitnessTable {
// CHECK-NEXT:   ValueWitnessTable vwTable;
// CHECK-NEXT:   EnumValueWitnessGetEnumTagTy _Nonnull getEnumTag;
// CHECK-NEXT:   EnumValueWitnessDestructiveProjectEnumDataTy _Nonnull destructiveProjectEnumData;
// CHECK-NEXT:   EnumValueWitnessDestructiveInjectEnumTagTy _Nonnull destructiveInjectEnumTag;
// CHECK-NEXT: };
// CHECK-EMPTY:
// CHECK-NEXT: #undef SWIFT_NOEXCEPT_FUNCTION_PTR
// CHECK-EMPTY:
// CHECK-EMPTY:
// CHECK-NEXT: // type metadata address for Bool.
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
// CHECK: #ifdef __cplusplus
// CHECK-NEXT: }
// CHECK-NEXT: #endif
// CHECK-EMPTY:
// CHECK-EMPTY:
// CHECK-NEXT: } // namespace _impl
// CHECK-EMPTY:
// CHECK-EMPTY:
// CHECK-NEXT: #pragma clang diagnostic push
// CHECK-NEXT: #pragma clang diagnostic ignored "-Wc++17-extensions"
// CHECK-NEXT: template<>
// CHECK-NEXT: static inline const constexpr bool isUsableInGenericContext<bool> = true;
// CHECK-EMPTY:
// CHECK-NEXT: template<>
// CHECK-NEXT: struct TypeMetadataTrait<bool> {
// CHECK-NEXT:   static SWIFT_INLINE_THUNK void * _Nonnull getTypeMetadata() {
// CHECK-NEXT:     return &_impl::$sSbN;
// CHECK-NEXT:   }
// CHECK-NEXT: };
// CHECK-EMPTY:
// CHECK-NEXT: template<>
// CHECK-NEXT: static inline const constexpr bool isUsableInGenericContext<int8_t> = true;
// CHECK-EMPTY:
// CHECK-NEXT: template<>
// CHECK-NEXT: struct TypeMetadataTrait<int8_t> {
// CHECK-NEXT:   static SWIFT_INLINE_THUNK void * _Nonnull getTypeMetadata() {
// CHECK-NEXT:     return &_impl::$ss4Int8VN;
// CHECK-NEXT:   }
// CHECK-NEXT: };
// CHECK-EMPTY:
// CHECK-NEXT: template<>
// CHECK-NEXT: static inline const constexpr bool isUsableInGenericContext<uint8_t> = true;
// CHECK-EMPTY:
// CHECK-NEXT: template<>
// CHECK-NEXT: struct TypeMetadataTrait<uint8_t> {
// CHECK-NEXT:   static SWIFT_INLINE_THUNK void * _Nonnull getTypeMetadata() {
// CHECK-NEXT:     return &_impl::$ss5UInt8VN;
// CHECK-NEXT:   }
// CHECK-NEXT: };
// CHECK-EMPTY:
// CHECK-NEXT: template<>
// CHECK-NEXT: static inline const constexpr bool isUsableInGenericContext<int16_t> = true;
// CHECK-EMPTY:
// CHECK-NEXT: template<>
// CHECK-NEXT: struct TypeMetadataTrait<int16_t> {
// CHECK-NEXT:   static SWIFT_INLINE_THUNK void * _Nonnull getTypeMetadata() {
// CHECK-NEXT:     return &_impl::$ss5Int16VN;
// CHECK-NEXT:   }
// CHECK-NEXT: };
// CHECK-EMPTY:
// CHECK-NEXT: template<>
// CHECK-NEXT: static inline const constexpr bool isUsableInGenericContext<uint16_t> = true;
// CHECK-EMPTY:
// CHECK-NEXT: template<>
// CHECK-NEXT: struct TypeMetadataTrait<uint16_t> {
// CHECK-NEXT:   static SWIFT_INLINE_THUNK void * _Nonnull getTypeMetadata() {
// CHECK-NEXT:     return &_impl::$ss6UInt16VN;
// CHECK-NEXT:   }
// CHECK-NEXT: };
// CHECK-EMPTY:
// CHECK-NEXT: template<>
// CHECK-NEXT: static inline const constexpr bool isUsableInGenericContext<int32_t> = true;
// CHECK-EMPTY:
// CHECK-NEXT: template<>
// CHECK-NEXT: struct TypeMetadataTrait<int32_t> {
// CHECK-NEXT:   static SWIFT_INLINE_THUNK void * _Nonnull getTypeMetadata() {
// CHECK-NEXT:     return &_impl::$ss5Int32VN;
// CHECK-NEXT:   }
// CHECK-NEXT: };
// CHECK-EMPTY:
// CHECK-NEXT: template<>
// CHECK-NEXT: static inline const constexpr bool isUsableInGenericContext<uint32_t> = true;
// CHECK-EMPTY:
// CHECK-NEXT: template<>
// CHECK-NEXT: struct TypeMetadataTrait<uint32_t> {
// CHECK-NEXT:   static SWIFT_INLINE_THUNK void * _Nonnull getTypeMetadata() {
// CHECK-NEXT:     return &_impl::$ss6UInt32VN;
// CHECK-NEXT:   }
// CHECK-NEXT: };
// CHECK-EMPTY:
// CHECK-NEXT: template<>
// CHECK-NEXT: static inline const constexpr bool isUsableInGenericContext<int64_t> = true;
// CHECK-EMPTY:
// CHECK-NEXT: template<>
// CHECK-NEXT: struct TypeMetadataTrait<int64_t> {
// CHECK-NEXT:   static SWIFT_INLINE_THUNK void * _Nonnull getTypeMetadata() {
// CHECK-NEXT:     return &_impl::$ss5Int64VN;
// CHECK-NEXT:   }
// CHECK-NEXT: };
// CHECK-EMPTY:
// CHECK-NEXT: template<>
// CHECK-NEXT: static inline const constexpr bool isUsableInGenericContext<uint64_t> = true;
// CHECK-EMPTY:
// CHECK-NEXT: template<>
// CHECK-NEXT: struct TypeMetadataTrait<uint64_t> {
// CHECK-NEXT:   static SWIFT_INLINE_THUNK void * _Nonnull getTypeMetadata() {
// CHECK-NEXT:     return &_impl::$ss6UInt64VN;
// CHECK-NEXT:   }
// CHECK-NEXT: };
// CHECK-EMPTY:
// CHECK-NEXT: template<>
// CHECK-NEXT: static inline const constexpr bool isUsableInGenericContext<float> = true;
// CHECK-EMPTY:
// CHECK-NEXT: template<>
// CHECK-NEXT: struct TypeMetadataTrait<float> {
// CHECK-NEXT:   static SWIFT_INLINE_THUNK void * _Nonnull getTypeMetadata() {
// CHECK-NEXT:     return &_impl::$sSfN;
// CHECK-NEXT:   }
// CHECK-NEXT: };
// CHECK-EMPTY:
// CHECK-NEXT: template<>
// CHECK-NEXT: static inline const constexpr bool isUsableInGenericContext<double> = true;
// CHECK-EMPTY:
// CHECK-NEXT: template<>
// CHECK-NEXT: struct TypeMetadataTrait<double> {
// CHECK-NEXT:   static SWIFT_INLINE_THUNK void * _Nonnull getTypeMetadata() {
// CHECK-NEXT:     return &_impl::$sSdN;
// CHECK-NEXT:   }
// CHECK-NEXT: };
// CHECK-EMPTY:
// CHECK-NEXT: template<>
// CHECK-NEXT: static inline const constexpr bool isUsableInGenericContext<void *> = true;
// CHECK-EMPTY:
// CHECK-NEXT: template<>
// CHECK-NEXT: struct TypeMetadataTrait<void *> {
// CHECK-NEXT:   static SWIFT_INLINE_THUNK void * _Nonnull getTypeMetadata() {
// CHECK-NEXT:     return &_impl::$ss13OpaquePointerVN;
// CHECK-NEXT:   }
// CHECK-NEXT: };
// CHECK-EMPTY:
// CHECK: #pragma clang diagnostic pop
// CHECK-EMPTY:
// CHECK-NEXT: } // namespace swift
// CHECK-EMPTY:
// CHECK-NEXT: #endif
