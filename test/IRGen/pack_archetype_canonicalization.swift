// RUN: %target-swift-frontend -emit-ir %s -disable-availability-checking

// This would crash.
public struct G<T> {}

public struct GG<each T> {
  public var variables: (repeat G<each T>)

  public init() {
    fatalError()
  }
}
