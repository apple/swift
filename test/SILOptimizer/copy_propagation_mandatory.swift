// RUN: %target-swift-frontend -Onone %s -parse-as-library -emit-sil -Xllvm -sil-disable-pass=function-signature-opts | %FileCheck %s

// Check end-to-end lifetime durability.

class K {
  init() {}
}

@inline(never)
func getK() -> K {
  return K()
}

@inline(never)
func useK(k: K) {}

@inline(never)
func dummy() -> () {}

// CHECK-LABEL: sil hidden @$s26copy_propagation_mandatory10testSimpleyyF : $@convention(thin) () -> () {
// CHECK: [[GETK:%.*]] = apply
// CHECK: [[UNOWNED:%.*]] = ref_to_unowned [[GETK]] : $K to $@sil_unowned K
// CHECK: unowned_retain [[UNOWNED]] : $@sil_unowned K
// CHECK: apply %{{.*}} : $@convention(thin) (@guaranteed K) -> ()
// CHECK: strong_release [[GETK]] : $K
// CHECK: apply %{{.*}}() : $@convention(thin) () -> ()
// CHECK: unowned_release [[UNOWNED]] : $@sil_unowned K
// CHECK-LABEL: } // end sil function '$s26copy_propagation_mandatory10testSimpleyyF'
func testSimple() {
  let k = getK()
  useK(k: k)
  dummy()
}
