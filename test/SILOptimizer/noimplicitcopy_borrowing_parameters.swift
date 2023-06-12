// RUN: %target-swift-frontend -emit-sil %s -verify -sil-verify-all

////////////////////////
// MARK: Declarations //
////////////////////////

public class Klass {}

public struct NonTrivialStruct {
    var k = Klass()

    func doSomethingDefault() {}
    borrowing func doSomethingBorrowing() {}
    mutating func doSomethingMutating() {}
}

public protocol P {
    static var value: Self { get }
}

public struct GenericNonTrivialStruct<T : P> {
    var t = T.value

    func doSomethingDefault() {}
    borrowing func doSomethingBorrowing() {}
    mutating func doSomethingMutating() {}
}

public struct TrivialStruct {
    var k: Int

    func doSomethingDefault() {}
    borrowing func doSomethingBorrowing() {}
    mutating func doSomethingMutating() {}
}

enum LoadableEnum {
    case x(NonTrivialStruct)
    case y(Int)
}

enum TrivialEnum {
    case x(Int64)
    case y(Int)
}

enum AddressOnlyEnum<T> {
    case x(NonTrivialStruct)
    case y(T)
}

func borrowValDefault(_ x: NonTrivialStruct) {}
func borrowValBorrowing(_ x: borrowing NonTrivialStruct) {}
func borrowValDefault(_ x: TrivialStruct) {}
func borrowValBorrowing(_ x: borrowing TrivialStruct) {}
func borrowValDefault<T : P>(_ x: GenericNonTrivialStruct<T>) {}
func borrowValBorrowing<T : P>(_ x: borrowing GenericNonTrivialStruct<T>) {}
func consumeVal(_ x: consuming NonTrivialStruct) {}
func consumeVal<T : P>(_ x: consuming GenericNonTrivialStruct<T>) {}

///////////////////////////////////////////
// MARK: Simple Loadable Borrowing Tests //
///////////////////////////////////////////

func testLoadableBorrowingConsume(_ x: borrowing NonTrivialStruct) {
    // expected-error @-1 {{'x' is borrowed and cannot be consumed}}
    let _ = x // expected-note {{consumed here}}
    let _ = x.k
}

func testLoadableBorrowingConsume2(_ x: borrowing NonTrivialStruct) {
    // expected-error @-1 {{'x' is borrowed and cannot be consumed}}
    var y = x // expected-note {{consumed here}}
    y = x // expected-note {{consumed here}}
    _ = y
}

func testLoadableBorrowingConsume3(_ x: borrowing NonTrivialStruct) {
    // expected-error @-1 {{'x' is borrowed and cannot be consumed}}
    let y = x // expected-note {{consumed here}}
    _ = y
}

func testLoadableBorrowingUse(_ x: borrowing NonTrivialStruct) {
    _ = x
}

func testLoadableBorrowingUseAndConsume(_ x: borrowing NonTrivialStruct) {
    // expected-error @-1 {{'x' is borrowed and cannot be consumed}}
    _ = x
    let _ = x // expected-note {{consumed here}}
}

func testLoadableBorrowingCallBorrowValDefault(_ x: borrowing NonTrivialStruct) {
    borrowValDefault(x)
}

func testLoadableBorrowingCallBorrowValBorrowing(_ x: borrowing NonTrivialStruct) {
    borrowValBorrowing(x)
}

func testLoadableBorrowingCallMethodSelfDefault(_ x: borrowing NonTrivialStruct) {
    x.doSomethingDefault()
}

func testLoadableBorrowingCallMethodSelfBorrowing(_ x: borrowing NonTrivialStruct) {
    x.doSomethingBorrowing()
}

func testLoadableBorrowingEscapingClosure(_ x: borrowing NonTrivialStruct) {
    // expected-error @-1 {{'x' cannot be captured by an escaping closure since it is a borrowed parameter}}
    var f: () -> () = {}
    f = { // expected-note {{closure capturing 'x' here}}
        _ = x
    }
    _ = f
}

func testLoadableBorrowingNonEscapingClosure(_ x: borrowing NonTrivialStruct) {
    func useNonEscaping(_ f: () -> ()) {}
    useNonEscaping {
        _ = x
    }
}

func testLoadableBorrowingConsumeOperator(_ x: borrowing NonTrivialStruct) {
    // expected-error @-1 {{'x' is borrowed and cannot be consumed}}
    _ = consume x // expected-note {{consumed here}}
}

func testLoadableBorrowingEnum(_ x: borrowing LoadableEnum) {
    // expected-error @-1 {{'x' is borrowed and cannot be consumed}}
    switch x { // expected-note {{consumed here}}
    case let .x(y):
        _ = y
        break
    case .y:
        break
    }
}

func testLoadableBorrowingCopyOperator(_ x: borrowing NonTrivialStruct) {
    _ = copy x
    let _ = copy x
    consumeVal(copy x)
}

//////////////////////////////////////////
// MARK: Trivial Struct Borrowing Tests //
//////////////////////////////////////////

func testTrivialBorrowingConsume(_ x: borrowing TrivialStruct) {
    // expected-error @-1 {{'x' is borrowed and cannot be consumed}}
    let _ = x // expected-note {{consumed here}}
    let _ = x.k
}

func testTrivialBorrowingConsume2(_ x: borrowing TrivialStruct) {
    // expected-error @-1 {{'x' is borrowed and cannot be consumed}}
    var y = x // expected-note {{consumed here}}
    y = x // expected-note {{consumed here}}
    _ = y
}

func testTrivialBorrowingConsume3(_ x: borrowing TrivialStruct) {
    // expected-error @-1 {{'x' is borrowed and cannot be consumed}}
    let y = x // expected-note {{consumed here}}
    _ = y
}

func testTrivialBorrowingUse(_ x: borrowing TrivialStruct) {
    _ = x
}

func testTrivialBorrowingUseAndConsume(_ x: borrowing TrivialStruct) {
    // expected-error @-1 {{'x' is borrowed and cannot be consumed}}
    _ = x
    let _ = x // expected-note {{consumed here}}
}

func testTrivialBorrowingCallBorrowValDefault(_ x: borrowing TrivialStruct) {
    borrowValDefault(x)
}

func testTrivialBorrowingCallBorrowValBorrowing(_ x: borrowing TrivialStruct) {
    borrowValBorrowing(x)
}

func testTrivialBorrowingCallMethodSelfDefault(_ x: borrowing TrivialStruct) {
    x.doSomethingDefault()
}

func testTrivialBorrowingCallMethodSelfBorrowing(_ x: borrowing TrivialStruct) {
    x.doSomethingBorrowing()
}

func testTrivialBorrowingEscapingClosure(_ x: borrowing TrivialStruct) {
    // expected-error @-1 {{'x' cannot be captured by an escaping closure since it is a borrowed parameter}}
    var f: () -> () = {}
    f = { // expected-note {{closure capturing 'x' here}}
        _ = x
    }
    _ = f
}

func testTrivialBorrowingNonEscapingClosure(_ x: borrowing TrivialStruct) {
    // expected-error @-1 {{'x' cannot be captured by an escaping closure since it is a borrowed parameter}}
    // TODO: Wrong
    func useNonEscaping(_ f: () -> ()) {}
    useNonEscaping { // expected-note {{closure capturing 'x' here}}
        _ = x
    }
}

func testTrivialBorrowingConsumeOperator(_ x: borrowing TrivialStruct) {
    // expected-error @-1 {{'x' is borrowed and cannot be consumed}}
    _ = consume x // expected-note {{consumed here}}
}

func testTrivialBorrowingEnum(_ x: borrowing TrivialEnum) {
    // expected-error @-1 {{'x' is borrowed and cannot be consumed}}
    switch x { // expected-note {{consumed here}}
    case let .x(y):
        _ = y
        break
    case .y:
        break
    }
}

func testTrivialBorrowingCopyOperator(_ x: borrowing TrivialStruct) {
    _ = copy x
    let _ = copy x
}

//////////////////////////////////////////////
// MARK: Simple AddressOnly Borrowing Tests //
//////////////////////////////////////////////

func testAddressOnlyBorrowSimple<T>(_ x: borrowing T) {}

func testAddressOnlyBorrowingConsume<T : P>(_ x: borrowing GenericNonTrivialStruct<T>) {
    // expected-error @-1 {{'x' is borrowed and cannot be consumed}}
    let _ = x // expected-note {{consumed here}}
    let _ = x.t
}

func testAddressOnlyBorrowingConsume2<T : P>(_ x: borrowing GenericNonTrivialStruct<T>) {
    // expected-error @-1 {{'x' is borrowed and cannot be consumed}}
    // expected-error @-2 {{'x' is borrowed and cannot be consumed}}
    var y = x // expected-note {{consumed here}}
    y = x // expected-note {{consumed here}}
    _ = y
}

func testAddressOnlyBorrowingConsume3<T : P>(_ x: borrowing GenericNonTrivialStruct<T>) {
    // expected-error @-1 {{'x' is borrowed and cannot be consumed}}
    let y = x // expected-note {{consumed here}}
    _ = y
}

func testAddressOnlyBorrowingUse<T : P>(_ x: borrowing GenericNonTrivialStruct<T>) {
    _ = x
}

func testAddressOnlyBorrowingUseAndConsume<T : P>(_ x: borrowing GenericNonTrivialStruct<T>) {
    // expected-error @-1 {{'x' is borrowed and cannot be consumed}}
    borrowValDefault(x)
    let _ = x // expected-note {{consumed here}}
}

func testAddressOnlyBorrowingCallBorrowValDefault<T : P>(_ x: borrowing GenericNonTrivialStruct<T>) {
    borrowValDefault(x)
}

func testAddressOnlyBorrowingCallBorrowValBorrowing<T : P>(_ x: borrowing GenericNonTrivialStruct<T>) {
    borrowValBorrowing(x)
}

func testAddressOnlyBorrowingCallMethodSelfDefault<T : P>(_ x: borrowing GenericNonTrivialStruct<T>) {
    x.doSomethingDefault()
}

func testAddressOnlyBorrowingCallMethodSelfBorrowing<T : P>(_ x: borrowing GenericNonTrivialStruct<T>) {
    x.doSomethingBorrowing()
}

func testAddressOnlyBorrowingEscapingClosure<T : P>(_ x: borrowing GenericNonTrivialStruct<T>) {
    // expected-error @-1 {{'x' is borrowed and cannot be consumed}}
    var f: () -> () = {}
    f = { // expected-note {{consumed here}}
        _ = x
    }
    _ = f
}

func testAddressOnlyBorrowingNonEscapingClosure<T : P>(_ x: borrowing GenericNonTrivialStruct<T>) {
    // expected-error @-1 {{'x' is borrowed and cannot be consumed}}
    func useNonEscaping(_ f: () -> ()) {}
    useNonEscaping { // expected-note {{consumed here}}
        _ = x
    }
}

func testAddressOnlyBorrowingCast<T : P>(_ x: borrowing GenericNonTrivialStruct<T>) {
    // expected-error @-1 {{'x' is borrowed and cannot be consumed}}
    let _ = x as Any // expected-note {{consumed here}}
}

func testAddressOnlyBorrowingCastCheck<T : P>(_ x: borrowing GenericNonTrivialStruct<T>) {
    // expected-error @-1 {{'x' is borrowed and cannot be consumed}}
    if x is Any { // expected-note {{consumed here}}
        // expected-warning @-1 {{'is' test is always true}}

    }
}

func testAddressOnlyBorrowingEnum<T>(_ x: borrowing AddressOnlyEnum<T>) {
    // expected-error @-1 {{'x' is borrowed and cannot be consumed}}
    switch x { // expected-note {{consumed here}}
    case let .x(y):
        _ = y
        break
    case let .y(z):
        _ = z
        break
    }
}

func testAddressOnlyConsumeOperator<T>(_ x: borrowing GenericNonTrivialStruct<T>) {
    // expected-error @-1 {{'x' is borrowed and cannot be consumed}}
    _ = consume x // expected-note {{consumed here}}
}

func testAddressOnlyCopyOperator<T>(_ x: borrowing GenericNonTrivialStruct<T>) {
    _ = copy x
    let _ = copy x
    consumeVal(copy x)
}


///////////////////////////////
// MARK: Loadable Self Tests //
///////////////////////////////

struct LoadableSelfTest {
    var k = Klass()

    consuming func consumeSelf() {}
    func doSomethingDefault() {}
    borrowing func doSomethingBorrowing() {}

    borrowing func testUseSelf() {
        _ = self
    }

    borrowing func testLetUseSelf() {
        // expected-error @-1 {{'self' is borrowed and cannot be consumed}}
        let _ = self // expected-note {{consumed here}}
    }

    borrowing func callDoSomethingDefault() {
        self.doSomethingDefault()
    }

    borrowing func callDoSomethingBorrowing() {
        self.doSomethingBorrowing()
    }

    borrowing func testConsumeOperatorSelf() {
        // expected-error @-1 {{'self' is borrowed and cannot be consumed}}
        _ = consume self // expected-note {{consumed here}}
    }

    borrowing func testCopyOperatorSelf() {
        _ = copy self
        let _ = copy self
        (copy self).consumeSelf()
    }

    borrowing func testUseField() {
        _ = self.k
    }

    borrowing func testConsumeField() {
        // No error, since our field is copyable.
        let _ = self.k
    }

    borrowing func testCallConsumeMethod() {
        // expected-error @-1 {{'self' is borrowed and cannot be consumed}}
        consumeSelf() // expected-note {{consumed here}}
    }

    borrowing func testCallFreeFunctionConsumeSelf() {
        // expected-error @-1 {{'self' is borrowed and cannot be consumed}}
        consumeLoadableSelfTest(self) // expected-note {{consumed here}}
    }

    borrowing func testCallEscapingClosure() {
        // expected-error @-1 {{'self' cannot be captured by an escaping closure since it is a borrowed parameter}}
        var f: () -> () = {}
        f = { // expected-note {{closure capturing 'self' here}}
            let _ = self
        }
        f()
    }

    borrowing func testCallNonEscapingClosure() {
        func f(_ x: () -> ()) {}
        f {
            _ = self
        }
    }
}

func consumeLoadableSelfTest(_ x: consuming LoadableSelfTest) {}

///////////////////////////////////
// MARK: Address Only Self Tests //
///////////////////////////////////

struct AddressOnlySelfTest<T> {
    var t: T

    consuming func consumeSelf() {}
    func doSomethingDefault() {}
    borrowing func doSomethingBorrowing() {}

    borrowing func testUseSelf() {
        _ = self
    }

    borrowing func testLetUseSelf() {
        // expected-error @-1 {{'self' is borrowed and cannot be consumed}}
        let _ = self // expected-note {{consumed here}}
    }

    borrowing func callDoSomethingDefault() {
        self.doSomethingDefault()
    }

    borrowing func callDoSomethingBorrowing() {
        self.doSomethingBorrowing()
    }

    borrowing func testConsumeOperatorSelf() {
        // expected-error @-1 {{'self' is borrowed and cannot be consumed}}
        _ = consume self // expected-note {{consumed here}}
    }

    borrowing func testCopyOperatorSelf() {
        _ = copy self
        let _ = copy self
        (copy self).consumeSelf()
    }

    borrowing func testUseField() {
        _ = self.t
    }

    borrowing func testConsumeField() {
        // No error, since our field is copyable.
        let _ = self.t
    }

    borrowing func testCallConsumeMethod() {
        // expected-error @-1 {{'self' is borrowed and cannot be consumed}}
        consumeSelf() // expected-note {{consumed here}}
    }

    borrowing func testCallFreeFunctionConsumeSelf() {
        // expected-error @-1 {{'self' is borrowed and cannot be consumed}}
        consumeAddressOnlySelfTest(self) // expected-note {{consumed here}}
    }

    borrowing func testCallEscapingClosure() {
        // expected-error @-1 {{'self' is borrowed and cannot be consumed}}
        // TODO: Capture.
        var f: () -> () = {}
        f = { // expected-note {{consumed here}}
            let _ = self
        }
        f()
    }

    borrowing func testCallNonEscapingClosure() {
        // expected-error @-1 {{'self' is borrowed and cannot be consumed}}
        // TODO: fix
        func f(_ x: () -> ()) {}
        f { // expected-note {{consumed here}}
            _ = self
        }
    }
}

func consumeAddressOnlySelfTest<T>(_ x: consuming AddressOnlySelfTest<T>) {}

//////////////////////////////
// MARK: Trivial Self Tests //
//////////////////////////////

struct TrivialSelfTest {
    var t: Int

    consuming func consumeSelf() {}
    func doSomethingDefault() {}
    borrowing func doSomethingBorrowing() {}

    borrowing func testUseSelf() {
        _ = self
    }

    borrowing func testLetUseSelf() {
        // expected-error @-1 {{'self' is borrowed and cannot be consumed}}
        let _ = self // expected-note {{consumed here}}
    }

    borrowing func callDoSomethingDefault() {
        self.doSomethingDefault()
    }

    borrowing func callDoSomethingBorrowing() {
        self.doSomethingBorrowing()
    }

    borrowing func testConsumeOperatorSelf() {
        // expected-error @-1 {{'self' is borrowed and cannot be consumed}}
        _ = consume self // expected-note {{consumed here}}
    }

    borrowing func testCopyOperatorSelf() {
        _ = copy self
    }

    borrowing func testUseField() {
        _ = self.t
    }

    borrowing func testConsumeField() {
        // No error, since our field is copyable.
        let _ = self.t
    }

    borrowing func testCallConsumeMethod() {
        // expected-error @-1 {{'self' is borrowed and cannot be consumed}}
        consumeSelf() // expected-note {{consumed here}}
    }

    borrowing func testCallFreeFunctionConsumeSelf() {
        // expected-error @-1 {{'self' is borrowed and cannot be consumed}}
        consumeTrivialSelfTest(self) // expected-note {{consumed here}}
    }

    borrowing func testCallEscapingClosure() {
        // expected-error @-1 {{'self' cannot be captured by an escaping closure since it is a borrowed parameter}}
        var f: () -> () = {}
        f = { // expected-note {{closure capturing 'self' here}}
            let _ = self
        }
        f()
    }

    borrowing func testCallNonEscapingClosure() {
        // expected-error @-1 {{'self' cannot be captured by an escaping closure since it is a borrowed parameter}}
        // TODO: Error
        func f(_ x: () -> ()) {}
        f { // expected-note {{closure capturing 'self' here}}
            _ = self
        }
    }
}

func consumeTrivialSelfTest(_ x: consuming TrivialSelfTest) {}
