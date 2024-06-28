// RUN: %target-swift-emit-silgen -parse-as-library %s | %FileCheck %s

func callee(_ fn: () -> Any) {}

func caller<each T>(_ t: repeat each T) {
  repeat callee { each t }
}

// CHECK-LABEL: sil hidden [ossa] @$s26pack_element_expr_captures6calleryyxxQpRvzlF : $@convention(thin) <each T> (@pack_guaranteed Pack{repeat each T}) -> () {
// CHECK: [[FN:%.*]] = function_ref @$s26pack_element_expr_captures6calleryyxxQpRvzlFypyXEfU_ : $@convention(thin) <each τ_0_0><τ_1_0> (@in_guaranteed τ_1_0) -> @out Any
// CHECK: [[ELT_ADDR:%.*]] = pack_element_get {{.*}} of %0 : $*Pack{repeat each T} as $*@pack_element("{{.*}}") each T
// CHECK: [[ELT_COPY:%.*]] = alloc_stack $@pack_element("{{.*}}") each T
// CHECK: copy_addr [[ELT_ADDR]] to [init] [[ELT_COPY]] : $*@pack_element("{{.*}}") each T
// CHECK: [[CLOSURE:%.*]] = partial_apply [callee_guaranteed] [[FN]]<Pack{repeat each T}, @pack_element("{{.*}}") each T>([[ELT_COPY]])
// CHECK: [[CONVERTED:%.*]] = convert_escape_to_noescape [not_guaranteed] %15 : $@callee_guaranteed () -> @out Any to $@noescape @callee_guaranteed () -> @out Any
// CHECK: [[CALLEE:%.*]]  = function_ref @$s26pack_element_expr_captures6calleeyyypyXEF : $@convention(thin) (@guaranteed @noescape @callee_guaranteed () -> @out Any) -> ()
// CHECK: apply [[CALLEE]]([[CONVERTED]]) : $@convention(thin) (@guaranteed @noescape @callee_guaranteed () -> @out Any) -> ()

// CHECK-LABEL: sil private [ossa] @$s26pack_element_expr_captures6calleryyxxQpRvzlFypyXEfU_ : $@convention(thin) <each T><τ_1_0> (@in_guaranteed τ_1_0) -> @out Any {
// CHECK: bb0(%0 : $*Any, %1 : @closureCapture $*τ_1_0):
// CHECK: [[ADDR:%.*]] = init_existential_addr %0 : $*Any, $τ_1_0
// CHECK: copy_addr %1 to [init] [[ADDR]] : $*τ_1_0
// CHECK: return
