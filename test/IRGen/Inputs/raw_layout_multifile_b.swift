extension Foo where T == Int32 {

}

public func foo(_: borrowing Foo<Int32>) {}
