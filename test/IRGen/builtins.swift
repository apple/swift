
// RUN: %target-swift-frontend %use_no_opaque_pointers -module-name builtins -parse-stdlib -Xllvm -sil-disable-pass=target-constant-folding -disable-access-control -primary-file %s -emit-ir -o - -disable-objc-attr-requires-foundation-module | %FileCheck %s --check-prefix=CHECK --check-prefix=CHECK-%target-runtime
// RUN: %target-swift-frontend -module-name builtins -parse-stdlib -Xllvm -sil-disable-pass=target-constant-folding -disable-access-control -primary-file %s -emit-ir -o - -disable-objc-attr-requires-foundation-module

// REQUIRES: CPU=x86_64 || CPU=arm64 || CPU=arm64e

import Swift

// CHECK-DAG: [[REFCOUNT:%swift.refcounted.*]] = type
// CHECK-DAG: [[X:%T8builtins1XC]] = type
// CHECK-DAG: [[Y:%T8builtins1YC]] = type

typealias Int = Builtin.Int32
typealias Bool = Builtin.Int1

// CHECK: call swiftcc void @swift_errorInMain(

infix operator *
infix operator /
infix operator %

infix operator +
infix operator -

infix operator <<
infix operator >>

infix operator ...

infix operator <
infix operator <=
infix operator >
infix operator >=

infix operator ==
infix operator !=

func * (lhs: Int, rhs: Int) -> Int {
  return Builtin.mul_Int32(lhs, rhs)
  // CHECK: mul i32
}
func / (lhs: Int, rhs: Int) -> Int {
  return Builtin.sdiv_Int32(lhs, rhs)
  // CHECK: sdiv i32
}
func % (lhs: Int, rhs: Int) -> Int {
  return Builtin.srem_Int32(lhs, rhs)
  // CHECK: srem i32
}
func + (lhs: Int, rhs: Int) -> Int {
  return Builtin.add_Int32(lhs, rhs)
  // CHECK: add i32
}
func - (lhs: Int, rhs: Int) -> Int {
  return Builtin.sub_Int32(lhs, rhs)
  // CHECK: sub i32
}
// In C, 180 is <<, >>
func < (lhs: Int, rhs: Int) -> Bool {
  return Builtin.cmp_slt_Int32(lhs, rhs)
  // CHECK: icmp slt i32
}
func > (lhs: Int, rhs: Int) -> Bool {
  return Builtin.cmp_sgt_Int32(lhs, rhs)
  // CHECK: icmp sgt i32
}
func <=(lhs: Int, rhs: Int) -> Bool {
  return Builtin.cmp_sle_Int32(lhs, rhs)
  // CHECK: icmp sle i32
}
func >=(lhs: Int, rhs: Int) -> Bool {
  return Builtin.cmp_sge_Int32(lhs, rhs)
  // CHECK: icmp sge i32
}
func ==(lhs: Int, rhs: Int) -> Bool {
  return Builtin.cmp_eq_Int32(lhs, rhs)
  // CHECK: icmp eq i32
}
func !=(lhs: Int, rhs: Int) -> Bool {
  return Builtin.cmp_ne_Int32(lhs, rhs)
  // CHECK: icmp ne i32
}

func gepRaw_test(_ ptr: Builtin.RawPointer, offset: Builtin.Int64)
   -> Builtin.RawPointer {
  return Builtin.gepRaw_Int64(ptr, offset)
  // CHECK: getelementptr inbounds i8, i8*
}

// CHECK: define hidden {{.*}}i64 @"$s8builtins9load_test{{[_0-9a-zA-Z]*}}F"
func load_test(_ ptr: Builtin.RawPointer) -> Builtin.Int64 {
  // CHECK: [[CASTPTR:%.*]] = bitcast i8* [[PTR:%.*]] to i64*
  // CHECK-NEXT: load i64, i64* [[CASTPTR]]
  // CHECK: ret
  return Builtin.load(ptr)
}

// CHECK: define hidden {{.*}}i64 @"$s8builtins13load_raw_test{{[_0-9a-zA-Z]*}}F"
func load_raw_test(_ ptr: Builtin.RawPointer) -> Builtin.Int64 {
  // CHECK: [[CASTPTR:%.*]] = bitcast i8* [[PTR:%.*]] to i64*
  // CHECK-NEXT: load i64, i64* [[CASTPTR]], align 1
  // CHECK: ret
  return Builtin.loadRaw(ptr)
}

// CHECK: define hidden {{.*}}void @"$s8builtins11assign_test{{[_0-9a-zA-Z]*}}F"
func assign_test(_ value: Builtin.Int64, ptr: Builtin.RawPointer) {
  Builtin.assign(value, ptr)
  // CHECK: ret
}

// CHECK: define hidden {{.*}}%swift.refcounted* @"$s8builtins16load_object_test{{[_0-9a-zA-Z]*}}F"
func load_object_test(_ ptr: Builtin.RawPointer) -> Builtin.NativeObject {
  // CHECK: [[T0:%.*]] = load [[REFCOUNT]]*, [[REFCOUNT]]**
  // CHECK: call [[REFCOUNT]]* @swift_retain([[REFCOUNT]]* returned [[T0]])
  // CHECK: ret [[REFCOUNT]]* [[T0]]
  return Builtin.load(ptr)
}

// CHECK: define hidden {{.*}}%swift.refcounted* @"$s8builtins20load_raw_object_test{{[_0-9a-zA-Z]*}}F"
func load_raw_object_test(_ ptr: Builtin.RawPointer) -> Builtin.NativeObject {
  // CHECK: [[T0:%.*]] = load [[REFCOUNT]]*, [[REFCOUNT]]** %{{.*}}, align 1
  // CHECK: call [[REFCOUNT]]* @swift_retain([[REFCOUNT]]* returned [[T0]])
  // CHECK: ret [[REFCOUNT]]* [[T0]]
  return Builtin.loadRaw(ptr)
}

// CHECK: define hidden {{.*}}void @"$s8builtins18assign_object_test{{[_0-9a-zA-Z]*}}F"
func assign_object_test(_ value: Builtin.NativeObject, ptr: Builtin.RawPointer) {
  Builtin.assign(value, ptr)
}

// CHECK: define hidden {{.*}}void @"$s8builtins16init_object_test{{[_0-9a-zA-Z]*}}F"
func init_object_test(_ value: Builtin.NativeObject, ptr: Builtin.RawPointer) {
  // CHECK: [[DEST:%.*]] = bitcast i8* {{%.*}} to [[REFCOUNT]]**
  // CHECK-NEXT: call [[REFCOUNT]]* @swift_retain([[REFCOUNT]]* returned [[SRC:%.*]])
  // CHECK-NEXT: store [[REFCOUNT]]* [[SRC]], [[REFCOUNT]]** [[DEST]]
  Builtin.initialize(value, ptr)
}

func cast_test(_ ptr: inout Builtin.RawPointer, i8: inout Builtin.Int8,
               i64: inout Builtin.Int64, f: inout Builtin.FPIEEE32,
               d: inout Builtin.FPIEEE64
) {
  // CHECK: cast_test

  i8 = Builtin.trunc_Int64_Int8(i64)    // CHECK: trunc
  i64 = Builtin.zext_Int8_Int64(i8)     // CHECK: zext
  i64 = Builtin.sext_Int8_Int64(i8)     // CHECK: sext
  i64 = Builtin.ptrtoint_Int64(ptr)     // CHECK: ptrtoint
  ptr = Builtin.inttoptr_Int64(i64)     // CHECK: inttoptr
  i64 = Builtin.fptoui_FPIEEE64_Int64(d) // CHECK: fptoui
  i64 = Builtin.fptosi_FPIEEE64_Int64(d) // CHECK: fptosi
  d = Builtin.uitofp_Int64_FPIEEE64(i64) // CHECK: uitofp
  d = Builtin.sitofp_Int64_FPIEEE64(i64) // CHECK: sitofp
  d = Builtin.fpext_FPIEEE32_FPIEEE64(f) // CHECK: fpext
  f = Builtin.fptrunc_FPIEEE64_FPIEEE32(d) // CHECK: fptrunc
  i64 = Builtin.bitcast_FPIEEE64_Int64(d)   // CHECK: bitcast
  d = Builtin.bitcast_Int64_FPIEEE64(i64)   // CHECK: bitcast
}

func vector_bitcast_test(_ src: Builtin.Vec16xInt8) -> Builtin.Int16 {
  // CHECK: vector_bitcast_test
  // This is the idiom for pmovmskb on x86 targets:
  let zero: Builtin.Vec16xInt8 = Builtin.zeroInitializer()
  let mask = Builtin.cmp_slt_Vec16xInt8(src, zero)
  return Builtin.bitcast_Vec16xInt1_Int16(mask) // CHECK: bitcast
}

func vector_bitcast_test_ii(_ src: Builtin.Int16) -> Builtin.Vec16xInt8 {
  // CHECK: vector_bitcast_test_ii
  let v16x1 = Builtin.bitcast_Int16_Vec16xInt1(src) // CHECK: bitcast
  return Builtin.sext_Vec16xInt1_Vec16xInt8(v16x1)   // CHECK: sext
}

func shufflevector_test(_ src: Builtin.FPIEEE32) -> Builtin.Vec4xFPIEEE32 {
  // CHECK: insertelement <4 x float> zeroinitializer
  // CHECK: shufflevector <4 x float>
  let vec = Builtin.insertelement_Vec4xFPIEEE32_FPIEEE32_Int32(
    Builtin.zeroInitializer(), src, Builtin.zeroInitializer()
  )
  return Builtin.shufflevector_Vec4xFPIEEE32_Vec4xInt32(
    vec, vec, Builtin.zeroInitializer()
  )
}

func intrinsic_test(_ i32: inout Builtin.Int32, i16: inout Builtin.Int16,
                    _ v8i16: Builtin.Vec8xInt16) {
  // CHECK: intrinsic_test
  i32 = Builtin.int_bswap_Int32(i32) // CHECK: llvm.bswap.i32(

  i16 = Builtin.int_bswap_Int16(i16) // CHECK: llvm.bswap.i16(
  
  var x = Builtin.int_sadd_with_overflow_Int16(i16, i16) // CHECK: call { i16, i1 } @llvm.sadd.with.overflow.i16(
  
  i16 = Builtin.int_vector_reduce_smin_Vec8xInt16(v8i16) // CHECK: llvm.vector.reduce.smin.v8i16(
  
  Builtin.int_trap() // CHECK: llvm.trap()
}

// CHECK: define hidden {{.*}}void @"$s8builtins19sizeof_alignof_testyyF"()
func sizeof_alignof_test() {
  // CHECK: store i64 4, i64*
  var xs = Builtin.sizeof(Int.self) 
  // CHECK: store i64 4, i64*
  var xa = Builtin.alignof(Int.self) 
  // CHECK: store i64 1, i64*
  var ys = Builtin.sizeof(Bool.self) 
  // CHECK: store i64 1, i64*
  var ya = Builtin.alignof(Bool.self) 

}

// CHECK: define hidden {{.*}}void @"$s8builtins28sizeof_alignof_metatype_testyyF"()
func sizeof_alignof_metatype_test() {
  // CHECK: store i64 8, i64*
  var xs = Builtin.sizeof(Int.Type.self) 
  // CHECK: store i64 8, i64*
  var xa = Builtin.alignof(Int.Type.self) 
  // CHECK: store i64 8, i64*
  var xt = Builtin.strideof(Int.Type.self) 
}

// CHECK: define hidden {{.*}}void @"$s8builtins27generic_sizeof_alignof_testyyxlF"
func generic_sizeof_alignof_test<T>(_: T) {
  // CHECK:      [[T0:%.*]] = getelementptr inbounds %swift.vwtable, %swift.vwtable* [[T:%.*]], i32 0, i32 8
  // CHECK-NEXT: [[SIZE:%.*]] = load i64, i64* [[T0]]
  // CHECK-NEXT: store i64 [[SIZE]], i64* [[S:%.*]]
  var s = Builtin.sizeof(T.self)
  // CHECK:      [[T0:%.*]] = getelementptr inbounds %swift.vwtable, %swift.vwtable* [[T:%.*]], i32 0, i32 10
  // CHECK-NEXT: [[FLAGS:%.*]] = load i32, i32* [[T0]]
  // CHECK-NEXT: [[T2:%.*]] = zext i32 [[FLAGS]] to i64
  // CHECK-NEXT: [[T3:%.*]] = and i64 [[T2]], 255
  // CHECK-NEXT: [[ALIGN:%.*]] = add i64 [[T3]], 1
  // CHECK-NEXT: store i64 [[ALIGN]], i64* [[A:%.*]]
  var a = Builtin.alignof(T.self)
}

// CHECK: define hidden {{.*}}void @"$s8builtins21generic_strideof_testyyxlF"
func generic_strideof_test<T>(_: T) {
  // CHECK:      [[T0:%.*]] = getelementptr inbounds %swift.vwtable, %swift.vwtable* [[T:%.*]], i32 9
  // CHECK-NEXT: [[STRIDE:%.*]] = load i64, i64* [[T0]]
  // CHECK-NEXT: store i64 [[STRIDE]], i64* [[S:%.*]]
  var s = Builtin.strideof(T.self)
}

class X {}

class Y {}
func move(_ ptr: Builtin.RawPointer) {
  var temp : Y = Builtin.take(ptr)
  // CHECK:      define hidden {{.*}}void @"$s8builtins4move{{[_0-9a-zA-Z]*}}F"
  // CHECK:        [[SRC:%.*]] = bitcast i8* {{%.*}} to [[Y]]**
  // CHECK-NEXT:   [[VAL:%.*]] = load [[Y]]*, [[Y]]** [[SRC]]
  // CHECK-NEXT:   store [[Y]]* [[VAL]], [[Y]]** {{%.*}}
}

func allocDealloc(_ size: Builtin.Word, align: Builtin.Word) {
  var ptr = Builtin.allocRaw(size, align)
  Builtin.deallocRaw(ptr, size, align)
}

func fence_test() {
  // CHECK: fence acquire
  Builtin.fence_acquire()

  // CHECK: fence syncscope("singlethread") acq_rel
  Builtin.fence_acqrel_singlethread()
}

func cmpxchg_test(_ ptr: Builtin.RawPointer, a: Builtin.Int32, b: Builtin.Int32) {
  // rdar://12939803 - ER: support atomic cmpxchg/xchg with pointers

  // CHECK: [[Z_RES:%.*]] = cmpxchg i32* {{.*}}, i32 {{.*}}, i32 {{.*}} acquire acquire
  // CHECK: [[Z_VAL:%.*]] = extractvalue { i32, i1 } [[Z_RES]], 0
  // CHECK: [[Z_SUCCESS:%.*]] = extractvalue { i32, i1 } [[Z_RES]], 1
  // CHECK: store i32 [[Z_VAL]], i32* {{.*}}, align 4
  // CHECK: [[Z_SUCCESS_B:%.*]] = zext i1 [[Z_SUCCESS]] to i8
  // CHECK: store i8 [[Z_SUCCESS_B]], i8* {{.*}}, align 1
  var (z, zSuccess) = Builtin.cmpxchg_acquire_acquire_Int32(ptr, a, b)

  // CHECK: [[Y_RES:%.*]] = cmpxchg volatile i32* {{.*}}, i32 {{.*}}, i32 {{.*}} monotonic monotonic
  // CHECK: [[Y_VAL:%.*]] = extractvalue { i32, i1 } [[Y_RES]], 0
  // CHECK: [[Y_SUCCESS:%.*]] = extractvalue { i32, i1 } [[Y_RES]], 1
  // CHECK: store i32 [[Y_VAL]], i32* {{.*}}, align 4
  // CHECK: [[Y_SUCCESS_B:%.*]] = zext i1 [[Y_SUCCESS]] to i8
  // CHECK: store i8 [[Y_SUCCESS_B]], i8* {{.*}}, align 1
  var (y, ySuccess) = Builtin.cmpxchg_monotonic_monotonic_volatile_Int32(ptr, a, b)

  // CHECK: [[X_RES:%.*]] = cmpxchg volatile i32* {{.*}}, i32 {{.*}}, i32 {{.*}} syncscope("singlethread") acquire monotonic
  // CHECK: [[X_VAL:%.*]] = extractvalue { i32, i1 } [[X_RES]], 0
  // CHECK: [[X_SUCCESS:%.*]] = extractvalue { i32, i1 } [[X_RES]], 1
  // CHECK: store i32 [[X_VAL]], i32* {{.*}}, align 4
  // CHECK: [[X_SUCCESS_B:%.*]] = zext i1 [[X_SUCCESS]] to i8
  // CHECK: store i8 [[X_SUCCESS_B]], i8* {{.*}}, align 1
  var (x, xSuccess) = Builtin.cmpxchg_acquire_monotonic_volatile_singlethread_Int32(ptr, a, b)

  // CHECK: [[W_RES:%.*]] = cmpxchg volatile i64* {{.*}}, i64 {{.*}}, i64 {{.*}} seq_cst seq_cst
  // CHECK: [[W_VAL:%.*]] = extractvalue { i64, i1 } [[W_RES]], 0
  // CHECK: [[W_SUCCESS:%.*]] = extractvalue { i64, i1 } [[W_RES]], 1
  // CHECK: [[W_VAL_PTR:%.*]] = inttoptr i64 [[W_VAL]] to i8*
  // CHECK: store i8* [[W_VAL_PTR]], i8** {{.*}}, align 8
  // CHECK: [[W_SUCCESS_B:%.*]] = zext i1 [[W_SUCCESS]] to i8
  // CHECK: store i8 [[W_SUCCESS_B]], i8* {{.*}}, align 1
  var (w, wSuccess) = Builtin.cmpxchg_seqcst_seqcst_volatile_singlethread_RawPointer(ptr, ptr, ptr)

  // CHECK: [[V_RES:%.*]] = cmpxchg weak volatile i64* {{.*}}, i64 {{.*}}, i64 {{.*}} seq_cst seq_cst
  // CHECK: [[V_VAL:%.*]] = extractvalue { i64, i1 } [[V_RES]], 0
  // CHECK: [[V_SUCCESS:%.*]] = extractvalue { i64, i1 } [[V_RES]], 1
  // CHECK: [[V_VAL_PTR:%.*]] = inttoptr i64 [[V_VAL]] to i8*
  // CHECK: store i8* [[V_VAL_PTR]], i8** {{.*}}, align 8
  // CHECK: [[V_SUCCESS_B:%.*]] = zext i1 [[V_SUCCESS]] to i8
  // CHECK: store i8 [[V_SUCCESS_B]], i8* {{.*}}, align 1
  var (v, vSuccess) = Builtin.cmpxchg_seqcst_seqcst_weak_volatile_singlethread_RawPointer(ptr, ptr, ptr)
}

func atomicrmw_test(_ ptr: Builtin.RawPointer, a: Builtin.Int32,
                    ptr2: Builtin.RawPointer) {
  // CHECK: atomicrmw add i32* {{.*}}, i32 {{.*}} acquire
  var z = Builtin.atomicrmw_add_acquire_Int32(ptr, a)

  // CHECK: atomicrmw volatile max i32* {{.*}}, i32 {{.*}} monotonic
  var y = Builtin.atomicrmw_max_monotonic_volatile_Int32(ptr, a)
  
  // CHECK: atomicrmw volatile xchg i32* {{.*}}, i32 {{.*}} syncscope("singlethread") acquire
  var x = Builtin.atomicrmw_xchg_acquire_volatile_singlethread_Int32(ptr, a)
  
  // rdar://12939803 - ER: support atomic cmpxchg/xchg with pointers
  // CHECK: atomicrmw volatile xchg i64* {{.*}}, i64 {{.*}} syncscope("singlethread") acquire
  var w = Builtin.atomicrmw_xchg_acquire_volatile_singlethread_RawPointer(ptr, ptr2)

}

func addressof_test(_ a: inout Int, b: inout Bool) {
  // CHECK: bitcast i32* {{.*}} to i8*
  var ap : Builtin.RawPointer = Builtin.addressof(&a)
  // CHECK: bitcast i1* {{.*}} to i8*
  var bp : Builtin.RawPointer = Builtin.addressof(&b)
}

func fneg_test(_ half: Builtin.FPIEEE16,
               single: Builtin.FPIEEE32,
               double: Builtin.FPIEEE64)
  -> (Builtin.FPIEEE16, Builtin.FPIEEE32, Builtin.FPIEEE64)
{
  // CHECK: fsub half 0xH8000, {{%.*}}
  // CHECK: fsub float -0.000000e+00, {{%.*}}
  // CHECK: fsub double -0.000000e+00, {{%.*}}
  return (Builtin.fneg_FPIEEE16(half),
          Builtin.fneg_FPIEEE32(single),
          Builtin.fneg_FPIEEE64(double))
}

// The call to the builtins should get removed before we reach IRGen.
func testStaticReport(_ b: Bool, ptr: Builtin.RawPointer) -> () {
  Builtin.staticReport(b, b, ptr);
  return Builtin.staticReport(b, b, ptr);
}

// CHECK-LABEL: define hidden {{.*}}void @"$s8builtins12testCondFail{{[_0-9a-zA-Z]*}}F"(i1 %0, i1 %1)
func testCondFail(_ b: Bool, c: Bool) {
  // CHECK: [[EXPECT:%.*]] = call i1 @llvm.expect.i1(i1 %0, i1 false)
  // CHECK: br i1 [[EXPECT]], label %[[FAIL:.*]], label %[[CONT:.*]]
  Builtin.condfail_message(b, StaticString("message").unsafeRawPointer)
  // CHECK: [[CONT]]
  // CHECK: [[EXPECT:%.*]] = call i1 @llvm.expect.i1(i1 %1, i1 false)
  // CHECK: br i1 [[EXPECT]], label %[[FAIL2:.*]], label %[[CONT:.*]]
  Builtin.condfail_message(c, StaticString("message").unsafeRawPointer)
  // CHECK: [[CONT]]:
  // CHECK: ret void

  // CHECK: [[FAIL]]:
  // CHECK: call void @llvm.trap()
  // CHECK: unreachable

  // CHECK: [[FAIL2]]:
  // CHECK: call void @llvm.trap()
  // CHECK: unreachable
}

// CHECK-LABEL: define hidden {{.*}}void @"$s8builtins8testOnce{{[_0-9a-zA-Z]*}}F"(i8* %0, i8* %1) {{.*}} {
// CHECK:         [[PRED_PTR:%.*]] = bitcast i8* %0 to [[WORD:i64|i32]]*
// CHECK-objc:    [[PRED:%.*]] = load {{.*}} [[WORD]]* [[PRED_PTR]]
// CHECK-objc:    [[IS_DONE:%.*]] = icmp eq [[WORD]] [[PRED]], -1
// CHECK-objc:    [[IS_DONE_X:%.*]] = call i1 @llvm.expect.i1(i1 [[IS_DONE]], i1 true)
// CHECK-objc:    br i1 [[IS_DONE_X]], label %[[DONE:.*]], label %[[NOT_DONE:.*]]
// CHECK-objc:  [[DONE]]:
// CHECK-objc:    [[PRED:%.*]] = load {{.*}} [[WORD]]* [[PRED_PTR]]
// CHECK-objc:    [[IS_DONE:%.*]] = icmp eq [[WORD]] [[PRED]], -1
// CHECK-objc:    call void @llvm.assume(i1 [[IS_DONE]])
// CHECK-objc:  [[NOT_DONE]]:
// CHECK:         call void @swift_once([[WORD]]* [[PRED_PTR]], i8* %1, i8* undef)
// CHECK-objc:    br label %[[DONE]]

func testOnce(_ p: Builtin.RawPointer, f: @escaping @convention(c) (Builtin.RawPointer) -> ()) {
  Builtin.once(p, f)
}

// CHECK-LABEL: define hidden {{.*}}void @"$s8builtins19testOnceWithContext{{[_0-9a-zA-Z]*}}F"(i8* %0, i8* %1, i8* %2) {{.*}} {
// CHECK:         [[PRED_PTR:%.*]] = bitcast i8* %0 to [[WORD:i64|i32]]*
// CHECK-objc:    [[PRED:%.*]] = load {{.*}} [[WORD]]* [[PRED_PTR]]
// CHECK-objc:    [[IS_DONE:%.*]] = icmp eq [[WORD]] [[PRED]], -1
// CHECK-objc:    [[IS_DONE_X:%.*]] = call i1 @llvm.expect.i1(i1 [[IS_DONE]], i1 true)
// CHECK-objc:    br i1 [[IS_DONE_X]], label %[[DONE:.*]], label %[[NOT_DONE:.*]]
// CHECK-objc:  [[DONE]]:
// CHECK-objc:    [[PRED:%.*]] = load {{.*}} [[WORD]]* [[PRED_PTR]]
// CHECK-objc:    [[IS_DONE:%.*]] = icmp eq [[WORD]] [[PRED]], -1
// CHECK-objc:    call void @llvm.assume(i1 [[IS_DONE]])
// CHECK-objc:  [[NOT_DONE]]:
// CHECK:         call void @swift_once([[WORD]]* [[PRED_PTR]], i8* %1, i8* %2)
// CHECK-objc:    br label %[[DONE]]
func testOnceWithContext(_ p: Builtin.RawPointer, f: @escaping @convention(c) (Builtin.RawPointer) -> (), k: Builtin.RawPointer) {
  Builtin.onceWithContext(p, f, k)
}

class C {}
struct S {}
#if _runtime(_ObjC)
@objc class O {}
@objc protocol OP1 {}
@objc protocol OP2 {}
#endif
protocol P {}

// CHECK-LABEL: define hidden {{.*}}void @"$s8builtins10canBeClass{{[_0-9a-zA-Z]*}}F"
func canBeClass<T>(_ f: @escaping (Builtin.Int8) -> (), _: T) {
#if _runtime(_ObjC)
  // CHECK-objc: call {{.*}}void {{%.*}}(i8 1
  f(Builtin.canBeClass(O.self))
  // CHECK-objc: call {{.*}}void {{%.*}}(i8 1
  f(Builtin.canBeClass(OP1.self))
  typealias ObjCCompo = OP1 & OP2
  // CHECK-objc: call {{.*}}void {{%.*}}(i8 1
  f(Builtin.canBeClass(ObjCCompo.self))
#endif

  // CHECK: call {{.*}}void {{%.*}}(i8 0
  f(Builtin.canBeClass(S.self))
  // CHECK: call {{.*}}void {{%.*}}(i8 1
  f(Builtin.canBeClass(C.self))
  // CHECK: call {{.*}}void {{%.*}}(i8 0
  f(Builtin.canBeClass(P.self))
#if _runtime(_ObjC)
  typealias MixedCompo = OP1 & P
  // CHECK-objc: call {{.*}}void {{%.*}}(i8 0
  f(Builtin.canBeClass(MixedCompo.self))
#endif

  // CHECK: call {{.*}}void {{%.*}}(i8 2
  f(Builtin.canBeClass(T.self))
}

// CHECK-LABEL: define hidden {{.*}}void @"$s8builtins15destroyPODArray{{[_0-9a-zA-Z]*}}F"(i8* %0, i64 %1)
// CHECK-NOT:   loop:
// CHECK:         ret void
func destroyPODArray(_ array: Builtin.RawPointer, count: Builtin.Word) {
  Builtin.destroyArray(Int.self, array, count)
}

// CHECK-LABEL: define hidden {{.*}}void @"$s8builtins18destroyNonPODArray{{[_0-9a-zA-Z]*}}F"(i8* %0, i64 %1) {{.*}} {
// CHECK-NOT:       loop:
// CHECK:       call void @swift_arrayDestroy(
func destroyNonPODArray(_ array: Builtin.RawPointer, count: Builtin.Word) {
  Builtin.destroyArray(C.self, array, count)
}

// CHECK-LABEL: define hidden {{.*}}void @"$s8builtins15destroyGenArray_5count_yBp_BwxtlF"(i8* %0, i64 %1, %swift.opaque* noalias nocapture %2, %swift.type* %T)
// CHECK-NOT:   loop:
// CHECK:         call void @swift_arrayDestroy
func destroyGenArray<T>(_ array: Builtin.RawPointer, count: Builtin.Word, _: T) {
  Builtin.destroyArray(T.self, array, count)
}


// CHECK-LABEL: define hidden {{.*}}void @"$s8builtins12copyPODArray{{[_0-9a-zA-Z]*}}F"(i8* %0, i8* %1, i64 %2)
// check:         mul nuw i64 4, %2
// check:         call void @llvm.memcpy.p0i8.p0i8.i64(i8* {{.*}}, i8* {{.*}}, i64 {{.*}}, i32 4, i1 false)
// check:         mul nuw i64 4, %2
// check:         call void @llvm.memmove.p0i8.p0i8.i64(i8* {{.*}}, i8* {{.*}}, i64 {{.*}}, i32 4, i1 false)
// check:         mul nuw i64 4, %2
// check:         call void @llvm.memmove.p0i8.p0i8.i64(i8* {{.*}}, i8* {{.*}}, i64 {{.*}}, i32 4, i1 false)
// check:         mul nuw i64 4, %2
// check:         call void @llvm.memcpy.p0i8.p0i8.i64(i8* {{.*}}, i8* {{.*}}, i64 {{.*}}, i32 4, i1 false)
// check:         mul nuw i64 4, %2
// check:         call void @llvm.memmove.p0i8.p0i8.i64(i8* {{.*}}, i8* {{.*}}, i64 {{.*}}, i32 4, i1 false)
// check:         mul nuw i64 4, %2
// check:         call void @llvm.memmove.p0i8.p0i8.i64(i8* {{.*}}, i8* {{.*}}, i64 {{.*}}, i32 4, i1 false)
// check:         mul nuw i64 4, %2
// check:         call void @llvm.memcpy.p0i8.p0i8.i64(i8* {{.*}}, i8* {{.*}}, i64 {{.*}}, i32 4, i1 false)
func copyPODArray(_ dest: Builtin.RawPointer, src: Builtin.RawPointer, count: Builtin.Word) {
  Builtin.copyArray(Int.self, dest, src, count)
  Builtin.takeArrayFrontToBack(Int.self, dest, src, count)
  Builtin.takeArrayBackToFront(Int.self, dest, src, count)
  Builtin.assignCopyArrayNoAlias(Int.self, dest, src, count)
  Builtin.assignCopyArrayFrontToBack(Int.self, dest, src, count)
  Builtin.assignCopyArrayBackToFront(Int.self, dest, src, count)
  Builtin.assignTakeArray(Int.self, dest, src, count)
}


// CHECK-LABEL: define hidden {{.*}}void @"$s8builtins11copyBTArray{{[_0-9a-zA-Z]*}}F"(i8* %0, i8* %1, i64 %2) {{.*}} {
// CHECK-NOT:       loop:
// CHECK:         call void @swift_arrayInitWithCopy
// CHECK:         mul nuw i64 8, %2
// CHECK:         call void @llvm.memmove.p0i8.p0i8.i64(i8* {{.*}}, i8* {{.*}}, i64 {{.*}}, i1 false)
// CHECK:         mul nuw i64 8, %2
// CHECK:         call void @llvm.memmove.p0i8.p0i8.i64(i8* {{.*}}, i8* {{.*}}, i64 {{.*}}, i1 false)
// CHECK:         call void @swift_arrayAssignWithCopyNoAlias(
// CHECK:         call void @swift_arrayAssignWithCopyFrontToBack(
// CHECK:         call void @swift_arrayAssignWithCopyBackToFront(
// CHECK:         call void @swift_arrayAssignWithTake(
func copyBTArray(_ dest: Builtin.RawPointer, src: Builtin.RawPointer, count: Builtin.Word) {
  Builtin.copyArray(C.self, dest, src, count)
  Builtin.takeArrayFrontToBack(C.self, dest, src, count)
  Builtin.takeArrayBackToFront(C.self, dest, src, count)
  Builtin.assignCopyArrayNoAlias(C.self, dest, src, count)
  Builtin.assignCopyArrayFrontToBack(C.self, dest, src, count)
  Builtin.assignCopyArrayBackToFront(C.self, dest, src, count)
  Builtin.assignTakeArray(C.self, dest, src, count)
}

struct W { weak var c: C? }

// CHECK-LABEL: define hidden {{.*}}void @"$s8builtins15copyNonPODArray{{[_0-9a-zA-Z]*}}F"(i8* %0, i8* %1, i64 %2) {{.*}} {
// CHECK-NOT:       loop:
// CHECK:         call void @swift_arrayInitWithCopy(
// CHECK-NOT:       loop{{.*}}:
// CHECK:         call void @swift_arrayInitWithTakeFrontToBack(
// CHECK-NOT:       loop{{.*}}:
// CHECK:          call void @swift_arrayInitWithTakeBackToFront(
// CHECK:         call void @swift_arrayAssignWithCopyNoAlias(
// CHECK:         call void @swift_arrayAssignWithCopyFrontToBack(
// CHECK:         call void @swift_arrayAssignWithCopyBackToFront(
// CHECK:         call void @swift_arrayAssignWithTake(
func copyNonPODArray(_ dest: Builtin.RawPointer, src: Builtin.RawPointer, count: Builtin.Word) {
  Builtin.copyArray(W.self, dest, src, count)
  Builtin.takeArrayFrontToBack(W.self, dest, src, count)
  Builtin.takeArrayBackToFront(W.self, dest, src, count)
  Builtin.assignCopyArrayNoAlias(W.self, dest, src, count)
  Builtin.assignCopyArrayFrontToBack(W.self, dest, src, count)
  Builtin.assignCopyArrayBackToFront(W.self, dest, src, count)
  Builtin.assignTakeArray(W.self, dest, src, count)
}

// CHECK-LABEL: define hidden {{.*}}void @"$s8builtins12copyGenArray{{[_0-9a-zA-Z]*}}F"(i8* %0, i8* %1, i64 %2, %swift.opaque* noalias nocapture %3, %swift.type* %T)
// CHECK-NOT:   loop:
// CHECK:        call void @swift_arrayInitWithCopy
// CHECK-NOT:   loop:
// CHECK:        call void @swift_arrayInitWithTakeFrontToBack(
// CHECK-NOT:   loop:
// CHECK:        call void @swift_arrayInitWithTakeBackToFront(
// CHECK:        call void @swift_arrayAssignWithCopyNoAlias(
// CHECK:        call void @swift_arrayAssignWithCopyFrontToBack(
// CHECK:        call void @swift_arrayAssignWithCopyBackToFront(
// CHECK:        call void @swift_arrayAssignWithTake(
func copyGenArray<T>(_ dest: Builtin.RawPointer, src: Builtin.RawPointer, count: Builtin.Word, _: T) {
  Builtin.copyArray(T.self, dest, src, count)
  Builtin.takeArrayFrontToBack(T.self, dest, src, count)
  Builtin.takeArrayBackToFront(T.self, dest, src, count)
  Builtin.assignCopyArrayNoAlias(T.self, dest, src, count)
  Builtin.assignCopyArrayFrontToBack(T.self, dest, src, count)
  Builtin.assignCopyArrayBackToFront(T.self, dest, src, count)
  Builtin.assignTakeArray(T.self, dest, src, count)
}

// CHECK-LABEL: define hidden {{.*}}void @"$s8builtins24conditionallyUnreachableyyF"
// CHECK-NEXT:  entry
// CHECK-NEXT:    unreachable
func conditionallyUnreachable() {
  Builtin.conditionallyUnreachable()
}

struct Abc {
	var value : Builtin.Word
}

// CHECK-LABEL: define hidden {{.*}}@"$s8builtins22assumeNonNegative_testyBwAA3AbcVzF"
func assumeNonNegative_test(_ x: inout Abc) -> Builtin.Word {
  // CHECK: load {{.*}}, !range ![[R:[0-9]+]]
  return Builtin.assumeNonNegative_Word(x.value)
}

@inline(never)
func return_word(_ x: Builtin.Word) -> Builtin.Word {
	return x
}

// CHECK-LABEL: define hidden {{.*}}@"$s8builtins23assumeNonNegative_test2yBwBwF"
func assumeNonNegative_test2(_ x: Builtin.Word) -> Builtin.Word {
  // CHECK: call {{.*}}, !range ![[R]]
  return Builtin.assumeNonNegative_Word(return_word(x))
}

struct Empty {}
struct Pair { var i: Int, b: Bool }

// CHECK-LABEL: define hidden {{.*}}i64 @"$s8builtins15zeroInitializerAA5EmptyV_AA4PairVtyF"() {{.*}} {
// CHECK:  [[ALLOCA:%.*]] = alloca { i64 }
// CHECK:  bitcast
// CHECK:  lifetime.start
// CHECK:  [[EMPTYPAIR:%.*]] = bitcast { i64 }* [[ALLOCA]]
// CHECK:  [[PAIR:%.*]] = getelementptr inbounds {{.*}} [[EMPTYPAIR]], i32 0, i32 0
// CHECK:  [[FLDI:%.*]] = getelementptr inbounds {{.*}} [[PAIR]], i32 0, i32 0
// CHECK:  store i32 0, i32* [[FLDI]]
// CHECK:  [[FLDB:%.*]] = getelementptr inbounds {{.*}} [[PAIR]], i32 0, i32 1
// CHECK:  [[BYTE_ADDR:%.*]] = bitcast i1* [[FLDB]] to i8*
// CHECK:  store i8 0, i8* [[BYTE_ADDR]]
// CHECK:  [[RET:%.*]] = getelementptr inbounds {{.*}} [[ALLOCA]], i32 0, i32 0
// CHECK:  [[RES:%.*]] = load i64, i64* [[RET]]
// CHECK:  ret i64 [[RES]]
func zeroInitializer() -> (Empty, Pair) {
  return (Builtin.zeroInitializer(), Builtin.zeroInitializer())
}

// CHECK-LABEL: define hidden {{.*}}i64 @"$s8builtins20zeroInitializerTupleAA5EmptyV_AA4PairVtyF"() {{.*}} {
// CHECK:  [[ALLOCA:%.*]] = alloca { i64 }
// CHECK:  bitcast
// CHECK:  lifetime.start
// CHECK:  [[EMPTYPAIR:%.*]] = bitcast { i64 }* [[ALLOCA]]
// CHECK:  [[PAIR:%.*]] = getelementptr inbounds {{.*}} [[EMPTYPAIR]], i32 0, i32 0
// CHECK:  [[FLDI:%.*]] = getelementptr inbounds {{.*}} [[PAIR]], i32 0, i32 0
// CHECK:  store i32 0, i32* [[FLDI]]
// CHECK:  [[FLDB:%.*]] = getelementptr inbounds {{.*}} [[PAIR]], i32 0, i32 1
// CHECK:  [[BYTE_ADDR:%.*]] = bitcast i1* [[FLDB]] to i8*
// CHECK:  store i8 0, i8* [[BYTE_ADDR]]
// CHECK:  [[RET:%.*]] = getelementptr inbounds {{.*}} [[ALLOCA]], i32 0, i32 0
// CHECK:  [[RES:%.*]] = load i64, i64* [[RET]]
// CHECK:  ret i64 [[RES]]
func zeroInitializerTuple() -> (Empty, Pair) {
  return Builtin.zeroInitializer()
}

// CHECK-LABEL: define hidden {{.*}}void @"$s8builtins20zeroInitializerEmptyyyF"() {{.*}} {
// CHECK:         ret void
func zeroInitializerEmpty() {
  return Builtin.zeroInitializer()
}

// ----------------------------------------------------------------------------
// isUnique variants
// ----------------------------------------------------------------------------

// CHECK: define hidden {{.*}}void @"$s8builtins26acceptsBuiltinNativeObjectyyBoSgzF"([[BUILTIN_NATIVE_OBJECT_TY:%.*]]* nocapture dereferenceable({{.*}}) %0) {{.*}} {
func acceptsBuiltinNativeObject(_ ref: inout Builtin.NativeObject?) {}

// native
// CHECK-LABEL: define hidden {{.*}}i1 @"$s8builtins8isUniqueyBi1_BoSgzF"({{%.*}}* nocapture dereferenceable({{.*}}) %0) {{.*}} {
// CHECK-NEXT: entry:
// CHECK:      %[[BITCAST_RC:.+]] = bitcast [[BUILTIN_NATIVE_OBJECT_TY]]* %0 to %swift.refcounted**
// CHECK:      %[[LD_RC:.+]] = load %swift.refcounted*, %swift.refcounted** %[[BITCAST_RC]]
// CHECK-NEXT: %[[RET:.+]] = call zeroext i1 @swift_isUniquelyReferenced_native(%swift.refcounted* %[[LD_RC]])
// CHECK-NEXT: ret i1 %[[RET]]
func isUnique(_ ref: inout Builtin.NativeObject?) -> Bool {
  return Builtin.isUnique(&ref)
}

// native nonNull
// CHECK-LABEL: define hidden {{.*}}i1 @"$s8builtins8isUniqueyBi1_BozF"(%swift.refcounted** nocapture dereferenceable({{.*}}) %0) {{.*}} {
// CHECK-NEXT: entry:
// CHECK:      %[[LD_RC:.+]] = load %swift.refcounted*, %swift.refcounted** %0
// CHECK:      %[[RET:.+]] = call zeroext i1 @swift_isUniquelyReferenced_nonNull_native(%swift.refcounted* %[[LD_RC]])
// CHECK-NEXT: ret i1 %[[RET]]
func isUnique(_ ref: inout Builtin.NativeObject) -> Bool {
  return Builtin.isUnique(&ref)
}

// CHECK: define hidden {{.*}}void @"$s8builtins16acceptsAnyObjectyyyXlSgzF"([[OPTIONAL_ANYOBJECT_TY:%.*]]* nocapture dereferenceable({{.*}}) %0) {{.*}} {
func acceptsAnyObject(_ ref: inout Builtin.AnyObject?) {}

// ObjC
// CHECK-LABEL: define hidden {{.*}}i1 @"$s8builtins8isUniqueyBi1_yXlSgzF"({{%.*}}* nocapture dereferenceable({{.*}}) %0) {{.*}} {
// CHECK-NEXT: entry:
// CHECK:      [[ADDR:%.+]] = getelementptr inbounds [[OPTIONAL_ANYOBJECT_TY]], [[OPTIONAL_ANYOBJECT_TY]]* %0, i32 0, i32 0
// CHECK-NEXT: [[CASTED:%.+]] = bitcast {{.+}}* [[ADDR]] to [[UNKNOWN_OBJECT:%objc_object|%swift\.refcounted]]**
// CHECK-NEXT: [[REF:%.+]] = load [[UNKNOWN_OBJECT]]*, [[UNKNOWN_OBJECT]]** [[CASTED]]
// CHECK-objc-NEXT: [[RESULT:%.+]] = call zeroext i1 @swift_isUniquelyReferenced{{(NonObjC)?}}([[UNKNOWN_OBJECT]]* [[REF]])
// CHECK-native-NEXT: [[RESULT:%.+]] = call zeroext i1 @swift_isUniquelyReferenced_native([[UNKNOWN_OBJECT]]* [[REF]])
// CHECK-NEXT: ret i1 [[RESULT]]
func isUnique(_ ref: inout Builtin.AnyObject?) -> Bool {
  return Builtin.isUnique(&ref)
}

// ObjC nonNull
// CHECK-LABEL: define hidden {{.*}}i1 @"$s8builtins8isUniqueyBi1_yXlzF"
// CHECK-SAME:    (%AnyObject* nocapture dereferenceable({{.*}}) %0) {{.*}} {
// CHECK-NEXT: entry:
// CHECK:      [[ADDR:%.+]] = getelementptr inbounds %AnyObject, %AnyObject* %0, i32 0, i32 0
// CHECK:      [[REF:%.+]] = load [[UNKNOWN_OBJECT]]*, [[UNKNOWN_OBJECT]]** [[ADDR]]
// CHECK-objc-NEXT: [[RESULT:%.+]] = call zeroext i1 @swift_isUniquelyReferenced{{(NonObjC)?}}_nonNull([[UNKNOWN_OBJECT]]* [[REF]])
// CHECK-native-NEXT: [[RESULT:%.+]] = call zeroext i1 @swift_isUniquelyReferenced_nonNull_native([[UNKNOWN_OBJECT]]* [[REF]])
// CHECK-NEXT: ret i1 [[RESULT]]
func isUnique(_ ref: inout Builtin.AnyObject) -> Bool {
  return Builtin.isUnique(&ref)
}

// BridgeObject nonNull
// CHECK-LABEL: define hidden {{.*}}i1 @"$s8builtins8isUniqueyBi1_BbzF"(%swift.bridge** nocapture dereferenceable({{.*}}) %0) {{.*}} {
// CHECK-NEXT: entry:
// CHECK:      %[[LD:.+]] = load %swift.bridge*, %swift.bridge** %0
// CHECK:      %[[RET:.+]] = call zeroext i1 @swift_isUniquelyReferenced{{(NonObjC)?}}_nonNull_bridgeObject(%swift.bridge* %[[LD]])
// CHECK-NEXT: ret i1 %[[RET]]
func isUnique(_ ref: inout Builtin.BridgeObject) -> Bool {
  return Builtin.isUnique(&ref)
}

// CHECK-LABEL: define hidden{{.*}} @"$s8builtins10assumeTrueyyBi1_F"
// CHECK: call void @llvm.assume(i1 %0)
// CHECK: ret
func assumeTrue(_ x: Builtin.Int1) {
  Builtin.assume_Int1(x)
}
// BridgeObject nonNull
// CHECK-LABEL: define hidden {{.*}}i1 @"$s8builtins15isUnique_nativeyBi1_BbzF"(%swift.bridge** nocapture dereferenceable({{.*}}) %0) {{.*}} {
// CHECK-NEXT: entry:
// CHECK:      %[[BC:.+]] = bitcast %swift.bridge** %0 to %swift.refcounted**
// CHECK:      %[[LD:.+]] = load %swift.refcounted*, %swift.refcounted** %[[BC]]
// CHECK-NEXT: %[[RET:.+]] = call zeroext i1 @swift_isUniquelyReferenced_nonNull_native(%swift.refcounted* %[[LD]])
// CHECK-NEXT: ret i1 %[[RET]]
func isUnique_native(_ ref: inout Builtin.BridgeObject) -> Bool {
  return Builtin.isUnique_native(&ref)
}

// ImplicitlyUnwrappedOptional argument to isUnique.
// CHECK-LABEL: define hidden {{.*}}i1 @"$s8builtins11isUniqueIUOyBi1_BoSgzF"(%{{.*}}* nocapture dereferenceable({{.*}}) %0) {{.*}} {
// CHECK-NEXT: entry:
// CHECK: call zeroext i1 @swift_isUniquelyReferenced_native(%swift.refcounted*
// CHECK: ret i1
func isUniqueIUO(_ ref: inout Builtin.NativeObject?) -> Bool {
  var iuo : Builtin.NativeObject! = ref
  return Builtin.isUnique(&iuo)
}

// CHECK-LABEL: define hidden {{.*}} @"$s8builtins19COWBufferForReadingyAA1CCADnF"
// CHECK: ret %T8builtins1CC* %0
func COWBufferForReading(_ ref: __owned C) -> C {
  return Builtin.COWBufferForReading(ref)
}

// CHECK-LABEL: define {{.*}} @{{.*}}generic_ispod_test
func generic_ispod_test<T>(_: T) {
  // CHECK:      [[T0:%.*]] = getelementptr inbounds %swift.vwtable, %swift.vwtable* [[T:%.*]], i32 10
  // CHECK-NEXT: [[FLAGS:%.*]] = load i32, i32* [[T0]]
  // CHECK-NEXT: [[ISNOTPOD:%.*]] = and i32 [[FLAGS]], 65536
  // CHECK-NEXT: [[ISPOD:%.*]] = icmp eq i32 [[ISNOTPOD]], 0
  // CHECK-NEXT: [[BYTE_ADDR:%.*]] = bitcast i1* [[S:%.*]] to i8*
  // CHECK-NEXT: [[BYTE:%.*]] = zext i1 [[ISPOD]] to i8
  // CHECK-NEXT: store i8 [[BYTE]], i8* [[BYTE_ADDR]]
  var s = Builtin.ispod(T.self)
}

// CHECK-LABEL: define {{.*}} @{{.*}}ispod_test
func ispod_test() {
  // CHECK: store i8 1, i8*
  // CHECK: store i8 0, i8*
  var t = Builtin.ispod(Int.self)
  var f = Builtin.ispod(Builtin.NativeObject.self)
}

// CHECK-LABEL: define {{.*}} @{{.*}}generic_isbitwisetakable_test
func generic_isbitwisetakable_test<T>(_: T) {
  // CHECK:      [[T0:%.*]] = getelementptr inbounds %swift.vwtable, %swift.vwtable* [[T:%.*]], i32 10
  // CHECK-NEXT: [[FLAGS:%.*]] = load i32, i32* [[T0]]
  // CHECK-NEXT: [[ISNOTBITWISETAKABLE:%.*]] = and i32 [[FLAGS]], 1048576
  // CHECK-NEXT: [[ISBITWISETAKABLE:%.*]] = icmp eq i32 [[ISNOTBITWISETAKABLE]], 0
  // CHECK-NEXT: [[BYTE_ADDR:%.*]] = bitcast i1* [[S:%.*]]
  // CHECK-NEXT: [[BYTE:%.*]] = zext i1 [[ISBITWISETAKABLE]] to i8
  // CHECK-NEXT: store i8 [[BYTE]], i8* [[BYTE_ADDR]]
  var s = Builtin.isbitwisetakable(T.self)
}

// CHECK-LABEL: define {{.*}} @{{.*}}isbitwisetakable_test
func isbitwisetakable_test() {
  // CHECK: store i8 1, i8*
  // CHECK: store i8 1, i8*
  // CHECK: store i8 1, i8*
  // CHECK: store i8 1, i8*
  // CHECK: store i8 0, i8*
  var t1 = Builtin.isbitwisetakable(Int.self)
  var t2 = Builtin.isbitwisetakable(C.self)
  var t3 = Builtin.isbitwisetakable(Abc.self)
  var t4 = Builtin.isbitwisetakable(Empty.self)
  var f = Builtin.isbitwisetakable(W.self)
}

// CHECK-LABEL: define {{.*}} @{{.*}}is_same_metatype
func is_same_metatype_test(_ t1: Any.Type, _ t2: Any.Type) {
  // CHECK: [[MT1_AS_PTR:%.*]] = bitcast %swift.type* %0 to i8*
  // CHECK: [[MT2_AS_PTR:%.*]] = bitcast %swift.type* %1 to i8*
  // CHECK: icmp eq i8* [[MT1_AS_PTR]], [[MT2_AS_PTR]]
  var t = Builtin.is_same_metatype(t1, t2)
}

// CHECK-LABEL: define {{.*}} @{{.*}}atomicload
func atomicload(_ p: Builtin.RawPointer) {
  // CHECK: [[A:%.*]] = load atomic i8*, i8** {{%.*}} unordered, align 8
  let a: Builtin.RawPointer = Builtin.atomicload_unordered_RawPointer(p)
  // CHECK: [[B:%.*]] = load atomic i32, i32* {{%.*}} syncscope("singlethread") monotonic, align 4
  let b: Builtin.Int32 = Builtin.atomicload_monotonic_singlethread_Int32(p)
  // CHECK: [[C:%.*]] = load atomic volatile i64, i64* {{%.*}} syncscope("singlethread") acquire, align 8
  let c: Builtin.Int64 =
    Builtin.atomicload_acquire_volatile_singlethread_Int64(p)
  // CHECK: [[D0:%.*]] = load atomic volatile i32, i32* {{%.*}} seq_cst, align 4
  // CHECK: [[D:%.*]] = bitcast i32 [[D0]] to float
  let d: Builtin.FPIEEE32 = Builtin.atomicload_seqcst_volatile_FPIEEE32(p)

  // CHECK: store atomic i8* [[A]], i8** {{%.*}} unordered, align 8
  Builtin.atomicstore_unordered_RawPointer(p, a)
  // CHECK: store atomic i32 [[B]], i32* {{%.*}} syncscope("singlethread") monotonic, align 4
  Builtin.atomicstore_monotonic_singlethread_Int32(p, b)
  // CHECK: store atomic volatile i64 [[C]], i64* {{%.*}} syncscope("singlethread") release, align 8
  Builtin.atomicstore_release_volatile_singlethread_Int64(p, c)
  // CHECK: [[D1:%.*]] = bitcast float [[D]] to i32
  // CHECK: store atomic volatile i32 [[D1]], i32* {{.*}} seq_cst, align 4
  Builtin.atomicstore_seqcst_volatile_FPIEEE32(p, d)
}

// CHECK-LABEL: define {{.*}} @"$s8builtins14stringObjectOryS2u_SutF"(i64 %0, i64 %1)
// CHECK:      %4 = or i64 %0, %1
// CHECK-NEXT: ret i64 %4
func stringObjectOr(_ x: UInt, _ y: UInt) -> UInt {
  return UInt(Builtin.stringObjectOr_Int64(
  x._value, y._value))
}

func createInt(_ fn: () -> ()) throws {}
// CHECK-LABEL: define {{.*}}testForceTry
// CHECK: call swiftcc void @swift_unexpectedError(%swift.error*
func testForceTry(_ fn: () -> ()) {
  try! createInt(fn)
}

// CHECK-LABEL: declare{{( dllimport)?}} swiftcc void @swift_unexpectedError(%swift.error*

enum MyError : Error {
  case A, B
}

throw MyError.A

/// Builtin.globalStringTablePointer must be reduced to a string_literal instruction before IRGen. IRGen
/// should make this a trap.
// CHECK-LABEL: define {{.*}}globalStringTablePointer
// CHECK: call void @llvm.trap()
// CHECK: ret i8* undef
@_transparent
func globalStringTablePointerUse(_ str: String) -> Builtin.RawPointer {
  return Builtin.globalStringTablePointer(str);
}


// CHECK-LABEL: define {{.*}}convertTaskToJob
// CHECK:      call %swift.refcounted* @swift_retain(%swift.refcounted* returned %0)
// CHECK-NEXT: [[T0:%.*]] = bitcast %swift.refcounted* %0 to %swift.job*
// CHECK-NEXT: ret %swift.job* [[T0]]
func convertTaskToJob(_ task: Builtin.NativeObject) -> Builtin.Job {
  return Builtin.convertTaskToJob(task)
}


// CHECK: ![[R]] = !{i64 0, i64 9223372036854775807}
