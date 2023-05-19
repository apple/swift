// RUN: %target-swift-emit-silgen -module-name Test -parse-as-library %s -verify -unavailable-decl-optimization=stub | %FileCheck %s --check-prefixes=CHECK

public struct S {}

// CHECK-LABEL: sil{{.*}}@$s4Test15unavailableFuncAA1SVyF
// CHECK:         [[FNREF:%.*]] = function_ref @$[[DIAGNOSEFN:ss31_diagnoseUnavailableCodeReacheds5NeverOy(FTwb|F)]] : $@convention(thin) () -> Never
// CHECK-NEXT:    [[APPLY:%.*]] = apply [[FNREF]]()
// CHECK:         function_ref @$s4Test1SVACycfC
// CHECK:       } // end sil function '$s4Test15unavailableFuncAA1SVyF'
@available(*, unavailable)
public func unavailableFunc() -> S {
  return S()
}

enum SomeError: Error { case generic }

// CHECK-LABEL: sil{{.*}}@$s4Test23unavailableThrowingFuncyyKF
// CHECK:         [[FNREF:%.*]] = function_ref @$[[DIAGNOSEFN]] : $@convention(thin) () -> Never
// CHECK-NEXT:    [[APPLY:%.*]] = apply [[FNREF]]()
// CHECK:         throw
// CHECK:       } // end sil function '$s4Test23unavailableThrowingFuncyyKF'
@available(*, unavailable)
public func unavailableThrowingFunc() throws {
  throw SomeError.generic
}

// one-time initialization function for globalVar
// CHECK-LABEL: sil{{.*}}@$s4Test9globalVar_WZ
// CHECK:         [[FNREF:%.*]] = function_ref @$[[DIAGNOSEFN]] : $@convention(thin) () -> Never
// CHECK-NEXT:    [[APPLY:%.*]] = apply [[FNREF]]()
// CHECK:         alloc_global
// CHECK:       } // end sil function '$s4Test9globalVar_WZ'
//
// globalVar.unsafeMutableAddressor
// CHECK-LABEL: sil{{.*}}@$s4Test9globalVarAA1SVvau
// CHECK:         [[FNREF:%.*]] = function_ref @$[[DIAGNOSEFN]] : $@convention(thin) () -> Never
// CHECK-NEXT:    [[APPLY:%.*]] = apply [[FNREF]]()
// CHECK:         global_addr @$s4Test9globalVar_Wz
// CHECK:       } // end sil function '$s4Test9globalVarAA1SVvau'
@available(*, unavailable)
public var globalVar = S()
