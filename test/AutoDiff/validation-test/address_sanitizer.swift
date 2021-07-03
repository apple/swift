// RUN: %empty-directory(%t)
// RUN: %target-build-swift %s -sanitize=address -o %t/sr13973
// RUN: %target-run %t/sr13973

// REQUIRES: executable_test
// REQUIRES: asan_runtime

import _Differentiation

struct SR13973 {
  let x: Float = 0

  @differentiable
  func errorVector(_ t: Float) -> Float {
    return t
  }
}

func sr13973() {
  let s = SR13973()
  _ = valueWithPullback(at: 0, in: s.errorVector)
}

sr13973()
