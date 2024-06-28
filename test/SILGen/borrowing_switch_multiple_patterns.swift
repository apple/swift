// RUN: %target-swift-emit-silgen -verify %s

struct Inner: ~Copyable {}
enum Outer: ~Copyable { case value(Inner, Int) }

func borrow(_: borrowing Inner) {}
func consume(_: consuming Inner) {}

func foo(x: borrowing Outer) {
    switch x {
    case .value(let y, 0), // expected-error{{not implemented}}
         .value(let y, _):
        borrow(y)
    }

}

func bar(x: borrowing Outer) {
    switch x {
    case .value(let y, 0):
        borrow(y)
        fallthrough

    case .value(let y, _): // expected-error{{not implemented}}
        borrow(y)
    }

}

func zim(x: consuming Outer) {
    switch consume x {
    case .value(let y, 0), // expected-error{{not implemented}}
         .value(let y, _):
        consume(y)
    }

}

func zang(x: consuming Outer) {
    switch consume x {
    case .value(let y, 0):
        // should eventually test that this gets diagnosed as a double-consume
        //consume(y)
        fallthrough

    case .value(let y, _): // expected-error{{not implemented}}
        consume(y)
    }

}
