public func module1Func() -> Int { return 1 }

public protocol P {
    func memberMethod()
    func defaultProvided()
}

extension P {
    public func defaultProvided() {}
}
