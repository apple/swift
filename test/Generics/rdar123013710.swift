// RUN: %target-typecheck-verify-swift

public protocol P {
    associatedtype A
    associatedtype B: P

    var b: B { get }
    static func f() -> Any?
}

protocol Q: P where B == Never {} // expected-error {{circular reference}}

extension Never: Q, P { // expected-note 2{{through reference here}}
   public typealias A = Never
   public static func f() -> Any? { nil }
}

extension Q {
   public var b: Never { fatalError() } // expected-note {{through reference here}}
}

