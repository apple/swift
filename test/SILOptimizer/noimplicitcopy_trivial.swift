// RUN: %target-swift-frontend -sil-verify-all -enable-experimental-move-only -verify %s -parse-stdlib -emit-sil

import Swift

public struct Trivial {
    var value: Int
}

var boolValue: Bool { return true }

public func trivialUseMoveOnlyWithoutEscaping(_ x: Trivial) {
}

public func trivialSimpleChainTest(_ x: Trivial) {
    @_noImplicitCopy let x2 = x
    let y2 = x2
    let k2 = y2
    trivialUseMoveOnlyWithoutEscaping(k2)
}

public func trivialSimpleChainTestArg(@_noImplicitCopy _ x2: Trivial) { // expected-error {{'x2' is borrowed and cannot be consumed}}
    let y2 = x2 // expected-note {{consumed here}}
    let k2 = y2
    trivialUseMoveOnlyWithoutEscaping(k2)
}

public func trivialSimpleChainTestOwnedArg(@_noImplicitCopy _ x2: __owned Trivial) {
    let y2 = x2
    let k2 = y2
    trivialUseMoveOnlyWithoutEscaping(k2)
}

public func trivialMultipleNonConsumingUseTest(_ x: Trivial) {
    @_noImplicitCopy let x2 = x
    trivialUseMoveOnlyWithoutEscaping(x2)
    trivialUseMoveOnlyWithoutEscaping(x2)
    print(x2)
}

public func trivialMultipleNonConsumingUseTestArg(@_noImplicitCopy _ x2: Trivial) { // expected-error {{'x2' is borrowed and cannot be consumed}}
    trivialUseMoveOnlyWithoutEscaping(x2)
    trivialUseMoveOnlyWithoutEscaping(x2)
    print(x2) // expected-note {{consumed here}}
}

public func trivialMultipleNonConsumingUseTestOwnedArg(@_noImplicitCopy _ x2: __owned Trivial) {
    trivialUseMoveOnlyWithoutEscaping(x2)
    trivialUseMoveOnlyWithoutEscaping(x2)
    print(x2)
}

public func trivialUseAfterConsume(_ x: Trivial) {
    @_noImplicitCopy let x2 = x
    // expected-error @-1 {{'x2' consumed more than once}}
    // expected-error @-2 {{'x2' consumed more than once}}
    let y = x2 // expected-note {{consumed here}}
    let z = x2 // expected-note {{consumed again here}}
    // expected-note @-1 {{consumed here}}
    let _ = y
    let _ = z
    print(x2)
    // expected-note @-1 {{consumed again here}}
}

public func trivialUseAfterConsumeArg(@_noImplicitCopy _ x2: Trivial) { // expected-error {{'x2' is borrowed and cannot be consumed}}
    let y = x2 // expected-note {{consumed here}}
    let z = x2 // expected-note {{consumed here}}
    let _ = y
    let _ = z
    print(x2) // expected-note {{consumed here}}
}

public func trivialUseAfterConsumeOwnedArg(@_noImplicitCopy _ x2: __owned Trivial) {
    // expected-error @-1 {{'x2' consumed more than once}}
    // expected-error @-2 {{'x2' consumed more than once}}
    let y = x2 // expected-note {{consumed here}}
    let z = x2 // expected-note {{consumed again here}}
    // expected-note @-1 {{consumed here}}
    let _ = y
    let _ = z
    print(x2)
    // expected-note @-1 {{consumed again here}}
}

public func trivialDoubleConsume(_ x: Trivial) {
    @_noImplicitCopy let x2 = x  // expected-error {{'x2' consumed more than once}}
    let y = x2 // expected-note {{consumed here}}
    let z = x2 // expected-note {{consumed again here}}
    let _ = y
    let _ = z
}

public func trivialDoubleConsumeArg(@_noImplicitCopy _ x2: Trivial) { // expected-error {{'x2' is borrowed and cannot be consumed}}
    let y = x2 // expected-note {{consumed here}}
    let z = x2 // expected-note {{consumed here}}
    let _ = y
    let _ = z
}

public func trivialDoubleConsumeOwnedArg(@_noImplicitCopy _ x2: __owned Trivial) { // expected-error {{'x2' consumed more than once}}
    let y = x2 // expected-note {{consumed here}}
    let z = x2 // expected-note {{consumed again here}}
    let _ = y
    let _ = z
}

public func trivialLoopConsume(_ x: Trivial) {
    @_noImplicitCopy let x2 = x // expected-error {{'x2' consumed in a loop}}
    for _ in 0..<1024 {
        let y = x2 // expected-note {{consumed in loop here}}
        let _ = y
    }
}

public func trivialLoopConsumeArg(@_noImplicitCopy _ x2: Trivial) { // expected-error {{'x2' is borrowed and cannot be consumed}}
    for _ in 0..<1024 {
        let y = x2 // expected-note {{consumed here}}
        let _ = y
    }
}

public func trivialLoopConsumeOwnedArg(@_noImplicitCopy _ x2: __owned Trivial) { // expected-error {{'x2' consumed in a loop}}
    for _ in 0..<1024 {
        let y = x2 // expected-note {{consumed in loop here}}
        let _ = y
    }
}

public func trivialDiamond(_ x: Trivial) {
    @_noImplicitCopy let x2 = x
    if boolValue {
        let y = x2
        let _ = y
    } else {
        let z = x2
        let _ = z
    }
}

public func trivialDiamondArg(@_noImplicitCopy _ x2: Trivial) { // expected-error {{'x2' is borrowed and cannot be consumed}}
    if boolValue {
        let y = x2 // expected-note {{consumed here}}
        let _ = y
    } else {
        let z = x2 // expected-note {{consumed here}}
        let _ = z
    }
}

public func trivialDiamondOwnedArg(@_noImplicitCopy _ x2: __owned Trivial) {
    if boolValue {
        let y = x2
        let _ = y
    } else {
        let z = x2
        let _ = z
    }
}

public func trivialDiamondInLoop(_ x: Trivial) {
    @_noImplicitCopy let x2 = x // expected-error {{'x2' consumed more than once}}
    // expected-error @-1 {{'x2' consumed in a loop}}
    for _ in 0..<1024 {
      if boolValue {
          let y = x2 // expected-note {{consumed here}}
          let _ = y
      } else {
          let z = x2 // expected-note {{consumed in loop here}}
          // expected-note @-1 {{consumed again here}}
          let _ = z
      }
    }
}

public func trivialDiamondInLoopArg(@_noImplicitCopy _ x2: Trivial) { // expected-error {{'x2' is borrowed and cannot be consumed}}
    for _ in 0..<1024 {
      if boolValue {
          let y = x2 // expected-note {{consumed here}}
          let _ = y
      } else {
          let z = x2 // expected-note {{consumed here}}
          let _ = z
      }
    }
}

public func trivialDiamondInLoopOwnedArg(@_noImplicitCopy _ x2: __owned Trivial) {
    // expected-error @-1 {{'x2' consumed more than once}}
    // expected-error @-2 {{'x2' consumed in a loop}}
    for _ in 0..<1024 {
      if boolValue {
          let y = x2 // expected-note {{consumed here}}
          let _ = y
      } else {
          let z = x2 // expected-note {{consumed in loop here}}
          // expected-note @-1 {{consumed again here}}
          let _ = z
      }
    }
}

public func trivialAssignToVar1(_ x: Trivial) {
    @_noImplicitCopy let x2 = x // expected-error {{'x2' consumed more than once}}
    var x3 = x2 // expected-note {{consumed here}}
    x3 = x2 // expected-note {{consumed again here}}
    x3 = x
    print(x3)
}

public func trivialAssignToVar1Arg(_ x: Trivial, @_noImplicitCopy _ x2: Trivial) { // expected-error {{'x2' is borrowed and cannot be consumed}}
    var x3 = x2 // expected-note {{consumed here}}
    x3 = x2 // expected-note {{consumed here}}
    x3 = x
    print(x3)
}

public func trivialAssignToVar1OwnedArg(_ x: Trivial, @_noImplicitCopy _ x2: __owned Trivial) { // expected-error {{'x2' consumed more than once}}
    var x3 = x2 // expected-note {{consumed here}}
    x3 = x2 // expected-note {{consumed again here}}
    x3 = x
    print(x3)
}

public func trivialAssignToVar2(_ x: Trivial) {
    @_noImplicitCopy let x2 = x // expected-error {{'x2' consumed more than once}}
    var x3 = x2 // expected-note {{consumed here}}
    x3 = x2 // expected-note {{consumed again here}}
    trivialUseMoveOnlyWithoutEscaping(x3)
}

public func trivialAssignToVar2Arg(@_noImplicitCopy _ x2: Trivial) { // expected-error {{'x2' is borrowed and cannot be consumed}}
    var x3 = x2 // expected-note {{consumed here}}
    x3 = x2 // expected-note {{consumed here}}
    trivialUseMoveOnlyWithoutEscaping(x3)
}

public func trivialAssignToVar2OwnedArg(@_noImplicitCopy _ x2: __owned Trivial) { // expected-error {{'x2' consumed more than once}}
    var x3 = x2 // expected-note {{consumed here}}
    x3 = x2 // expected-note {{consumed again here}}
    trivialUseMoveOnlyWithoutEscaping(x3)
}

public func trivialAssignToVar3(_ x: Trivial) {
    @_noImplicitCopy let x2 = x
    var x3 = x2
    x3 = x
    print(x3)
}

public func trivialAssignToVar3Arg(_ x: Trivial, @_noImplicitCopy _ x2: Trivial) { // expected-error {{'x2' is borrowed and cannot be consumed}}
    var x3 = x2 // expected-note {{consumed here}}
    x3 = x
    print(x3)
}

public func trivialAssignToVar3OwnedArg(_ x: Trivial, @_noImplicitCopy _ x2: __owned Trivial) {
    var x3 = x2
    x3 = x
    print(x3)
}

public func trivialAssignToVar4(_ x: Trivial) {
    @_noImplicitCopy let x2 = x // expected-error {{'x2' consumed more than once}}
    let x3 = x2 // expected-note {{consumed here}}
    print(x2) // expected-note {{consumed again here}}
    print(x3)
}

public func trivialAssignToVar4Arg(@_noImplicitCopy _ x2: Trivial) { // expected-error {{'x2' is borrowed and cannot be consumed}}
    let x3 = x2 // expected-note {{consumed here}}
    print(x2) // expected-note {{consumed here}}
    print(x3)
}

public func trivialAssignToVar4OwnedArg(@_noImplicitCopy _ x2: __owned Trivial) { // expected-error {{'x2' consumed more than once}}
    let x3 = x2 // expected-note {{consumed here}}
    print(x2) // expected-note {{consumed again here}}
    print(x3)
}

public func trivialAssignToVar5(_ x: Trivial) {
    @_noImplicitCopy let x2 = x // expected-error {{'x2' used after consume}}
    var x3 = x2 // expected-note {{consumed here}}
    trivialUseMoveOnlyWithoutEscaping(x2) // expected-note {{used here}}
    x3 = x
    print(x3)
}

public func trivialAssignToVar5Arg(_ x: Trivial, @_noImplicitCopy _ x2: Trivial) { // expected-error {{'x2' is borrowed and cannot be consumed}}
    var x3 = x2 // expected-note {{consumed here}}
    trivialUseMoveOnlyWithoutEscaping(x2)
    x3 = x
    print(x3)
}

public func trivialAssignToVar5OwnedArg(_ x: Trivial, @_noImplicitCopy _ x2: __owned Trivial) { // expected-error {{'x2' used after consume}}
    var x3 = x2 // expected-note {{consumed here}}
    trivialUseMoveOnlyWithoutEscaping(x2) // expected-note {{used here}}
    x3 = x
    print(x3)
}

public func trivialAccessField(_ x: Trivial) {
    @_noImplicitCopy let x2 = x
    print(x2.value)
    for _ in 0..<1024 {
        print(x2.value)
    }
}

public func trivialAccessFieldArg(@_noImplicitCopy _ x2: Trivial) {
    print(x2.value)
    for _ in 0..<1024 {
        print(x2.value)
    }
}

public func trivialAccessFieldOwnedArg(@_noImplicitCopy _ x2: __owned Trivial) {
    print(x2.value)
    for _ in 0..<1024 {
        print(x2.value)
    }
}

//////////////////////
// Aggregate Struct //
//////////////////////

public struct AggStruct {
    var lhs: Trivial
    var center: Builtin.Int32
    var rhs: Trivial
}

public func aggStructUseMoveOnlyWithoutEscaping(_ x: AggStruct) {
}
public func aggStructConsume(_ x: __owned AggStruct) {
}

public func aggStructSimpleChainTest(_ x: AggStruct) {
    @_noImplicitCopy let x2 = x
    let y2 = x2
    let k2 = y2
    aggStructUseMoveOnlyWithoutEscaping(k2)
}

public func aggStructSimpleChainTestArg(@_noImplicitCopy _ x2: AggStruct) { // expected-error {{'x2' is borrowed and cannot be consumed}}
    let y2 = x2 // expected-note {{consumed here}}
    let k2 = y2
    aggStructUseMoveOnlyWithoutEscaping(k2)
}

public func aggStructSimpleChainTestOwnedArg(@_noImplicitCopy _ x2: __owned AggStruct) {
    let y2 = x2
    let k2 = y2
    aggStructUseMoveOnlyWithoutEscaping(k2)
}

public func aggStructSimpleNonConsumingUseTest(_ x: AggStruct) {
    @_noImplicitCopy let x2 = x
    aggStructUseMoveOnlyWithoutEscaping(x2)
}

public func aggStructSimpleNonConsumingUseTestArg(@_noImplicitCopy _ x2: AggStruct) {
    aggStructUseMoveOnlyWithoutEscaping(x2)
}

public func aggStructSimpleNonConsumingUseTestOwnedArg(@_noImplicitCopy _ x2: __owned AggStruct) {
    aggStructUseMoveOnlyWithoutEscaping(x2)
}

public func aggStructMultipleNonConsumingUseTest(_ x: AggStruct) {
    @_noImplicitCopy let x2 = x
    aggStructUseMoveOnlyWithoutEscaping(x2)
    aggStructUseMoveOnlyWithoutEscaping(x2)
    print(x2)
}

public func aggStructMultipleNonConsumingUseTestArg(@_noImplicitCopy _ x2: AggStruct) { // expected-error {{'x2' is borrowed and cannot be consumed}}
    aggStructUseMoveOnlyWithoutEscaping(x2)
    aggStructUseMoveOnlyWithoutEscaping(x2)
    print(x2) // expected-note {{consumed here}}
}

public func aggStructMultipleNonConsumingUseTestOwnedArg(@_noImplicitCopy _ x2: __owned AggStruct) {
    aggStructUseMoveOnlyWithoutEscaping(x2)
    aggStructUseMoveOnlyWithoutEscaping(x2)
    print(x2)
}

public func aggStructUseAfterConsume(_ x: AggStruct) {
    @_noImplicitCopy let x2 = x
    // expected-error @-1 {{'x2' consumed more than once}}
    // expected-error @-2 {{'x2' consumed more than once}}
    let y = x2 // expected-note {{consumed here}}
    let _ = y
    let z = x2 // expected-note {{consumed again here}}
    // expected-note @-1 {{consumed here}}
    let _ = z
    print(x2) // expected-note {{consumed again here}}
}

public func aggStructUseAfterConsumeArg(@_noImplicitCopy _ x2: AggStruct) { // expected-error {{'x2' is borrowed and cannot be consumed}}
    let y = x2 // expected-note {{consumed here}}
    let _ = y
    let z = x2 // expected-note {{consumed here}}
    let _ = z
    print(x2) // expected-note {{consumed here}}
}


public func aggStructUseAfterConsumeOwnedArg(@_noImplicitCopy _ x2: __owned AggStruct) {
    // expected-error @-1 {{'x2' consumed more than once}}
    // expected-error @-2 {{'x2' consumed more than once}}
    let y = x2 // expected-note {{consumed here}}
    let _ = y
    let z = x2 // expected-note {{consumed again here}}
    // expected-note @-1 {{consumed here}}
    let _ = z
    print(x2) // expected-note {{consumed again here}}
}

public func aggStructDoubleConsume(_ x: AggStruct) {
    @_noImplicitCopy let x2 = x  // expected-error {{'x2' consumed more than once}}
    let y = x2 // expected-note {{consumed here}}
    let _ = y
    let z = x2 // expected-note {{consumed again here}}
    let _ = z
}

public func aggStructDoubleConsumeArg(@_noImplicitCopy _ x2: AggStruct) { // expected-error {{'x2' is borrowed and cannot be consumed}}
    let y = x2 // expected-note {{consumed here}}
    let _ = y
    let z = x2 // expected-note {{consumed here}}
    let _ = z
}

public func aggStructDoubleConsumeOwnedArg(@_noImplicitCopy _ x2: __owned AggStruct) { // expected-error {{'x2' consumed more than once}}
    let y = x2 // expected-note {{consumed here}}
    let _ = y
    let z = x2 // expected-note {{consumed again here}}
    let _ = z
}

public func aggStructLoopConsume(_ x: AggStruct) {
    @_noImplicitCopy let x2 = x // expected-error {{'x2' consumed in a loop}}
    for _ in 0..<1024 {
        let y = x2 // expected-note {{consumed in loop here}}
        let _ = y
    }
}

public func aggStructLoopConsumeArg(@_noImplicitCopy _ x2: AggStruct) { // expected-error {{'x2' is borrowed and cannot be consumed}}
    for _ in 0..<1024 {
        let y = x2 // expected-note {{consumed here}}
        let _ = y
    }
}

public func aggStructLoopConsumeOwnedArg(@_noImplicitCopy _ x2: __owned AggStruct) { // expected-error {{'x2' consumed in a loop}}
    for _ in 0..<1024 {
        let y = x2 // expected-note {{consumed in loop here}}
        let _ = y
    }
}

public func aggStructDiamond(_ x: AggStruct) {
    @_noImplicitCopy let x2 = x
    if boolValue {
        let y = x2
        let _ = y
    } else {
        let y = x2
        let _ = y
    }
}

public func aggStructDiamondArg(@_noImplicitCopy _ x2: AggStruct) { // expected-error {{'x2' is borrowed and cannot be consumed}}
    if boolValue {
        let y = x2 // expected-note {{consumed here}}
        let _ = y
    } else {
        let y = x2 // expected-note {{consumed here}}
        let _ = y
    }
}

public func aggStructDiamondOwnedArg(@_noImplicitCopy _ x2: __owned AggStruct) {
    if boolValue {
        let y = x2
        let _ = y
    } else {
        let y = x2
        let _ = y
    }
}

public func aggStructDiamondInLoop(_ x: AggStruct) {
    @_noImplicitCopy let x2 = x
    // expected-error @-1 {{'x2' consumed in a loop}}
    // expected-error @-2 {{'x2' consumed more than once}}
    // expected-error @-3 {{'x2' consumed more than once}}
    // expected-error @-4 {{'x2' consumed more than once}}
    for _ in 0..<1024 {
        if boolValue {
            let y = x2 // expected-note {{consumed here}}
            let _ = y
            aggStructConsume(x2) // expected-note {{consumed here}}
            // expected-note @-1 {{consumed again here}}
        } else {
            let y = x2 // expected-note {{consumed here}}
            // expected-note @-1 {{consumed again here}}
            let _ = y
            aggStructConsume(x2) // expected-note {{consumed again here}}
            // expected-note @-1 {{consumed in loop here}}
        }
    }
}

public func aggStructDiamondInLoopArg(@_noImplicitCopy _ x2: AggStruct) { // expected-error {{'x2' is borrowed and cannot be consumed}}
    for _ in 0..<1024 {
        if boolValue {
            let y = x2 // expected-note {{consumed here}}
            let _ = y

            aggStructConsume(x2) // expected-note {{consumed here}}
        } else {
            let y = x2 // expected-note {{consumed here}}
            let _ = y
            aggStructConsume(x2) // expected-note {{consumed here}}
        }
    }
}

public func aggStructDiamondInLoopOwnedArg(@_noImplicitCopy _ x2: __owned AggStruct) {
    // expected-error @-1 {{'x2' consumed more than once}}
    // expected-error @-2 {{'x2' consumed more than once}}
    // expected-error @-3 {{'x2' consumed more than once}}
    // expected-error @-4 {{'x2' consumed in a loop}}
    for _ in 0..<1024 {
        if boolValue {
            let y = x2 // expected-note {{consumed here}}
            let _ = y
            aggStructConsume(x2)
            // expected-note @-1 {{consumed here}}
            // expected-note @-2 {{consumed again here}}
        } else {
            let y = x2
            // expected-note @-1 {{consumed here}}
            // expected-note @-2 {{consumed again here}}
            let _ = y
            aggStructConsume(x2)
            // expected-note @-1 {{consumed in loop here}}
            // expected-note @-2 {{consumed again here}}
        }
    }
}

public func aggStructAccessField(_ x: AggStruct) {
    @_noImplicitCopy let x2 = x
    print(x2.lhs)
    for _ in 0..<1024 {
        print(x2.lhs)
    }
}

public func aggStructAccessFieldArg(@_noImplicitCopy _ x2: AggStruct) {
    print(x2.lhs)
    for _ in 0..<1024 {
        print(x2.lhs)
    }
}

public func aggStructAccessFieldOwnedArg(@_noImplicitCopy _ x2: __owned AggStruct) {
    print(x2.lhs)
    for _ in 0..<1024 {
        print(x2.lhs)
    }
}

//////////////////////////////
// Aggregate Generic Struct //
//////////////////////////////

public struct AggGenericStruct<T> {
    var lhs: Trivial
    var rhs: Builtin.RawPointer
}

public func aggGenericStructUseMoveOnlyWithoutEscaping(_ x: AggGenericStruct<Trivial>) {
}
public func aggGenericStructConsume(_ x: __owned AggGenericStruct<Trivial>) {
}

public func aggGenericStructSimpleChainTest(_ x: AggGenericStruct<Trivial>) {
    @_noImplicitCopy let x2 = x
    let y2 = x2
    let k2 = y2
    aggGenericStructUseMoveOnlyWithoutEscaping(k2)
}

public func aggGenericStructSimpleChainTestArg(@_noImplicitCopy _ x2: AggGenericStruct<Trivial>) { // expected-error {{'x2' is borrowed and cannot be consumed}}
    let y2 = x2 // expected-note {{consumed here}}
    let k2 = y2
    aggGenericStructUseMoveOnlyWithoutEscaping(k2)
}

public func aggGenericStructSimpleChainTestOwnedArg(@_noImplicitCopy _ x2: __owned AggGenericStruct<Trivial>) {
    let y2 = x2
    let k2 = y2
    aggGenericStructUseMoveOnlyWithoutEscaping(k2)
}

public func aggGenericStructSimpleNonConsumingUseTest(_ x: AggGenericStruct<Trivial>) {
    @_noImplicitCopy let x2 = x
    aggGenericStructUseMoveOnlyWithoutEscaping(x2)
}

public func aggGenericStructSimpleNonConsumingUseTestArg(@_noImplicitCopy _ x2: AggGenericStruct<Trivial>) {
    aggGenericStructUseMoveOnlyWithoutEscaping(x2)
}

public func aggGenericStructSimpleNonConsumingUseTestOwnedArg(@_noImplicitCopy _ x2: __owned AggGenericStruct<Trivial>) {
    aggGenericStructUseMoveOnlyWithoutEscaping(x2)
}

public func aggGenericStructMultipleNonConsumingUseTest(_ x: AggGenericStruct<Trivial>) {
    @_noImplicitCopy let x2 = x
    aggGenericStructUseMoveOnlyWithoutEscaping(x2)
    aggGenericStructUseMoveOnlyWithoutEscaping(x2)
    print(x2)
}

public func aggGenericStructMultipleNonConsumingUseTestArg(@_noImplicitCopy _ x2: AggGenericStruct<Trivial>) { // expected-error {{'x2' is borrowed and cannot be consumed}}
    aggGenericStructUseMoveOnlyWithoutEscaping(x2)
    aggGenericStructUseMoveOnlyWithoutEscaping(x2)
    print(x2) // expected-note {{consumed here}}
}

public func aggGenericStructMultipleNonConsumingUseTestOwnedArg(@_noImplicitCopy _ x2: __owned AggGenericStruct<Trivial>) {
    aggGenericStructUseMoveOnlyWithoutEscaping(x2)
    aggGenericStructUseMoveOnlyWithoutEscaping(x2)
    print(x2)
}

public func aggGenericStructUseAfterConsume(_ x: AggGenericStruct<Trivial>) {
    @_noImplicitCopy let x2 = x
    // expected-error @-1 {{'x2' consumed more than once}}
    // expected-error @-2 {{'x2' consumed more than once}}
    aggGenericStructUseMoveOnlyWithoutEscaping(x2)
    let y = x2  // expected-note {{consumed here}}
    let _ = y
    aggGenericStructConsume(x2) // expected-note {{consumed here}}
    // expected-note @-1 {{consumed again here}}
    print(x2)
    // expected-note @-1 {{consumed again here}}
}

public func aggGenericStructUseAfterConsumeArg(@_noImplicitCopy _ x2: AggGenericStruct<Trivial>) { // expected-error {{'x2' is borrowed and cannot be consumed}}
    aggGenericStructUseMoveOnlyWithoutEscaping(x2)
    let y = x2  // expected-note {{consumed here}}
    let _ = y
    aggGenericStructConsume(x2) // expected-note {{consumed here}}
    print(x2) // expected-note {{consumed here}}
}

public func aggGenericStructUseAfterConsumeOwnedArg(@_noImplicitCopy _ x2: __owned AggGenericStruct<Trivial>) {
    // expected-error @-1 {{'x2' consumed more than once}}
    // expected-error @-2 {{'x2' consumed more than once}}
    aggGenericStructUseMoveOnlyWithoutEscaping(x2)
    let y = x2  // expected-note {{consumed here}}
    let _ = y
    aggGenericStructConsume(x2) // expected-note {{consumed here}}
    // expected-note @-1 {{consumed again here}}
    print(x2)
    // expected-note @-1 {{consumed again here}}
}

public func aggGenericStructDoubleConsume(_ x: AggGenericStruct<Trivial>) {
    @_noImplicitCopy let x2 = x  // expected-error {{'x2' consumed more than once}}
    let y = x2  // expected-note {{consumed here}}
    let _ = y
    let z = x2  // expected-note {{consumed again here}}
    let _ = z
}

public func aggGenericStructDoubleConsumeArg(@_noImplicitCopy _ x2: AggGenericStruct<Trivial>) { // expected-error {{'x2' is borrowed and cannot be consumed}}
    let y = x2  // expected-note {{consumed here}}
    let _ = y
    let z = x2  // expected-note {{consumed here}}
    let _ = z
}

public func aggGenericStructDoubleConsumeOwnedArg(@_noImplicitCopy _ x2: __owned AggGenericStruct<Trivial>) { // expected-error {{'x2' consumed more than once}}
    let y = x2  // expected-note {{consumed here}}
    let _ = y
    let z = x2  // expected-note {{consumed again here}}
    let _ = z
}

public func aggGenericStructLoopConsume(_ x: AggGenericStruct<Trivial>) {
    @_noImplicitCopy let x2 = x // expected-error {{'x2' consumed in a loop}}
    for _ in 0..<1024 {
        let y = x2  // expected-note {{consumed in loop here}}
        let _ = y
    }
}

public func aggGenericStructLoopConsumeArg(@_noImplicitCopy _ x2: AggGenericStruct<Trivial>) { // expected-error {{'x2' is borrowed and cannot be consumed}}
    for _ in 0..<1024 {
        let y = x2  // expected-note {{consumed here}}
        let _ = y
    }
}

public func aggGenericStructLoopConsumeOwnedArg(@_noImplicitCopy _ x2: __owned AggGenericStruct<Trivial>) { // expected-error {{'x2' consumed in a loop}}
    for _ in 0..<1024 {
        let y = x2  // expected-note {{consumed in loop here}}
        let _ = y
    }
}

public func aggGenericStructDiamond(_ x: AggGenericStruct<Trivial>) {
    @_noImplicitCopy let x2 = x // expected-error {{'x2' consumed more than once}}
    if boolValue {
        let y = x2 // expected-note {{consumed here}}
        let _ = y
        aggGenericStructConsume(x2) // expected-note {{consumed again here}}
    } else {
        let z = x2
        let _ = z
    }
}

public func aggGenericStructDiamondArg(@_noImplicitCopy _ x2: AggGenericStruct<Trivial>) { // expected-error {{'x2' is borrowed and cannot be consumed}}
    if boolValue {
        let y = x2 // expected-note {{consumed here}}
        let _ = y
        aggGenericStructConsume(x2) // expected-note {{consumed here}}
    } else {
        let z = x2 // expected-note {{consumed here}}
        let _ = z
    }
}

public func aggGenericStructDiamondOwnedArg(@_noImplicitCopy _ x2: __owned AggGenericStruct<Trivial>) {
    // expected-error @-1 {{'x2' consumed more than once}}
    if boolValue {
        let y = x2 // expected-note {{consumed here}}
        let _ = y
        aggGenericStructConsume(x2) // expected-note {{consumed again here}}
    } else {
        let z = x2
        let _ = z
    }
}

public func aggGenericStructDiamondInLoop(_ x: AggGenericStruct<Trivial>) {
    @_noImplicitCopy let x2 = x // expected-error {{'x2' consumed more than once}}
    // expected-error @-1 {{'x2' consumed in a loop}}
    for _ in 0..<1024 {
        if boolValue {
            let y = x2 // expected-note {{consumed here}}
            let _ = y
        } else {
            let y = x2 // expected-note {{consumed again here}}
            // expected-note @-1 {{consumed in loop here}}
            let _ = y
        }
    }
}

public func aggGenericStructDiamondInLoopArg(@_noImplicitCopy _ x2: AggGenericStruct<Trivial>) { // expected-error {{'x2' is borrowed and cannot be consumed}}
    for _ in 0..<1024 {
        if boolValue {
            let y = x2 // expected-note {{consumed here}}
            let _ = y
        } else {
            let y = x2 // expected-note {{consumed here}}
            let _ = y
        }
    }
}

public func aggGenericStructDiamondInLoopOwnedArg(@_noImplicitCopy _ x2: __owned AggGenericStruct<Trivial>) {
    // expected-error @-1 {{'x2' consumed more than once}}
    // expected-error @-2 {{'x2' consumed in a loop}}
    for _ in 0..<1024 {
        if boolValue {
            let y = x2 // expected-note {{consumed here}}
            let _ = y
        } else {
            let y = x2 // expected-note {{consumed again here}}
            // expected-note @-1 {{consumed in loop here}}
            let _ = y
        }
    }
}

public func aggGenericStructAccessField(_ x: AggGenericStruct<Trivial>) {
    @_noImplicitCopy let x2 = x
    print(x2.lhs)
    for _ in 0..<1024 {
        print(x2.lhs)
    }
}

public func aggGenericStructAccessFieldArg(@_noImplicitCopy _ x2: AggGenericStruct<Trivial>) {
    print(x2.lhs)
    for _ in 0..<1024 {
        print(x2.lhs)
    }
}

public func aggGenericStructAccessFieldOwnedArg(@_noImplicitCopy _ x2: __owned AggGenericStruct<Trivial>) {
    print(x2.lhs)
    for _ in 0..<1024 {
        print(x2.lhs)
    }
}

////////////////////////////////////////////////////////////
// Aggregate Generic Struct + Generic But Body is Trivial //
////////////////////////////////////////////////////////////

public func aggGenericStructUseMoveOnlyWithoutEscaping<T>(_ x: AggGenericStruct<T>) {
}
public func aggGenericStructConsume<T>(_ x: __owned AggGenericStruct<T>) {
}

public func aggGenericStructSimpleChainTest<T>(_ x: AggGenericStruct<T>) {
    @_noImplicitCopy let x2 = x
    let y2 = x2
    let k2 = y2
    aggGenericStructUseMoveOnlyWithoutEscaping(k2)
}

public func aggGenericStructSimpleChainTestArg<T>(@_noImplicitCopy _ x2: AggGenericStruct<T>) { // expected-error {{'x2' is borrowed and cannot be consumed}}
    let y2 = x2 // expected-note {{consumed here}}
    let k2 = y2
    aggGenericStructUseMoveOnlyWithoutEscaping(k2)
}

public func aggGenericStructSimpleChainTestOwnedArg<T>(@_noImplicitCopy _ x2: __owned AggGenericStruct<T>) {
    let y2 = x2
    let k2 = y2
    aggGenericStructUseMoveOnlyWithoutEscaping(k2)
}

public func aggGenericStructSimpleNonConsumingUseTest<T>(_ x: AggGenericStruct<T>) {
    @_noImplicitCopy let x2 = x
    aggGenericStructUseMoveOnlyWithoutEscaping(x2)
}

public func aggGenericStructSimpleNonConsumingUseTestArg<T>(@_noImplicitCopy _ x2: AggGenericStruct<T>) {
    aggGenericStructUseMoveOnlyWithoutEscaping(x2)
}

public func aggGenericStructSimpleNonConsumingUseTestOwnedArg<T>(@_noImplicitCopy _ x2: __owned AggGenericStruct<T>) {
    aggGenericStructUseMoveOnlyWithoutEscaping(x2)
}

public func aggGenericStructMultipleNonConsumingUseTest<T>(_ x: AggGenericStruct<T>) {
    @_noImplicitCopy let x2 = x
    aggGenericStructUseMoveOnlyWithoutEscaping(x2)
    aggGenericStructUseMoveOnlyWithoutEscaping(x2)
    print(x2)
}

public func aggGenericStructMultipleNonConsumingUseTestArg<T>(@_noImplicitCopy _ x2: AggGenericStruct<T>) { // expected-error {{'x2' is borrowed and cannot be consumed}}
    aggGenericStructUseMoveOnlyWithoutEscaping(x2)
    aggGenericStructUseMoveOnlyWithoutEscaping(x2)
    print(x2) // expected-note {{consumed here}}
}

public func aggGenericStructMultipleNonConsumingUseTestOwnedArg<T>(@_noImplicitCopy _ x2: __owned AggGenericStruct<T>) {
    aggGenericStructUseMoveOnlyWithoutEscaping(x2)
    aggGenericStructUseMoveOnlyWithoutEscaping(x2)
    print(x2)
}

public func aggGenericStructUseAfterConsume<T>(_ x: AggGenericStruct<T>) {
    @_noImplicitCopy let x2 = x
    // expected-error @-1 {{'x2' used after consume}}
    // expected-error @-2 {{'x2' consumed more than once}}
    // expected-error @-3 {{'x2' consumed more than once}}
    aggGenericStructUseMoveOnlyWithoutEscaping(x2)
    let y = x2 // expected-note {{consumed here}}
    let _ = y
    aggGenericStructConsume(x2) // expected-note {{consumed here}}
    // expected-note @-1 {{consumed again here}}
    print(x2) // expected-note {{consumed here}}
    // expected-note @-1 {{consumed again here}}
    aggGenericStructUseMoveOnlyWithoutEscaping(x2)
    // expected-note @-1 {{used here}}
}

public func aggGenericStructUseAfterConsumeArg<T>(@_noImplicitCopy _ x2: AggGenericStruct<T>) { // expected-error {{'x2' is borrowed and cannot be consumed}}
    aggGenericStructUseMoveOnlyWithoutEscaping(x2)
    let y = x2 // expected-note {{consumed here}}
    let _ = y
    aggGenericStructConsume(x2) // expected-note {{consumed here}}
    print(x2) // expected-note {{consumed here}}
}

public func aggGenericStructUseAfterConsumeOwnedArg<T>(@_noImplicitCopy _ x2: __owned AggGenericStruct<T>) {
    // expected-error @-1 {{'x2' consumed more than once}}
    // expected-error @-2 {{'x2' consumed more than once}}
    aggGenericStructUseMoveOnlyWithoutEscaping(x2)
    let y = x2 // expected-note {{consumed here}}
    let _ = y
    aggGenericStructConsume(x2) // expected-note {{consumed again here}}
    // expected-note @-1 {{consumed here}}
    print(x2) // expected-note {{consumed again here}}
}

public func aggGenericStructDoubleConsume<T>(_ x: AggGenericStruct<T>) {
    @_noImplicitCopy let x2 = x  // expected-error {{'x2' consumed more than once}}
    let y = x2 // expected-note {{consumed here}}
    let _ = y
    let z = x2 // expected-note {{consumed again here}}
    let _ = z
}

public func aggGenericStructDoubleConsumeArg<T>(@_noImplicitCopy _ x2: AggGenericStruct<T>) { // expected-error {{'x2' is borrowed and cannot be consumed}}
    let y = x2 // expected-note {{consumed here}}
    let _ = y
    let z = x2 // expected-note {{consumed here}}
    let _ = z
}

public func aggGenericStructDoubleConsumeOwnedArg<T>(@_noImplicitCopy _ x2: __owned AggGenericStruct<T>) { // expected-error {{'x2' consumed more than once}}
    let y = x2 // expected-note {{consumed here}}
    let _ = y
    let z = x2 // expected-note {{consumed again here}}
    let _ = z
}

public func aggGenericStructLoopConsume<T>(_ x: AggGenericStruct<T>) {
    @_noImplicitCopy let x2 = x // expected-error {{'x2' consumed in a loop}}
    for _ in 0..<1024 {
        let z = x2 // expected-note {{consumed in loop here}}
        let _ = z
    }
}

public func aggGenericStructLoopConsumeArg<T>(@_noImplicitCopy _ x2: AggGenericStruct<T>) { // expected-error {{'x2' is borrowed and cannot be consumed}}
    for _ in 0..<1024 {
        let z = x2 // expected-note {{consumed here}}
        let _ = z
    }
}

public func aggGenericStructLoopConsumeOwnedArg<T>(@_noImplicitCopy _ x2: __owned AggGenericStruct<T>) { // expected-error {{'x2' consumed in a loop}}
    for _ in 0..<1024 {
        let z = x2 // expected-note {{consumed in loop here}}
        let _ = z
    }
}

public func aggGenericStructDiamond<T>(_ x: AggGenericStruct<T>) {
    @_noImplicitCopy let x2 = x
    if boolValue {
        let z = x2
        let _ = z
    } else {
        let z = x2
        let _ = z
    }
}

public func aggGenericStructDiamondArg<T>(@_noImplicitCopy _ x2: AggGenericStruct<T>) { // expected-error {{'x2' is borrowed and cannot be consumed}}
    if boolValue {
        let z = x2 // expected-note {{consumed here}}
        let _ = z
    } else {
        let z = x2 // expected-note {{consumed here}}
        let _ = z
    }
}

public func aggGenericStructDiamondOwnedArg<T>(@_noImplicitCopy _ x2: __owned AggGenericStruct<T>) {
    if boolValue {
        let z = x2
        let _ = z
    } else {
        let z = x2
        let _ = z
    }
}

public func aggGenericStructDiamondInLoop<T>(_ x: AggGenericStruct<T>) {
    @_noImplicitCopy let x2 = x // expected-error {{'x2' consumed more than once}}
    // expected-error @-1 {{'x2' consumed in a loop}}
    for _ in 0..<1024 {
        if boolValue {
            let z = x2 // expected-note {{consumed here}}
            let _ = z
        } else {
            let y = x2 // expected-note {{consumed again here}}
            // expected-note @-1 {{consumed in loop here}}
            let _ = y
        }
    }
}

public func aggGenericStructDiamondInLoopArg<T>(@_noImplicitCopy _ x2: AggGenericStruct<T>) { // expected-error {{'x2' is borrowed and cannot be consumed}}
    for _ in 0..<1024 {
        if boolValue {
            let z = x2 // expected-note {{consumed here}}
            let _ = z
        } else {
            let y = x2 // expected-note {{consumed here}}
            let _ = y
        }
    }
}

public func aggGenericStructDiamondInLoopOwnedArg<T>(@_noImplicitCopy _ x2: __owned AggGenericStruct<T>) {
    // expected-error @-1 {{'x2' consumed more than once}}
    // expected-error @-2 {{'x2' consumed in a loop}}
    for _ in 0..<1024 {
        if boolValue {
            let z = x2 // expected-note {{consumed here}}
            let _ = z
        } else {
            let y = x2 // expected-note {{consumed again here}}
            // expected-note @-1 {{consumed in loop here}}
            let _ = y
        }
    }
}

public func aggGenericStructAccessField<T>(_ x: AggGenericStruct<T>) {
    @_noImplicitCopy let x2 = x
    print(x2.lhs)
    for _ in 0..<1024 {
        print(x2.lhs)
    }
}

public func aggGenericStructAccessFieldArg<T>(@_noImplicitCopy _ x2: AggGenericStruct<T>) {
    print(x2.lhs)
    for _ in 0..<1024 {
        print(x2.lhs)
    }
}

public func aggGenericStructAccessFieldOwnedArg<T>(@_noImplicitCopy _ x2: __owned AggGenericStruct<T>) {
    print(x2.lhs)
    for _ in 0..<1024 {
        print(x2.lhs)
    }
}

///////////////////
// Return Values //
///////////////////

public func noImplicitCopyArgReturn(@_noImplicitCopy _ x: Trivial) -> Trivial { // expected-error {{'x' is borrowed and cannot be consumed}}
    return x // expected-note {{consumed here}}
}

public func noImplicitCopyArgReturnWithAssign(@_noImplicitCopy _ x: Trivial) -> Trivial { // expected-error {{'x' is borrowed and cannot be consumed}}
    let y = x // expected-note {{consumed here}}
    print(y)
    return x // expected-note {{consumed here}}
}

public func noImplicitCopyReturn(_ x: Int) -> Int {
    @_noImplicitCopy let y = x
    return y
}

public func noImplicitCopyReturnUse(_ x: Int) -> Int {
    @_noImplicitCopy let y = x // expected-error {{'y' consumed more than once}}
    let z = y // expected-note {{consumed here}}
    let _ = z
    return y // expected-note {{consumed again here}}
}

func takeClosure(_ f: ()->Int) -> Int { f() }

public func test1(i: consuming Int) -> Int {
  takeClosure { [i = copy i] in i }
}

public func test2(i: borrowing Int) -> Int {
  takeClosure { [i = copy i] in i }
}

public func test3(i: consuming Int) -> Int {
  takeClosure { i }
}

// TODO: incorrect diagnostic:
//       error: 'i' cannot be captured by an escaping closure since it is a borrowed parameter
// public func test4(i: borrowing Int) -> Int {
//  takeClosure { i }
// }
