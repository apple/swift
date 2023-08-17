// RUN: %target-typecheck-verify-swift
// RUN: %target-typecheck-verify-swift -enable-library-evolution

@frozen
public struct NonCopyableS : ~Copyable {}
@frozen
public enum NonCopyableE : ~Copyable {}

public struct S {
    public var x: NonCopyableS { // expected-error {{noncopyable variable cannot provide a read and set accessor}}
        _read { fatalError() }
        set { fatalError() }
    }
    public var x2: NonCopyableE { // expected-error {{noncopyable variable cannot provide a read and set accessor}}
        _read { fatalError() }
        set { fatalError() }
    }
    public subscript(x x: Int) -> NonCopyableS { // expected-error {{noncopyable subscript cannot provide a read and set accessor}}
        _read { fatalError() }
        set { fatalError() }
    }
    public subscript(y y: Int) -> NonCopyableE { // expected-error {{noncopyable subscript cannot provide a read and set accessor}}
        _read { fatalError() }
        set { fatalError() }
    }
}

public class C {
    public var x: NonCopyableS { // expected-error {{noncopyable variable cannot provide a read and set accessor}}
        _read { fatalError() }
        set { fatalError() }
    }
    public var x2: NonCopyableE { // expected-error {{noncopyable variable cannot provide a read and set accessor}}
        _read { fatalError() }
        set { fatalError() }
    }
    public subscript(x x: Int) -> NonCopyableS { // expected-error {{noncopyable subscript cannot provide a read and set accessor}}
        _read { fatalError() }
        set { fatalError() }
    }
    public subscript(y y: Int) -> NonCopyableE { // expected-error {{noncopyable subscript cannot provide a read and set accessor}}
        _read { fatalError() }
        set { fatalError() }
    }
}

public enum E {
    public var x: NonCopyableS { // expected-error {{noncopyable variable cannot provide a read and set accessor}}
        _read { fatalError() }
        set { fatalError() }
    }
    public var x2: NonCopyableE { // expected-error {{noncopyable variable cannot provide a read and set accessor}}
        _read { fatalError() }
        set { fatalError() }
    }
    public subscript(x x: Int) -> NonCopyableS { // expected-error {{noncopyable subscript cannot provide a read and set accessor}}
        _read { fatalError() }
        set { fatalError() }
    }
    public subscript(y y: Int) -> NonCopyableE { // expected-error {{noncopyable subscript cannot provide a read and set accessor}}
        _read { fatalError() }
        set { fatalError() }
    }
}
