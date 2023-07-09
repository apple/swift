// RUN: %target-swift-emit-sil -sil-verify-all -verify %s

// This test makes sure when -enable-experimental-feature
// MoveOnlyPartialConsumption is disabled, we emit errors whenever we perform
// partial consumption.

public class Klass {}

public struct Empty : ~Copyable {}
public struct SingleFieldInt : ~Copyable {
    var y = 5
}
public struct SingleFieldLoadable2 : ~Copyable {
    var e = Empty()
}
public struct SingleFieldLoadable : ~Copyable {
    var e = Empty()
    var k = Klass()
}
public enum LoadableEnum : ~Copyable {
    case first(Empty)
    case second(Klass)
}
public struct LoadableType : ~Copyable {
    var e = Empty()
    var l = SingleFieldLoadable()
    var k = Klass()
    var lEnum: LoadableEnum = .first(Empty())
}

public protocol P {}

public struct SingleFieldAddressOnly2 : ~Copyable {
    var e = Empty()
    var k: P? = nil
}

public struct SingleFieldAddressOnly : ~Copyable {
    var e = Empty()
    var k: P? = nil
    var e2 = SingleFieldAddressOnly2()
}
public enum AddressOnlyEnum : ~Copyable {
    case first(Empty)
    case second(P?)
}
public struct AddressOnlyType : ~Copyable {
    var e = Empty()
    var l = SingleFieldAddressOnly()
    var k: P? = nil
    var lEnum: AddressOnlyEnum = .first(Empty())
}

func consumeVal(_ x: consuming LoadableType) {}
func consumeVal(_ x: consuming LoadableEnum) {}
func consumeVal(_ x: consuming Empty) {}
func consumeVal(_ x: consuming AddressOnlyType) {}
func consumeVal(_ x: consuming SingleFieldAddressOnly) {}
func consumeVal(_ x: consuming P) {}
func mutateVal(_ x: inout Empty) {}
func mutateVal(_ x: inout LoadableType) {}
func mutateVal(_ x: inout AddressOnlyType) {}
func mutateVal(_ x: inout SingleFieldAddressOnly) {}
func mutateVal(_ x: inout SingleFieldAddressOnly2) {}
func mutateVal(_ x: inout P) {}
func borrowVal(_ x: borrowing Empty) {}
func borrowVal(_ x: borrowing LoadableType) {}
func borrowVal(_ x: borrowing AddressOnlyType) {}
func borrowVal(_ x: borrowing P) {}

var bool: Bool { false }

//////////////////////////
// MARK: Loadable Tests //
//////////////////////////

func loadableTestLet() {
    let x = LoadableType()
    consumeVal(x)
    let _ = x.e // expected-error {{cannot partially consume 'x'}}
    consumeVal(x.e) // expected-error {{cannot partially consume 'x'}}
    borrowVal(x.e)
    let _ = x.k
    let _ = x.l.e // expected-error {{cannot partially consume 'x'}}
    consumeVal(x.l.e) // expected-error {{cannot partially consume 'x'}}
    borrowVal(x.e)
    let _ = x.l.k

    switch x.lEnum { // expected-error {{cannot partially consume 'x'}}
    case .first:
        break
    case .second:
        break
    }
}

func loadableTestLet2() {
    let x = LoadableType() // expected-error {{'x' used after consume}}
    consumeVal(x) // expected-note {{consumed here}}
    borrowVal(x) // expected-note {{used here}}
}

func loadableTestVar() {
    var x = LoadableType()
    x = LoadableType()
    consumeVal(x)
    let _ = x.e // expected-error {{cannot partially consume 'x'}}
    consumeVal(x.e) // expected-error {{cannot partially consume 'x'}}
    borrowVal(x.e)
    let _ = x.k
    let _ = x.l.e // expected-error {{cannot partially consume 'x'}}
    consumeVal(x.l.e) // expected-error {{cannot partially consume 'x'}}
    borrowVal(x.l.e)
    let _ = x.l.k
    switch x.lEnum { // expected-error {{cannot partially consume 'x'}}
    case .first:
        break
    case .second:
        break
    }
    consumeVal(x.lEnum) // expected-error {{cannot partially consume 'x'}}
}

func loadableTestVar2() {
    var x = LoadableType()
    x = LoadableType()
    mutateVal(&x)
    mutateVal(&x.e)
    x.e = Empty()
    x = LoadableType()
    consumeVal(x)
}

// We do not emit an error here since we emit an early error for x.e.
func loadableTestArg(_ x: borrowing LoadableType) {
    // expected-error @-1 {{'x' is borrowed and cannot be consumed}}
    let _ = x.e // expected-note {{consumed here}}
    let _ = x.k
    let _ = x.l.e
    let _ = x.l.k
    switch x.lEnum {
    case .first:
        break
    case .second:
        break
    }
}

func loadableTestInOutArg(_ x: inout LoadableType) {
    consumeVal(x)
    let _ = x.e // expected-error {{cannot partially consume 'x'}}
    consumeVal(x.e) // expected-error {{cannot partially consume 'x'}}
    let _ = x.k
    let _ = x.l.e // expected-error {{cannot partially consume 'x'}}
    let _ = x.l.k
    switch x.lEnum { // expected-error {{cannot partially consume 'x'}}
    case .first:
        break
    case .second:
        break
    }
}

func loadableTestInOutArg(_ x: consuming LoadableType) {
    let _ = x.e // expected-error {{cannot partially consume 'x'}}
    let _ = x.k
    let _ = x.l.e // expected-error {{cannot partially consume 'x'}}
    let _ = x.l.k
    switch x.lEnum { // expected-error {{cannot partially consume 'x'}}
    case .first:
        break
    case .second:
        break
    }
}

//////////////////////////////
// MARK: Address Only Tests //
//////////////////////////////

func addressOnlyTestLet() {
    let x = AddressOnlyType()
    let _ = x.e // expected-error {{cannot partially consume 'x'}}
    consumeVal(x.e) // expected-error {{cannot partially consume 'x'}}
    let _ = x.k
    let _ = x.l.e // expected-error {{cannot partially consume 'x'}}
    consumeVal(x.l.e) // expected-error {{cannot partially consume 'x'}}
    let _ = x.l.k

    switch x.lEnum { // expected-error {{cannot partially consume 'x'}}
    case .first:
        break
    case .second:
        break
    }
}

func addressOnlyTestVar() {
    var x = AddressOnlyType()
    x = AddressOnlyType()
    let _ = x.e // expected-error {{cannot partially consume 'x'}}
    let _ = x.k
    let _ = x.l.e // expected-error {{cannot partially consume 'x'}}
    let _ = x.l.k
    switch x.lEnum { // expected-error {{cannot partially consume 'x'}}
    case .first:
        break
    case .second:
        break
    }
}

func addressOnlyTestVar2() {
    var x = AddressOnlyType()
    x = AddressOnlyType()
    x.e = Empty()
    x = AddressOnlyType()
    mutateVal(&x)
    mutateVal(&x.e)
}

// We do not emit an error here since we emit an early error for x.e.
func addressOnlyTestArg(_ x: borrowing AddressOnlyType) {
    // expected-error @-1 {{'x' is borrowed and cannot be consumed}}
    // expected-error @-2 {{'x' is borrowed and cannot be consumed}}
    // expected-error @-3 {{'x' is borrowed and cannot be consumed}}
    // expected-error @-4 {{'x' is borrowed and cannot be consumed}}
    let _ = x.e // expected-note {{consumed here}}
    let _ = x.k
    let _ = x.l.e // expected-note {{consumed here}}
    let _ = x.l.k // expected-note {{consumed here}}
    switch x.lEnum { // expected-note {{consumed here}}
    case .first:
        break
    case .second:
        break
    }
}

func addressOnlyTestInOutArg(_ x: inout AddressOnlyType) {
    let _ = x.e // expected-error {{cannot partially consume 'x'}}
    x.e = Empty()
    consumeVal(x.e) // expected-error {{cannot partially consume 'x'}}
    let _ = x.k
    x.k = nil
    x.l = SingleFieldAddressOnly()
    consumeVal(x.l) // expected-error {{cannot partially consume 'x'}}
    let _ = x.l.e // expected-error {{cannot partially consume 'x'}}
    consumeVal(x.l.e) // expected-error {{cannot partially consume 'x'}}
    x.l.e = Empty()
    let _ = x.l.k
    switch x.lEnum { // expected-error {{cannot partially consume 'x'}}
    case .first:
        break
    case .second:
        break
    }
}

func addressOnlyTestInOutArg2(_ x: inout AddressOnlyType) {
    let _ = x.e // expected-error {{cannot partially consume 'x'}}
    x.e = Empty()
    mutateVal(&x.e)
    let _ = x.k
    x.k = nil
    x.l = SingleFieldAddressOnly()
    mutateVal(&x.l)
    mutateVal(&x.l.e)
    mutateVal(&x.l.e2)
    consumeVal(x.l) // expected-error {{cannot partially consume 'x'}}
}

func addressOnlyTestConsumingArg(_ x: consuming AddressOnlyType) {
    let _ = x.e // expected-error {{cannot partially consume 'x'}}
    consumeVal(x.e) // expected-error {{cannot partially consume 'x'}}
    let _ = x.k
    let _ = x.l.e // expected-error {{cannot partially consume 'x'}}
    consumeVal(x.l.e) // expected-error {{cannot partially consume 'x'}}
    let _ = x.l.k
    consumeVal(x.l.k!)
    switch x.lEnum { // expected-error {{cannot partially consume 'x'}}
    case .first:
        break
    case .second:
        break
    }
}

func addressOnlyTestConsumingArg2(_ x: consuming AddressOnlyType) {
    x.e = Empty()
    let _ = x
    x = AddressOnlyType()
    mutateVal(&x)
    mutateVal(&x.e)
    mutateVal(&x.l.e)
    mutateVal(&x.l.e2)
}

///////////////////////////////////////////
// MARK: Partial Reinit Single Field Int //
///////////////////////////////////////////

func partialReinitTestSingleFieldIntConsuming(x: consuming SingleFieldInt) {
    let _ = consume x // expected-note {{consumed here}}
    x.y = 5 // expected-error {{cannot partially reinitialize 'x' after it has been consumed; only full reinitialization is allowed}}
}

func partialReinitTestSingleFieldIntConsuming2(x: consuming SingleFieldInt) {
    x.y = 5
}

func partialReinitTestSingleFieldIntConsuming3(x: consuming SingleFieldInt) {
    let _ = consume x
    x = SingleFieldInt()
    x.y = 5
}

func partialReinitTestSingleFieldIntConsuming4(x: consuming SingleFieldInt) {
    let _ = consume x // expected-note {{consumed here}}

    if bool {
    }

    x.y = 5 // expected-error {{cannot partially reinitialize 'x' after it has been consumed; only full reinitialization is allowed}}
}

func partialReinitTestSingleFieldIntConsuming5(x: consuming SingleFieldInt) {
    let _ = consume x // expected-note {{consumed here}}

    if bool {
        x = SingleFieldInt()
    }
    x.y = 5 // expected-error {{cannot partially reinitialize 'x' after it has been consumed; only full reinitialization is allowed}}
}

func partialReinitTestSingleFieldIntConsuming5a(x: consuming SingleFieldInt) {
    let _ = consume x

    if bool {
        x = SingleFieldInt()
    } else {
        x = SingleFieldInt()
    }
    x.y = 5
}

func partialReinitTestSingleFieldIntConsuming5b(x: consuming SingleFieldInt) {
    let _ = consume x // expected-note {{consumed here}}

    if bool {
        x.y = 5 // expected-error {{cannot partially reinitialize 'x' after it has been consumed; only full reinitialization is allowed}}
    } else {
        x = SingleFieldInt()
    }
    x.y = 5
}

func partialReinitTestSingleFieldIntConsuming6(x: consuming SingleFieldInt) {
    let _ = consume x
    x = SingleFieldInt()
    if bool {
        x = SingleFieldInt()
    }
    x.y = 5
}

func partialReinitTestSingleFieldIntConsuming6a(x: consuming SingleFieldInt) {
    let _ = consume x // expected-note {{consumed here}}
    for _ in 0..<1024 {}
    x.y = 5 // expected-error {{cannot partially reinitialize 'x' after it has been consumed; only full reinitialization is allowed}}
}

func partialReinitTestSingleFieldIntConsuming6b(x: consuming SingleFieldInt) {
    // We error here since we /could/ skip the loop entirely.
    let _ = consume x // expected-note {{consumed here}}
    for _ in 0..<1024 {
        x = SingleFieldInt()
    }
    x.y = 5 // expected-error {{cannot partially reinitialize 'x' after it has been consumed; only full reinitialization is allowed}}
}

func partialReinitTestSingleFieldIntConsuming6c(x: consuming SingleFieldInt) {
    // We error here since we /could/ skip the loop entirely.
    let _ = consume x // expected-note {{consumed here}}
    for _ in 0..<1024 {
        x = SingleFieldInt()
    }
    x.y = 5 // expected-error {{cannot partially reinitialize 'x' after it has been consumed; only full reinitialization is allowed}}
}

func partialReinitTestSingleFieldIntInOut(x: inout SingleFieldInt) {
    let _ = consume x // expected-note {{consumed here}}
    x.y = 5 // expected-error {{cannot partially reinitialize 'x' after it has been consumed; only full reinitialization is allowed}}
}

func partialReinitTestSingleFieldIntInOut2(x: inout SingleFieldInt) {
    x.y = 5
}

func partialReinitTestSingleFieldIntInOut3(x: inout SingleFieldInt) {
    let _ = consume x
    x = SingleFieldInt()
    x.y = 5
}

func partialReinitTestSingleFieldIntInOut4(x: inout SingleFieldInt) {
    let _ = consume x // expected-note {{consumed here}}

    if bool {
    }
    x.y = 5 // expected-error {{cannot partially reinitialize 'x' after it has been consumed; only full reinitialization is allowed}}
}

func partialReinitTestSingleFieldIntInOut5(x: inout SingleFieldInt) {
    let _ = consume x // expected-note {{consumed here}}

    if bool {
        x = SingleFieldInt()
    }

    x.y = 5 // expected-error {{cannot partially reinitialize 'x' after it has been consumed; only full reinitialization is allowed}}
}

func partialReinitTestSingleFieldIntInOut5a(x: inout SingleFieldInt) {
    let _ = consume x

    if bool {
        x = SingleFieldInt()
    } else {
        x = SingleFieldInt()
    }
    x.y = 5
}

func partialReinitTestSingleFieldIntInOut5b(x: inout SingleFieldInt) {
    let _ = consume x // expected-note {{consumed here}}

    if bool {
        x.y = 5 // expected-error {{cannot partially reinitialize 'x' after it has been consumed; only full reinitialization is allowed}}
    } else {
        x = SingleFieldInt()
    }
    x.y = 5
}

func partialReinitTestSingleFieldIntInOut6(x: inout SingleFieldInt) {
    let _ = consume x
    x = SingleFieldInt()
    if bool {
        x = SingleFieldInt()
    }
    x.y = 5
}

func partialReinitTestSingleFieldIntInOut6a(x: inout SingleFieldInt) {
    let _ = consume x // expected-note {{consumed here}}
    for _ in 0..<1024 {}
    x.y = 5 // expected-error {{cannot partially reinitialize 'x' after it has been consumed; only full reinitialization is allowed}}
}

func partialReinitTestSingleFieldIntInOut6b(x: inout SingleFieldInt) {
    // We error here since we /could/ skip the loop entirely.
    let _ = consume x // expected-note {{consumed here}}
    for _ in 0..<1024 {
        x = SingleFieldInt()
    }
    x.y = 5 // expected-error {{cannot partially reinitialize 'x' after it has been consumed; only full reinitialization is allowed}}
}

func partialReinitTestSingleFieldIntInOut6c(x: inout SingleFieldInt) {
    // We error here since we /could/ skip the loop entirely.
    let _ = consume x // expected-note {{consumed here}}
    for _ in 0..<1024 {
        x = SingleFieldInt()
    }
    x.y = 5 // expected-error {{cannot partially reinitialize 'x' after it has been consumed; only full reinitialization is allowed}}
}

////////////////////////////////////////////////
// MARK: Partial Reinit Single Field Loadable //
////////////////////////////////////////////////

// Test both a true single field (SingleFieldLoadable2) and a pair
// (SingleFieldLoadable).
func partialReinitTestSingleFieldLoadable2Consuming(x: consuming SingleFieldLoadable2) {
    let _ = consume x // expected-note {{consumed here}}
    x.e = Empty() // expected-error {{cannot partially reinitialize 'x' after it has been consumed; only full reinitialization is allowed}}
}

func partialReinitTestSingleFieldLoadable2Consuming2(x: consuming SingleFieldLoadable2) {
    x.e = Empty()
}

func partialReinitTestSingleFieldLoadable2Consuming3(x: consuming SingleFieldLoadable2) {
    let _ = consume x
    x = SingleFieldLoadable2()
    x.e = Empty()
}

func partialReinitTestSingleFieldLoadable2Consuming4(x: consuming SingleFieldLoadable2) {
    let _ = consume x // expected-note {{consumed here}}

    if bool {
    }
    x.e = Empty() // expected-error {{cannot partially reinitialize 'x' after it has been consumed; only full reinitialization is allowed}}
}

func partialReinitTestSingleFieldLoadable2Consuming5(x: consuming SingleFieldLoadable2) {
    let _ = consume x // expected-note {{consumed here}}

    if bool {
        x = SingleFieldLoadable2()
    }
    x.e = Empty() // expected-error {{cannot partially reinitialize 'x' after it has been consumed; only full reinitialization is allowed}}
}

func partialReinitTestSingleFieldLoadable2Consuming5a(x: consuming SingleFieldLoadable2) {
    let _ = consume x

    if bool {
        x = SingleFieldLoadable2()
    } else {
        x = SingleFieldLoadable2()
    }
    x.e = Empty()
}

func partialReinitTestSingleFieldLoadable2Consuming5b(x: consuming SingleFieldLoadable2) {
    let _ = consume x // expected-note {{consumed here}}

    if bool {
        x.e = Empty() // expected-error {{cannot partially reinitialize 'x' after it has been consumed; only full reinitialization is allowed}}
    } else {
        x = SingleFieldLoadable2()
    }
    x.e = Empty()
}

func partialReinitTestSingleFieldLoadable2Consuming6(x: consuming SingleFieldLoadable2) {
    let _ = consume x
    x = SingleFieldLoadable2()
    if bool {
        x = SingleFieldLoadable2()
    }
    x.e = Empty()
}

func partialReinitTestSingleFieldLoadable2Consuming6a(x: consuming SingleFieldLoadable2) {
    let _ = consume x // expected-note {{consumed here}}
    for _ in 0..<1024 {}
    x.e = Empty() // expected-error {{cannot partially reinitialize 'x' after it has been consumed; only full reinitialization is allowed}}
}

func partialReinitTestSingleFieldLoadable2Consuming6b(x: consuming SingleFieldLoadable2) {
    // We error here since we /could/ skip the loop entirely.
    let _ = consume x // expected-note {{consumed here}}
    for _ in 0..<1024 {
        x = SingleFieldLoadable2()
    }
    x.e = Empty() // expected-error {{cannot partially reinitialize 'x' after it has been consumed; only full reinitialization is allowed}}
}

func partialReinitTestSingleFieldLoadable2Consuming6c(x: consuming SingleFieldLoadable2) {
    // We error here since we /could/ skip the loop entirely.
    let _ = consume x // expected-note {{consumed here}}
    for _ in 0..<1024 {
        x = SingleFieldLoadable2()
    }
    x.e = Empty() // expected-error {{cannot partially reinitialize 'x' after it has been consumed; only full reinitialization is allowed}}
}

func partialReinitTestSingleFieldLoadable2InOut(x: inout SingleFieldLoadable2) {
    let _ = consume x // expected-note {{consumed here}}
    x.e = Empty() // expected-error {{cannot partially reinitialize 'x' after it has been consumed; only full reinitialization is allowed}}
}

func partialReinitTestSingleFieldLoadable2InOut2(x: inout SingleFieldLoadable2) {
    x.e = Empty()
}

func partialReinitTestSingleFieldLoadable2InOut3(x: inout SingleFieldLoadable2) {
    let _ = consume x
    x = SingleFieldLoadable2()
    x.e = Empty()
}

func partialReinitTestSingleFieldLoadable2InOut4(x: inout SingleFieldLoadable2) {
    let _ = consume x // expected-note {{consumed here}}

    if bool {
    }
    x.e = Empty() // expected-error {{cannot partially reinitialize 'x' after it has been consumed; only full reinitialization is allowed}}
}

func partialReinitTestSingleFieldLoadable2InOut5(x: inout SingleFieldLoadable2) {
    let _ = consume x // expected-note {{consumed here}}

    if bool {
        x = SingleFieldLoadable2()
    }

    x.e = Empty() // expected-error {{cannot partially reinitialize 'x' after it has been consumed; only full reinitialization is allowed}}
}

func partialReinitTestSingleFieldLoadable2InOut5a(x: inout SingleFieldLoadable2) {
    let _ = consume x

    if bool {
        x = SingleFieldLoadable2()
    } else {
        x = SingleFieldLoadable2()
    }
    x.e = Empty()
}

func partialReinitTestSingleFieldLoadable2InOut5b(x: inout SingleFieldLoadable2) {
    let _ = consume x // expected-note {{consumed here}}

    if bool {
        x.e = Empty() // expected-error {{cannot partially reinitialize 'x' after it has been consumed; only full reinitialization is allowed}}
    } else {
        x = SingleFieldLoadable2()
    }
    x.e = Empty()
}

func partialReinitTestSingleFieldLoadable2InOut6(x: inout SingleFieldLoadable2) {
    let _ = consume x
    x = SingleFieldLoadable2()
    if bool {
        x = SingleFieldLoadable2()
    }
    x.e = Empty()
}

func partialReinitTestSingleFieldLoadable2InOut6a(x: inout SingleFieldLoadable2) {
    let _ = consume x // expected-note {{consumed here}}
    for _ in 0..<1024 {}
    x.e = Empty() // expected-error {{cannot partially reinitialize 'x' after it has been consumed; only full reinitialization is allowed}}
}

func partialReinitTestSingleFieldLoadable2InOut6b(x: inout SingleFieldLoadable2) {
    // We error here since we /could/ skip the loop entirely.
    let _ = consume x // expected-note {{consumed here}}
    for _ in 0..<1024 {
        x = SingleFieldLoadable2()
    }
    x.e = Empty() // expected-error {{cannot partially reinitialize 'x' after it has been consumed; only full reinitialization is allowed}}
}

func partialReinitTestSingleFieldLoadable2InOut6c(x: inout SingleFieldLoadable2) {
    // We error here since we /could/ skip the loop entirely.
    let _ = consume x // expected-note {{consumed here}}
    for _ in 0..<1024 {
        x = SingleFieldLoadable2()
    }
    x.e = Empty() // expected-error {{cannot partially reinitialize 'x' after it has been consumed; only full reinitialization is allowed}}
}

func partialReinitTestSingleFieldLoadableConsuming(x: consuming SingleFieldLoadable) {
    let _ = consume x // expected-note {{consumed here}}
    x.e = Empty() // expected-error {{cannot partially reinitialize 'x' after it has been consumed; only full reinitialization is allowed}}
}

func partialReinitTestSingleFieldLoadableConsuming2(x: consuming SingleFieldLoadable) {
    x.e = Empty()
}

func partialReinitTestSingleFieldLoadableConsuming3(x: consuming SingleFieldLoadable) {
    let _ = consume x
    x = SingleFieldLoadable()
    x.e = Empty()
}

func partialReinitTestSingleFieldLoadableConsuming4(x: consuming SingleFieldLoadable) {
    let _ = consume x // expected-note {{consumed here}}

    if bool {
    }
    x.e = Empty() // expected-error {{cannot partially reinitialize 'x' after it has been consumed; only full reinitialization is allowed}}
}

func partialReinitTestSingleFieldLoadableConsuming5(x: consuming SingleFieldLoadable) {
    let _ = consume x // expected-note {{consumed here}}

    if bool {
        x = SingleFieldLoadable()
    }
    x.e = Empty() // expected-error {{cannot partially reinitialize 'x' after it has been consumed; only full reinitialization is allowed}}
}

func partialReinitTestSingleFieldLoadableConsuming5a(x: consuming SingleFieldLoadable) {
    let _ = consume x

    if bool {
        x = SingleFieldLoadable()
    } else {
        x = SingleFieldLoadable()
    }
    x.e = Empty()
}

func partialReinitTestSingleFieldLoadableConsuming5b(x: consuming SingleFieldLoadable) {
    let _ = consume x // expected-note {{consumed here}}

    if bool {
        x.e = Empty() // expected-error {{cannot partially reinitialize 'x' after it has been consumed; only full reinitialization is allowed}}
    } else {
        x = SingleFieldLoadable()
    }
    x.e = Empty()
}

func partialReinitTestSingleFieldLoadableConsuming6(x: consuming SingleFieldLoadable) {
    let _ = consume x
    x = SingleFieldLoadable()
    if bool {
        x = SingleFieldLoadable()
    }
    x.e = Empty()
}

func partialReinitTestSingleFieldLoadableConsuming6a(x: consuming SingleFieldLoadable) {
    let _ = consume x // expected-note {{consumed here}}
    for _ in 0..<1024 {}
    x.e = Empty() // expected-error {{cannot partially reinitialize 'x' after it has been consumed; only full reinitialization is allowed}}
}

func partialReinitTestSingleFieldLoadableConsuming6b(x: consuming SingleFieldLoadable) {
    // We error here since we /could/ skip the loop entirely.
    let _ = consume x // expected-note {{consumed here}}
    for _ in 0..<1024 {
        x = SingleFieldLoadable()
    }
    x.e = Empty() // expected-error {{cannot partially reinitialize 'x' after it has been consumed; only full reinitialization is allowed}}
}

func partialReinitTestSingleFieldLoadableConsuming6c(x: consuming SingleFieldLoadable) {
    // We error here since we /could/ skip the loop entirely.
    let _ = consume x // expected-note {{consumed here}}
    for _ in 0..<1024 {
        x = SingleFieldLoadable()
    }
    x.e = Empty() // expected-error {{cannot partially reinitialize 'x' after it has been consumed; only full reinitialization is allowed}}
}

func partialReinitTestSingleFieldLoadableInOut(x: inout SingleFieldLoadable) {
    let _ = consume x // expected-note {{consumed here}}
    x.e = Empty() // expected-error {{cannot partially reinitialize 'x' after it has been consumed; only full reinitialization is allowed}}
}

func partialReinitTestSingleFieldLoadableInOut2(x: inout SingleFieldLoadable) {
    x.e = Empty()
}

func partialReinitTestSingleFieldLoadableInOut3(x: inout SingleFieldLoadable) {
    let _ = consume x
    x = SingleFieldLoadable()
    x.e = Empty()
}

func partialReinitTestSingleFieldLoadableInOut4(x: inout SingleFieldLoadable) {
    let _ = consume x // expected-note {{consumed here}}

    if bool {
    }
    x.e = Empty() // expected-error {{cannot partially reinitialize 'x' after it has been consumed; only full reinitialization is allowed}}
}

func partialReinitTestSingleFieldLoadableInOut5(x: inout SingleFieldLoadable) {
    let _ = consume x // expected-note {{consumed here}}

    if bool {
        x = SingleFieldLoadable()
    }

    x.e = Empty() // expected-error {{cannot partially reinitialize 'x' after it has been consumed; only full reinitialization is allowed}}
}

func partialReinitTestSingleFieldLoadableInOut5a(x: inout SingleFieldLoadable) {
    let _ = consume x

    if bool {
        x = SingleFieldLoadable()
    } else {
        x = SingleFieldLoadable()
    }
    x.e = Empty()
}

func partialReinitTestSingleFieldLoadableInOut5b(x: inout SingleFieldLoadable) {
    let _ = consume x // expected-note {{consumed here}}

    if bool {
        x.e = Empty() // expected-error {{cannot partially reinitialize 'x' after it has been consumed; only full reinitialization is allowed}}
    } else {
        x = SingleFieldLoadable()
    }
    x.e = Empty()
}

func partialReinitTestSingleFieldLoadableInOut6(x: inout SingleFieldLoadable) {
    let _ = consume x
    x = SingleFieldLoadable()
    if bool {
        x = SingleFieldLoadable()
    }
    x.e = Empty()
}

func partialReinitTestSingleFieldLoadableInOut6a(x: inout SingleFieldLoadable) {
    let _ = consume x // expected-note {{consumed here}}
    for _ in 0..<1024 {}
    x.e = Empty() // expected-error {{cannot partially reinitialize 'x' after it has been consumed; only full reinitialization is allowed}}
}

func partialReinitTestSingleFieldLoadableInOut6b(x: inout SingleFieldLoadable) {
    // We error here since we /could/ skip the loop entirely.
    let _ = consume x // expected-note {{consumed here}}
    for _ in 0..<1024 {
        x = SingleFieldLoadable()
    }
    x.e = Empty() // expected-error {{cannot partially reinitialize 'x' after it has been consumed; only full reinitialization is allowed}}
}

func partialReinitTestSingleFieldLoadableInOut6c(x: inout SingleFieldLoadable) {
    // We error here since we /could/ skip the loop entirely.
    let _ = consume x // expected-note {{consumed here}}
    for _ in 0..<1024 {
        x = SingleFieldLoadable()
    }
    x.e = Empty() // expected-error {{cannot partially reinitialize 'x' after it has been consumed; only full reinitialization is allowed}}
}

func partialReinitTestSingleFieldLoadableInOut6d(x: inout SingleFieldLoadable) {
    for _ in 0..<1024 {
        x = SingleFieldLoadable()
    }
    x = SingleFieldLoadable()
    if bool {}
    x.e = Empty()
}

///////////////////////////////////////
// MARK: Partial Reinit Address Only //
///////////////////////////////////////

func partialReinitTestSingleFieldAddressOnlyConsuming(x: consuming SingleFieldAddressOnly) {
    let _ = consume x // expected-note {{consumed here}}
    x.e = Empty() // expected-error {{cannot partially reinitialize 'x' after it has been consumed; only full reinitialization is allowed}}
}

func partialReinitTestSingleFieldAddressOnlyConsuming2(x: consuming SingleFieldAddressOnly) {
    x.e = Empty()
}

func partialReinitTestSingleFieldAddressOnlyConsuming3(x: consuming SingleFieldAddressOnly) {
    let _ = consume x
    x = SingleFieldAddressOnly()
    x.e = Empty()
}

func partialReinitTestSingleFieldAddressOnlyConsuming4(x: consuming SingleFieldAddressOnly) {
    let _ = consume x // expected-note {{consumed here}}

    if bool {
    }
    x.e = Empty() // expected-error {{cannot partially reinitialize 'x' after it has been consumed; only full reinitialization is allowed}}
}

func partialReinitTestSingleFieldAddressOnlyConsuming5(x: consuming SingleFieldAddressOnly) {
    let _ = consume x // expected-note {{consumed here}}

    if bool {
        x = SingleFieldAddressOnly()
    }
    x.e = Empty() // expected-error {{cannot partially reinitialize 'x' after it has been consumed; only full reinitialization is allowed}}
}

func partialReinitTestSingleFieldAddressOnlyConsuming5a(x: consuming SingleFieldAddressOnly) {
    let _ = consume x

    if bool {
        x = SingleFieldAddressOnly()
    } else {
        x = SingleFieldAddressOnly()
    }
    x.e = Empty()
}

func partialReinitTestSingleFieldAddressOnlyConsuming5b(x: consuming SingleFieldAddressOnly) {
    let _ = consume x // expected-note {{consumed here}}

    if bool {
        x.e = Empty() // expected-error {{cannot partially reinitialize 'x' after it has been consumed; only full reinitialization is allowed}}
    } else {
        x = SingleFieldAddressOnly()
    }
    x.e = Empty()
}

func partialReinitTestSingleFieldAddressOnlyConsuming6(x: consuming SingleFieldAddressOnly) {
    let _ = consume x
    x = SingleFieldAddressOnly()
    if bool {
        x = SingleFieldAddressOnly()
    }
    x.e = Empty()
}

func partialReinitTestSingleFieldAddressOnlyConsuming6a(x: consuming SingleFieldAddressOnly) {
    let _ = consume x // expected-note {{consumed here}}
    for _ in 0..<1024 {}
    x.e = Empty() // expected-error {{cannot partially reinitialize 'x' after it has been consumed; only full reinitialization is allowed}}
}

func partialReinitTestSingleFieldAddressOnlyConsuming6b(x: consuming SingleFieldAddressOnly) {
    // We error here since we /could/ skip the loop entirely.
    let _ = consume x // expected-note {{consumed here}}
    for _ in 0..<1024 {
        x = SingleFieldAddressOnly()
    }
    x.e = Empty() // expected-error {{cannot partially reinitialize 'x' after it has been consumed; only full reinitialization is allowed}}
}

func partialReinitTestSingleFieldAddressOnlyConsuming6c(x: consuming SingleFieldAddressOnly) {
    // We error here since we /could/ skip the loop entirely.
    let _ = consume x // expected-note {{consumed here}}
    for _ in 0..<1024 {
        x = SingleFieldAddressOnly()
    }
    x.e = Empty() // expected-error {{cannot partially reinitialize 'x' after it has been consumed; only full reinitialization is allowed}}
}

func partialReinitTestSingleFieldAddressOnlyInOut(x: inout SingleFieldAddressOnly) {
    let _ = consume x // expected-note {{consumed here}}
    x.e = Empty() // expected-error {{cannot partially reinitialize 'x' after it has been consumed; only full reinitialization is allowed}}
}

func partialReinitTestSingleFieldAddressOnlyInOut2(x: inout SingleFieldAddressOnly) {
    x.e = Empty()
}

func partialReinitTestSingleFieldAddressOnlyInOut3(x: inout SingleFieldAddressOnly) {
    let _ = consume x
    x = SingleFieldAddressOnly()
    x.e = Empty()
}

func partialReinitTestSingleFieldAddressOnlyInOut4(x: inout SingleFieldAddressOnly) {
    let _ = consume x // expected-note {{consumed here}}

    if bool {
    }
    x.e = Empty() // expected-error {{cannot partially reinitialize 'x' after it has been consumed; only full reinitialization is allowed}}
}

func partialReinitTestSingleFieldAddressOnlyInOut5(x: inout SingleFieldAddressOnly) {
    let _ = consume x // expected-note {{consumed here}}

    if bool {
        x = SingleFieldAddressOnly()
    }

    x.e = Empty() // expected-error {{cannot partially reinitialize 'x' after it has been consumed; only full reinitialization is allowed}}
}

func partialReinitTestSingleFieldAddressOnlyInOut5a(x: inout SingleFieldAddressOnly) {
    let _ = consume x

    if bool {
        x = SingleFieldAddressOnly()
    } else {
        x = SingleFieldAddressOnly()
    }
    x.e = Empty()
}

func partialReinitTestSingleFieldAddressOnlyInOut5b(x: inout SingleFieldAddressOnly) {
    let _ = consume x // expected-note {{consumed here}}

    if bool {
        x.e = Empty() // expected-error {{cannot partially reinitialize 'x' after it has been consumed; only full reinitialization is allowed}}
    } else {
        x = SingleFieldAddressOnly()
    }
    x.e = Empty()
}

func partialReinitTestSingleFieldAddressOnlyInOut5c(x: inout SingleFieldAddressOnly) {
    let _ = consume x // expected-note {{consumed here}}

    if bool {
        x.e2 = SingleFieldAddressOnly2() // expected-error {{cannot partially reinitialize 'x' after it has been consumed; only full reinitialization is allowed}}
    } else {
        x = SingleFieldAddressOnly()
    }
    x.e2 = SingleFieldAddressOnly2()
}

func partialReinitTestSingleFieldAddressOnlyInOut6(x: inout SingleFieldAddressOnly) {
    let _ = consume x
    x = SingleFieldAddressOnly()
    if bool {
        x = SingleFieldAddressOnly()
    }
    x.e = Empty()
}

func partialReinitTestSingleFieldAddressOnlyInOut6a(x: inout SingleFieldAddressOnly) {
    let _ = consume x // expected-note {{consumed here}}
    for _ in 0..<1024 {}
    x.e = Empty() // expected-error {{cannot partially reinitialize 'x' after it has been consumed; only full reinitialization is allowed}}
}

func partialReinitTestSingleFieldAddressOnlyInOut6b(x: inout SingleFieldAddressOnly) {
    // We error here since we /could/ skip the loop entirely.
    let _ = consume x // expected-note {{consumed here}}
    for _ in 0..<1024 {
        x = SingleFieldAddressOnly()
    }
    x.e = Empty() // expected-error {{cannot partially reinitialize 'x' after it has been consumed; only full reinitialization is allowed}}
}

func partialReinitTestSingleFieldAddressOnlyInOut6c(x: inout SingleFieldAddressOnly) {
    // We error here since we /could/ skip the loop entirely.
    let _ = consume x // expected-note {{consumed here}}
    for _ in 0..<1024 {
        x = SingleFieldAddressOnly()
    }
    x.e = Empty() // expected-error {{cannot partially reinitialize 'x' after it has been consumed; only full reinitialization is allowed}}
}

/////////////////////////////////////////////////
// MARK: Partial Reinit Error on Copyable Type //
/////////////////////////////////////////////////

func partialReinitTestErrorOnCopyableField() {
    var x = LoadableType()
    _ = consume x // expected-note {{consumed here}}
    x.k = Klass() // expected-error {{cannot partially reinitialize 'x' after it has been consumed; only full reinitialization is allowed}}
    x = LoadableType()
    x.k = Klass()
}

////////////////////////////////////
// MARK: Partial Reinit Test Self //
////////////////////////////////////

struct TestTrivialReturnValue : ~Copyable {
    var i: Int = 5

    deinit {}

    mutating func drain2() -> Int {
        let buffer = (consume self).i // expected-note {{consumed here}}
        i = 5 // expected-error {{cannot partially reinitialize 'self' after it has been consumed; only full reinitialization is allowed}}
        return buffer
    }
}

struct TestAddressOnlyReturnValue : ~Copyable {
    var i: Int = 5
    var p: P? = nil

    deinit {}

    mutating func drain2() -> Int {
        let buffer = (consume self).i // expected-note {{consumed here}}
        p = nil // expected-error {{cannot partially reinitialize 'self' after it has been consumed; only full reinitialization is allowed}}
        return buffer
    }
}
