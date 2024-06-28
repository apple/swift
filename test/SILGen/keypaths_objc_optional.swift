// RUN: %target-swift-emit-silgen -import-objc-header %swift_src_root/test/Inputs/ObjCOptionalRequirements.h %s | %FileCheck  %s --check-prefix=CHECK --check-prefix=CHECK-%target-os-%target-cpu
// RUN: %target-swift-emit-ir -import-objc-header %swift_src_root/test/Inputs/ObjCOptionalRequirements.h %s

// REQUIRES: objc_interop

import Foundation

@objc class Object: NSObject {
  var name: String
  init(name: String) {
    self.name = name
  }
}
@objc protocol SwiftProtocol {
  @objc optional var object: Object { get set }

  @objc optional subscript(_: Bool) -> Object { get set }
}

// CHECK-LABEL: sil hidden [ossa] @{{.*}}testKeyPathAccessorsForOptionalStorageComponentsyyF
// CHECK: keypath $KeyPath<any SwiftProtocol, Optional<Object>>, (objc "object"; root $any SwiftProtocol; gettable_property $Optional<Object>,  id #SwiftProtocol.object!getter.foreign : <Self where Self : SwiftProtocol> (Self) -> () -> Object, getter @$[[SWIFT_PROP_GETTER:[_a-zA-Z0-9]+]]
// CHECK: keypath $KeyPath<any SwiftProtocol, Optional<Object>>, (root $any SwiftProtocol; gettable_property $Optional<Object>,  id #SwiftProtocol.subscript!getter.foreign : <Self where Self : SwiftProtocol> (Self) -> (Bool) -> Object, getter @$[[SWIFT_SUBSCR_GETTER:[_a-zA-Z0-9]+]]
// CHECK: keypath $ReferenceWritableKeyPath<any SwiftProtocol, String>, (root $any SwiftProtocol; gettable_property $Optional<Object>,  id #SwiftProtocol.object!getter.foreign : <Self where Self : SwiftProtocol> (Self) -> () -> Object, getter @$[[SWIFT_PROP_GETTER]] : {{.*}}; optional_force : $Object; settable_property $String,
// CHECK: keypath $ReferenceWritableKeyPath<any SwiftProtocol, String>, (root $any SwiftProtocol; gettable_property $Optional<Object>,  id #SwiftProtocol.subscript!getter.foreign : <Self where Self : SwiftProtocol> (Self) -> (Bool) -> Object, getter @$[[SWIFT_SUBSCR_GETTER]] : {{.*}}; optional_force : $Object; settable_property $String,
// CHECK: keypath $KeyPath<any ObjCProtocol, Optional<Bool>>, (objc "flag"; root $any ObjCProtocol; gettable_property $Optional<Bool>,  id #ObjCProtocol.flag!getter.foreign : <Self where Self : ObjCProtocol> (Self) -> () -> Bool, getter @$[[OBJC_PROP_GETTER:[_a-zA-Z0-9]+]]
// CHECK: } // end sil function '${{.*}}testKeyPathAccessorsForOptionalStorageComponentsyyF'
//
// CHECK: sil shared [thunk] [ossa] @$[[SWIFT_PROP_GETTER]] : $@convention(keypath_accessor_getter) (@in_guaranteed any SwiftProtocol) -> @out Optional<Object> {
// CHECK:   [[BASE:%[0-9]+]] = open_existential_ref {{%[0-9]+}} : $any SwiftProtocol to $[[OPENED_TY:@opened\("[-A-F0-9]+", any SwiftProtocol\) Self]]
// CHECK:   dynamic_method_br [[BASE]] : $[[OPENED_TY]], #SwiftProtocol.object!getter.foreign, bb1
// CHECK: bb1({{%[0-9]+}} : $@convention(objc_method) ([[OPENED_TY]]) -> @autoreleased Object)
// CHECK: } // end sil function '$[[SWIFT_PROP_GETTER]]'
//
// CHECK: sil shared [thunk] [ossa] @$[[SWIFT_SUBSCR_GETTER]] : $@convention(keypath_accessor_getter) (@in_guaranteed any SwiftProtocol, @in_guaranteed Bool) -> @out Optional<Object> {
// CHECK:   [[BASE:%[0-9]+]] = open_existential_ref {{%[0-9]+}} : $any SwiftProtocol to $[[OPENED_TY:@opened\("[-A-F0-9]+", any SwiftProtocol\) Self]]
// CHECK:   [[INDEX:%[0-9]+]] = load [trivial] {{%[0-9]+}} : $*Bool
// CHECK:   dynamic_method_br [[BASE]] : $[[OPENED_TY]], #SwiftProtocol.subscript!getter.foreign, bb1, bb2
// CHECK: bb1({{%[0-9]+}} : $@convention(objc_method) (ObjCBool, [[OPENED_TY]]) -> @autoreleased Object):
// CHECK:   apply {{%[0-9]+}}([[INDEX]]) : $@callee_guaranteed (Bool) -> @owned Object
// CHECK: bb2:
// CHECK: } // end sil function '$[[SWIFT_SUBSCR_GETTER]]'
//
// CHECK: sil shared [thunk] [ossa] @$[[OBJC_PROP_GETTER]] : $@convention(keypath_accessor_getter) (@in_guaranteed any ObjCProtocol) -> @out Optional<Bool> {
// CHECK: bb0([[OUT:%[0-9]+]] : $*Optional<Bool>,
// CHECK:   [[BASE:%[0-9]+]] = open_existential_ref {{%[0-9]+}} : $any ObjCProtocol to $[[OPENED_TY:@opened\("[-A-F0-9]+", any ObjCProtocol\) Self]]
// CHECK:   [[DEST:%[0-9]+]] = alloc_stack $Optional<Bool>
// CHECK:   dynamic_method_br [[BASE]] : $[[OPENED_TY]], #ObjCProtocol.flag!getter.foreign, bb1, bb2
// CHECK: bb1({{%[0-9]+}} : $@convention(objc_method) ([[OPENED_TY]]) -> {{ObjCBool|Bool}}):
// CHECK-macosx-x86_64: function_ref @${{.*}} : $@convention(thin) (@guaranteed @callee_guaranteed () -> ObjCBool) -> Bool
// CHECK:   inject_enum_addr [[DEST]] {{.*}} #Optional.some!enumelt
// CHECK:   br bb3
// CHECK: bb2:
// CHECK:   inject_enum_addr [[DEST]] {{.*}} #Optional.none!enumelt
// CHECK:   br bb3
// CHECK: bb3:
// CHECK:   [[TMP:%[0-9]+]] = load [trivial] [[DEST]]
// CHECK:   store [[TMP]] to [trivial] [[OUT]]
// CHECK: } // end sil function '$[[OBJC_PROP_GETTER]]'
func testKeyPathAccessorsForOptionalStorageComponents() {
  _ = \SwiftProtocol.object
  _ = \SwiftProtocol.[true]

  _ = \SwiftProtocol.object!.name
  _ = \SwiftProtocol.[true]!.name

  _ = \ObjCProtocol.flag
}

// CHECK-macosx-x86_64: sil [transparent] [serialized] {{.*}}@$s10ObjectiveC22_convertObjCBoolToBoolySbAA0cD0VF : $@convention(thin) (ObjCBool) -> Bool
