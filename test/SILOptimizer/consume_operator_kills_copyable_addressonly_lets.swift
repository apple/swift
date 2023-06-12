// RUN: %target-swift-frontend -verify %s -parse-stdlib -emit-sil -o /dev/null


import Swift

//////////////////
// Declarations //
//////////////////

public class Klass {}

public protocol P {}
public protocol SubP1 : P {}
public protocol SubP2 : P {}

func consumingUse<T>(_ k: __owned T) {}
var booleanValue: Bool { false }
func nonConsumingUse<T>(_ k: T) {}

///////////
// Tests //
///////////

//===---
// Let + Non Consuming Use
//

public func simpleLinearUse<T>(_ x: T) {
    let y = x // expected-error {{'y' used after consume}}
    let _ = consume y // expected-note {{consumed here}}
    nonConsumingUse(y) // expected-note {{used here}}
}

// We just emit an error today for the first error in a block.
public func simpleLinearUse2<T>(_ x: T) {
    let y = x // expected-error {{'y' used after consume}}
    let _ = consume y // expected-note {{consumed here}}
    nonConsumingUse(y) // expected-note {{used here}}
    nonConsumingUse(y)
}

public func conditionalUse1<T>(_ x: T) {
    let y = x
    if booleanValue {
        let _ = consume y
    } else {
        nonConsumingUse(y)
    }
}

public func loopUse1<T>(_ x: T) {
    let y = x  // expected-error {{'y' used after consume}}
    let _ = consume y // expected-note {{consumed here}}
    for _ in 0..<1024 {
        nonConsumingUse(y) // expected-note {{used here}}
    }
}

//===---
// Let + Consuming Use
//

public func simpleLinearConsumingUse<T>(_ x: T) {
    let y = x // expected-error {{'y' used after consume}}
    let _ = consume y // expected-note {{consumed here}}
    consumingUse(y) // expected-note {{used here}}
}

public func conditionalUseOk1<T>(_ x: T) {
    let y = x
    if booleanValue {
        let _ = consume y
    } else {
        consumingUse(y)
    }
}

// This test makes sure that in the case where we have two consuming uses, with
// different first level copies, we emit a single diagnostic.
public func conditionalBadConsumingUse<T>(_ x: T) {
    let y = x // expected-error {{'y' used after consume}}
    if booleanValue {
        let _ = consume y // expected-note {{consumed here}}
        consumingUse(y) // expected-note {{used here}}
    } else {
        // We shouldn't get any diagnostic on this use.
        consumingUse(y)
    }

    // But this one and the first consumingUse should get a diagnostic. But
    // since this is a later error, we require the user to recompile for now.
    consumingUse(y)
}

public func conditionalBadConsumingUse2<T>(_ x: T) {
    let y = x // expected-error {{'y' used after consume}}
    if booleanValue {
        let _ = consume y // expected-note {{consumed here}}
    } else {
        // We shouldn't get any diagnostic on this use.
        consumingUse(y)
    }

    consumingUse(y) // expected-note {{used here}}
}


// This test makes sure that in the case where we have two consuming uses, with
// different first level copies, we emit a single diagnostic.
public func conditionalBadConsumingUseLoop<T>(_ x: T) {
    let y = x // expected-error {{'y' used after consume}}
    if booleanValue {
        let _ = consume y // expected-note {{consumed here}}
        consumingUse(y) // expected-note {{used here}}
    } else {
        // We shouldn't get any diagnostic on this use.
        consumingUse(y)
    }

    // But this one and the first consumingUse should get a diagnostic.
    //
    // We do not actually emit the diagnostic here since we emit only one
    // diagnostic per consume at a time.
    for _ in 0..<1024 {
        consumingUse(y)
    }
}

// This test makes sure that in the case where we have two consuming uses, with
// different first level copies, we emit a single diagnostic.
public func conditionalBadConsumingUseLoop2<T>(_ x: T) {
    let y = x // expected-error {{'y' used after consume}}
    if booleanValue {
        let _ = consume y // expected-note {{consumed here}}
    } else {
        // We shouldn't get any diagnostic on this use.
        consumingUse(y)
    }

    for _ in 0..<1024 {
        consumingUse(y) // expected-note {{used here}}
    }
}

//===
// Parameters

// This is ok, no uses after.
public func simpleMoveOfParameter<T>(_ x: T) -> () {
    let _ = consume x // expected-error {{'consume' applied to value that the compiler does not support}}
}

public func simpleMoveOfOwnedParameter<T>(_ x: __owned T) -> () {
    let _ = consume x
}

public func errorSimpleMoveOfParameter<T>(_ x: __owned T) -> () { // expected-error {{'x' used after consume}}
    let _ = consume x // expected-note {{consumed here}}
    let _ = consume x // expected-note {{used here}}
}

public func errorSimple2MoveOfParameter<T>(_ x: __owned T) -> () { // expected-error {{'x' used after consume}}
    let _ = consume x // expected-note {{consumed here}}
    let _ = consumingUse(x) // expected-note {{used here}}
}

// TODO: I wonder if we could do better for the 2nd error. At least we tell the
// user it is due to the loop.
public func errorLoopMultipleMove<T>(_ x: __owned T) -> () { // expected-error {{'x' used after consume}}
                                                      // expected-error @-1 {{'x' used after consume}}
    let _ = consume x // expected-note {{consumed here}}
    for _ in 0..<1024 {
        let _ = consume x // expected-note {{consumed here}}
                         // expected-note @-1 {{used here}}
                         // expected-note @-2 {{used here}}
    }
}

public func errorLoopMoveOfParameter<T>(_ x: __owned T) -> () { // expected-error {{'x' used after consume}}
    let _ = consume x // expected-note {{consumed here}}
    for _ in 0..<1024 {
        consumingUse(x) // expected-note {{used here}}
    }
}

public func errorLoop2MoveOfParameter<T>(_ x: __owned T) -> () { // expected-error {{'x' used after consume}}
    let _ = consume x // expected-note {{consumed here}}
    for _ in 0..<1024 {
        nonConsumingUse(x) // expected-note {{used here}}
    }
}

public func errorSimple2MoveOfParameterNonConsume<T>(_ x: __owned T) -> () { // expected-error {{'x' used after consume}}
    let _ = consume x // expected-note {{consumed here}}
    let _ = nonConsumingUse(x) // expected-note {{used here}}
}

public func errorLoopMoveOfParameterNonConsume<T>(_ x: __owned T) -> () { // expected-error {{'x' used after consume}}
    let _ = consume x // expected-note {{consumed here}}
    for _ in 0..<1024 {
        nonConsumingUse(x) // expected-note {{used here}}
    }
}

////////////////////////
// Pattern Match Lets //
////////////////////////

public func patternMatchIfCaseLet<T>(_ x: T?) {
    if case let .some(y) = x { // expected-error {{'y' used after consume}}
        let _ = consume y // expected-note {{consumed here}}
        nonConsumingUse(y) // expected-note {{used here}}
    }
}

public func patternMatchSwitchLet<T>(_ x: T?) {
    switch x {
    case .none:
        break
    case .some(let y): // expected-error {{'y' used after consume}}
        let _ = consume y // expected-note {{consumed here}}
        nonConsumingUse(y) // expected-note {{used here}}
    }
}

public func patternMatchSwitchLet2<T>(_ x: (T?, T?)?) {
    switch x {
    case .some((.some(let y), _)): // expected-error {{'y' used after consume}}
        let _ = consume y // expected-note {{consumed here}}
        nonConsumingUse(y) // expected-note {{used here}}
    default:
        break
    }
}

public func patternMatchSwitchLet3<T>(_ x: __owned (T?, T?)?) { // expected-error {{'x' used after consume}}
    let _ = consume x // expected-note {{consumed here}}
    switch x { // expected-note {{used here}}
    case .some((.some(_), .some(let z))): // expected-error {{'z' used after consume}}
        let _ = consume z // expected-note {{consumed here}}
        nonConsumingUse(z) // expected-note {{used here}}
    default:
        break
    }
}

////////////////
// Aggregates //
////////////////

public struct Pair<T> {
    var x: T
    var y: T
    var z: Int
}

// Current semantics is that we error on any use of any part of pair once we
// have invalidated a part of pair. We can be less restrictive in the future.
//
// TODO: Why are we emitting two uses here.
public func performMoveOnOneEltOfPair<T>(_ p: __owned Pair<T>) { // expected-error {{'p' used after consume}}
    let _ = p.z // Make sure we don't crash when we access a trivial value from Pair.
    let _ = consume p // expected-note {{consumed here}}
    nonConsumingUse(p.y) // expected-note {{used here}}
}

public class TPair<T> {
    var x: T? = nil
    var y: T? = nil
}

public func multipleVarsWithSubsequentBorrows<T : Equatable>(_ p: T) -> Bool {
    let k = p
    let k2 = k
    let k3 = consume k
    return k2 == k3
}

////////////////
// Cast Tests //
////////////////

public func castTest0<T : SubP1>(_ x: __owned T) -> P { // expected-error {{'x' used after consume}}
    let _ = consume x // expected-note {{consumed here}}
    return x as P // expected-note {{used here}}
}

public func castTest1<T : P>(_ x: __owned T) -> SubP2 { // expected-error {{'x' used after consume}}
    let _ = consume x // expected-note {{consumed here}}
    return x as! SubP2 // expected-note {{used here}}
}

public func castTest2<T : P>(_ x: __owned T) -> SubP1? { // expected-error {{'x' used after consume}}
    let _ = consume x // expected-note {{consumed here}}
    return x as? SubP1 // expected-note {{used here}}
}

public func castTestSwitch1<T : P>(_ x: __owned T) { // expected-error {{'x' used after consume}}
    let _ = consume x // expected-note {{consumed here}}
    switch x { // expected-note {{used here}}
    case let k as SubP1:
        print(k)
    default:
        print("Nope")
    }
}

public func castTestSwitch2<T : P>(_ x: __owned T) { // expected-error {{'x' used after consume}}
    let _ = consume x // expected-note {{consumed here}}
    switch x { // expected-note {{used here}}
    case let k as SubP1:
        print(k)
    case let k as SubP2:
        print(k)
    default:
        print("Nope")
    }
}

public func castTestSwitchInLoop<T : P>(_ x: __owned T) { // expected-error {{'x' used after consume}}
    let _ = consume x // expected-note {{consumed here}}

    for _ in 0..<1024 {
        switch x { // expected-note {{used here}}
        case let k as SubP1:
            print(k)
        default:
            print("Nope")
        }
    }
}

public func castTestIfLet<T : P>(_ x: __owned T) { // expected-error {{'x' used after consume}}
    let _ = consume x // expected-note {{consumed here}}
    if case let k as SubP1 = x { // expected-note {{used here}}
        print(k)
    } else {
        print("no")
    }
}

public func castTestIfLetInLoop<T : P>(_ x: __owned T) { // expected-error {{'x' used after consume}}
    let _ = consume x // expected-note {{consumed here}}
    for _ in 0..<1024 {
        if case let k as SubP1 = x { // expected-note {{used here}}
            print(k)
        } else {
            print("no")
        }
    }
}

public enum EnumWithKlass {
    case none
    case klass(P)
}

public func castTestIfLet2(_ x : __owned EnumWithKlass) { // expected-error {{'x' used after consume}}
    let _ = consume x // expected-note {{consumed here}}
    if case let .klass(k as SubP1) = x { // expected-note {{used here}}
        print(k)
    } else {
        print("no")
    }
}

/////////////////////////
// Partial Apply Tests //
/////////////////////////

// Emit a better error here. At least we properly error.
public func partialApplyTest<T>(_ x: __owned T) { // expected-error {{'x' used after consume}}
    let _ = consume x // expected-note {{consumed here}}
    let f = { // expected-note {{used here}}
        nonConsumingUse(x)
    }
    f()
}

/////////////////
// Defer Tests //
/////////////////

// TODO: Emit an error in the defer.
public func deferTest<T>(_ x: __owned T) { // expected-error {{'x' used after consume}}
    let _ = consume x // expected-note {{consumed here}}
    defer { // expected-note {{used here}}
        nonConsumingUse(x)
    }
    print("do Something")
}
