public func takes_p1<T: P1>(_: T.Type) {}
public protocol P1 {
  func normal()
  func generic<T: P3>(_: T)
}
public protocol P2 {}
public protocol P3 {}

public struct IsP2: P2 {}
public struct IsP3: P3 {}


public struct Single<A> {}
extension Single: P1 where A: P2 {
  public func normal() {}
  public func generic<T: P3>(_: T) {}
}

// witness method for Single.normal

// CHECK-LABEL: define linkonce_odr hidden swiftcc void @"$s42conditional_conformance_basic_conformances6SingleVyxGAA2P1A2A2P2RzlAaEP6normalyyFTW"(ptr noalias nocapture swiftself %0, ptr %Self, ptr %SelfWitnessTable)
// CHECK-NEXT:  entry:
// CHECK-NEXT:    [[A_P2_PTR:%.*]] = getelementptr inbounds ptr, ptr %SelfWitnessTable, i32 -1
// CHECK-NEXT:    [[A_P2_i8star:%.*]] = load ptr, ptr [[A_P2_PTR]], align 8
// CHECK-NEXT:    [[A_PTR:%.*]] = getelementptr inbounds ptr, ptr %Self, i64 2
// CHECK-NEXT:    [[A:%.*]] = load ptr, ptr [[A_PTR]], align 8
// CHECK-NEXT:    call swiftcc void @"$s42conditional_conformance_basic_conformances6SingleVA2A2P2RzlE6normalyyF"(ptr [[A]], ptr [[A_P2_i8star]])
// CHECK-NEXT:    ret void
// CHECK-NEXT:  }

// witness method for Single.generic

// CHECK-LABEL: define linkonce_odr hidden swiftcc void @"$s42conditional_conformance_basic_conformances6SingleVyxGAA2P1A2A2P2RzlAaEP7genericyyqd__AA2P3Rd__lFTW"(ptr noalias nocapture %0, ptr %"\CF\84_1_0", ptr %"\CF\84_1_0.P3", ptr noalias nocapture swiftself %1, ptr %Self, ptr %SelfWitnessTable)
// CHECK-NEXT:  entry:
// CHECK-NEXT:    [[A_P2_PTR:%.*]] = getelementptr inbounds ptr, ptr %SelfWitnessTable, i32 -1
// CHECK-NEXT:    [[A_P2_i8star:%.*]] = load ptr, ptr [[A_P2_PTR]], align 8
// CHECK-NEXT:    [[A_PTR:%.*]] = getelementptr inbounds ptr, ptr %Self, i64 2
// CHECK-NEXT:    [[A:%.*]] = load ptr, ptr [[A_PTR]], align 8
// CHECK-NEXT:    call swiftcc void @"$s42conditional_conformance_basic_conformances6SingleVA2A2P2RzlE7genericyyqd__AA2P3Rd__lF"(ptr noalias nocapture %0, ptr [[A]], ptr %"\CF\84_1_0", ptr [[A_P2_i8star]], ptr %"\CF\84_1_0.P3")
// CHECK-NEXT:    ret void
// CHECK-NEXT:  }

public func single_generic<T: P2>(_: T.Type) {
  takes_p1(Single<T>.self)
}
// CHECK-LABEL: define{{( dllexport)?}}{{( protected)?}} swiftcc void @"$s42conditional_conformance_basic_conformances14single_genericyyxmAA2P2RzlF"(ptr %0, ptr %T, ptr %T.P2)
// CHECK-NEXT:  entry:
// CHECK:         %conditional.requirement.buffer = alloca [1 x ptr], align 8
// CHECK:         [[T0:%.*]] = call swiftcc %swift.metadata_response @"$s42conditional_conformance_basic_conformances6SingleVMa"(i64 0, ptr %T)
// CHECK-NEXT:    [[Single_TYPE:%.*]] = extractvalue %swift.metadata_response [[T0]], 0
// CHECK-NEXT:    [[CONDITIONAL_REQUIREMENTS:%.*]] = getelementptr inbounds [1 x ptr], ptr %conditional.requirement.buffer, i32 0, i32 0
// CHECK-NEXT:    [[T_P2_PTR:%.*]] = getelementptr inbounds ptr, ptr [[CONDITIONAL_REQUIREMENTS]], i32 0
// CHECK-NEXT:    store ptr %T.P2, ptr [[T_P2_PTR]], align 8
// CHECK-NEXT:    [[Single_P1:%.*]] = call ptr @swift_getWitnessTable
// CHECK-NEXT:    call swiftcc void @"$s42conditional_conformance_basic_conformances8takes_p1yyxmAA2P1RzlF"(ptr [[Single_TYPE]], ptr [[Single_TYPE]], ptr [[Single_P1]])
// CHECK-NEXT:    ret void
// CHECK-NEXT:  }


public func single_concrete() {
  takes_p1(Single<IsP2>.self)
}
// CHECK-LABEL: define{{( dllexport)?}}{{( protected)?}} swiftcc void @"$s42conditional_conformance_basic_conformances15single_concreteyyF"()
// CHECK-NEXT:  entry:
// CHECK-NEXT:    [[Single_P1:%.*]] = call ptr @"$s42conditional_conformance_basic_conformances6SingleVyAA4IsP2VGACyxGAA2P1A2A0G0RzlWl"()
// CHECK-NEXT:    call swiftcc void @"$s42conditional_conformance_basic_conformances8takes_p1yyxmAA2P1RzlF"(
// CHECK-SAME:      ptr getelementptr inbounds (
// CHECK-SAME:        %swift.full_type,
// CHECK-SAME:        ptr @"$s42conditional_conformance_basic_conformances6SingleVyAA4IsP2VGMf"
// CHECK-SAME:        i32 0,
// CHECK-SAME:        i32 2
// CHECK-SAME:      ),
// CHECK-SAME:      ptr getelementptr inbounds (
// CHECK-SAME:        %swift.full_type,
// CHECK-SAME:        ptr @"$s42conditional_conformance_basic_conformances6SingleVyAA4IsP2VGMf"
// CHECK-SAME:        i32 0,
// CHECK-SAME:        i32 2
// CHECK-SAME:      ),
// CHECK-SAME:      ptr [[Single_P1]]
// CHECK-SAME:    )
// CHECK-NEXT:    ret void
// CHECK-NEXT:  }


// Lazy witness table accessor for the concrete Single<IsP2> : P1.

// CHECK-LABEL: define linkonce_odr hidden ptr @"$s42conditional_conformance_basic_conformances6SingleVyAA4IsP2VGACyxGAA2P1A2A0G0RzlWl"()
// CHECK-NEXT:  entry:
// CHECK-NEXT:    %conditional.requirement.buffer = alloca [1 x ptr], align 8
// CHECK-NEXT:    [[CACHE:%.*]] = load ptr, ptr @"$s42conditional_conformance_basic_conformances6SingleVyAA4IsP2VGACyxGAA2P1A2A0G0RzlWL", align 8
// CHECK-NEXT:    [[IS_NULL:%.*]] = icmp eq ptr [[CACHE]], null
// CHECK-NEXT:    br i1 [[IS_NULL]], label %cacheIsNull, label %cont

// CHECK:       cacheIsNull:
// CHECK-NEXT:    [[CONDITIONAL_REQUIREMENTS:%.*]] = getelementptr inbounds [1 x ptr], ptr %conditional.requirement.buffer, i32 0, i32 0
// CHECK-NEXT:    [[A_P2_PTR:%.*]] = getelementptr inbounds ptr, ptr [[CONDITIONAL_REQUIREMENTS]], i32 0
// CHECK-NEXT:    store ptr @"$s42conditional_conformance_basic_conformances4IsP2VAA0F0AAWP", ptr [[A_P2_PTR]], align 8

// CHECK-NEXT:    [[Single_P1:%.*]] = call ptr @swift_getWitnessTable(
// CHECK-SAME:        ptr
// CHECK-SAME:        @"$s42conditional_conformance_basic_conformances6SingleVyxGAA2P1A2A2P2RzlMc",
// CHECK-SAME:      ptr getelementptr inbounds (
// CHECK-SAME:        %swift.full_type,
// CHECK-SAME:          ptr @"$s42conditional_conformance_basic_conformances6SingleVyAA4IsP2VGMf"
// CHECK-SAME:        i32 0,
// CHECK-SAME:        i32 2
// CHECK-SAME:      ),
// CHECK-SAME:      ptr [[CONDITIONAL_REQUIREMENTS]]
// CHECK-SAME:    )
// CHECK-NEXT:    store atomic ptr [[Single_P1]], ptr @"$s42conditional_conformance_basic_conformances6SingleVyAA4IsP2VGACyxGAA2P1A2A0G0RzlWL" release, align 8
// CHECK-NEXT:    br label %cont

// CHECK:       cont:
// CHECK-NEXT:    [[T0:%.*]] = phi ptr [ [[CACHE]], %entry ], [ [[Single_P1]], %cacheIsNull ]
// CHECK-NEXT:    ret ptr [[T0]]
// CHECK-NEXT:  }

// TYPEBYNAME-LABEL: define linkonce_odr hidden ptr @"$s42conditional_conformance_basic_conformances6SingleVyAA4IsP2VGACyxGAA2P1A2A0G0RzlWl"()
// TYPEBYNAME-NEXT:  entry:
// TYPEBYNAME-NEXT:    %conditional.requirement.buffer = alloca [1 x ptr], align 8
// TYPEBYNAME-NEXT:    [[CACHE:%.*]] = load ptr, ptr @"$s42conditional_conformance_basic_conformances6SingleVyAA4IsP2VGACyxGAA2P1A2A0G0RzlWL", align 8
// TYPEBYNAME-NEXT:    [[IS_NULL:%.*]] = icmp eq ptr [[CACHE]], null
// TYPEBYNAME-NEXT:    br i1 [[IS_NULL]], label %cacheIsNull, label %cont

// TYPEBYNAME:       cacheIsNull:
// TYPEBYNAME-NEXT:    [[T0:%.*]] = call ptr @__swift_instantiateConcreteTypeFromMangledNameAbstract(ptr @"$s42conditional_conformance_basic_conformances6SingleVyAA4IsP2VGMD")
// TYPEBYNAME-NEXT:    [[CONDITIONAL_REQUIREMENTS:%.*]] = getelementptr inbounds [1 x ptr], ptr %conditional.requirement.buffer, i32 0, i32 0
// TYPEBYNAME-NEXT:    [[A_P2_PTR:%.*]] = getelementptr inbounds ptr, ptr [[CONDITIONAL_REQUIREMENTS]], i32 0
// TYPEBYNAME-NEXT:    store ptr @"$s42conditional_conformance_basic_conformances4IsP2VAA0F0AAWP", ptr [[A_P2_PTR]], align 8

// TYPEBYNAME-NEXT:    [[Single_P1:%.*]] = call ptr @swift_getWitnessTable
// TYPEBYNAME-NEXT:    store atomic ptr [[Single_P1]], ptr @"$s42conditional_conformance_basic_conformances6SingleVyAA4IsP2VGACyxGAA2P1A2A0G0RzlWL" release, align 8
// TYPEBYNAME-NEXT:    br label %cont

// TYPEBYNAME:       cont:
// TYPEBYNAME-NEXT:    [[T0:%.*]] = phi ptr [ [[CACHE]], %entry ], [ [[Single_P1]], %cacheIsNull ]
// TYPEBYNAME-NEXT:    ret ptr [[T0]]
// TYPEBYNAME-NEXT:  }

// TYPEBYNAME_PRESPECIALIZED-LABEL: define linkonce_odr hidden ptr @"$s42conditional_conformance_basic_conformances6SingleVyAA4IsP2VGACyxGAA2P1A2A0G0RzlWl"()
// TYPEBYNAME_PRESPECIALIZED-NEXT:  entry:
// TYPEBYNAME_PRESPECIALIZED-NEXT:    %conditional.requirement.buffer = alloca [1 x ptr], align 8
// TYPEBYNAME_PRESPECIALIZED-NEXT:    [[CACHE:%.*]] = load ptr, ptr @"$s42conditional_conformance_basic_conformances6SingleVyAA4IsP2VGACyxGAA2P1A2A0G0RzlWL", align 8
// TYPEBYNAME_PRESPECIALIZED-NEXT:    [[IS_NULL:%.*]] = icmp eq ptr [[CACHE]], null
// TYPEBYNAME_PRESPECIALIZED-NEXT:    br i1 [[IS_NULL]], label %cacheIsNull, label %cont

// TYPEBYNAME_PRESPECIALIZED:       cacheIsNull:
// TYPEBYNAME_PRESPECIALIZED-NEXT:    [[CONDITIONAL_REQUIREMENTS:%.*]] = getelementptr inbounds [1 x ptr], ptr %conditional.requirement.buffer, i32 0, i32 0
// TYPEBYNAME_PRESPECIALIZED-NEXT:    [[A_P2_PTR:%.*]] = getelementptr inbounds ptr, ptr [[CONDITIONAL_REQUIREMENTS]], i32 0
// TYPEBYNAME_PRESPECIALIZED-NEXT:    store ptr @"$s42conditional_conformance_basic_conformances4IsP2VAA0F0AAWP", ptr [[A_P2_PTR]], align 8

// TYPEBYNAME_PRESPECIALIZED-NEXT:    [[Single_P1:%.*]] = call ptr @swift_getWitnessTable
// TYPEBYNAME_PRESPECIALIZED-NEXT:    store atomic ptr [[Single_P1]], ptr @"$s42conditional_conformance_basic_conformances6SingleVyAA4IsP2VGACyxGAA2P1A2A0G0RzlWL" release, align 8
// TYPEBYNAME_PRESPECIALIZED-NEXT:    br label %cont

// TYPEBYNAME_PRESPECIALIZED:       cont:
// TYPEBYNAME_PRESPECIALIZED-NEXT:    [[T0:%.*]] = phi ptr [ [[CACHE]], %entry ], [ [[Single_P1]], %cacheIsNull ]
// TYPEBYNAME_PRESPECIALIZED-NEXT:    ret ptr [[T0]]
// TYPEBYNAME_PRESPECIALIZED-NEXT:  }


public struct Double<B, C> {}
extension Double: P1 where B: P2, C: P3 {
  public func normal() {}
  public func generic<T: P3>(_: T) {}
}

// witness method for Double.normal

// CHECK-LABEL: define linkonce_odr hidden swiftcc void @"$s42conditional_conformance_basic_conformances6DoubleVyxq_GAA2P1A2A2P2RzAA2P3R_rlAaEP6normalyyFTW"(ptr noalias nocapture swiftself %0, ptr %Self, ptr %SelfWitnessTable)
// CHECK-NEXT:  entry:
// CHECK-NEXT:    [[B_P2_PTR:%.*]] = getelementptr inbounds ptr, ptr %SelfWitnessTable, i32 -1
// CHECK-NEXT:    [[B_P2_i8star:%.*]] = load ptr, ptr [[B_P2_PTR]], align 8

// CHECK-NEXT:    [[C_P3_PTR:%.*]] = getelementptr inbounds ptr, ptr %SelfWitnessTable, i32 -2
// CHECK-NEXT:    [[C_P3_i8star:%.*]] = load ptr, ptr [[C_P3_PTR]], align 8

// CHECK-NEXT:    [[B_PTR:%.*]] = getelementptr inbounds ptr, ptr %Self, i64 2
// CHECK-NEXT:    [[B:%.*]] = load ptr, ptr [[B_PTR]], align 8

// CHECK-NEXT:    [[C_PTR:%.*]] = getelementptr inbounds ptr, ptr %Self, i64 3
// CHECK-NEXT:    [[C:%.*]] = load ptr, ptr [[C_PTR]], align 8

// CHECK-NEXT:    call swiftcc void @"$s42conditional_conformance_basic_conformances6DoubleVA2A2P2RzAA2P3R_rlE6normalyyF"(ptr [[B]], ptr [[C]], ptr [[B_P2_i8star]], ptr [[C_P3_i8star]])
// CHECK-NEXT:    ret void
// CHECK-NEXT:  }

// witness method for Double.generic

// CHECK-LABEL: define linkonce_odr hidden swiftcc void @"$s42conditional_conformance_basic_conformances6DoubleVyxq_GAA2P1A2A2P2RzAA2P3R_rlAaEP7genericyyqd__AaGRd__lFTW"(ptr noalias nocapture %0, ptr %"\CF\84_1_0", ptr %"\CF\84_1_0.P3", ptr noalias nocapture swiftself %1, ptr %Self, ptr %SelfWitnessTable)
// CHECK-NEXT:  entry:

// CHECK-NEXT:    [[B_P2_PTR:%.*]] = getelementptr inbounds ptr, ptr %SelfWitnessTable, i32 -1
// CHECK-NEXT:    [[B_P2_i8star:%.*]] = load ptr, ptr [[B_P2_PTR]], align 8

// CHECK-NEXT:    [[C_P3_PTR:%.*]] = getelementptr inbounds ptr, ptr %SelfWitnessTable, i32 -2
// CHECK-NEXT:    [[C_P3_i8star:%.*]] = load ptr, ptr [[C_P3_PTR]], align 8

// CHECK-NEXT:    [[B_PTR:%.*]] = getelementptr inbounds ptr, ptr %Self, i64 2
// CHECK-NEXT:    [[B:%.*]] = load ptr, ptr [[B_PTR]], align 8

// CHECK-NEXT:    [[C_PTR:%.*]] = getelementptr inbounds ptr, ptr %Self, i64 3
// CHECK-NEXT:    [[C:%.*]] = load ptr, ptr [[C_PTR]], align 8

// CHECK-NEXT:    call swiftcc void @"$s42conditional_conformance_basic_conformances6DoubleVA2A2P2RzAA2P3R_rlE7genericyyqd__AaERd__lF"(ptr noalias nocapture %0, ptr [[B]], ptr [[C]], ptr %"\CF\84_1_0", ptr [[B_P2_i8star]], ptr [[C_P3_i8star]], ptr %"\CF\84_1_0.P3")
// CHECK-NEXT:    ret void
// CHECK-NEXT:  }


public func double_generic_generic<U: P2, V: P3>(_: U.Type, _: V.Type) {
  takes_p1(Double<U, V>.self)
}
// CHECK-LABEL: define{{( dllexport)?}}{{( protected)?}} swiftcc void @"$s42conditional_conformance_basic_conformances015double_generic_F0yyxm_q_mtAA2P2RzAA2P3R_r0_lF"(ptr %0, ptr %1, ptr %U, ptr %V, ptr %U.P2, ptr %V.P3)
// CHECK-NEXT:  entry:
// CHECK:          %conditional.requirement.buffer = alloca [2 x ptr], align 8
// CHECK:         [[T0:%.*]] = call swiftcc %swift.metadata_response @"$s42conditional_conformance_basic_conformances6DoubleVMa"(i64 0, ptr %U, ptr %V)
// CHECK-NEXT:    [[Double_TYPE:%.*]] = extractvalue %swift.metadata_response [[T0]], 0

// CHECK-NEXT:    [[CONDITIONAL_REQUIREMENTS:%.*]] = getelementptr inbounds [2 x ptr], ptr %conditional.requirement.buffer, i32 0, i32 0
// CHECK-NEXT:    [[B_P2_PTR:%.*]] = getelementptr inbounds ptr, ptr [[CONDITIONAL_REQUIREMENTS]], i32 0
// CHECK-NEXT:    store ptr %U.P2, ptr [[B_P2_PTR]], align 8
// CHECK-NEXT:    [[C_P3_PTR:%.*]] = getelementptr inbounds ptr, ptr [[CONDITIONAL_REQUIREMENTS]], i32 1
// CHECK-NEXT:    store ptr %V.P3, ptr [[C_P3_PTR]], align 8

// CHECK-NEXT:    [[Double_P1:%.*]] = call ptr @swift_getWitnessTable
// CHECK-NEXT:    call swiftcc void @"$s42conditional_conformance_basic_conformances8takes_p1yyxmAA2P1RzlF"(ptr [[Double_TYPE]], ptr [[Double_TYPE]], ptr [[Double_P1]])
// CHECK-NEXT:    ret void
// CHECK-NEXT:  }

public func double_generic_concrete<X: P2>(_: X.Type) {
  takes_p1(Double<X, IsP3>.self)
}
// CHECK-LABEL: define{{( dllexport)?}}{{( protected)?}} swiftcc void @"$s42conditional_conformance_basic_conformances23double_generic_concreteyyxmAA2P2RzlF"(ptr %0, ptr %X, ptr %X.P2)
// CHECK-NEXT:  entry:
// CHECK:         %conditional.requirement.buffer = alloca [2 x ptr], align 8
// CHECK:    [[Double_TYPE_Response:%[0-9]+]] = call swiftcc %swift.metadata_response @"$s42conditional_conformance_basic_conformances6DoubleVMa"(
// CHECK-SAME:      i64 0,
// CHECK-SAME:      ptr
// CHECK-SAME:      %X,
// CHECK-SAME:        ptr getelementptr inbounds (
// CHECK-SAME:          <{
// CHECK-SAME:            ptr,
// CHECK-SAME:            ptr,
// CHECK-SAME:            i64,
// CHECK-SAME:            ptr,
// CHECK-SAME:            i64
// CHECK-SAME:          }>,
// CHECK-SAME:          ptr @"$s42conditional_conformance_basic_conformances4IsP3VMf",
// CHECK-SAME:          i32 0,
// CHECK-SAME:          i32 2
// CHECK-SAME:      )
// CHECK-SAME:    )
// CHECK:    [[Double_TYPE:%[0-9]+]] = extractvalue %swift.metadata_response [[Double_TYPE_Response]], 0
// CHECK:    [[CONDITIONAL_REQUIREMENTS:%.*]] = getelementptr inbounds [2 x ptr], ptr %conditional.requirement.buffer, i32 0, i32 0
// CHECK-NEXT:    [[B_P2_PTR:%.*]] = getelementptr inbounds ptr, ptr [[CONDITIONAL_REQUIREMENTS]], i32 0
// CHECK-NEXT:    store ptr %X.P2, ptr [[B_P2_PTR]], align 8
// CHECK-NEXT:    [[C_P3_PTR:%.*]] = getelementptr inbounds ptr, ptr [[CONDITIONAL_REQUIREMENTS]], i32 1
// CHECK-NEXT:    store ptr @"$s42conditional_conformance_basic_conformances4IsP3VAA0F0AAWP", ptr [[C_P3_PTR]], align 8

// CHECK-NEXT:    [[Double_P1:%.*]] = call ptr @swift_getWitnessTable(
// CHECK-SAME:        ptr
// CHECK-SAME:        @"$s42conditional_conformance_basic_conformances6DoubleVyxq_GAA2P1A2A2P2RzAA2P3R_rlMc"
// CHECK-SAME:      ptr %{{[0-9]+}},
// CHECK-SAME:      ptr [[CONDITIONAL_REQUIREMENTS]]
// CHECK-SAME:    )

// CHECK-NEXT:    call swiftcc void @"$s42conditional_conformance_basic_conformances8takes_p1yyxmAA2P1RzlF"(
// CHECK-SAME:      ptr [[Double_TYPE]], 
// CHECK-SAME:      ptr [[Double_TYPE]], 
// CHECK-SAME:      ptr [[Double_P1]]
// CHECK-SAME:    )
// CHECK-NEXT:    ret void
// CHECK-NEXT:  }

public func double_concrete_concrete() {
  takes_p1(Double<IsP2, IsP3>.self)
}
// CHECK-LABEL: define{{( dllexport)?}}{{( protected)?}} swiftcc void @"$s42conditional_conformance_basic_conformances016double_concrete_F0yyF"()
// CHECK-NEXT:  entry:
// CHECK-NEXT:    [[Double_P1:%.*]] = call ptr @"$s42conditional_conformance_basic_conformances6DoubleVyAA4IsP2VAA0F2P3VGACyxq_GAA2P1A2A0G0RzAA0H0R_rlWl"()
// CHECK-NEXT:    call swiftcc void @"$s42conditional_conformance_basic_conformances8takes_p1yyxmAA2P1RzlF"(
// CHECK-SAME:      ptr getelementptr inbounds (
// CHECK-SAME:        %swift.full_type,
// CHECK-SAME:        ptr @"$s42conditional_conformance_basic_conformances6DoubleVyAA4IsP2VAA0F2P3VGMf"
// CHECK-SAME:        i32 0,
// CHECK-SAME:        i32 2
// CHECK-SAME:      ),
// CHECK-SAME:      ptr getelementptr inbounds (
// CHECK-SAME:        %swift.full_type,
// CHECK-SAME:        ptr @"$s42conditional_conformance_basic_conformances6DoubleVyAA4IsP2VAA0F2P3VGMf"
// CHECK-SAME:        i32 0,
// CHECK-SAME:        i32 2
// CHECK-SAME:      ),
// CHECK-SAME:      ptr [[Double_P1]]
// CHECK-SAME:    )
// CHECK-NEXT:    ret void
// CHECK-NEXT:  }

// Lazy witness table accessor for the concrete Double<IsP2, IsP3> : P1.

// CHECK-LABEL: define linkonce_odr hidden ptr @"$s42conditional_conformance_basic_conformances6DoubleVyAA4IsP2VAA0F2P3VGACyxq_GAA2P1A2A0G0RzAA0H0R_rlWl"()
// CHECK-NEXT:  entry:
// CHECK-NEXT:    %conditional.requirement.buffer = alloca [2 x ptr], align 8
// CHECK-NEXT:    [[CACHE:%.*]] = load ptr, ptr @"$s42conditional_conformance_basic_conformances6DoubleVyAA4IsP2VAA0F2P3VGACyxq_GAA2P1A2A0G0RzAA0H0R_rlWL", align 8
// CHECK-NEXT:    [[IS_NULL:%.*]] = icmp eq ptr [[CACHE]], null
// CHECK-NEXT:    br i1 [[IS_NULL]], label %cacheIsNull, label %cont

// CHECK:       cacheIsNull:
// CHECK-NEXT:    [[CONDITIONAL_REQUIREMENTS:%.*]] = getelementptr inbounds [2 x ptr], ptr %conditional.requirement.buffer, i32 0, i32 0
// CHECK-NEXT:    [[B_P2_PTR:%.*]] = getelementptr inbounds ptr, ptr [[CONDITIONAL_REQUIREMENTS]], i32 0
// CHECK-NEXT:    store ptr @"$s42conditional_conformance_basic_conformances4IsP2VAA0F0AAWP", ptr [[B_P2_PTR]], align 8
// CHECK-NEXT:    [[C_P3_PTR:%.*]] = getelementptr inbounds ptr, ptr [[CONDITIONAL_REQUIREMENTS]], i32 1
// CHECK-NEXT:    store ptr @"$s42conditional_conformance_basic_conformances4IsP3VAA0F0AAWP", ptr [[C_P3_PTR]], align 8

// CHECK-NEXT:    [[Double_P1:%.*]] = call ptr @swift_getWitnessTable(
// CHECK-SAME:        ptr
// CHECK-SAME:        @"$s42conditional_conformance_basic_conformances6DoubleVyxq_GAA2P1A2A2P2RzAA2P3R_rlMc"
// CHECK-SAME:      ptr getelementptr inbounds (
// CHECK-SAME:        %swift.full_type,
// CHECK-SAME:        ptr  @"$s42conditional_conformance_basic_conformances6DoubleVyAA4IsP2VAA0F2P3VGMf"
// CHECK-SAME:        i32 0,
// CHECK-SAME:        i32 2
// CHECK-SAME:      ),
// CHECK-SAME:      ptr [[CONDITIONAL_REQUIREMENTS]]
// CHECK-SAME:    )
// CHECK-NEXT:    store atomic ptr [[Double_P1]], ptr @"$s42conditional_conformance_basic_conformances6DoubleVyAA4IsP2VAA0F2P3VGACyxq_GAA2P1A2A0G0RzAA0H0R_rlWL" release, align 8
// CHECK-NEXT:    br label %cont

// CHECK:       cont:
// CHECK-NEXT:    [[T0:%.*]] = phi ptr [ [[CACHE]], %entry ], [ [[Double_P1]], %cacheIsNull ]
// CHECK-NEXT:    ret ptr [[T0]]
// CHECK-NEXT:  }


func dynamicCastToP1(_ value: Any) -> P1? {
  return value as? P1
}

// https://github.com/apple/swift/issues/49649

protocol P4 {}
typealias P4Typealias = P4
protocol P5 {}

struct S_49649<T> {}
extension S_49649 : P5 where T == P4Typealias {}
