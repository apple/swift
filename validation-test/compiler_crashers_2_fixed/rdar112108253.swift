// RUN: %target-swift-frontend -emit-ir %s -disable-availability-checking

public protocol P {
  associatedtype A
}

public struct G<each T> {}

public protocol Q {
  init()
}

public struct S<T: P, each U: P>: Q where repeat T.A == G<repeat (each U).A> {
  public init() {}
  public init(predicate: repeat each U) {}
}

