// RUN: %target-swift-frontend(mock-sdk: %clang-importer-sdk) -emit-silgen -module-name Test -parse-as-library %s -verify -unavailable-decl-optimization=stub | %FileCheck %s --check-prefixes=CHECK

// REQUIRES: objc_interop

import Foundation

func foo() {}

@available(*, unavailable)
@objc public class C: NSObject {
  // C.__allocating_init()
  // CHECK-LABEL: sil{{.*}}@$s4Test1CCACycfC
  // CHECK:         [[FNREF:%.*]] = function_ref @$[[DIAGNOSEFN:ss31_diagnoseUnavailableCodeReacheds5NeverOy(FTwb|F)]] : $@convention(thin) () -> Never
  // CHECK-NEXT:    [[APPLY:%.*]] = apply [[FNREF]]()
  // CHECK:         {{%.*}} = function_ref @$s4Test1CCACycfcTD
  // CHECK:       } // end sil function '$s4Test1CCACycfC'
  //
  // dynamic C.init()
  // CHECK-LABEL: sil{{.*}}@$s4Test1CCACycfcTD
  // CHECK:         [[FNREF:%.*]] = function_ref @$[[DIAGNOSEFN]] : $@convention(thin) () -> Never
  // CHECK-NEXT:    [[APPLY:%.*]] = apply [[FNREF]]()
  // CHECK:         objc_method %0 : $C, #C.init!initializer.foreign
  // CHECK:       } // end sil function '$s4Test1CCACycfcTD'
  //
  // C.init()
  // CHECK-LABEL: sil{{.*}}@$s4Test1CCACycfc
  // CHECK:         [[FNREF:%.*]] = function_ref @$[[DIAGNOSEFN]] : $@convention(thin) () -> Never
  // CHECK-NEXT:    [[APPLY:%.*]] = apply [[FNREF]]()
  // CHECK:         {{%.*}} = function_ref @$s4Test3fooyyF
  // CHECK:       } // end sil function '$s4Test1CCACycfc'
  //
  // @objc C.init()
  // CHECK-LABEL: sil{{.*}}@$s4Test1CCACycfcTo
  // CHECK:         [[FNREF:%.*]] = function_ref @$[[DIAGNOSEFN]] : $@convention(thin) () -> Never
  // CHECK-NEXT:    [[APPLY:%.*]] = apply [[FNREF]]()
  // CHECK:         {{%.*}} = function_ref @$s4Test1CCACycfc
  // CHECK:       } // end sil function '$s4Test1CCACycfcTo'
  public override init() {
    foo()
  }

  // C.__deallocating_deinit
  // CHECK-LABEL: sil{{.*}}@$s4Test1CCfD
  // CHECK:         [[FNREF:%.*]] = function_ref @$[[DIAGNOSEFN]] : $@convention(thin) () -> Never
  // CHECK-NEXT:    [[APPLY:%.*]] = apply [[FNREF]]()
  // CHECK:         {{%.*}} = function_ref @$s4Test3fooyyF
  // CHECK:       } // end sil function '$s4Test1CCfD'
  //
  // @objc C.__deallocating_deinit
  // CHECK-LABEL: sil{{.*}}@$s4Test1CCfDTo
  // CHECK:         [[FNREF:%.*]] = function_ref @$[[DIAGNOSEFN]] : $@convention(thin) () -> Never
  // CHECK-NEXT:    [[APPLY:%.*]] = apply [[FNREF]]()
  // CHECK:         {{%.*}} = function_ref @$s4Test1CCfD
  // CHECK:       } // end sil function '$s4Test1CCfDTo'
  deinit {
    foo()
  }
}
