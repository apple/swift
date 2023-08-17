// RUN: %empty-directory(%t)
// RUN: %target-swift-frontend -emit-module -enable-library-evolution -unavailable-decl-optimization=none -emit-module-path=%t/resilient_struct.swiftmodule -module-name=resilient_struct %S/../Inputs/resilient_struct.swift
// RUN: %target-swift-frontend -emit-module -enable-library-evolution -unavailable-decl-optimization=none -emit-module-path=%t/resilient_enum.swiftmodule -module-name=resilient_enum -I %t %S/../Inputs/resilient_enum.swift

// RUN: %target-swift-emit-sil -module-name Test -parse-as-library %s -verify -unavailable-decl-optimization=complete -Onone -I %t | %FileCheck %s

import resilient_struct
import resilient_enum

func foo() {}

public enum HasUnavailableElement {
  case available

  @available(*, unavailable)
  case unavailable
}

// CHECK-LABEL: sil @$s4Test22testFullyCoveredSwitchyyAA21HasUnavailableElementOF : $@convention(thin) (HasUnavailableElement) -> () {
// CHECK:         switch_enum %0 : $HasUnavailableElement, case #HasUnavailableElement.available!enumelt: [[AVAILBB:bb[0-9]+]], default [[DEFAULTBB:bb[0-9]+]]
// CHECK:       [[DEFAULTBB]]:
// CHECK-NEXT:    integer_literal $Builtin.Int1, -1
// CHECK-NEXT:    cond_fail {{%.*}} : $Builtin.Int1, "unexpected enum value"
// CHECK-NEXT:    unreachable
// CHECK:       } // end sil function '$s4Test22testFullyCoveredSwitchyyAA21HasUnavailableElementOF'
public func testFullyCoveredSwitch(_ e: HasUnavailableElement) {
  switch e {
  case .available: ()
  case .unavailable: ()
  }
}

// CHECK-LABEL: sil @$s4Test31testFullyCoveredSwitchResilientyy14resilient_enum0F23EnumWithUnavailableCaseOF : $@convention(thin) (@in_guaranteed ResilientEnumWithUnavailableCase) -> () {
// CHECK:         switch_enum_addr %2 : $*ResilientEnumWithUnavailableCase, case #ResilientEnumWithUnavailableCase.available!enumelt: [[AVAILBB:bb[0-9]+]], default [[DEFAULTBB:bb[0-9]+]]
// CHECK:       [[DEFAULTBB]]:
// CHECK:         function_ref @$ss27_diagnoseUnexpectedEnumCase4types5NeverOxm_tlF
// CHECK-NEXT:    apply
// CHECK-NEXT:    unreachable
// CHECK:       } // end sil function '$s4Test31testFullyCoveredSwitchResilientyy14resilient_enum0F23EnumWithUnavailableCaseOF'
public func testFullyCoveredSwitchResilient(_ e: ResilientEnumWithUnavailableCase) {
  switch e {
  case .available: ()
  case .unavailable: ()
  }
}

// CHECK-LABEL: sil @$s4Test37testUnknownDefaultCaseSwitchResilientyy14resilient_enum0g19EnumWithUnavailableE0OF : $@convention(thin) (@in_guaranteed ResilientEnumWithUnavailableCase) -> () {
// CHECK:         switch_enum_addr %2 : $*ResilientEnumWithUnavailableCase, case #ResilientEnumWithUnavailableCase.available!enumelt: [[AVAILBB:bb[0-9]+]], default [[DEFAULTBB:bb[0-9]+]]
// CHECK:       [[DEFAULTBB]]:
// CHECK:         function_ref @$s4Test3fooyyF
// CHECK-NEXT:    apply
// CHECK:         br
// CHECK:       } // end sil function '$s4Test37testUnknownDefaultCaseSwitchResilientyy14resilient_enum0g19EnumWithUnavailableE0OF'
public func testUnknownDefaultCaseSwitchResilient(_ e: ResilientEnumWithUnavailableCase) {
  switch e {
  case .available: ()
  case .unavailable: ()
  @unknown default:
    foo()
  }
}

// CHECK-LABEL: sil @$s4Test35testUnavailableElementSkippedSwitchyyAA03HascD0OF : $@convention(thin) (HasUnavailableElement) -> () {
// CHECK:         switch_enum %0 : $HasUnavailableElement, case #HasUnavailableElement.available!enumelt: [[AVAILBB:bb[0-9]+]], default [[DEFAULTBB:bb[0-9]+]]
// CHECK:       [[DEFAULTBB]]:
// CHECK:         function_ref @$ss27_diagnoseUnexpectedEnumCase4types5NeverOxm_tlF
// CHECK-NEXT:    apply
// CHECK-NEXT:    unreachable
// CHECK:       } // end sil function '$s4Test35testUnavailableElementSkippedSwitchyyAA03HascD0OF'
public func testUnavailableElementSkippedSwitch(_ e: HasUnavailableElement) {
  switch e {
  case .available: ()
  }
}

// CHECK-LABEL: sil @$s4Test44testUnavailableElementSkippedSwitchResilientyy14resilient_enum0g8EnumWithC4CaseOF : $@convention(thin) (@in_guaranteed ResilientEnumWithUnavailableCase) -> () {
// CHECK:         switch_enum_addr %2 : $*ResilientEnumWithUnavailableCase, case #ResilientEnumWithUnavailableCase.available!enumelt: [[AVAILBB:bb[0-9]+]], default [[DEFAULTBB:bb[0-9]+]]
// CHECK:       [[DEFAULTBB]]:
// CHECK:         function_ref @$ss27_diagnoseUnexpectedEnumCase4types5NeverOxm_tlF
// CHECK-NEXT:    apply
// CHECK-NEXT:    unreachable
// CHECK:       } // end sil function '$s4Test44testUnavailableElementSkippedSwitchResilientyy14resilient_enum0g8EnumWithC4CaseOF'
public func testUnavailableElementSkippedSwitchResilient(_ e: ResilientEnumWithUnavailableCase) {
  switch e {
  case .available: ()
  }
}

// CHECK-LABEL: sil @$s4Test33testAvailableElementSkippedSwitchyyAA014HasUnavailableD0OF : $@convention(thin) (HasUnavailableElement) -> () {
// CHECK:         switch_enum %0 : $HasUnavailableElement, default [[DEFAULTBB:bb[0-9]+]]
// CHECK:       [[DEFAULTBB]]:
// CHECK:         function_ref @$s4Test3fooyyF
// CHECK:         apply
// CHECK:       } // end sil function '$s4Test33testAvailableElementSkippedSwitchyyAA014HasUnavailableD0OF'
public func testAvailableElementSkippedSwitch(_ e: HasUnavailableElement) {
  switch e {
  case .unavailable: ()
  default: foo()
  }
}

// CHECK-LABEL: sil @$s4Test26testAvailableElementIfCaseyyAA014HasUnavailableD0OF : $@convention(thin) (HasUnavailableElement) -> () {
// CHECK:         switch_enum %0 : $HasUnavailableElement, case #HasUnavailableElement.available!enumelt: [[AVAILBB:bb[0-9]+]], default [[DEFAULTBB:bb[0-9]+]]
// CHECK:       [[DEFAULTBB]]:
// CHECK-NEXT:    integer_literal $Builtin.Int1, -1
// CHECK-NEXT:    cond_fail {{%.*}} : $Builtin.Int1, "unexpected enum value"
// CHECK-NEXT:    unreachable
// CHECK:       } // end sil function '$s4Test26testAvailableElementIfCaseyyAA014HasUnavailableD0OF'
public func testAvailableElementIfCase(_ e: HasUnavailableElement) {
  if case .available = e {
    ()
  }
}

// CHECK-LABEL: sil @$s4Test35testAvailableElementIfCaseResilientyy14resilient_enum0g19EnumWithUnavailableF0OF : $@convention(thin) (@in_guaranteed ResilientEnumWithUnavailableCase) -> () {
// CHECK          switch_enum_addr %2 : $*ResilientEnumWithUnavailableCase, case #ResilientEnumWithUnavailableCase.available!enumelt: [[AVAILBB:bb[0-9]+]], default [[DEFAULTBB:bb[0-9]+]]
// CHECK:       [[DEFAULTBB]]:
// CHECK-NOT:     unreachable
// CHECK:       } // end sil function '$s4Test35testAvailableElementIfCaseResilientyy14resilient_enum0g19EnumWithUnavailableF0OF'
public func testAvailableElementIfCaseResilient(_ e: ResilientEnumWithUnavailableCase) {
  if case .available = e {
    ()
  }
}

// CHECK-LABEL: sil @$s4Test28testUnavailableElementIfCaseyyAA03HascD0OF : $@convention(thin) (HasUnavailableElement) -> () {
// CHECK:         switch_enum %0 : $HasUnavailableElement, case #HasUnavailableElement.available!enumelt: [[AVAILBB:bb[0-9]+]], default [[DEFAULTBB:bb[0-9]+]]
// CHECK:       [[DEFAULTBB]]:
// CHECK-NEXT:    integer_literal $Builtin.Int1, -1
// CHECK-NEXT:    cond_fail {{%.*}} : $Builtin.Int1, "unexpected enum value"
// CHECK-NEXT:    unreachable
// CHECK:       } // end sil function '$s4Test28testUnavailableElementIfCaseyyAA03HascD0OF'
public func testUnavailableElementIfCase(_ e: HasUnavailableElement) {
  if case .unavailable = e {
    ()
  }
}

// CHECK-LABEL: sil @$s4Test37testUnavailableElementIfCaseResilientyy14resilient_enum0g8EnumWithcF0OF : $@convention(thin) (@in_guaranteed ResilientEnumWithUnavailableCase) -> () {
// CHECK:         switch_enum_addr %2 : $*ResilientEnumWithUnavailableCase, default [[DEFAULTBB:bb[0-9]+]]
// CHECK:       [[DEFAULTBB]]:
// CHECK-NOT:     unreachable
// CHECK:       } // end sil function '$s4Test37testUnavailableElementIfCaseResilientyy14resilient_enum0g8EnumWithcF0OF'
public func testUnavailableElementIfCaseResilient(_ e: ResilientEnumWithUnavailableCase) {
  if case .unavailable = e {
    ()
  }
}

// CHECK-LABEL: sil @$s4Test47testFallthroughIntoUnavailableElementCaseSwitchyyAA03HaseF0OF : $@convention(thin) (HasUnavailableElement) -> () {
// CHECK:         switch_enum %0 : $HasUnavailableElement, case #HasUnavailableElement.available!enumelt: [[AVAILBB:bb[0-9]+]], default [[DEFAULTBB:bb[0-9]+]]
// CHECK:       [[AVAILBB]]:
// CHECK:         function_ref @$s4Test3fooyyF
// CHECK-NEXT:    apply
// CHECK:         return
// CHECK:       [[DEFAULTBB]]:
// CHECK-NEXT:    integer_literal $Builtin.Int1, -1
// CHECK-NEXT:    cond_fail {{%.*}} : $Builtin.Int1, "unexpected enum value"
// CHECK-NEXT:    unreachable
// CHECK:       } // end sil function '$s4Test47testFallthroughIntoUnavailableElementCaseSwitchyyAA03HaseF0OF'
public func testFallthroughIntoUnavailableElementCaseSwitch(_ e: HasUnavailableElement) {
  switch e {
  case .available:
    fallthrough
  case .unavailable:
    foo()
  }
}

public struct AvailableStruct {}

@available(*, unavailable)
public struct UnavailableStruct {}

public enum HasUnavailableElementAndPayloads {
  case available(AvailableStruct)

  @available(*, unavailable)
  case unavailable(Double)
}

// CHECK-LABEL: sil @$s4Test33testFullyCoveredSwitchWithPayloadyyAA32HasUnavailableElementAndPayloadsOF : $@convention(thin) (HasUnavailableElementAndPayloads) -> () {
// CHECK:         switch_enum %0 : $HasUnavailableElementAndPayloads, case #HasUnavailableElementAndPayloads.available!enumelt: [[AVAILBB:bb[0-9]+]], default [[DEFAULTBB:bb[0-9]+]]
// CHECK:       [[DEFAULTBB]]:
// CHECK-NEXT:    integer_literal $Builtin.Int1, -1
// CHECK-NEXT:    cond_fail {{%.*}} : $Builtin.Int1, "unexpected enum value"
// CHECK-NEXT:    unreachable
// CHECK:       } // end sil function '$s4Test33testFullyCoveredSwitchWithPayloadyyAA32HasUnavailableElementAndPayloadsOF'
public func testFullyCoveredSwitchWithPayload(_ e: HasUnavailableElementAndPayloads) {
  switch e {
  case .available(_): ()
  case .unavailable(_): ()
  }
}

// CHECK-LABEL: sil @$s4Test42testFullyCoveredSwitchWithPayloadResilientyy14resilient_enum0h4Enumf18UnavailableCaseAndG0OF : $@convention(thin) (@in_guaranteed ResilientEnumWithUnavailableCaseAndPayload) -> () {
// CHECK:         switch_enum_addr %2 : $*ResilientEnumWithUnavailableCaseAndPayload, case #ResilientEnumWithUnavailableCaseAndPayload.double!enumelt: [[DOUBLEBB:bb[0-9]+]], default [[DEFAULTBB:bb[0-9]+]]
// CHECK:       [[DEFAULTBB]]:
// CHECK:         function_ref @$ss27_diagnoseUnexpectedEnumCase4types5NeverOxm_tlF
// CHECK-NEXT:    apply
// CHECK-NEXT:    unreachable
// CHECK:       } // end sil function '$s4Test42testFullyCoveredSwitchWithPayloadResilientyy14resilient_enum0h4Enumf18UnavailableCaseAndG0OF'
public func testFullyCoveredSwitchWithPayloadResilient(_ e: ResilientEnumWithUnavailableCaseAndPayload) {
  switch e {
  case .int(_): ()
  case .double(_): ()
  }
}
