// RUN: %swift-frontend %s -enable-import-ptrauth-field-function-pointers -emit-ir -target arm64e-apple-ios13.0 -I %S/Inputs/ -validate-tbd-against-ir=none -Xllvm -sil-disable-pass=OnoneSimplification | %FileCheck %s
// REQUIRES: CPU=arm64e
// REQUIRES: OS=ios

import PointerAuth

// CHECK: define hidden swiftcc i32 @"$s25ptrauth_field_fptr_import05test_B8_fn_reads5Int32VyF"() #0 {
// CHECK: [[LD:%.*]] = load i64, ptr @ptr_to_secure_struct, align 8
// CHECK: 2:                                                ; preds = %entry
// CHECK:   [[CAST0:%.*]] = inttoptr i64 [[LD]] to ptr
// CHECK:   br label %9
// CHECK: 9:
// CHECK:   [[SECURESTRUCT:%.*]] = phi ptr [ [[CAST0]], {{.*}} ]
// CHECK:   %.secure_func_ptr = getelementptr inbounds %TSo12SecureStructV, ptr [[SECURESTRUCT]], i32 0, i32 0
// CHECK:   [[PTR:%.*]] = load ptr, ptr %.secure_func_ptr, align 8
// CHECK:   [[COND:%.*]] = icmp ne ptr [[PTR]], null
// CHECK:   br i1 [[COND]], label %resign-nonnull, label %resign-null
// CHECK: resign-nonnull:
// CHECK:   [[SIGNEDINT:%.*]] = ptrtoint ptr [[PTR]] to i64
// CHECK:   [[DEFAULTSIGNVAL:%.*]] = call i64 @llvm.ptrauth.resign(i64 [[SIGNEDINT]], i32 1, i64 88, i32 0, i64 0)
// CHECK:   [[AUTHPTR:%.*]] = inttoptr i64 [[DEFAULTSIGNVAL]] to ptr
// CHECK:   store ptr [[AUTHPTR]], ptr %ptrauth.temp, align 8
// CHECK:   br label %resign-cont
// CHECK: resign-null:
// CHECK:   store ptr [[PTR]], ptr %ptrauth.temp, align 8
// CHECK:   br label %resign-cont
// CHECK: resign-cont:                                      ; preds = %resign-null, %resign-nonnull
// CHECK:   [[FUNCPTR:%.*]] = load i64, ptr %ptrauth.temp, align 8
func test_field_fn_read() -> Int32 {
  let fn = ptr_to_secure_struct!.pointee.secure_func_ptr!
  return fn()
}

// CHECK-LABEL: define hidden swiftcc void @"$s25ptrauth_field_fptr_import05test_B14_fn_ptr_modifyyyF"() #0 {
// CHECK:   [[SECURESTRUCT:%.*]] = phi ptr [
// CHECK:   %.secure_func_ptr = getelementptr inbounds %TSo12SecureStructV, ptr [[SECURESTRUCT]], i32 0, i32 0
// CHECK:   store i64 ptrtoint (ptr @returnInt.ptrauth to i64), ptr %ptrauth.temp, align 8
// CHECK:   [[LD:%.*]] = load ptr, ptr %ptrauth.temp, align 8
// CHECK:   [[COND:%.*]] = icmp ne ptr [[LD]], null
// CHECK:   br i1 [[COND]], label %resign-nonnull, label %resign-null
// CHECK: resign-nonnull:
// CHECK:   [[CAST4:%.*]] = ptrtoint ptr [[LD]] to i64
// CHECK:   [[SIGN:%.*]] = call i64 @llvm.ptrauth.resign(i64 [[CAST4]], i32 0, i64 0, i32 1, i64 88)
// CHECK:   [[CAST5:%.*]] = inttoptr i64 [[SIGN]] to ptr
// CHECK:   store ptr [[CAST5]], ptr %.secure_func_ptr, align 8
func test_field_fn_ptr_modify() {
  ptr_to_secure_struct!.pointee.secure_func_ptr = returnInt
}

// CHECK: define hidden swiftcc i32 @"$s25ptrauth_field_fptr_import024test_addr_discriminated_B8_fn_reads5Int32VyF"() #0 {
// CHECK: [[LD:%.*]] = load i64, ptr @ptr_to_addr_discriminated_secure_struct, align 8
// CHECK:   [[CAST0:%.*]] = inttoptr i64 [[LD]] to ptr
// CHECK:   br label %[[L1:[0-9]+]]
// CHECK: [[L1]]:
// CHECK:   [[AddressDiscriminatedSecureStruct:%.*]] = phi ptr [ [[CAST0]]
// CHECK:   %.secure_func_ptr = getelementptr inbounds %TSo32AddressDiscriminatedSecureStructV, ptr [[AddressDiscriminatedSecureStruct]], i32 0, i32 0
// CHECK:   [[PTR:%.*]] = load ptr, ptr %.secure_func_ptr, align 8
// CHECK:   [[COND:%.*]] = icmp ne ptr [[PTR]], null
// CHECK:   br i1 [[COND]], label %resign-nonnull, label %resign-null
// CHECK: resign-nonnull:
// CHECK:   [[ADDR:%.*]] = ptrtoint ptr %.secure_func_ptr to i64
// CHECK:   [[BLEND:%.*]] = call i64 @llvm.ptrauth.blend(i64 [[ADDR]], i64 88)
// CHECK:   [[CAST4:%.*]] = ptrtoint ptr [[PTR]] to i64
// CHECK:   [[DEFAULTSIGNVAL:%.*]] = call i64 @llvm.ptrauth.resign(i64 [[CAST4]], i32 1, i64 [[BLEND]], i32 0, i64 0)
// CHECK:   [[AUTHPTR:%.*]] = inttoptr i64 [[DEFAULTSIGNVAL]] to ptr
// CHECK:   store ptr [[AUTHPTR]], ptr %ptrauth.temp, align 8
// CHECK:   br label %resign-cont
// CHECK: resign-cont:
// CHECK:   [[FUNCPTR:%.*]] = load i64, ptr %ptrauth.temp, align 8
func test_addr_discriminated_field_fn_read() -> Int32 {
  let fn = ptr_to_addr_discriminated_secure_struct!.pointee.secure_func_ptr!
  return fn()
}

// CHECK-LABEL: define hidden swiftcc void @"$s25ptrauth_field_fptr_import024test_addr_discriminated_B14_fn_ptr_modifyyyF"() #0 {
// CHECK: entry:
// CHECK:   [[PTR:%.*]] = load i64, ptr @ptr_to_addr_discriminated_secure_struct, align 8
// CHECK:   [[CAST0:%.*]] = inttoptr i64 [[PTR]] to ptr
// CHECK:   br label %[[L1:[0-9]+]]
// CHECK: [[L1]]:
// CHECK:   [[AddressDiscriminatedSecureStruct:%.*]] = phi ptr [ [[CAST0]]
// CHECK:   %.secure_func_ptr = getelementptr inbounds %TSo32AddressDiscriminatedSecureStructV, ptr [[AddressDiscriminatedSecureStruct]], i32 0, i32 0
// CHECK:   store i64 ptrtoint (ptr @returnInt.ptrauth to i64), ptr %ptrauth.temp, align 8
// CHECK:   [[LD:%.*]] = load ptr, ptr %ptrauth.temp, align 8
// CHECK:   [[COND:%.*]] = icmp ne ptr [[LD]], null
// CHECK:   br i1 [[COND]], label %resign-nonnull, label %resign-null
// CHECK: resign-nonnull:
// CHECK:   [[CAST4:%.*]] = ptrtoint ptr %.secure_func_ptr to i64
// CHECK:   [[BLEND:%.*]] = call i64 @llvm.ptrauth.blend(i64 [[CAST4]], i64 88)
// CHECK:   [[CAST5:%.*]] = ptrtoint ptr [[LD]] to i64
// CHECK:   [[SIGN:%.*]] = call i64 @llvm.ptrauth.resign(i64 [[CAST5]], i32 0, i64 0, i32 1, i64 [[BLEND]])
// CHECK:   [[CAST6:%.*]] = inttoptr i64 [[SIGN]] to ptr
// CHECK:   store ptr [[CAST6]], ptr %.secure_func_ptr, align 8
func test_addr_discriminated_field_fn_ptr_modify() {
  ptr_to_addr_discriminated_secure_struct!.pointee.secure_func_ptr = returnInt
}

// CHECK-LABEL: define hidden swiftcc i32 @"$s25ptrauth_field_fptr_import28test_addr_discriminated_copys5Int32VyF"() #0 {
// CHECK: entry:
// CHECK:   [[STRUCT:%.*]] = alloca %TSo32AddressDiscriminatedSecureStructV, align 8
// CHECK:   [[LD:%.*]] = load i64, ptr @ptr_to_addr_discriminated_secure_struct, align 8
// CHECK:   [[CAST0:%.*]] = inttoptr i64 [[LD]] to ptr
// CHECK:   br label %[[L1:[0-9]+]]
// CHECK: [[L1]]:
// CHECK:   [[PHI:%.*]] = phi ptr [ [[CAST0]]
// CHECK:   call void @__copy_assignment_8_8_pa1_88_0(ptr [[STRUCT]], ptr [[PHI]])
func test_addr_discriminated_copy() -> Int32 {
  let struct_with_signed_val = ptr_to_addr_discriminated_secure_struct.pointee
  return struct_with_signed_val.secure_func_ptr()
}

print(test_field_fn_read())
test_field_fn_ptr_modify()
print(test_addr_discriminated_field_fn_read())
test_addr_discriminated_field_fn_ptr_modify()
print(test_addr_discriminated_copy())
