// RUN: %target-swift-frontend -emit-sil -disable-objc-attr-requires-foundation-module -verify %s

// High-level tests that DI rejects passing let constants to
// mutating witness methods


// Mark: General Definitions

protocol TestProtocol {
    var foo: Int { get set }
}

struct TestStruct: TestProtocol {
    var foo: Int
}

// Mark: - Case1: Illegally mutating let property of class in initializer

class TestClass {
    let testObject: TestProtocol // expected-note {{change 'let' to 'var' to make it mutable}}
    init() {
        testObject = TestStruct(foo: 42)
        testObject.foo = 666 // expected-error {{cannot mutate property 'foo' of immutable value 'self.testObject'}}
    }
}

// Mark: - Case2: Illegally mutating global let constant

let testObject: TestProtocol  // expected-note {{change 'let' to 'var' to make it mutable}}
testObject = TestStruct(foo: 42)

testObject.foo = 666 // expected-error {{cannot mutate property 'foo' of immutable value 'testObject'}}

extension TestProtocol {
    mutating func messThingsUp() {
        foo = 666
    }
    mutating func messThingsUpAndThenThrow() throws {
        foo = 616
    }
}

// Mark: - Case3: Illegally mutating let constant in a function scope

let testObject2: TestProtocol  // expected-note 2 {{change 'let' to 'var' to make it mutable}}
testObject2 = TestStruct(foo: 42)
testObject2.messThingsUp() // expected-error {{mutating method 'messThingsUp' may not be used on immutable value 'testObject2'}}
try! testObject2.messThingsUpAndThenThrow() // expected-error {{mutating method 'messThingsUpAndThenThrow' may not be used on immutable value 'testObject2'}}

func testFunc() {
    let testObject: TestProtocol // expected-note {{change 'let' to 'var' to make it mutable}}

    testObject = TestStruct(foo: 42)
    testObject.foo = 666 // expected-error {{cannot mutate property 'foo' of immutable value 'testObject'}}
}

// Mark: - Case4: Illegally passing a let constants property as an inout parameter

let testObject3: TestProtocol  // expected-note {{change 'let' to 'var' to make it mutable}}
testObject3 = TestStruct(foo: 42)

func mutateThis(mutatee: inout Int) {
    mutatee = 666
}
mutateThis(mutatee: &testObject3.foo) // expected-error {{cannot mutate property 'foo' of immutable value 'testObject3'}}
