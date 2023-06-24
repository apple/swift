public func takes_p1<T: P1>(_: T.Type) {}
public protocol P1 {
  associatedtype AT1

  func normal()
  func generic<T: P3>(_: T)
}
public protocol P2 {
  associatedtype AT2: P3
}
public protocol P3 {
  associatedtype AT3
}

public struct Nothing {}

public struct IsP2: P2 {
  public typealias AT2 = IsP3
}
public struct IsP3: P3 {
  public typealias AT3 = Nothing
}

public struct IsAlsoP2: P2 {
  public typealias AT2 = IsBoth
}
public struct IsBoth: P2, P3 {
  public typealias AT2 = HoldsP3
  public typealias AT3 = Nothing
}
public struct HoldsP3: P3 {
  public typealias AT3 = IsP3
}

public struct Double<B: P2, C> {}
extension Double: P1 where B.AT2: P2, C: P3, B.AT2.AT2.AT3: P3 {
  public typealias AT1 = C
  public func normal() {}
  public func generic<T: P3>(_: T) {}
}

// witness method for Double.normal

// CHECK-LABEL: define linkonce_odr hidden swiftcc void @"$s34conditional_conformance_with_assoc6DoubleVyxq_GAA2P1A2A2P3R_AA2P23AT2RpzAafH_AhaGP3AT3RPzrlAaEP6normalyyFTW"(ptr noalias nocapture swiftself %0, ptr %Self, ptr %SelfWitnessTable)
// CHECK-NEXT:  entry:
// CHECK-NEXT:    [[C_P3_PTR:%.*]] = getelementptr inbounds ptr, ptr %SelfWitnessTable, i32 -1
// CHECK-NEXT:    [[C_P3:%.*]] = load ptr, ptr [[C_P3_PTR]], align 8

// CHECK-NEXT:    [[B_AT2_P2_PTR:%.*]] = getelementptr inbounds ptr, ptr %SelfWitnessTable, i32 -2
// CHECK-NEXT:    [[B_AT2_P2:%.*]] = load ptr, ptr [[B_AT2_P2_PTR]], align 8

// CHECK-NEXT:    [[B_AT2_AT2_AT3_P3_PTR:%.*]] = getelementptr inbounds ptr, ptr %SelfWitnessTable, i32 -3
// CHECK-NEXT:    [[B_AT2_AT2_AT3_P3:%.*]] = load ptr, ptr [[B_AT2_AT2_AT3_P3_PTR]], align 8

// CHECK-NEXT:    [[B_PTR:%.*]] = getelementptr inbounds ptr, ptr %Self, i64 2
// CHECK-NEXT:    [[B:%.*]] = load ptr, ptr [[B_PTR]], align 8

// CHECK-NEXT:    [[C_PTR:%.*]] = getelementptr inbounds ptr, ptr %Self, i64 3
// CHECK-NEXT:    [[C:%.*]] = load ptr, ptr [[C_PTR]], align 8

// CHECK-NEXT:    [[B_P2_PTR:%.*]] = getelementptr inbounds ptr, ptr %Self, i64 4
// CHECK-NEXT:    %"\CF\84_0_0.P2" = load ptr, ptr [[B_P2_PTR]], align 8

// CHECK-NEXT:    call swiftcc void @"$s34conditional_conformance_with_assoc6DoubleVA2A2P3R_AA2P23AT2RpzAadF_AfaEP3AT3RPzrlE6normalyyF"(ptr %"\CF\84_0_0", ptr %"\CF\84_0_1", ptr %"\CF\84_0_0.P2", ptr [[C_P3]], ptr [[B_AT2_P2]], ptr [[B_AT2_AT2_AT3_P3]])
// CHECK-NEXT:    ret void
// CHECK-NEXT:  }

// witness method for Double.generic

// CHECK-LABEL: define linkonce_odr hidden swiftcc void @"$s34conditional_conformance_with_assoc6DoubleVyxq_GAA2P1A2A2P3R_AA2P23AT2RpzAafH_AhaGP3AT3RPzrlAaEP7genericyyqd__AaFRd__lFTW"(ptr noalias nocapture %0, ptr %"\CF\84_1_0", ptr %"\CF\84_1_0.P3", ptr noalias nocapture swiftself %1, ptr %Self, ptr %SelfWitnessTable)
// CHECK-NEXT:  entry:
// CHECK-NEXT:    [[C_P3_PTR:%.*]] = getelementptr inbounds ptr, ptr %SelfWitnessTable, i32 -1
// CHECK-NEXT:    [[C_P3:%.*]] = load ptr, ptr [[C_P3_PTR]], align 8

// CHECK-NEXT:    [[B_AT2_P2_PTR:%.*]] = getelementptr inbounds ptr, ptr %SelfWitnessTable, i32 -2
// CHECK-NEXT:    [[B_AT2_P2:%.*]] = load ptr, ptr [[B_AT2_P2_PTR]], align 8

// CHECK-NEXT:    [[B_AT2_AT2_AT3_P3_PTR:%.*]] = getelementptr inbounds ptr, ptr %SelfWitnessTable, i32 -3
// CHECK-NEXT:    [[B_AT2_AT2_AT3_P3:%.*]] = load ptr, ptr [[B_AT2_AT2_AT3_P3_PTR]], align 8

// CHECK-NEXT:    [[B_PTR:%.*]] = getelementptr inbounds ptr, ptr %Self, i64 2
// CHECK-NEXT:    [[B:%.*]] = load ptr, ptr [[B_PTR]], align 8

// CHECK-NEXT:    [[C_PTR:%.*]] = getelementptr inbounds ptr, ptr %Self, i64 3
// CHECK-NEXT:    [[C:%.*]] = load ptr, ptr [[C_PTR]], align 8

// CHECK-NEXT:    [[B_P2_PTR:%.*]] = getelementptr inbounds ptr, ptr %Self, i64 4
// CHECK-NEXT:    %"\CF\84_0_0.P2" = load ptr, ptr [[B_P2_PTR]], align 8

// CHECK-NEXT:    call swiftcc void @"$s34conditional_conformance_with_assoc6DoubleVA2A2P3R_AA2P23AT2RpzAadF_AfaEP3AT3RPzrlE7genericyyqd__AaDRd__lF"(ptr noalias nocapture %0, ptr %"\CF\84_0_0", ptr %"\CF\84_0_1", ptr %"\CF\84_1_0", ptr %"\CF\84_0_0.P2", ptr [[C_P3]], ptr %"\CF\84_1_0.P3", ptr [[B_AT2_P2]], ptr [[B_AT2_AT2_AT3_P3]])
// CHECK-NEXT:    ret void
// CHECK-NEXT:  }


public func generic_generic<T: P2, U>(_: T.Type, _: U.Type)
  where T.AT2: P2, U: P3, T.AT2.AT2.AT3: P3
{
  takes_p1(Double<T, U>.self)
}
// CHECK-LABEL: define{{( dllexport)?}}{{( protected)?}} swiftcc void @"$s34conditional_conformance_with_assoc08generic_E0yyxm_q_mtAA2P2RzAA2P3R_AaC3AT2RpzAadE_AeaCP3AT3RPzr0_lF"(ptr %0, ptr %1, ptr %T, ptr %U, ptr %T.P2, ptr %U.P3, ptr %T.AT2.P2, ptr %T.AT2.AT2.AT3.P3)
// CHECK-NEXT:  entry:
// CHECK:         %conditional.requirement.buffer = alloca [3 x ptr], align 8
// CHECK:         [[T0:%.*]] = call swiftcc %swift.metadata_response @"$s34conditional_conformance_with_assoc6DoubleVMa"(i64 0, ptr %T, ptr %U, ptr %T.P2)
// CHECK-NEXT:    [[Double_TYPE:%.*]] = extractvalue %swift.metadata_response [[T0]], 0

// CHECK-NEXT:    [[CONDITIONAL_REQUIREMENTS:%.*]] = getelementptr inbounds [3 x ptr], ptr %conditional.requirement.buffer, i32 0, i32 0
// CHECK-NEXT:    [[C_P3_PTR:%.*]] = getelementptr inbounds ptr, ptr [[CONDITIONAL_REQUIREMENTS]], i32 0
// CHECK-NEXT:    store ptr %U.P3, ptr [[C_P3_PTR]], align 8
// CHECK-NEXT:    [[B_AT2_P2_PTR:%.*]] = getelementptr inbounds ptr, ptr [[CONDITIONAL_REQUIREMENTS]], i32 1
// CHECK-NEXT:    store ptr %T.AT2.P2, ptr [[B_AT2_P2_PTR]], align 8
// CHECK-NEXT:    [[B_AT2_AT2_AT3_P3_PTR:%.*]] = getelementptr inbounds ptr, ptr [[CONDITIONAL_REQUIREMENTS]], i32 2
// CHECK-NEXT:    store ptr %T.AT2.AT2.AT3.P3, ptr [[B_AT2_AT2_AT3_P3_PTR]], align 8

// CHECK-NEXT:    [[Double_P1:%.*]] = call ptr @swift_getWitnessTable
// CHECK-NEXT:    call swiftcc void @"$s34conditional_conformance_with_assoc8takes_p1yyxmAA2P1RzlF"(ptr [[Double_TYPE]], ptr [[Double_TYPE]], ptr [[Double_P1]])
// CHECK-NEXT:    ret void
// CHECK-NEXT:  }

public func generic_concrete<T: P2>(_: T.Type)
  where T.AT2: P2, T.AT2.AT2.AT3: P3
{
  takes_p1(Double<T, IsP3>.self)
}
// CHECK-LABEL: define{{( dllexport)?}}{{( protected)?}} swiftcc void @"$s34conditional_conformance_with_assoc16generic_concreteyyxmAA2P2RzAaC3AT2RpzAA2P3AD_AdaCP3AT3RPzlF"(ptr %0, ptr %T, ptr %T.P2, ptr %T.AT2.P2, ptr %T.AT2.AT2.AT3.P3)
// CHECK-NEXT:  entry:
// CHECK:         %conditional.requirement.buffer = alloca [3 x ptr], align 8
// CHECK:         [[T0:%.*]] = call swiftcc %swift.metadata_response @"$s34conditional_conformance_with_assoc6DoubleVMa"(
// CHECK-SAME:           i64 0,
// CHECK-SAME:           ptr
// CHECK-SAME:           %T,
// CHECK-SAME:             ptr getelementptr inbounds (
// CHECK-SAME:               <{
// CHECK-SAME:                 ptr,
// CHECK-SAME:                 i64,
// CHECK-SAME:                 ptr
// CHECK-SAME:               }>,
// CHECK-SAME:               ptr @"$s34conditional_conformance_with_assoc4IsP3VMf",
// CHECK-SAME:               i32 0,
// CHECK-SAME:               i32 2
// CHECK-SAME:           ),
// CHECK-SAME:           ptr %T.P2
// CHECK-SAME:         )
// CHECK-NEXT:    [[Double_TYPE:%.*]] = extractvalue %swift.metadata_response [[T0]], 0

// CHECK-NEXT:    [[CONDITIONAL_REQUIREMENTS:%.*]] = getelementptr inbounds [3 x ptr], ptr %conditional.requirement.buffer, i32 0, i32 0
// CHECK-NEXT:    [[C_P3_PTR:%.*]] = getelementptr inbounds ptr, ptr [[CONDITIONAL_REQUIREMENTS]], i32 0
// CHECK-NEXT:    store ptr @"$s34conditional_conformance_with_assoc4IsP3VAA0F0AAWP", ptr [[C_P3_PTR]], align 8
// CHECK-NEXT:    [[B_AT2_P2_PTR:%.*]] = getelementptr inbounds ptr, ptr [[CONDITIONAL_REQUIREMENTS]], i32 1
// CHECK-NEXT:    store ptr %T.AT2.P2, ptr [[B_AT2_P2_PTR]], align 8
// CHECK-NEXT:    [[B_AT2_AT2_AT3_P3_PTR:%.*]] = getelementptr inbounds ptr, ptr [[CONDITIONAL_REQUIREMENTS]], i32 2
// CHECK-NEXT:    store ptr %T.AT2.AT2.AT3.P3, ptr [[B_AT2_AT2_AT3_P3_PTR]], align 8

// CHECK-NEXT:    [[Double_P1:%.*]] = call ptr @swift_getWitnessTable
// CHECK-NEXT:    call swiftcc void @"$s34conditional_conformance_with_assoc8takes_p1yyxmAA2P1RzlF"(ptr [[Double_TYPE]], ptr [[Double_TYPE]], ptr [[Double_P1]])
// CHECK-NEXT:    ret void
// CHECK-NEXT:  }


public func concrete_generic<U>(_: U.Type)
  where U: P3
{
  takes_p1(Double<IsAlsoP2, U>.self)
}

// CHECK-LABEL: define{{( dllexport)?}}{{( protected)?}} swiftcc void @"$s34conditional_conformance_with_assoc16concrete_genericyyxmAA2P3RzlF"(ptr %0, ptr %U, ptr %U.P3)
// CHECK-NEXT:  entry:
// CHECK:       %conditional.requirement.buffer = alloca [3 x ptr], align 8
// CHECK:       [[T0:%.*]] = call swiftcc %swift.metadata_response @"$s34conditional_conformance_with_assoc6DoubleVMa"(i64 0, ptr getelementptr inbounds (<{ {{.*}} }>, ptr @"$s34conditional_conformance_with_assoc8IsAlsoP2VMf", i32 0, i32 2), ptr %U, ptr @"$s34conditional_conformance_with_assoc8IsAlsoP2VAA0G0AAWP")
// CHECK-NEXT:  [[Double_TYPE:%.*]] = extractvalue %swift.metadata_response [[T0]], 0
// CHECK-NEXT:  [[CONDITIONAL_REQUIREMENTS:%.*]] = getelementptr inbounds [3 x ptr], ptr %conditional.requirement.buffer, i32 0, i32 0
// CHECK-NEXT:  [[C_P3_PTR:%.*]] = getelementptr inbounds ptr, ptr [[CONDITIONAL_REQUIREMENTS]], i32 0
// CHECK-NEXT:  store ptr %U.P3, ptr [[C_P3_PTR]], align 8
// CHECK-NEXT:  [[B_AT2_P2_PTR:%.*]] = getelementptr inbounds ptr, ptr [[CONDITIONAL_REQUIREMENTS]], i32 1
// CHECK-NEXT:  store ptr @"$s34conditional_conformance_with_assoc6IsBothVAA2P2AAWP", ptr [[B_AT2_P2_PTR]], align 8
// CHECK-NEXT:  [[B_AT2_AT2_AT3_P3_PTR:%.*]] = getelementptr inbounds ptr, ptr [[CONDITIONAL_REQUIREMENTS]], i32 2
// CHECK-NEXT:  store ptr @"$s34conditional_conformance_with_assoc4IsP3VAA0F0AAWP", ptr [[B_AT2_AT2_AT3_P3_PTR]], align 8
// CHECK-NEXT:  [[Double_P1:%.*]] = call ptr @swift_getWitnessTable
// CHECK-NEXT:  call swiftcc void @"$s34conditional_conformance_with_assoc8takes_p1yyxmAA2P1RzlF"(ptr [[Double_TYPE]], ptr [[Double_TYPE]], ptr [[Double_P1]])
// CHECK-NEXT:  ret void
// CHECK-NEXT:}


public func concrete_concrete() {
  takes_p1(Double<IsAlsoP2, IsP3>.self)
}

// CHECK-LABEL: define{{( dllexport)?}}{{( protected)?}} swiftcc void @"$s34conditional_conformance_with_assoc09concrete_E0yyF"()
// CHECK-NEXT:  entry:
// CHECK-NEXT:    [[X:%.*]] = call {{.*}} @__swift_instantiateConcreteTypeFromMangledName({{.*}} @"$s34conditional_conformance_with_assoc6DoubleVyAA8IsAlsoP2VAA0F2P3VGMD")
// CHECK-NEXT:    [[Z:%.*]] = call ptr @"$s34conditional_conformance_with_assoc6DoubleVyAA8IsAlsoP2VAA0F2P3VGACyxq_GAA2P1A2A0I0R_AA0H03AT2RpzAakM_AmaLP3AT3RPzrlWl"()
// CHECK-NEXT:    call swiftcc void @"$s34conditional_conformance_with_assoc8takes_p1yyxmAA2P1RzlF"(ptr [[X]], ptr [[X]], ptr [[Z]])
// CHECK-NEXT:    ret void
// CHECK-NEXT: }

// Lazy witness table accessor for the concrete Double<IsAlsoP2, IsP3> : P1.

// CHECK-LABEL: define linkonce_odr hidden ptr @"$s34conditional_conformance_with_assoc6DoubleVyAA8IsAlsoP2VAA0F2P3VGACyxq_GAA2P1A2A0I0R_AA0H03AT2RpzAakM_AmaLP3AT3RPzrlWl"()
// CHECK-NEXT:  entry:
// CHECK-NEXT:    %conditional.requirement.buffer = alloca [3 x ptr], align 8
// CHECK-NEXT:    [[CACHE:%.*]] = load ptr, ptr @"$s34conditional_conformance_with_assoc6DoubleVyAA8IsAlsoP2VAA0F2P3VGACyxq_GAA2P1A2A0I0R_AA0H03AT2RpzAakM_AmaLP3AT3RPzrlWL", align 8
// CHECK-NEXT:    [[IS_NULL:%.*]] = icmp eq ptr [[CACHE]], null
// CHECK-NEXT:    br i1 [[IS_NULL]], label %cacheIsNull, label %cont

// CHECK:       cacheIsNull:
// CHECK-STABLE-ABI-FALSE-NEXT:   [[T0:%.*]] = call swiftcc %swift.metadata_response @"$s34conditional_conformance_with_assoc6DoubleVyAA8IsAlsoP2VAA0F2P3VGMa"(i64 255)
// CHECK-STABLE-ABI-FALSE-NEXT:   [[Double_TYPE:%.*]] = extractvalue %swift.metadata_response [[T0]], 0
// CHECK-STABLE-ABI-FALSE-NEXT:   extractvalue %swift.metadata_response [[T0]], 1
// CHECK-STABLE-ABI-TRUE-NEXT:    [[T0:%.*]] = call ptr @__swift_instantiateConcreteTypeFromMangledNameAbstract(ptr @"$s34conditional_conformance_with_assoc6DoubleVyAA8IsAlsoP2VAA0F2P3VGMD")
// CHECK-NEXT:    [[CONDITIONAL_REQUIREMENTS:%.*]] = getelementptr inbounds [3 x ptr], ptr %conditional.requirement.buffer, i32 0, i32 0
// CHECK-NEXT:    [[C_P3_PTR:%.*]] = getelementptr inbounds ptr, ptr [[CONDITIONAL_REQUIREMENTS]], i32 0
// CHECK-NEXT:    store ptr @"$s34conditional_conformance_with_assoc4IsP3VAA0F0AAWP", ptr [[C_P3_PTR]], align 8
// CHECK-NEXT:    [[B_AT2_P2_PTR:%.*]] = getelementptr inbounds ptr, ptr [[CONDITIONAL_REQUIREMENTS]], i32 1
// CHECK-NEXT:    store ptr @"$s34conditional_conformance_with_assoc6IsBothVAA2P2AAWP", ptr [[B_AT2_P2_PTR]], align 8
// CHECK-NEXT:    [[B_AT2_AT2_AT3_P3_PTR:%.*]] = getelementptr inbounds ptr, ptr [[CONDITIONAL_REQUIREMENTS]], i32 2
// CHECK-NEXT:    store ptr @"$s34conditional_conformance_with_assoc4IsP3VAA0F0AAWP", ptr [[B_AT2_AT2_AT3_P3_PTR]], align 8
// CHECK-NEXT:    [[Double_P1:%.*]] = call ptr @swift_getWitnessTable
// CHECK-NEXT:    store atomic ptr [[Double_P1]], ptr @"$s34conditional_conformance_with_assoc6DoubleVyAA8IsAlsoP2VAA0F2P3VGACyxq_GAA2P1A2A0I0R_AA0H03AT2RpzAakM_AmaLP3AT3RPzrlWL" release, align 8
// CHECK-NEXT:    br label %cont

// CHECK:       cont:
// CHECK-NEXT:    [[T0:%.*]] = phi ptr [ [[CACHE]], %entry ], [ [[Double_P1]], %cacheIsNull ]
// CHECK-NEXT:    ret ptr [[T0]]
// CHECK-NEXT:  }



protocol Base {
}

protocol Sub : Base {
  associatedtype S : Base
}

struct X<T> {
}

extension X: Base where T: Base { }
extension X: Sub where T: Sub, T.S == T {
   typealias S = X<T>
}
