// RUN: %target-swift-frontend -emit-ir %s

public func apply<Input, each Transformed>(
  _ transform: (repeat (Input) -> each Transformed),
  _ input: Input
) -> (repeat each Transformed) {
  (repeat (each transform)(input))
}

struct S: Equatable {
  // MARK: Equatables
  let eq = "🎛"
  let u = 1.0 / 1_000_000
  let al = 13

  let breaksEquatableSynthesis: Void

  static func == (ecuador0: Self, ecuador1: Self) -> Bool {
    let getProperties = (
      \.eq as (Self) -> _,
      \.u as (Self) -> _,
      \.al as (Self) -> _
    )
    return apply(getProperties, ecuador0) == apply(getProperties, ecuador1)
  }
}
