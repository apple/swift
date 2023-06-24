public func takes_p1<T: P1>(_: T.Type) {}
public protocol P1 {
  func normal()
  func generic<T: P3>(_: T)
}
public protocol P2 {}
public protocol P3 {}

public struct IsP2: P2 {}

public class Base<A> {}
extension Base: P1 where A: P2 {
  public func normal() {}
  public func generic<T: P3>(_: T) {}
}

// witness method for Base.normal

// CHECK-LABEL: define linkonce_odr hidden swiftcc void @"$s32conditional_conformance_subclass4BaseCyxGAA2P1A2A2P2RzlAaEP6normalyyFTW"(ptr noalias nocapture swiftself dereferenceable(8) %0, ptr %Self, ptr %SelfWitnessTable)
// CHECK-NEXT:  entry:
// CHECK-NEXT:    [[A_P2_PTR:%.*]] = getelementptr inbounds ptr, ptr %SelfWitnessTable, i32 -1
// CHECK-NEXT:    [[A_P2:%.*]] = load ptr, ptr [[A_P2_PTR]], align 8
// CHECK-NEXT:    [[SELF:%.]] = load ptr, ptr %0
// CHECK-NEXT:    call swiftcc void @"$s32conditional_conformance_subclass4BaseCA2A2P2RzlE6normalyyF"(ptr [[A_P2]], ptr swiftself [[SELF]])
// CHECK-NEXT:    ret void
// CHECK-NEXT:  }

// witness method for Base.generic

// CHECK-LABEL: define linkonce_odr hidden swiftcc void @"$s32conditional_conformance_subclass4BaseCyxGAA2P1A2A2P2RzlAaEP7genericyyqd__AA2P3Rd__lFTW"(ptr noalias nocapture %0, ptr %"\CF\84_1_0", ptr %"\CF\84_1_0.P3", ptr noalias nocapture swiftself dereferenceable(8) %1, ptr %Self, ptr %SelfWitnessTable)
// CHECK-NEXT:  entry:
// CHECK-NEXT:    [[A_P2_PTR:%.*]] = getelementptr inbounds ptr, ptr %SelfWitnessTable, i32 -1
// CHECK-NEXT:    [[A_P2:%.*]] = load ptr, ptr [[A_P2_PTR]], align 8
// CHECK-NEXT:    [[SELF:%.]] = load ptr, ptr %1, align 8
// CHECK-NEXT:    call swiftcc void @"$s32conditional_conformance_subclass4BaseCA2A2P2RzlE7genericyyqd__AA2P3Rd__lF"(ptr noalias nocapture %0, ptr %"\CF\84_1_0", ptr [[A_P2]], ptr %"\CF\84_1_0.P3", ptr swiftself [[SELF]])
// CHECK-NEXT:    ret void
// CHECK-NEXT:  }


public class SubclassGeneric<T>: Base<T> {}
public class SubclassConcrete: Base<IsP2> {}
public class SubclassGenericConcrete: SubclassGeneric<IsP2> {}

public func subclassgeneric_generic<T: P2>(_: T.Type) {
  takes_p1(SubclassGeneric<T>.self)
}

// CHECK-LABEL: define{{( dllexport| protected)?}} swiftcc void @"$s32conditional_conformance_subclass23subclassgeneric_genericyyxmAA2P2RzlF"(ptr %0, ptr %T, ptr %T.P2)
// CHECK-NEXT:  entry:
// CHECK:         %conditional.requirement.buffer = alloca [1 x ptr], align 8
// CHECK:         [[T0:%.*]] = call swiftcc %swift.metadata_response @"$s32conditional_conformance_subclass15SubclassGenericCMa"(i64 0, ptr %T)
// CHECK-NEXT:    [[SubclassGeneric_TYPE:%.*]] = extractvalue %swift.metadata_response [[T0]], 0
// CHECK-NEXT:    [[CONDITIONAL_REQUIREMENTS:%.*]] = getelementptr inbounds [1 x ptr], ptr %conditional.requirement.buffer, i32 0, i32 0
// CHECK-NEXT:    [[T_P2_PTR:%.*]] = getelementptr inbounds ptr, ptr [[CONDITIONAL_REQUIREMENTS]], i32 0
// CHECK-NEXT:    store ptr %T.P2, ptr [[T_P2_PTR]], align 8
// CHECK-NEXT:    [[Base_P1:%.*]] = call ptr @swift_getWitnessTable
// CHECK-NEXT:    call swiftcc void @"$s32conditional_conformance_subclass8takes_p1yyxmAA2P1RzlF"(ptr [[SubclassGeneric_TYPE]], ptr [[SubclassGeneric_TYPE]], ptr [[Base_P1]])
// CHECK-NEXT:    ret void
// CHECK-NEXT:  }

public func subclassgeneric_concrete() {
  takes_p1(SubclassGeneric<IsP2>.self)
}

// CHECK-LABEL: define{{( dllexport| protected)?}} swiftcc void @"$s32conditional_conformance_subclass24subclassgeneric_concreteyyF"()
// CHECK-NEXT:  entry:
// CHECK-NEXT:    [[SubclassGeneric_TYPE:%.*]] = call {{.*}}@"$s32conditional_conformance_subclass15SubclassGenericCyAA4IsP2VGMD"
// CHECK-NEXT:    [[Base_P1:%.*]] = call ptr @"$s32conditional_conformance_subclass15SubclassGenericCyAA4IsP2VGAA4BaseCyxGAA2P1A2A0G0RzlWl"()
// CHECK-NEXT:    call swiftcc void @"$s32conditional_conformance_subclass8takes_p1yyxmAA2P1RzlF"(ptr [[SubclassGeneric_TYPE]], ptr [[SubclassGeneric_TYPE]], ptr [[Base_P1]])
// CHECK-NEXT:    ret void
// CHECK-NEXT:  }

// Lazy witness table accessor for the concrete SubclassGeneric<IsP2> : Base.

// CHECK-LABEL: define linkonce_odr hidden ptr @"$s32conditional_conformance_subclass15SubclassGenericCyAA4IsP2VGAA4BaseCyxGAA2P1A2A0G0RzlWl"()
// CHECK-NEXT:  entry:
// CHECK-NEXT:    %conditional.requirement.buffer = alloca [1 x ptr], align 8
// CHECK-NEXT:    [[CACHE:%.*]] = load ptr, ptr @"$s32conditional_conformance_subclass15SubclassGenericCyAA4IsP2VGAA4BaseCyxGAA2P1A2A0G0RzlWL", align 8
// CHECK-NEXT:    [[IS_NULL:%.*]] = icmp eq ptr [[CACHE]], null
// CHECK-NEXT:    br i1 [[IS_NULL]], label %cacheIsNull, label %cont

// CHECK:       cacheIsNull:
// CHECK-STABLE-ABI-FALSE-NEXT: [[T0:%.*]] = call swiftcc %swift.metadata_response @"$s32conditional_conformance_subclass15SubclassGenericCyAA4IsP2VGMa"(i64 255)
// CHECK-STABLE-ABI-FALSE-NEXT: [[SubclassGeneric_TYPE:%.*]] = extractvalue %swift.metadata_response [[T0]], 0
// CHECK-STABLE-ABI-FALSE-NEXT: extractvalue %swift.metadata_response [[T0]], 1
// CHECK-STABLE-ABI-TRUE-NEXT:  [[T0:%.*]] = call ptr @__swift_instantiateConcreteTypeFromMangledNameAbstract(ptr @"$s32conditional_conformance_subclass15SubclassGenericCyAA4IsP2VGMD")

// CHECK-NEXT:    [[CONDITIONAL_REQUIREMENTS:%.*]] = getelementptr inbounds [1 x ptr], ptr %conditional.requirement.buffer, i32 0, i32 0
// CHECK-NEXT:    [[A_P2_PTR:%.*]] = getelementptr inbounds ptr, ptr [[CONDITIONAL_REQUIREMENTS]], i32 0
// CHECK-NEXT:    store ptr @"$s32conditional_conformance_subclass4IsP2VAA0E0AAWP", ptr [[A_P2_PTR]], align 8
// CHECK-NEXT:    [[Base_P1:%.*]] = call ptr @swift_getWitnessTable
// CHECK-NEXT:    store atomic ptr [[Base_P1]], ptr @"$s32conditional_conformance_subclass15SubclassGenericCyAA4IsP2VGAA4BaseCyxGAA2P1A2A0G0RzlWL" release, align 8
// CHECK-NEXT:    br label %cont

// CHECK:       cont:
// CHECK-NEXT:    [[T0:%.*]] = phi ptr [ [[CACHE]], %entry ], [ [[Base_P1]], %cacheIsNull ]
// CHECK-NEXT:    ret ptr [[T0]]
// CHECK-NEXT:  }

public func subclassconcrete() {
  takes_p1(SubclassConcrete.self)
}

// CHECK-LABEL: define{{( dllexport| protected)?}} swiftcc void @"$s32conditional_conformance_subclass16subclassconcreteyyF"()
// CHECK-NEXT:  entry:
// CHECK-NEXT:    [[T0:%.*]] = call swiftcc %swift.metadata_response @"$s32conditional_conformance_subclass16SubclassConcreteCMa"(i64 0)
// CHECK-NEXT:    [[SubclassConcrete_TYPE:%.*]] = extractvalue %swift.metadata_response [[T0]], 0
// CHECK-NEXT:    [[SubclassConcrete_P1:%.*]] = call ptr @"$s32conditional_conformance_subclass16SubclassConcreteCAA4BaseCyxGAA2P1A2A2P2RzlWl"()
// CHECK-NEXT:    call swiftcc void @"$s32conditional_conformance_subclass8takes_p1yyxmAA2P1RzlF"(ptr [[SubclassConcrete_TYPE]], ptr [[SubclassConcrete_TYPE]], ptr [[SubclassConcrete_P1]])
// CHECK-NEXT:    ret void
// CHECK-NEXT:  }

// CHECK-LABEL: define linkonce_odr hidden ptr @"$s32conditional_conformance_subclass16SubclassConcreteCAA4BaseCyxGAA2P1A2A2P2RzlWl"()
// CHECK-NEXT:  entry:
// CHECK-NEXT:    %conditional.requirement.buffer = alloca [1 x ptr], align 8
// CHECK-NEXT:    [[CACHE:%.*]] = load ptr, ptr @"$s32conditional_conformance_subclass16SubclassConcreteCAA4BaseCyxGAA2P1A2A2P2RzlWL", align 8
// CHECK-NEXT:    [[IS_NULL:%.*]] = icmp eq ptr [[CACHE]], null
// CHECK-NEXT:    br i1 [[IS_NULL]], label %cacheIsNull, label %cont

// CHECK:       cacheIsNull:
// CHECK-NEXT:    [[T0:%.*]] = call swiftcc %swift.metadata_response @"$s32conditional_conformance_subclass16SubclassConcreteCMa"(i64 255)
// CHECK-NEXT:    [[SubclassConcrete_TYPE:%.*]] = extractvalue %swift.metadata_response [[T0]], 0
// CHECK-NEXT:    extractvalue %swift.metadata_response [[T0]], 1

// CHECK-NEXT:    [[CONDITIONAL_REQUIREMENTS:%.*]] = getelementptr inbounds [1 x ptr], ptr %conditional.requirement.buffer, i32 0, i32 0
// CHECK-NEXT:    [[A_P2_PTR:%.*]] = getelementptr inbounds ptr, ptr [[CONDITIONAL_REQUIREMENTS]], i32 0
// CHECK-NEXT:    store ptr @"$s32conditional_conformance_subclass4IsP2VAA0E0AAWP", ptr [[A_P2_PTR]], align 8
// CHECK-NEXT:    [[Base_P1:%.*]] = call ptr @swift_getWitnessTable
// CHECK-NEXT:    store atomic ptr [[Base_P1]], ptr @"$s32conditional_conformance_subclass16SubclassConcreteCAA4BaseCyxGAA2P1A2A2P2RzlWL" release, align 8
// CHECK-NEXT:    br label %cont

// CHECK:       cont:
// CHECK-NEXT:    [[T0:%.*]] = phi ptr [ [[CACHE]], %entry ], [ [[Base_P1]], %cacheIsNull ]
// CHECK-NEXT:    ret ptr [[T0]]
// CHECK-NEXT:  }

public func subclassgenericconcrete() {
  takes_p1(SubclassGenericConcrete.self)
}

// CHECK-LABEL: define{{( dllexport| protected)?}} swiftcc void @"$s32conditional_conformance_subclass23subclassgenericconcreteyyF"()
// CHECK-NEXT:  entry:
// CHECK-NEXT:    [[T0:%.*]] = call swiftcc %swift.metadata_response @"$s32conditional_conformance_subclass23SubclassGenericConcreteCMa"(i64 0)
// CHECK-NEXT:    [[SubclassGenericConcrete_TYPE:%.*]] = extractvalue %swift.metadata_response [[T0]], 0
// CHECK-NEXT:    [[SubclassGenericConcrete_P1:%.*]] = call ptr @"$s32conditional_conformance_subclass23SubclassGenericConcreteCAA4BaseCyxGAA2P1A2A2P2RzlWl"()
// CHECK-NEXT:    call swiftcc void @"$s32conditional_conformance_subclass8takes_p1yyxmAA2P1RzlF"(ptr [[SubclassGenericConcrete_TYPE]], ptr [[SubclassGenericConcrete_TYPE]], ptr [[SubclassGenericConcrete_P1]])
// CHECK-NEXT:    ret void
// CHECK-NEXT:  }

// CHECK-LABEL: define linkonce_odr hidden ptr @"$s32conditional_conformance_subclass23SubclassGenericConcreteCAA4BaseCyxGAA2P1A2A2P2RzlWl"()
// CHECK-NEXT:  entry:
// CHECK-NEXT:    %conditional.requirement.buffer = alloca [1 x ptr], align 8
// CHECK-NEXT:    [[CACHE:%.*]] = load ptr, ptr @"$s32conditional_conformance_subclass23SubclassGenericConcreteCAA4BaseCyxGAA2P1A2A2P2RzlWL", align 8
// CHECK-NEXT:    [[IS_NULL:%.*]] = icmp eq ptr [[CACHE]], null
// CHECK-NEXT:    br i1 [[IS_NULL]], label %cacheIsNull, label %cont

// CHECK:       cacheIsNull:
// CHECK-NEXT:    [[T0:%.*]] = call swiftcc %swift.metadata_response @"$s32conditional_conformance_subclass23SubclassGenericConcreteCMa"(i64 255)
// CHECK-NEXT:    [[SubclassGenericConcrete_TYPE:%.*]] = extractvalue %swift.metadata_response [[T0]], 0
// CHECK-NEXT:    extractvalue %swift.metadata_response [[T0]], 1
// CHECK-NEXT:    [[CONDITIONAL_REQUIREMENTS:%.*]] = getelementptr inbounds [1 x ptr], ptr %conditional.requirement.buffer, i32 0, i32 0
// CHECK-NEXT:    [[A_P2_PTR:%.*]] = getelementptr inbounds ptr, ptr [[CONDITIONAL_REQUIREMENTS]], i32 0
// CHECK-NEXT:    store ptr @"$s32conditional_conformance_subclass4IsP2VAA0E0AAWP", ptr [[A_P2_PTR]], align 8
// CHECK-NEXT:    [[Base_P1:%.*]] = call ptr @swift_getWitnessTable
// CHECK-NEXT:    store atomic ptr [[Base_P1]], ptr @"$s32conditional_conformance_subclass23SubclassGenericConcreteCAA4BaseCyxGAA2P1A2A2P2RzlWL" release, align 8
// CHECK-NEXT:    br label %cont

// CHECK:       cont:
// CHECK-NEXT:    [[T0:%.*]] = phi ptr [ [[CACHE]], %entry ], [ [[Base_P1]], %cacheIsNull ]
// CHECK-NEXT:    ret ptr [[T0]]
// CHECK-NEXT:  }
