// RUN: %target-swift-frontend -emit-ir %s -disable-availability-checking

public protocol SomeObject {}

public struct VariadicObjectBox<each S: SomeObject> {
    public let object: (repeat each S)
}

@resultBuilder
public struct VariadicObjectBoxBuilder {
    public static func buildBlock<each S: SomeObject>(_ object: (repeat each S)) -> VariadicObjectBox<repeat each S> {
        VariadicObjectBox(object: (repeat each object))
    }
}

public struct ObjectContainer {
    public init<each S: SomeObject>(box: VariadicObjectBox<repeat each S>) {
        // This init compiles
    }
    
    public init<each S: SomeObject>(@VariadicObjectBoxBuilder _ builder: () -> VariadicObjectBox<repeat each S>) {
        // This init crashes on compilation
    }
}
