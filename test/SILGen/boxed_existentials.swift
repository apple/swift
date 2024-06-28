// RUN: %target-swift-emit-silgen -module-name boxed_existentials -Xllvm -sil-full-demangle %s | %FileCheck %s
// RUN: %target-swift-emit-silgen -module-name boxed_existentials -Xllvm -sil-full-demangle %s | %FileCheck %s --check-prefix=GUARANTEED

func test_type_lowering(_ x: Error) { }
// CHECK-LABEL: sil hidden [ossa] @$s18boxed_existentials18test_type_loweringyys5Error_pF : $@convention(thin) (@guaranteed any Error) -> () {
// CHECK-NOT:         destroy_value %0 : $any Error

class Document {}

enum ClericalError: Error {
  case MisplacedDocument(Document)

  var _domain: String { return "" }
  var _code: Int { return 0 }
}

func test_concrete_erasure(_ x: ClericalError) -> Error {
  return x
}
// CHECK-LABEL: sil hidden [ossa] @$s18boxed_existentials21test_concrete_erasureys5Error_pAA08ClericalF0OF
// CHECK:       bb0([[ARG:%.*]] : @guaranteed $ClericalError):
// CHECK:         [[ARG_COPY:%.*]] = copy_value [[ARG]]
// CHECK:         [[EXISTENTIAL:%.*]] = alloc_existential_box $any Error, $ClericalError
// CHECK:         [[ADDR:%.*]] = project_existential_box $ClericalError in [[EXISTENTIAL]] : $any Error
// CHECK:         store [[EXISTENTIAL]] to [init] [[EXISTENTIAL_BUF:%.*]] :
// CHECK:         store [[ARG_COPY]] to [init] [[ADDR]] : $*ClericalError
// CHECK-NOT:         destroy_value [[ARG]]
// CHECK:         [[EXISTENTIAL2:%.*]] = load [take] [[EXISTENTIAL_BUF]]
// CHECK:         return [[EXISTENTIAL2]] : $any Error

protocol HairType {}

func test_composition_erasure(_ x: HairType & Error) -> Error {
  return x
}
// CHECK-LABEL: sil hidden [ossa] @$s18boxed_existentials24test_composition_erasureys5Error_psAC_AA8HairTypepF
// CHECK:         [[VALUE_ADDR:%.*]] = open_existential_addr immutable_access [[OLD_EXISTENTIAL:%.*]] : $*any Error & HairType to $*[[VALUE_TYPE:@opened\(.*, any Error & HairType\) Self]]
// CHECK:         [[NEW_EXISTENTIAL:%.*]] = alloc_existential_box $any Error, $[[VALUE_TYPE]]
// CHECK:         [[ADDR:%.*]] = project_existential_box $[[VALUE_TYPE]] in [[NEW_EXISTENTIAL]] : $any Error
// CHECK:         store [[NEW_EXISTENTIAL]] to [init] [[NEW_EXISTENTIALBUF:%.*]] :
// CHECK:         copy_addr [[VALUE_ADDR]] to [init] [[ADDR]]
// CHECK-NOT:         destroy_addr [[OLD_EXISTENTIAL]]
// CHECK:         [[NEW_EXISTENTIAL2:%.*]] = load [take] [[NEW_EXISTENTIALBUF]]
// CHECK:         return [[NEW_EXISTENTIAL2]]

protocol HairClass: class {}

func test_class_composition_erasure(_ x: HairClass & Error) -> Error {
  return x
}
// CHECK-LABEL: sil hidden [ossa] @$s18boxed_existentials30test_class_composition_erasureys5Error_psAC_AA9HairClasspF
// CHECK:         [[VALUE:%.*]] = open_existential_ref [[OLD_EXISTENTIAL:%.*]] : $any Error & HairClass to $[[VALUE_TYPE:@opened\(.*, any Error & HairClass\) Self]]
// CHECK:         [[NEW_EXISTENTIAL:%.*]] = alloc_existential_box $any Error, $[[VALUE_TYPE]]
// CHECK:         [[ADDR:%.*]] = project_existential_box $[[VALUE_TYPE]] in [[NEW_EXISTENTIAL]] : $any Error
// CHECK:         store [[NEW_EXISTENTIAL]] to [init] [[NEW_EXISTENTIALBUF:%.*]] :
// CHECK:         [[COPIED_VALUE:%.*]] = copy_value [[VALUE]]
// CHECK:         store [[COPIED_VALUE]] to [init] [[ADDR]]
// CHECK:         [[NEW_EXISTENTIAL2:%.*]] = load [take] [[NEW_EXISTENTIALBUF]]
// CHECK:         return [[NEW_EXISTENTIAL2]]

func test_property(_ x: Error) -> String {
  return x._domain
}
// CHECK-LABEL: sil hidden [ossa] @$s18boxed_existentials13test_propertyySSs5Error_pF
// CHECK: bb0([[ARG:%.*]] : @guaranteed $any Error):
// CHECK:         [[VALUE:%.*]] = open_existential_box [[ARG]] : $any Error to $*[[VALUE_TYPE:@opened\(.*, any Error\) Self]]
// CHECK:         [[METHOD:%.*]] = witness_method $[[VALUE_TYPE]], #Error._domain!getter
// -- self parameter of witness is @in_guaranteed; no need to copy since
//    value in box is immutable and box is guaranteed
// CHECK:         [[RESULT:%.*]] = apply [[METHOD]]<[[VALUE_TYPE]]>([[VALUE]])
// CHECK-NOT:         destroy_value [[ARG]]
// CHECK:         return [[RESULT]]

func test_property_of_lvalue(_ x: Error) -> String {
  var x = x
  return x._domain
}

// CHECK-LABEL: sil hidden [ossa] @$s18boxed_existentials23test_property_of_lvalueySSs5Error_pF :
// CHECK:       bb0([[ARG:%.*]] : @guaranteed $any Error):
// CHECK:         [[VAR:%.*]] = alloc_box ${ var any Error }
// CHECK:         [[VAR_LIFETIME:%[^,]+]] = begin_borrow [lexical] [var_decl] [[VAR]]
// CHECK:         [[PVAR:%.*]] = project_box [[VAR_LIFETIME]]
// CHECK:         [[ARG_COPY:%.*]] = copy_value [[ARG]] : $any Error
// CHECK:         store [[ARG_COPY]] to [init] [[PVAR]]
// CHECK:         [[ACCESS:%.*]] = begin_access [read] [unknown] [[PVAR]] : $*any Error
// CHECK:         [[VALUE_BOX:%.*]] = load [copy] [[ACCESS]]
// CHECK:         [[BORROWED_VALUE_BOX:%.*]] = begin_borrow [[VALUE_BOX]]
// CHECK:         [[VALUE:%.*]] = open_existential_box [[BORROWED_VALUE_BOX]] : $any Error to $*[[VALUE_TYPE:@opened\(.*, any Error\) Self]]
// CHECK:         [[COPY:%.*]] = alloc_stack $[[VALUE_TYPE]]
// CHECK:         copy_addr [[VALUE]] to [init] [[COPY]]
// CHECK:         destroy_value [[VALUE_BOX]]
// CHECK:         [[METHOD:%.*]] = witness_method $[[VALUE_TYPE]], #Error._domain!getter
// CHECK:         [[RESULT:%.*]] = apply [[METHOD]]<[[VALUE_TYPE]]>([[COPY]])
// CHECK:         destroy_addr [[COPY]]
// CHECK:         dealloc_stack [[COPY]]
// CHECK:         end_borrow [[VAR_LIFETIME]]
// CHECK:         destroy_value [[VAR]]
// CHECK-NOT:         destroy_value [[ARG]]
// CHECK:         return [[RESULT]]
// CHECK:      } // end sil function '$s18boxed_existentials23test_property_of_lvalueySSs5Error_pF'
extension Error {
  func extensionMethod() { }
}

// CHECK-LABEL: sil hidden [ossa] @$s18boxed_existentials21test_extension_methodyys5Error_pF
func test_extension_method(_ error: Error) {
  // CHECK: bb0([[ARG:%.*]] : @guaranteed $any Error):
  // CHECK: [[VALUE:%.*]] = open_existential_box [[ARG]]
  // CHECK: [[METHOD:%.*]] = function_ref
  // CHECK-NOT: copy_addr
  // CHECK: apply [[METHOD]]<{{.*}}>([[VALUE]])
  // CHECK-NOT: destroy_addr [[COPY]]
  // CHECK-NOT: destroy_addr [[VALUE]]
  // CHECK-NOT: destroy_addr [[VALUE]]
  // -- destroy_value the owned argument
  // CHECK-NOT: destroy_value %0
  error.extensionMethod()
}

func plusOneError() -> Error { }

// CHECK-LABEL: sil hidden [ossa] @$s18boxed_existentials31test_open_existential_semanticsyys5Error_p_sAC_ptF
// GUARANTEED-LABEL: sil hidden [ossa] @$s18boxed_existentials31test_open_existential_semanticsyys5Error_p_sAC_ptF
// CHECK: bb0([[ARG0:%.*]]: @guaranteed $any Error,
// GUARANTEED: bb0([[ARG0:%.*]]: @guaranteed $any Error,
func test_open_existential_semantics(_ guaranteed: Error,
                                     _ immediate: Error) {
  var immediate = immediate
  // CHECK: [[IMMEDIATE_BOX:%.*]] = alloc_box ${ var any Error }
  // CHECK: [[IMMEDIATE_LIFETIME:%[^,]+]] = begin_borrow [lexical] [var_decl] [[IMMEDIATE_BOX]]
  // CHECK: [[PB:%.*]] = project_box [[IMMEDIATE_LIFETIME]]
  // GUARANTEED: [[IMMEDIATE_BOX:%.*]] = alloc_box ${ var any Error }
  // GUARANTEED: [[IMMEDIATE_LIFETIME:%[^,]+]] = begin_borrow [lexical] [var_decl] [[IMMEDIATE_BOX]]
  // GUARANTEED: [[PB:%.*]] = project_box [[IMMEDIATE_LIFETIME]]

  // CHECK-NOT: copy_value [[ARG0]]
  // CHECK: [[VALUE:%.*]] = open_existential_box [[ARG0]]
  // CHECK: [[METHOD:%.*]] = function_ref
  // CHECK-NOT: copy_addr
  // CHECK: apply [[METHOD]]<{{.*}}>([[VALUE]])
  // CHECK-NOT: destroy_value [[ARG0]]

  // GUARANTEED-NOT: copy_value [[ARG0]]
  // GUARANTEED: [[VALUE:%.*]] = open_existential_box [[ARG0]]
  // GUARANTEED: [[METHOD:%.*]] = function_ref
  // GUARANTEED: apply [[METHOD]]<{{.*}}>([[VALUE]])
  // GUARANTEED-NOT: destroy_addr [[VALUE]]
  // GUARANTEED-NOT: destroy_value [[ARG0]]
  guaranteed.extensionMethod()

  // CHECK: [[ACCESS:%.*]] = begin_access [read] [unknown] [[PB]] : $*any Error
  // CHECK: [[IMMEDIATE:%.*]] = load [copy] [[ACCESS]]
  // -- need a copy_value to guarantee
  // CHECK: [[IMMEDIATE_BORROW:%.*]] = begin_borrow [[IMMEDIATE]]
  // CHECK: [[VALUE:%.*]] = open_existential_box [[IMMEDIATE_BORROW]]
  // CHECK: [[METHOD:%.*]] = function_ref
  // CHECK-NOT: copy_addr
  // CHECK: apply [[METHOD]]<{{.*}}>([[VALUE]])
  // -- end the guarantee
  // -- TODO: could in theory do this sooner, after the value's been copied
  //    out.
  // CHECK: destroy_value [[IMMEDIATE]]

  // GUARANTEED: [[ACCESS:%.*]] = begin_access [read] [unknown] [[PB]] : $*any Error
  // GUARANTEED: [[IMMEDIATE:%.*]] = load [copy] [[ACCESS]]
  // -- need a copy_value to guarantee
  // GUARANTEED: [[BORROWED_IMMEDIATE:%.*]] = begin_borrow [[IMMEDIATE]]
  // GUARANTEED: [[VALUE:%.*]] = open_existential_box [[BORROWED_IMMEDIATE]]
  // GUARANTEED: [[METHOD:%.*]] = function_ref
  // GUARANTEED: apply [[METHOD]]<{{.*}}>([[VALUE]])
  // GUARANTEED-NOT: destroy_addr [[VALUE]]
  // -- end the guarantee
  // GUARANTEED: destroy_value [[IMMEDIATE]]
  immediate.extensionMethod()

  // CHECK: [[F:%.*]] = function_ref {{.*}}plusOneError
  // CHECK: [[PLUS_ONE:%.*]] = apply [[F]]()
  // CHECK: [[PLUS_ONE_BORROW:%.*]] = begin_borrow [[PLUS_ONE]]
  // CHECK: [[VALUE:%.*]] = open_existential_box [[PLUS_ONE_BORROW]]
  // CHECK: [[METHOD:%.*]] = function_ref
  // CHECK-NOT: copy_addr
  // CHECK: apply [[METHOD]]<{{.*}}>([[VALUE]])
  // CHECK: destroy_value [[PLUS_ONE]]

  // GUARANTEED: [[F:%.*]] = function_ref {{.*}}plusOneError
  // GUARANTEED: [[PLUS_ONE:%.*]] = apply [[F]]()
  // GUARANTEED: [[BORROWED_PLUS_ONE:%.*]] = begin_borrow [[PLUS_ONE]]
  // GUARANTEED: [[VALUE:%.*]] = open_existential_box [[BORROWED_PLUS_ONE]]
  // GUARANTEED: [[METHOD:%.*]] = function_ref
  // GUARANTEED: apply [[METHOD]]<{{.*}}>([[VALUE]])
  // GUARANTEED-NOT: destroy_addr [[VALUE]]
  // GUARANTEED: destroy_value [[PLUS_ONE]]
  plusOneError().extensionMethod()
}

// CHECK-LABEL: sil hidden [ossa] @$s18boxed_existentials14erasure_to_anyyyps5Error_p_sAC_ptF
// CHECK:       bb0([[OUT:%.*]] : $*Any, [[GUAR:%.*]] : @guaranteed $any Error,
func erasure_to_any(_ guaranteed: Error, _ immediate: Error) -> Any {
  var immediate = immediate
  // CHECK:       [[IMMEDIATE_BOX:%.*]] = alloc_box ${ var any Error }
  // CHECK:       [[IMMEDIATE_LIFETIME:%[^,]+]] = begin_borrow [lexical] [var_decl] [[IMMEDIATE_BOX]]
  // CHECK:       [[PB:%.*]] = project_box [[IMMEDIATE_LIFETIME]]
  if true {
    // CHECK-NOT: copy_value [[GUAR]]
    // CHECK:     [[FROM_VALUE:%.*]] = open_existential_box [[GUAR:%.*]]
    // CHECK:     [[TO_VALUE:%.*]] = init_existential_addr [[OUT]]
    // CHECK:     copy_addr [[FROM_VALUE]] to [init] [[TO_VALUE]]
    // CHECK-NOT: destroy_value [[GUAR]]
    return guaranteed
  } else if true {
    // CHECK:     [[ACCESS:%.*]] = begin_access [read] [unknown] [[PB]]
    // CHECK:     [[IMMEDIATE:%.*]] = load [copy] [[ACCESS]]
    // CHECK:     [[BORROWED_IMMEDIATE:%.*]] = begin_borrow [[IMMEDIATE]]
    // CHECK:     [[FROM_VALUE:%.*]] = open_existential_box [[BORROWED_IMMEDIATE]]
    // CHECK:     [[TO_VALUE:%.*]] = init_existential_addr [[OUT]]
    // CHECK:     copy_addr [[FROM_VALUE]] to [init] [[TO_VALUE]]
    // CHECK:     destroy_value [[IMMEDIATE]]
    return immediate
  } else if true {
    // CHECK:     function_ref boxed_existentials.plusOneError
    // CHECK:     [[PLUS_ONE:%.*]] = apply
    // CHECK:     [[BORROWED_PLUS_ONE:%.*]] = begin_borrow [[PLUS_ONE]]
    // CHECK:     [[FROM_VALUE:%.*]] = open_existential_box [[BORROWED_PLUS_ONE]]
    // CHECK:     [[TO_VALUE:%.*]] = init_existential_addr [[OUT]]
    // CHECK:     copy_addr [[FROM_VALUE]] to [init] [[TO_VALUE]]
    // CHECK:     destroy_value [[PLUS_ONE]]

    return plusOneError()
  }
}

extension Error {
  var myError: Error {
    return self
  }
}

// Make sure we don't assert on this.
// CHECK-LABEL: sil hidden [ossa] @$s18boxed_existentials4testyyF
// CHECK:  [[ERROR_ADDR:%.*]] = alloc_stack $any Error
// CHECK:  [[ARRAY_GET:%.*]] = function_ref @$sSayxSicig
// CHECK:  apply [[ARRAY_GET]]<any Error>([[ERROR_ADDR]]
// CHECK:  [[ERROR:%.*]] = load [take] [[ERROR_ADDR]] : $*any Error
// CHECK:  [[BORROWED_ERROR:%.*]] = begin_borrow [[ERROR]]
// CHECK:  open_existential_box [[BORROWED_ERROR]]
func test() {
  var errors: [Error] = []
  test_property(errors[0].myError)
}
