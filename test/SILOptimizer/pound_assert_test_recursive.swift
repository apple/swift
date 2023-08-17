// RUN: not %target-swift-frontend -enable-experimental-static-assert -emit-sil %s 2>&1 | %FileCheck %s

// This is a special FileCheck test for testing that we properly catch that we
// are recursing here. The reason why this is separate from the other
// pound_assert tests is that the "limit exceeded" here diagnostic can vary
// depending on the codegen since we are using an arbitrary limit of 512. If the
// codegen changes, the line where we stop evaluating can change meaning that
// the note moves around lines. With FileCheck we have more flexibility to just
// match what we actually want.

// CHECK: error: #assert condition not constant
// CHECK: note: exceeded instruction limit: {{.*}} when evaluating the expression at compile time
// CHECK: limit exceeded here
func recursive(a: Int) -> Int {
  return a == 0 ? 0 : recursive(a: a-1)
}

func test_recursive() {
  #assert(recursive(a: 20000) > 42)
}

