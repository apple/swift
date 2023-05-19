// RUN: %target-swift-frontend -emit-ir %s -disable-availability-checking

// FIXME: Add more tests

public protocol P {}

public struct G<each T>: P {}

public func returnsG<each T>(_ t: repeat each T) -> some P {
  return G<repeat each T>()
}
