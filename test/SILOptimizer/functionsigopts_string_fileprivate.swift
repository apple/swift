// RUN: %target-swift-frontend -O -emit-sil -primary-file %s | %FileCheck %s

private var _storage: String? = nil

// CHECK-LABEL: sil private [noinline] @$s34functionsigopts_string_fileprivate12setStorageTo33_{{[a-zA-Z0-9]+}} : $@convention(thin) (@guaranteed String) -> () {
// CHECK: bb{{[0-9]+}}([[STRING:%.*]] : $String):
// CHECK: [[FUNCTION:%.*]] = function_ref @$s34functionsigopts_string_fileprivate10setStorage33_{{[a-zA-Z0-9]+}}_tF
// CHECK: apply [[FUNCTION]]([[STRING]])
// CHECK-LABEL: } // end sil function '$s34functionsigopts_string_fileprivate12setStorageTo33_{{[a-zA-Z0-9]+}}'
@inline(never)
fileprivate func setStorageTo(_ newValue: String) {
  setStorage(to: newValue)
}

// CHECK-LABEL: sil private [noinline] @$s34functionsigopts_string_fileprivate10setStorage33_{{[a-zA-Z0-9]+}}_tF : $@convention(thin) (@guaranteed String) -> () {
// CHECK-LABEL: } // end sil function '$s34functionsigopts_string_fileprivate10setStorage33_{{[a-zA-Z0-9]+}}_tF'
@inline(never)
fileprivate func setStorage(to newValue: String) {
  _storage = newValue
}

setStorageTo("hi")
