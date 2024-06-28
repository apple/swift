// RUN: %empty-directory(%t)
// RUN: %target-swift-frontend %s -typecheck -module-name Structs -clang-header-expose-decls=all-public -emit-clang-header-path %t/structs.h
// RUN: %FileCheck %s < %t/structs.h

// RUN: %check-interop-c-header-in-clang(%t/structs.h -Wno-unused-function)

public struct StructOneI64 {
    let x: Int64
}

public struct StructU16AndPointer {
    let x: UInt8
    let y: UnsafeMutableRawPointer
}

public struct StructDoubleAndFloat {
    let x: Double
    let y: Float
}

// CHECK:      struct Structs_StructDoubleAndFloat {
// CHECK-NEXT:   _Alignas(8) char _storage[12];
// CHECK-NEXT: };

// CHECK:      struct Structs_StructOneI64 {
// CHECK-NEXT:   _Alignas(8) char _storage[8];
// CHECK-NEXT: };

// CHECK:      struct Structs_StructU16AndPointer {
// CHECK-NEXT:   _Alignas({{4|8}}) char _storage[{{8|16}}];
// CHECK-NEXT: };

public func returnNewStructOneI64() -> StructOneI64 { return StructOneI64(x: 42 ) }

public func passThroughStructOneI64(_ x: StructOneI64) -> StructOneI64 { return x }

public func printStructOneI64(_ x: StructOneI64) {
    print("StructOneI64.x = \(x.x)")
}

public func returnNewStructU16AndPointer(_ x: UnsafeMutableRawPointer) -> StructU16AndPointer {
    return StructU16AndPointer(x: 55, y: x)
}

public func getStructU16AndPointer_x(_ x: StructU16AndPointer) -> UInt8 { return x.x }

public func getStructU16AndPointer_y(_ x: StructU16AndPointer) -> UnsafeMutableRawPointer { return x.y }

public func returnNewStructDoubleAndFloat(_ y: Float, _ x: Double) -> StructDoubleAndFloat {
    return StructDoubleAndFloat(x: x, y: y)
}

public func getStructDoubleAndFloat_x(_ x: StructDoubleAndFloat) -> Double { return x.x }

public func getStructDoubleAndFloat_y(_ x: StructDoubleAndFloat) -> Float { return x.y }

// CHECK:      struct swift_interop_passStub_Structs_double_0_8_float_8_12 {
// CHECK-NEXT:  double _1;
// CHECK-NEXT:  float _2;
// CHECK-NEXT: };

// CHECK:      static SWIFT_C_INLINE_THUNK struct swift_interop_passStub_Structs_double_0_8_float_8_12 swift_interop_passDirect_Structs_double_0_8_float_8_12(const char * _Nonnull value) {
// CHECK-NEXT:  struct swift_interop_passStub_Structs_double_0_8_float_8_12 result;
// CHECK-NEXT:  memcpy(&result._1, value + 0, 8);
// CHECK-NEXT:  memcpy(&result._2, value + 8, 4);
// CHECK-NEXT:  return result;
// CHECK-NEXT: }

// CHECK:      SWIFT_EXTERN double $s7Structs25getStructDoubleAndFloat_xySdAA0cdeF0VF(struct swift_interop_passStub_Structs_double_0_8_float_8_12 x) SWIFT_NOEXCEPT SWIFT_CALL;

// CHECK:      SWIFT_EXTERN float $s7Structs25getStructDoubleAndFloat_yySfAA0cdeF0VF(struct swift_interop_passStub_Structs_double_0_8_float_8_12 x) SWIFT_NOEXCEPT SWIFT_CALL;

// CHECK:      struct swift_interop_passStub_Structs_[[StructU16AndPointer:uint8_t_0_1_void_ptr_[0-9_]+]] {
// CHECK-NEXT:  uint8_t _1;
// CHECK-NEXT:  void * _Nullable _2;
// CHECK-NEXT: };

// CHECK:      static SWIFT_C_INLINE_THUNK struct swift_interop_passStub_Structs_[[StructU16AndPointer]] swift_interop_passDirect_Structs_[[StructU16AndPointer]](const char * _Nonnull value) {
// CHECK-NEXT:  struct swift_interop_passStub_Structs_[[StructU16AndPointer]] result;
// CHECK-NEXT:  memcpy(&result._1, value + 0, 1);
// CHECK-NEXT:  memcpy(&result._2, value + [[PTRSIZE:[48]]], [[PTRSIZE]]);
// CHECK-NEXT:  return result;
// CHECK-NEXT: }

// CHECK:      SWIFT_EXTERN uint8_t $s7Structs24getStructU16AndPointer_xys5UInt8VAA0cdeF0VF(struct swift_interop_passStub_Structs_[[StructU16AndPointer]] x) SWIFT_NOEXCEPT SWIFT_CALL;

// CHECK:      SWIFT_EXTERN void * _Nonnull $s7Structs24getStructU16AndPointer_yySvAA0cdeF0VF(struct swift_interop_passStub_Structs_[[StructU16AndPointer]] x) SWIFT_NOEXCEPT SWIFT_CALL;

// CHECK:      struct swift_interop_returnStub_Structs_uint64_t_0_8 {
// CHECK-NEXT:  uint64_t _1;
// CHECK-NEXT: };

// CHECK:      static SWIFT_C_INLINE_THUNK void swift_interop_returnDirect_Structs_uint64_t_0_8(char * _Nonnull result, struct swift_interop_returnStub_Structs_uint64_t_0_8 value) {
// CHECK-NEXT:  memcpy(result + 0, &value._1, 8);
// CHECK-NEXT: }

// CHECK:      struct swift_interop_passStub_Structs_uint64_t_0_8 {
// CHECK-NEXT:  uint64_t _1;
// CHECK-NEXT: };

// CHECK:      static SWIFT_C_INLINE_THUNK struct swift_interop_passStub_Structs_uint64_t_0_8 swift_interop_passDirect_Structs_uint64_t_0_8(const char * _Nonnull value) {
// CHECK-NEXT:  struct swift_interop_passStub_Structs_uint64_t_0_8 result;
// CHECK-NEXT:  memcpy(&result._1, value + 0, 8);
// CHECK-NEXT:  return result;
// CHECK-NEXT: }

// CHECK:      SWIFT_EXTERN struct swift_interop_returnStub_Structs_uint64_t_0_8 $s7Structs23passThroughStructOneI64yAA0deF0VADF(struct swift_interop_passStub_Structs_uint64_t_0_8 x) SWIFT_NOEXCEPT SWIFT_CALL;

// CHECK:      SWIFT_EXTERN void $s7Structs17printStructOneI64yyAA0cdE0VF(struct swift_interop_passStub_Structs_uint64_t_0_8 x) SWIFT_NOEXCEPT SWIFT_CALL;

// CHECK: struct swift_interop_returnStub_Structs_double_0_8_float_8_12 {
// CHECK-NEXT:   double _1;
// CHECK-NEXT:  float _2;
// CHECK-NEXT: };

// CHECK: static SWIFT_C_INLINE_THUNK void swift_interop_returnDirect_Structs_double_0_8_float_8_12(char * _Nonnull result, struct swift_interop_returnStub_Structs_double_0_8_float_8_12 value) {
// CHECK-NEXT:   memcpy(result + 0, &value._1, 8);
// CHECK-NEXT:   memcpy(result + 8, &value._2, 4);
// CHECK-NEXT: }

// CHECK:      SWIFT_EXTERN struct swift_interop_returnStub_Structs_double_0_8_float_8_12 $s7Structs29returnNewStructDoubleAndFloatyAA0defG0VSf_SdtF(float y, double x) SWIFT_NOEXCEPT SWIFT_CALL;

// CHECK:      SWIFT_EXTERN struct swift_interop_returnStub_Structs_uint64_t_0_8 $s7Structs21returnNewStructOneI64AA0deF0VyF(void) SWIFT_NOEXCEPT SWIFT_CALL;

// CHECK: struct swift_interop_returnStub_Structs_[[StructU16AndPointer]] {
// CHECK-NEXT:   uint8_t _1;
// CHECK-NEXT:   void * _Nullable _2;
// CHECK-NEXT: };

// CHECK:      static SWIFT_C_INLINE_THUNK void swift_interop_returnDirect_Structs_[[StructU16AndPointer]](char * _Nonnull result, struct swift_interop_returnStub_Structs_[[StructU16AndPointer]] value) {
// CHECK-NEXT:  memcpy(result + 0, &value._1, 1);
// CHECK-NEXT:  memcpy(result + [[PTRSIZE]], &value._2, [[PTRSIZE]]);
// CHECK-NEXT: }

// CHECK:      SWIFT_EXTERN struct swift_interop_returnStub_Structs_[[StructU16AndPointer]] $s7Structs28returnNewStructU16AndPointeryAA0defG0VSvF(void * _Nonnull x) SWIFT_NOEXCEPT SWIFT_CALL;
