// RUN: %target-swift-frontend -emit-sil %s -verify -sil-verify-all

////////////////////////
// MARK: Declarations //
////////////////////////

public class Klass {
}

public struct NonTrivialStruct {
    var k = Klass()

    func doSomethingDefault() {}
    borrowing func doSomethingBorrowing() {}
    consuming func doSomethingConsuming() {}
    mutating func doSomethingMutating() {}
}

public protocol P {
    static var value: Self { get }
}

public struct GenericNonTrivialStruct<T : P> {
    var t = T.value

    func doSomethingDefault() {}
    borrowing func doSomethingBorrowing() {}
    consuming func doSomethingConsuming() {}
    mutating func doSomethingMutating() {}
}

public struct TrivialStruct {
    var k: Int = 5

    func doSomethingDefault() {}
    consuming func doSomethingConsuming() {}
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
func consumeVal(_ x: consuming NonTrivialStruct) {}
func borrowValDefault(_ x: TrivialStruct) {}
func borrowValBorrowing(_ x: borrowing TrivialStruct) {}
func consumeVal(_ x: consuming TrivialStruct) {}
func borrowValDefault<T : P>(_ x: GenericNonTrivialStruct<T>) {}
func borrowValBorrowing<T : P>(_ x: borrowing GenericNonTrivialStruct<T>) {}
func consumeVal<T : P>(_ x: consuming GenericNonTrivialStruct<T>) {}

///////////////////////////////////////////
// MARK: Simple Loadable Borrowing Tests //
///////////////////////////////////////////

func testLoadableConsumingConsume(_ x: consuming NonTrivialStruct) {
    // expected-error @-1 {{'x' used after consume}}
    let _ = x // expected-note {{consumed here}}
    let _ = x.k // expected-note {{used here}}
}

func testLoadableConsumingConsume1a(_ x: consuming NonTrivialStruct) {
    let _ = x
    x = NonTrivialStruct()
    let _ = x.k
}

func testLoadableConsumingConsume2(_ x: consuming NonTrivialStruct) {
    // expected-error @-1 {{'x' consumed more than once}}
    var y = x // expected-note {{consumed here}}
    y = x // expected-note {{consumed again here}}
    _ = y
}

func testLoadableConsumingConsume3(_ x: consuming NonTrivialStruct) {
    let y = x
    _ = y
}

func testLoadableConsumingUse(_ x: consuming NonTrivialStruct) {
    _ = x
}

func testLoadableConsumingUseAndConsume(_ x: consuming NonTrivialStruct) {
    borrowValBorrowing(x)
    let _ = x
}

func testLoadableConsumingConsumeAndUse(_ x: consuming NonTrivialStruct) {
    // expected-error @-1 {{'x' used after consume}}
    let _ = x // expected-note {{consumed here}}
    borrowValBorrowing(x) // expected-note {{used here}}
}

func testLoadableConsumingConsumeAndUseReinit(_ x: consuming NonTrivialStruct) {
    let _ = x
    x = NonTrivialStruct()
    borrowValBorrowing(x)
}

func testLoadableConsumingCallBorrowValDefault(_ x: consuming NonTrivialStruct) {
    borrowValDefault(x)
}

func testLoadableConsumingCallBorrowValBorrowing(_ x: consuming NonTrivialStruct) {
    borrowValBorrowing(x)
}

func testLoadableConsumingCallConsumeVal(_ x: consuming NonTrivialStruct) {
    consumeVal(x)
}

func testLoadableConsumingCallConsumeValMultiple(_ x: consuming NonTrivialStruct) {
    // expected-error @-1 {{'x' consumed more than once}}
    consumeVal(x) // expected-note {{consumed here}}
    consumeVal(x) // expected-note {{consumed again here}}
}

func testLoadableConsumingCallMethodSelfDefault(_ x: consuming NonTrivialStruct) {
    x.doSomethingDefault()
}

func testLoadableConsumingCallMethodSelfConsuming(_ x: consuming NonTrivialStruct) {
    x.doSomethingConsuming()
}

func testLoadableConsumingCallMethodSelfMutating(_ x: consuming NonTrivialStruct) {
    x.doSomethingMutating()
}

func testLoadableConsumingEscapingClosure(_ x: consuming NonTrivialStruct) {
    var f: () -> () = {}
    f = {
        _ = x
    }
    _ = f
}

func testLoadableConsumingEscapingClosure2(_ x: consuming NonTrivialStruct) {
    _ = x // expected-error {{noncopyable 'x' cannot be consumed when captured by an escaping closure}}
    var f: () -> () = {}
    f = {
        _ = x
    }
    _ = f
}

func testLoadableConsumingEscapingClosure3(_ x: consuming NonTrivialStruct) {
    var f: () -> () = {}
    f = {
        _ = x
    }
    _ = f
    _ = x // expected-error {{noncopyable 'x' cannot be consumed when captured by an escaping closure}}
}

func testLoadableConsumingNonEscapingClosure(_ x: consuming NonTrivialStruct) {
    func useNonEscaping(_ f: () -> ()) {}
    useNonEscaping {
        _ = x
    }
}

func testLoadableConsumingNonEscapingClosure2(_ x: consuming NonTrivialStruct) {
    // expected-error @-1 {{'x' used after consume}}
    let _ = x // expected-note {{consumed here}}
    func useNonEscaping(_ f: () -> ()) {}
    useNonEscaping { // expected-note {{used here}}
        _ = x
    }
}

func testLoadableConsumingConsumeOperator(_ x: consuming NonTrivialStruct) {
    _ = consume x
}

func testLoadableConsumingConsumeOperator1(_ x: consuming NonTrivialStruct) {
    // expected-error @-1 {{'x' used after consume}}
    _ = consume x // expected-note {{consumed here}}
    borrowValDefault(x) // expected-note {{used here}}
}

func testLoadableConsumingConsumeOperator2(_ x: consuming NonTrivialStruct) {
    x = consume x
}

func testLoadableConsumingConsumeOperator3(_ x: consuming NonTrivialStruct) {
    _ = consume x
    let y = NonTrivialStruct()
    x = consume y
}

func testLoadableConsumingEnum(_ x: consuming LoadableEnum) {
    switch x {
    case let .x(y):
        _ = y
        break
    case .y:
        break
    }
}

func testLoadableConsumingEnum2(_ x: consuming LoadableEnum) {
    // expected-error @-1 {{'x' consumed more than once}}
    switch x { // expected-note {{consumed here}}
    case let .x(y):
        _ = consume x // expected-note {{consumed again here}}
        _ = y
        break
    case .y:
        break
    }
}

func testLoadableConsumingCopyOperator(_ x: consuming NonTrivialStruct) {
    _ = copy x
}

//////////////////////////////////////////
// MARK: Trivial Struct Consuming Tests //
//////////////////////////////////////////

func testTrivialConsumingConsume(_ x: consuming TrivialStruct) {
    // expected-error @-1 {{'x' used after consume}}
    let _ = x // expected-note {{consumed here}}
    let _ = x.k // expected-note {{used here}}
}

func testTrivialConsumingConsume2(_ x: consuming TrivialStruct) {
    // expected-error @-1 {{'x' consumed more than once}}
    var y = x // expected-note {{consumed here}}
    y = x // expected-note {{consumed again here}}
    _ = y
}

func testTrivialConsumingConsume3(_ x: consuming TrivialStruct) {
    let y = x
    _ = y
}

func testTrivialConsumingUse(_ x: consuming TrivialStruct) {
    _ = x
}

func testTrivialConsumingUseAndConsume(_ x: consuming TrivialStruct) {
    borrowValBorrowing(x)
    let _ = x
}

func testTrivialConsumingConsumeAndUse(_ x: consuming TrivialStruct) {
    // expected-error @-1 {{'x' used after consume}}
    let _ = x // expected-note {{consumed here}}
    borrowValBorrowing(x) // expected-note {{used here}}
}

func testTrivialConsumingConsumeAndUseReinit(_ x: consuming TrivialStruct) {
    let _ = x
    x = TrivialStruct()
    borrowValBorrowing(x)
}

func testTrivialConsumingCallBorrowValDefault(_ x: consuming TrivialStruct) {
    borrowValDefault(x)
}

func testTrivialConsumingCallBorrowValBorrowing(_ x: consuming TrivialStruct) {
    borrowValBorrowing(x)
}

func testTrivialConsumingCallConsumeVal(_ x: consuming TrivialStruct) {
    consumeVal(x)
}

func testTrivialConsumingCallMethodSelfDefault(_ x: consuming TrivialStruct) {
    x.doSomethingDefault()
}

func testTrivialConsumingCallMethodSelfConsuming(_ x: consuming TrivialStruct) {
    x.doSomethingConsuming()
}

func testTrivialConsumingEscapingClosure(_ x: consuming TrivialStruct) {
    var f: () -> () = {}
    f = {
        _ = x
    }
    _ = f
}

func testTrivialConsumingEscapingClosure2(_ x: consuming TrivialStruct) {
    let _ = x // expected-error {{noncopyable 'x' cannot be consumed when captured by an escaping closure}}
    var f: () -> () = {}
    f = {
        _ = x
    }
    _ = f
}

func testTrivialConsumingEscapingClosure3(_ x: consuming TrivialStruct) {
    var f: () -> () = {}
    f = {
        _ = x
    }
    _ = f
    let _ = x // expected-error {{noncopyable 'x' cannot be consumed when captured by an escaping closure}}
}

func testTrivialConsumingNonEscapingClosure(_ x: consuming TrivialStruct) {
    func useNonEscaping(_ f: () -> ()) {}
    useNonEscaping {
        _ = x
    }
}

func testTrivialConsumingNonEscapingClosure2(_ x: consuming TrivialStruct) {
    // expected-error @-1 {{'x' used after consume}}
    let _ = x // expected-note {{consumed here}}
    func useNonEscaping(_ f: () -> ()) {}
    useNonEscaping { // expected-note {{used here}}
        _ = x
    }
}

func testTrivialConsumingConsumeOperator(_ x: consuming TrivialStruct) {
    _ = consume x
}

func testTrivialConsumingEnum(_ x: consuming TrivialEnum) {
    switch x {
    case let .x(y):
        _ = y
        break
    case .y:
        break
    }
}

func testTrivialConsumingEnum2(_ x: consuming TrivialEnum) {
    // expected-error @-1 {{'x' used after consume}}
    switch x { // expected-note {{consumed here}}
    case let .x(y):
        _ = y
        break
    case .y:
        _ = copy x // expected-note {{used here}}
        break
    }
}


func testTrivialConsumingCopyOperator(_ x: consuming TrivialStruct) {
    _ = copy x
}

//////////////////////////////////////////////
// MARK: Simple AddressOnly Consuming Tests //
//////////////////////////////////////////////

func testAddressOnlyBorrowSimple<T>(_ x: borrowing T) {}

func testAddressOnlyConsumingConsume<T : P>(_ x: consuming GenericNonTrivialStruct<T>) {
    // expected-error @-1 {{'x' used after consume}}
    let _ = x // expected-note {{consumed here}}
    let _ = x.t // expected-note {{used here}}
}

func testAddressOnlyConsumingConsume2<T : P>(_ x: consuming GenericNonTrivialStruct<T>) {
    // expected-error @-1 {{'x' consumed more than once}}
    var y = x // expected-note {{consumed here}}
    y = x // expected-note {{consumed again here}}
    _ = y
}

func testAddressOnlyConsumingConsume3<T : P>(_ x: consuming GenericNonTrivialStruct<T>) {
    let y = x
    _ = y
}

func testAddressOnlyConsumingUse<T : P>(_ x: consuming GenericNonTrivialStruct<T>) {
    _ = x
}

func testAddressOnlyConsumingUseAndConsume<T : P>(_ x: consuming GenericNonTrivialStruct<T>) {
    borrowValDefault(x)
    let _ = x
}

func testAddressOnlyConsumingConsumeAndUse<T : P>(_ x: consuming GenericNonTrivialStruct<T>) {
    // expected-error @-1 {{'x' used after consume}}
    let _ = x // expected-note {{consumed here}}
    borrowValDefault(x) // expected-note {{used here}}
}

func testAddressOnlyConsumingConsumeAndUseReinit<T : P>(_ x: consuming GenericNonTrivialStruct<T>) {
    let _ = x
    x = GenericNonTrivialStruct<T>()
    borrowValDefault(x)
}

func testAddressOnlyConsumingCallBorrowValDefault<T : P>(_ x: consuming GenericNonTrivialStruct<T>) {
    borrowValDefault(x)
}

func testAddressOnlyConsumingCallBorrowValBorrowing<T : P>(_ x: consuming GenericNonTrivialStruct<T>) {
    borrowValBorrowing(x)
}

func testAddressOnlyConsumingCallConsumeVal<T : P>(_ x: consuming GenericNonTrivialStruct<T>) {
    consumeVal(x)
}

func testAddressOnlyConsumingCallMethodSelfDefault<T : P>(_ x: consuming GenericNonTrivialStruct<T>) {
    x.doSomethingDefault()
}

func testAddressOnlyConsumingCallMethodSelfConsuming<T : P>(_ x: consuming GenericNonTrivialStruct<T>) {
    x.doSomethingConsuming()
}

func testAddressOnlyConsumingEscapingClosure<T : P>(_ x: consuming GenericNonTrivialStruct<T>) {
    var f: () -> () = {}
    f = {
        _ = x
    }
    _ = f
}

func testAddressOnlyConsumingEscapingClosure2<T : P>(_ x: consuming GenericNonTrivialStruct<T>) {
    let _ = x // expected-error {{noncopyable 'x' cannot be consumed when captured by an escaping closure}}
    var f: () -> () = {}
    f = {
        _ = x
    }
    _ = f
}

func testAddressOnlyConsumingEscapingClosure3<T : P>(_ x: consuming GenericNonTrivialStruct<T>) {
    var f: () -> () = {}
    f = {
        _ = x
    }
    _ = f
    let _ = x // expected-error {{noncopyable 'x' cannot be consumed when captured by an escaping closure}}
}

func testAddressOnlyConsumingNonEscapingClosure<T : P>(_ x: consuming GenericNonTrivialStruct<T>) {
    func useNonEscaping(_ f: () -> ()) {}
    useNonEscaping {
        _ = x
    }
}

func testAddressOnlyConsumingNonEscapingClosure2<T : P>(_ x: consuming GenericNonTrivialStruct<T>) {
    // expected-error @-1 {{'x' used after consume}}
    let _ = x // expected-note {{consumed here}}
    func useNonEscaping(_ f: () -> ()) {}
    useNonEscaping { // expected-note {{used here}}
        _ = x
    }
}

func testAddressOnlyConsumingCast<T : P>(_ x: consuming GenericNonTrivialStruct<T>) {
    let _ = x as Any
}

func testAddressOnlyConsumingCast2<T : P>(_ x: consuming GenericNonTrivialStruct<T>) {
    // expected-error @-1 {{'x' consumed more than once}}
    let _ = x as Any // expected-note {{consumed here}}
    let _ = x as Any // expected-note {{consumed again here}}
}

func testAddressOnlyConsumingCastCheck<T : P>(_ x: consuming GenericNonTrivialStruct<T>) {
    if x is Any { // expected-warning {{'is' test is always true}}
    }
}

func testAddressOnlyConsumingCastCheck2<T : P>(_ x: consuming GenericNonTrivialStruct<T>) {
    // expected-error @-1 {{'x' consumed more than once}}
    if x is Any { // expected-note {{consumed here}}
        // expected-warning @-1 {{'is' test is always true}}

    }
    if x is Any { // expected-note {{consumed again here}}
        // expected-warning @-1 {{'is' test is always true}}
    }
}

func testAddressOnlyConsumingEnum<T>(_ x: consuming AddressOnlyEnum<T>) {
    switch x {
    case let .x(y):
        _ = y
        break
    case let .y(z):
        _ = z
        break
    }
}

func testAddressOnlyConsumingEnum2<T>(_ x: consuming AddressOnlyEnum<T>) {
    // expected-error @-1 {{'x' used after consume}}
    switch x { // expected-note {{consumed here}}
    case let .x(y):
        _ = y
        break
    case let .y(z):
        _ = z
        break
    }
    _ = copy x // expected-note {{used here}}
}

func testAddressOnlyConsumeOperator<T>(_ x: consuming GenericNonTrivialStruct<T>) {
    _ = consume x
}

func testAddressOnlyCopyOperator<T>(_ x: consuming GenericNonTrivialStruct<T>) {
    _ = copy x 
}

///////////////////////////////
// MARK: Loadable Self Tests //
///////////////////////////////

struct LoadableSelfTest {
    var k = Klass()

    consuming func consumeSelf() {}
    func doSomethingDefault() {}
    consuming func doSomethingConsuming() {}
    borrowing func doSomethingBorrowing() {}
    mutating func doSomethingMutating() {}

    consuming func testUseSelf() {
        _ = self
    }

    consuming func testUseSelf2() {
        self.doSomethingDefault()
        self.doSomethingDefault()
    }

    consuming func testUseSelf3() {
        self.doSomethingBorrowing()
        self.doSomethingDefault()
    }

    consuming func testUseSelf4() {
        self.doSomethingBorrowing()
        self.doSomethingBorrowing()
    }

    consuming func testLetUseSelf() {
        let _ = self
    }

    consuming func callDoSomethingDefault() {
        self.doSomethingDefault()
    }

    consuming func callDoSomethingBorrowing() {
        self.doSomethingBorrowing()
    }

    consuming func callDoSomethingConsuming() {
        self.doSomethingConsuming()
    }

    consuming func callDoSomethingMutating() {
        self.doSomethingMutating()
    }

    consuming func callDoSomethingMutating2() {
        self.doSomethingMutating()
        _ = consume self
    }

    consuming func callDoSomethingMutating3() {
        // expected-error @-1 {{'self' used after consume}}
        _ = consume self // expected-note {{consumed here}}
        self.doSomethingMutating() // expected-note {{used here}}
    }

    consuming func testConsumeOperatorSelf() {
        _ = consume self
    }

    consuming func testConsumeOperatorSelf2() {
        // expected-error @-1 {{'self' consumed more than once}}
        _ = consume self // expected-note {{consumed here}}
        _ = consume self // expected-note {{consumed again here}}
    }

    consuming func testCopyOperatorSelf() {
        _ = copy self
    }

    consuming func testUseField() {
        _ = self.k
    }

    consuming func testConsumeField() {
        // No error, since our field is copyable.
        let _ = self.k
    }

    consuming func testCallConsumeMethod() {
        consumeSelf()
    }

    consuming func testCallConsumeMethod2() {
        // expected-error @-1 {{'self' consumed more than once}}
        consumeSelf() // expected-note {{consumed here}}
        consumeSelf() // expected-note {{consumed again here}}
    }

    consuming func testCallFreeFunctionConsumeSelf() {
        consumeLoadableSelfTest(self)
    }

    consuming func testCallFreeFunctionConsumeSelf2() {
        // expected-error @-1 {{'self' consumed more than once}}
        consumeLoadableSelfTest(self) // expected-note {{consumed here}}
        consumeSelf() // expected-note {{consumed again here}}
    }

    consuming func testCallEscapingClosure() {
        var f: () -> () = {}
        f = {
            let _ = self
        }
        f()
    }

    consuming func testCallEscapingClosure2() {
        let _ = self // expected-error {{noncopyable 'self' cannot be consumed when captured by an escaping closure}}
        var f: () -> () = {}
        f = {
            let _ = self
        }
        f()
    }

    consuming func testCallEscapingClosure3() {
        var f: () -> () = {}
        f = {
            let _ = self
        }
        f()
        let _ = self // expected-error {{noncopyable 'self' cannot be consumed when captured by an escaping closure}}
    }

    consuming func testCallNonEscapingClosure() {
        func f(_ x: () -> ()) {}
        f {
            _ = self
        }
    }

    consuming func testCallNonEscapingClosure2() {
        // expected-error @-1 {{'self' used after consume}}
        let _ = self // expected-note {{consumed here}}
        func f(_ x: () -> ()) {}
        f { // expected-note {{used here}}
            _ = self
        }
    }

    consuming func testCallNonEscapingClosure3() {
        let _ = self
        self = LoadableSelfTest()
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
    consuming func doSomethingConsuming() {}
    mutating func doSomethingMutating() {}
    borrowing func doSomethingBorrowing() {}

    consuming func testUseSelf() {
        // expected-error @-1 {{'self' consumed more than once}}
        _ = self // expected-note {{consumed here}}
        _ = self // expected-note {{consumed again here}}
    }

    consuming func testUseSelf2() {
        self.doSomethingDefault()
        self.doSomethingDefault()
    }

    consuming func testUseSelf3() {
        self.doSomethingBorrowing()
        self.doSomethingDefault()
    }

    consuming func testUseSelf4() {
        self.doSomethingDefault()
        self.doSomethingBorrowing()
    }

    consuming func testLetUseSelf() {
        // expected-error @-1 {{'self' consumed more than once}}
        let _ = self // expected-note {{consumed here}}
        let _ = self // expected-note {{consumed again here}}
    }

    consuming func callDoSomethingDefault() {
        self.doSomethingDefault()
        self.doSomethingBorrowing()
    }

    consuming func callDoSomethingConsuming() {
        self.doSomethingConsuming()
    }

    consuming func callDoSomethingConsuming2() {
        // expected-error @-1 {{'self' used after consume}}
        self.doSomethingConsuming() // expected-note {{consumed here}}
        self.doSomethingDefault() // expected-note {{used here}}
    }

    consuming func callDoSomethingConsuming3() {
        // expected-error @-1 {{'self' used after consume}}
        self.doSomethingConsuming() // expected-note {{consumed here}}
        self.doSomethingDefault() // expected-note {{used here}}
    }

    consuming func callDoSomethingConsuming4() {
        // expected-error @-1 {{'self' used after consume}}
        self.doSomethingConsuming() // expected-note {{consumed here}}
        self.doSomethingMutating() // expected-note {{used here}}
    }

    consuming func callDoSomethingConsuming4(_ x: AddressOnlySelfTest<T>) {
        self.doSomethingConsuming()
        self = x
        self.doSomethingDefault()
    }

    consuming func testConsumeOperatorSelf() {
        _ = consume self
    }

    consuming func testCopyOperatorSelf() {
        _ = copy self
    }

    consuming func testUseField() {
        _ = self.t
    }

    consuming func testConsumeField() {
        // No error, since our field is copyable.
        let _ = self.t
    }

    consuming func testCallConsumeMethod() {
        consumeSelf()
    }

    consuming func testCallFreeFunctionConsumeSelf() {
        consumeAddressOnlySelfTest(self)
    }

    consuming func testCallFreeFunctionConsumeSelf2() {
        // expected-error @-1 {{'self' consumed more than once}}
        consumeAddressOnlySelfTest(self) // expected-note {{consumed here}}
        consumeSelf() // expected-note {{consumed again here}}
    }

    consuming func testCallEscapingClosure() {
        var f: () -> () = {}
        f = {
            let _ = self
        }
        f()
    }

    consuming func testCallEscapingClosure2() {
        let _ = self // expected-error {{noncopyable 'self' cannot be consumed when captured by an escaping closure}}
        var f: () -> () = {}
        f = {
            let _ = self
        }
        f()
    }

    consuming func testCallEscapingClosure3() {
        var f: () -> () = {}
        f = {
            let _ = self
        }
        f()
        let _ = self // expected-error {{noncopyable 'self' cannot be consumed when captured by an escaping closure}}
    }

    consuming func testCallEscapingClosure4() {
        var f: () -> () = {}
        f = {
            let _ = self
        }
        f()
        f = {
            let _ = self
        }
        let _ = self // expected-error {{noncopyable 'self' cannot be consumed when captured by an escaping closure}}
    }

    consuming func testCallNonEscapingClosure() {
        func f(_ x: () -> ()) {}
        f {
            _ = self
        }
    }

    consuming func testCallNonEscapingClosure2() {
        // expected-error @-1 {{'self' used after consume}}
        let _ = self // expected-note {{consumed here}}
        func f(_ x: () -> ()) {}
        f { // expected-note {{used here}}
            _ = self
        }
        f {
            _ = self
        }
    }
}

func consumeAddressOnlySelfTest<T>(_ x: consuming AddressOnlySelfTest<T>) {}

//////////////////////////////
// MARK: Trivial Self Tests //
//////////////////////////////

struct TrivialSelfTest {
    var t = 5

    consuming func consumeSelf() {}
    func doSomethingDefault() {}
    borrowing func doSomethingBorrowing() {}
    consuming func doSomethingConsuming() {}
    mutating func doSomethingMutating() {}

    consuming func testUseSelf() {
        _ = self
    }

    consuming func testUseSelf2() {
        // expected-error @-1 {{'self' consumed more than once}}
        _ = self // expected-note {{consumed here}}
        _ = self // expected-note {{consumed again here}}
    }

    consuming func testUseSelf3() {
        // expected-error @-1 {{'self' consumed more than once}}
        //
        // TODO: This is why we need to change SILFunctionType to contain
        // information about default conventions.
        self.doSomethingDefault() // expected-note {{consumed here}}
        self.doSomethingDefault() // expected-note {{consumed again here}}
    }

    consuming func testUseSelf4() {
        // expected-error @-1 {{'self' used after consume}}
        self.doSomethingDefault() // expected-note {{consumed here}}
        self.doSomethingBorrowing() // expected-note {{used here}}
    }

    consuming func testUseSelf5() {
        self.doSomethingBorrowing()
        self.doSomethingBorrowing()
    }

    consuming func testLetUseSelf() {
        let _ = self
    }

    consuming func callDoSomethingDefault() {
        self.doSomethingDefault()
    }

    consuming func callDoSomethingConsuming() {
        self.doSomethingConsuming()
    }

    consuming func testConsumeOperatorSelf() {
        _ = consume self
    }

    consuming func testConsumeOperatorSelf2() {
        // expected-error @-1 {{'self' consumed more than once}}
        _ = consume self // expected-note {{consumed here}}
        _ = consume self // expected-note {{consumed again here}}
    }

    consuming func testCopyOperatorSelf() {
        _ = copy self
    }

    consuming func testUseField() {
        _ = self.t
    }

    consuming func testConsumeField() {
        // No error, since our field is copyable.
        let _ = self.t
    }

    consuming func testCallConsumeMethod() {
        consumeSelf()
    }

    consuming func testCallFreeFunctionConsumeSelf() {
        consumeTrivialSelfTest(self)
    }

    consuming func testCallFreeFunctionConsumeSelf2() {
        // expected-error @-1 {{'self' consumed more than once}}
        consumeTrivialSelfTest(self) // expected-note {{consumed here}}
        consumeSelf() // expected-note {{consumed again here}}
    }

    consuming func testCallEscapingClosure() {
        var f: () -> () = {}
        f = {
            let _ = self
        }
        f()
    }

    consuming func testCallEscapingClosure2() {
        let _ = self // expected-error {{noncopyable 'self' cannot be consumed when captured by an escaping closure}}
        var f: () -> () = {}
        f = {
            let _ = self
        }
        f()
    }

    consuming func testCallNonEscapingClosure() {
        func f(_ x: () -> ()) {}
        f {
            _ = self
        }
    }

    consuming func testCallNonEscapingClosure2() {
        // expected-error @-1 {{'self' used after consume}}
        let _ = self // expected-note {{consumed here}}
        func f(_ x: () -> ()) {}
        f { // expected-note {{used here}}
            _ = self
        }
    }

    consuming func testCallNonEscapingClosure3() {
        let _ = self
        self = TrivialSelfTest()
        func f(_ x: () -> ()) {}
        f {
            _ = self
        }
    }
}

func consumeTrivialSelfTest(_ x: consuming TrivialSelfTest) {}
