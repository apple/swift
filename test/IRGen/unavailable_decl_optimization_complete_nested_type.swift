// RUN: %target-swift-frontend -parse-as-library -module-name Test -validate-tbd-against-ir=missing %s -emit-ir | %FileCheck %s --check-prefixes=CHECK,CHECK-NO-STRIP

// RUN: %target-swift-frontend -parse-as-library -module-name Test -validate-tbd-against-ir=missing -unavailable-decl-optimization=complete %s -emit-ir | %FileCheck %s --check-prefixes=CHECK,CHECK-STRIP

// CHECK-NO-STRIP: s4Test29unavailableFuncWithNestedTypeyyF
// CHECK-STRIP-NOT: s4Test29unavailableFuncWithNestedTypeyyF
@available(*, unavailable)
public func unavailableFuncWithNestedType() {
  struct NestedInFunction {
    // s4Test29unavailableFuncWithNestedTypeyyF0E10InFunctionL_VADycfC
    init() {}
  }

  _ = NestedInFunction()
}

// CHECK-NO-STRIP: s4Test29unavailableFuncWithNestedTypeyyF0E10InFunctionL_VADycfC
// CHECK-STRIP-NOT: s4Test29unavailableFuncWithNestedTypeyyF0E10InFunctionL_VADycfC

public struct S {}

extension S {
  @available(*, unavailable)
  public struct NestedInExtension {
    // CHECK-NO-STRIP: s4Test1SV17NestedInExtensionV6methodyyF
    // CHECK-STRIP-NOT: s4Test1SV17NestedInExtensionV6methodyyF
    public func method() {}
  }
}

// CHECK-NO-STRIP: s4Test1SV17NestedInExtensionVMa
// CHECK-STRIP-NOT: s4Test1SV17NestedInExtensionVMa

// CHECK-NO-STRIP: s4Test29unavailableFuncWithNestedTypeyyF0E10InFunctionL_VMa
// CHECK-STRIP-NOT: s4Test29unavailableFuncWithNestedTypeyyF0E10InFunctionL_VMa
