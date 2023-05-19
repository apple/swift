// RUN: %target-typecheck-verify-swift

let _: AnyObject = "a" // expected-error {{value of type 'String' expected to be instance of class or class-constrained type}}

let _: [AnyObject] = ["a"] // expected-error {{cannot convert value of type 'String' to expected element type 'AnyObject'}}
let _: [String: AnyObject] = ["a": "a"] // expected-error {{cannot convert value of type 'String' to expected dictionary value type 'AnyObject'}}
let _: [AnyObject: String] = ["a": "a"] // expected-error {{type 'AnyObject' does not conform to protocol 'Hashable'}}
// expected-error@-1 {{cannot convert value of type 'String' to expected dictionary key type 'AnyObject'}}
let _: (AnyObject, Void) = ("a", ()) // expected-error {{value of type 'String' expected to be instance of class or class-constrained type}}
