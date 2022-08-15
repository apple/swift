// RUN: %target-typecheck-verify-swift -dump-ast > %t.ast
// RUN: %FileCheck %s < %t.ast

// Verify that upcasts of array literals upcast the individual elements in place
// rather than introducing a collection_upcast_expr.

protocol P { }
struct X : P { }

struct TakesArray<T> {
  init(_: [(T) -> Void]) { }
}

// CHECK-LABEL: func_decl{{.*}}"arrayUpcast(_:_:)"
// CHECK: assign_expr
// CHECK-NOT: collection_upcast_expr
// CHECK: array_expr type='[(X) -> Void]'
// CHECK: function_conversion_expr implicit type='(X) -> Void'
// CHECK-NEXT: {{declref_expr.*x1}}
// CHECK-NEXT: function_conversion_expr implicit type='(X) -> Void'
// CHECK-NEXT: {{declref_expr.*x2}}
func arrayUpcast(_ x1: @escaping (P) -> Void, _ x2: @escaping (P) -> Void) {
  _ = TakesArray<X>([x1, x2])
}

struct TakesDictionary<T> {
  init(_: [Int : (T) -> Void]) { }
}

// CHECK-LABEL: func_decl{{.*}}"dictionaryUpcast(_:_:)"
// CHECK: assign_expr
// CHECK-NOT: collection_upcast_expr
// CHECK: paren_expr type='[Int : (X) -> Void]'
// CHECK-NOT: collection_upcast_expr
// CHECK: (dictionary_expr type='[Int : (X) -> Void]'
func dictionaryUpcast(_ x1: @escaping (P) -> Void, _ x2: @escaping (P) -> Void) {
  _ = TakesDictionary<X>(([1: x1, 2: x2]))
}
