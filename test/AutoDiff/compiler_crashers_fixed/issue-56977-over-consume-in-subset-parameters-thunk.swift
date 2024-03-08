// RUN: %target-build-swift %s

// https://github.com/apple/swift/issues/56977
// An over-consume in a subset parameters thunk detected after enabling OSSA

// The test is flaky and crashes in some CI builds


import _Differentiation

struct Type2: Differentiable {
  var test1: Double

  @differentiable(reverse)
  public init(test1: Double) {
    self.test1 = test1
  }
}

struct Type1: Differentiable {
  var test1: Double
  var test3: [Type2]

  @differentiable(reverse)
  public init(test1: Double, test3: [Type2]) {
    self.test1 = test1
    self.test3 = test3
  }
}

@differentiable(reverse)
func ingestValue(val1: Double, val2: Double) -> Type1 {
  return Type1(test1: val1 * val2, test3: [])
}
