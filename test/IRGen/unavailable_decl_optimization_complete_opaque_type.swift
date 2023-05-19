// RUN: %target-swift-frontend -parse-as-library -module-name Test -validate-tbd-against-ir=missing %s -emit-ir | %FileCheck %s --check-prefixes=CHECK,CHECK-NO-STRIP

// RUN: %target-swift-frontend -parse-as-library -module-name Test -validate-tbd-against-ir=missing -unavailable-decl-optimization=complete %s -emit-ir | %FileCheck %s --check-prefixes=CHECK,CHECK-STRIP

// CHECK-NO-STRIP: s4Test1SV31unavailableFuncWithOpaqueReturnQryFQOMQ
// CHECK-STRIP-NOT: s4Test1SV31unavailableFuncWithOpaqueReturnQryFQOMQ

// CHECK-NO-STRIP: s4Test1SV30unavailableVarWithOpaqueReturnQrvpQOMQ
// CHECK-STRIP-NOT: s4Test1SV30unavailableVarWithOpaqueReturnQrvpQOMQ

public protocol P {}

@available(SwiftStdlib 5.1, *)
public struct S: P {
  // CHECK-NO-STRIP: s4Test1SV31unavailableFuncWithOpaqueReturnQryF
  // CHECK-STRIP-NOT: s4Test1SV31unavailableFuncWithOpaqueReturnQryF
  @available(*, unavailable)
  public func unavailableFuncWithOpaqueReturn() -> some P {
    return self
  }

  // CHECK-NO-STRIP: s4Test1SV30unavailableVarWithOpaqueReturnQrvg
  // CHECK-STRIP-NOT: s4Test1SV30unavailableVarWithOpaqueReturnQrvg
  @available(*, unavailable)
  public var unavailableVarWithOpaqueReturn: some P {
    return self
  }
}
