// RUN: %target-swift-frontend -emit-ir %s | %FileCheck %s

// Ensure that the superclass's generic argument canonicalizes to
// Sequence.Element.

// CHECK: @"symbolic _____y7ElementSTQasdzG

public protocol ElementTypeProtocol: RandomAccessCollection {
    typealias ElementType = Element
}
extension Array: ElementTypeProtocol {}

private class WrapperBase<T> {}
private final class WrapperDerived<C: ElementTypeProtocol>: WrapperBase<C.ElementType> {
    init(base: C) {
        let _ = base
    }
}
