// RUN: %target-swift-emit-silgen -module-name Test -parse-as-library %s -verify -unavailable-decl-optimization=stub | %FileCheck %s --check-prefixes=CHECK

public struct S {}
func foo() {}

@available(*, unavailable)
public class ExplicitInitClass {
  // ExplicitInitClass.s.getter
  // CHECK-LABEL: sil{{.*}}@$s4Test17ExplicitInitClassC1sAA1SVvg
  // CHECK:         [[FNREF:%.*]] = function_ref @$[[DIAGNOSEFN:ss31_diagnoseUnavailableCodeReacheds5NeverOy(FTwb|F)]] : $@convention(thin) () -> Never
  // CHECK-NEXT:    [[APPLY:%.*]] = apply [[FNREF]]()
  // CHECK:         load
  // CHECK:       } // end sil function '$s4Test17ExplicitInitClassC1sAA1SVvg'
  //
  // ExplicitInitClass.s.setter
  // CHECK-LABEL: sil{{.*}}@$s4Test17ExplicitInitClassC1sAA1SVvs
  // CHECK:         [[FNREF:%.*]] = function_ref @$[[DIAGNOSEFN]] : $@convention(thin) () -> Never
  // CHECK-NEXT:    [[APPLY:%.*]] = apply [[FNREF]]()
  // CHECK:         assign
  // CHECK:       } // end sil function '$s4Test17ExplicitInitClassC1sAA1SVvs'
  //
  // ExplicitInitClass.s.modify
  // CHECK-LABEL: sil{{.*}}@$s4Test17ExplicitInitClassC1sAA1SVvM
  // CHECK:         [[FNREF:%.*]] = function_ref @$[[DIAGNOSEFN]] : $@convention(thin) () -> Never
  // CHECK-NEXT:    [[APPLY:%.*]] = apply [[FNREF]]()
  // CHECK:         yield
  // CHECK:       } // end sil function '$s4Test17ExplicitInitClassC1sAA1SVvM'
  public var s: S

  // ExplicitInitClass.__allocating_init()
  // CHECK-LABEL: sil{{.*}}@$s4Test17ExplicitInitClassCACycfC
  // CHECK:         [[FNREF:%.*]] = function_ref @$[[DIAGNOSEFN]] : $@convention(thin) () -> Never
  // CHECK-NEXT:    [[APPLY:%.*]] = apply [[FNREF]]()
  // CHECK:         alloc_ref $ExplicitInitClass
  // CHECK:         function_ref @$s4Test17ExplicitInitClassCACycfc
  // CHECK:       } // end sil function '$s4Test17ExplicitInitClassCACycfC'
  //
  // ExplicitInitClass.init()
  // CHECK-LABEL: sil{{.*}}@$s4Test17ExplicitInitClassCACycfc
  // CHECK:         [[FNREF:%.*]] = function_ref @$[[DIAGNOSEFN]] : $@convention(thin) () -> Never
  // CHECK-NEXT:    [[APPLY:%.*]] = apply [[FNREF]]()
  // CHECK:         function_ref @$s4Test1SVACycfC
  // CHECK:       } // end sil function '$s4Test17ExplicitInitClassCACycfc'
  public init() {
    s = S()
  }

  // ExplicitInitClass.deinit
  // CHECK-LABEL: sil{{.*}}@$s4Test17ExplicitInitClassCfd
  // CHECK:         [[FNREF:%.*]] = function_ref @$[[DIAGNOSEFN]] : $@convention(thin) () -> Never
  // CHECK-NEXT:    [[APPLY:%.*]] = apply [[FNREF]]()
  // CHECK:         {{%.*}} = function_ref @$s4Test3fooyyF
  // CHECK:       } // end sil function '$s4Test17ExplicitInitClassCfd'
  //
  // ExplicitInitClass.__deallocating_deinit
  // CHECK-LABEL: sil{{.*}}@$s4Test17ExplicitInitClassCfD
  // CHECK:         [[FNREF:%.*]] = function_ref @$[[DIAGNOSEFN]] : $@convention(thin) () -> Never
  // CHECK-NEXT:    [[APPLY:%.*]] = apply [[FNREF]]()
  // CHECK:         {{%.*}} = function_ref @$s4Test17ExplicitInitClassCfd
  // CHECK:       } // end sil function '$s4Test17ExplicitInitClassCfD'
  deinit {
    foo()
  }
}
