// RUN: %target-swift-frontend -sil-verify-all -verify -emit-sil -enable-experimental-feature MoveOnlyEnumDeinits %s

class Klass {}

var globalMoveOnlyStruct = MoveOnlyStruct()
var globalMoveOnlyEnum = MoveOnlyEnum.lhs(Klass())

@_moveOnly
struct MoveOnlyStruct {
    var k = Klass()

    deinit {
        // expected-error @-1 {{'self' consumed more than once}}
        // expected-error @-2 {{'self' consumed more than once}}
        // expected-error @-3 {{'self' consumed more than once}}
        let x = self // expected-note {{consumed here}}
        _ = x
        var y = MoveOnlyStruct() // expected-error {{'y' consumed more than once}}
        y = self
        // expected-note @-1 {{consumed here}}
        // expected-note @-2 {{consumed again here}}
        _ = y
        // expected-note @-1 {{consumed here}}
        let z = y // expected-note {{consumed again here}}
        let _ = z
        globalMoveOnlyStruct = self // expected-note {{consumed here}}
        // expected-note @-1 {{consumed again here}}
    } // expected-note {{consumed again here}}
}

@_moveOnly
enum MoveOnlyEnum {
    case lhs(Klass)
    case rhs(Klass)

    deinit {
        // expected-error @-1 {{'self' consumed more than once}}
        // expected-error @-2 {{'self' consumed more than once}}
        // expected-error @-3 {{'self' consumed more than once}}
        let x = self // expected-note {{consumed here}}
        _ = x
        var y = MoveOnlyEnum.lhs(Klass())
        y = self // expected-note {{consumed here}}
        // expected-note @-1 {{consumed again here}}
        _ = y 
        globalMoveOnlyEnum = self // expected-note {{consumed here}}
        // expected-note @-1 {{consumed again here}}
    } // expected-note {{consumed again here}}
}
