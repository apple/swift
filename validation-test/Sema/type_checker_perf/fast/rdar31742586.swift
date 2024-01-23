// RUN: %target-typecheck-verify-swift -solver-expression-time-threshold=1
// REQUIRES: tools-release,no_asan

func rdar31742586() -> Double {
  return -(1 + 2) + -(3 + 4) + 5 - (-(1 + 2) + -(3 + 4) + 5)
}
