// RUN: %target-swift-frontend(mock-sdk: %clang-importer-sdk) -I %S/../IDE/Inputs/custom-modules %s -emit-sil -disable-copy-propagation | %FileCheck %s

// Using -disable-copy-propagation to pattern match against older SIL
// output. At least until -enable-copy-propagation has been around
// long enough in the same form to be worth rewriting CHECK lines.

// REQUIRES: objc_interop

import Foundation
import ImportAsMember.Class

// CHECK-LABEL: sil shared [serializable] [thunk] @$sSo4HiveC5queenABSgSo3BeeCSg_tcfCTO : $@convention(method) (@owned Optional<Bee>, @thick Hive.Type) -> @owned Optional<Hive>
func testInstanceTypeFactoryMethod(queen: Bee) {
  // CHECK: bb0([[QUEEN:%[0-9]+]] : $Optional<Bee>, [[HIVE_META:%[0-9]+]] : $@thick Hive.Type):
  // CHECK-NEXT:   [[HIVE_META_OBJC:%[0-9]+]] = thick_to_objc_metatype [[HIVE_META]] : $@thick Hive.Type to $@objc_metatype Hive.Type
  // CHECK-NEXT:   [[FACTORY:%[0-9]+]] = objc_method [[HIVE_META_OBJC]] : $@objc_metatype Hive.Type, #Hive.init!allocator.foreign : (Hive.Type) -> (Bee?) -> Hive?, $@convention(objc_method) (Optional<Bee>, @objc_metatype Hive.Type) -> @autoreleased Optional<Hive>
  // CHECK-NEXT:   [[HIVE:%[0-9]+]] = apply [[FACTORY]]([[QUEEN]], [[HIVE_META_OBJC]]) : $@convention(objc_method) (Optional<Bee>, @objc_metatype Hive.Type) -> @autoreleased Optional<Hive>
  // CHECK-NEXT:   release_value [[QUEEN]]
  // CHECK-NEXT:   return [[HIVE]] : $Optional<Hive>
  var hive1 = Hive(queen: queen)
}

extension Hive {
  // CHECK-LABEL: sil hidden @$sSo4HiveC027definite_init_objc_factory_C0E10otherQueenABSo3BeeC_tcfC
  convenience init(otherQueen other: Bee) {
    // CHECK: bb0({{.*}}, [[META:%.*]] : $@thick Hive.Type)
    // CHECK: [[SELF_ADDR:%[0-9]+]] = alloc_stack $Hive
    // CHECK: [[OBJC_META:%[0-9]+]] = thick_to_objc_metatype [[META]] : $@thick Hive.Type to $@objc_metatype Hive.Type
    // CHECK: [[FACTORY:%[0-9]+]] = objc_method [[OBJC_META]] : $@objc_metatype Hive.Type, #Hive.init!allocator.foreign : (Hive.Type) -> (Bee?) -> Hive?, $@convention(objc_method) (Optional<Bee>, @objc_metatype Hive.Type) -> @autoreleased Optional<Hive>
    // CHECK: apply [[FACTORY]]([[QUEEN:%[0-9]+]], [[OBJC_META]]) : $@convention(objc_method) (Optional<Bee>, @objc_metatype Hive.Type) -> @autoreleased Optional<Hive>
    // CHECK: store [[NEW_SELF:%[0-9]+]] to [[SELF_ADDR]]
    // CHECK: dealloc_stack [[SELF_ADDR]]
    // CHECK: return [[NEW_SELF]]
    self.init(queen: other)
  }

  convenience init(otherFlakyQueen other: Bee) throws {
    try self.init(flakyQueen: other)
  }
}

extension SomeClass {
  // SIL-LABEL: sil hidden @_TFE16import_as_memberCSo9SomeClasscfT6doubleSd_S0_
  // SIL: bb0([[DOUBLE:%[0-9]+]] : $Double
  // SIL-NOT: value_metatype
  // SIL: [[FNREF:%[0-9]+]] = function_ref @MakeIAMSomeClass
  // SIL: apply [[FNREF]]([[DOUBLE]])
  convenience init(double: Double) {
    self.init(value: double)
  }
}

class SubHive : Hive {
  // CHECK-LABEL: sil hidden @$s027definite_init_objc_factory_B07SubHiveC20delegatesToInheritedACyt_tcfC
  convenience init(delegatesToInherited: ()) {
    // CHECK: bb0([[METATYPE:%.*]] : $@thick SubHive.Type)
    // CHECK: [[UPMETA:%.*]] = upcast [[METATYPE]]
    // CHECK: [[OBJC:%.*]] = thick_to_objc_metatype [[UPMETA]] : $@thick Hive.Type to $@objc_metatype Hive.Type
    // CHECK: [[METHOD:%.*]] = objc_method [[OBJC]] : $@objc_metatype Hive.Type, #Hive.init!allocator.foreign : (Hive.Type) -> (Bee?) -> Hive?
    // CHECK: apply [[METHOD]]({{.*}}, [[OBJC]])

    // CHECK: return {{%.*}} : $SubHive
    self.init(queen: Bee())
  }
}
