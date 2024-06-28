// RUN: %target-swift-frontend -dump-parse -experimental-skip-all-function-bodies %s | %FileCheck %s

func foo() {
  print("hello")
}
// CHECK-NOT: brace_stmt
