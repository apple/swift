// RUN: %target-swift-emit-silgen %s

public protocol P {
  associatedtype A : Equatable
}

public struct G<A: Equatable>: P {}

extension P where A == Int {
  public init(integerLiteral: Int) {
    fatalError()
  }
}

extension G: ExpressibleByIntegerLiteral where A == Int {}

public func f() -> G<Int> {
  return 123
}
