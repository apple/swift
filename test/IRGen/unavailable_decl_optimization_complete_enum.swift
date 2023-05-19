// RUN: %target-swift-frontend -parse-as-library -module-name Test -validate-tbd-against-ir=missing %s -emit-ir | %FileCheck %s --check-prefixes=CHECK,CHECK-NO-STRIP

// RUN: %target-swift-frontend -parse-as-library -module-name Test -validate-tbd-against-ir=missing -unavailable-decl-optimization=complete %s -emit-ir | %FileCheck %s --check-prefixes=CHECK,CHECK-STRIP

// CHECK: private constant [27 x i8] c"availableEnumAvailableCase\00"

// FIXME: Should this reflection metadata for an unavailable case be stripped?
// CHECK: private constant [29 x i8] c"availableEnumUnavailableCase\00"

// CHECK-NO-STRIP: private constant [25 x i8] c"unavailableEnumFirstCase\00"
// CHECK-STRIP-NOT: private constant [25 x i8] c"unavailableEnumFirstCase\00"


public enum AvailableEnum {
  case availableEnumAvailableCase

  @available(*, unavailable)
  case availableEnumUnavailableCase

  // CHECK-NO-STRIP: s4Test13AvailableEnumO17unavailableMethodyyF
  // CHECK-STRIP-NOT: s4Test13AvailableEnumO17unavailableMethodyyF
  @available(*, unavailable)
  public func unavailableMethod() {}

  // CHECK: s4Test13AvailableEnumO21__derived_enum_equalsySbAC_ACtFZ
  // CHECK: s4Test13AvailableEnumO4hash4intoys6HasherVz_tF
  // CHECK: s4Test13AvailableEnumO9hashValueSivg
}

@available(*, unavailable)
public enum UnavailableEnum {
  case unavailableEnumFirstCase

  // CHECK-NO-STRIP: s4Test15UnavailableEnumO6methodyyF
  // CHECK-STRIP-NOT: s4Test15UnavailableEnumO6methodyyF
  public func method() {}

  // CHECK-NO-STRIP: s4Test15UnavailableEnumO21__derived_enum_equalsySbAC_ACtFZ
  // CHECK-STRIP-NOT: s4Test15UnavailableEnumO21__derived_enum_equalsySbAC_ACtFZ

  // CHECK-NO-STRIP: s4Test15UnavailableEnumO4hash4intoys6HasherVz_tF
  // CHECK-STRIP-NOT: s4Test15UnavailableEnumO4hash4intoys6HasherVz_tF

  // CHECK-NO-STRIP: s4Test15UnavailableEnumO9hashValueSivg
  // CHECK-STRIP-NOT: s4Test15UnavailableEnumO9hashValueSivg
}

// CHECK: s4Test13AvailableEnumOwug

// CHECK: s4Test13AvailableEnumOMa

// CHECK-NO-STRIP: s4Test15UnavailableEnumOwug
// CHECK-STRIP-NOT: s4Test15UnavailableEnumOwug

// CHECK-NO-STRIP: s4Test15UnavailableEnumOMa
// CHECK-STRIP-NOT: s4Test15UnavailableEnumOMa
