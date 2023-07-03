// RUN: %target-swift-frontend -module-name generic_tuples -emit-ir -primary-file %s | %FileCheck %s

// Make sure that optimization passes don't choke on storage types for generic tuples
// RUN: %target-swift-frontend -module-name generic_tuples -emit-ir -O %s

// REQUIRES: CPU=x86_64

// CHECK-DAG: [[OPAQUE:%swift.opaque]] = type opaque
// CHECK-DAG: [[TUPLE_TYPE:%swift.tuple_type]] = type { %swift.type, i64, ptr, [0 x %swift.tuple_element_type] }
// CHECK-DAG: %swift.tuple_element_type = type { ptr, i32 }

func dup<T>(_ x: T) -> (T, T) { var x = x; return (x,x) }
// CHECK:    define hidden swiftcc void @"$s14generic_tuples3dupyx_xtxlF"(ptr noalias nocapture %0, ptr noalias nocapture %1, ptr noalias nocapture %2, ptr %T)
// CHECK:    entry:
//   Allocate a local variable for 'x'.
// CHECK: [[VWT_ADDR:%.*]] = getelementptr inbounds ptr, ptr %T, i64 -1
// CHECK: [[VWT:%.*]] = load ptr, ptr [[VWT_ADDR]]
// CHECK: [[SIZE_ADDR:%.*]] = getelementptr inbounds %swift.vwtable, ptr [[VWT]], i32 0, i32 8
// CHECK: [[SIZE:%.*]] = load i64, ptr [[SIZE_ADDR]]
// CHECK: [[X_ALLOCA:%.*]] = alloca i8, {{.*}} [[SIZE]], align 16
// Debug info shadow copy.
// CHECK: store ptr [[X_ALLOCA]]
// CHECK:      [[WITNESS_ADDR:%.*]] = getelementptr inbounds ptr, ptr [[VWT]], i32 2
// CHECK-NEXT: [[WITNESS:%.*]] = load ptr, ptr [[WITNESS_ADDR]], align 8
// CHECK-NEXT: [[X:%.*]] = call ptr [[WITNESS]](ptr noalias [[X_ALLOCA]], ptr noalias {{.*}}, ptr %T)
//   Copy 'x' into the first result.
// CHECK-NEXT: call ptr [[WITNESS]](ptr noalias %0, ptr noalias [[X_ALLOCA]], ptr %T)
//   Copy 'x' into the second element.
// CHECK-NEXT: [[WITNESS_ADDR:%.*]] = getelementptr inbounds ptr, ptr [[VWT]], i32 4
// CHECK-NEXT: [[WITNESS:%.*]] = load ptr, ptr [[WITNESS_ADDR]], align 8
// CHECK-NEXT: call ptr [[WITNESS]](ptr noalias %1, ptr noalias [[X_ALLOCA]], ptr %T)

struct S {}


func callDup(_ s: S) { _ = dup(s) }
// CHECK-LABEL: define hidden swiftcc void @"$s14generic_tuples7callDupyyAA1SVF"()
// CHECK-NEXT: entry:
// CHECK-NEXT: call swiftcc void @"$s14generic_tuples3dupyx_xtxlF"({{.*}} undef, {{.*}} undef, ptr {{.*}} @"$s14generic_tuples1SVMf", {{.*}})
// CHECK-NEXT: ret void

class C {}

func dupC<T : C>(_ x: T) -> (T, T) { return (x, x) }
// CHECK-LABEL: define hidden swiftcc { ptr, ptr } @"$s14generic_tuples4dupCyx_xtxAA1CCRbzlF"(ptr %0, ptr %T)
// CHECK-NEXT: entry:
// CHECK: call ptr @swift_retain(ptr returned %0)
// CHECK-NEXT: call ptr @swift_retain(ptr returned %0)
// CHECK-NEXT: [[TUP1:%.*]] = insertvalue { ptr, ptr } undef, ptr %0, 0
// CHECK-NEXT: [[TUP2:%.*]] = insertvalue { ptr, ptr } [[TUP1:%.*]], ptr %0, 1
// CHECK-NEXT: ret { ptr, ptr } [[TUP2]]

func callDupC(_ c: C) { _ = dupC(c) }
// CHECK-LABEL: define hidden swiftcc void @"$s14generic_tuples8callDupCyyAA1CCF"(ptr %0)
// CHECK-NEXT: entry:
// CHECK:      [[REQUEST:%.*]] = call {{.*}} @"$s14generic_tuples1CCMa"
// CHECK-NEXT: [[METATYPE:%.*]] = extractvalue {{.*}} [[REQUEST]]
// CHECK-NEXT: [[TUPLE:%.*]] = call swiftcc { ptr, ptr } @"$s14generic_tuples4dupCyx_xtxAA1CCRbzlF"(ptr %0, ptr [[METATYPE]])
// CHECK-NEXT: [[LEFT:%.*]] = extractvalue { ptr, ptr } [[TUPLE]], 0
// CHECK-NEXT: [[RIGHT:%.*]] = extractvalue { ptr, ptr } [[TUPLE]], 1
// CHECK-NEXT: call void @swift_release(ptr [[RIGHT]])
// CHECK-NEXT: call void @swift_release(ptr [[LEFT]])
// CHECK-NEXT: ret void

// CHECK: define hidden swiftcc i64 @"$s14generic_tuples4lumpySi_xxtxlF"(ptr noalias nocapture %0, ptr noalias nocapture %1, ptr noalias nocapture %2, ptr %T)
func lump<T>(_ x: T) -> (Int, T, T) { return (0,x,x) }
// CHECK: define hidden swiftcc { i64, i64 } @"$s14generic_tuples5lump2ySi_SixtxlF"(ptr noalias nocapture %0, ptr noalias nocapture %1, ptr %T)
func lump2<T>(_ x: T) -> (Int, Int, T) { return (0,0,x) }
// CHECK: define hidden swiftcc void @"$s14generic_tuples5lump3yx_xxtxlF"(ptr noalias nocapture %0, ptr noalias nocapture %1, ptr noalias nocapture %2, ptr noalias nocapture %3, ptr %T)
func lump3<T>(_ x: T) -> (T, T, T) { return (x,x,x) }
// CHECK: define hidden swiftcc i64 @"$s14generic_tuples5lump4yx_SixtxlF"(ptr noalias nocapture %0, ptr noalias nocapture %1, ptr noalias nocapture %2, ptr %T)
func lump4<T>(_ x: T) -> (T, Int, T) { return (x,0,x) }

// CHECK: define hidden swiftcc i64 @"$s14generic_tuples6unlumpyS2i_xxt_tlF"(i64 %0, ptr noalias nocapture %1, ptr noalias nocapture %2, ptr %T)
func unlump<T>(_ x: (Int, T, T)) -> Int { return x.0 }
// CHECK: define hidden swiftcc void @"$s14generic_tuples7unlump1yxSi_xxt_tlF"(ptr noalias nocapture sret({{.*}}) %0, i64 %1, ptr noalias nocapture %2, ptr noalias nocapture %3, ptr %T)
func unlump1<T>(_ x: (Int, T, T)) -> T { return x.1 }
// CHECK: define hidden swiftcc void @"$s14generic_tuples7unlump2yxx_Sixt_tlF"(ptr noalias nocapture sret({{.*}}) %0, ptr noalias nocapture %1, i64 %2, ptr noalias nocapture %3, ptr %T)
func unlump2<T>(_ x: (T, Int, T)) -> T { return x.0 }
// CHECK: define hidden swiftcc i64 @"$s14generic_tuples7unlump3ySix_Sixt_tlF"(ptr noalias nocapture %0, i64 %1, ptr noalias nocapture %2, ptr %T)
func unlump3<T>(_ x: (T, Int, T)) -> Int { return x.1 }


