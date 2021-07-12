// RUN: not %target-swift-frontend -dump-parse %s | %FileCheck %s
// RUN: not %target-swift-frontend -dump-ast %s | %FileCheck %s -check-prefix=CHECK-AST

// CHECK-LABEL: (func_decl{{.*}}"foo(_:)"
// CHECK-AST-LABEL: (func_decl{{.*}}"foo(_:)"
func foo(_ n: Int) -> Int {
  // CHECK:   (body=brace_stmt
  // CHECK:     (return_stmt
  // CHECK:       (integer_literal_expr type='<null type>'{{.*}} 42 {{.*}})))
  // CHECK-AST: (body=brace_stmt
  // CHECK-AST:   (return_stmt
  // CHECK-AST:     (integer_literal_expr type='{{[^']+}}' {{.*}} 42 {{.*}})
    return 42
}

// -dump-parse should print an AST even though this code is invalid.
// CHECK-LABEL: (func_decl{{.*}}"bar()"
// CHECK-AST-LABEL: (func_decl{{.*}}"bar()"
func bar() {
  // CHECK: (body=brace_stmt
  // CHECK-NEXT:   (unresolved_decl_ref_expr type='{{[^']+}}'{{.*}} "foo"
  // CHECK-NEXT:   (unresolved_decl_ref_expr type='{{[^']+}}'{{.*}} "foo"
  // CHECK-NEXT:   (unresolved_decl_ref_expr type='{{[^']+}}'{{.*}} "foo"
  // CHECK-AST: (body=brace_stmt
  // CHECK-AST-NEXT:   (declref_expr type='{{[^']+}}' {{.*}} decl='main.(file).foo@
  // CHECK-AST-NEXT:   (declref_expr type='{{[^']+}}' {{.*}} decl='main.(file).foo@
  // CHECK-AST-NEXT:   (declref_expr type='{{[^']+}}' {{.*}} decl='main.(file).foo@
  foo foo foo
}

// CHECK-LABEL: (enum_decl{{.*}}trailing_semi "TrailingSemi"
enum TrailingSemi {

  // CHECK-LABEL: (enum_case_decl{{.*}}trailing_semi
  // CHECK-NOT:   (enum_element_decl{{.*}}trailing_semi
  // CHECK:       (enum_element_decl{{.*}}"A")
  // CHECK:       (enum_element_decl{{.*}}"B")
  case A,B;

  // CHECK-LABEL: (subscript_decl{{.*}}trailing_semi
  // CHECK-NOT:   (func_decl{{.*}}trailing_semi get_for="subscript(_:)"
  // CHECK:       (accessor_decl{{.*}} get_for="subscript(_:)"
  subscript(x: Int) -> Int {
    // CHECK-LABEL: (pattern_binding_decl{{.*}}trailing_semi
    // CHECK-NOT:   (var_decl{{.*}}trailing_semi "y"
    // CHECK:       (var_decl{{.*}}"y"
    var y = 1;

    // CHECK-LABEL: (sequence_expr {{.*}} trailing_semi
    y += 1;

    // CHECK-LABEL: (return_stmt{{.*}}trailing_semi
    return y;
  };
};

// The substitution map for a declref should be relatively unobtrustive.
// CHECK-AST-LABEL:   (func_decl{{.*}}"generic(_:)" <T : Hashable> interface type='<T where T : Hashable> (T) -> ()' access=internal captures=(<generic> )
func generic<T: Hashable>(_: T) {}
// CHECK-AST:       (pattern_binding_decl
// CHECK-AST:         (processed_init=declref_expr type='(Int) -> ()' location={{.*}} range={{.*}} decl='main.(file).generic@{{.*}} [with (substitution_map generic_signature=<T where T : Hashable> T='Int')]' function_ref=unapplied))
let _: (Int) -> () = generic

// Closures should be marked as escaping or not.
func escaping(_: @escaping (Int) -> Int) {}
escaping({ $0 })
// CHECK-AST:        (fn=declref_expr type='(@escaping (Int) -> Int) -> ()'
// CHECK-AST-NEXT:        (arg=paren_expr
// CHECK-AST-NEXT:          (closure_expr type='(Int) -> Int' {{.*}} discriminator=0 escaping single_expr

func nonescaping(_: (Int) -> Int) {}
nonescaping({ $0 })
// CHECK-AST:        (fn=declref_expr type='((Int) -> Int) -> ()'
// CHECK-AST-NEXT:        (arg=paren_expr
// CHECK-AST-NEXT:          (closure_expr type='(Int) -> Int' {{.*}} discriminator=1 single_expr

// CHECK-LABEL: (struct_decl range='[{{.+}}]' "MyStruct")
struct MyStruct {}

// CHECK-LABEL: (enum_decl range='[{{.+}}]' "MyEnum"
enum MyEnum {

    // CHECK-LABEL: (enum_case_decl range='[{{.+}}]'
    // CHECK-NEXT:    (enum_element_decl range='[{{.+}}]' "foo(x:)"
    // CHECK-NEXT:      (parameter_list range='[{{.+}}]'
    // CHECK-NEXT:         (parameter range='[{{.+}}]' "x" type='<null type>' api_name="x")))
    // CHECK-NEXT:     (enum_element_decl range='[{{.+}}]' "bar"))
    // CHECK-NEXT:  (enum_element_decl range='[{{.+}}]' "foo(x:)"
    // CHECK-NEXT:    (parameter_list range='[{{.+}}]'
    // CHECK-NEXT:      (parameter range='[{{.+}}]' "x" type='<null type>' api_name="x")))
    // CHECK-NEXT:  (enum_element_decl range='[{{.+}}]' "bar"))
    case foo(x: MyStruct), bar
}

// CHECK-LABEL: (top_level_code_decl range='[{{.+}}]'
// CHECK-NEXT:    (brace_stmt implicit range='[{{.+}}]'
// CHECK-NEXT:      (sequence_expr type='<null type>'
// CHECK-NEXT:        (discard_assignment_expr type='<null type>'{{.*}})
// CHECK-NEXT:        (assign_expr type='<null type>'
// CHECK-NEXT:          (dest=<<null>>)
// CHECK-NEXT:          (src=<<null>>))
// CHECK-NEXT:        (closure_expr type='<null type>'{{.*}} discriminator={{[0-9]+}}
// CHECK-NEXT:          (parameter_list range='[{{.+}}]'
// CHECK-NEXT:            (parameter range='[{{.+}}]' "v" type='<null type>'))
// CHECK-NEXT:          (body=brace_stmt range='[{{.+}}]')))))
_ = { (v: MyEnum) in }

// CHECK-LABEL: (struct_decl range='[{{.+}}]' "SelfParam"
struct SelfParam {

  // CHECK-LABEL: (func_decl range='[{{.+}}]' "createOptional()" type
  // CHECK-NEXT:    (self=parameter implicit range='[{{.+}}]' "self" type='<null type>')
  // CHECK-NEXT:    (parameter_list range='[{{.+}}]')
  // CHECK-NEXT:    (result=type_optional
  // CHECK-NEXT:      (type_ident
  // CHECK-NEXT:        (component "SelfParam" unbound)))
  static func createOptional() -> SelfParam? {

    // CHECK-LABEL: (single_expression_body=call_expr type='<null type>'{{.*}} arg_labels=
    // CHECK-NEXT:    (fn=unresolved_decl_ref_expr type='<null type>'{{.*}} "SelfParam" function_ref=unapplied)
    // CHECK-NEXT:    (arg=tuple_expr type='()'{{.*}}))
    SelfParam()
  }
}
