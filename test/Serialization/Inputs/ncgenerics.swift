
public protocol Generator<Value> {
    associatedtype Value
    mutating func next() -> Value?
}

public struct Counter: Generator {
    var state: Int = 0
    public mutating func next() -> Int? {
        let value = state
        state += 1
        return value
    }
}

public func advance<T: Generator>(by n: Int, _ t: inout T) {
    for _ in 0..<n {
        _ = t.next()
    }
}

#if $NoncopyableGenerics
public enum Maybe<Wrapped: ~Copyable>: ~Copyable {
    case just(Wrapped)
    case none
}

extension Maybe: Copyable where Wrapped: Copyable {}

public func ncIdentity<T: ~Copyable>(_ t: consuming T) -> T { return t }

public protocol Either<Left, Right>: ~Copyable {
    associatedtype Left: ~Copyable
    associatedtype Right: ~Copyable
}
#endif

