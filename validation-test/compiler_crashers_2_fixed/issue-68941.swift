// RUN: %target-swift-frontend -emit-ir %s -disable-availability-checking

public struct S<each T> {
    public var value: (repeat each T)

    public init(_ value: repeat each T) {
        self.value = (repeat each value)
        let getters: (repeat () -> each T) = (repeat { each self.value })
    }
}
