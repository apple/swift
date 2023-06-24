// RUN: %target-typecheck-verify-swift

// https://github.com/apple/swift/issues/66553

func baz(y: [Int], z: Int) -> Int {
  switch z {
  case y[let z]: // expected-error {{'let' binding pattern cannot appear in an expression}}
    z
  default:
    z
  }
}
