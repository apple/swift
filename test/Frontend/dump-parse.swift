// RUN: not %target-swift-frontend -dump-parse %s | %FileCheck %s
// RUN: not %target-swift-frontend -dump-ast %s | %FileCheck %s -check-prefix=CHECK-AST

// CHECK-LABEL: (func_decl{{.*}}"foo(_:)"
// CHECK-AST-LABEL: (func_decl{{.*}}"foo(_:)"
func foo(_ n: Int) -> Int {
  // CHECK:   (brace_stmt
  // CHECK:     (return_stmt
  // CHECK:       (integer_literal_expr type='<null>' value=42 {{.*}})))
  // CHECK-AST: (brace_stmt
  // CHECK-AST:   (return_stmt
  // CHECK-AST:     (integer_literal_expr type='{{[^']+}}' {{.*}} value=42 {{.*}})
    return 42
}

// -dump-parse should print an AST even though this code is invalid.
// CHECK-LABEL: (func_decl{{.*}}"bar()"
// CHECK-AST-LABEL: (func_decl{{.*}}"bar()"
func bar() {
  // CHECK: (brace_stmt
  // CHECK-NEXT:   (unresolved_decl_ref_expr type='{{[^']+}}' name=foo
  // CHECK-NEXT:   (unresolved_decl_ref_expr type='{{[^']+}}' name=foo
  // CHECK-NEXT:   (unresolved_decl_ref_expr type='{{[^']+}}' name=foo
  // CHECK-AST: (brace_stmt
  // CHECK-AST-NEXT:   (declref_expr type='{{[^']+}}' {{.*}} decl=main.(file).foo
  // CHECK-AST-NEXT:   (declref_expr type='{{[^']+}}' {{.*}} decl=main.(file).foo
  // CHECK-AST-NEXT:   (declref_expr type='{{[^']+}}' {{.*}} decl=main.(file).foo
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
  // CHECK-NOT:   (func_decl{{.*}}trailing_semi 'anonname={{.*}}' get_for=subscript(_:)
  // CHECK:       (accessor_decl{{.*}}'anonname={{.*}}' get_for=subscript(_:)
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
// CHECK-AST:         (declref_expr type='(Int) -> ()' location={{.*}} range={{.*}} decl=main.(file).generic@{{.*}} [with (substitution_map generic_signature=<T where T : Hashable> (substitution T -> Int))] function_ref=unapplied))
let _: (Int) -> () = generic

// Closures should be marked as escaping or not.
func escaping(_: @escaping (Int) -> Int) {}
escaping({ $0 })
// CHECK-AST:        (declref_expr type='(@escaping (Int) -> Int) -> ()'
// CHECK-AST-NEXT:        (argument_list
// CHECK-AST-NEXT:          (argument
// CHECK-AST-NEXT:            (closure_expr type='(Int) -> Int' {{.*}} discriminator=0 escaping single-expression

func nonescaping(_: (Int) -> Int) {}
nonescaping({ $0 })
// CHECK-AST:        (declref_expr type='((Int) -> Int) -> ()'
// CHECK-AST-NEXT:        (argument_list
// CHECK-AST-NEXT:          (argument
// CHECK-AST-NEXT:            (closure_expr type='(Int) -> Int' {{.*}} discriminator=1 single-expression

// CHECK-LABEL: (struct_decl range=[{{.+}}] "MyStruct")
struct MyStruct {}

// CHECK-LABEL: (enum_decl range=[{{.+}}] "MyEnum"
enum MyEnum {

    // CHECK-LABEL: (enum_case_decl range=[{{.+}}]
    // CHECK-NEXT:    (enum_element_decl range=[{{.+}}]  "foo(x:)"
    // CHECK-NEXT:      (parameter_list range=[{{.+}}]
    // CHECK-NEXT:         (parameter "x" apiName=x)))
    // CHECK-NEXT:     (enum_element_decl range=[{{.+}}] "bar"))
    // CHECK-NEXT:  (enum_element_decl range=[{{.+}}] "foo(x:)"
    // CHECK-NEXT:    (parameter_list range=[{{.+}}]
    // CHECK-NEXT:      (parameter "x" apiName=x)))
    // CHECK-NEXT:  (enum_element_decl range=[{{.+}}] "bar"))
    case foo(x: MyStruct), bar
}

// CHECK-LABEL: (top_level_code_decl range=[{{.+}}]
// CHECK-NEXT:    (brace_stmt implicit range=[{{.+}}]
// CHECK-NEXT:      (sequence_expr type='<null>'
// CHECK-NEXT:        (discard_assignment_expr type='<null>')
// CHECK-NEXT:        (assign_expr type='<null>'
// CHECK-NEXT:          (**NULL EXPRESSION**)
// CHECK-NEXT:          (**NULL EXPRESSION**))
// CHECK-NEXT:        (closure_expr type='<null>' discriminator={{[0-9]+}}
// CHECK-NEXT:          (parameter_list range=[{{.+}}]
// CHECK-NEXT:            (parameter "v"))
// CHECK-NEXT:          (brace_stmt range=[{{.+}}])))))
_ = { (v: MyEnum) in }

// CHECK-LABEL: (struct_decl range=[{{.+}}] "SelfParam"
struct SelfParam {

  // CHECK-LABEL: (func_decl range=[{{.+}}] "createOptional()" type
  // CHECK-NEXT:    (parameter "self")
  // CHECK-NEXT:    (parameter_list range=[{{.+}}])
  // CHECK-NEXT:    (result
  // CHECK-NEXT:      (type_optional
  // CHECK-NEXT:        (type_ident
  // CHECK-NEXT:          (component id='SelfParam' bind=none))))
  static func createOptional() -> SelfParam? {

    // CHECK-LABEL: (call_expr type='<null>'
    // CHECK-NEXT:    (unresolved_decl_ref_expr type='<null>' name=SelfParam function_ref=unapplied)
    // CHECK-NEXT:    (argument_list)
    SelfParam()
  }
}
