// RUN: %target-typecheck-verify-swift -parse-stdlib -disable-availability-checking

import Swift

@_moveOnly class C { // expected-error {{'@_moveOnly' attribute is only valid on structs or enums}}{{1-12=}}
    @_moveOnly // expected-error {{'@_moveOnly' attribute cannot be applied to this declaration}}
    func foo() {}
}

@_moveOnly
struct S {
    @_moveOnly // expected-error {{'@_moveOnly' attribute cannot be applied to this declaration}}
    func foo() {}
}

@_moveOnly
enum E {
    @_moveOnly // expected-error {{'@_moveOnly' attribute cannot be applied to this declaration}}
    func foo() {}
}

@_moveOnly let l = C()  // expected-error {{'@_moveOnly' attribute cannot be applied to this declaration}}

@_moveOnly protocol P {} // expected-error {{'@_moveOnly' attribute is only valid on structs or enums}}{{1-12=}}
@_moveOnly actor A {} // expected-error {{'@_moveOnly' attribute is only valid on structs or enums}}{{1-12=}}
@_moveOnly extension C {} // expected-error {{'@_moveOnly' attribute cannot be applied to this declaration}}{{1-12=}}
