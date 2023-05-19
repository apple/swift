// RUN: %target-swift-frontend -parse-as-library -module-name Test -validate-tbd-against-ir=missing %s -emit-ir | %FileCheck %s --check-prefixes=CHECK,CHECK-NO-STRIP

// RUN: %target-swift-frontend -parse-as-library -module-name Test -validate-tbd-against-ir=missing -unavailable-decl-optimization=complete %s -emit-ir | %FileCheck %s --check-prefixes=CHECK,CHECK-STRIP

// CHECK-NO-STRIP: s4Test1SVAA1PAAMc
// CHECK-STRIP-NOT: s4Test1SVAA1PAAMc

// CHECK-NO-STRIP: s4Test1SVAA1PAAWP
// CHECK-STRIP-NOT: s4Test1SVAA1PAAWP

// CHECK-NO-STRIP: s4Test1PMp
// CHECK-STRIP-NOT: s4Test1PMp

@available(*, unavailable)
public protocol P {
  func requirement()
}

public struct S {}

@available(*, unavailable)
extension S: P {
  // CHECK-NO-STRIP: s4Test1SV11requirementyyF
  // CHECK-STRIP-NOT: s4Test1SV11requirementyyF
  public func requirement() {}
}
