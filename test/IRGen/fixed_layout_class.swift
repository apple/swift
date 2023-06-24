// RUN: %empty-directory(%t)
// RUN: %{python} %utils/chex.py < %s > %t/class_resilience.swift
// RUN: %target-swift-frontend -emit-module -enable-library-evolution -emit-module-path=%t/resilient_struct.swiftmodule -module-name=resilient_struct %S/../Inputs/resilient_struct.swift
// RUN: %target-swift-frontend -emit-module -enable-library-evolution -emit-module-path=%t/resilient_enum.swiftmodule -module-name=resilient_enum -I %t %S/../Inputs/resilient_enum.swift
// RUN: %target-swift-frontend -emit-module -enable-library-evolution -emit-module-path=%t/resilient_class.swiftmodule -module-name=resilient_class -I %t %S/../Inputs/resilient_class.swift
// RUN: %target-swift-frontend -emit-module -enable-library-evolution -emit-module-path=%t/fixed_layout_class.swiftmodule -module-name=fixed_layout_class -I %t %S/../Inputs/fixed_layout_class.swift
// RUN: %target-swift-frontend -I %t -emit-ir -enable-library-evolution %t/class_resilience.swift | %FileCheck %t/class_resilience.swift --check-prefix=CHECK --check-prefix=CHECK-%target-ptrsize --check-prefix=CHECK-%target-runtime -DINT=i%target-ptrsize
// RUN: %target-swift-frontend -I %t -emit-ir -enable-library-evolution -O %t/class_resilience.swift

// This tests @_fixed_layout classes in resilient modules.
import fixed_layout_class

// CHECK-LABEL: define{{( dllexport)?}}{{( protected)?}} swiftcc void @"$s16class_resilience20useRootClassPropertyyy013fixed_layout_A0026OutsideParentWithResilientF0CF"(ptr %0)
public func useRootClassProperty(_ o: OutsideParentWithResilientProperty) {
  // CHECK: getelementptr inbounds %T18fixed_layout_class34OutsideParentWithResilientPropertyC, ptr %0, i32 0, i32 1
  let a = o.p
  // CHECK: load [[INT]], ptr @"$s18fixed_layout_class34OutsideParentWithResilientPropertyC1s16resilient_struct4SizeVvpWvd"
  let b = o.s
  // CHECK: load [[INT]], ptr @"$s18fixed_layout_class34OutsideParentWithResilientPropertyC5colors5Int32VvpWvd"
  let c = o.color
  // CHECK: ret void
}

// CHECK-LABEL: define{{( dllexport)?}}{{( protected)?}} swiftcc void @"$s16class_resilience19useSubclassPropertyyy013fixed_layout_A012OutsideChildCF"(ptr %0)
public func useSubclassProperty(_ o: OutsideChild) {
  // CHECK: getelementptr inbounds %T18fixed_layout_class13OutsideParentC, ptr {{%[0-9]+}}, i32 0, i32 1
  let a = o.property
  // CHECK: getelementptr inbounds %T18fixed_layout_class12OutsideChildC, ptr %0, i32 0, i32 2
  let b = o.childProperty
  // CHECK: ret void
}

// CHECK-LABEL: define{{( dllexport)?}}{{( protected)?}} swiftcc void @"$s16class_resilience27useGenericRootClassPropertyyy013fixed_layout_A00D13OutsideParentCyxGlF"(ptr %0)
public func useGenericRootClassProperty<A>(_ o: GenericOutsideParent<A>) {
  // -- we load the base offset twice, first to get the generic parameter out and
  // then for the property itself.

  // CHECK: [[BASE:%.*]] = load [[INT]], ptr @"$s18fixed_layout_class20GenericOutsideParentCMo"

  // CHECK: [[METADATA:%.*]] = load ptr, ptr %0

  // CHECK: [[BASE:%.*]] = load [[INT]], ptr @"$s18fixed_layout_class20GenericOutsideParentCMo"
  // CHECK: [[FIELD_OFFSET_OFFSET:%.*]] = add [[INT]] [[BASE]], {{4|8}}

  // CHECK: [[FIELD_OFFSET_ADDR:%.*]] = getelementptr inbounds i8, ptr [[METADATA]], [[INT]] [[FIELD_OFFSET_OFFSET]]
  // CHECK: [[FIELD_OFFSET:%.*]] = load [[INT]], ptr [[FIELD_OFFSET_ADDR]]
  let a = o.property

  // CHECK: ret void
}

// CHECK-LABEL: define{{( dllexport)?}}{{( protected)?}} swiftcc void @"$s16class_resilience27useGenericRootClassPropertyyy013fixed_layout_A00D13OutsideParentCySiGF"(ptr %0)
public func useGenericRootClassProperty(_ o: GenericOutsideParent<Int>) {
  // CHECK: getelementptr inbounds %T18fixed_layout_class20GenericOutsideParentCySiG, ptr %0, i32 0, i32 1
  let a = o.property

  // CHECK: ret void
}

// CHECK-LABEL: define{{( dllexport)?}}{{( protected)?}} swiftcc void @"$s16class_resilience26useGenericSubclassPropertyyy013fixed_layout_A00D12OutsideChildCyxGlF"(ptr %0)
public func useGenericSubclassProperty<A>(_ o: GenericOutsideChild<A>) {
  // -- we load the base offset twice, first to get the generic parameter out and
  // then for the property itself.

  // CHECK: [[BASE:%.*]] = load [[INT]], ptr @"$s18fixed_layout_class19GenericOutsideChildCMo"

  // CHECK: [[METADATA:%.*]] = load ptr, ptr %0

  // CHECK: [[BASE:%.*]] = load [[INT]], ptr @"$s18fixed_layout_class20GenericOutsideParentCMo"
  // CHECK: [[FIELD_OFFSET_OFFSET:%.*]] = add [[INT]] [[BASE]], {{4|8}}

  // CHECK: [[FIELD_OFFSET_ADDR:%.*]] = getelementptr inbounds i8, ptr [[METADATA]], [[INT]] [[FIELD_OFFSET_OFFSET]]
  // CHECK: [[FIELD_OFFSET:%.*]] = load [[INT]], ptr [[FIELD_OFFSET_ADDR]]
  let a = o.property

  // CHECK: [[METADATA:%.*]] = load ptr, ptr %0

  // CHECK: [[BASE:%.*]] = load [[INT]], ptr @"$s18fixed_layout_class19GenericOutsideChildCMo"
  // CHECK: [[FIELD_OFFSET_OFFSET:%.*]] = add [[INT]] [[BASE]], {{4|8}}

  // CHECK: [[FIELD_OFFSET_ADDR:%.*]] = getelementptr inbounds i8, ptr [[METADATA]], [[INT]] [[FIELD_OFFSET_OFFSET]]
  // CHECK: [[FIELD_OFFSET:%.*]] = load [[INT]], ptr [[FIELD_OFFSET_ADDR]]
  let b = o.childProperty

  // CHECK: ret void
}

// CHECK-LABEL: define{{( dllexport)?}}{{( protected)?}} swiftcc void @"$s16class_resilience26useGenericSubclassPropertyyy013fixed_layout_A00D12OutsideChildCySiGF"(ptr %0)
public func useGenericSubclassProperty(_ o: GenericOutsideChild<Int>) {
  // CHECK: getelementptr inbounds %T18fixed_layout_class20GenericOutsideParentCySiG, ptr %0, i32 0, i32 1
  let a = o.property

  // CHECK: getelementptr inbounds %T18fixed_layout_class19GenericOutsideChildCySiG, ptr %0, i32 0, i32 2
  let b = o.childProperty

  // CHECK: ret void
}

// CHECK-LABEL: define{{( dllexport)?}}{{( protected)?}} swiftcc void @"$s16class_resilience17callVirtualMethodyy013fixed_layout_A013OutsideParentCF"(ptr %0)
public func callVirtualMethod(_ o: OutsideParent) {
  // Note: virtual method calls still use dispatch thunks

  // CHECK: call swiftcc void @"$s18fixed_layout_class13OutsideParentC6methodyyFTj"
  let a = o.method()

  // CHECK: ret void
}

@_fixed_layout open class MyChildOfOutsideParent : OutsideParent {
  public func newMethod() {}
}

// Make sure we emit the dispatch thunk:

// CHECK-LABEL: define{{( dllexport)?}}{{( protected)?}} swiftcc void @"$s16class_resilience22MyChildOfOutsideParentC9newMethodyyFTj"(ptr swiftself %0)
