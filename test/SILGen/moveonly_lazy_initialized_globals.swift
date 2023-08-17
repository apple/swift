// RUN: %target-swift-emit-silgen -sil-verify-all -parse-as-library %s | %FileCheck %s

struct S: ~Copyable {
  let s: String
  init(_ s: String) { self.s = s }
  deinit { print("deiniting \(s)") }
}

struct M4 : ~Copyable {
  var s1: S
  var s2: S
  init(_ s: String) {
      fatalError()
  }
}

func rewriteTwo(_ one: inout S, _ two: inout S) {
  one = S("new1")
  two = S("new2")
}

var m = M4("1")
var m2 = M4("1")

struct Doit {
  static var m3 = M4("1")
  static var m4 = M4("1")

  // CHECK-LABEL: sil hidden [ossa] @$s33moveonly_lazy_initialized_globals4DoitV3runyyFZ : $@convention(method) (@thin Doit.Type) -> () {
  // CHECK: [[F1:%.*]] = function_ref @$s33moveonly_lazy_initialized_globals1mAA2M4Vvau : $@convention(thin) () -> Builtin.RawPointer
  // CHECK: [[PTR1:%.*]] = apply [[F1]]()
  // CHECK: [[ADDR1:%.*]] = pointer_to_address [[PTR1]]
  // CHECK: [[F2:%.*]] = function_ref @$s33moveonly_lazy_initialized_globals2m2AA2M4Vvau : $@convention(thin) () -> Builtin.RawPointer
  // CHECK: [[PTR2:%.*]] = apply [[F2]]()
  // CHECK: [[ADDR2:%.*]] = pointer_to_address [[PTR2]]
  // CHECK: [[ACCESS1:%.*]] = begin_access [modify] [dynamic] [[ADDR1]]
  // CHECK: [[MARK1:%.*]] = mark_must_check [assignable_but_not_consumable] [[ACCESS1]]
  // CHECK: [[GEP1:%.*]] = struct_element_addr [[MARK1]]
  // CHECK: [[ACCESS2:%.*]] = begin_access [modify] [dynamic] [[ADDR2]]
  // CHECK: [[MARK2:%.*]] = mark_must_check [assignable_but_not_consumable] [[ACCESS2]]
  // CHECK: [[GEP2:%.*]] = struct_element_addr [[MARK2]]
  // CHECK: apply {{%.*}}([[GEP1]], [[GEP2]])
  //
  // CHECK: [[F3:%.*]] = function_ref @$s33moveonly_lazy_initialized_globals4DoitV2m3AA2M4Vvau : $@convention(thin) () -> Builtin.RawPointer
  // CHECK: [[PTR3:%.*]] = apply [[F3]]()
  // CHECK: [[ADDR3:%.*]] = pointer_to_address [[PTR3]]
  // CHECK: [[F4:%.*]] = function_ref @$s33moveonly_lazy_initialized_globals4DoitV2m4AA2M4Vvau : $@convention(thin) () -> Builtin.RawPointer
  // CHECK: [[PTR4:%.*]] = apply [[F4]]()
  // CHECK: [[ADDR4:%.*]] = pointer_to_address [[PTR4]]
  // CHECK: [[ACCESS3:%.*]] = begin_access [modify] [dynamic] [[ADDR3]]
  // CHECK: [[MARK3:%.*]] = mark_must_check [assignable_but_not_consumable] [[ACCESS3]]
  // CHECK: [[GEP3:%.*]] = struct_element_addr [[MARK3]]
  // CHECK: [[ACCESS4:%.*]] = begin_access [modify] [dynamic] [[ADDR4]]
  // CHECK: [[MARK4:%.*]] = mark_must_check [assignable_but_not_consumable] [[ACCESS4]]
  // CHECK: [[GEP4:%.*]] = struct_element_addr [[MARK4]]
  // CHECK: apply {{%.*}}([[GEP3]], [[GEP4]])
  // CHECK: } // end sil function '$s33moveonly_lazy_initialized_globals4DoitV3runyyFZ'
  static func run() {
      rewriteTwo(&m.s1, &m2.s2)
      rewriteTwo(&m3.s1, &m4.s2)
  }
}
