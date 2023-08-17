// RUN: %target-swift-frontend -debugger-testing-transform -Xllvm -sil-full-demangle -emit-sil -module-name M %s | %FileCheck %s -check-prefix=CHECK-SIL

// REQUIRES: executable_test
// RUN: %target-build-swift -Xfrontend -debugger-testing-transform %s -o %t
// RUN: %target-codesign %t
// RUN: %target-run %t | %FileCheck %s -check-prefix=CHECK-E2E
// RUN: rm -rf %t

var a = 1001
a = 1002

// CHECK-SIL-LABEL: sil private @$s1MyyXEfU0_ : {{.*}} () -> ()
// CHECK-SIL: [[int:%.*]] = integer_literal {{.*}}, 1002
// CHECK-SIL: [[struct:%.*]] = struct $Int ([[int]] : {{.*}})
// CHECK-SIL: store [[struct]] {{.*}} : $*Int
// CHECK-SIL: string_literal utf8 "a"
// CHECK-SIL: [[PO:%.*]] = function_ref {{.*}}_stringForPrintObjectySSypF
// CHECK-SIL: [[PO_result:%.*]] = apply [[PO]]
// CHECK-SIL: [[check_expect:%.*]] = function_ref {{.*}}_debuggerTestingCheckExpectyySS_SStF
// CHECK-SIL: apply [[check_expect]]

print("a = \(a)") // CHECK-E2E: a = 1002

// CHECK-SIL-LABEL: sil private @$s1MyyXEfU_yyXEfU1_
({ () -> () in
  a = 1003
  // CHECK-SIL: function_ref {{.*}}_debuggerTestingCheckExpectyySS_SStF
  print("a = \(a)") // CHECK-E2E-NEXT: a = 1003
 })()

// CHECK-SIL-LABEL: sil private @$s1MyyXEfU2_
// CHECK-SIL: function_ref {{.*}}_debuggerTestingCheckExpectyySS_SStF
a = 1004
print("a = \(a)") // CHECK-E2E-NEXT: a = 1004

// CHECK-SIL-LABEL: sil hidden @$s1M2f1yyF
// CHECK-SIL-NOT: _debuggerTestingCheckExpect
func f1() {
  // We don't attempt to instrument in this case because we don't try
  // to prove that the var decl is definitively initialized.
  var e: Int
  e = 5001
  print("e = \(e)") // CHECK-E2E-NEXT: e = 5001
}

f1()

// CHECK-SIL-LABEL: sil private @$s1M2f2yyFyyXEfU3_
func f2() {
  var b = 2001
  b = 2002
  // CHECK-SIL: function_ref {{.*}}_debuggerTestingCheckExpectyySS_SStF
  print("b = \(b)") // CHECK-E2E-NEXT: b = 2002
}

f2()

// CHECK-SIL-LABEL: sil private @$s1M2f3yyFyyXEfU_yyXEfU4_
func f3() {
  var c: Int = 3001
  ({ () -> () in
    c = 3002
    // CHECK-SIL: function_ref {{.*}}_debuggerTestingCheckExpectyySS_SStF
    print("c = \(c)") // CHECK-E2E-NEXT: c = 3002
  })()
}

f3()

// CHECK-SIL-LABEL: sil private @$s1M2f4yySaySiGzFyyXEfU5_
func f4(_ d: inout [Int]) {
  d[0] = 4002
  // CHECK-SIL: function_ref {{.*}}_debuggerTestingCheckExpectyySS_SStF
  print("d[0] = \(d[0])") // CHECK-E2E-NEXT: d[0] = 4002
}

var d: [Int] = [4001]
f4(&d)

// CHECK-SIL-LABEL: sil private @$s1M2f5yySSzFyyXEfU6_
func f5(_ v: inout String) {
  v = "Hello world"
  // CHECK-SIL: function_ref {{.*}}_debuggerTestingCheckExpectyySS_SStF
  print("v = \(v)") // CHECK-E2E-NEXT: v = Hello world
}

var v: String = ""
f5(&v)
