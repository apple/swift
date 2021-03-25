
// RUN: %target-swift-emit-silgen -module-name objc_thunks -Xllvm -sil-full-demangle -Xllvm -sil-print-debuginfo -sdk %S/Inputs -I %S/Inputs -enable-source-import %s -emit-verbose-sil -swift-version 5 -access-notes-path %S/Inputs/objc_access_notes.accessnotes -verify | %FileCheck %s

// Verify that the access notes are necessary for the test to pass.
// RUN-X: not %target-swift-emit-silgen -module-name objc_thunks -Xllvm -sil-full-demangle -Xllvm -sil-print-debuginfo -sdk %S/Inputs -I %S/Inputs -enable-source-import %s -emit-verbose-sil -swift-version 5 | %FileCheck %s

// REQUIRES: objc_interop

import gizmo
import ansible

class Hoozit : Gizmo {
  // expected-remark@+2 {{access note for fancy test suite adds attribute 'objc' to this instance method}}
  // expected-note@+1 {{add attribute explicitly to silence this warning}} {{3-3=@objc }}
  func typical(_ x: Int, y: Gizmo) -> Gizmo { return y }
  // CHECK-LABEL: sil hidden [thunk] [ossa] @$s11objc_thunks6HoozitC7typical_1ySo5GizmoCSi_AGtFTo : $@convention(objc_method) (Int, Gizmo, Hoozit) -> @autoreleased Gizmo {
  // CHECK: bb0([[X:%.*]] : $Int, [[Y:%.*]] : @unowned $Gizmo, [[THIS:%.*]] : @unowned $Hoozit):
  // CHECK-NEXT:   [[Y_COPY:%.*]] = copy_value [[Y]]
  // CHECK-NEXT:   [[THIS_COPY:%.*]] = copy_value [[THIS]]
  // CHECK-NEXT:   [[BORROWED_Y_COPY:%.*]] = begin_borrow [[Y_COPY]]
  // CHECK-NEXT:   [[BORROWED_THIS_COPY:%.*]] = begin_borrow [[THIS_COPY]]
  // CHECK-NEXT:   // function_ref objc_thunks.Hoozit.typical
  // CHECK-NEXT:   [[NATIVE:%.*]] = function_ref @$s11objc_thunks6HoozitC7typical_1ySo5GizmoCSi_AGtF : $@convention(method) (Int, @guaranteed Gizmo, @guaranteed Hoozit) -> @owned Gizmo
  // CHECK-NEXT:   [[RES:%.*]] = apply [[NATIVE]]([[X]], [[BORROWED_Y_COPY]], [[BORROWED_THIS_COPY]]) {{.*}} line:[[@LINE-9]]:8:auto_gen
  // CHECK-NEXT:   end_borrow [[BORROWED_THIS_COPY]]
  // CHECK-NEXT:   end_borrow [[BORROWED_Y_COPY]]
  // CHECK-NEXT:   destroy_value [[THIS_COPY]] : $Hoozit
  // CHECK-NEXT:   destroy_value [[Y_COPY]]
  // CHECK-NEXT:   return [[RES]] : $Gizmo{{.*}} line:[[@LINE-14]]:8:auto_gen
  // CHECK-NEXT: } // end sil function '$s11objc_thunks6HoozitC7typical_1ySo5GizmoCSi_AGtFTo'

  // NS_CONSUMES_SELF by inheritance
  override func fork() { }
  // CHECK-LABEL: sil hidden [thunk] [ossa] @$s11objc_thunks6HoozitC4forkyyFTo : $@convention(objc_method) (@owned Hoozit) -> () {
  // CHECK: bb0([[THIS:%.*]] : @owned $Hoozit):
  // CHECK-NEXT:   [[BORROWED_THIS:%.*]] = begin_borrow [[THIS]]
  // CHECK-NEXT:   // function_ref
  // CHECK-NEXT:   [[NATIVE:%.*]] = function_ref @$s11objc_thunks6HoozitC4forkyyF : $@convention(method) (@guaranteed Hoozit) -> ()
  // CHECK-NEXT:   apply [[NATIVE]]([[BORROWED_THIS]])
  // CHECK-NEXT:   end_borrow [[BORROWED_THIS]]
  // CHECK-NEXT:   destroy_value [[THIS]]
  // CHECK-NEXT:   return
  // CHECK-NEXT: }

  // NS_CONSUMED 'gizmo' argument by inheritance
  override class func consume(_ gizmo: Gizmo?) { }
   // CHECK-LABEL: sil hidden [thunk] [ossa] @$s11objc_thunks6HoozitC7consumeyySo5GizmoCSgFZTo : $@convention(objc_method) (@owned Optional<Gizmo>, @objc_metatype Hoozit.Type) -> () {
  // CHECK: bb0([[GIZMO:%.*]] : @owned $Optional<Gizmo>, [[THIS:%.*]] : $@objc_metatype Hoozit.Type):
  // CHECK-NEXT: [[BORROWED_GIZMO:%.*]] = begin_borrow [[GIZMO]]
  // CHECK-NEXT: [[THICK_THIS:%[0-9]+]] = objc_to_thick_metatype [[THIS]] : $@objc_metatype Hoozit.Type to $@thick Hoozit.Type
  // CHECK:   [[NATIVE:%.*]] = function_ref @$s11objc_thunks6HoozitC7consumeyySo5GizmoCSgFZ : $@convention(method) (@guaranteed Optional<Gizmo>, @thick Hoozit.Type) -> ()
  // CHECK-NEXT:   apply [[NATIVE]]([[BORROWED_GIZMO]], [[THICK_THIS]])
  // CHECK-NEXT:   end_borrow [[BORROWED_GIZMO]]
  // CHECK-NEXT:   destroy_value [[GIZMO]]
  // CHECK-NEXT:   return
  // CHECK-NEXT: }

  // NS_RETURNS_RETAINED by family (-copy)
  // expected-remark@+2 {{access note for fancy test suite adds attribute 'objc' to this instance method}}
  // expected-note@+1 {{add attribute explicitly to silence this warning}} {{3-3=@objc }}
  func copyFoo() -> Gizmo { return self }
  // CHECK-LABEL: sil hidden [thunk] [ossa] @$s11objc_thunks6HoozitC7copyFooSo5GizmoCyFTo : $@convention(objc_method) (Hoozit) -> @owned Gizmo
  // CHECK: bb0([[THIS:%.*]] : @unowned $Hoozit):
  // CHECK-NEXT:   [[THIS_COPY:%.*]] = copy_value [[THIS]]
  // CHECK-NEXT:   [[BORROWED_THIS_COPY:%.*]] = begin_borrow [[THIS_COPY]]
  // CHECK-NEXT:   // function_ref
  // CHECK-NEXT:   [[NATIVE:%.*]] = function_ref @$s11objc_thunks6HoozitC7copyFooSo5GizmoCyF : $@convention(method) (@guaranteed Hoozit) -> @owned Gizmo
  // CHECK-NEXT:   [[RES:%.*]] = apply [[NATIVE]]([[BORROWED_THIS_COPY]])
  // CHECK-NEXT:   end_borrow [[BORROWED_THIS_COPY]]
  // CHECK-NEXT:   destroy_value [[THIS_COPY]]
  // CHECK-NEXT:   return [[RES]]
  // CHECK-NEXT: }

  // NS_RETURNS_RETAINED by family (-mutableCopy)
  // expected-remark@+2 {{access note for fancy test suite adds attribute 'objc' to this instance method}}
  // expected-note@+1 {{add attribute explicitly to silence this warning}} {{3-3=@objc }}
  func mutableCopyFoo() -> Gizmo { return self }
  // CHECK-LABEL: sil hidden [thunk] [ossa] @$s11objc_thunks6HoozitC14mutableCopyFooSo5GizmoCyFTo : $@convention(objc_method) (Hoozit) -> @owned Gizmo
  // CHECK: bb0([[THIS:%.*]] : @unowned $Hoozit):
  // CHECK-NEXT:   [[THIS_COPY:%.*]] = copy_value [[THIS]]
  // CHECK-NEXT:   [[BORROWED_THIS_COPY:%.*]] = begin_borrow [[THIS_COPY]]
  // CHECK-NEXT:   // function_ref
  // CHECK-NEXT:   [[NATIVE:%.*]] = function_ref @$s11objc_thunks6HoozitC14mutableCopyFooSo5GizmoCyF : $@convention(method) (@guaranteed Hoozit) -> @owned Gizmo
  // CHECK-NEXT:   [[RES:%.*]] = apply [[NATIVE]]([[BORROWED_THIS_COPY]])
  // CHECK-NEXT:   end_borrow [[BORROWED_THIS_COPY]]
  // CHECK-NEXT:   destroy_value [[THIS_COPY]]
  // CHECK-NEXT:   return [[RES]]
  // CHECK-NEXT: }

  // NS_RETURNS_RETAINED by family (-copy). This is different from Swift's
  // normal notion of CamelCase, but it's what Clang does, so we should match
  // it.
  // expected-remark@+2 {{access note for fancy test suite adds attribute 'objc' to this instance method}}
  // expected-note@+1 {{add attribute explicitly to silence this warning}} {{3-3=@objc }}
  func copy8() -> Gizmo { return self }
  // CHECK-LABEL: sil hidden [thunk] [ossa] @$s11objc_thunks6HoozitC5copy8So5GizmoCyFTo : $@convention(objc_method) (Hoozit) -> @owned Gizmo
  // CHECK: bb0([[THIS:%.*]] : @unowned $Hoozit):
  // CHECK-NEXT:   [[THIS_COPY:%.*]] = copy_value [[THIS]]
  // CHECK-NEXT:   [[BORROWED_THIS_COPY:%.*]] = begin_borrow [[THIS_COPY]]
  // CHECK-NEXT:   // function_ref
  // CHECK-NEXT:   [[NATIVE:%.*]] = function_ref @$s11objc_thunks6HoozitC5copy8So5GizmoCyF : $@convention(method) (@guaranteed Hoozit) -> @owned Gizmo
  // CHECK-NEXT:   [[RES:%.*]] = apply [[NATIVE]]([[BORROWED_THIS_COPY]])
  // CHECK-NEXT:   end_borrow [[BORROWED_THIS_COPY]]
  // CHECK-NEXT:   destroy_value [[THIS_COPY]]
  // CHECK-NEXT:   return [[RES]]
  // CHECK-NEXT: }

  // NS_RETURNS_RETAINED by family (-copy)
  // expected-remark@+2 {{access note for fancy test suite adds attribute 'objc' to this instance method}}
  // expected-note@+1 {{add attribute explicitly to silence this warning}} {{3-3=@objc(copyDuplicate) }}
  func makeDuplicate() -> Gizmo { return self }
  // CHECK-LABEL: sil hidden [thunk] [ossa] @$s11objc_thunks6HoozitC13makeDuplicateSo5GizmoCyFTo : $@convention(objc_method) (Hoozit) -> @owned Gizmo
  // CHECK: bb0([[THIS:%.*]] : @unowned $Hoozit):
  // CHECK-NEXT:   [[THIS_COPY:%.*]] = copy_value [[THIS]]
  // CHECK-NEXT:   [[BORROWED_THIS_COPY:%.*]] = begin_borrow [[THIS_COPY]]
  // CHECK-NEXT:   // function_ref
  // CHECK-NEXT:   [[NATIVE:%.*]] = function_ref @$s11objc_thunks6HoozitC13makeDuplicateSo5GizmoCyF : $@convention(method) (@guaranteed Hoozit) -> @owned Gizmo
  // CHECK-NEXT:   [[RES:%.*]] = apply [[NATIVE]]([[BORROWED_THIS_COPY]])
  // CHECK-NEXT:   end_borrow [[BORROWED_THIS_COPY]]
  // CHECK-NEXT:   destroy_value [[THIS_COPY]]
  // CHECK-NEXT:   return [[RES]]
  // CHECK-NEXT: }

  // Override the normal family conventions to make this non-consuming and
  // returning at +0.
  // expected-remark@+2 {{access note for fancy test suite adds attribute 'objc' to this instance method}}
  // expected-note@+1 {{add attribute explicitly to silence this warning}} {{3-3=@objc }}
  func initFoo() -> Gizmo { return self }
  // CHECK-LABEL: sil hidden [thunk] [ossa] @$s11objc_thunks6HoozitC7initFooSo5GizmoCyFTo : $@convention(objc_method) (Hoozit) -> @autoreleased Gizmo
  // CHECK: bb0([[THIS:%.*]] : @unowned $Hoozit):
  // CHECK-NEXT:   [[THIS_COPY:%.*]] = copy_value [[THIS]]
  // CHECK-NEXT:   [[BORROWED_THIS_COPY:%.*]] = begin_borrow [[THIS_COPY]]
  // CHECK-NEXT:   // function_ref
  // CHECK-NEXT:   [[NATIVE:%.*]] = function_ref @$s11objc_thunks6HoozitC7initFooSo5GizmoCyF : $@convention(method) (@guaranteed Hoozit) -> @owned Gizmo
  // CHECK-NEXT:   [[RES:%.*]] = apply [[NATIVE]]([[BORROWED_THIS_COPY]])
  // CHECK-NEXT:   end_borrow [[BORROWED_THIS_COPY]]
  // CHECK-NEXT:   destroy_value [[THIS_COPY]]
  // CHECK-NEXT:   return [[RES]]
  // CHECK-NEXT: }

  // expected-remark@+2 {{access note for fancy test suite adds attribute 'objc' to this property}}
  // expected-note@+1 {{add attribute explicitly to silence this warning}} {{3-3=@objc }}
  var typicalProperty: Gizmo
  // -- getter
  // CHECK-LABEL: sil hidden [thunk] [ossa] @$s11objc_thunks6HoozitC15typicalPropertySo5GizmoCvgTo : $@convention(objc_method) (Hoozit) -> @autoreleased Gizmo {
  // CHECK: bb0([[SELF:%.*]] : @unowned $Hoozit):
  // CHECK-NEXT:   [[SELF_COPY:%.*]] = copy_value [[SELF]]
  // CHECK-NEXT:   [[BORROWED_SELF_COPY:%.*]] = begin_borrow [[SELF_COPY]]
  // CHECK-NEXT:   // function_ref objc_thunks.Hoozit.typicalProperty.getter
  // CHECK-NEXT:   [[GETIMPL:%.*]] = function_ref @$s11objc_thunks6HoozitC15typicalPropertySo5GizmoCvg
  // CHECK-NEXT:   [[RES:%.*]] = apply [[GETIMPL]]([[BORROWED_SELF_COPY]])
  // CHECK-NEXT:   end_borrow [[BORROWED_SELF_COPY]]
  // CHECK-NEXT:   destroy_value [[SELF_COPY]]
  // CHECK-NEXT:   return [[RES]] : $Gizmo
  // CHECK-NEXT: }

  // CHECK-LABEL: sil hidden [ossa] @$s11objc_thunks6HoozitC15typicalPropertySo5GizmoCvg : $@convention(method) (@guaranteed Hoozit) -> @owned Gizmo
  // CHECK: bb0(%0 : @guaranteed $Hoozit):
  // CHECK-NEXT:   debug_value %0
  // CHECK-NEXT:   [[ADDR:%.*]] = ref_element_addr %0 : {{.*}}, #Hoozit.typicalProperty
  // CHECK-NEXT:   [[READ:%.*]] = begin_access [read] [dynamic] [[ADDR]] : $*Gizmo
  // CHECK-NEXT:   [[RES:%.*]] = load [copy] [[READ]] {{.*}}
  // CHECK-NEXT:   end_access [[READ]] : $*Gizmo
  // CHECK-NEXT:   return [[RES]]

  // -- setter
  // CHECK-LABEL: sil hidden [thunk] [ossa] @$s11objc_thunks6HoozitC15typicalPropertySo5GizmoCvsTo : $@convention(objc_method) (Gizmo, Hoozit) -> () {
  // CHECK: bb0([[VALUE:%.*]] : @unowned $Gizmo, [[THIS:%.*]] : @unowned $Hoozit):
  // CHECK:   [[VALUE_COPY:%.*]] = copy_value [[VALUE]] : $Gizmo
  // CHECK:   [[THIS_COPY:%.*]] = copy_value [[THIS]] : $Hoozit
  // CHECK:   [[BORROWED_THIS_COPY:%.*]] = begin_borrow [[THIS_COPY]]
  // CHECK:   // function_ref objc_thunks.Hoozit.typicalProperty.setter
  // CHECK:   [[FR:%.*]] = function_ref @$s11objc_thunks6HoozitC15typicalPropertySo5GizmoCvs
  // CHECK:   [[RES:%.*]] = apply [[FR]]([[VALUE_COPY]], [[BORROWED_THIS_COPY]])
  // CHECK:   end_borrow [[BORROWED_THIS_COPY]]
  // CHECK:   destroy_value [[THIS_COPY]]
  // CHECK:   return [[RES]] : $(), loc {{.*}}, scope {{.*}} // id: {{.*}} line:[[@LINE-34]]:7:auto_gen
  // CHECK: } // end sil function '$s11objc_thunks6HoozitC15typicalPropertySo5GizmoCvsTo'

  // CHECK-LABEL: sil hidden [ossa] @$s11objc_thunks6HoozitC15typicalPropertySo5GizmoCvs
  // CHECK: bb0([[ARG0:%.*]] : @owned $Gizmo, [[ARG1:%.*]] : @guaranteed $Hoozit):
  // CHECK:   [[BORROWED_ARG0:%.*]] = begin_borrow [[ARG0]]
  // CHECK:   [[ARG0_COPY:%.*]] = copy_value [[BORROWED_ARG0]]
  // CHECK:   [[ADDR:%.*]] = ref_element_addr [[ARG1]] : {{.*}}, #Hoozit.typicalProperty
  // CHECK:   [[WRITE:%.*]] = begin_access [modify] [dynamic] [[ADDR]] : $*Gizmo
  // CHECK:   assign [[ARG0_COPY]] to [[WRITE]] : $*Gizmo
  // CHECK:   end_access [[WRITE]] : $*Gizmo
  // CHECK:   end_borrow [[BORROWED_ARG0]]
  // CHECK:   destroy_value [[ARG0]]
  // CHECK: } // end sil function '$s11objc_thunks6HoozitC15typicalPropertySo5GizmoCvs'

  // NS_RETURNS_RETAINED getter by family (-copy)
  // expected-remark@+2 {{access note for fancy test suite adds attribute 'objc' to this property}}
  // expected-note@+1 {{add attribute explicitly to silence this warning}} {{3-3=@objc }}
  var copyProperty: Gizmo
  // -- getter
  // CHECK-LABEL: sil hidden [thunk] [ossa] @$s11objc_thunks6HoozitC12copyPropertySo5GizmoCvgTo : $@convention(objc_method) (Hoozit) -> @owned Gizmo {
  // CHECK: bb0([[SELF:%.*]] : @unowned $Hoozit):
  // CHECK-NEXT:   [[SELF_COPY:%.*]] = copy_value [[SELF]]
  // CHECK-NEXT:   [[BORROWED_SELF_COPY:%.*]] = begin_borrow [[SELF_COPY]]
  // CHECK-NEXT:   // function_ref objc_thunks.Hoozit.copyProperty.getter
  // CHECK-NEXT:   [[FR:%.*]] = function_ref @$s11objc_thunks6HoozitC12copyPropertySo5GizmoCvg
  // CHECK-NEXT:   [[RES:%.*]] = apply [[FR]]([[BORROWED_SELF_COPY]])
  // CHECK-NEXT:   end_borrow [[BORROWED_SELF_COPY]]
  // CHECK-NEXT:   destroy_value [[SELF_COPY]]
  // CHECK-NEXT:   return [[RES]]
  // CHECK-NEXT: }

  // CHECK-LABEL: sil hidden [ossa] @$s11objc_thunks6HoozitC12copyPropertySo5GizmoCvg
  // CHECK: bb0(%0 : @guaranteed $Hoozit):
  // CHECK:        [[ADDR:%.*]] = ref_element_addr %0 : {{.*}}, #Hoozit.copyProperty
  // CHECK-NEXT:   [[READ:%.*]] = begin_access [read] [dynamic] [[ADDR]] : $*Gizmo
  // CHECK-NEXT:   [[RES:%.*]] = load [copy] [[READ]]
  // CHECK-NEXT:   end_access [[READ]] : $*Gizmo
  // CHECK-NEXT:   return [[RES]]

  // -- setter is normal
  // CHECK-LABEL: sil hidden [thunk] [ossa] @$s11objc_thunks6HoozitC12copyPropertySo5GizmoCvsTo : $@convention(objc_method) (Gizmo, Hoozit) -> () {
  // CHECK: bb0([[VALUE:%.*]] : @unowned $Gizmo, [[THIS:%.*]] : @unowned $Hoozit):
  // CHECK-NEXT:   [[VALUE_COPY:%.*]] = copy_value [[VALUE]]
  // CHECK-NEXT:   [[THIS_COPY:%.*]] = copy_value [[THIS]]
  // CHECK-NEXT:   [[BORROWED_THIS_COPY:%.*]] = begin_borrow [[THIS_COPY]]
  // CHECK-NEXT:   // function_ref objc_thunks.Hoozit.copyProperty.setter
  // CHECK-NEXT:   [[FR:%.*]] = function_ref @$s11objc_thunks6HoozitC12copyPropertySo5GizmoCvs
  // CHECK-NEXT:   [[RES:%.*]] = apply [[FR]]([[VALUE_COPY]], [[BORROWED_THIS_COPY]])
  // CHECK-NEXT:   end_borrow [[BORROWED_THIS_COPY]]
  // CHECK-NEXT:   destroy_value [[THIS_COPY]]
  // CHECK-NEXT:   return [[RES]]

  // CHECK-LABEL: sil hidden [ossa] @$s11objc_thunks6HoozitC12copyPropertySo5GizmoCvs
  // CHECK: bb0([[ARG1:%.*]] : @owned $Gizmo, [[SELF:%.*]] : @guaranteed $Hoozit):
  // CHECK:   [[BORROWED_ARG1:%.*]] = begin_borrow [[ARG1]]
  // CHECK:   [[ARG1_COPY:%.*]] = copy_value [[BORROWED_ARG1]]
  // CHECK:   [[ADDR:%.*]] = ref_element_addr [[SELF]] : {{.*}}, #Hoozit.copyProperty
  // CHECK:   [[WRITE:%.*]] = begin_access [modify] [dynamic] [[ADDR]] : $*Gizmo
  // CHECK:   assign [[ARG1_COPY]] to [[WRITE]]
  // CHECK:   end_access [[WRITE]] : $*Gizmo
  // CHECK:   end_borrow [[BORROWED_ARG1]]
  // CHECK:   destroy_value [[ARG1]]
  // CHECK: } // end sil function '$s11objc_thunks6HoozitC12copyPropertySo5GizmoCvs'

  // expected-remark@+2 {{access note for fancy test suite adds attribute 'objc' to this property}}
  // expected-note@+1 {{add attribute explicitly to silence this warning}} {{3-3=@objc }}
  var roProperty: Gizmo { return self }
  // -- getter
  // CHECK-LABEL: sil hidden [thunk] [ossa] @$s11objc_thunks6HoozitC10roPropertySo5GizmoCvgTo : $@convention(objc_method) (Hoozit) -> @autoreleased Gizmo {
  // CHECK: bb0([[THIS:%.*]] : @unowned $Hoozit):
  // CHECK-NEXT:   [[THIS_COPY:%.*]] = copy_value [[THIS]]
  // CHECK-NEXT:   [[BORROWED_THIS_COPY:%.*]] = begin_borrow [[THIS_COPY]]
  // CHECK-NEXT:   // function_ref
  // CHECK-NEXT:   [[NATIVE:%.*]] = function_ref @$s11objc_thunks6HoozitC10roPropertySo5GizmoCvg : $@convention(method) (@guaranteed Hoozit) -> @owned Gizmo
  // CHECK-NEXT:   [[RES:%.*]] = apply [[NATIVE]]([[BORROWED_THIS_COPY]])
  // CHECK-NEXT:   end_borrow [[BORROWED_THIS_COPY]]
  // CHECK-NEXT:   destroy_value [[THIS_COPY]] : $Hoozit
  // CHECK-NEXT:   return [[RES]] : $Gizmo
  // CHECK-NEXT: } // end sil function '$s11objc_thunks6HoozitC10roPropertySo5GizmoCvgTo'

  // -- no setter
  // CHECK-NOT: sil hidden [thunk] [ossa] @$s11objc_thunks6HoozitC10roPropertySo5GizmoCvsTo

  // expected-remark@+2 {{access note for fancy test suite adds attribute 'objc' to this property}}
  // expected-note@+1 {{add attribute explicitly to silence this warning}} {{3-3=@objc }}
  var rwProperty: Gizmo {
    get {
      return self
    }
    set {}
  }
  // -- getter
  // CHECK-LABEL: sil hidden [thunk] [ossa] @$s11objc_thunks6HoozitC10rwPropertySo5GizmoCvgTo : $@convention(objc_method) (Hoozit) -> @autoreleased Gizmo

  // -- setter
  // CHECK-LABEL: sil hidden [thunk] [ossa] @$s11objc_thunks6HoozitC10rwPropertySo5GizmoCvsTo : $@convention(objc_method) (Gizmo, Hoozit) -> () {
  // CHECK: bb0([[VALUE:%.*]] : @unowned $Gizmo, [[THIS:%.*]] : @unowned $Hoozit):
  // CHECK-NEXT:   [[VALUE_COPY:%.*]] = copy_value [[VALUE]]
  // CHECK-NEXT:   [[THIS_COPY:%.*]] = copy_value [[THIS]]
  // CHECK-NEXT:   [[BORROWED_THIS_COPY:%.*]] = begin_borrow [[THIS_COPY]]
  // CHECK-NEXT:   // function_ref
  // CHECK-NEXT:   [[NATIVE:%.*]] = function_ref @$s11objc_thunks6HoozitC10rwPropertySo5GizmoCvs : $@convention(method) (@owned Gizmo, @guaranteed Hoozit) -> ()
  // CHECK-NEXT:   apply [[NATIVE]]([[VALUE_COPY]], [[BORROWED_THIS_COPY]])
  // CHECK-NEXT:   end_borrow [[BORROWED_THIS_COPY]]
  // CHECK-NEXT:   destroy_value [[THIS_COPY]]
  // CHECK-NEXT:   return
  // CHECK-NEXT: }

  // expected-remark@+2 {{access note for fancy test suite adds attribute 'objc' to this property}}
  // expected-note@+1 {{add attribute explicitly to silence this warning}} {{3-3=@objc }}
  var copyRWProperty: Gizmo {
    get {
      return self
    }
    set {}
  }
  // -- getter
  // CHECK-LABEL: sil hidden [thunk] [ossa] @$s11objc_thunks6HoozitC14copyRWPropertySo5GizmoCvgTo : $@convention(objc_method) (Hoozit) -> @owned Gizmo {
  // CHECK: bb0([[THIS:%.*]] : @unowned $Hoozit):
  // CHECK-NEXT:   [[THIS_COPY:%.*]] = copy_value [[THIS]]
  // CHECK-NEXT:   [[BORROWED_THIS_COPY:%.*]] = begin_borrow [[THIS_COPY]]
  // CHECK-NEXT:   // function_ref
  // CHECK-NEXT:   [[NATIVE:%.*]] = function_ref @$s11objc_thunks6HoozitC14copyRWPropertySo5GizmoCvg : $@convention(method) (@guaranteed Hoozit) -> @owned Gizmo
  // CHECK-NEXT:   [[RES:%.*]] = apply [[NATIVE]]([[BORROWED_THIS_COPY]])
  // CHECK-NEXT:   end_borrow [[BORROWED_THIS_COPY]]
  // CHECK-NEXT:   destroy_value [[THIS_COPY]]
  // CHECK-NOT:    return
  // CHECK-NEXT:   return [[RES]]
  // CHECK-NEXT: }

  // -- setter is normal
  // CHECK-LABEL: sil hidden [thunk] [ossa] @$s11objc_thunks6HoozitC14copyRWPropertySo5GizmoCvsTo : $@convention(objc_method) (Gizmo, Hoozit) -> () {
  // CHECK: bb0([[VALUE:%.*]] : @unowned $Gizmo, [[THIS:%.*]] : @unowned $Hoozit):
  // CHECK-NEXT:   [[VALUE_COPY:%.*]] = copy_value [[VALUE]]
  // CHECK-NEXT:   [[THIS_COPY:%.*]] = copy_value [[THIS]]
  // CHECK-NEXT:   [[BORROWED_THIS_COPY:%.*]] = begin_borrow [[THIS_COPY]]
  // CHECK-NEXT:   // function_ref
  // CHECK-NEXT:   [[NATIVE:%.*]] = function_ref @$s11objc_thunks6HoozitC14copyRWPropertySo5GizmoCvs : $@convention(method) (@owned Gizmo, @guaranteed Hoozit) -> ()
  // CHECK-NEXT:   apply [[NATIVE]]([[VALUE_COPY]], [[BORROWED_THIS_COPY]])
  // CHECK-NEXT:   end_borrow [[BORROWED_THIS_COPY]]
  // CHECK-NEXT:   destroy_value [[THIS_COPY]]
  // CHECK-NEXT:   return
  // CHECK-NEXT: }

  // expected-remark@+2 {{access note for fancy test suite adds attribute 'objc' to this property}}
  // expected-note@+1 {{add attribute explicitly to silence this warning}} {{3-3=@objc }}
  var initProperty: Gizmo
  // -- getter
  // CHECK-LABEL: sil hidden [thunk] [ossa] @$s11objc_thunks6HoozitC12initPropertySo5GizmoCvgTo : $@convention(objc_method) (Hoozit) -> @autoreleased Gizmo {
  // CHECK: bb0([[THIS:%.*]] : @unowned $Hoozit):
  // CHECK-NEXT:   [[THIS_COPY:%.*]] = copy_value [[THIS]]
  // CHECK-NEXT:   [[BORROWED_THIS_COPY:%.*]] = begin_borrow [[THIS_COPY]]
  // CHECK-NEXT:   // function_ref
  // CHECK-NEXT:   [[NATIVE:%.*]] = function_ref @$s11objc_thunks6HoozitC12initPropertySo5GizmoCvg : $@convention(method) (@guaranteed Hoozit) -> @owned Gizmo
  // CHECK-NEXT:   [[RES:%.*]] = apply [[NATIVE]]([[BORROWED_THIS_COPY]])
  // CHECK-NEXT:   end_borrow [[BORROWED_THIS_COPY]]
  // CHECK-NEXT:   destroy_value [[THIS_COPY]]
  // CHECK-NEXT:   return [[RES]]
  // CHECK-NEXT: }

  // -- setter
  // CHECK-LABEL: sil hidden [thunk] [ossa] @$s11objc_thunks6HoozitC12initPropertySo5GizmoCvsTo : $@convention(objc_method) (Gizmo, Hoozit) -> () {
  // CHECK: bb0([[VALUE:%.*]] : @unowned $Gizmo, [[THIS:%.*]] : @unowned $Hoozit):
  // CHECK-NEXT:   [[VALUE_COPY:%.*]] = copy_value [[VALUE]]
  // CHECK-NEXT:   [[THIS_COPY:%.*]] = copy_value [[THIS]]
  // CHECK-NEXT:   [[BORROWED_THIS_COPY:%.*]] = begin_borrow [[THIS_COPY]]
  // CHECK-NEXT:   // function_ref
  // CHECK-NEXT:   [[NATIVE:%.*]] = function_ref @$s11objc_thunks6HoozitC12initPropertySo5GizmoCvs : $@convention(method) (@owned Gizmo, @guaranteed Hoozit) -> ()
  // CHECK-NEXT:   apply [[NATIVE]]([[VALUE_COPY]], [[BORROWED_THIS_COPY]])
  // CHECK-NEXT:   end_borrow [[BORROWED_THIS_COPY]]
  // CHECK-NEXT:   destroy_value [[THIS_COPY]]
  // CHECK-NEXT:   return
  // CHECK-NEXT: }

  // expected-remark@+2 {{access note for fancy test suite adds attribute 'objc' to this property}}
  // expected-note@+1 {{add attribute explicitly to silence this warning}} {{3-3=@objc }}
  var propComputed: Gizmo {
    // expected-remark@+2 {{access note for fancy test suite adds attribute 'objc' to this getter}}
    // expected-note@+1 {{add attribute explicitly to silence this warning}} {{5-5=@objc(initPropComputedGetter) }}
    get { return self }
    // expected-remark@+2 {{access note for fancy test suite adds attribute 'objc' to this setter}}
    // expected-note@+1 {{add attribute explicitly to silence this warning}} {{5-5=@objc(initPropComputedSetter:) }}
    set {}
  }
  // -- getter
  // CHECK-LABEL: sil hidden [thunk] [ossa] @$s11objc_thunks6HoozitC12propComputedSo5GizmoCvgTo : $@convention(objc_method) (Hoozit) -> @autoreleased Gizmo {
  // CHECK: bb0([[THIS:%.*]] : @unowned $Hoozit):
  // CHECK-NEXT:   [[THIS_COPY:%.*]] = copy_value [[THIS]]
  // CHECK-NEXT:   [[BORROWED_THIS_COPY:%.*]] = begin_borrow [[THIS_COPY]]
  // CHECK-NEXT:   // function_ref
  // CHECK-NEXT:   [[NATIVE:%.*]] = function_ref @$s11objc_thunks6HoozitC12propComputedSo5GizmoCvg : $@convention(method) (@guaranteed Hoozit) -> @owned Gizmo
  // CHECK-NEXT:   [[RES:%.*]] = apply [[NATIVE]]([[BORROWED_THIS_COPY]])
  // CHECK-NEXT:   end_borrow [[BORROWED_THIS_COPY]]
  // CHECK-NEXT:   destroy_value [[THIS_COPY]]
  // CHECK-NEXT:   return [[RES]]
  // CHECK-NEXT: }

  // -- setter
  // CHECK-LABEL: sil hidden [thunk] [ossa] @$s11objc_thunks6HoozitC12propComputedSo5GizmoCvsTo : $@convention(objc_method) (Gizmo, Hoozit) -> () {
  // CHECK: bb0([[VALUE:%.*]] : @unowned $Gizmo, [[THIS:%.*]] : @unowned $Hoozit):
  // CHECK-NEXT:   [[VALUE_COPY:%.*]] = copy_value [[VALUE]]
  // CHECK-NEXT:   [[THIS_COPY:%.*]] = copy_value [[THIS]]
  // CHECK-NEXT:   [[BORROWED_THIS_COPY:%.*]] = begin_borrow [[THIS_COPY]]
  // CHECK-NEXT:   // function_ref
  // CHECK-NEXT:   [[NATIVE:%.*]] = function_ref @$s11objc_thunks6HoozitC12propComputedSo5GizmoCvs : $@convention(method) (@owned Gizmo, @guaranteed Hoozit) -> ()
  // CHECK-NEXT:   apply [[NATIVE]]([[VALUE_COPY]], [[BORROWED_THIS_COPY]])
  // CHECK-NEXT:   end_borrow [[BORROWED_THIS_COPY]]
  // CHECK-NEXT:   destroy_value [[THIS_COPY]]
  // CHECK-NEXT:   return
  // CHECK-NEXT: }

  // Don't export generics to ObjC yet
  func generic<T>(_ x: T) {}
  // CHECK-NOT: sil hidden [thunk] [ossa] @_TToFC11objc_thunks6Hoozit7generic{{.*}}

  // Constructor.
  // CHECK-LABEL: sil hidden [ossa] @$s11objc_thunks6HoozitC7bellsOnACSi_tcfc : $@convention(method) (Int, @owned Hoozit) -> @owned Hoozit {
  // CHECK: [[SELF_BOX:%[0-9]+]] = alloc_box ${ var Hoozit }
  // CHECK: [[MARKED_SELF_BOX:%[0-9]+]] = mark_uninitialized [derivedself] [[SELF_BOX]]
  // CHECK: [[PB_BOX:%.*]] = project_box [[MARKED_SELF_BOX]]
  // CHECK: [[GIZMO:%[0-9]+]] = upcast [[SELF:%[0-9]+]] : $Hoozit to $Gizmo
  // CHECK: [[BORROWED_GIZMO:%.*]] = begin_borrow [[GIZMO]]
  // CHECK: [[CAST_BORROWED_GIZMO:%.*]] = unchecked_ref_cast [[BORROWED_GIZMO]] : $Gizmo to $Hoozit
  // CHECK: [[SUPERMETHOD:%[0-9]+]] = objc_super_method [[CAST_BORROWED_GIZMO]] : $Hoozit, #Gizmo.init!initializer.foreign : (Gizmo.Type) -> (Int) -> Gizmo?, $@convention(objc_method) (Int, @owned Gizmo) -> @owned Optional<Gizmo>
  // CHECK-NEXT: end_borrow [[BORROWED_GIZMO]]
  // CHECK-NEXT: [[SELF_REPLACED:%[0-9]+]] = apply [[SUPERMETHOD]](%0, [[X:%[0-9]+]]) : $@convention(objc_method) (Int, @owned Gizmo) -> @owned Optional<Gizmo>
  // CHECK-NOT: unconditional_checked_cast downcast [[SELF_REPLACED]] : $Gizmo to Hoozit
  // CHECK: unchecked_ref_cast
  // CHECK: return
  override init(bellsOn x : Int) {
    super.init(bellsOn: x)
    other()
  }

  // Subscript
  // expected-remark@+2 {{access note for fancy test suite adds attribute 'objc' to this subscript}}
  // expected-note@+1 {{add attribute explicitly to silence this warning}} {{3-3=@objc }}
  subscript (i: Int) -> Hoozit {
  // Getter
  // CHECK-LABEL: sil hidden [thunk] [ossa] @$s11objc_thunks6HoozitCyACSicigTo : $@convention(objc_method) (Int, Hoozit) -> @autoreleased Hoozit
  // CHECK: bb0([[I:%[0-9]+]] : $Int, [[SELF:%[0-9]+]] : @unowned $Hoozit):
  // CHECK-NEXT: [[SELF_COPY:%.*]] = copy_value [[SELF]] : $Hoozit
  // CHECK-NEXT: [[BORROWED_SELF_COPY:%.*]] = begin_borrow [[SELF_COPY]]
  // CHECK-NEXT: // function_ref
  // CHECK-NEXT: [[NATIVE:%[0-9]+]] = function_ref @$s11objc_thunks6HoozitCyACSicig : $@convention(method) (Int, @guaranteed Hoozit) -> @owned Hoozit
  // CHECK-NEXT: [[RESULT:%[0-9]+]] = apply [[NATIVE]]([[I]], [[BORROWED_SELF_COPY]]) : $@convention(method) (Int, @guaranteed Hoozit) -> @owned Hoozit
  // CHECK-NEXT: end_borrow [[BORROWED_SELF_COPY]]
  // CHECK-NEXT: destroy_value [[SELF_COPY]]
  // CHECK-NEXT: return [[RESULT]] : $Hoozit
  get {
    return self
  }

  // Setter
  // CHECK-LABEL: sil hidden [thunk] [ossa] @$s11objc_thunks6HoozitCyACSicisTo : $@convention(objc_method) (Hoozit, Int, Hoozit) -> ()
  // CHECK: bb0([[VALUE:%[0-9]+]] : @unowned $Hoozit, [[I:%[0-9]+]] : $Int, [[SELF:%[0-9]+]] : @unowned $Hoozit):
  // CHECK:   [[VALUE_COPY:%.*]] = copy_value [[VALUE]] : $Hoozit
  // CHECK:   [[SELF_COPY:%.*]] = copy_value [[SELF]] : $Hoozit
  // CHECK:   [[BORROWED_SELF_COPY:%.*]] = begin_borrow [[SELF_COPY]]
  // CHECK:   [[NATIVE:%[0-9]+]] = function_ref @$s11objc_thunks6HoozitCyACSicis : $@convention(method) (@owned Hoozit, Int, @guaranteed Hoozit) -> ()
  // CHECK:   [[RESULT:%[0-9]+]] = apply [[NATIVE]]([[VALUE_COPY]], [[I]], [[BORROWED_SELF_COPY]]) : $@convention(method) (@owned Hoozit, Int, @guaranteed Hoozit) -> ()
  // CHECK:   end_borrow [[BORROWED_SELF_COPY]]
  // CHECK:   destroy_value [[SELF_COPY]]
  // CHECK:   return [[RESULT]] : $()
  // CHECK: } // end sil function '$s11objc_thunks6HoozitCyACSicisTo'
  set {}
  }
}

class Wotsit<T> : Gizmo {
  // CHECK-LABEL: sil hidden [thunk] [ossa] @$s11objc_thunks6WotsitC5plainyyFTo : $@convention(objc_method) <T> (Wotsit<T>) -> () {
  // CHECK: bb0([[SELF:%.*]] : @unowned $Wotsit<T>):
  // CHECK-NEXT: [[SELF_COPY:%.*]] = copy_value [[SELF]] : $Wotsit<T>
  // CHECK-NEXT: [[BORROWED_SELF_COPY:%.*]] = begin_borrow [[SELF_COPY]]
  // CHECK-NEXT: // function_ref
  // CHECK-NEXT: [[NATIVE:%.*]] = function_ref @$s11objc_thunks6WotsitC5plainyyF : $@convention(method) <τ_0_0> (@guaranteed Wotsit<τ_0_0>) -> ()
  // CHECK-NEXT: [[RESULT:%.*]] = apply [[NATIVE]]<T>([[BORROWED_SELF_COPY]]) : $@convention(method) <τ_0_0> (@guaranteed Wotsit<τ_0_0>) -> ()
  // CHECK-NEXT: end_borrow [[BORROWED_SELF_COPY]]
  // CHECK-NEXT: destroy_value [[SELF_COPY]] : $Wotsit<T>
  // CHECK-NEXT: return [[RESULT]] : $()
  // CHECK-NEXT: }
  @objc(plain)
  func plain() { }
  // expected-remark@-2 {{access note for fancy test suite changes the '@objc' name of this instance method to 'fancy', but source code specifies 'plain'; the access note will be ignored}}

  func generic<U>(_ x: U) {}

  var property : T

  init(t: T) {
    self.property = t
    super.init()
  }

  // CHECK-LABEL: sil hidden [thunk] [ossa] @$s11objc_thunks6WotsitC11descriptionSSvgTo : $@convention(objc_method) <T> (Wotsit<T>) -> @autoreleased NSString {
  // CHECK: bb0([[SELF:%.*]] : @unowned $Wotsit<T>):
  // CHECK-NEXT:   [[SELF_COPY:%.*]] = copy_value [[SELF]] : $Wotsit<T>
  // CHECK-NEXT:   [[BORROWED_SELF_COPY:%.*]] = begin_borrow [[SELF_COPY]]
  // CHECK-NEXT:   // function_ref
  // CHECK-NEXT:   [[NATIVE:%.*]] = function_ref @$s11objc_thunks6WotsitC11descriptionSSvg : $@convention(method) <τ_0_0> (@guaranteed Wotsit<τ_0_0>) -> @owned String
  // CHECK-NEXT:   [[RESULT:%.*]] = apply [[NATIVE:%.*]]<T>([[BORROWED_SELF_COPY]]) : $@convention(method) <τ_0_0> (@guaranteed Wotsit<τ_0_0>) -> @owned String
  // CHECK-NEXT:   end_borrow [[BORROWED_SELF_COPY]]
  // CHECK-NEXT:   destroy_value [[SELF_COPY]] : $Wotsit<T>
  // CHECK-NEXT:   // function_ref
  // CHECK-NEXT:   [[BRIDGE:%.*]] = function_ref @$sSS10FoundationE19_bridgeToObjectiveCSo8NSStringCyF
  // CHECK-NEXT:   [[BORROWED_RESULT:%.*]] = begin_borrow [[RESULT]]
  // CHECK-NEXT:   [[NSRESULT:%.*]] = apply [[BRIDGE]]([[BORROWED_RESULT]]) : $@convention(method) (@guaranteed String) -> @owned NSString
  // CHECK-NEXT:   end_borrow [[BORROWED_RESULT]]
  // CHECK-NEXT:   destroy_value [[RESULT]]
  // CHECK-NEXT:   return [[NSRESULT]] : $NSString
  // CHECK-NEXT: }
  override var description : String {
    return "Hello, world."
  }

  // CHECK-LABEL: sil hidden [thunk] [ossa] @$s11objc_thunks6WotsitCACyxGSgycfcTo : $@convention(objc_method) <T> (@owned Wotsit<T>) -> @owned Optional<Wotsit<T>>

  // CHECK-LABEL: sil hidden [thunk] [ossa] @$s11objc_thunks6WotsitC7bellsOnACyxGSgSi_tcfcTo : $@convention(objc_method) <T> (Int, @owned Wotsit<T>) -> @owned Optional<Wotsit<T>>

  // Ivar destroyer
  // CHECK-LABEL: sil hidden [ossa] @$s11objc_thunks6WotsitCfETo

}

// CHECK-NOT: sil hidden [thunk] [ossa] @_TToF{{.*}}Wotsit{{.*}}

// Extension initializers, properties and methods need thunks too.
extension Hoozit {
  // CHECK-LABEL: sil hidden [thunk] [ossa] @$s11objc_thunks6HoozitC3intACSi_tcfcTo : $@convention(objc_method) (Int, @owned Hoozit) -> @owned Hoozit
  convenience init(int i: Int) { self.init(bellsOn: i) }
  // expected-remark@-1 {{access note for fancy test suite adds attribute 'objc' to this initializer}}
  // expected-note@-2 {{add attribute explicitly to silence this warning}} {{3-3=@objc }}
  // expected-remark@-3 {{access note for fancy test suite adds modifier 'dynamic' to this initializer}}
  // expected-note@-4 {{add modifier explicitly to silence this warning}} {{3-3=dynamic }}

  // CHECK-LABEL: sil hidden [ossa] @$s11objc_thunks6HoozitC6doubleACSd_tcfC : $@convention(method) (Double, @thick Hoozit.Type) -> @owned Hoozit
  convenience init(double d: Double) {
    var x = X()
    self.init(int:Int(d))
    other()
  }

  // expected-remark@+2 {{access note for fancy test suite adds attribute 'objc' to this instance method}}
  // expected-note@+1 {{add attribute explicitly to silence this warning}} {{3-3=@objc }}
  func foof() {}
  // CHECK-LABEL: sil hidden [thunk] [ossa] @$s11objc_thunks6HoozitC4foofyyFTo : $@convention(objc_method) (Hoozit) -> () {

  var extensionProperty: Int { return 0 }
  // CHECK-LABEL: sil hidden [ossa] @$s11objc_thunks6HoozitC17extensionPropertySivg : $@convention(method) (@guaranteed Hoozit) -> Int
}

// Calling objc methods of subclass should go through native entry points
func useHoozit(_ h: Hoozit) {
// sil [ossa] @$s11objc_thunks9useHoozit1hyAA0D0C_tF
  // In the class decl, overrides importd method, 'dynamic' was inferred
  h.fork()
  // CHECK: objc_method {{%.*}} : {{.*}}, #Hoozit.fork!foreign

  // In an extension, 'dynamic' was inferred.
  h.foof()
  // CHECK: objc_method {{%.*}} : {{.*}}, #Hoozit.foof!foreign
}

func useWotsit(_ w: Wotsit<String>) {
// sil [ossa] @$s11objc_thunks9useWotsit1wySo0D0CySSG_tF
  w.plain()
  // CHECK: class_method {{%.*}} : {{.*}}, #Wotsit.plain :
  w.generic(2)
  // CHECK: class_method {{%.*}} : {{.*}}, #Wotsit.generic :

  // Inherited methods only have objc entry points
  w.clone()
  // CHECK: objc_method {{%.*}} : {{.*}}, #Gizmo.clone!foreign
}

func other() { }

class X { }

// CHECK-LABEL: sil hidden [ossa] @$s11objc_thunks8propertyySiSo5GizmoCF
func property(_ g: Gizmo) -> Int {
  // CHECK: bb0([[ARG:%.*]] : @guaranteed $Gizmo):
  // CHECK:   objc_method [[ARG]] : $Gizmo, #Gizmo.count!getter.foreign
  return g.count
}

// CHECK-LABEL: sil hidden [ossa] @$s11objc_thunks13blockPropertyyySo5GizmoCF
func blockProperty(_ g: Gizmo) {
  // CHECK: bb0([[ARG:%.*]] : @guaranteed $Gizmo):
  // CHECK:   objc_method [[ARG]] : $Gizmo, #Gizmo.block!setter.foreign
  g.block = { }
  // CHECK:   objc_method [[ARG]] : $Gizmo, #Gizmo.block!getter.foreign
  g.block()
}

class DesignatedStubs : Gizmo {
  var i: Int

  override init() { i = 5 }

  // CHECK-LABEL: sil hidden [ossa] @$s11objc_thunks15DesignatedStubsC7bellsOnACSgSi_tcfc
  // CHECK: string_literal utf8 "objc_thunks.DesignatedStubs"
  // CHECK: string_literal utf8 "init(bellsOn:)"
  // CHECK: string_literal utf8 "{{.*}}objc_access_notes.swift"
  // CHECK: function_ref @$ss25_unimplementedInitializer9className04initD04file4line6columns5NeverOs12StaticStringV_A2JS2utF
  // CHECK: return

  // CHECK-NOT: sil hidden [ossa] @_TFCSo15DesignatedStubsc{{.*}}
}

class DesignatedOverrides : Gizmo {
  var i: Int = 5

  // CHECK-LABEL: sil hidden [ossa] @$s11objc_thunks19DesignatedOverridesCACSgycfc
  // CHECK-NOT: return
  // CHECK: function_ref @$s11objc_thunks19DesignatedOverridesC1iSivpfi : $@convention(thin) () -> Int
  // CHECK: objc_super_method [[SELF:%[0-9]+]] : $DesignatedOverrides, #Gizmo.init!initializer.foreign : (Gizmo.Type) -> () -> Gizmo?, $@convention(objc_method) (@owned Gizmo) -> @owned Optional<Gizmo>
  // CHECK: return

  // CHECK-LABEL: sil hidden [ossa] @$s11objc_thunks19DesignatedOverridesC7bellsOnACSgSi_tcfc
  // CHECK: function_ref @$s11objc_thunks19DesignatedOverridesC1iSivpfi : $@convention(thin) () -> Int
  // CHECK: objc_super_method [[SELF:%[0-9]+]] : $DesignatedOverrides, #Gizmo.init!initializer.foreign : (Gizmo.Type) -> (Int) -> Gizmo?, $@convention(objc_method) (Int, @owned Gizmo) -> @owned Optional<Gizmo>
  // CHECK: return
}

// Make sure we copy blocks passed in as IUOs - <rdar://problem/22471309>

func registerAnsible() {
  // CHECK: function_ref @$s11objc_thunks15registerAnsibleyyFyyycSgcfU_
  Ansible.anseAsync({ completion in completion!() })
}
@_silgen_name("noescape")
func noescape(f: @convention(block) () -> ())

// CHECK: sil hidden [ossa] @$s11objc_thunks21testObjCNoescapeThunkyyF : $@convention(thin) () -> () {
// CHECK: [[REABSTRACT:%.*]] = function_ref @$sIeg_IyB_TR
// CHECK: init_block_storage_header {{.*}} : $*@block_storage @callee_guaranteed () -> (), invoke [[REABSTRACT]]
// CHECK: return
func testObjCNoescapeThunk() {
  noescape {
  }
}

// Noescape verification relies on there not being a retain/release in order to
// work in the presence of a objective c throwing implementation function.
// CHECK: sil {{.*}} [ossa] @$sIeg_IyB_TR
// CHECK: bb0([[T0:%.*]] : $*@block_storage @callee_guaranteed () -> ()):
// CHECK-NEXT:  [[T1:%.*]] = project_block_storage [[T0]]
// CHECK-NEXT:  [[T2:%.*]] = load_borrow [[T1]]
// CHECK-NEXT:  [[T3:%.*]] = apply [[T2]]()
// CHECK-NEXT:  [[T4:%.*]] = tuple ()
// CHECK-NEXT:  end_borrow [[T2]]
// CHECK-NEXT:  return [[T4]]

// Call a convenience initializer
func buildGizmo1() -> Gizmo {
  return Gizmo(outBells: 10)
}

func buildGizmo2() -> Gizmo {
  return Gizmo(whistles: 10)
}
