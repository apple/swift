// RUN: %target-swift-frontend -enable-type-layout -primary-file %s -emit-ir | %FileCheck %s --check-prefix=CHECK
// RUN: %target-swift-frontend -enable-type-layout -primary-file %s -O -emit-ir | %FileCheck %s --check-prefix=OPT --check-prefix=OPT-%target-ptrsize
// RUN: %target-swift-frontend -primary-file %s -emit-ir | %FileCheck %s --check-prefix=NOTL

public struct B<T> {
  var x: T
  var y: T
}

public struct A<T> {
  var a : B<T>
  var b:  B<T>
}

public class C<T> {
  init(a: B<T>, b: B<T>) {
    self.a = a
    self.b = b
  }
  var a : B<T>
  var b:  B<T>
}

public struct Fixed<T> {
  var a : Int8
  var b : Int16
  var c : C<T>
}

public enum NullableEnum<T> {
  case Value(a: C<T>)
  case None
}

public struct ForwardStruct<T> {
  var a : UInt64
  var b : UInt64
  var c : C<T>
}
public enum ForwardEnum<T> {
  case Value(a: ForwardStruct<T>)
  case None
}

// NOTL-LABEL: define{{.*}} ptr @"$s30typelayout_based_value_witness1AVwCP"(
// NOTL:   @"$s30typelayout_based_value_witness1BVMa"(
// NOTL: }

// CHECK-LABEL: define{{.*}} ptr @"$s30typelayout_based_value_witness1AVwCP"(
// CHECK-NOT:   @"$s30typelayout_based_value_witness1BVMa"(
// CHECK: }

// CHECK-LABEL: define{{.*}} void @"$s30typelayout_based_value_witness1AVwxx"(
// CHECK-NOT: @"$s30typelayout_based_value_witness1BVMa"
// CHECK: }

// CHECK-LABEL: define{{.*}} ptr @"$s30typelayout_based_value_witness1AVwcp"(
// CHECK-NOT: @"$s30typelayout_based_value_witness1BVMa"
// CHECK: }

// CHECK-LABEL: define{{.*}} ptr @"$s30typelayout_based_value_witness1AVwca"(
// CHECK-NOT: @"$s30typelayout_based_value_witness1BVMa"
// CHECK: }

// CHECK-LABEL: define{{.*}} ptr @"$s30typelayout_based_value_witness1AVwtk"(
// CHECK-NOT: @"$s30typelayout_based_value_witness1BVMa"
// CHECK: }

// CHECK-LABEL: define{{.*}} ptr @"$s30typelayout_based_value_witness1AVwta"(
// CHECK-NOT: @"$s30typelayout_based_value_witness1BVMa"
// CHECK: }

// CHECK-LABEL: define{{.*}} i32 @"$s30typelayout_based_value_witness1AVwet"(
// CHECK-NOT: @"$s30typelayout_based_value_witness1BVMa"
// CHECK: }

// CHECK-LABEL: define{{.*}} void @"$s30typelayout_based_value_witness1AVwst"(
// CHECK-NOT: @"$s30typelayout_based_value_witness1BVMa"
// CHECK: }


// OPT: define{{.*}} void @"$s30typelayout_based_value_witness1AVwxx"(ptr noalias %object, ptr nocapture readonly %"A<T>")
// OPT:   [[T_PARAM:%.*]] = getelementptr inbounds ptr, ptr %"A<T>", {{(i64|i32)}} 2
// OPT:   [[T:%.*]] = load ptr, ptr [[T_PARAM]]
// OPT:   [[VWT_ADDR:%.*]] = getelementptr inbounds ptr, ptr [[T]], {{(i64|i32)}} -1
// OPT:   [[VWT:%.*]] = load ptr, ptr [[VWT_ADDR]]
// OPT:   [[DESTROY_VW:%.*]] = getelementptr inbounds ptr, ptr [[VWT]], {{(i64|i32)}} 1
// OPT:   [[DESTROY:%.*]] = load ptr, ptr [[DESTROY_VW]]
// OPT:   tail call void [[DESTROY]](ptr noalias %object, ptr [[T]])
// OPT:   [[SIZE_VW:%.*]] = getelementptr inbounds %swift.vwtable, ptr [[VWT]], {{i64|i32}} 0, {{(i64|i32)}} 8
// OPT:   [[SIZE_T:%.*]] = load {{(i64|i32)}}, ptr [[SIZE_VW]]
// OPT:   [[OBJECT:%.*]] = ptrtoint ptr %object to {{(i64|i32)}}
// OPT:   [[FLAGS_VW:%.*]] = getelementptr inbounds {{.*}}, ptr [[VWT]], {{i64|i32}} 0, {{(i64|i32)}} 10
// OPT:   [[FLAGS3:%.*]] = load i32, ptr [[FLAGS_VW]]
// OPT:   [[FLAGS:%.*]] = and i32 [[FLAGS3]], 255
// OPT-64:   %flags.alignmentMask = zext i32 [[FLAGS]] to i64
// OPT-64:   [[TMP:%.*]] = add {{(i64|i32)}} [[SIZE_T]], %flags.alignmentMask
// OPT-32:   [[TMP:%.*]] = add {{(i64|i32)}} %flags.alignmentMask, [[SIZE_T]]
// OPT:   [[TMP2:%.*]] = add {{(i64|i32)}} [[TMP]], [[OBJECT]]
// OPT:   [[INVERTED:%.*]] = xor {{(i64|i32)}} %flags.alignmentMask, -1
// OPT:   [[ADDR2:%.*]] = and {{(i64|i32)}} [[TMP2:%.*]], [[INVERTED:%.*]]
// OPT:   [[ADDR_T2:%.*]] = inttoptr {{(i64|i32)}} [[ADDR2]] to ptr
// OPT:   tail call void [[DESTROY]](ptr noalias [[ADDR_T2]], ptr [[T]])
// OPT:   [[TMP3:%.*]] = and {{(i64|i32)}} [[TMP]], [[INVERTED:%.*]]
// OPT:   [[TMP4:%.*]] = add {{(i64|i32)}} [[TMP2:%.*]], [[TMP3]]
// OPT:   [[ADDR3:%.*]] = and {{(i64|i32)}} [[TMP4]], [[INVERTED:%.*]]
// OPT:   [[ADDR_T3:%.*]] = inttoptr {{(i64|i32)}} [[ADDR3]] to ptr
// OPT:   tail call void [[DESTROY]](ptr noalias [[ADDR_T3]], ptr [[T]])
// OPT:   [[TMP5:%.*]] = add {{(i64|i32)}} [[TMP]], [[ADDR3]]
// OPT:   [[ADDR4:%.*]] = and {{(i64|i32)}} [[TMP5]], [[INVERTED:%.*]]
// OPT:   [[ADDR_T4:%.*]] = inttoptr {{(i64|i32)}} [[ADDR4]] to ptr
// OPT:   tail call void [[DESTROY]](ptr noalias [[ADDR_T4]], ptr [[T]])
// OPT:   ret void
// CHECK: }

// Let's not crash on the following example.
public protocol P {}

public class Ref<T: P> {}

public enum E1<R: P> {
  case first(R)
  case second(S<R>)
}

public struct S<T: P> {
  public let f: E2<T>? = nil
}

public enum E2<T: P> {
    case first(Ref<T>)
    case second(String)
}
