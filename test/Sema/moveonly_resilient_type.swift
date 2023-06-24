// RUN: %target-swift-frontend -typecheck %s -enable-library-evolution -verify

public struct ResilientStruct : ~Copyable { // expected-error {{noncopyable struct 'ResilientStruct' must be @frozen in library evolution mode; non-@frozen public and @usableFromInline noncopyable types are not supported}}
}

@frozen
public struct FrozenStruct : ~Copyable {
    public init() {}
}

@usableFromInline
struct UsableFromInlineStruct : ~Copyable { // expected-error {{noncopyable struct 'UsableFromInlineStruct' must be @frozen in library evolution mode; non-@frozen public and @usableFromInline noncopyable types are not supported}}
}

public enum ResilientEnum : ~Copyable { // expected-error {{noncopyable enum 'ResilientEnum' must be @frozen in library evolution mode; non-@frozen public and @usableFromInline noncopyable types are not supported}}
}

@frozen
public enum FrozenEnum : ~Copyable {
}

@usableFromInline
enum UsableFromInlineEnum : ~Copyable { // expected-error {{noncopyable enum 'UsableFromInlineEnum' must be @frozen in library evolution mode; non-@frozen public and @usableFromInline noncopyable types are not supported}}
}

public class C {
    @usableFromInline
    var x: FrozenStruct

    public init() {}

    @inlinable
    convenience public init(delegating: ()) {
        self.init()
        x = FrozenStruct()
    }
}
