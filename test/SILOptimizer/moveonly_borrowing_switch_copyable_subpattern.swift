// RUN: %target-swift-frontend -emit-sil -verify -enable-experimental-feature BorrowingSwitch %s
// RUN: %target-swift-frontend -enable-experimental-feature NoncopyableGenerics -emit-sil -verify -enable-experimental-feature BorrowingSwitch %s

struct Payload: ~Copyable {
    var x: Int
    var y: String
}

enum Foo: ~Copyable {
    case nonCopyablePayload(Payload)
    case copyablePayload(String)
}

func hungryCondition(_: consuming String) -> Bool { fatalError() }
func condition(_: borrowing String) -> Bool { fatalError() }

func eat(_: consuming String) {}
func nibble(_: borrowing String) {}

func test(borrowing foo: borrowing Foo) {
    switch foo {
    case .nonCopyablePayload(let x): // expected-warning{{}}
        break

    // OK to form a normal `let` binding when the payload is copyable.
    // Then it's OK to consume copies of it in the condition clause
    // and in the body.
    case .copyablePayload(let x) where hungryCondition(x):
        eat(x)
        nibble(x)
    case .copyablePayload(let x) where condition(x):
        eat(x)
        nibble(x)

    // `_borrowing` match variables impose the no-implicit-copy constraint
    // like `borrowing` parameters do.
    case .copyablePayload(_borrowing x) // expected-error{{'x' is borrowed and cannot be consumed}}
      where hungryCondition(x): // expected-note{{consumed here}}
        eat(x) // expected-note{{consumed here}}
        nibble(x)

    case .copyablePayload(_borrowing x) // expected-error{{'x' is borrowed and cannot be consumed}}
      where condition(x):
        eat(x) // expected-note{{consumed here}}
        nibble(x)

    // Explicit copies are OK.
    case .copyablePayload(_borrowing x)
      where hungryCondition(copy x):
        eat(copy x)
        nibble(x)

    case .copyablePayload(_borrowing x):
        nibble(x)
    }
}
