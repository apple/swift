//RUN: %target-swift-frontend -emit-sil -verify %s

@_silgen_name("cond")
func cond() -> Bool

struct Foo: ~Copyable {}

func consume(_: consuming Foo) {}

func test1(_ x: inout Foo, _ y: consuming Foo) { // expected-error{{missing reinitialization}}
    consume(x) // expected-note{{consumed here}}
    if cond() {
        return
    } else {
        x = y
    }
}

func test2(_ x: inout Foo, _ y: consuming Foo) { // expected-error{{missing reinitialization}}
    consume(x) // expected-note{{consumed here}}
    if cond() {
        x = y
    } else {
        return
    }
}
