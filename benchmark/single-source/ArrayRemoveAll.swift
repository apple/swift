// This test checks the performance of removeAll
// on a non-uniquely referenced array.

import TestsUtils

public let benchmarks = [
  BenchmarkInfo(
    name: "Array.removeAll.keepingCapacity.Int",
    runFunction: run_ArrayRemoveAll_Class,
    tags: [.validation, .api, .Array],
    setUpFunction: { blackHole(inputArray_Class) }
  ),
  BenchmarkInfo(
    name: "Array.removeAll.keepingCapacity.Object",
    runFunction: run_ArrayRemoveAll_Int,
    tags: [.validation, .api, .Array],
    setUpFunction: { blackHole(inputArray_Int) }
  )
]

class Slow {
  public var num: Int

  init(num: Int) {
    self.num = num
  }
}

let inputArray_Int: [Int] = Array(0..<500_000)
let inputArray_Class: [Slow] = (0..<50_000).map(Slow.init(num:))

@inline(never)
func removeAll<T>(_ arr: [T]) -> [T] {
  var copy = arr
  copy.removeAll(keepingCapacity: true)
  return copy
}

@inline(never)
func run_ArrayRemoveAll_Class(_ n: Int) {
  var copy = removeAll(inputArray_Class);
  for _ in 1..<n {
    copy = removeAll(inputArray_Class)
  }
  check(copy.capacity == inputArray_Class.capacity)
}

@inline(never)
func run_ArrayRemoveAll_Int(_ n: Int) {
  var copy = removeAll(inputArray_Int);
  for _ in 1..<n {
    copy = removeAll(inputArray_Int)
  }
  check(copy.capacity == inputArray_Int.capacity)
}
