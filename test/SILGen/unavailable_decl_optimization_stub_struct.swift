// RUN: %target-swift-emit-silgen -module-name Test -parse-as-library %s -verify -unavailable-decl-optimization=stub | %FileCheck %s --check-prefixes=CHECK

public struct S {}

@available(*, unavailable)
public struct ImplicitInitStruct {
  // CHECK-LABEL: sil hidden {{.*}} @$s4Test18ImplicitInitStructVACycfC
  // CHECK:         [[FNREF:%.*]] = function_ref @$[[DIAGNOSEFN:ss31_diagnoseUnavailableCodeReacheds5NeverOy(FTwb|F)]] : $@convention(thin) () -> Never
  // CHECK-NEXT:    [[APPLY:%.*]] = apply [[FNREF]]()
  // CHECK:         return
  // CHECK:       } // end sil function '$s4Test18ImplicitInitStructVACycfC'
}

@available(*, unavailable)
public func testImplicitConstructor() {
  // Force s4Test18ImplicitInitStructVACycfC to be emitted.
  _ = ImplicitInitStruct()
}

@available(*, unavailable)
public struct ExplicitInitStruct {
  // ExplicitInitStruct.s.getter
  // CHECK-LABEL: sil{{.*}}@$s4Test18ExplicitInitStructV1sAA1SVvg
  // CHECK:         [[FNREF:%.*]] = function_ref @$[[DIAGNOSEFN]] : $@convention(thin) () -> Never
  // CHECK-NEXT:    [[APPLY:%.*]] = apply [[FNREF]]()
  // CHECK:         struct_extract
  // CHECK:       } // end sil function '$s4Test18ExplicitInitStructV1sAA1SVvg'
  //
  // ExplicitInitStruct.s.setter
  // CHECK-LABEL: sil{{.*}}@$s4Test18ExplicitInitStructV1sAA1SVvs
  // CHECK:         [[FNREF:%.*]] = function_ref @$[[DIAGNOSEFN]] : $@convention(thin) () -> Never
  // CHECK-NEXT:    [[APPLY:%.*]] = apply [[FNREF]]()
  // CHECK:         assign
  // CHECK:       } // end sil function '$s4Test18ExplicitInitStructV1sAA1SVvs'
  //
  // ExplicitInitStruct.s.modify
  // CHECK-LABEL: sil{{.*}}@$s4Test18ExplicitInitStructV1sAA1SVvM
  // CHECK:         [[FNREF:%.*]] = function_ref @$[[DIAGNOSEFN]] : $@convention(thin) () -> Never
  // CHECK-NEXT:    [[APPLY:%.*]] = apply [[FNREF]]()
  // CHECK:         yield
  // CHECK:       } // end sil function '$s4Test18ExplicitInitStructV1sAA1SVvM'
  public var s: S

  // CHECK-LABEL: sil{{.*}}@$s4Test18ExplicitInitStructVACycfC
  // CHECK:         [[FNREF:%.*]] = function_ref @$[[DIAGNOSEFN]] : $@convention(thin) () -> Never
  // CHECK-NEXT:    [[APPLY:%.*]] = apply [[FNREF]]()
  // CHECK:         function_ref @$s4Test1SVACycfC
  // CHECK:       } // end sil function '$s4Test18ExplicitInitStructVACycfC'
  public init() {
    s = S()
  }
}

func foo() {}

@available(*, unavailable)
@_moveOnly
struct MoveOnlyStruct {
  // CHECK-LABEL: sil{{.*}}@$s4Test14MoveOnlyStructVfD
  // CHECK:         [[FNREF:%.*]] = function_ref @$[[DIAGNOSEFN]] : $@convention(thin) () -> Never
  // CHECK-NEXT:    [[APPLY:%.*]] = apply [[FNREF]]()
  // CHECK:         {{%.*}} = function_ref @$s4Test3fooyyF
  // CHECK:       } // end sil function '$s4Test14MoveOnlyStructVfD'
  deinit {
    foo()
  }
}
