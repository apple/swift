// RUN: %target-swift-frontend -enable-experimental-move-only -verify %s -emit-sil

func useValue<T>(_ x: T) {}
func consumeValue<T>(_ x: __owned T) {}

struct GenericAggregate<T> {
    var value: T
}

func test1<T>(_ x: T) {
    @_noImplicitCopy let x2 = x // expected-error {{@_noImplicitCopy can not be used on a generic or existential typed binding or a nominal type containing such typed things}}

    // These fail b/c we use an unchecked_addr_cast to convert addresses from
    // @moveOnly to non-@moveOnly. We should change moveonly_to_copyable to
    // handle addresses as well.
    //
    // An earlier change, I believe made it so that SILGen did not emit these
    // unchecked_addr_cast.
    consumeValue(x2) // expected-error {{'x2' is borrowed and cannot be consumed}}
    // expected-note @-1 {{consumed here}}
    consumeValue(x2) // expected-error {{'x2' is borrowed and cannot be consumed}}
    // expected-note @-1 {{consumed here}}
}
