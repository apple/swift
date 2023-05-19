// RUN: %target-swift-emit-sil -module-name Test -parse-as-library %s -verify -unavailable-decl-optimization=stub | %FileCheck %s --check-prefixes=CHECK

public struct S {}

// CHECK-LABEL: sil{{.*}}@$s4Test15unavailableFuncAA1SVyF
// CHECK:         [[FNREF:%.*]] = function_ref @$[[DIAGNOSEFN:ss31_diagnoseUnavailableCodeReacheds5NeverOy(FTwb|F)]] : $@convention(thin) () -> Never
// CHECK-NEXT:    [[APPLY:%.*]] = apply [[FNREF]]()
// CHECK-NEXT:    unreachable
// CHECK-NEXT:  } // end sil function '$s4Test15unavailableFuncAA1SVyF'
@available(*, unavailable)
public func unavailableFunc() -> S {

  // No warning should be produced for this code even though the compiler
  // inserted call to _diagnoseUnavailableCodeReached() makes it unreachable.
  return S()
}
