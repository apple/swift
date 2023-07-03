// RUN: %empty-directory(%t)
// RUN: %build-irgen-test-overlays
// RUN: %target-swift-frontend(mock-sdk: -sdk %S/Inputs -I %t) %s -emit-ir | %FileCheck %s -DINT=i%target-ptrsize

// REQUIRES: CPU=x86_64
// REQUIRES: objc_interop
// REQUIRES: swift_stdlib_no_asserts,optimized_stdlib

import gizmo

// CHECK: [[CLASS:%objc_class]] = type
// CHECK: [[TYPE:%swift.type]] = type
// CHECK: [[SUPER:%objc_super]] = type
// CHECK: [[NSRECT:%TSo6NSRectV]] = type

class Hoozit : Gizmo {
  // CHECK: define hidden swiftcc void @"$s10objc_super6HoozitC4frobyyF"(ptr swiftself %0) {{.*}} {
  override func frob() {
    // CHECK: [[TMP:%.*]] = call swiftcc %swift.metadata_response @"$s10objc_super6HoozitCMa"([[INT]] 0)
    // CHECK: [[T0:%.*]] = extractvalue %swift.metadata_response [[TMP]], 0
    // CHECK: store ptr [[T0]], ptr {{.*}}, align 8
    // CHECK: load ptr, ptr @"\01L_selector(frob)"
    // CHECK: call void @objc_msgSendSuper2(ptr {{.*}}, ptr {{.*}})
    super.frob()
  }
  // CHECK: }

  // CHECK: define hidden swiftcc void @"$s10objc_super6HoozitC5runceyyFZ"(ptr swiftself %0) {{.*}} {
  override class func runce() {
    // CHECK: store ptr @"OBJC_METACLASS_$__TtC10objc_super6Hoozit", ptr {{.*}}, align 8
    // CHECK: load ptr, ptr @"\01L_selector(runce)"
    // CHECK: call void @objc_msgSendSuper2(ptr {{.*}}, ptr {{.*}})
    super.runce()
  }
  // CHECK: }

  // CHECK: define hidden swiftcc { double, double, double, double } @"$s10objc_super6HoozitC5frameSo6NSRectVyF"(ptr swiftself %0) {{.*}} {
  override func frame() -> NSRect {
    // CHECK: [[TMP:%.*]] = call swiftcc %swift.metadata_response @"$s10objc_super6HoozitCMa"([[INT]] 0)
    // CHECK: [[T0:%.*]] = extractvalue %swift.metadata_response [[TMP]], 0
    // CHECK: store ptr [[T0]], ptr {{.*}}, align 8
    // CHECK: load ptr, ptr @"\01L_selector(frame)"
    // CHECK: call void @objc_msgSendSuper2_stret(ptr noalias nocapture sret({{.*}}) {{.*}}, ptr {{.*}}, ptr {{.*}})
    return NSInsetRect(super.frame(), 2.0, 2.0)
  }
  // CHECK: }

  // CHECK: define hidden swiftcc ptr @"$s10objc_super6HoozitC1xACSi_tcfc"(i64 %0, ptr swiftself %1) {{.*}} {
  init(x:Int) {
    // CHECK: load ptr, ptr @"\01L_selector(init)"
    // CHECK: call ptr @objc_msgSendSuper2(ptr {{.*}}, ptr {{.*}})
    // CHECK-NOT: @swift_dynamicCastClassUnconditional
    // CHECK: ret
    super.init()
  }
  // CHECK: }

  // CHECK: define hidden swiftcc ptr @"$s10objc_super6HoozitC1yACSi_tcfc"(i64 %0, ptr swiftself %1) {{.*}} {
  init(y:Int) {
    // CHECK: load ptr, ptr @"\01L_selector(initWithBellsOn:)"
    // CHECK: call ptr @objc_msgSendSuper2(ptr {{.*}}, ptr {{.*}}, i64 {{.*}})
    // CHECK-NOT: swift_dynamicCastClassUnconditional
    // CHECK: ret
    super.init(bellsOn:y)
  }
  // CHECK: }
}

func acceptFn(_ fn: () -> Void) { }

class PartialApply : Gizmo {
  // CHECK: define hidden swiftcc void @"$s10objc_super12PartialApplyC4frobyyF"(ptr swiftself %0) {{.*}} {
  override func frob() {
    // CHECK: call swiftcc void @"$s10objc_super8acceptFnyyyyXEF"(ptr [[PARTIAL_FORWARDING_THUNK:@"\$[A-Za-z0-9_]+"]], ptr %{{[0-9]+}})
    acceptFn(super.frob)
  }
  // CHECK: }
}

// CHECK: define internal swiftcc void @"$s10objc_super12PartialApplyC4frobyyFyycfu_"(ptr %0)
// CHECK: call swiftcc %swift.metadata_response @"$s10objc_super12PartialApplyCMa"([[INT]] 0)
// CHECK: @"\01L_selector(frob)"
// CHECK: @objc_msgSendSuper2
// CHECK: }

class GenericRuncer<T> : Gizmo {
  var x: Gizmo? = nil
  var y: T?

// Use a constant indirect field access instead of a non-constant direct
// access because the layout dependents on the alignment of y.

// CHECK: define hidden swiftcc i64 @"$s10objc_super13GenericRuncerC1xSo5GizmoCSgvg"(ptr swiftself %0)
// CHECK:    inttoptr
// CHECK:   [[ISA:%.*]] = load i64, ptr %0
// CHECK:   [[ISAMASK:%.*]] = load i64, ptr @swift_isaMask
// CHECK:   [[CLASS:%.*]] = and i64 [[ISA]], [[ISAMASK]]
// CHECK:   [[TY:%.*]] = inttoptr i64 [[CLASS]] to ptr
// CHECK:   [[OFFSETADDR:%.*]] = getelementptr inbounds i64, ptr [[TY]], i64 11
// CHECK:   [[FIELDOFFSET:%.*]] = load i64, ptr [[OFFSETADDR]]
// CHECK:   [[FIELDADDR:%.*]] = getelementptr inbounds i8, ptr %0, i64 [[FIELDOFFSET]]
// CHECK:   [[OPTIONAL:%.*]] = load i64, ptr [[FIELDADDR]]
// CHECK:   [[OBJ:%.*]] = inttoptr i64 [[OPTIONAL]] to ptr
// CHECK:   call ptr @llvm.objc.retain(ptr [[OBJ]])
// CHECK:   ret i64 [[OPTIONAL]]

  // CHECK: define hidden swiftcc void @"$s10objc_super13GenericRuncerC5runceyyFZ"(ptr swiftself %0) {{.*}} {
  override class func runce() {
    // CHECK:      [[TMP:%.*]] = call swiftcc %swift.metadata_response @"$s10objc_super13GenericRuncerCMa"([[INT]] 0, ptr %T)
    // CHECK-NEXT: [[CLASS:%.*]] = extractvalue %swift.metadata_response [[TMP]], 0
    // CHECK-NEXT: [[ISA:%.*]] = load i64, ptr [[CLASS]], align 8
    // CHECK-NEXT: [[ISA_MASK:%.*]] = load i64, ptr @swift_isaMask, align 8
    // CHECK-NEXT: [[ISA_MASKED:%.*]] = and i64 [[ISA]], [[ISA_MASK]]
    // CHECK-NEXT: [[ISA_PTR:%.*]] = inttoptr i64 [[ISA_MASKED]] to ptr
    // CHECK:      [[METACLASS_ADDR:%.*]] = getelementptr inbounds %objc_super, ptr %objc_super, i32 0, i32 1
    // CHECK-NEXT: store ptr [[ISA_PTR]], ptr [[METACLASS_ADDR]], align 8
    // CHECK-NEXT: [[SELECTOR:%.*]] = load ptr, ptr @"\01L_selector(runce)", align 8
    // CHECK-NEXT: call void @objc_msgSendSuper2(ptr %objc_super, ptr [[SELECTOR]])
    // CHECK-NEXT: ret void
    super.runce()
  }
}

// CHECK: define internal swiftcc void [[PARTIAL_FORWARDING_THUNK]](ptr swiftself %0) {{.*}} {
// CHECK: @"$ss12StaticStringV14withUTF8BufferyxxSRys5UInt8VGXElFxAFXEfU_yt_Tgq5"
// CHECK: }
