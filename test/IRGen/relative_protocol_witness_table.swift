// RUN: %target-swift-frontend %use_no_opaque_pointers -enable-relative-protocol-witness-tables -module-name A -primary-file %s -emit-ir | %FileCheck %s --check-prefix=CHECK-%target-cpu --check-prefix=CHECK
// RUN: %target-swift-frontend -enable-relative-protocol-witness-tables -module-name A -primary-file %s -emit-ir

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
// CHECK-SAME: [i32 trunc (i64 sub (i64 ptrtoint (%swift.protocol_conformance_descriptor* @"$s1A7AStructVAA8FuncOnlyAAMc" to i64),
// CHECK-SAME:                      i64 ptrtoint ([3 x i32]* @"$s1A7AStructVAA8FuncOnlyAAWP" to i64)) to i32),
// CHECK-SAME:  i32 trunc (i64 sub (i64 ptrtoint (void (%T1A7AStructV*, %swift.type*, i8**)* @"$s1A7AStructVAA8FuncOnlyA2aDP1ayyFTW" to i64),
// CHECK-SAME:                      i64 ptrtoint (i32* getelementptr inbounds ([3 x i32], [3 x i32]* @"$s1A7AStructVAA8FuncOnlyAAWP", i32 0, i32 1) to i64)) to i32),
// CHECK-SAME:  i32 trunc (i64 sub (i64 ptrtoint (void (%T1A7AStructV*, %swift.type*, i8**)* @"$s1A7AStructVAA8FuncOnlyA2aDP1byyFTW" to i64),
// CHECK-SAME:                      i64 ptrtoint (i32* getelementptr inbounds ([3 x i32], [3 x i32]* @"$s1A7AStructVAA8FuncOnlyAAWP", i32 0, i32 2) to i64)) to i32)
// CHECK-SAME: ], align 8

// Simple Table with parent.

// CHECK: @"$s1A7BStructVAA9InheritedAAWP" = hidden constant [3 x i32]
// CHECK-SAME: [i32 trunc (i64 sub (i64 ptrtoint (%swift.protocol_conformance_descriptor* @"$s1A7BStructVAA9InheritedAAMc" to i64),
// CHECK-SAME:                      i64 ptrtoint ([3 x i32]* @"$s1A7BStructVAA9InheritedAAWP" to i64)) to i32),
// CHECK-SAME:  i32 trunc (i64 sub (i64 ptrtoint ([3 x i32]* @"$s1A7BStructVAA8FuncOnlyAAWP" to i64),
// CHECK-SAME:                      i64 ptrtoint (i32* getelementptr inbounds ([3 x i32], [3 x i32]* @"$s1A7BStructVAA9InheritedAAWP", i32 0, i32 1) to i64)) to i32),
// CHECK-SAME   i32 trunc (i64 sub (i64 ptrtoint (void (%T1A7BStructV*, %swift.type*, i8**)* @"$s1A7BStructVAA9InheritedA2aDP1cyyFTW" to i64),
// CHECK-SAME:                      i64 ptrtoint (i32* getelementptr inbounds ([3 x i32], [3 x i32]* @"$s1A7BStructVAA9InheritedAAWP", i32 0, i32 2) to i64)) to i32)
// CHECK-SAME: ], align 8


// Simple associated type conformance.

// CHECK: @"$s1A7CStructVAA9WithAssocAAWP" = hidden constant [3 x i32]
// CHECK-SAME: [i32 trunc (i64 sub (i64 ptrtoint (%swift.protocol_conformance_descriptor* @"$s1A7CStructVAA9WithAssocAAMc" to i64),
// CHECK-SAME:                      i64 ptrtoint ([3 x i32]* @"$s1A7CStructVAA9WithAssocAAWP" to i64)) to i32),
// CHECK-SAME:  i32 trunc (i64 sub (i64 ptrtoint (i8* getelementptr inbounds (<{ [2 x i8], i8 }>, <{ [2 x i8], i8 }>* @"symbolic Si", i32 0, i32 0, i64 1) to i64),
// CHECK-SAME:                      i64 ptrtoint (i32* getelementptr inbounds ([3 x i32], [3 x i32]* @"$s1A7CStructVAA9WithAssocAAWP", i32 0, i32 1) to i64)) to i32),
// CHECK-SAME:  i32 trunc (i64 sub (i64 ptrtoint (void (%T1A7CStructV*, %swift.type*, i8**)* @"$s1A7CStructVAA9WithAssocA2aDP1ayyFTW" to i64),
// CHECK-SAME:                      i64 ptrtoint (i32* getelementptr inbounds ([3 x i32], [3 x i32]* @"$s1A7CStructVAA9WithAssocAAWP", i32 0, i32 2) to i64)) to i32)
// CHECK-SAME: ], align 8

// CHECK: @"$s1A7DStructVAA20WithAssocConformanceAAWP" = hidden constant [4 x i32]
// CHECK-SAME: [i32 trunc (i64 sub (i64 ptrtoint (%swift.protocol_conformance_descriptor* @"$s1A7DStructVAA20WithAssocConformanceAAMc" to i64),
// CHECK-SAME:                      i64 ptrtoint ([4 x i32]* @"$s1A7DStructVAA20WithAssocConformanceAAWP" to i64)) to i32),
// CHECK-SAME:  i32 trunc (i64 sub (i64 ptrtoint (i8* getelementptr (i8, i8* getelementptr inbounds (<{ i8, i8, i32, i8 }>, <{ i8, i8, i32, i8 }>* @"associated conformance 1A7DStructVAA20WithAssocConformanceAA0C4TypeAaDP_AA8FuncOnly", i32 0, i32 0), i64 1) to i64),
// CHECK-SAME:                      i64 ptrtoint (i32* getelementptr inbounds ([4 x i32], [4 x i32]* @"$s1A7DStructVAA20WithAssocConformanceAAWP", i32 0, i32 1) to i64)) to i32),
// CHECK-SAME:  i32 trunc (i64 sub (i64 ptrtoint (i8* getelementptr inbounds (i8, i8* getelementptr inbounds (<{ i8, i32, i8 }>, <{ i8, i32, i8 }>* @"symbolic _____ 1A7AStructV", i32 0, i32 0), i64 1) to i64),
// CHECK-SAME:                      i64 ptrtoint (i32* getelementptr inbounds ([4 x i32], [4 x i32]* @"$s1A7DStructVAA20WithAssocConformanceAAWP", i32 0, i32 2) to i64)) to i32),
// CHECK-SAME:  i32 trunc (i64 sub (i64 ptrtoint (void (%T1A7AStructV*, %T1A7DStructV*, %swift.type*, i8**)* @"$s1A7DStructVAA20WithAssocConformanceA2aDP04initC00C4TypeQzyFTW" to i64),
// CHECK-SAME:                      i64 ptrtoint (i32* getelementptr inbounds ([4 x i32], [4 x i32]* @"$s1A7DStructVAA20WithAssocConformanceAAWP", i32 0, i32 3) to i64)) to i32)
// CHECK-SAME: ], align 8

// CHECK: @"$s1A7GStructVyxGAA20WithAssocConformanceAAWP" = hidden constant [4 x i32]
// CHECK-SAME: [i32 trunc (i64 sub (i64 ptrtoint ({ i32, i32, i32, i32, i16, i16, i32, i32 }* @"$s1A7GStructVyxGAA20WithAssocConformanceAAMc" to i64),
// CHECK-SAME:                      i64 ptrtoint ([4 x i32]* @"$s1A7GStructVyxGAA20WithAssocConformanceAAWP" to i64)) to i32),
// CHECK-SAME:  i32 trunc (i64 sub (i64 ptrtoint (i8* getelementptr (i8, i8* getelementptr inbounds (<{ i8, i8, i32, i8 }>, <{ i8, i8, i32, i8 }>* @"associated conformance 1A7GStructVyxGAA20WithAssocConformanceAA0C4TypeAaEP_AA8FuncOnly", i32 0, i32 0), i64 1) to i64),
// CHECK-SAME:                      i64 ptrtoint (i32* getelementptr inbounds ([4 x i32], [4 x i32]* @"$s1A7GStructVyxGAA20WithAssocConformanceAAWP", i32 0, i32 1) to i64)) to i32),
// CHECK-SAME:  i32 trunc (i64 sub (i64 ptrtoint (i8* getelementptr inbounds (<{ [1 x i8], i8 }>, <{ [1 x i8], i8 }>* @"symbolic x", i32 0, i32 0, i64 1) to i64),
// CHECK-SAME:                      i64 ptrtoint (i32* getelementptr inbounds ([4 x i32], [4 x i32]* @"$s1A7GStructVyxGAA20WithAssocConformanceAAWP", i32 0, i32 2) to i64)) to i32),
// CHECK-SAME:  i32 trunc (i64 sub (i64 ptrtoint (void (%swift.opaque*, %T1A7GStructV*, %swift.type*, i8**)* @"$s1A7GStructVyxGAA20WithAssocConformanceA2aEP04initC00C4TypeQzyFTW" to i64),
// CHECK-SAME:                      i64 ptrtoint (i32* getelementptr inbounds ([4 x i32], [4 x i32]* @"$s1A7GStructVyxGAA20WithAssocConformanceAAWP", i32 0, i32 3) to i64)) to i32)
// CHECK-SAME: ], align 8

// Conditional conformance

// CHECK: @"$s1A17ConditionalStructVyxGAA20WithAssocConformanceA2A8FuncOnlyRzAA5InitPRzlWP" = hidden constant [4 x i32]
// CHECK-SAME: [i32 trunc (i64 sub (i64 ptrtoint ({ i32, i32, i32, i32, i32, i32, i32, i32, i32, i32, i16, i16, i32, i32 }* @"$s1A17ConditionalStructVyxGAA20WithAssocConformanceA2A8FuncOnlyRzAA5InitPRzlMc" to i64),
// CHECK-SAME:                      i64 ptrtoint ([4 x i32]* @"$s1A17ConditionalStructVyxGAA20WithAssocConformanceA2A8FuncOnlyRzAA5InitPRzlWP" to i64)) to i32),
// CHECK-SAME:  i32 trunc (i64 sub (i64 ptrtoint (i8* getelementptr (i8, i8* getelementptr inbounds (<{ i8, i8, i32, i8 }>, <{ i8, i8, i32, i8 }>* @"associated conformance 1A17ConditionalStructVyxGAA20WithAssocConformanceA2A8FuncOnlyRzAA5InitPRzl0D4TypeAaEP_AaF", i32 0, i32 0), i64 1) to i64),
// CHECK-SAME:                      i64 ptrtoint (i32* getelementptr inbounds ([4 x i32], [4 x i32]* @"$s1A17ConditionalStructVyxGAA20WithAssocConformanceA2A8FuncOnlyRzAA5InitPRzlWP", i32 0, i32 1) to i64)) to i32),
// CHECK-SAME:  i32 trunc (i64 sub (i64 ptrtoint (i8* getelementptr inbounds (<{ [1 x i8], i8 }>, <{ [1 x i8], i8 }>* @"symbolic x", i32 0, i32 0, i64 1) to i64),
// CHECK-SAME:                      i64 ptrtoint (i32* getelementptr inbounds ([4 x i32], [4 x i32]* @"$s1A17ConditionalStructVyxGAA20WithAssocConformanceA2A8FuncOnlyRzAA5InitPRzlWP", i32 0, i32 2) to i64)) to i32),
// CHECK-SAME:  i32 trunc (i64 sub (i64 ptrtoint (void (%swift.opaque*, %T1A17ConditionalStructV*, %swift.type*, i8**)* @"$s1A17ConditionalStructVyxGAA20WithAssocConformanceA2A8FuncOnlyRzAA5InitPRzlAaEP04initD00D4TypeQzyFTW" to i64),
// CHECK-SAME:                      i64 ptrtoint (i32* getelementptr inbounds ([4 x i32], [4 x i32]* @"$s1A17ConditionalStructVyxGAA20WithAssocConformanceA2A8FuncOnlyRzAA5InitPRzlWP", i32 0, i32 3) to i64)) to i32)
// CHECK-SAME: ], align 8

// Make sure value witness table lookup is done right.

// CHECK-x86_64: define{{.*}} swiftcc void @"$s1A7testVWTyyxlF"(%swift.opaque* {{.*}}, %swift.type* [[T:%.*]])
// CHECK-x86_64:  [[T0:%.*]] = bitcast %swift.type* [[T]] to i8***
// CHECK-x86_64:  [[VWT_ADDR:%.*]] = getelementptr inbounds i8**, i8*** [[T0]], i64 -1
// CHECK-x86_64:  [[VWT_PTR:%.*]] = load i8**, i8*** [[VWT_ADDR]]
// CHECK-x86_64:  [[T1:%.*]] = getelementptr inbounds i8*, i8** [[VWT_PTR]], i32 2
// CHECK-x86_64:  [[T2:%.*]] = load i8*, i8** [[T1]]
// CHECK-x86_64:  [[T3:%.*]] = bitcast i8* [[T2]] to %swift.opaque* (%swift.opaque*, %swift.opaque*, %swift.type*)*
// CHECK-x86_64:  call %swift.opaque* [[T3]](%swift.opaque* {{.*}}, %swift.opaque* {{.*}}, %swift.type* [[T]])


// Simple witness entry access.

// CHECK: define{{.*}} swiftcc void @"$s1A14requireWitnessyyxAA8FuncOnlyRzlF"(%swift.opaque* noalias nocapture {{%.*}}, %swift.type* {{%.*}}, i8** [[PWT:%.*]])
// CHECK:[[ENTRY:.*]]:
// CHECK:  [[T4:%.*]] = call i8* @"__swift_relative_protocol_witness_table_access_1_$s1A8FuncOnlyP1ayyFTq"(i8** [[PWT]])
// CHECK:  [[T5:%.*]] = bitcast i8* [[T4]] to void (%swift.opaque*, %swift.type*, i8**)*
// CHECK:  call{{.*}} swiftcc void [[T5]]

// CHECK: define{{.*}} hidden i8* @"__swift_relative_protocol_witness_table_access_1_$s1A8FuncOnlyP1ayyFTq"(i8** [[PWT:%.*]])
// CHECK:   [[T0:%.*]] = ptrtoint i8** [[PWT]] to i64
// CHECK:   [[T1:%.*]] = and i64 [[T0]], 1
// CHECK:   [[C:%.*]] = icmp eq i64 [[T1]], 1
// CHECK:   br i1 [[C]], label %[[LBL1:.*]], label %[[LBL2:.*]]
// CHECK:[[LBL1]]:
// CHECK:   [[T2:%.*]] = and i64 [[T0]], -2
// CHECK:   [[T3:%.*]] = inttoptr i64 [[T2]] to i8***
// CHECK:   [[T4:%.*]] = load i8**, i8*** [[T3]]
// CHECK:   br label %[[LBL2]]
// CHECK:[[LBL2]]:
// CHECK:   [[T5:%.*]] = phi i8** [ [[PWT]], %[[ENTRY]] ], [ [[T4]], %[[LBL1]] ]
// CHECK-arm64e:   [[T6:%.*]] = ptrtoint i8** [[T5]] to i64
// CHECK-arm64e:   [[T7:%.*]] = call i64 @llvm.ptrauth.auth(i64 [[T6]], i32 2, i64 47152)
// CHECK-arm64e:   [[T8:%.*]] = inttoptr i64 [[T7]] to i8**
// CHECK-arm64e:   [[CAST:%.*]] = bitcast i8** [[T8]] to i32*
// CHECK-arm64e:   [[SLOT:%.*]] = getelementptr inbounds i32, i32* [[CAST]], i32 1
// CHECK-arm64:   [[CAST:%.*]] = bitcast i8** [[T5]] to i32*
// CHECK-arm64:   [[SLOT:%.*]] = getelementptr inbounds i32, i32* [[CAST]], i32 1
// CHECK-x86_64:   [[CAST:%.*]] = bitcast i8** [[T5]] to i32*
// CHECK-x86_64:   [[SLOT:%.*]] = getelementptr inbounds i32, i32* [[CAST]], i32 1
// CHECK:   [[T0:%.*]] = load i32, i32* [[SLOT]]
// CHECK:   [[T1:%.*]] = sext i32 [[T0]] to i64
// CHECK:   [[T2:%.*]] = ptrtoint i32* [[SLOT]] to i64
// CHECK:   [[T3:%.*]] = add i64 [[T2]], [[T1]]
// CHECK:   [[T4:%.*]] = inttoptr i64 [[T3]] to i8*
// CHECK:   ret i8* [[T4]]

// Parent witness entry access.

// CHECK: define hidden swiftcc void @"$s1A15requireWitness2yyxAA9InheritedRzlF"(%swift.opaque* noalias nocapture {{%.*}}, %swift.type* {{%.*}}, i8** [[T_INHERITED:%.*]])
// CHECK: [[T_FUNCONLY:%.*]] = call i8** @__swift_relative_protocol_witness_table_parent_1(i8** [[T_INHERITED]])
// CHECK: call i8* @"__swift_relative_protocol_witness_table_access_1_$s1A8FuncOnlyP1ayyFTq"(i8** [[T_FUNCONLY]])

// CHECK: define{{.*}} hidden i8** @__swift_relative_protocol_witness_table_parent_1(i8** [[T_INHERITED:%.*]])
// CHECK:  [[T0:%.*]] = ptrtoint i8** [[T_INHERITED]] to i64
// CHECK:  [[T1:%.*]] = and i64 [[T0]], 1
// CHECK:  [[T2:%.*]] = icmp eq i64 [[T1]], 1
// CHECK:  br i1 [[T2]], label %[[L1:.*]], label %[[L2:.*]]

// CHECK:[[L1]]:
// CHECK:  [[T3:%.*]] = and i64 [[T0]], -2
// CHECK:  [[T4:%.*]] = inttoptr i64 [[T3]] to i8**
// CHECK:  [[T5:%.*]] = getelementptr inbounds i8*, i8** [[T4]], i32 1
// CHECK:  [[T6:%.*]] = load i8*, i8** [[T5]]
// CHECK:  [[T7:%.*]] = bitcast i8* [[T6]] to i8**
// CHECK:  br label %[[L3:.*]]

// CHECK:[[L2]]:
// CHECK-arm64e:  [[P0:%.*]] = ptrtoint i8** [[T_INHERITED]] to i64
// CHECK-arm64e:  [[P1:%.*]] = call i64 @llvm.ptrauth.auth(i64 [[P0]], i32 2, i64 47152)
// CHECK-arm64e:  [[P2:%.*]] = inttoptr i64 [[P1]] to i8**
// CHECK-arm64e:  [[T8:%.*]] = bitcast i8** [[P2]] to i32*
// CHECK-arm64:  [[T8:%.*]] = bitcast i8** [[T_INHERITED]] to i32*
// CHECK-x86_64:  [[T8:%.*]] = bitcast i8** [[T_INHERITED]] to i32*
// CHECK:  [[T9:%.*]] = getelementptr inbounds i32, i32* [[T8]], i32 1
// CHECK:  [[T10:%.*]] = load i32, i32* [[T9]]
// CHECK:  [[T11:%.*]] = sext i32 [[T10]] to i64
// CHECK:  [[T12:%.*]] = ptrtoint i32* [[T9]] to i64
// CHECK:  [[T13:%.*]] = add i64 [[T12]], [[T11]]
// CHECK:  [[T14:%.*]] = inttoptr i64 [[T13]] to i8*
// CHECK:  [[T15:%.*]] = bitcast i8* [[T14]] to i8**
// CHECK-arm64e:  [[T16:%.*]] = ptrtoint i8** [[T15]] to i64
// CHECK-arm64e:  [[T17:%.*]] = call i64 @llvm.ptrauth.sign(i64 [[T16]], i32 2, i64 47152)
// CHECK-arm64e:  [[T15:%.*]] = inttoptr i64 [[T17]] to i8**
// CHECK:  br label %[[L3:.*]]

// CHECK:[[L3]]:
// CHECK:  phi i8** [ [[T7]], %[[L1]] ], [ [[T15]], %[[L2]] ]

// Passing the witness table.

// CHECK: define{{.*}} swiftcc void @"$s1A6useIt2yyF"()
// CHECK:   call swiftcc void @"$s1A15requireWitness2yyxAA9InheritedRzlF"(%swift.opaque* {{.*}}, %swift.type* {{.*}} @"$s1A7BStructVMf"{{.*}}, i8** {{.*}} @"$s1A7BStructVAA9InheritedAAWP{{(.ptrauth)?}}"
// CHECK:   ret void

// Accessing an associated witness
// TODO: we will probably end up calling a different entry point
// CHECK: define{{.*}} swiftcc void @"$s1A15requireWitness3yyxAA9WithAssocRzlF"(
// CHECK:   call{{.*}} swiftcc %swift.metadata_response @swift_getAssociatedTypeWitnessRelative(
// CHECK:   ret void

// CHECK: define{{.*}} swiftcc void @"$s1A15requireWitness4yyxAA20WithAssocConformanceRzlF"(%swift.opaque* {{.*}}, %swift.type* [[TYPE:%.*]], i8** [[PWT:%.*]])
// CHECK:  [[RES:%.*]] = call{{.*}} swiftcc %swift.metadata_response @swift_getAssociatedTypeWitnessRelative(i64 0, i8** [[PWT]], %swift.type* [[TYPE]]
// CHECK:  [[ASSOCTYPE:%.*]] = extractvalue %swift.metadata_response [[RES]], 0
// CHECK:  call{{.*}} swiftcc i8** @swift_getAssociatedConformanceWitnessRelative(i8** [[PWT]], %swift.type* [[TYPE]], %swift.type* [[ASSOCTYPE]]

// Conditional conformance witness table entry.

// CHECK: define{{.*}} swiftcc void @"$s1A17ConditionalStructVyxGAA20WithAssocConformanceA2A8FuncOnlyRzAA5InitPRzlAaEP04initD00D4TypeQzyFTW"(%swift.opaque* {{.*}}, %T1A17ConditionalStructV* {{.*}}, %swift.type* [[SELF:%.*]], i8** [[SELFWITNESSTBL:%.*]])
// CHECK: entry:
// CHECK:   [[C0:%.*]] = ptrtoint i8** [[SELFWITNESSTBL]] to i64
// CHECK:   [[C1:%.*]] = and i64 [[C0]], -2
// CHECK:   [[T1:%.*]] = inttoptr i64 [[C1]] to i8**
// CHECK:   [[T2:%.*]] = getelementptr inbounds i8*, i8** [[T1]], i32 -1
// CHECK:   [[T3:%.*]] = load i8*, i8** [[T2]]
// CHECK:   [[FUNCONLY:%.*]] = bitcast i8* [[T3]] to i8**
// CHECK:   [[T4:%.*]] = ptrtoint i8** [[SELFWITNESSTBL]] to i64
// CHECK:   [[T5:%.*]] = and i64 [[T4]], -2
// CHECK:   [[T6:%.*]] = inttoptr i64 [[T5]] to i8**
// CHECK:   [[T7:%.*]] = getelementptr inbounds i8*, i8** [[T6]], i32 -2
// CHECK:   [[T8:%.*]] = load i8*, i8** [[T7]]
// CHECK:   [[INITP:%.*]] = bitcast i8* [[T8]] to i8**
// CHECK:   [[T15:%.*]] = bitcast %swift.type* [[SELF]] to %swift.type**
// CHECK:   [[T16:%.*]] = getelementptr inbounds %swift.type*, %swift.type** [[T15]], i64 2
// CHECK:   [[T:%.*]] = load %swift.type*, %swift.type** [[T16]]
// CHECK:   call{{.*}} swiftcc void @"$s1A17ConditionalStructVA2A8FuncOnlyRzAA5InitPRzlE9initAssocxyF"(%swift.opaque* {{.*}}, %swift.type* [[T]], i8** [[FUNCONLY]], i8** [[INITP]])


// CHECK: define{{.*}} swiftcc void @"$s1A39instantiate_conditional_conformance_2ndyyxAA3SubRz1SQzRszlF"(%swift.opaque* {{.*}}, %swift.type* [[T:%.*]], i8** [[TSUB:%.*]])
// CHECK:  [[CONDBUFFER:%.*]] = alloca [1 x i8**]
// CHECK:  [[T0:%.*]] = getelementptr inbounds [1 x i8**], [1 x i8**]* [[CONDBUFFER]], i32 0, i32 0
// CHECK:  [[T1:%.*]] = getelementptr inbounds i8**, i8*** [[T0]], i32 0
// CHECK:  store i8** [[TSUB]], i8*** [[T1]]
// CHECK:  call i8** @swift_getWitnessTableRelative({{.*}}@"$s1A1XVyxGAA3SubA2aERz1SQzRszlMc{{.*}}, %swift.type* {{.*}}, i8*** [[T0]])


// CHECK: define{{.*}} void @"$s1A1XVyxGAA3SubA2aERz1SQzRszlWI"(i8** [[C0:%.*]], %swift.type* {{.*}}, i8** [[C1:%.*]])
// CHECK: entry:
// CHECK:   [[C2:%.*]] = alloca [1 x i8**]
// CHECK:   [[T2:%.*]] = bitcast i8** [[C1]] to i8***
// CHECK:   [[T3:%.*]] = getelementptr inbounds i8**, i8*** [[T2]], i32 0
// CHECK:   [[T4:%.*]] = load i8**, i8*** [[T3]]
// CHECK:   [[TBASE:%.*]] = call i8** @__swift_relative_protocol_witness_table_parent_1(i8** [[T4]])
// CHECK:   [[T24:%.*]] = getelementptr inbounds [1 x i8**], [1 x i8**]* [[C2]], i32 0, i32 0
// CHECK:   [[T25:%.*]] = getelementptr inbounds i8**, i8*** [[T24]], i32 0
// CHECK:   store i8** [[TBASE]], i8*** [[T25]]
// CHECK:   [[T26:%.*]] = call i8** @swift_getWitnessTableRelative({{.*}}@"$s1A1XVyxGAA4BaseA2aERzlMc{{.*}}, %swift.type* {{.*}}, i8*** [[T24]])
// CHECK:   [[T27:%.*]] = bitcast i8** [[T26]] to i8*
// CHECK:   [[T28:%.*]] = getelementptr inbounds i8*, i8** [[C0]], i32 1
// CHECK:   store i8* [[T27]], i8** [[T28]]
