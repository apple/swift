// RUN: %target-swift-frontend -emit-ir %s

public protocol P {}

public protocol Q {
  associatedtype A: P
}

public func f<T: P>(_: T) {}

public func foo1<each T: Q, each U>(t: repeat each T, u: repeat each U)
    where repeat (each U) == (each T).A {
  repeat f(each u)
}

public func foo2<each T: Q>(t: repeat each T, u: repeat (each T).A) {
  repeat f(each u)
}
