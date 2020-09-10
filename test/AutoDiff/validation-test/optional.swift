// RUN: %target-run-simple-swift
// REQUIRES: executable_test
// REQUIRES: swift_stable_abi

// Test differentiation of `Optional` values and operations.

import DifferentiationUnittest
import StdlibUnittest

var OptionalTests = TestSuite("OptionalDifferentiation")

//===----------------------------------------------------------------------===//
// Basic tests.
//===----------------------------------------------------------------------===//

// TODO(TF-433): operator `??` lowers to an active `try_apply`.
/*
@differentiable
func optional_nil_coalescing(_ maybeX: Float?) -> Float {
  return maybeX ?? 10
}
*/

OptionalTests.test("Let") {
  @differentiable
  func optional_let(_ maybeX: Float?) -> Float {
    if let x = maybeX {
      return x * x
    }
    return 10
  }
  expectEqual(gradient(at: 10, in: optional_let), .init(20.0))
  expectEqual(gradient(at: nil, in: optional_let), .init(0.0))

  @differentiable
  func optional_let_tracked(_ maybeX: Tracked<Float>?) -> Tracked<Float> {
    if let x = maybeX {
      return x * x
    }
    return 10
  }
  expectEqual(gradient(at: 10, in: optional_let_tracked), .init(20.0))
  expectEqual(gradient(at: nil, in: optional_let_tracked), .init(0.0))

  @differentiable
  func optional_let_nonresilient_tracked(_ maybeX: NonresilientTracked<Float>?)
    -> NonresilientTracked<Float>
  {
    if let x = maybeX {
      return x * x
    }
    return 10
  }
  expectEqual(
    gradient(at: 10, in: optional_let_nonresilient_tracked), .init(20.0))
  expectEqual(
    gradient(at: nil, in: optional_let_nonresilient_tracked), .init(0.0))

  @differentiable
  func optional_let_nested(_ nestedMaybeX: Float??) -> Float {
    if let maybeX = nestedMaybeX {
      if let x = maybeX {
        return x * x
      }
      return 10
    }
    return 10
  }
  expectEqual(gradient(at: 10, in: optional_let_nested), .init(.init(20.0)))
  expectEqual(gradient(at: nil, in: optional_let_nested), .init(.init(0.0)))

  @differentiable
  func optional_let_nested_tracked(_ nestedMaybeX: Tracked<Float>??) -> Tracked<
    Float
  > {
    if let maybeX = nestedMaybeX {
      if let x = maybeX {
        return x * x
      }
      return 10
    }
    return 10
  }
  expectEqual(
    gradient(at: 10, in: optional_let_nested_tracked), .init(.init(20.0)))
  expectEqual(
    gradient(at: nil, in: optional_let_nested_tracked), .init(.init(0.0)))

  @differentiable
  func optional_let_nested_nonresilient_tracked(
    _ nestedMaybeX: NonresilientTracked<Float>??
  ) -> NonresilientTracked<Float> {
    if let maybeX = nestedMaybeX {
      if let x = maybeX {
        return x * x
      }
      return 10
    }
    return 10
  }
  expectEqual(
    gradient(at: 10, in: optional_let_nested_nonresilient_tracked),
    .init(.init(20.0)))
  expectEqual(
    gradient(at: nil, in: optional_let_nested_nonresilient_tracked),
    .init(.init(0.0)))

  @differentiable
  func optional_let_generic<T: Differentiable>(_ maybeX: T?, _ defaultValue: T)
    -> T
  {
    if let x = maybeX {
      return x
    }
    return defaultValue
  }
  expectEqual(gradient(at: 10, 20, in: optional_let_generic), (.init(1.0), 0.0))
  expectEqual(
    gradient(at: nil, 20, in: optional_let_generic), (.init(0.0), 1.0))

  expectEqual(
    gradient(
      at: Tracked<Float>.init(10), Tracked<Float>.init(20),
      in: optional_let_generic), (.init(1.0), 0.0))
  expectEqual(
    gradient(at: nil, Tracked<Float>.init(20), in: optional_let_generic),
    (.init(0.0), 1.0))

  @differentiable
  func optional_let_nested_generic<T: Differentiable>(
    _ nestedMaybeX: T??, _ defaultValue: T
  ) -> T {
    if let maybeX = nestedMaybeX {
      if let x = maybeX {
        return x
      }
      return defaultValue
    }
    return defaultValue
  }

  expectEqual(
    gradient(at: 10.0, 20.0, in: optional_let_nested_generic),
    (.init(.init(1.0)), 0.0))
  expectEqual(
    gradient(at: nil, 20, in: optional_let_nested_generic),
    (.init(.init(0.0)), 1.0))
}

OptionalTests.test("Switch") {
  @differentiable
  func optional_switch(_ maybeX: Float?) -> Float {
    switch maybeX {
    case nil: return 10
    case let .some(x): return x * x
    }
  }
  expectEqual(gradient(at: 10, in: optional_switch), .init(20.0))
  expectEqual(gradient(at: nil, in: optional_switch), .init(0.0))

  @differentiable
  func optional_switch_tracked(_ maybeX: Tracked<Float>?) -> Tracked<Float> {
    switch maybeX {
    case nil: return 10
    case let .some(x): return x * x
    }
  }
  expectEqual(gradient(at: 10, in: optional_switch_tracked), .init(20.0))
  expectEqual(gradient(at: nil, in: optional_switch_tracked), .init(0.0))

  @differentiable
  func optional_switch_nonresilient_tracked(
    _ maybeX: NonresilientTracked<Float>?
  ) -> NonresilientTracked<Float> {
    switch maybeX {
    case nil: return 10
    case let .some(x): return x * x
    }
  }
  expectEqual(
    gradient(at: 10, in: optional_switch_nonresilient_tracked), .init(20.0))
  expectEqual(
    gradient(at: nil, in: optional_switch_nonresilient_tracked), .init(0.0))

  @differentiable
  func optional_switch_nested(_ nestedMaybeX: Float??) -> Float {
    switch nestedMaybeX {
    case nil: return 10
    case let .some(maybeX):
      switch maybeX {
      case nil: return 10
      case let .some(x): return x * x
      }
    }
  }
  expectEqual(gradient(at: 10, in: optional_switch_nested), .init(.init(20.0)))
  expectEqual(gradient(at: nil, in: optional_switch_nested), .init(.init(0.0)))

  @differentiable
  func optional_switch_nested_tracked(_ nestedMaybeX: Tracked<Float>??)
    -> Tracked<Float>
  {
    switch nestedMaybeX {
    case nil: return 10
    case let .some(maybeX):
      switch maybeX {
      case nil: return 10
      case let .some(x): return x * x
      }
    }
  }
  expectEqual(
    gradient(at: 10, in: optional_switch_nested_tracked), .init(.init(20.0)))
  expectEqual(
    gradient(at: nil, in: optional_switch_nested_tracked), .init(.init(0.0)))

  @differentiable
  func optional_switch_nested_nonresilient_tracked(
    _ nestedMaybeX: NonresilientTracked<Float>??
  ) -> NonresilientTracked<Float> {
    switch nestedMaybeX {
    case nil: return 10
    case let .some(maybeX):
      switch maybeX {
      case nil: return 10
      case let .some(x): return x * x
      }
    }
  }
  expectEqual(
    gradient(at: 10, in: optional_switch_nested_nonresilient_tracked),
    .init(.init(20.0)))
  expectEqual(
    gradient(at: nil, in: optional_switch_nested_nonresilient_tracked),
    .init(.init(0.0)))

  @differentiable
  func optional_switch_generic<T: Differentiable>(
    _ maybeX: T?, _ defaultValue: T
  ) -> T {
    switch maybeX {
    case nil: return defaultValue
    case let .some(x): return x
    }
  }
  expectEqual(
    gradient(at: 10, 20, in: optional_switch_generic), (.init(1.0), 0.0))
  expectEqual(
    gradient(at: nil, 20, in: optional_switch_generic), (.init(0.0), 1.0))

  @differentiable
  func optional_switch_nested_generic<T: Differentiable>(
    _ nestedMaybeX: T??, _ defaultValue: T
  ) -> T {
    switch nestedMaybeX {
    case nil: return defaultValue
    case let .some(maybeX):
      switch maybeX {
      case nil: return defaultValue
      case let .some(x): return x
      }
    }
  }
  expectEqual(
    gradient(at: 10, 20, in: optional_switch_nested_generic),
    (.init(.init(1.0)), 0.0))
  expectEqual(
    gradient(at: nil, 20, in: optional_switch_nested_generic),
    (.init(.init(0.0)), 1.0))
}

OptionalTests.test("Optional binding: if let") {
  @differentiable
  func optional_var1(_ maybeX: Float?) -> Float {
    var maybeX = maybeX
    if let x = maybeX {
      return x * x
    }
    return 10
  }
  expectEqual(gradient(at: 10, in: optional_var1), .init(20.0))
  expectEqual(gradient(at: nil, in: optional_var1), .init(0.0))

  @differentiable
  func optional_var1_tracked(_ maybeX: Tracked<Float>?) -> Tracked<Float> {
    var maybeX = maybeX
    if let x = maybeX {
      return x * x
    }
    return 10
  }
  expectEqual(gradient(at: 10, in: optional_var1_tracked), .init(20.0))
  expectEqual(gradient(at: nil, in: optional_var1_tracked), .init(0.0))

  @differentiable
  func optional_var1_nonresilient_tracked(_ maybeX: NonresilientTracked<Float>?)
    -> NonresilientTracked<Float>
  {
    var maybeX = maybeX
    if let x = maybeX {
      return x * x
    }
    return 10
  }
  expectEqual(
    gradient(at: 10, in: optional_var1_nonresilient_tracked), .init(20.0))
  expectEqual(
    gradient(at: nil, in: optional_var1_nonresilient_tracked), .init(0.0))

  @differentiable
  func optional_var1_nested(_ nestedMaybeX: Float??) -> Float {
    var nestedMaybeX = nestedMaybeX
    if let maybeX = nestedMaybeX {
      if var x = maybeX {
        return x * x
      }
      return 10
    }
    return 10
  }
  expectEqual(gradient(at: 10, in: optional_var1_nested), .init(.init(20.0)))
  expectEqual(gradient(at: nil, in: optional_var1_nested), .init(.init(0.0)))

  @differentiable
  func optional_var1_nested_tracked(_ nestedMaybeX: Tracked<Float>??)
    -> Tracked<Float>
  {
    var nestedMaybeX = nestedMaybeX
    if let maybeX = nestedMaybeX {
      if var x = maybeX {
        return x * x
      }
      return 10
    }
    return 10
  }
  expectEqual(
    gradient(at: 10, in: optional_var1_nested_tracked), .init(.init(20.0)))
  expectEqual(
    gradient(at: nil, in: optional_var1_nested_tracked), .init(.init(0.0)))

  @differentiable
  func optional_var1_nested_nonresilient_tracked(
    _ nestedMaybeX: NonresilientTracked<Float>??
  ) -> NonresilientTracked<Float> {
    var nestedMaybeX = nestedMaybeX
    if let maybeX = nestedMaybeX {
      if var x = maybeX {
        return x * x
      }
      return 10
    }
    return 10
  }
  expectEqual(
    gradient(at: 10, in: optional_var1_nested_nonresilient_tracked),
    .init(.init(20.0)))
  expectEqual(
    gradient(at: nil, in: optional_var1_nested_nonresilient_tracked),
    .init(.init(0.0)))

  @differentiable
  func optional_var1_generic<T: Differentiable>(_ maybeX: T?, _ defaultValue: T)
    -> T
  {
    var maybeX = maybeX
    if let x = maybeX {
      return x
    }
    return defaultValue
  }
  expectEqual(
    gradient(at: 10, 20, in: optional_var1_generic), (.init(1.0), 0.0))
  expectEqual(
    gradient(at: nil, 20, in: optional_var1_generic), (.init(0.0), 1.0))

  @differentiable
  func optional_var1_nested_generic<T: Differentiable>(
    _ nestedMaybeX: T??, _ defaultValue: T
  ) -> T {
    var nestedMaybeX = nestedMaybeX
    if let maybeX = nestedMaybeX {
      if var x = maybeX {
        return x
      }
      return defaultValue
    }
    return defaultValue
  }
  expectEqual(
    gradient(at: 10, 20, in: optional_var1_nested_generic),
    (.init(.init(1.0)), 0.0))
  expectEqual(
    gradient(at: nil, 20, in: optional_var1_nested_generic),
    (.init(.init(0.0)), 1.0))
}

OptionalTests.test("Optional binding: if var") {
  @differentiable
  func optional_var2(_ maybeX: Float?) -> Float {
    if var x = maybeX {
      return x * x
    }
    return 10
  }
  expectEqual(gradient(at: 10, in: optional_var2), .init(20.0))
  expectEqual(gradient(at: nil, in: optional_var2), .init(0.0))

  @differentiable
  func optional_var2_tracked(_ maybeX: Tracked<Float>?) -> Tracked<Float> {
    if var x = maybeX {
      return x * x
    }
    return 10
  }
  expectEqual(gradient(at: 10, in: optional_var2_tracked), .init(20.0))
  expectEqual(gradient(at: nil, in: optional_var2_tracked), .init(0.0))

  @differentiable
  func optional_var2_nonresilient_tracked(_ maybeX: NonresilientTracked<Float>?)
    -> NonresilientTracked<Float>
  {
    if var x = maybeX {
      return x * x
    }
    return 10
  }
  expectEqual(
    gradient(at: 10, in: optional_var2_nonresilient_tracked), .init(20.0))
  expectEqual(
    gradient(at: nil, in: optional_var2_nonresilient_tracked), .init(0.0))

  @differentiable
  func optional_var2_nested(_ nestedMaybeX: Float??) -> Float {
    if var maybeX = nestedMaybeX {
      if var x = maybeX {
        return x * x
      }
      return 10
    }
    return 10
  }
  expectEqual(gradient(at: 10, in: optional_var2_nested), .init(.init(20.0)))
  expectEqual(gradient(at: nil, in: optional_var2_nested), .init(.init(0.0)))

  @differentiable
  func optional_var2_nested_tracked(_ nestedMaybeX: Tracked<Float>??)
    -> Tracked<Float>
  {
    if var maybeX = nestedMaybeX {
      if var x = maybeX {
        return x * x
      }
      return 10
    }
    return 10
  }
  expectEqual(
    gradient(at: 10, in: optional_var2_nested_tracked), .init(.init(20.0)))
  expectEqual(
    gradient(at: nil, in: optional_var2_nested_tracked), .init(.init(0.0)))

  @differentiable
  func optional_var2_nested_nonresilient_tracked(
    _ nestedMaybeX: NonresilientTracked<Float>??
  ) -> NonresilientTracked<Float> {
    if var maybeX = nestedMaybeX {
      if var x = maybeX {
        return x * x
      }
      return 10
    }
    return 10
  }
  expectEqual(
    gradient(at: 10, in: optional_var2_nested_nonresilient_tracked),
    .init(.init(20.0)))
  expectEqual(
    gradient(at: nil, in: optional_var2_nested_nonresilient_tracked),
    .init(.init(0.0)))

  @differentiable
  func optional_var2_generic<T: Differentiable>(_ maybeX: T?, _ defaultValue: T)
    -> T
  {
    if var x = maybeX {
      return x
    }
    return defaultValue
  }
  expectEqual(
    gradient(at: 10, 20, in: optional_var2_generic), (.init(1.0), 0.0))
  expectEqual(
    gradient(at: nil, 20, in: optional_var2_generic), (.init(0.0), 1.0))

  @differentiable
  func optional_var2_nested_generic<T: Differentiable>(
    _ nestedMaybeX: T??, _ defaultValue: T
  ) -> T {
    if var maybeX = nestedMaybeX {
      if var x = maybeX {
        return x
      }
      return defaultValue
    }
    return defaultValue
  }
  expectEqual(
    gradient(at: 10, 20, in: optional_var2_nested_generic),
    (.init(.init(1.0)), 0.0))
  expectEqual(
    gradient(at: nil, 20, in: optional_var2_nested_generic),
    (.init(.init(0.0)), 1.0))
}

runAllTests()
