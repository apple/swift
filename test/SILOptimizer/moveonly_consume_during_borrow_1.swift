// RUN: %target-swift-frontend -emit-sil -verify %s

func foo(x: consuming Foo) { // expected-error{{'x' used after consume}}
    x.bar.foo(x) // expected-error{{overlapping accesses to 'x'}} expected-note 3 {{}}
}

struct Foo: ~Copyable {
    var bar: Bar {
        _read { fatalError() }
    }
}

struct Bar: ~Copyable {
    func foo(_: consuming Foo) {}
}
