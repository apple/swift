// RUN: %target-swift-frontend -verify %s -parse-stdlib -emit-sil -o /dev/null

import Swift

//////////////////
// Declarations //
//////////////////

public class Klass {
    var k: Klass? = nil

    func getOtherKlass() -> Klass? { nil }
}
public class SubKlass1 : Klass {}
public class SubKlass2 : Klass {}

struct KlassWrapper {
    var k: Klass
}

func consumingUse(_ k: __owned Klass) {}
var booleanValue: Bool { false }
var booleanValue2: Bool { false }
func nonConsumingUse(_ k: Klass) {}
func exchangeUse(_ k: Klass) -> Klass { k }

///////////
// Tests //
///////////

public func performMoveOnVarSingleBlock(_ p: Klass) {
    var x = p
    let _ = consume x
    x = p
    nonConsumingUse(x)
}

public func performMoveOnVarSingleBlockError(_ p: Klass) {
    var x = p // expected-error {{'x' used after consume}}
    let _ = consume x // expected-note {{consumed here}}
    nonConsumingUse(x) // expected-note {{used here}}
    x = p
    nonConsumingUse(x)
}

public func performMoveOnVarMultiBlock(_ p: Klass) {
    var x = p
    let _ = consume x

    while booleanValue {
        print("true")
    }

    while booleanValue {
        print("true")
    }

    x = p
    nonConsumingUse(x)
}

public func performMoveOnVarMultiBlockError1(_ p: Klass) {
    var x = p // expected-error {{'x' used after consume}}
    let _ = consume x // expected-note {{consumed here}}

    nonConsumingUse(x) // expected-note {{used here}}

    while booleanValue {
        print("true")
    }

    // We only emit an error on the first one.
    nonConsumingUse(x)

    while booleanValue {
        print("true")
    }

    // We only emit an error on the first one.
    nonConsumingUse(x)

    x = p
    nonConsumingUse(x)
}

public func performMoveOnVarMultiBlockError2(_ p: Klass) {
    var x = p // expected-error {{'x' used after consume}}
    let _ = consume x // expected-note {{consumed here}}

    while booleanValue {
        print("true")
    }

    nonConsumingUse(x) // expected-note {{used here}}

    while booleanValue {
        print("true")
    }

    // We only error on the first one.
    nonConsumingUse(x)

    x = p
    nonConsumingUse(x)
}

public func performMoveConditionalReinitialization(_ p: Klass) {
    var x = p

    if booleanValue {
        nonConsumingUse(x)
        let _ = consume x
        x = p
        nonConsumingUse(x)
    } else {
        nonConsumingUse(x)
    }

    nonConsumingUse(x)
}

public func performMoveConditionalReinitialization2(_ p: Klass) {
    var x = p // expected-error {{'x' used after consume}}

    if booleanValue {
        nonConsumingUse(x)
        let _ = consume x // expected-note {{consumed here}}
        nonConsumingUse(x) // expected-note {{used here}}
        x = p
        nonConsumingUse(x)
    } else {
        nonConsumingUse(x)
    }

    nonConsumingUse(x)
}

public func performMoveConditionalReinitialization3(_ p: Klass, _ p2: Klass, _ p3: Klass) {
    var x = p // expected-error {{'x' used after consume}}
              // expected-error @-1 {{'x' used after consume}}

    if booleanValue {
        nonConsumingUse(x)
        let _ = consume x   // expected-note {{consumed here}}
        nonConsumingUse(x) // expected-note {{used here}}
        nonConsumingUse(x) // We only emit for the first one.
        x = p2
        nonConsumingUse(x)
        let _ = consume x   // expected-note {{consumed here}}
        nonConsumingUse(x) // expected-note {{used here}}
    } else {
        nonConsumingUse(x)
    }

    nonConsumingUse(x)
}

// Even though the examples below are for lets, since the let is not initially
// defined it comes out like a var.
public func performMoveOnLaterDefinedInit(_ p: Klass) {
    let x: Klass // expected-error {{'x' used after consume}}
    do {
        x = p
    }
    let _ = consume x // expected-note {{consumed here}}
    nonConsumingUse(x) // expected-note {{used here}}
}

public func performMoveOnLaterDefinedInit2(_ p: Klass) {
    let x: Klass
    do {
        x = p
    }
    nonConsumingUse(x)
    let _ = consume x
}

public func performMoveOnInOut(_ p: inout Klass) { // expected-error {{'p' used after consume}}
    let buf = consume p // expected-note {{consumed here}}
    let _ = buf
} // expected-note {{used here}}

public func performMoveOnInOut2(_ p: inout Klass, _ p2: Klass) {
    let buf = consume p
    p = p2
    let _ = buf
}

// Mutating self is an inout argument.
struct S {
    var buffer: Klass?

    mutating func appendNoError() {
        let b = (consume self).buffer!
        let maybeNewB = exchangeUse(b)
        self = .init(buffer: maybeNewB)
    }

    mutating func appendError() { // expected-error {{'self' used after consume}}
        let b = (consume self).buffer // expected-note {{consumed here}}
        let _ = b
    } // expected-note {{used here}}

    mutating func appendThrowingNoError1(_ f: () throws -> ()) throws {
        let b = (consume self).buffer!
        let maybeNewB = exchangeUse(b)
        // We have to initialize self before we call try since otherwise we will
        // not initialize self along the throws path.
        self = .init(buffer: maybeNewB)
        try f()
    }

    mutating func appendThrowingNoError2(_ f: () throws -> ()) {
        do {
            let b = (consume self).buffer!
            try f()
            let maybeNewB = exchangeUse(b)
            self = .init(buffer: maybeNewB)
        } catch {
            self = .init(buffer: nil)
        }
    }

    // In this case, since we initialize self before the try point, we will have
    // re-initialized self before hitting either the code after the try that is
    // inline or the catch block.
    mutating func appendThrowingNoError3(_ f: () throws -> ()) {
        do {
            let b = (consume self).buffer!
            let maybeNewB = exchangeUse(b)
            self = .init(buffer: maybeNewB)
            try f()
        } catch {
        }
    }

    mutating func appendThrowingError0(_ f: () throws -> ()) throws { // expected-error {{'self' used after consume}}
        let b = (consume self).buffer! // expected-note {{consumed here}}
        let maybeNewB = exchangeUse(b)
        try f() // expected-note {{used here}}
        self = .init(buffer: maybeNewB)
    }


    mutating func appendThrowingError1(_ f: () throws -> ()) throws { // expected-error {{'self' used after consume}}
        let b = (consume self).buffer! // expected-note {{consumed here}}
        let maybeNewB = exchangeUse(b)
        let _ = maybeNewB
        try f() // expected-note {{used here}}
    }

    mutating func appendThrowingError2(_ f: () throws -> ()) { // expected-error {{'self' used after consume}}
        do {
            let b = (consume self).buffer // expected-note {{consumed here}}
            let _ = b
            try f()
        } catch {
            self = .init(buffer: nil)
        }
    } // expected-note {{used here}}

    mutating func appendThrowingError3(_ f: () throws -> ()) { // expected-error {{'self' used after consume}}
        do {
            let b = (consume self).buffer! // expected-note {{consumed here}}
            try f()
            let maybeNewB = exchangeUse(b)
            self = .init(buffer: maybeNewB)
        } catch {
        }
    } // expected-note {{used here}}

    mutating func appendThrowingError4(_ f: () throws -> ()) { // expected-error {{'self' used after consume}}
        do {
            let b = (consume self).buffer // expected-note {{consumed here}}
            let _ = b
            try f()
        } catch {
        }
    } // expected-note {{used here}}
}

/////////////////
// Defer Tests //
/////////////////

extension KlassWrapper {
    mutating func deferTestSuccess1() {
        let _ = (consume self)
        defer {
            self = KlassWrapper(k: Klass())
        }
        print("123")
    }

    // Make sure we can init/reinit self multiple times without error.
    mutating func deferTestSuccess2() {
        let _ = (consume self)
        self = KlassWrapper(k: Klass())
        let _ = (consume self)
        defer {
            self = KlassWrapper(k: Klass())
        }
        print("123")
    }

    mutating func deferTestSuccess3() {
        let _ = (consume self)
        defer {
            self = KlassWrapper(k: Klass())
        }
        defer {
            self = KlassWrapper(k: Klass())
        }
        print("123")
    }

    // We do not support moving within a defer right now.
    mutating func deferTestFail1() {
        let _ = (consume self)
        defer {
            self = KlassWrapper(k: Klass())
            let _ = (consume self) // expected-error {{'consume' applied to value that the compiler does not support}}
        }
        print("123")
    }

    // We do not support moving within a defer right now.
    mutating func deferTestFail2() { // expected-error {{'self' used after consume}}
        let _ = (consume self) // expected-note {{consumed here}}
        defer {
            nonConsumingUse(k) // expected-note {{used here}}
            self = KlassWrapper(k: Klass())
        }
        print("123")
    }


    mutating func deferTestFail3() { // expected-error {{'self' used after consume}}
        let _ = (consume self) // expected-note {{consumed here}}
        nonConsumingUse(k) // expected-note {{used here}}
        defer {
            nonConsumingUse(k)
            self = KlassWrapper(k: Klass())
        }
        print("123")
    }

    mutating func deferTestFail4() { // expected-error {{'self' used after consume}}
        let _ = (consume self) // expected-note {{consumed here}}
        defer {
            consumingUse(k) // expected-note {{used here}}
            self = KlassWrapper(k: Klass())
        }
        print("123")
    }

    // TODO: We should definitely be erroring on consuming use I think.
    mutating func deferTestFail5() { // expected-error {{'self' used after consume}}
        let _ = (consume self) // expected-note {{consumed here}}
        for _ in 0..<1024 {
            defer {
                consumingUse(k)
                self = KlassWrapper(k: Klass())
            }
            print("foo bar")
        }
        print("123")
    }  // expected-note {{used here}}

    // TODO: We should be erroring on nonConsumingUse rather than the end of
    // scope use.
    //
    mutating func deferTestFail6() { // expected-error {{'self' used after consume}}
        let _ = (consume self) // expected-note {{consumed here}}
        for _ in 0..<1024 {
            defer {
                nonConsumingUse(k)
                self = KlassWrapper(k: Klass())
            }
            print("foo bar")
        }
        print("123")
    }  // expected-note {{used here}}

    mutating func deferTestFail7() { // expected-error {{'self' used after consume}}
        for _ in 0..<1024 {
            let _ = (consume self) // expected-note {{consumed here}}
            defer {
                nonConsumingUse(k) // expected-note {{used here}}
                self = KlassWrapper(k: Klass())
            }
            print("foo bar")
        }
        print("123")
    }

    mutating func deferTestFail8() { // expected-error {{'self' used after consume}}
        let _ = (consume self) // expected-note {{consumed here}}
        defer {
            if booleanValue {
                nonConsumingUse(k) // expected-note {{used here}}
            }
            self = KlassWrapper(k: Klass())
        }
        print("foo bar")
    }

    mutating func deferTestFail9() { // expected-error {{'self' used after consume}}
        let _ = (consume self) // expected-note {{consumed here}}
        defer {
            if booleanValue {
                nonConsumingUse(k) // expected-note {{used here}}
            } else {
                nonConsumingUse(k)
            }
            self = KlassWrapper(k: Klass())
        }
        print("foo bar")
    }

    mutating func deferTestFail10() { // expected-error {{'self' used after consume}}
        let _ = (consume self) // expected-note {{consumed here}}
        defer {
            for _ in 0..<1024 {
                nonConsumingUse(k) // expected-note {{used here}}
            }
            self = KlassWrapper(k: Klass())
        }
        print("foo bar")
    }

    mutating func deferTestFail11() { // expected-error {{'self' used after consume}}
        let _ = (consume self) // expected-note {{consumed here}}
        if booleanValue {
            print("creating blocks")
        } else {
            print("creating blocks2")
        }
        defer {
            for _ in 0..<1024 {
                nonConsumingUse(k) // expected-note {{used here}}
            }
            self = KlassWrapper(k: Klass())
        }
        print("foo bar")
    }

    mutating func deferTestFail12() { // expected-error {{'self' used after consume}}
        if booleanValue {
            print("creating blocks")
        } else {
            let _ = (consume self) // expected-note {{consumed here}}
            print("creating blocks2")
        }

        defer {
            for _ in 0..<1024 {
                nonConsumingUse(k) // expected-note {{used here}}
            }
            self = KlassWrapper(k: Klass())
        }
        print("foo bar")
    }

    mutating func deferTestSuccess13() {
        if booleanValue {
            print("creating blocks")
        } else {
            let _ = (consume self)
            print("creating blocks2")
        }

        defer {
            self = KlassWrapper(k: Klass())
        }
        print("foo bar")
    }

    mutating func deferTestSuccess14() {
        if booleanValue {
            print("creating blocks")
            self.doSomething()
        } else {
            let _ = (consume self)
            print("creating blocks2")
        }

        defer {
            self = KlassWrapper(k: Klass())
        }
        print("foo bar")
    }
}

////////////////
// Cast Tests //
////////////////

public func castTest0(_ x: __owned SubKlass1) -> Klass {
    var x2 = x  // expected-error {{'x2' used after consume}}
    x2 = x
    let _ = consume x2 // expected-note {{consumed here}}
    return x2 as Klass // expected-note {{used here}}
}

public func castTest1(_ x: __owned Klass) -> SubKlass1 {
    var x2 = x  // expected-error {{'x2' used after consume}}
    x2 = x
    let _ = consume x2 // expected-note {{consumed here}}
    return x2 as! SubKlass1 // expected-note {{used here}}
}

public func castTest2(_ x: __owned Klass) -> SubKlass1? {
    var x2 = x // expected-error {{'x2' used after consume}}
    x2 = x
    let _ = consume x2 // expected-note {{consumed here}}
    return x2 as? SubKlass1 // expected-note {{used here}}
}

public func castTestSwitch1(_ x : __owned Klass) {
    var x2 = x // expected-error {{'x2' used after consume}}
    x2 = x
    let _ = consume x2 // expected-note {{consumed here}}
    switch x2 {  // expected-note {{used here}}
    case let k as SubKlass1:
        print(k)
    default:
        print("Nope")
    }
}

public func castTestSwitch2(_ x : __owned Klass) {
    var x2 = x // expected-error {{'x2' used after consume}}
    x2 = x
    let _ = consume x2 // expected-note {{consumed here}}
    switch x2 { // expected-note {{used here}}
    case let k as SubKlass1:
        print(k)
    case let k as SubKlass2:
        print(k)
    default:
        print("Nope")
    }
}

public func castTestSwitchInLoop(_ x : __owned Klass) {
    var x2 = x // expected-error {{'x2' used after consume}}
    x2 = x
    let _ = consume x2 // expected-note {{consumed here}}

    for _ in 0..<1024 {
        switch x2 { // expected-note {{used here}}
        case let k as SubKlass1:
            print(k)
        default:
            print("Nope")
        }
    }
}

public func castTestIfLet(_ x : __owned Klass) {
    var x2 = x // expected-error {{'x2' used after consume}}
    x2 = x
    let _ = consume x2 // expected-note {{consumed here}}
    if case let k as SubKlass1 = x2 { // expected-note {{used here}}
        print(k)
    } else {
        print("no")
    }
}

public func castTestIfLetInLoop(_ x : __owned Klass) {
    var x2 = x // expected-error {{'x2' used after consume}}
    x2 = x
    let _ = consume x2 // expected-note {{consumed here}}
    for _ in 0..<1024 {
        if case let k as SubKlass1 = x2 { // expected-note {{used here}}
            print(k)
        } else {
            print("no")
        }
    }
}

public enum EnumWithKlass {
    case none
    case klass(Klass)
}

public func castTestIfLet2(_ x : __owned EnumWithKlass) {
    var x2 = x // expected-error {{'x2' used after consume}}
    x2 = x
    let _ = consume x2 // expected-note {{consumed here}}
    if case let .klass(k as SubKlass1) = x2 { // expected-note {{used here}}
        print(k)
    } else {
        print("no")
    }
}

///////////////
// GEP Tests //
///////////////

public func castAccess(_ x : __owned Klass) {
    var x2 = x // expected-error {{'x2' used after consume}}
    x2 = x
    let _ = consume x2 // expected-note {{consumed here}}
    let _ = x2.k // expected-note {{used here}}
}

public func castAccess2(_ x : __owned Klass) {
    var x2 = x // expected-error {{'x2' used after consume}}
    x2 = x
    let _ = consume x2 // expected-note {{consumed here}}
    let _ = x2.k!.getOtherKlass() // expected-note {{used here}}
}

/////////////////////////
// Partial Apply Tests //
/////////////////////////

// Emit a better error here. At least we properly error.
public func partialApplyTest(_ x: __owned Klass) {
    var x2 = x // expected-error {{'x2' used after consume}}
    x2 = x
    let _ = consume x2 // expected-note {{consumed here}}
    let f = { // expected-note {{used here}}
        print(x2)
    }
    f()
}

////////////////////////
// Misc Tests on Self //
////////////////////////

extension KlassWrapper {

    func doSomething() { print("foo") }

    // This test makes sure that we are able to properly put in the destroy_addr
    // in the "creating blocks" branch. There used to be a bug where the impl
    // would need at least one destroy_addr to properly infer the value to put
    // into blocks not reachable from the consume but that are on the dominance
    // frontier from the _move. This was unnecessary and the test makes sure we
    // do not fail on this again.
    mutating func noDestroyAddrBeforeOptInsertAfter() {
        if booleanValue {
            print("creating blocks")
        } else {
            let _ = (consume self)
            print("creating blocks2")
        }

        self = .init(k: Klass())
        print("foo bar")
    }

    // A derived version of noDestroyAddrBeforeOptInsertAfter that makes sure
    // when we insert the destroy_addr, we destroy self at the end of the block.
    mutating func noDestroyAddrBeforeOptInsertAfter2() {
        if booleanValue {
            print("creating blocks")
            self.doSomething()
        } else {
            let _ = (consume self)
            print("creating blocks2")
        }

        self = .init(k: Klass())
        print("foo bar")
    }
}

//////////////////////////////////
// Multiple Captures from Defer //
//////////////////////////////////

func multipleCapture1(_ k: Klass) -> () {
    var k2 = k
    var k3 = k
    let _ = consume k2
    let _ = consume k3
    var k4 = k
    k4 = k
    defer {
        k2 = Klass()
        print(k4)
        k3 = Klass()
    }
    print("foo bar")
}

func multipleCapture2(_ k: Klass) -> () {
    var k2 = k // expected-error {{'k2' used after consume}}
    k2 = k
    var k3 = k
    let _ = consume k2 // expected-note {{consumed here}}
    let _ = consume k3
    var k4 = k
    k4 = k
    defer {
        print(k2) // expected-note {{used here}}
        print(k4)
        k3 = Klass()
    }
    print("foo bar")
}

//////////////////////
// Reinit in pieces //
//////////////////////

// These tests exercise the diagnostic to see how we error if we re-initialize a
// var in pieces. Eventually we should teach either this diagnostic pass how to
// handle this or teach DI how to combine the initializations into one large
// reinit.
struct KlassPair {
    var lhs: Klass
    var rhs: Klass
}

func reinitInPieces1(_ k: KlassPair) {
    var k2 = k
    k2 = k

    let _ = consume k2 // expected-error {{'consume' applied to value that the compiler does not support}}
    k2.lhs = Klass()
    k2.rhs = Klass()
}

////////////////////////
// InOut and Use Test //
////////////////////////

func useValueAndInOut(_ x: Klass, _ y: inout Klass) {}
func useValueAndInOut(_ x: inout Klass, _ y: Klass) {}

func inoutAndUseTest(_ x: Klass) {
    var y = x // expected-error {{'y' used after consume}}
              // expected-error @-1 {{'y' used after consume}}
    useValueAndInOut(consume y, &y) // expected-note {{used here}}
                                  // expected-note @-1 {{consumed here}}
    useValueAndInOut(&y, consume y) // expected-note {{used here}}
                                  // expected-note @-1 {{consumed here}}
}
