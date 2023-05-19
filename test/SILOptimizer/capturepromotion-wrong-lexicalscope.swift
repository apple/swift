// Make sure project_box gets assigned the correct lexical scope when we create it.
// RUN: %target-swift-frontend -primary-file %s -Onone -emit-sil -Xllvm -sil-print-after=capture-promotion -Xllvm \
// RUN:   -sil-print-debuginfo -o /dev/null -module-name null 2>&1 | %FileCheck %s

// CHECK: sil hidden [ossa] @$s4null19captureStackPromoteSiycyF : $@convention(thin) () -> @owned @callee_guaranteed () -> Int {
// CHECK: bb0:
// CHECK:   [[BOX:%[^,]+]] = alloc_box ${ var Int }, var, name "x", loc {{.*}}:32:7, scope 3
// CHECK:   [[BOX_ADDR:%[^,]+]] = project_box [[BOX]] : ${ var Int }, 0, loc {{.*}}:32:7, scope 3
// CHECK:   [[ONE:%[^,]+]] = integer_literal $Builtin.IntLiteral, 1, loc {{.*}}:32:11, scope 4
// CHECK:   [[THIN_INT_TYPE:%[^,]+]] = metatype $@thin Int.Type, loc {{.*}}:32:11, scope 4
// CHECK:   [[INTEGER_LITERAL:%[^,]+]] = function_ref @$sSi22_builtinIntegerLiteralSiBI_tcfC : $@convention(method) (Builtin.IntLiteral, @thin Int.Type) -> Int, loc {{.*}}:32:11, scope 4
// CHECK:   [[ONE_INT:%[^,]+]] = apply [[INTEGER_LITERAL]]([[ONE]], [[THIN_INT_TYPE]]) : $@convention(method) (Builtin.IntLiteral, @thin Int.Type) -> Int, loc {{.*}}:32:11, scope 4
// CHECK:   store [[ONE_INT]] to [trivial] [[BOX_ADDR]] : $*Int, loc {{.*}}:32:11, scope 4
// CHECK:   [[BOX_COPY:%[^,]+]] = copy_value [[BOX]] : ${ var Int }, loc {{.*}}:33:11, scope 4
// CHECK:   [[BOX_COPY_ADDR:%[^,]+]] = project_box [[BOX_COPY]] : ${ var Int }, 0, loc {{.*}}:33:11, scope 4
// CHECK:   mark_function_escape [[BOX_ADDR]] : $*Int, loc {{.*}}:33:11, scope 4
// CHECK:   [[SPECIALIZED_F:%[^,]+]] = function_ref @$s4null19captureStackPromoteSiycyFSiycfU_Tf2i_n : $@convention(thin) (Int) -> Int, loc {{.*}}:33:11, scope 4
// CHECK:   [[REGISTER_11:%[^,]+]] = load [trivial] [[BOX_COPY_ADDR]] : $*Int, loc {{.*}}:33:11, scope 4
// CHECK:   destroy_value [[BOX_COPY]] : ${ var Int }, loc {{.*}}:33:11, scope 4
// CHECK:   [[CLOSURE:%[^,]+]] = partial_apply [callee_guaranteed] [[SPECIALIZED_F]]([[REGISTER_11]]) : $@convention(thin) (Int) -> Int, loc {{.*}}:33:11, scope 4
// CHECK:   [[BORROW:%.*]] = begin_borrow [lexical] [[CLOSURE]]
// CHECK:   debug_value [[BORROW]] : $@callee_guaranteed () -> Int, let, name "f", loc {{.*}}:33:7, scope 6
// CHECK:   [[CLOSURE_COPY:%[^,]+]] = copy_value [[BORROW]] : $@callee_guaranteed () -> Int, loc {{.*}}:34:10, scope 6
// There used to be an end_borrow here. We leave an emptyline here to preserve line numbers.
// CHECK:   destroy_value [[CLOSURE]] : $@callee_guaranteed () -> Int, loc {{.*}}:35:1, scope 6
// CHECK:   destroy_value [[BOX]] : ${ var Int }, loc {{.*}}:35:1, scope 6
// CHECK:   return [[CLOSURE_COPY]] : $@callee_guaranteed () -> Int, loc {{.*}}:34:3, scope 6
// CHECK: }


func captureStackPromote() -> () -> Int {
  var x = 1
  let f = { x }
  return f
}
