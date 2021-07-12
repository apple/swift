// RUN: %target-swift-frontend -dump-ast %s | %FileCheck %s
// RUN: %target-swift-frontend -emit-ir %s > /dev/null

// RUN: %target-swift-frontend -dump-ast -DVAR %s | %FileCheck %s
// RUN: %target-swift-frontend -emit-ir -DVAR %s > /dev/null

// CHECK: (top_level_code_decl
// CHECK: (guard_stmt
#if VAR
guard var x = Optional(0) else { fatalError() }
#else
guard let x = Optional(0) else { fatalError() }
#endif

// CHECK: (top_level_code_decl
_ = 0 // intervening code

// CHECK-LABEL: (func_decl{{.*}}"function()" interface type='() -> ()' access=internal captures=(x<direct>)
func function() {
  _ = x
}

// CHECK-LABEL: (original_init=closure_expr
// CHECK: location={{.*}}top-level-guard.swift:[[@LINE+3]]
// CHECK: captures=(x<direct>)
// CHECK: (var_decl{{.*}}"closure"
let closure: () -> Void = {
  _ = x
}

// CHECK-LABEL: (original_init=capture_list
// CHECK: location={{.*}}top-level-guard.swift:[[@LINE+5]]
// CHECK: (body=closure_expr
// CHECK: location={{.*}}top-level-guard.swift:[[@LINE+3]]
// CHECK: captures=(x<direct>)
// CHECK: (var_decl{{.*}}"closureCapture"
let closureCapture: () -> Void = { [x] in
  _ = x
}

// CHECK-LABEL: (defer_stmt
// CHECK-NEXT: (func_decl{{.*}}implicit "$defer()" interface type='() -> ()' access=fileprivate captures=(x<direct><noescape>)
defer {
  _ = x
}

#if VAR
x = 5
#endif
