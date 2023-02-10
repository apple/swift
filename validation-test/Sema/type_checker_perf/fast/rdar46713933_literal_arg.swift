// RUN: %target-typecheck-verify-swift -solver-expression-time-threshold=1
// REQUIRES: tools-release,no_asan

func wrap<T>(_ key: String, _ value: T) -> T { return value }
func wrap<T: ExpressibleByIntegerLiteral>(_ key: String, _ value: T) -> T { return value }
func wrap<T: ExpressibleByFloatLiteral>(_ key: String, _ value: T) -> T { return value }
func wrap<T: ExpressibleByStringLiteral>(_ key: String, _ value: T) -> T { return value }

func wrapped() -> Int {
  return wrap("1", 1) + wrap("1", 1) + wrap("1", 1) + wrap("1", 1) + wrap("1", 1) + wrap("1", 1)
}
