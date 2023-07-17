// RUN: %target-swift-frontend -emit-ir %s -disable-availability-checking

public protocol P {
  associatedtype A
}

public struct G<each T> {}

public struct S<T: P, each U: P> where repeat T.A == G<repeat each U> {
    public init(predicate: repeat each U) {}
}

