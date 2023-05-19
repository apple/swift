// RUN: %target-swift-frontend -parse-as-library -module-name Test -validate-tbd-against-ir=missing %s -emit-ir | %FileCheck %s --check-prefixes=CHECK,CHECK-NO-STRIP

// RUN: %target-swift-frontend -parse-as-library -module-name Test -validate-tbd-against-ir=missing -unavailable-decl-optimization=complete %s -emit-ir | %FileCheck %s --check-prefixes=CHECK,CHECK-STRIP

// CHECK-NO-STRIP: s4Test14globalConstantSbvp
// CHECK-NO-STRIP: s4Test14globalConstantSbvau
// CHECK-STRIP-NOT: s4Test14globalConstantSbvp
// CHECK-STRIP-NOT: s4Test14globalConstantSbvau
@available(*, unavailable)
public let globalConstant = true

// CHECK-NO-STRIP: s4Test15unavailableFuncyyF
// CHECK-STRIP-NOT: s4Test15unavailableFuncyyF
@available(*, unavailable)
public func unavailableFunc() {}
