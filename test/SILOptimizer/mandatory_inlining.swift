// RUN: %target-swift-frontend -sil-verify-all -primary-file %s -emit-sil -o - -verify | %FileCheck %s
// RUN: %target-swift-frontend -sil-verify-all -primary-file %s -emit-sil -o - -verify

// These tests are deliberately shallow, because I do not want to depend on the
// specifics of SIL generation, which might change for reasons unrelated to this
// pass

func foo(_ x: Float) -> Float {
  return bar(x);
}

// CHECK-LABEL: sil hidden @$s18mandatory_inlining3foo{{[_0-9a-zA-Z]*}}F
// CHECK: bb0(%0 : $Float):
// CHECK-NEXT: debug_value %0 : $Float, let, name "x"
// CHECK-NEXT: return %0

@_transparent func bar(_ x: Float) -> Float {
  return baz(x)
}

// CHECK-LABEL: sil hidden [transparent] @$s18mandatory_inlining3bar{{[_0-9a-zA-Z]*}}F
  // CHECK-NOT: function_ref
  // CHECK-NOT: apply
  // CHECK: return

@_transparent func baz(_ x: Float) -> Float {
  return x
}

// CHECK-LABEL: sil hidden [transparent] @$s18mandatory_inlining3baz{{[_0-9a-zA-Z]*}}F
// CHECK: return

func spam(_ x: Int) -> Int {
  return x
}

// CHECK-LABEL: sil hidden @$s18mandatory_inlining4spam{{[_0-9a-zA-Z]*}}F

@_transparent func ham(_ x: Int) -> Int {
  return spam(x)
}

// CHECK-LABEL: sil hidden [transparent] @$s18mandatory_inlining3ham{{[_0-9a-zA-Z]*}}F
  // CHECK: function_ref @$s18mandatory_inlining4spam{{[_0-9a-zA-Z]*}}F
  // CHECK: apply
  // CHECK: return

func eggs(_ x: Int) -> Int {
  return ham(x)
}

// CHECK-LABEL: sil hidden @$s18mandatory_inlining4eggs{{[_0-9a-zA-Z]*}}F
  // CHECK: function_ref @$s18mandatory_inlining4spam{{[_0-9a-zA-Z]*}}F
  // CHECK: apply
  // CHECK: return

@_transparent func call_auto_closure(_ x: @autoclosure () -> Bool) -> Bool {
  return x()
}

func test_auto_closure_with_capture(_ x: Bool) -> Bool {
  return call_auto_closure(x)
}

// This should be fully inlined and simply return x; however, there's a lot of
// non-SSA cruft that I don't want this test to depend on, so I'm just going
// to verify that it doesn't have any function applications left

// CHECK-LABEL: sil hidden @{{.*}}test_auto_closure_with_capture
  // CHECK-NOT: = apply
  // CHECK: return

func test_auto_closure_without_capture() -> Bool {
  return call_auto_closure(false)
}

// This should be fully inlined and simply return false, which is easier to check for

// CHECK-LABEL: sil hidden @$s18mandatory_inlining33test_auto_closure_without_captureSbyF
  // CHECK: [[FV:%.*]] = integer_literal $Builtin.Int1, 0
  // CHECK: [[FALSE:%.*]] = struct $Bool ([[FV:%.*]] : $Builtin.Int1)
  // CHECK: return [[FALSE]]

infix operator &&& : LogicalConjunctionPrecedence
infix operator ||| : LogicalDisjunctionPrecedence

@_transparent func &&& (lhs: Bool, rhs: @autoclosure () -> Bool) -> Bool {
  if lhs {
    return rhs()
  }

  return false
}

@_transparent func ||| (lhs: Bool, rhs: @autoclosure () -> Bool) -> Bool {
  if lhs {
    return true
  }

  return rhs()
}

func test_chained_short_circuit(_ x: Bool, y: Bool, z: Bool) -> Bool {
  return x &&& (y ||| z)
}

// The test below just makes sure there are no uninlined [transparent] calls
// left (i.e. the autoclosure and the short-circuiting boolean operators are
// recursively inlined properly)

// CHECK-LABEL: sil hidden @$s18mandatory_inlining26test_chained_short_circuit{{[_0-9a-zA-Z]*}}F
  // CHECK-NOT: = apply [transparent]
  // CHECK: return


// Union element constructors should be inlined automatically.
enum X {
  case onetransp
  case twotransp
}

func testInlineUnionElement() -> X {
  return X.onetransp
  // CHECK-LABEL: sil hidden @$s18mandatory_inlining22testInlineUnionElementAA1XOyF
  // CHECK: enum $X, #X.onetransp!enumelt
  // CHECK-NOT: = apply
  // CHECK: return
}



@_transparent
func call_let_auto_closure(_ x: @autoclosure () -> Bool) -> Bool {
  return x()
}

// CHECK-LABEL: sil hidden @{{.*}}test_let_auto_closure_with_value_capture
// CHECK: bb0(%0 : $Bool):
// CHECK-NEXT: debug_value %0 : $Bool
// CHECK-NEXT: return %0 : $Bool
// CHECK-LABEL: // end sil function '{{.*}}test_let_auto_closure_with_value_capture
func test_let_auto_closure_with_value_capture(_ x: Bool) -> Bool {
  return call_let_auto_closure(x)
}


class C {}

// CHECK-LABEL: sil hidden [transparent] @$s18mandatory_inlining25class_constrained_generic{{[_0-9a-zA-Z]*}}F
@_transparent
func class_constrained_generic<T : C>(_ o: T) -> AnyClass? {
  // CHECK: return
  return T.self
}

// CHECK-LABEL: sil hidden @$s18mandatory_inlining6invokeyyAA1CCF : $@convention(thin) (@guaranteed C) -> () {
func invoke(_ c: C) {
  // CHECK-NOT: function_ref @$s18mandatory_inlining25class_constrained_generic{{[_0-9a-zA-Z]*}}F
  // CHECK-NOT: apply
  _ = class_constrained_generic(c)
  // CHECK: return
}

// Make sure we don't crash.
@_transparent
public func mydo(_ what: @autoclosure () -> ()) {
  what()
}
public class A {
  public func bar() {}
  public func foo(_ act: (@escaping () ->()) -> ()) {
    act { [unowned self] in
      mydo( self.bar() )
    }
  }
}

// This used to crash during mandatory inlining because noreturn folding would
// create sil instructions with undef in unreachable code.
func dontCrash() {
  fatalError() // expected-note {{a call to a never-returning function}}
  let k = "foo" // expected-warning {{will never be executed}}
  switch k {
  case "bar":
    return
  default:
    fatalError("baz \(k)")
  }
}

func switchLoopWithPartialApplyCallee(reportError: ((String) -> (Void))?) {
  let reportError = reportError ?? { error in
    print(error)
  }

  for _ in 0..<1 {
    reportError("foo bar baz")
  }
}

func switchLoopWithPartialApplyCaller() {
  switchLoopWithPartialApplyCallee { error in
      print(error)
  }
}

private class Cl {
  func foo<E: Error>(_ e: E.Type) throws(E) {
  }
}


private func devirtualizeClassMethodWithTypedThrow<E: Error>(_ x: Cl, e: E.Type) {
  try! x.foo(e)
}

