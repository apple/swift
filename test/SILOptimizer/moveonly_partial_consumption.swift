// RUN: %target-swift-emit-sil -sil-verify-all -verify %s

// This test makes sure when -enable-experimental-feature
// MoveOnlyPartialConsumption is disabled, we emit errors whenever we perform
// partial consumption.

public class Klass {}

public struct Empty : ~Copyable {}
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
