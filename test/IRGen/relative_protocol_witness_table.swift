// RUN: %target-swift-frontend -enable-relative-protocol-witness-tables -module-name A -primary-file %s -emit-ir | %FileCheck %s --check-prefix=CHECK-%target-cpu --check-prefix=CHECK

// REQUIRES: CPU=x86_64 || CPU=arm64 || CPU=arm64e

func testVWT<T>(_ t: T) {
  var local = t
}

protocol FuncOnly {
    func a()
    func b()
}

struct AStruct : FuncOnly {
    func a() {}
    func b() {}
}

func requireWitness<T: FuncOnly> (_ t: T) {
    t.a()
}

func useIt() {
   requireWitness(AStruct())
}

protocol Inherited : FuncOnly {
    func c()
}

struct BStruct : Inherited {
    func a() {}
    func b() {}
    func c() {}
}

func requireWitness2<T: Inherited> (_ t: T) {
    t.a()
}

func useIt2() {
    requireWitness2(BStruct())
}

protocol WithAssoc {
    associatedtype AssocType
    func a()
}

struct CStruct : WithAssoc {
    typealias AssocType = Int
    func a() {}
}

func requireWitness3<T: WithAssoc> (_ t: T) {
    let a = T.self.AssocType
}

protocol WithAssocConformance {
    associatedtype AssocType : FuncOnly
    func initAssoc() -> AssocType
}

struct DStruct : WithAssocConformance {
    func initAssoc() -> AStruct  {
        return AStruct()
    }
}

func requireWitness4<T: WithAssocConformance>(_ t: T) {
    requireWitness(t.initAssoc())
}

protocol InitP {
  init()
}

struct GStruct<T> : WithAssocConformance where T: FuncOnly, T: InitP {
    func initAssoc() -> T {
        return T()
    }
}

struct ConditionalStruct<T> {
    init(_ t: T) {}
}

extension ConditionalStruct : WithAssocConformance where T: FuncOnly, T: InitP {
  func initAssoc() -> T {
      return T()
  }
}

func instantiate_conditional_conformance<T>(_ t : T) where T: FuncOnly, T: InitP {
  requireWitness4(ConditionalStruct(t))
}

protocol Base {
}

protocol Sub : Base {
  associatedtype S : Base
}

struct X<T> {
    init(_ t: T) {}
}

extension X: Base where T: Base { }
extension X: Sub where T: Sub, T.S == T {
   typealias S = X<T>
}

func requireWitness5<T: Sub> (_ t: T) {}

func instantiate_conditional_conformance_2nd<T>(_ t : T)  where T: Sub, T.S == T {
    requireWitness5(X(t))
}

// Relative protocol witness table.

// Simple Table.

// CHECK:    @"$s1A7AStructVAA8FuncOnlyAAWP" = hidden constant [3 x i32]
// CHECK-SAME: [i32 trunc (i64 sub (i64 ptrtoint (ptr @"$s1A7AStructVAA8FuncOnlyAAMc" to i64),
// CHECK-SAME:                      i64 ptrtoint (ptr @"$s1A7AStructVAA8FuncOnlyAAWP" to i64)) to i32),
// CHECK-SAME:  i32 trunc (i64 sub (i64 ptrtoint (ptr @"$s1A7AStructVAA8FuncOnlyA2aDP1ayyFTW" to i64),
// CHECK-SAME:                      i64 ptrtoint (ptr getelementptr inbounds ([3 x i32], ptr @"$s1A7AStructVAA8FuncOnlyAAWP", i32 0, i32 1) to i64)) to i32),
// CHECK-SAME:  i32 trunc (i64 sub (i64 ptrtoint (ptr @"$s1A7AStructVAA8FuncOnlyA2aDP1byyFTW" to i64),
// CHECK-SAME:                      i64 ptrtoint (ptr getelementptr inbounds ([3 x i32], ptr @"$s1A7AStructVAA8FuncOnlyAAWP", i32 0, i32 2) to i64)) to i32)
// CHECK-SAME: ], align 8

// Simple Table with parent.

// CHECK: @"$s1A7BStructVAA9InheritedAAWP" = hidden constant [3 x i32]
// CHECK-SAME: [i32 trunc (i64 sub (i64 ptrtoint (ptr @"$s1A7BStructVAA9InheritedAAMc" to i64),
// CHECK-SAME:                      i64 ptrtoint (ptr @"$s1A7BStructVAA9InheritedAAWP" to i64)) to i32),
// CHECK-SAME:  i32 trunc (i64 sub (i64 ptrtoint (ptr @"$s1A7BStructVAA8FuncOnlyAAWP" to i64),
// CHECK-SAME:                      i64 ptrtoint (ptr getelementptr inbounds ([3 x i32], ptr @"$s1A7BStructVAA9InheritedAAWP", i32 0, i32 1) to i64)) to i32),
// CHECK-SAME   i32 trunc (i64 sub (i64 ptrtoint (ptr @"$s1A7BStructVAA9InheritedA2aDP1cyyFTW" to i64),
// CHECK-SAME:                      i64 ptrtoint (ptr getelementptr inbounds ([3 x i32], ptr @"$s1A7BStructVAA9InheritedAAWP", i32 0, i32 2) to i64)) to i32)
// CHECK-SAME: ], align 8


// Simple associated type conformance.

// CHECK: @"$s1A7CStructVAA9WithAssocAAWP" = hidden constant [3 x i32]
// CHECK-SAME: [i32 trunc (i64 sub (i64 ptrtoint (ptr @"$s1A7CStructVAA9WithAssocAAMc" to i64),
// CHECK-SAME:                      i64 ptrtoint (ptr @"$s1A7CStructVAA9WithAssocAAWP" to i64)) to i32),
// CHECK-SAME:  i32 trunc (i64 sub (i64 ptrtoint (ptr getelementptr inbounds (i8, ptr @"symbolic Si", i64 1) to i64),
// CHECK-SAME:                      i64 ptrtoint (ptr getelementptr inbounds ([3 x i32], ptr @"$s1A7CStructVAA9WithAssocAAWP", i32 0, i32 1) to i64)) to i32),
// CHECK-SAME:  i32 trunc (i64 sub (i64 ptrtoint (ptr @"$s1A7CStructVAA9WithAssocA2aDP1ayyFTW" to i64),
// CHECK-SAME:                      i64 ptrtoint (ptr getelementptr inbounds ([3 x i32], ptr @"$s1A7CStructVAA9WithAssocAAWP", i32 0, i32 2) to i64)) to i32)
// CHECK-SAME: ], align 8

// CHECK: @"$s1A7DStructVAA20WithAssocConformanceAAWP" = hidden constant [4 x i32]
// CHECK-SAME: [i32 trunc (i64 sub (i64 ptrtoint (ptr @"$s1A7DStructVAA20WithAssocConformanceAAMc" to i64),
// CHECK-SAME:                      i64 ptrtoint (ptr @"$s1A7DStructVAA20WithAssocConformanceAAWP" to i64)) to i32),
// CHECK-SAME:  i32 trunc (i64 sub (i64 ptrtoint (ptr getelementptr inbounds (i8, ptr @"associated conformance 1A7DStructVAA20WithAssocConformanceAA0C4TypeAaDP_AA8FuncOnly", i64 1) to i64),
// CHECK-SAME:                      i64 ptrtoint (ptr getelementptr inbounds ([4 x i32], ptr @"$s1A7DStructVAA20WithAssocConformanceAAWP", i32 0, i32 1) to i64)) to i32)
// CHECK-SAME:  i32 trunc (i64 sub (i64 ptrtoint (ptr getelementptr inbounds (i8, ptr @"symbolic _____ 1A7AStructV", i64 1) to i64),
// CHECK-SAME:                      i64 ptrtoint (ptr getelementptr inbounds ([4 x i32], ptr @"$s1A7DStructVAA20WithAssocConformanceAAWP", i32 0, i32 2) to i64)) to i32)
// CHECK-SAME:  i32 trunc (i64 sub (i64 ptrtoint (ptr @"$s1A7DStructVAA20WithAssocConformanceA2aDP04initC00C4TypeQzyFTW" to i64),
// CHECK-SAME:                      i64 ptrtoint (ptr getelementptr inbounds ([4 x i32], ptr @"$s1A7DStructVAA20WithAssocConformanceAAWP", i32 0, i32 3) to i64)) to i32)
// CHECK-SAME: ], align 8

// CHECK: @"$s1A7GStructVyxGAA20WithAssocConformanceAAWP" = hidden constant [4 x i32]
// CHECK-SAME: [i32 trunc (i64 sub (i64 ptrtoint (ptr @"$s1A7GStructVyxGAA20WithAssocConformanceAAMc" to i64),
// CHECK-SAME:                      i64 ptrtoint (ptr @"$s1A7GStructVyxGAA20WithAssocConformanceAAWP" to i64)) to i32),
// CHECK-SAME:  i32 trunc (i64 sub (i64 ptrtoint (ptr getelementptr inbounds (i8, ptr @"associated conformance 1A7GStructVyxGAA20WithAssocConformanceAA0C4TypeAaEP_AA8FuncOnly", i64 1) to i64),
// CHECK-SAME:                      i64 ptrtoint (ptr getelementptr inbounds ([4 x i32], ptr @"$s1A7GStructVyxGAA20WithAssocConformanceAAWP", i32 0, i32 1) to i64)) to i32)
// CHECK-SAME:  i32 trunc (i64 sub (i64 ptrtoint (ptr getelementptr inbounds (i8, ptr @"symbolic x", i64 1) to i64),
// CHECK-SAME:                      i64 ptrtoint (ptr getelementptr inbounds ([4 x i32], ptr @"$s1A7GStructVyxGAA20WithAssocConformanceAAWP", i32 0, i32 2) to i64)) to i32)
// CHECK-SAME:  i32 trunc (i64 sub (i64 ptrtoint (ptr @"$s1A7GStructVyxGAA20WithAssocConformanceA2aEP04initC00C4TypeQzyFTW" to i64),
// CHECK-SAME:                      i64 ptrtoint (ptr getelementptr inbounds ([4 x i32], ptr @"$s1A7GStructVyxGAA20WithAssocConformanceAAWP", i32 0, i32 3) to i64)) to i32)
// CHECK-SAME: ], align 8

// Conditional conformance

// CHECK: @"$s1A17ConditionalStructVyxGAA20WithAssocConformanceA2A8FuncOnlyRzAA5InitPRzlWP" = hidden constant [4 x i32]
// CHECK-SAME: [i32 trunc (i64 sub (i64 ptrtoint (ptr @"$s1A17ConditionalStructVyxGAA20WithAssocConformanceA2A8FuncOnlyRzAA5InitPRzlMc" to i64),
// CHECK-SAME:                      i64 ptrtoint (ptr @"$s1A17ConditionalStructVyxGAA20WithAssocConformanceA2A8FuncOnlyRzAA5InitPRzlWP" to i64)) to i32),
// CHECK-SAME:  i32 trunc (i64 sub (i64 ptrtoint (ptr getelementptr inbounds (i8, ptr @"associated conformance 1A17ConditionalStructVyxGAA20WithAssocConformanceA2A8FuncOnlyRzAA5InitPRzl0D4TypeAaEP_AaF", i64 1) to i64),
// CHECK-SAME:                      i64 ptrtoint (ptr getelementptr inbounds ([4 x i32], ptr @"$s1A17ConditionalStructVyxGAA20WithAssocConformanceA2A8FuncOnlyRzAA5InitPRzlWP", i32 0, i32 1) to i64)) to i32)
// CHECK-SAME: i32 trunc (i64 sub (i64 ptrtoint (ptr getelementptr inbounds (i8, ptr @"symbolic x", i64 1) to i64),
// CHECK-SAME:                     i64 ptrtoint (ptr getelementptr inbounds ([4 x i32], ptr @"$s1A17ConditionalStructVyxGAA20WithAssocConformanceA2A8FuncOnlyRzAA5InitPRzlWP", i32 0, i32 2) to i64)) to i32)
// CHECK-SAME:  i32 trunc (i64 sub (i64 ptrtoint (ptr @"$s1A17ConditionalStructVyxGAA20WithAssocConformanceA2A8FuncOnlyRzAA5InitPRzlAaEP04initD00D4TypeQzyFTW" to i64),
// CHECK-SAME:                      i64 ptrtoint (ptr getelementptr inbounds ([4 x i32], ptr @"$s1A17ConditionalStructVyxGAA20WithAssocConformanceA2A8FuncOnlyRzAA5InitPRzlWP", i32 0, i32 3) to i64)) to i32)
// CHECK-SAME: ], align 8

// Make sure value witness table lookup is done right.

// CHECK-x86_64: define{{.*}} swiftcc void @"$s1A7testVWTyyxlF"(ptr {{.*}}, ptr [[T:%.*]])
// CHECK-x86_64:  [[VWT_ADDR:%.*]] = getelementptr inbounds ptr, ptr [[T]], i64 -1
// CHECK-x86_64:  [[VWT_PTR:%.*]] = load ptr, ptr [[VWT_ADDR]]
// CHECK-x86_64:  [[T1:%.*]] = getelementptr inbounds ptr, ptr [[VWT_PTR]], i32 2
// CHECK-x86_64:  [[T2:%.*]] = load ptr, ptr [[T1]]
// CHECK-x86_64:  call ptr [[T2]](ptr {{.*}}, ptr {{.*}}, ptr [[T]])


// Simple witness entry access.

// CHECK: define{{.*}} swiftcc void @"$s1A14requireWitnessyyxAA8FuncOnlyRzlF"(ptr noalias nocapture {{%.*}}, ptr {{%.*}}, ptr [[PWT:%.*]])
// CHECK:[[ENTRY:.*]]:
// CHECK:  [[T4:%.*]] = call ptr @"__swift_relative_protocol_witness_table_access_1_$s1A8FuncOnlyP1ayyFTq"(ptr [[PWT]])
// CHECK:  call{{.*}} swiftcc void [[T4]]

// CHECK: define{{.*}} hidden ptr @"__swift_relative_protocol_witness_table_access_1_$s1A8FuncOnlyP1ayyFTq"(ptr [[PWT:%.*]])
// CHECK:   [[T0:%.*]] = ptrtoint ptr [[PWT]] to i64
// CHECK:   [[T1:%.*]] = and i64 [[T0]], 1
// CHECK:   [[C:%.*]] = icmp eq i64 [[T1]], 1
// CHECK:   br i1 [[C]], label %[[LBL1:.*]], label %[[LBL2:.*]]
// CHECK:[[LBL1]]:
// CHECK:   [[T2:%.*]] = and i64 [[T0]], -2
// CHECK:   [[T3:%.*]] = inttoptr i64 [[T2]] to ptr
// CHECK:   [[T4:%.*]] = load ptr, ptr [[T3]]
// CHECK:   br label %[[LBL2]]
// CHECK:[[LBL2]]:
// CHECK:   [[T5:%.*]] = phi ptr [ [[PWT]], %[[ENTRY]] ], [ [[T4]], %[[LBL1]] ]
// CHECK-arm64e:   [[T6:%.*]] = ptrtoint ptr [[T5]] to i64
// CHECK-arm64e:   [[T7:%.*]] = call i64 @llvm.ptrauth.auth(i64 [[T6]], i32 2, i64 47152)
// CHECK-arm64e:   [[T8:%.*]] = inttoptr i64 [[T7]] to ptr
// CHECK-arm64e:   [[SLOT:%.*]] = getelementptr inbounds i32, ptr [[T8]], i32 1
// CHECK-arm64:   [[SLOT:%.*]] = getelementptr inbounds i32, ptr [[T5]], i32 1
// CHECK-x86_64:   [[SLOT:%.*]] = getelementptr inbounds i32, ptr [[T5]], i32 1
// CHECK:   [[T0:%.*]] = load i32, ptr [[SLOT]]
// CHECK:   [[T1:%.*]] = sext i32 [[T0]] to i64
// CHECK:   [[T2:%.*]] = ptrtoint ptr [[SLOT]] to i64
// CHECK:   [[T3:%.*]] = add i64 [[T2]], [[T1]]
// CHECK:   [[T4:%.*]] = inttoptr i64 [[T3]] to ptr
// CHECK:   ret ptr [[T4]]

// Parent witness entry access.

// CHECK: define hidden swiftcc void @"$s1A15requireWitness2yyxAA9InheritedRzlF"(ptr noalias nocapture {{%.*}}, ptr {{%.*}}, ptr [[T_INHERITED:%.*]])
// CHECK: [[T_FUNCONLY:%.*]] = call ptr @__swift_relative_protocol_witness_table_parent_1(ptr [[T_INHERITED]])
// CHECK: call ptr @"__swift_relative_protocol_witness_table_access_1_$s1A8FuncOnlyP1ayyFTq"(ptr [[T_FUNCONLY]])

// CHECK: define{{.*}} hidden ptr @__swift_relative_protocol_witness_table_parent_1(ptr [[T_INHERITED:%.*]])
// CHECK:  [[T0:%.*]] = ptrtoint ptr [[T_INHERITED]] to i64
// CHECK:  [[T1:%.*]] = and i64 [[T0]], 1
// CHECK:  [[T2:%.*]] = icmp eq i64 [[T1]], 1
// CHECK:  br i1 [[T2]], label %[[L1:.*]], label %[[L2:.*]]

// CHECK:[[L1]]:
// CHECK:  [[T3:%.*]] = and i64 [[T0]], -2
// CHECK:  [[T4:%.*]] = inttoptr i64 [[T3]] to ptr
// CHECK:  [[T5:%.*]] = getelementptr inbounds ptr, ptr [[T4]], i32 1
// CHECK:  [[T6:%.*]] = load ptr, ptr [[T5]]
// CHECK:  br label %[[L3:.*]]

// CHECK:[[L2]]:
// CHECK-arm64e:  [[P0:%.*]] = ptrtoint ptr [[T_INHERITED]] to i64
// CHECK-arm64e:  [[P1:%.*]] = call i64 @llvm.ptrauth.auth(i64 [[P0]], i32 2, i64 47152)
// CHECK-arm64e:  [[T_INHERITED:%.*]] = inttoptr i64 [[P1]] to ptr
// CHECK:  [[T9:%.*]] = getelementptr inbounds i32, ptr [[T_INHERITED]], i32 1
// CHECK:  [[T10:%.*]] = load i32, ptr [[T9]]
// CHECK:  [[T11:%.*]] = sext i32 [[T10]] to i64
// CHECK:  [[T12:%.*]] = ptrtoint ptr [[T9]] to i64
// CHECK:  [[T13:%.*]] = add i64 [[T12]], [[T11]]
// CHECK:  [[T14:%.*]] = inttoptr i64 [[T13]] to ptr
// CHECK-arm64e:  [[T16:%.*]] = ptrtoint ptr [[T14]] to i64
// CHECK-arm64e:  [[T17:%.*]] = call i64 @llvm.ptrauth.sign(i64 [[T16]], i32 2, i64 47152)
// CHECK-arm64e:  [[T14:%.*]] = inttoptr i64 [[T17]] to ptr
// CHECK:  br label %[[L3:.*]]

// CHECK:[[L3]]:
// CHECK:  phi ptr [ [[T6]], %[[L1]] ], [ [[T14]], %[[L2]] ]

// Passing the witness table.

// CHECK: define{{.*}} swiftcc void @"$s1A6useIt2yyF"()
// CHECK:   call swiftcc void @"$s1A15requireWitness2yyxAA9InheritedRzlF"(ptr {{.*}}, ptr {{.*}}, ptr @"$s1A7BStructVMf"{{.*}}, ptr @"$s1A7BStructVAA9InheritedAAWP{{(.ptrauth)?}}"
// CHECK:   ret void

// Accessing an associated witness
// TODO: we will probably end up calling a different entry point
// CHECK: define{{.*}} swiftcc void @"$s1A15requireWitness3yyxAA9WithAssocRzlF"(
// CHECK:   call{{.*}} swiftcc %swift.metadata_response @swift_getAssociatedTypeWitnessRelative(
// CHECK:   ret void

// CHECK: define{{.*}} swiftcc void @"$s1A15requireWitness4yyxAA20WithAssocConformanceRzlF"(ptr {{.*}}, ptr [[TYPE:%.*]], ptr [[PWT:%.*]])
// CHECK:  [[RES:%.*]] = call{{.*}} swiftcc %swift.metadata_response @swift_getAssociatedTypeWitnessRelative(i64 0, ptr [[PWT]], ptr [[TYPE]]
// CHECK:  [[ASSOCTYPE:%.*]] = extractvalue %swift.metadata_response [[RES]], 0
// CHECK:  call{{.*}} swiftcc ptr @swift_getAssociatedConformanceWitnessRelative(ptr [[PWT]], ptr [[TYPE]], ptr [[ASSOCTYPE]]

// Conditional conformance witness table entry.

// CHECK: define{{.*}} swiftcc void @"$s1A17ConditionalStructVyxGAA20WithAssocConformanceA2A8FuncOnlyRzAA5InitPRzlAaEP04initD00D4TypeQzyFTW"(ptr {{.*}}, ptr {{.*}}, ptr [[SELF:%.*]], ptr [[SELFWITNESSTBL:%.*]])
// CHECK: entry:
// CHECK:   [[C0:%.*]] = ptrtoint ptr [[SELFWITNESSTBL]] to i64
// CHECK:   [[C1:%.*]] = and i64 [[C0]], -2
// CHECK:   [[T1:%.*]] = inttoptr i64 [[C1]] to ptr
// CHECK:   [[T2:%.*]] = getelementptr inbounds ptr, ptr [[T1]], i32 -1
// CHECK:   [[T3:%.*]] = load ptr, ptr [[T2]]
// CHECK:   [[T4:%.*]] = ptrtoint ptr [[SELFWITNESSTBL]] to i64
// CHECK:   [[T5:%.*]] = and i64 [[T4]], -2
// CHECK:   [[T6:%.*]] = inttoptr i64 [[T5]] to ptr
// CHECK:   [[T7:%.*]] = getelementptr inbounds ptr, ptr [[T6]], i32 -2
// CHECK:   [[T8:%.*]] = load ptr, ptr [[T7]]
// CHECK:   [[T16:%.*]] = getelementptr inbounds ptr, ptr [[SELF]], i64 2
// CHECK:   [[T:%.*]] = load ptr, ptr [[T16]]
// CHECK:   call{{.*}} swiftcc void @"$s1A17ConditionalStructVA2A8FuncOnlyRzAA5InitPRzlE9initAssocxyF"(ptr {{.*}}, ptr [[T]], ptr [[T3]], ptr [[T8]])


// CHECK: define{{.*}} swiftcc void @"$s1A39instantiate_conditional_conformance_2ndyyxAA3SubRz1SQzRszlF"(ptr {{.*}}, ptr [[T:%.*]], ptr [[TSUB:%.*]])
// CHECK:  [[CONDBUFFER:%.*]] = alloca [1 x ptr]
// CHECK:  [[T0:%.*]] = getelementptr inbounds [1 x ptr], ptr [[CONDBUFFER]], i32 0, i32 0
// CHECK:  [[T1:%.*]] = getelementptr inbounds ptr, ptr [[T0]], i32 0
// CHECK:  store ptr [[TSUB]], ptr [[T1]]
// CHECK:  call ptr @swift_getWitnessTableRelative({{.*}}@"$s1A1XVyxGAA3SubA2aERz1SQzRszlMc{{.*}}, ptr {{.*}}, ptr [[T0]])


// CHECK: define{{.*}} void @"$s1A1XVyxGAA3SubA2aERz1SQzRszlWI"(ptr [[C0:%.*]], ptr {{.*}}, ptr [[C1:%.*]])
// CHECK: entry:
// CHECK:   [[C2:%.*]] = alloca [1 x ptr]
// CHECK:   [[T3:%.*]] = getelementptr inbounds ptr, ptr [[C1]], i32 0
// CHECK:   [[T4:%.*]] = load ptr, ptr [[T3]]
// CHECK:   [[TBASE:%.*]] = call ptr @__swift_relative_protocol_witness_table_parent_1(ptr [[T4]])
// CHECK:   [[T24:%.*]] = getelementptr inbounds [1 x ptr], ptr [[C2]], i32 0, i32 0
// CHECK:   [[T25:%.*]] = getelementptr inbounds ptr, ptr [[T24]], i32 0
// CHECK:   store ptr [[TBASE]], ptr [[T25]]
// CHECK:   [[T26:%.*]] = call ptr @swift_getWitnessTableRelative({{.*}}@"$s1A1XVyxGAA4BaseA2aERzlMc{{.*}}, ptr {{.*}}, ptr [[T24]])
// CHECK:   [[T28:%.*]] = getelementptr inbounds ptr, ptr [[C0]], i32 1
// CHECK:   store ptr [[T26]], ptr [[T28]]
