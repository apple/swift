// RUN: %target-swift-frontend -emit-ir -O %s

@available(macOS 9999, iOS 9999, tvOS 9999, watchOS 9999, *)
public struct Predicate<each Input> {
    var x: Any? = nil
    public func evaluate(_: repeat each Input) -> Bool { return false }
}

public struct PredicateBindings {
}

@available(macOS 9999, iOS 9999, tvOS 9999, watchOS 9999, *)
public protocol PredicateExpression<Output> {
    associatedtype Output

    func evaluate(_ bindings: PredicateBindings) throws -> Output
}

@available(macOS 9999, iOS 9999, tvOS 9999, watchOS 9999, *)
public struct PredicateEvaluate<
    Condition : PredicateExpression,
    each Input : PredicateExpression
>
where
    Condition.Output == Predicate<repeat (each Input).Output>
{   

    public typealias Output = Bool
    
    public let predicate: Condition
    public let input: (repeat each Input)
        
    public init(predicate: Condition, input: repeat each Input) {
        self.predicate = predicate
        self.input = (repeat each input)
    }
        
    public func evaluate(_ bindings: PredicateBindings) throws -> Output {
        try predicate.evaluate(bindings).evaluate(repeat (each input).evaluate(bindings))
    }
}
