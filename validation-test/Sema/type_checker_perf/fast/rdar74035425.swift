// RUN: %target-typecheck-verify-swift -solver-expression-time-threshold=1
// REQUIRES: tools-release,no_asan

struct Value {
  let debugDescription: String
}

func test(values: [[Value]]) -> String {
  "[" + "" + values.map({ "[" + $0.map({ $0.debugDescription }).joined(separator: ", ") + "]" }).joined(separator: ", ") + "]"
}
