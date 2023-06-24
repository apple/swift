// RUN: %target-swift-frontend -emit-sil -enable-copy-propagation=false %s | %FileCheck %s

// Using -enable-copy-propagation=false to pattern match against older SIL
// output. At least until -enable-copy-propagation has been around
// long enough in the same form to be worth rewriting CHECK lines.

class OtherClass {}

class FirstClass {
  var x: OtherClass

  // CHECK-LABEL: sil hidden @$s24definite_init_root_class10FirstClassC1nACSgs5Int32V_tcfc : $@convention(method) (Int32, @owned FirstClass) -> @owned Optional<FirstClass>
  init?(n: Int32) {
    // CHECK:   [[CONTROL:%.*]] = alloc_stack $Builtin.Int1
    // CHECK:   [[ZERO:%.*]] = integer_literal $Builtin.Int1, 0
    // CHECK:   store [[ZERO]] to [[CONTROL]] : $*Builtin.Int1

    // CHECK:   [[ZERO:%.*]] = integer_literal $Builtin.Int32, 0
    // CHECK:   [[N:%.*]] = struct_extract %0 : $Int32, #Int32._value
    // CHECK:   [[CMP:%.*]] = builtin "cmp_eq_Int32"([[N]] : $Builtin.Int32, [[ZERO]] : $Builtin.Int32) : $Builtin.Int1
    // CHECK:   cond_br [[CMP]], bb1, bb2
    if n == 0 {
      return nil
    }

    // CHECK: bb1:
    // CHECK:   br bb5

    // CHECK: bb2:
    // CHECK:   [[METATYPE:%.*]] = metatype $@thick OtherClass.Type
    // CHECK:   [[INIT:%.*]] = function_ref @$s24definite_init_root_class10OtherClassCACycfC : $@convention(method) (@thick OtherClass.Type) -> @owned OtherClass
    // CHECK:   [[OTHER:%.*]] = apply [[INIT]]([[METATYPE]]) : $@convention(method) (@thick OtherClass.Type) -> @owned OtherClass
    // CHECK:   [[X_ADDR:%.*]] = ref_element_addr %1 : $FirstClass, #FirstClass.x
    // CHECK:   [[X_ACCESS:%.*]] = begin_access [init] [static] %15 : $*OtherClass
    // CHECK:   [[ONE:%.*]] = integer_literal $Builtin.Int1, -1
    // CHECK:   store [[ONE]] to [[CONTROL]] : $*Builtin.Int1
    // CHECK:   store [[OTHER]] to [[X_ACCESS]] : $*OtherClass
    // CHECK:   end_access [[X_ACCESS]] : $*OtherClass
    x = OtherClass()

    // CHECK:   [[ONE:%.*]] = integer_literal $Builtin.Int32, 1
    // CHECK:   [[N:%.*]] = struct_extract %0 : $Int32, #Int32._value
    // CHECK:   [[CMP:%.*]] = builtin "cmp_eq_Int32"([[N]] : $Builtin.Int32, [[ONE]] : $Builtin.Int32) : $Builtin.Int1
    // CHECK:   cond_br [[CMP]], bb3, bb4
    if n == 1 {
      return nil
    }

    // CHECK: bb3:
    // CHECK:   br bb5

    // CHECK: bb4:
    // CHECK:   [[RESULT:%.*]] = enum $Optional<FirstClass>, #Optional.some!enumelt, %1 : $FirstClass
    // CHECK:   br bb12([[RESULT]] : $Optional<FirstClass>)

    // CHECK: bb5:
    // CHECK:   [[BIT:%.*]] = load [[CONTROL]] : $*Builtin.Int1
    // CHECK:   cond_br [[BIT]], bb6, bb7

    // CHECK: bb6:
    // CHECK:   strong_release %1 : $FirstClass
    // CHECK:   br bb11

    // CHECK: bb7:
    // CHECK:   [[BIT:%.*]] = load [[CONTROL]] : $*Builtin.Int1
    // CHECK:   cond_br [[BIT]], bb8, bb9

    // CHECK: bb8:
    // CHECK:   [[X_ADDR:%.*]] = ref_element_addr %1 : $FirstClass, #FirstClass.x
    // CHECK:   [[X_ACCESS:%.*]] = begin_access [deinit] [static] [[X_ADDR]] : $*OtherClass
    // CHECK:   destroy_addr [[X_ACCESS]] : $*OtherClass
    // CHECK:   end_access [[X_ACCESS]] : $*OtherClass
    // CHECK:   br bb10

    // CHECK: bb9:
    // CHECK:   br bb10

    // CHECK:   [[METATYPE:%.*]] = metatype $@thick FirstClass.Type
    // CHECK:   dealloc_partial_ref %1 : $FirstClass, [[METATYPE]] : $@thick FirstClass.Type
    // CHECK:   br bb11

    // CHECK: bb11:
    // CHECK:   [[NIL:%.*]] = enum $Optional<FirstClass>, #Optional.none!enumelt
    // CHECK:   br bb12([[NIL]] : $Optional<FirstClass>)

    // CHECK: bb12([[RESULT:%.*]] : $Optional<FirstClass>):
    // CHECK:   dealloc_stack [[CONTROL]] : $*Builtin.Int1
    // CHECK:   return [[RESULT]] : $Optional<FirstClass>
  }
}

class SecondClass {
  var x: OtherClass
  var y: OtherClass

  // CHECK-LABEL: sil hidden @$s24definite_init_root_class11SecondClassC1nACSgs5Int32V_tcfc : $@convention(method) (Int32, @owned SecondClass) -> @owned Optional<SecondClass> {
  init?(n: Int32) {
    // CHECK:   [[CONTROL:%.*]] = alloc_stack $Builtin.Int2
    // CHECK:   [[ZERO:%.*]] = integer_literal $Builtin.Int2, 0
    // CHECK:   store [[ZERO]] to [[CONTROL]] : $*Builtin.Int2

    // CHECK:   [[ZERO:%.*]] = integer_literal $Builtin.Int32, 0
    // CHECK:   [[N:%.*]] = struct_extract %0 : $Int32, #Int32._value
    // CHECK:   [[CMP:%.*]] = builtin "cmp_eq_Int32"([[N]] : $Builtin.Int32, [[ZERO]] : $Builtin.Int32) : $Builtin.Int1
    // CHECK:   cond_br [[CMP]], bb1, bb2
    if n == 0 {
      return nil
    }

    // CHECK: bb1:
    // CHECK:   br bb7

    // CHECK: bb2:
    // CHECK:   [[METATYPE:%.*]] = metatype $@thick OtherClass.Type
    // CHECK:   [[INIT:%.*]] = function_ref @$s24definite_init_root_class10OtherClassCACycfC : $@convention(method) (@thick OtherClass.Type) -> @owned OtherClass
    // CHECK:   [[OTHER:%.*]] = apply [[INIT]]([[METATYPE]]) : $@convention(method) (@thick OtherClass.Type) -> @owned OtherClass
    // CHECK:   [[X_ADDR:%.*]] = ref_element_addr %1 : $SecondClass, #SecondClass.x
    // CHECK:   [[X_ACCESS:%.*]] = begin_access [init] [static] [[X_ADDR]] : $*OtherClass
    // CHECK:   [[ONE:%.*]] = integer_literal $Builtin.Int2, 1
    // CHECK:   store [[ONE]] to [[CONTROL]] : $*Builtin.Int2
    // CHECK:   store [[OTHER]] to [[X_ACCESS]] : $*OtherClass
    // CHECK:   end_access [[X_ACCESS]] : $*OtherClass
    x = OtherClass()

    // CHECK:   [[ONE:%.*]] = integer_literal $Builtin.Int32, 1
    // CHECK:   [[N:%.*]] = struct_extract %0 : $Int32, #Int32._value
    // CHECK:   [[CMP:%.*]] = builtin "cmp_eq_Int32"([[N]] : $Builtin.Int32, [[ONE]] : $Builtin.Int32) : $Builtin.Int1
    // CHECK:   cond_br [[CMP]], bb3, bb4
    if n == 1 {
      return nil
    }

    // CHECK: bb3:
    // CHECK:   br bb7

    // CHECK: bb4:
    // CHECK:   [[METATYPE:%.*]] = metatype $@thick OtherClass.Type
    // CHECK:   [[INIT:%.*]] = function_ref @$s24definite_init_root_class10OtherClassCACycfC : $@convention(method) (@thick OtherClass.Type) -> @owned OtherClass
    // CHECK:   [[OTHER:%.*]] = apply [[INIT]]([[METATYPE]]) : $@convention(method) (@thick OtherClass.Type) -> @owned OtherClass
    // CHECK:   [[Y_ADDR:%.*]] = ref_element_addr %1 : $SecondClass, #SecondClass.y
    // CHECK:   [[Y_ACCESS:%.*]] = begin_access [init] [static] [[Y_ADDR]] : $*OtherClass
    // CHECK:   [[THREE:%.*]] = integer_literal $Builtin.Int2, -1
    // CHECK:   store [[THREE]] to [[CONTROL]] : $*Builtin.Int2
    // CHECK:   store [[OTHER]] to [[Y_ACCESS]] : $*OtherClass
    // CHECK:   end_access [[Y_ACCESS]] : $*OtherClass
    y = OtherClass()

    // CHECK:   [[TWO:%.*]] = integer_literal $Builtin.Int32, 2
    // CHECK:   [[N:%.*]] = struct_extract %0 : $Int32, #Int32._value
    // CHECK:   [[CMP:%.*]] = builtin "cmp_eq_Int32"([[N]] : $Builtin.Int32, [[TWO]] : $Builtin.Int32) : $Builtin.Int1
    // CHECK:   cond_br [[CMP]], bb5, bb6
    if n == 2 {
      return nil
    }

    // CHECK: bb5:
    // CHECK:   br bb7

    // CHECK: bb6:
    // CHECK:   [[RESULT:%.*]] = enum $Optional<SecondClass>, #Optional.some!enumelt, %1 : $SecondClass
    // CHECK:   br bb17([[RESULT]] : $Optional<SecondClass>)

    // CHECK: bb7:
    // CHECK:   [[BITS:%.*]] = load [[CONTROL]] : $*Builtin.Int2
    // CHECK:   [[THREE:%.*]] = integer_literal $Builtin.Int2, -1
    // CHECK:   [[BIT:%.*]] = builtin "cmp_eq_Int2"([[BITS]] : $Builtin.Int2, [[THREE]] : $Builtin.Int2) : $Builtin.Int1
    // CHECK:   cond_br [[BIT]], bb8, bb9

    // CHECK: bb8:
    // CHECK:   strong_release %1 : $SecondClass
    // CHECK:   br bb16

    // CHECK: bb9:
    // CHECK:   [[BITS:%.*]] = load [[CONTROL]] : $*Builtin.Int2
    // CHECK:   [[BIT:%.*]] = builtin "trunc_Int2_Int1"([[BITS]] : $Builtin.Int2) : $Builtin.Int1
    // CHECK:   cond_br [[BIT]], bb10, bb11

    // CHECK: bb10:
    // CHECK:   [[X_ADDR:%.*]] = ref_element_addr %1 : $SecondClass, #SecondClass.x
    // CHECK:   [[X_ACCESS:%.*]] = begin_access [deinit] [static] [[X_ADDR]] : $*OtherClass
    // CHECK:   destroy_addr [[X_ACCESS]] : $*OtherClass
    // CHECK:   end_access [[X_ACCESS]] : $*OtherClass
    // CHECK:   br bb12

    // CHECK: bb11:
    // CHECK:   br bb12

    // CHECK: bb12:
    // CHECK:   [[BITS:%.*]] = load [[CONTROL]] : $*Builtin.Int2
    // CHECK:   [[ONE:%.*]] = integer_literal $Builtin.Int2, 1
    // CHECK:   [[TMP:%.*]] = builtin "lshr_Int2"([[BITS]] : $Builtin.Int2, [[ONE]] : $Builtin.Int2) : $Builtin.Int2
    // CHECK:   [[BIT:%.*]] = builtin "trunc_Int2_Int1"([[TMP]] : $Builtin.Int2) : $Builtin.Int1
    // CHECK:   cond_br [[BIT]], bb13, bb14

    // CHECK: bb13:
    // CHECK:   [[Y_ADDR:%.*]] = ref_element_addr %1 : $SecondClass, #SecondClass.y
    // CHECK:   [[Y_ACCESS:%.*]] = begin_access [deinit] [static] [[Y_ADDR]] : $*OtherClass
    // CHECK:   destroy_addr [[Y_ACCESS]] : $*OtherClass
    // CHECK:   end_access [[Y_ACCESS]] : $*OtherClass
    // CHECK:   br bb15

    // CHECK: bb14:
    // CHECK:   br bb15

    // CHECK: bb15:
    // CHECK:   [[METATYPE:%.*]] = metatype $@thick SecondClass.Type
    // CHECK:   dealloc_partial_ref %1 : $SecondClass, [[METATYPE]] : $@thick SecondClass.Type
    // CHECK:   br bb16

    // CHECK: bb16:
    // CHECK:   [[NIL:%.*]] = enum $Optional<SecondClass>, #Optional.none!enumelt
    // CHECK:   br bb17([[NIL]] : $Optional<SecondClass>)

    // CHECK: bb17([[RESULT:%.*]] : $Optional<SecondClass>):
    // CHECK:   dealloc_stack [[CONTROL]] : $*Builtin.Int2
    // CHECK:   return [[RESULT]] : $Optional<SecondClass>
  }
}
