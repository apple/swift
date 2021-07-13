// RUN: %empty-directory(%t)

// RUN: %target-build-swift -module-name keypaths_resilient -emit-module %S/Inputs/keypaths_multi_file_c.swift -emit-module-path %t/keypaths_resilient.swiftmodule

// RUN: %target-swift-emit-silgen -module-name keypaths -primary-file %s %S/Inputs/keypaths_multi_file_b.swift -I %t | %FileCheck %s
// RUN: %target-swift-emit-silgen -module-name keypaths %s -primary-file %S/Inputs/keypaths_multi_file_b.swift -I %t | %FileCheck --check-prefix=DEFINITION %s

import keypaths_resilient

func foo(x: Int) -> KeyPath<A, Int> {
  switch x {
  case 0:
    return \A.x
  default:
    return \A.[0]
  }
  return \A.x
}

// A.x getter
// CHECK-LABEL: sil hidden_external @$s8keypaths1AV1xSivg
// DEFINITION-LABEL: sil hidden [ossa] @$s8keypaths1AV1xSivg

// A.subscript getter
// CHECK-LABEL: sil hidden_external @$s8keypaths1AVyS2icig
// DEFINITION-LABEL: sil hidden [ossa] @$s8keypaths1AVyS2icig

func bar<T>(_: T) {
  _ = \C<T>.b
  _ = \C<T>[0]

  _ = \D<T>.b
  _ = \D<T>[0]

  _ = \P.b

  // FIXME: crashes
  // _ = \P[0]
}

// https://bugs.swift.org/browse/SR-8643
class MM<T: P> : PP {}
func foo<T3: BB, T4: MM<T3>>(t: T3, u: T4) {
  let _ = \CC<T4>.x
}
