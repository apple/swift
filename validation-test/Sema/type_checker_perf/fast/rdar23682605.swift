// RUN: %target-typecheck-verify-swift -solver-expression-time-threshold=1
// REQUIRES: tools-release,no_asan

func memoize<T: Hashable, U>( body: @escaping ((T)->U, T)->U ) -> (T)->U {
  var memo = Dictionary<T, U>()
  var result: ((T)->U)!
  result = { x in
    if let q = memo[x] { return q }
    let r = body(result, x)
    memo[x] = r
    return r
  }
  return result
}

let fibonacci = memoize {
  fibonacci, n in
  n < 2 ? Double(n) : fibonacci(n - 1) + fibonacci(n - 2)
}
