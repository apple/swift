// RUN: %target-run-simple-swift
// REQUIRES: executable_test

import _Differentiation
import StdlibUnittest

var ModifyAccessorTests = TestSuite("ModifyAccessor")

ModifyAccessorTests.test("SimpleModifyAccessor") {
  struct S: Differentiable {
    private var _x : Float

    func _endMutation() {}

    var x: Float {
      get{_x}
      set(newValue) { _x = newValue }
      _modify {
        defer { _endMutation() }
        if (x > 0) {
          yield &_x
        } else {
          yield &_x
        }
      }
    }

    init(_ x : Float) {
      self._x = x
    }
  }

  func modify_struct(_ x : Float) -> Float {
    var s = S(x)
    s.x *= s.x
    return s.x
  }

  expectEqual((100, 20), valueWithGradient(at: 10, of: modify_struct))
}

runAllTests()

