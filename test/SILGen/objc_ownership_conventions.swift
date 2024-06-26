// RUN: %target-swift-emit-silgen -module-name objc_ownership_conventions -sdk %S/Inputs -I %S/Inputs -enable-source-import %s -enable-objc-interop | %FileCheck %s

import gizmo

// CHECK-LABEL: sil hidden [ossa] @$s26objc_ownership_conventions5test3So8NSObjectCyF
func test3() -> NSObject {
  // initializer returns at +1
  return Gizmo()
  // CHECK: [[GIZMO_META:%[0-9]+]] = metatype $@thick Gizmo.Type
  // CHECK: [[CTOR:%[0-9]+]] = function_ref @$sSo5GizmoC{{[_0-9a-zA-Z]*}}fC : $@convention(method) (@thick Gizmo.Type) -> @owned Optional<Gizmo>
  // CHECK-NEXT: [[GIZMO:%[0-9]+]] = apply [[CTOR]]([[GIZMO_META]]) : $@convention(method) (@thick Gizmo.Type) -> @owned Optional<Gizmo>
  // CHECK: [[GIZMO_NS:%[0-9]+]] = upcast [[GIZMO:%[0-9]+]] : $Gizmo to $NSObject
  // CHECK: return [[GIZMO_NS]] : $NSObject

  // CHECK-LABEL: sil shared [serialized] [ossa] @$sSo5GizmoC{{[_0-9a-zA-Z]*}}fC : $@convention(method) (@thick Gizmo.Type) -> @owned Optional<Gizmo>
  // alloc is implicitly ns_returns_retained
  // init is implicitly ns_consumes_self and ns_returns_retained
  // CHECK: bb0([[GIZMO_META:%[0-9]+]] : $@thick Gizmo.Type):
  // CHECK-NEXT: [[GIZMO_META_OBJC:%[0-9]+]] = thick_to_objc_metatype [[GIZMO_META]] : $@thick Gizmo.Type to $@objc_metatype Gizmo.Type
  // CHECK-NEXT: [[GIZMO:%[0-9]+]] = alloc_ref_dynamic [objc] [[GIZMO_META_OBJC]] : $@objc_metatype Gizmo.Type, $Gizmo
  // CHECK-NEXT: // function_ref
  // CHECK-NEXT: [[INIT_CTOR:%[0-9]+]] = function_ref @$sSo5GizmoC{{[_0-9a-zA-Z]*}}fcTO
  // CHECK-NEXT: [[RESULT:%[0-9]+]] = apply [[INIT_CTOR]]([[GIZMO]]) : $@convention(method) (@owned Gizmo) -> @owned Optional<Gizmo>
  // CHECK-NEXT: return [[RESULT]] : $Optional<Gizmo>
}



// Normal message send with argument, no transfers.
// CHECK-LABEL: sil hidden [ossa] @$s26objc_ownership_conventions5test5{{[_0-9a-zA-Z]*}}F
func test5(_ g: Gizmo) {
  var g = g
  Gizmo.inspect(g)
  // CHECK: bb0([[ARG:%.*]] : @guaranteed $Gizmo):
  // CHECK:   [[GIZMO_BOX:%.*]] = alloc_box ${ var Gizmo }
  // CHECK:   [[GIZMO_BOX_LIFETIME:%[^,]+]] = begin_borrow [lexical] [var_decl] [[GIZMO_BOX]]
  // CHECK:   [[GIZMO_BOX_PB:%.*]] = project_box [[GIZMO_BOX_LIFETIME]]
  // CHECK:   [[ARG_COPY:%.*]] = copy_value [[ARG]]
  // CHECK:   store [[ARG_COPY]] to [init] [[GIZMO_BOX_PB]]
  // CHECK:   [[CLASS:%.*]] = metatype $@objc_metatype Gizmo.Type
  // CHECK:   [[READ:%.*]] = begin_access [read] [unknown] [[GIZMO_BOX_PB]] : $*Gizmo
  // CHECK:   [[V:%.*]] = load [copy] [[READ]]
  // CHECK:   [[G:%.*]] = enum $Optional<Gizmo>, #Optional.some!enumelt, [[V]]
  // CHECK:   [[METHOD:%.*]] = objc_method [[CLASS]] : {{.*}}, #Gizmo.inspect!foreign
  // CHECK:   apply [[METHOD]]([[G]], [[CLASS]])
  // CHECK:   destroy_value [[G]]
  // CHECK:   end_borrow [[GIZMO_BOX_LIFETIME]]
  // CHECK:   destroy_value [[GIZMO_BOX]]
  // CHECK-NOT:   destroy_value [[ARG]]
}
// CHECK: } // end sil function '$s26objc_ownership_conventions5test5{{[_0-9a-zA-Z]*}}F'

// The argument to consume is __attribute__((ns_consumed)).
// CHECK-LABEL: sil hidden [ossa] @$s26objc_ownership_conventions5test6{{[_0-9a-zA-Z]*}}F
func test6(_ g: Gizmo) {
  var g = g
  Gizmo.consume(g)
  // CHECK: bb0([[ARG:%.*]] : @guaranteed $Gizmo):
  // CHECK:   [[GIZMO_BOX:%.*]] = alloc_box ${ var Gizmo }
  // CHECK:   [[GIZMO_BOX_LIFETIME:%[^,]+]] = begin_borrow [lexical] [var_decl] [[GIZMO_BOX]]
  // CHECK:   [[GIZMO_BOX_PB:%.*]] = project_box [[GIZMO_BOX_LIFETIME]]
  // CHECK:   [[ARG_COPY:%.*]] = copy_value [[ARG]]
  // CHECK:   store [[ARG_COPY]] to [init] [[GIZMO_BOX_PB]]
  // CHECK:   [[CLASS:%.*]] = metatype $@objc_metatype Gizmo.Type
  // CHECK:   [[READ:%.*]] = begin_access [read] [unknown] [[GIZMO_BOX_PB]] : $*Gizmo
  // CHECK:   [[V:%.*]] = load [copy] [[READ]]
  // CHECK:   [[G:%.*]] = enum $Optional<Gizmo>, #Optional.some!enumelt, [[V]]
  // CHECK:   [[METHOD:%.*]] = objc_method [[CLASS]] : {{.*}}, #Gizmo.consume!foreign
  // CHECK:   apply [[METHOD]]([[G]], [[CLASS]])
  // CHECK-NOT:  destroy_value [[G]]
  // CHECK:   end_borrow [[GIZMO_BOX_LIFETIME]]
  // CHECK:   destroy_value [[GIZMO_BOX]]
  // CHECK-NOT:  destroy_value [[G]]
  // CHECK-NOT:   destroy_value [[ARG]]
  // CHECK-NOT:  destroy_value [[G]]
}
// CHECK: } // end sil function '$s26objc_ownership_conventions5test6{{[_0-9a-zA-Z]*}}F'

// fork is __attribute__((ns_consumes_self)).
// CHECK-LABEL: sil hidden [ossa] @$s26objc_ownership_conventions5test7{{[_0-9a-zA-Z]*}}F
func test7(_ g: Gizmo) {
  var g = g
  g.fork()
  // CHECK: bb0([[ARG:%.*]] : @guaranteed $Gizmo):
  // CHECK:   [[GIZMO_BOX:%.*]] = alloc_box ${ var Gizmo }
  // CHECK:   [[GIZMO_BOX_LIFETIME:%[^,]+]] = begin_borrow [lexical] [var_decl] [[GIZMO_BOX]]
  // CHECK:   [[GIZMO_BOX_PB:%.*]] = project_box [[GIZMO_BOX_LIFETIME]]
  // CHECK:   [[ARG_COPY:%.*]] = copy_value [[ARG]]
  // CHECK:   store [[ARG_COPY]] to [init] [[GIZMO_BOX_PB]]
  // CHECK:   [[READ:%.*]] = begin_access [read] [unknown] [[GIZMO_BOX_PB]] : $*Gizmo
  // CHECK:   [[G:%.*]] = load [copy] [[READ]]
  // CHECK:   [[METHOD:%.*]] = objc_method [[G]] : {{.*}}, #Gizmo.fork!foreign
  // CHECK:   apply [[METHOD]]([[G]])
  // CHECK-NOT:  destroy_value [[G]]
  // CHECK:   end_borrow [[GIZMO_BOX_LIFETIME]]
  // CHECK:   destroy_value [[GIZMO_BOX]]
  // CHECK-NOT:  destroy_value [[G]]
  // CHECK-NOT:   destroy_value [[ARG]]
  // CHECK-NOT:  destroy_value [[G]]
}
// CHECK: } // end sil function '$s26objc_ownership_conventions5test7{{[_0-9a-zA-Z]*}}F'

// clone is __attribute__((ns_returns_retained)).
// CHECK-LABEL: sil hidden [ossa] @$s26objc_ownership_conventions5test8{{[_0-9a-zA-Z]*}}F
func test8(_ g: Gizmo) -> Gizmo {
  return g.clone()
  // CHECK: bb0([[G:%.*]] : @guaranteed $Gizmo):
  // CHECK-NOT:  copy_value
  // CHECK:      [[METHOD:%.*]] = objc_method [[G]] : {{.*}}, #Gizmo.clone!foreign
  // CHECK-NOT:  copy_value [[RESULT]]
  // CHECK:      bb2([[RESULT:%.*]] : @owned $Gizmo):
  // CHECK-NOT:  copy_value [[RESULT]]
  // CHECK-NOT: destroy_value [[G]]
  // CHECK-NEXT: return [[RESULT]]
}
// duplicate returns an autoreleased object at +0.
// CHECK-LABEL: sil hidden [ossa] @$s26objc_ownership_conventions5test9{{[_0-9a-zA-Z]*}}F
func test9(_ g: Gizmo) -> Gizmo {
  return g.duplicate()
  // CHECK: bb0([[G:%.*]] : @guaranteed $Gizmo):
  // CHECK-NOT:      copy_value [[G:%0]]
  // CHECK: [[METHOD:%.*]] = objc_method [[G]] : {{.*}}, #Gizmo.duplicate!foreign
  // CHECK-NEXT: [[RESULT:%.*]] = apply [[METHOD]]([[G]])
  // CHECK-NOT:  copy_value [[RESULT]]
  // CHECK: bb2([[RESULT:%.*]] : @owned $Gizmo):
  // CHECK-NOT:  copy_value [[RESULT]]
  // CHECK-NOT: destroy_value [[G]]
  // CHECK-NEXT: return [[RESULT]]
}

// CHECK-LABEL: sil hidden [ossa] @$s26objc_ownership_conventions6test10{{[_0-9a-zA-Z]*}}F
func test10(_ g: Gizmo) -> AnyClass {
  // CHECK: bb0([[G:%[0-9]+]] : @guaranteed $Gizmo):
  // CHECK:      [[G_COPY:%.*]] = copy_value [[G]]
  // CHECK-NEXT: [[NS_G_COPY:%[0-9]+]] = upcast [[G_COPY]] : $Gizmo to $NSObject
  // CHECK-NEXT: [[NS_G_BORROW:%.*]] = begin_borrow [[NS_G_COPY]]
  // CHECK-NEXT: [[GETTER:%[0-9]+]] = objc_method [[NS_G_BORROW]] : $NSObject, #NSObject.classProp!getter.foreign : (NSObject) -> () -> (any AnyObject.Type)?, $@convention(objc_method) (NSObject) -> Optional<@objc_metatype any AnyObject.Type>
  // CHECK-NEXT: [[OPT_OBJC:%.*]] = apply [[GETTER]]([[NS_G_BORROW]]) : $@convention(objc_method) (NSObject) -> Optional<@objc_metatype any AnyObject.Type>
  // CHECK-NEXT: switch_enum [[OPT_OBJC]] : $Optional<{{.*}}>, case #Optional.some!enumelt: [[SOME_BB:bb[0-9]+]], case #Optional.none!enumelt: [[NONE_BB:bb[0-9]+]]
  //
  // CHECK: [[SOME_BB]]([[OBJC:%.*]] : $@objc_metatype any AnyObject.Type):
  // CHECK-NEXT: [[THICK:%.*]] = objc_to_thick_metatype [[OBJC]]
  // CHECK:      [[T0:%.*]] = enum $Optional<@thick any AnyObject.Type>, #Optional.some!enumelt, [[THICK]]
  // CHECK:   bb{{.*}}(%{{.*}} : $Optional<@thick any AnyObject.Type>):
  // CHECK:      destroy_value [[NS_G_COPY]] : $NSObject
  // CHECK:   bb{{.*}}([[RES:%.*]] : $@thick any AnyObject.Type):
  // CHECK-NOT:      destroy_value [[G]] : $Gizmo
  // CHECK-NEXT: return [[RES]] : $@thick any AnyObject.Type
  return g.classProp
}

// CHECK-LABEL: sil hidden [ossa] @$s26objc_ownership_conventions6test11{{[_0-9a-zA-Z]*}}F
func test11(_ g: Gizmo) -> AnyClass {
  // CHECK: bb0([[G:%[0-9]+]] : @guaranteed $Gizmo):
  // CHECK: [[G_COPY:%.*]] = copy_value [[G]]
  // CHECK: [[NS_G_COPY:%[0-9]+]] = upcast [[G_COPY:%[0-9]+]] : $Gizmo to $NSObject
  // CHECK-NEXT: [[NS_G_BORROW:%.*]] = begin_borrow [[NS_G_COPY]]
  // CHECK-NEXT: [[GETTER:%[0-9]+]] = objc_method [[NS_G_BORROW]] : $NSObject, #NSObject.qualifiedClassProp!getter.foreign : (NSObject) -> () -> (any NSAnsing.Type)?, $@convention(objc_method) (NSObject) -> Optional<@objc_metatype any NSAnsing.Type>
  // CHECK-NEXT: [[OPT_OBJC:%.*]] = apply [[GETTER]]([[NS_G_BORROW]]) : $@convention(objc_method) (NSObject) -> Optional<@objc_metatype any NSAnsing.Type>
  // CHECK-NEXT: switch_enum [[OPT_OBJC]] : $Optional<{{.*}}>, case #Optional.some!enumelt: [[SOME_BB:bb[0-9]+]], case #Optional.none!enumelt: [[NONE_BB:bb[0-9]+]]
  //
  // CHECK: [[SOME_BB]]([[OBJC:%.*]] : $@objc_metatype any NSAnsing.Type):
  // CHECK-NEXT: [[THICK:%.*]] = objc_to_thick_metatype [[OBJC]]
  // CHECK:      [[T0:%.*]] = enum $Optional<@thick any NSAnsing.Type>, #Optional.some!enumelt, [[THICK]]
  // CHECK:   bb{{.*}}(%{{.*}} : $Optional<@thick any NSAnsing.Type>):
  // CHECK:      destroy_value [[NS_G_COPY]] : $NSObject
  // CHECK:   bb{{.*}}([[RES:%.*]] : $@thick any NSAnsing.Type):
  // CHECK:      [[OPENED:%.*]] = open_existential_metatype [[RES]]
  // CHECK:      [[RES_ANY:%.*]] = init_existential_metatype [[OPENED]]
  // CHECK-NOT:      destroy_value [[G]] : $Gizmo
  // CHECK-NEXT: return [[RES_ANY]] : $@thick any AnyObject.Type
  return g.qualifiedClassProp
}

// ObjC blocks should have cdecl calling convention and follow C/ObjC
// ownership conventions, where the callee, arguments, and return are all +0.
// CHECK-LABEL: sil hidden [ossa] @$s26objc_ownership_conventions10applyBlock{{[_0-9a-zA-Z]*}}F
func applyBlock(_ f: @convention(block) (Gizmo) -> Gizmo, x: Gizmo) -> Gizmo {
  // CHECK:     bb0([[BLOCK:%.*]] : @guaranteed $@convention(block) @noescape (Gizmo) -> @autoreleased Gizmo, [[ARG:%.*]] : @guaranteed $Gizmo):
  // CHECK:       [[BLOCK_COPY:%.*]] = copy_block [[BLOCK]]
  // CHECK:       [[BORROWED_BLOCK_COPY:%.*]] = begin_borrow [[BLOCK_COPY]]
  // CHECK:       [[BLOCK_COPY_COPY:%.*]] = copy_value [[BORROWED_BLOCK_COPY]]
  // CHECK:       [[RESULT:%.*]] = apply [[BLOCK_COPY_COPY]]([[ARG]])
  // CHECK:       destroy_value [[BLOCK_COPY_COPY]]
  // CHECK:       end_borrow [[BORROWED_BLOCK_COPY]]
  // CHECK-NOT:       destroy_value [[ARG]]
  // CHECK:       destroy_value [[BLOCK_COPY]]
  // CHECK-NOT:       destroy_value [[BLOCK]]
  // CHECK:       return [[RESULT]]
  return f(x)
}

// CHECK-LABEL: sil hidden [ossa] @$s26objc_ownership_conventions15maybeApplyBlock{{[_0-9a-zA-Z]*}}F
func maybeApplyBlock(_ f: (@convention(block) (Gizmo) -> Gizmo)?, x: Gizmo) -> Gizmo? {
  // CHECK:     bb0([[BLOCK:%.*]] : @guaranteed $Optional<@convention(block) (Gizmo) -> @autoreleased Gizmo>, [[ARG:%.*]] : @guaranteed $Gizmo):
  // CHECK:       [[BLOCK_COPY:%.*]] = copy_block [[BLOCK]]
  return f?(x)
}

func useInnerPointer(_ p: UnsafeMutableRawPointer) {}

// Handle inner-pointer methods by autoreleasing self after the call.
// CHECK-LABEL: sil hidden [ossa] @$s26objc_ownership_conventions18innerPointerMethodyySo5GizmoCF : $@convention(thin) (@guaranteed Gizmo) -> () {
// CHECK: bb0([[ARG:%.*]] : @guaranteed $Gizmo):
// CHECK:         [[METHOD:%.*]] = objc_method [[ARG]] : $Gizmo, #Gizmo.getBytes!foreign : (Gizmo) -> () -> UnsafeMutableRawPointer, $@convention(objc_method) (Gizmo) -> @unowned_inner_pointer UnsafeMutableRawPointer
// CHECK:         [[ARG_COPY:%.*]] = copy_value [[ARG]]
// => SEMANTIC ARC TODO: The apply below /should/ be on ARG_COPY. It is safe how
// it is today though since we are using a reference type.
// CHECK:         [[PTR:%.*]] = apply [[METHOD]]([[ARG]])
// CHECK:         [[DP:%.*]] = mark_dependence [[PTR]] {{.*}} on [[ARG_COPY]]
// CHECK:         [[USE:%.*]] = function_ref @$s26objc_ownership_conventions15useInnerPointer{{[_0-9a-zA-Z]*}}F
// CHECK:         apply [[USE]]([[DP]])
// CHECK-NOT:         destroy_value [[ARG]]
func innerPointerMethod(_ g: Gizmo) {
  useInnerPointer(g.getBytes())
}

// CHECK-LABEL: sil hidden [ossa] @$s26objc_ownership_conventions20innerPointerPropertyyySo5GizmoCF : $@convention(thin) (@guaranteed Gizmo) -> () {
// CHECK:       bb0([[ARG:%.*]] : @guaranteed $Gizmo):
// CHECK:         [[METHOD:%.*]] = objc_method [[ARG]] : $Gizmo, #Gizmo.innerProperty!getter.foreign : (Gizmo) -> () -> UnsafeMutableRawPointer, $@convention(objc_method) (Gizmo) -> @unowned_inner_pointer UnsafeMutableRawPointer
// CHECK:         [[ARG_COPY:%.*]] = copy_value [[ARG]]
// => SEMANTIC ARC TODO: The apply below should be on ARG_COPY. It is benign since objc objects are reference types.
// CHECK:         [[PTR:%.*]] = apply [[METHOD]]([[ARG]])
// CHECK:         [[DP:%.*]] = mark_dependence [[PTR]] {{.*}} on [[ARG_COPY]]
// CHECK:         [[USE:%.*]] = function_ref @$s26objc_ownership_conventions15useInnerPointer{{[_0-9a-zA-Z]*}}F
// CHECK:         apply [[USE]]([[DP]])
// CHECK-NOT:         destroy_value [[ARG]]
// CHECK: } // end sil function '$s26objc_ownership_conventions20innerPointerPropertyyySo5GizmoCF'
func innerPointerProperty(_ g: Gizmo) {
  useInnerPointer(g.innerProperty)
}
