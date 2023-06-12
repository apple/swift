// RUN: %target-swift-frontend -parse-as-library -module-name Test -validate-tbd-against-ir=missing %s -emit-ir | %FileCheck %s --check-prefixes=CHECK,CHECK-NO-STRIP
// RUN: %target-swift-frontend -parse-as-library -module-name Test -validate-tbd-against-ir=missing -enable-library-evolution %s -emit-ir | %FileCheck %s --check-prefixes=CHECK,CHECK-NO-STRIP,CHECK-RESILIENT,CHECK-NO-STRIP-RESILIENT

// RUN: %target-swift-frontend -parse-as-library -module-name Test -validate-tbd-against-ir=missing -unavailable-decl-optimization=complete %s -emit-ir | %FileCheck %s --check-prefixes=CHECK,CHECK-STRIP
// RUN: %target-swift-frontend -parse-as-library -module-name Test -validate-tbd-against-ir=missing -enable-library-evolution -unavailable-decl-optimization=complete %s -emit-ir | %FileCheck %s --check-prefixes=CHECK,CHECK-STRIP,CHECK-RESILIENT,CHECK-STRIP-RESILIENT

// CHECK: private constant [27 x i8] c"availableEnumAvailableCase\00"

// FIXME: Should this reflection metadata for an unavailable case be stripped?
// CHECK: private constant [29 x i8] c"availableEnumUnavailableCase\00"

// CHECK-NO-STRIP-RESILIENT: @"$s4Test13AvailableEnumO09availableC34UnavailableCaseWithAssociatedValueyAcA0E6StructVcACmFWC" = {{.*}}constant
// CHECK-STRIP-RESILIENT-NOT: @"$s4Test13AvailableEnumO09availableC34UnavailableCaseWithAssociatedValueyAcA0E6StructVcACmFWC" =

// CHECK-RESILIENT: @"$s4Test13AvailableEnumO09availablecB4CaseyA2CmFWC" = {{.*}}constant

// CHECK-NO-STRIP-RESILIENT: @"$s4Test13AvailableEnumO09availableC15UnavailableCaseyA2CmFWC" = {{.*}}constant
// CHECK-STRIP-RESILIENT-NOT: @"$s4Test13AvailableEnumO09availableC15UnavailableCaseyA2CmFWC" =

// CHECK-NO-STRIP: private constant [25 x i8] c"unavailableEnumFirstCase\00"
// CHECK-STRIP-NOT: private constant [25 x i8] c"unavailableEnumFirstCase\00"

// CHECK-NO-STRIP-RESILIENT: @"$s4Test15UnavailableEnumO011unavailableC9FirstCaseyA2CmFWC" = {{.*}}constant
// CHECK-STRIP-RESILIENT-NOT: @"$s4Test15UnavailableEnumO011unavailableC9FirstCaseyA2CmFWC"

@available(*, unavailable)
public struct UnavailableStruct {}

public enum AvailableEnum {
  case availableEnumAvailableCase

  @available(*, unavailable)
  case availableEnumUnavailableCase

  @available(*, unavailable)
  case availableEnumUnavailableCaseWithAssociatedValue(UnavailableStruct)

  // CHECK-NO-STRIP: s4Test13AvailableEnumO17unavailableMethodyyF
  // CHECK-STRIP-NOT: s4Test13AvailableEnumO17unavailableMethodyyF
  @available(*, unavailable)
  public func unavailableMethod() {}
}

// CHECK-NO-STRIP: s4Test17UnavailableStructVMa
// CHECK-STRIP-NOT: s4Test17UnavailableStructVMa

@available(*, unavailable)
public enum UnavailableEnum {
  case unavailableEnumFirstCase

  // CHECK-NO-STRIP: s4Test15UnavailableEnumO6methodyyF
  // CHECK-STRIP-NOT: s4Test15UnavailableEnumO6methodyyF
  public func method() {}
}

// CHECK: s4Test13AvailableEnumOwug

// CHECK: s4Test13AvailableEnumOMa

// CHECK-NO-STRIP: s4Test15UnavailableEnumOwug
// CHECK-STRIP-NOT: s4Test15UnavailableEnumOwug

// CHECK-NO-STRIP: s4Test15UnavailableEnumOMa
// CHECK-STRIP-NOT: s4Test15UnavailableEnumOMa
