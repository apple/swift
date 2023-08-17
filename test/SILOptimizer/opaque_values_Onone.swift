// RUN: %target-swift-frontend -enable-sil-opaque-values -parse-as-library -emit-sil -Onone %s | %FileCheck %s

// CHECK-LABEL: sil hidden @$s19opaque_values_Onone16generic_identity1txx_tlF : $@convention(thin) <T> (@in_guaranteed T) -> @out T {
// CHECK: bb0(%0 : $*T, %1 : $*T):
// CHECK:   debug_value %1 : $*T, let, name "t", argno 1
// CHECK:   copy_addr %1 to [init] %0 : $*T
// CHECK-LABEL: } // end sil function '$s19opaque_values_Onone16generic_identity1txx_tlF'
func generic_identity<T>(t: T) -> T {
  return t
}


enum Maybe1<T : Equatable> {
  case nope
  case yep(T)
  // CHECK-LABEL: sil hidden @maybe1_compare {{.*}} {
  // CHECK:         [[LHS_ADDR:%[^,]+]] = alloc_stack [lexical] $Maybe1
  // CHECK:         switch_enum_addr [[LHS_ADDR]] : $*Maybe1<T>, case #Maybe1.yep!enumelt: [[L_YEP:bb[0-9]+]], case #Maybe1.nope!enumelt: {{bb[0-9]+}}
  // CHECK:       [[L_YEP]]:
  // CHECK:         [[RHS_ADDR:%[^,]+]] = alloc_stack [lexical] $Maybe1
  // CHECK:         unchecked_take_enum_data_addr [[LHS_ADDR]] : $*Maybe1<T>, #Maybe1.yep!enumelt
  // CHECK:         switch_enum_addr [[RHS_ADDR]] : $*Maybe1<T>, case #Maybe1.yep!enumelt: [[L_AND_R_YEP:bb[0-9]+]], default {{bb[0-9]+}}
  // CHECK:       [[L_AND_R_YEP]]:
  // CHECK:         unchecked_take_enum_data_addr [[RHS_ADDR]] : $*Maybe1<T>, #Maybe1.yep!enumelt
  // CHECK-LABEL: } // end sil function 'maybe1_compare'
  @_silgen_name("maybe1_compare")
  static func compare(_ lhs: Maybe1, _ rhs: Maybe1) -> Bool {
    switch (lhs, rhs) {
    case (.yep(let l), .yep(let r)):
      return l == r
    case (.nope, .nope):
      return true
    default:
      return false
    }
  }
}

enum Maybe2<T : Equatable> {
  case nope
  case yep(T, T)

  // CHECK-LABEL: sil hidden @maybe2_compare {{.*}} {
  // CHECK:         [[LHS_ADDR:%[^,]+]] = alloc_stack [lexical] $Maybe2<T>
  // CHECK:         switch_enum_addr [[LHS_ADDR]] : $*Maybe2<T>, case #Maybe2.yep!enumelt: [[L_YEP:bb[0-9]+]], case #Maybe2.nope!enumelt: {{bb[0-9]+}}
  // CHECK:       [[L_YEP]]:
  // CHECK:         [[RHS_ADDR:%[^,]+]] = alloc_stack [lexical] $Maybe2<T>
  // CHECK:         unchecked_take_enum_data_addr [[LHS_ADDR]] : $*Maybe2<T>, #Maybe2.yep!enumelt
  // CHECK:         switch_enum_addr [[RHS_ADDR]] : $*Maybe2<T>, case #Maybe2.yep!enumelt: [[R_YEP:bb[0-9]+]], default {{bb[0-9]+}}
  // CHECK:       [[L_AND_R_YEP]]:
  // CHECK:         unchecked_take_enum_data_addr [[RHS_ADDR]] : $*Maybe2<T>, #Maybe2.yep!enumelt
  // CHECK-LABEL: } // end sil function 'maybe2_compare'
  @_silgen_name("maybe2_compare")
  static func compare(_ lhs: Maybe2, _ rhs: Maybe2) -> Bool {
    switch (lhs, rhs) {
    case (.yep(let l, _), .yep(let r, _)):
      return l == r
    case (.nope, .nope):
      return true
    default:
      return false
    }
  }
}

func doit<T>(_ f: () -> T) -> T {
  f()
}
// CHECK-LABEL: sil hidden @duplicate1 : {{.*}} {
// CHECK:       {{bb[0-9]+}}({{%[^,]+}} : $*Value, {{%[^,]+}} : $*Value, [[INSTANCE_ADDR_IN:%[^,]+]] : $*Value):
// CHECK:         [[INSTANCE_ADDR:%[^,]+]] = alloc_stack $Value
// CHECK:         [[OUTPUT_TUPLE_ADDR:%[^,]+]] = alloc_stack $(Value, Value)
// CHECK:         [[DUPLICATE_CLOSURE:%[^,]+]] = function_ref @$s19opaque_values_Onone10duplicate15valuex_xtx_tlFx_xtyXEfU_
// CHECK:         copy_addr [[INSTANCE_ADDR_IN]] to [init] [[INSTANCE_ADDR]]
// CHECK:         [[DUPLICATE_INSTANCE_CLOSURE:%[^,]+]] = partial_apply [callee_guaranteed] [on_stack] [[DUPLICATE_CLOSURE]]<Value>([[INSTANCE_ADDR]])
// CHECK:         [[DEPENDENDENCY:%[^,]+]] = mark_dependence [[DUPLICATE_INSTANCE_CLOSURE]] : $@noescape @callee_guaranteed () -> @out (Value, Value) on [[INSTANCE_ADDR]] : $*Value
// CHECK:         [[CONVERTED:%[^,]+]] = convert_function [[DEPENDENDENCY]]
// CHECK:         apply {{%[^,]+}}<(Value, Value)>([[OUTPUT_TUPLE_ADDR]], [[CONVERTED]])
// CHECK-LABEL: } // end sil function 'duplicate1'
// CHECK-LABEL: sil private @$s19opaque_values_Onone10duplicate15valuex_xtx_tlFx_xtyXEfU_ : {{.*}} {
// CHECK:       {{bb[0-9]+}}([[TUPLE_ADDR:%[^,]+]] : $*(Value, Value), [[VALUE_ADDR:%[^,]+]] :
// CHECK:         [[ELT_1_ADDR:%[^,]+]] = tuple_element_addr [[TUPLE_ADDR]]{{.*}}, 0
// CHECK:         copy_addr [[VALUE_ADDR]] to [init] [[ELT_1_ADDR]]
// CHECK:         [[ELT_2_ADDR:%[^,]+]] = tuple_element_addr [[TUPLE_ADDR]]{{.*}}, 1
// CHECK:         copy_addr [[VALUE_ADDR]] to [init] [[ELT_2_ADDR]] : $*Value
// CHECK-LABEL: } // end sil function '$s19opaque_values_Onone10duplicate15valuex_xtx_tlFx_xtyXEfU_'
@_silgen_name("duplicate1")
func duplicate1<Value>(value: Value) -> (Value, Value) {
  doit { 
      (value, value)
  }
}
// CHECK-LABEL: sil hidden @duplicate2 : {{.*}} {
// CHECK:       {{bb[0-9]+}}({{%[^,]+}} : $*Value, {{%[^,]+}} : $*Value, [[INSTANCE_ADDR_IN:%[^,]+]] : $*Value):
// CHECK:         [[INSTANCE_ADDR:%[^,]+]] = alloc_stack $Value
// CHECK:         [[OUTPUT_TUPLE_ADDR:%[^,]+]] = alloc_stack $(one: Value, two: Value)
// CHECK:         [[DUPLICATE_CLOSURE:%[^,]+]] = function_ref @$s19opaque_values_Onone10duplicate25valuex3one_x3twotx_tlFxAD_xAEtyXEfU_
// CHECK:         copy_addr [[INSTANCE_ADDR_IN]] to [init] [[INSTANCE_ADDR]]
// CHECK:         [[DUPLICATE_INSTANCE_CLOSURE:%[^,]+]] = partial_apply [callee_guaranteed] [on_stack] [[DUPLICATE_CLOSURE]]<Value>([[INSTANCE_ADDR]])
// CHECK:         [[DEPENDENDENCY:%[^,]+]] = mark_dependence [[DUPLICATE_INSTANCE_CLOSURE]] : $@noescape @callee_guaranteed () -> @out (one: Value, two: Value) on [[INSTANCE_ADDR]] : $*Value
// CHECK:         [[CONVERTED:%[^,]+]] = convert_function [[DEPENDENDENCY]]
// CHECK:         apply {{%[^,]+}}<(one: Value, two: Value)>([[OUTPUT_TUPLE_ADDR]], [[CONVERTED]])
// CHECK-LABEL: } // end sil function 'duplicate2'
// CHECK-LABEL: sil private @$s19opaque_values_Onone10duplicate25valuex3one_x3twotx_tlFxAD_xAEtyXEfU_ : {{.*}} {
// CHECK:       {{bb[0-9]+}}([[TUPLE_ADDR:%[^,]+]] : $*(one: Value, two: Value), [[VALUE_ADDR:%[^,]+]] :
// CHECK:         [[ELT_1_ADDR:%[^,]+]] = tuple_element_addr [[TUPLE_ADDR]]{{.*}}, 0
// CHECK:         copy_addr [[VALUE_ADDR]] to [init] [[ELT_1_ADDR]]
// CHECK:         [[ELT_2_ADDR:%[^,]+]] = tuple_element_addr [[TUPLE_ADDR]]{{.*}}, 1
// CHECK:         copy_addr [[VALUE_ADDR]] to [init] [[ELT_2_ADDR]] : $*Value
// CHECK-LABEL: } // end sil function '$s19opaque_values_Onone10duplicate25valuex3one_x3twotx_tlFxAD_xAEtyXEfU_'
@_silgen_name("duplicate2")
func duplicate2<Value>(value: Value) -> (one: Value, two: Value) {
  doit { 
      (one: value, two: value)
  }
}
@_silgen_name("duplicate_with_int1")
func duplicate_with_int1<Value>(value: Value) -> (Value, Value, Int) {
  doit {
    (value, value, 42)
  }
}
@_silgen_name("duplicate_with_int2")
func duplicate_with_int2<Value>(value: Value) -> ((Value, Value), Int) {
  doit {
    ((value, value), 42)
  }
}

// CHECK-LABEL: sil hidden @duplicate_with_int3 : {{.*}} {
// CHECK:       {{bb[0-9]+}}({{%[^,]+}} : $*Value, {{%[^,]+}} : $*Value, {{%[^,]+}} : $*Value, {{%[^,]+}} : $*Value, [[INSTANCE_ADDR_IN:%[^,]+]] : $*Value):
// CHECK:         [[INSTANCE_ADDR:%[^,]+]] = alloc_stack $Value
// CHECK:         [[CLOSURE:%[^,]+]] = function_ref @$s19opaque_values_Onone19duplicate_with_int35valueSi_x_x_x_SitxttSitx_tlFSi_x_x_x_SitxttSityXEfU_
// CHECK:         copy_addr [[INSTANCE_ADDR_IN]] to [init] [[INSTANCE_ADDR]]
// CHECK:         [[INSTANCE_CLOSURE:%[^,]+]] = partial_apply [callee_guaranteed] [on_stack] [[CLOSURE]]<Value>([[INSTANCE_ADDR]])
// CHECK:         [[DEPENDENCY:%[^,]+]] = mark_dependence [[INSTANCE_CLOSURE]]
// CHECK:         [[CONVERTED:%[^,]+]] = convert_function [[DEPENDENCY]]
// CHECK:         apply {{%[^,]+}}<(Int, (Value, (Value, (Value, Int), Value)), Int)>({{%[^,]+}}, [[CONVERTED]])
// CHECK-LABEL: } // end sil function 'duplicate_with_int3'
// CHECK-LABEL: sil private @$s19opaque_values_Onone19duplicate_with_int35valueSi_x_x_x_SitxttSitx_tlFSi_x_x_x_SitxttSityXEfU_ {{.*}} {
// CHECK:       {{bb[0-9]+}}([[OUT_ADDR:%[^,]+]] : $*(Int, (Value, (Value, (Value, Int), Value)), Int), [[IN_ADDR:%[^,]+]] : $*Value):
// CHECK:         [[OUT_1_ADDR:%[^,]+]] = tuple_element_addr [[OUT_ADDR]] : $*(Int, (Value, (Value, (Value, Int), Value)), Int), 1
// CHECK:         [[OUT_1_0_ADDR:%[^,]+]] = tuple_element_addr [[OUT_1_ADDR]] : $*(Value, (Value, (Value, Int), Value)), 0
// CHECK:         copy_addr [[IN_ADDR]] to [init] [[OUT_1_0_ADDR]]
// CHECK:         [[OUT_1_1_ADDR:%[^,]+]] = tuple_element_addr [[OUT_1_ADDR]] : $*(Value, (Value, (Value, Int), Value)), 1
// CHECK:         [[OUT_1_1_0_ADDR:%[^,]+]] = tuple_element_addr [[OUT_1_1_ADDR]] : $*(Value, (Value, Int), Value), 0
// CHECK:         copy_addr [[IN_ADDR]] to [init] [[OUT_1_1_0_ADDR]]
// CHECK:         [[OUT_1_1_1_ADDR:%[^,]+]] = tuple_element_addr [[OUT_1_1_ADDR]] : $*(Value, (Value, Int), Value), 1
// CHECK:         [[OUT_1_1_1_2_ADDR:%[^,]+]] = tuple_element_addr [[OUT_1_1_1_ADDR]] : $*(Value, Int), 0
// CHECK:         copy_addr [[IN_ADDR]] to [init] [[OUT_1_1_1_2_ADDR]]
// CHECK:         [[OUT_1_1_2_ADDR:%[^,]+]] = tuple_element_addr [[OUT_1_1_ADDR]] : $*(Value, (Value, Int), Value), 2
// CHECK:         copy_addr [[IN_ADDR]] to [init] [[OUT_1_1_2_ADDR]]
// CHECK:         [[OUT_1_1_1_1_ADDR:%[^,]+]] = tuple_element_addr [[OUT_1_1_1_ADDR]] : $*(Value, Int), 1
// CHECK:         store {{%[^,]+}} to [[OUT_1_1_1_1_ADDR]]
// CHECK:         [[OUT_0_ADDR:%[^,]+]] = tuple_element_addr [[OUT_ADDR]] : $*(Int, (Value, (Value, (Value, Int), Value)), Int), 0
// CHECK:         store {{%[^,]+}} to [[OUT_0_ADDR]]
// CHECK:         [[OUT_2_ADDR:%[^,]+]] = tuple_element_addr [[OUT_ADDR]] : $*(Int, (Value, (Value, (Value, Int), Value)), Int), 2
// CHECK:         store {{%[^,]+}} to [[OUT_2_ADDR]]
// CHECK-LABEL: } // end sil function '$s19opaque_values_Onone19duplicate_with_int35valueSi_x_x_x_SitxttSitx_tlFSi_x_x_x_SitxttSityXEfU_'
@_silgen_name("duplicate_with_int3")
func duplicate_with_int3<Value>(value: Value) -> (Int, (Value, (Value, (Value, Int), Value)), Int) {
  doit {
    (42, (value, (value, (value, 43), value)), 44)
  }
}

// CHECK-LABEL: sil hidden @get_a_generic_tuple : {{.*}} {
// CHECK:         [[GIVE_A_GENERIC_TUPLE:%[^,]+]] = function_ref @give_a_generic_tuple
// CHECK:         [[TUPLE_ADDR:%[^,]+]] = alloc_stack [lexical] $(This, This)
// CHECK:         [[TUPLE_1_ADDR:%[^,]+]] = tuple_element_addr [[TUPLE_ADDR]] : $*(This, This), 1
// CHECK:         [[TUPLE_0_ADDR:%[^,]+]] = tuple_element_addr [[TUPLE_ADDR]] : $*(This, This), 0
// CHECK:         apply [[GIVE_A_GENERIC_TUPLE]]<This>([[TUPLE_0_ADDR]], [[TUPLE_1_ADDR]], {{%[^,]+}})
// CHECK:         destroy_addr [[TUPLE_ADDR]]
// CHECK:         dealloc_stack [[TUPLE_ADDR]]
// CHECK-LABEL: } // end sil function 'get_a_generic_tuple'
struct This {}
@_silgen_name("give_a_generic_tuple")
func give_a_generic_tuple<This>(of ty: This.Type) -> (This, This)
@_silgen_name("get_a_generic_tuple")
func get_a_generic_tuple<This>(ty: This.Type) {
  let p = give_a_generic_tuple(of: ty)
}

// CHECK-LABEL: sil hidden @castAnyObjectToMeta {{.*}} {
// CHECK:       {{bb[0-9]+}}([[SRC:%[^,]+]] :
// CHECK:         [[SRC_ADDR:%[^,]+]] = alloc_stack $AnyObject
// CHECK:         store [[SRC]] to [[SRC_ADDR]]
// CHECK:         [[DEST_ADDR:%[^,]+]] = alloc_stack $@thick any AnyObject.Type
// CHECK:         unconditional_checked_cast_addr AnyObject in [[SRC_ADDR]] {{.*}} to @thick any AnyObject.Type in [[DEST_ADDR]]
// CHECK:         [[DEST:%[^,]+]] = load [[DEST_ADDR]]
// CHECK:         dealloc_stack [[DEST_ADDR]]
// CHECK:         dealloc_stack [[SRC_ADDR]]
// CHECK:         return [[DEST]]
// CHECK-LABEL: } // end sil function 'castAnyObjectToMeta'
@_silgen_name("castAnyObjectToMeta")
func castAnyObjectToMeta(_ ao: any AnyObject) -> AnyObject.Type {
  ao as! AnyObject.Type
}

// CHECK-LABEL: sil hidden @castGenericToMeta : {{.*}} {
// CHECK:       {{bb[0-9]+}}([[SRC:%[^,]+]] :
// CHECK:         [[SRC_ADDR:%[^,]+]] = alloc_stack $T
// CHECK:         copy_addr [[SRC]] to [init] [[SRC_ADDR]]
// CHECK:         [[DEST_ADDR:%[^,]+]] = alloc_stack $@thick any AnyObject.Type
// CHECK:         unconditional_checked_cast_addr T in [[SRC_ADDR]] {{.*}} to @thick any AnyObject.Type in [[DEST_ADDR]]
// CHECK:         [[DEST:%[^,]+]] = load [[DEST_ADDR]]
// CHECK:         dealloc_stack [[DEST_ADDR]]
// CHECK:         dealloc_stack [[SRC_ADDR]]
// CHECK:         return [[DEST]]
// CHECK-LABEL: } // end sil function 'castGenericToMeta'
@_silgen_name("castGenericToMeta")
func castGenericToMeta<T>(_ t: T) -> AnyObject.Type {
  t as! AnyObject.Type
}

// CHECK-LABEL: sil hidden @maybeCastAnyObjectToMeta : {{.*}} {
// CHECK:       {{bb[0-9]+}}([[SRC:%[^,]+]] :
// CHECK:         [[SRC_ADDR:%[^,]+]] = alloc_stack $AnyObject
// CHECK:         store [[SRC]] to [[SRC_ADDR]] : $*AnyObject
// CHECK:         [[DEST_ADDR:%[^,]+]] = alloc_stack $@thick any AnyObject.Type
// CHECK:         checked_cast_addr_br take_on_success AnyObject in [[SRC_ADDR]] {{.*}} to any AnyObject.Type in [[DEST_ADDR]] {{.*}}, [[SUCCESS:bb[0-9]+]], [[FAILURE:bb[0-9]+]]
// CHECK:       [[SUCCESS]]:
// CHECK:         load [[DEST_ADDR]] : $*@thick any AnyObject.Type
// CHECK:       [[FAILURE]]:
// CHECK:         load [[SRC_ADDR]] : $*AnyObject
// CHECK-LABEL: } // end sil function 'maybeCastAnyObjectToMeta'
@_silgen_name("maybeCastAnyObjectToMeta")
func maybeCastAnyObjectToMeta(_ ao: any AnyObject) -> AnyObject.Type? {
  if let m = ao as? AnyObject.Type {
    return m
  }
  return nil
}

// CHECK-LABEL: sil hidden @maybeCastGenericToMeta : {{.*}} {
// CHECK:       {{bb[0-9]+}}([[SRC:%[^,]+]] :
// CHECK:         [[SRC_ADDR:%[^,]+]] = alloc_stack $T
// CHECK:         copy_addr [[SRC]] to [init] [[SRC_ADDR]]
// CHECK:         [[DEST_ADDR:%[^,]+]] = alloc_stack $@thick any AnyObject.Type
// CHECK:         checked_cast_addr_br take_on_success T in [[SRC_ADDR]] {{.*}} to any AnyObject.Type in [[DEST_ADDR]] {{.*}}, [[SUCCESS:bb[0-9]+]], [[FAILURE:bb[0-9]+]]
// CHECK:       [[SUCCESS]]:
// CHECK:         load [[DEST_ADDR]]
// CHECK:       [[FAILURE]]:
// CHECK:         destroy_addr [[SRC_ADDR]]
// CHECK-LABEL: } // end sil function 'maybeCastGenericToMeta'
@_silgen_name("maybeCastGenericToMeta")
func maybeCastGenericToMeta<T>(_ t: T) -> AnyObject.Type? {
  if let m = t as? AnyObject.Type {
    return m
  }
  return nil
}
