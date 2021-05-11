import module1

public func module2Func() -> Int {
    return module1Func()
}

public struct Concrete1 : P {
    public func memberMethod() {}
    public func defaultProvided() {}
}

public struct Concrete2 : P {
    public func memberMethod() {}
    public init() {}
}

public func useP<T: P>(_ t: T) {
    t.memberMethod()
}
