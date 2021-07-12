// RUN: %empty-directory(%t)
// RUN: %target-swift-frontend -emit-module -o %t %S/Inputs/overload_intFunctions.swift
// RUN: %target-swift-frontend -emit-module -o %t %S/Inputs/overload_boolFunctions.swift
// RUN: %target-swift-frontend -emit-module -o %t %S/Inputs/overload_vars.swift
// RUN: %target-swift-frontend -typecheck %s -I %t -sdk "" -verify -swift-version 4
// RUN: %target-swift-frontend -typecheck %s -I %t -sdk "" -verify -swift-version 5

// RUN: not %target-swift-frontend -dump-ast %s -I %t -sdk "" > %t.astdump
// RUN: %FileCheck %s < %t.astdump

import overload_intFunctions
import overload_boolFunctions
import overload_vars
import var overload_vars.scopedVar
import func overload_boolFunctions.scopedFunction

struct LocalType {}
func something(_ obj: LocalType) -> LocalType { return obj }
func something(_ a: Int, _ b: Int, _ c: Int) -> () {}

var _ : Bool = something(true)
var _ : Int = something(1)
var _ : (Int, Int) = something(1, 2)
var _ : LocalType = something(LocalType())
var _ : () = something(1, 2, 3)
something = 42

let ambValue = ambiguousWithVar // no-warning - var preferred
let ambValueChecked: Int = ambValue
ambiguousWithVar = 42    // no-warning
ambiguousWithVar(true)   // no-warning

var localVar : Bool
localVar = false
localVar = 42 // expected-error {{type 'Int' cannot be used as a boolean; test for '!= 0' instead}}
localVar(42)  // expected-error {{cannot call value of non-function type 'Bool'}}
var _ : localVar // should still work

_ = scopedVar // no-warning
scopedVar(42)

var _ : Bool = scopedFunction(true)
var _ : Int  = scopedFunction(42)
scopedFunction = 42

// FIXME: Should be an error -- a type name and a function cannot overload.
var _ : Int = TypeNameWins(42)

TypeNameWins = 42 // expected-error {{cannot assign to immutable expression of type 'Int'}}
var _ : TypeNameWins // no-warning

// rdar://problem/21739333
protocol HasFooSub : HasFoo { }

extension HasFooSub {
  var foo: Int { return 0 }
}

// CHECK-LABEL: func_decl{{.*}}"testHasFooSub(_:)"
func testHasFooSub(_ hfs: HasFooSub) -> Int {
  // CHECK: return_stmt
  // CHECK-NOT: func_decl
  // CHECK: member_ref_expr{{.*}}decl='overload_vars.(file).HasFoo.foo [with
  return hfs.foo
}

extension HasBar {
  var bar: Int { return 0 }
}

// CHECK-LABEL: func_decl{{.*}}"testHasBar(_:)"
func testHasBar(_ hb: HasBar) -> Int {
  // CHECK: return_stmt
  // CHECK-NOT: func_decl
  // CHECK: member_ref_expr{{.*}}decl='overload_vars.(file).HasBar.bar [with
  return hb.bar
}


