// RUN: %target-swift-emit-sil -sil-verify-all -verify %s

//////////////////
// Declarations //
//////////////////

public class CopyableKlass {}

var boolValue: Bool { return true }

@_moveOnly
public struct NonTrivialStruct {
    var i: Int = 0
}

public func borrowVal(_ x: borrowing Int) {}
public func borrowVal(_ x: borrowing AggStruct) {}
public func borrowVal(_ x: borrowing NonTrivialStruct) {}
public func borrowVal(_ x: borrowing AggGenericStruct<CopyableKlass>) {}
public func borrowVal<T>(_ x: borrowing AggGenericStruct<T>) {}
public func borrowVal(_ x: borrowing EnumTy) {}

public func consumeVal(_ x: __owned Int) {}
public func consumeVal(_ x: __owned NonTrivialStruct) {}
public func consumeVal(_ x: __owned String) {}
public func consumeVal(_ x: __owned EnumTy) {}
public func consumeVal<T>(_ x: __owned AggGenericStruct<T>) {}
public func consumeVal(_ x: __owned AggStruct) {}
public func consumeVal(_ x: __owned AggGenericStruct<CopyableKlass>) {}


@_moveOnly
public enum NonTrivialEnum {
    case first
    case second((Int, Int))
    case third(NonTrivialStruct)
}

///////////
// Tests //
///////////

//////////////////////
// Aggregate Struct //
//////////////////////

@_moveOnly
public struct MOIntPair {
    var lhs: Int
    var rhs: Int
}

@_moveOnly
public struct AggStruct {
    var lhs: Int
    var center: Int
    var rhs: Int
    var pair: MOIntPair
}

public func aggStructSimpleChainTest(_ x: borrowing AggStruct) { // expected-error {{'x' is borrowed and cannot be consumed}}
    let x2 = x // expected-note {{consumed here}}
    let y2 = x2
    let k2 = y2
    borrowVal(k2)
}

public func aggStructSimpleChainTestArg(_ x2: borrowing AggStruct) { // expected-error {{'x2' is borrowed and cannot be consumed}}
    let y2 = x2 // expected-note {{consumed here}}
    let k2 = y2
    borrowVal(k2)
}

public func aggStructSimpleChainTestOwnedArg(_ x2: __owned AggStruct) {
    let y2 = x2
    let k2 = y2
    borrowVal(k2)
}

public func aggStructSimpleChainTestOwnedArg2(_ x2: consuming AggStruct) {
    let y2 = x2
    let k2 = y2
    borrowVal(k2)
}

public func aggStructSimpleNonConsumingUseTest(_ x: borrowing AggStruct) { // expected-error {{'x' is borrowed and cannot be consumed}}
    let x2 = x // expected-note {{consumed here}}
    borrowVal(x2)
}

public func aggStructSimpleNonConsumingUseTestArg(_ x2: borrowing AggStruct) {
    borrowVal(x2)
}

public func aggStructSimpleNonConsumingUseTestOwnedArg(_ x2: __owned AggStruct) {
    borrowVal(x2)
}

public func aggStructSimpleNonConsumingUseTestOwnedArg2(_ x2: consuming AggStruct) {
    borrowVal(x2)
}

public func aggStructMultipleNonConsumingUseTest(_ x: borrowing AggStruct) { // expected-error {{'x' is borrowed and cannot be consumed}}
    let x2 = x // expected-note {{consumed here}}
    borrowVal(x2)
    borrowVal(x2)
    consumeVal(x2)
}

public func aggStructMultipleNonConsumingUseTestArg(_ x2: borrowing AggStruct) { // expected-error {{'x2' is borrowed and cannot be consumed}}
    borrowVal(x2)
    borrowVal(x2)
    consumeVal(x2) // expected-note {{consumed here}}
}

public func aggStructMultipleNonConsumingUseTestOwnedArg(_ x2: __owned AggStruct) {
    borrowVal(x2)
    borrowVal(x2)
    consumeVal(x2)
}

public func aggStructMultipleNonConsumingUseTestOwnedArg2(_ x2: consuming AggStruct) {
    borrowVal(x2)
    borrowVal(x2)
    consumeVal(x2)
}

public func aggStructUseAfterConsume(_ x: borrowing AggStruct) { // expected-error {{'x' is borrowed and cannot be consumed}}
    let x2 = x // expected-error {{'x2' consumed more than once}}
               // expected-note @-1 {{consumed here}}
    borrowVal(x2)
    consumeVal(x2) // expected-note {{consumed here}}
    consumeVal(x2) // expected-note {{consumed again here}}
}

public func aggStructUseAfterConsumeArg(_ x2: borrowing AggStruct) { // expected-error {{'x2' is borrowed and cannot be consumed}}
    borrowVal(x2)
    consumeVal(x2) // expected-note {{consumed here}}
    consumeVal(x2) // expected-note {{consumed here}}
}

public func aggStructUseAfterConsumeOwnedArg(_ x2: __owned AggStruct) { // expected-error {{'x2' consumed more than once}}
    borrowVal(x2)
    consumeVal(x2) // expected-note {{consumed here}}
    consumeVal(x2) // expected-note {{consumed again here}}
}

public func aggStructUseAfterConsumeOwnedArg2(_ x2: consuming AggStruct) { // expected-error {{'x2' consumed more than once}}
    borrowVal(x2)
    consumeVal(x2) // expected-note {{consumed here}}
    consumeVal(x2) // expected-note {{consumed again here}}
}

public func aggStructDoubleConsume(_ x: borrowing AggStruct) { // expected-error {{'x' is borrowed and cannot be consumed}}
    let x2 = x  // expected-error {{'x2' consumed more than once}}
                // expected-note @-1 {{consumed here}}
    consumeVal(x2) // expected-note {{consumed here}}
    consumeVal(x2) // expected-note {{consumed again here}}
}

public func aggStructDoubleConsumeArg(_ x2: borrowing AggStruct) { // expected-error {{'x2' is borrowed and cannot be consumed}}
    consumeVal(x2) // expected-note {{consumed here}}
    consumeVal(x2) // expected-note {{consumed here}}
}

public func aggStructDoubleConsumeOwnedArg(_ x2: __owned AggStruct) { // expected-error {{'x2' consumed more than once}}
    consumeVal(x2) // expected-note {{consumed here}}
    consumeVal(x2) // expected-note {{consumed again here}}
}

public func aggStructDoubleConsumeOwnedArg2(_ x2: consuming AggStruct) { // expected-error {{'x2' consumed more than once}}
    consumeVal(x2) // expected-note {{consumed here}}
    consumeVal(x2) // expected-note {{consumed again here}}
}

public func aggStructLoopConsume(_ x: borrowing AggStruct) { // expected-error {{'x' is borrowed and cannot be consumed}}
    let x2 = x // expected-error {{'x2' consumed in a loop}}
               // expected-note @-1 {{consumed here}}
    for _ in 0..<1024 {
        consumeVal(x2) // expected-note {{consumed here}}
    }
}

public func aggStructLoopConsumeArg(_ x2: borrowing AggStruct) { // expected-error {{'x2' is borrowed and cannot be consumed}}
    for _ in 0..<1024 {
        consumeVal(x2) // expected-note {{consumed here}}
    }
}

public func aggStructLoopConsumeOwnedArg(_ x2: __owned AggStruct) { // expected-error {{'x2' consumed in a loop}}
    for _ in 0..<1024 {
        consumeVal(x2) // expected-note {{consumed here}}
    }
}

public func aggStructLoopConsumeOwnedArg2(_ x2: consuming AggStruct) { // expected-error {{'x2' consumed in a loop}}
    for _ in 0..<1024 {
        consumeVal(x2) // expected-note {{consumed here}}
    }
}

public func aggStructDiamond(_ x: borrowing AggStruct) { // expected-error {{'x' is borrowed and cannot be consumed}}
    let x2 = x // expected-note {{consumed here}}
    if boolValue {
        consumeVal(x2)
    } else {
        consumeVal(x2)
    }
}

public func aggStructDiamondArg(_ x2: borrowing AggStruct) { // expected-error {{'x2' is borrowed and cannot be consumed}}
    if boolValue {
        consumeVal(x2) // expected-note {{consumed here}}
    } else {
        consumeVal(x2) // expected-note {{consumed here}}
    }
}

public func aggStructDiamondOwnedArg(_ x2: __owned AggStruct) {
    if boolValue {
        consumeVal(x2)
    } else {
        consumeVal(x2)
    }
}

public func aggStructDiamondOwnedArg2(_ x2: consuming AggStruct) {
    if boolValue {
        consumeVal(x2)
    } else {
        consumeVal(x2)
    }
}

public func aggStructDiamondInLoop(_ x: borrowing AggStruct) { // expected-error {{'x' is borrowed and cannot be consumed}}
    let x2 = x // expected-error {{'x2' consumed more than once}}
    // expected-note @-1 {{consumed here}}
    // expected-error @-2 {{'x2' consumed in a loop}}
    for _ in 0..<1024 {
      if boolValue {
          consumeVal(x2) // expected-note {{consumed here}}
      } else {
          consumeVal(x2) // expected-note {{consumed here}}
          // expected-note @-1 {{consumed again here}}
      }
    }
}

public func aggStructDiamondInLoopArg(_ x2: borrowing AggStruct) { // expected-error {{'x2' is borrowed and cannot be consumed}}
    for _ in 0..<1024 {
      if boolValue {
          consumeVal(x2) // expected-note {{consumed here}}
      } else {
          consumeVal(x2) // expected-note {{consumed here}}
      }
    }
}

public func aggStructDiamondInLoopOwnedArg(_ x2: __owned AggStruct) { // expected-error {{'x2' consumed more than once}}
    // expected-error @-1 {{'x2' consumed in a loop}}
    for _ in 0..<1024 {
      if boolValue {
          consumeVal(x2) // expected-note {{consumed here}}
      } else {
          consumeVal(x2) // expected-note {{consumed here}}
          // expected-note @-1 {{consumed again here}}
      }
    }
}

public func aggStructDiamondInLoopOwnedArg2(_ x2: consuming AggStruct) { // expected-error {{'x2' consumed more than once}}
    // expected-error @-1 {{'x2' consumed in a loop}}
    for _ in 0..<1024 {
      if boolValue {
          consumeVal(x2) // expected-note {{consumed here}}
      } else {
          consumeVal(x2) // expected-note {{consumed here}}
          // expected-note @-1 {{consumed again here}}
      }
    }
}

public func aggStructAccessField(_ x: borrowing AggStruct) { // expected-error {{'x' is borrowed and cannot be consumed}}
    let x2 = x // expected-note {{consumed here}}
    borrowVal(x2.lhs)
    for _ in 0..<1024 {
        borrowVal(x2.lhs)
    }
}

public func aggStructAccessFieldArg(_ x2: borrowing AggStruct) {
    borrowVal(x2.lhs)
    for _ in 0..<1024 {
        borrowVal(x2.lhs)
    }
}

public func aggStructAccessFieldOwnedArg(_ x2: __owned AggStruct) {
    borrowVal(x2.lhs)
    for _ in 0..<1024 {
        borrowVal(x2.lhs)
    }
}

public func aggStructAccessFieldOwnedArg2(_ x2: consuming AggStruct) {
    borrowVal(x2.lhs)
    for _ in 0..<1024 {
        borrowVal(x2.lhs)
    }
}

public func aggStructConsumeField(_ x: borrowing AggStruct) { // expected-error {{'x' is borrowed and cannot be consumed}}
    let x2 = x // expected-note {{consumed here}}
    consumeVal(x2.lhs)
    for _ in 0..<1024 {
        consumeVal(x2.lhs)
    }
}

// TODO: We should error here!
public func aggStructConsumeFieldArg(_ x2: borrowing AggStruct) {
    consumeVal(x2.lhs)
    for _ in 0..<1024 {
        consumeVal(x2.lhs)
    }
}

public func aggStructConsumeFieldOwnedArg(_ x2: __owned AggStruct) {
    consumeVal(x2.lhs)
    for _ in 0..<1024 {
        consumeVal(x2.lhs)
    }
}

public func aggStructConsumeFieldOwnedArg2(_ x2: consuming AggStruct) {
    consumeVal(x2.lhs)
    for _ in 0..<1024 {
        consumeVal(x2.lhs)
    }
}

public func aggStructAccessGrandField(_ x: borrowing AggStruct) { // expected-error {{'x' is borrowed and cannot be consumed}}
    let x2 = x // expected-note {{consumed here}}
    borrowVal(x2.pair.lhs)
    for _ in 0..<1024 {
        borrowVal(x2.pair.lhs)
    }
}

public func aggStructAccessGrandFieldArg(_ x2: borrowing AggStruct) {
    borrowVal(x2.pair.lhs)
    for _ in 0..<1024 {
        borrowVal(x2.pair.lhs)
    }
}

public func aggStructAccessGrandFieldOwnedArg(_ x2: __owned AggStruct) {
    borrowVal(x2.pair.lhs)
    for _ in 0..<1024 {
        borrowVal(x2.pair.lhs)
    }
}

public func aggStructAccessGrandFieldOwnedArg2(_ x2: consuming AggStruct) {
    borrowVal(x2.pair.lhs)
    for _ in 0..<1024 {
        borrowVal(x2.pair.lhs)
    }
}

public func aggStructConsumeGrandField(_ x: borrowing AggStruct) { // expected-error {{'x' is borrowed and cannot be consumed}}
    let x2 = x // expected-note {{consumed here}}
    consumeVal(x2.pair.lhs)
    for _ in 0..<1024 {
        consumeVal(x2.pair.lhs)
    }
}

// TODO: This needs to error.
public func aggStructConsumeGrandFieldArg(_ x2: borrowing AggStruct) {
    consumeVal(x2.pair.lhs)
    for _ in 0..<1024 {
        consumeVal(x2.pair.lhs)
    }
}

public func aggStructConsumeGrandFieldOwnedArg(_ x2: __owned AggStruct) {
    consumeVal(x2.pair.lhs)
    for _ in 0..<1024 {
        consumeVal(x2.pair.lhs)
    }
}

public func aggStructConsumeGrandFieldOwnedArg2(_ x2: consuming AggStruct) {
    consumeVal(x2.pair.lhs)
    for _ in 0..<1024 {
        consumeVal(x2.pair.lhs)
    }
}

//////////////////////////////
// Aggregate Generic Struct //
//////////////////////////////

@_moveOnly
public struct AggGenericStruct<T> {
    var lhs: Int
    var rhs: UnsafeRawPointer
    var pair: MOIntPair
}

public func aggGenericStructSimpleChainTest(_ x: borrowing AggGenericStruct<CopyableKlass>) { // expected-error {{'x' is borrowed and cannot be consumed}}
    let x2 = x // expected-note {{consumed here}}
    let y2 = x2
    let k2 = y2
    borrowVal(k2)
}

public func aggGenericStructSimpleChainTestArg(_ x2: borrowing AggGenericStruct<CopyableKlass>) { // expected-error {{'x2' is borrowed and cannot be consumed}}
    let y2 = x2 // expected-note {{consumed here}}
    let k2 = y2
    borrowVal(k2)
}

public func aggGenericStructSimpleChainTestOwnedArg(_ x2: __owned AggGenericStruct<CopyableKlass>) {
    let y2 = x2
    let k2 = y2
    borrowVal(k2)
}

public func aggGenericStructSimpleChainTestOwnedArg2(_ x2: consuming AggGenericStruct<CopyableKlass>) {
    let y2 = x2
    let k2 = y2
    borrowVal(k2)
}

public func aggGenericStructSimpleNonConsumingUseTest(_ x: borrowing AggGenericStruct<CopyableKlass>) { // expected-error {{'x' is borrowed and cannot be consumed}}
    let x2 = x // expected-note {{consumed here}}
    borrowVal(x2)
}

public func aggGenericStructSimpleNonConsumingUseTestArg(_ x2: borrowing AggGenericStruct<CopyableKlass>) {
    borrowVal(x2)
}

public func aggGenericStructSimpleNonConsumingUseTestOwnedArg(_ x2: __owned AggGenericStruct<CopyableKlass>) {
    borrowVal(x2)
}

public func aggGenericStructSimpleNonConsumingUseTestOwnedArg2(_ x2: consuming AggGenericStruct<CopyableKlass>) {
    borrowVal(x2)
}

public func aggGenericStructMultipleNonConsumingUseTest(_ x: borrowing AggGenericStruct<CopyableKlass>) { // expected-error {{'x' is borrowed and cannot be consumed}}
    let x2 = x // expected-note {{consumed here}}
    borrowVal(x2)
    borrowVal(x2)
    consumeVal(x2)
}

public func aggGenericStructMultipleNonConsumingUseTestArg(_ x2: borrowing AggGenericStruct<CopyableKlass>) { // expected-error {{'x2' is borrowed and cannot be consumed}}
    borrowVal(x2)
    borrowVal(x2)
    consumeVal(x2) // expected-note {{consumed here}}
}

public func aggGenericStructMultipleNonConsumingUseTestOwnedArg(_ x2: __owned AggGenericStruct<CopyableKlass>) {
    borrowVal(x2)
    borrowVal(x2)
    consumeVal(x2)
}

public func aggGenericStructMultipleNonConsumingUseTestOwnedArg2(_ x2: consuming AggGenericStruct<CopyableKlass>) {
    borrowVal(x2)
    borrowVal(x2)
    consumeVal(x2)
}

public func aggGenericStructUseAfterConsume(_ x: borrowing AggGenericStruct<CopyableKlass>) { // expected-error {{'x' is borrowed and cannot be consumed}}
    let x2 = x // expected-error {{'x2' consumed more than once}}
               // expected-note @-1 {{consumed here}}
    borrowVal(x2)
    consumeVal(x2) // expected-note {{consumed here}}
    consumeVal(x2) // expected-note {{consumed again here}}
}

public func aggGenericStructUseAfterConsumeArg(_ x2: borrowing AggGenericStruct<CopyableKlass>) { // expected-error {{'x2' is borrowed and cannot be consumed}}
    borrowVal(x2)
    consumeVal(x2) // expected-note {{consumed here}}
    consumeVal(x2) // expected-note {{consumed here}}
}

public func aggGenericStructUseAfterConsumeOwnedArg(_ x2: __owned AggGenericStruct<CopyableKlass>) { // expected-error {{'x2' consumed more than once}}
    borrowVal(x2)
    consumeVal(x2) // expected-note {{consumed here}}
    consumeVal(x2) // expected-note {{consumed again here}}
}

public func aggGenericStructUseAfterConsumeOwnedArg2(_ x2: consuming AggGenericStruct<CopyableKlass>) { // expected-error {{'x2' consumed more than once}}
    borrowVal(x2)
    consumeVal(x2) // expected-note {{consumed here}}
    consumeVal(x2) // expected-note {{consumed again here}}
}

public func aggGenericStructDoubleConsume(_ x: borrowing AggGenericStruct<CopyableKlass>) { // expected-error {{'x' is borrowed and cannot be consumed}}
    let x2 = x  // expected-error {{'x2' consumed more than once}}
                // expected-note @-1 {{consumed here}}
    consumeVal(x2) // expected-note {{consumed here}}
    consumeVal(x2) // expected-note {{consumed again here}}
}

public func aggGenericStructDoubleConsumeArg(_ x2: borrowing AggGenericStruct<CopyableKlass>) { // expected-error {{'x2' is borrowed and cannot be consumed}}
    consumeVal(x2) // expected-note {{consumed here}}
    consumeVal(x2) // expected-note {{consumed here}}
}

public func aggGenericStructDoubleConsumeOwnedArg(_ x2: __owned AggGenericStruct<CopyableKlass>) { // expected-error {{'x2' consumed more than once}}
    consumeVal(x2) // expected-note {{consumed here}}
    consumeVal(x2) // expected-note {{consumed again here}}
}

public func aggGenericStructDoubleConsumeOwnedArg2(_ x2: consuming AggGenericStruct<CopyableKlass>) { // expected-error {{'x2' consumed more than once}}
    consumeVal(x2) // expected-note {{consumed here}}
    consumeVal(x2) // expected-note {{consumed again here}}
}

public func aggGenericStructLoopConsume(_ x: borrowing AggGenericStruct<CopyableKlass>) { // expected-error {{'x' is borrowed and cannot be consumed}}
    let x2 = x // expected-error {{'x2' consumed in a loop}}
               // expected-note @-1 {{consumed here}}
    for _ in 0..<1024 {
        consumeVal(x2) // expected-note {{consumed here}}
    }
}

public func aggGenericStructLoopConsumeArg(_ x2: borrowing AggGenericStruct<CopyableKlass>) { // expected-error {{'x2' is borrowed and cannot be consumed}}
    for _ in 0..<1024 {
        consumeVal(x2) // expected-note {{consumed here}}
    }
}

public func aggGenericStructLoopConsumeOwnedArg(_ x2: __owned AggGenericStruct<CopyableKlass>) { // expected-error {{'x2' consumed in a loop}}
    for _ in 0..<1024 {
        consumeVal(x2) // expected-note {{consumed here}}
    }
}

public func aggGenericStructLoopConsumeOwnedArg2(_ x2: consuming AggGenericStruct<CopyableKlass>) { // expected-error {{'x2' consumed in a loop}}
    for _ in 0..<1024 {
        consumeVal(x2) // expected-note {{consumed here}}
    }
}

public func aggGenericStructDiamond(_ x: borrowing AggGenericStruct<CopyableKlass>) { // expected-error {{'x' is borrowed and cannot be consumed}}
    let x2 = x // expected-note {{consumed here}}
    if boolValue {
        consumeVal(x2)
    } else {
        consumeVal(x2)
    }
}

public func aggGenericStructDiamondArg(_ x2: borrowing AggGenericStruct<CopyableKlass>) { // expected-error {{'x2' is borrowed and cannot be consumed}}
    if boolValue {
        consumeVal(x2) // expected-note {{consumed here}}
    } else {
        consumeVal(x2) // expected-note {{consumed here}}
    }
}

public func aggGenericStructDiamondOwnedArg(_ x2: __owned AggGenericStruct<CopyableKlass>) {
    if boolValue {
        consumeVal(x2)
    } else {
        consumeVal(x2)
    }
}

public func aggGenericStructDiamondOwnedArg2(_ x2: consuming AggGenericStruct<CopyableKlass>) {
    if boolValue {
        consumeVal(x2)
    } else {
        consumeVal(x2)
    }
}

public func aggGenericStructDiamondInLoop(_ x: borrowing AggGenericStruct<CopyableKlass>) { // expected-error {{'x' is borrowed and cannot be consumed}}
    let x2 = x // expected-error {{'x2' consumed more than once}}
    // expected-note @-1 {{consumed here}}
    // expected-error @-2 {{'x2' consumed in a loop}}
    for _ in 0..<1024 {
      if boolValue {
          consumeVal(x2) // expected-note {{consumed here}}
      } else {
          consumeVal(x2) // expected-note {{consumed here}}
          // expected-note @-1 {{consumed again here}}
      }
    }
}

public func aggGenericStructDiamondInLoopArg(_ x2: borrowing AggGenericStruct<CopyableKlass>) { // expected-error {{'x2' is borrowed and cannot be consumed}}
    for _ in 0..<1024 {
      if boolValue {
          consumeVal(x2) // expected-note {{consumed here}}
      } else {
          consumeVal(x2) // expected-note {{consumed here}}
      }
    }
}

public func aggGenericStructDiamondInLoopOwnedArg(_ x2: __owned AggGenericStruct<CopyableKlass>) { // expected-error {{'x2' consumed more than once}}
    // expected-error @-1 {{'x2' consumed in a loop}}
    for _ in 0..<1024 {
      if boolValue {
          consumeVal(x2) // expected-note {{consumed here}}
      } else {
          consumeVal(x2) // expected-note {{consumed here}}
          // expected-note @-1 {{consumed again here}}
      }
    }
}

public func aggGenericStructDiamondInLoopOwnedArg2(_ x2: consuming AggGenericStruct<CopyableKlass>) { // expected-error {{'x2' consumed more than once}}
    // expected-error @-1 {{'x2' consumed in a loop}}
    for _ in 0..<1024 {
      if boolValue {
          consumeVal(x2) // expected-note {{consumed here}}
      } else {
          consumeVal(x2) // expected-note {{consumed here}}
          // expected-note @-1 {{consumed again here}}
      }
    }
}

public func aggGenericStructAccessField(_ x: borrowing AggGenericStruct<CopyableKlass>) { // expected-error {{'x' is borrowed and cannot be consumed}}
    let x2 = x // expected-note {{consumed here}}
    borrowVal(x2.lhs)
    for _ in 0..<1024 {
        borrowVal(x2.lhs)
    }
}

public func aggGenericStructAccessFieldArg(_ x2: borrowing AggGenericStruct<CopyableKlass>) {
    borrowVal(x2.lhs)
    for _ in 0..<1024 {
        borrowVal(x2.lhs)
    }
}

public func aggGenericStructAccessFieldOwnedArg(_ x2: __owned AggGenericStruct<CopyableKlass>) {
    borrowVal(x2.lhs)
    for _ in 0..<1024 {
        borrowVal(x2.lhs)
    }
}

public func aggGenericStructAccessFieldOwnedArg2(_ x2: consuming AggGenericStruct<CopyableKlass>) {
    borrowVal(x2.lhs)
    for _ in 0..<1024 {
        borrowVal(x2.lhs)
    }
}

public func aggGenericStructConsumeField(_ x: borrowing AggGenericStruct<CopyableKlass>) { // expected-error {{'x' is borrowed and cannot be consumed}}
    let x2 = x // expected-note {{consumed here}}
    consumeVal(x2.lhs)
    for _ in 0..<1024 {
        consumeVal(x2.lhs)
    }
}

public func aggGenericStructConsumeFieldArg(_ x2: borrowing AggGenericStruct<CopyableKlass>) {
    consumeVal(x2.lhs)
    for _ in 0..<1024 {
        consumeVal(x2.lhs)
    }
}

public func aggGenericStructConsumeFieldOwnedArg(_ x2: __owned AggGenericStruct<CopyableKlass>) {
    consumeVal(x2.lhs)
    for _ in 0..<1024 {
        consumeVal(x2.lhs)
    }
}

public func aggGenericStructConsumeFieldOwnedArg2(_ x2: consuming AggGenericStruct<CopyableKlass>) {
    consumeVal(x2.lhs)
    for _ in 0..<1024 {
        consumeVal(x2.lhs)
    }
}

public func aggGenericStructAccessGrandField(_ x: borrowing AggGenericStruct<CopyableKlass>) { // expected-error {{'x' is borrowed and cannot be consumed}}
    let x2 = x // expected-note {{consumed here}}
    borrowVal(x2.pair.lhs)
    for _ in 0..<1024 {
        borrowVal(x2.pair.lhs)
    }
}

public func aggGenericStructAccessGrandFieldArg(_ x2: borrowing AggGenericStruct<CopyableKlass>) {
    borrowVal(x2.pair.lhs)
    for _ in 0..<1024 {
        borrowVal(x2.pair.lhs)
    }
}

public func aggGenericStructAccessGrandFieldOwnedArg(_ x2: __owned AggGenericStruct<CopyableKlass>) {
    borrowVal(x2.pair.lhs)
    for _ in 0..<1024 {
        borrowVal(x2.pair.lhs)
    }
}

public func aggGenericStructAccessGrandFieldOwnedArg2(_ x2: consuming AggGenericStruct<CopyableKlass>) {
    borrowVal(x2.pair.lhs)
    for _ in 0..<1024 {
        borrowVal(x2.pair.lhs)
    }
}

public func aggGenericStructConsumeGrandField(_ x: borrowing AggGenericStruct<CopyableKlass>) { // expected-error {{'x' is borrowed and cannot be consumed}}
    let x2 = x // expected-note {{consumed here}}
    consumeVal(x2.pair.lhs)
    for _ in 0..<1024 {
        consumeVal(x2.pair.lhs)
    }
}

public func aggGenericStructConsumeGrandFieldArg(_ x2: borrowing AggGenericStruct<CopyableKlass>) {
    consumeVal(x2.pair.lhs)
    for _ in 0..<1024 {
        consumeVal(x2.pair.lhs)
    }
}

public func aggGenericStructConsumeGrandFieldOwnedArg(_ x2: __owned AggGenericStruct<CopyableKlass>) {
    consumeVal(x2.pair.lhs)
    for _ in 0..<1024 {
        consumeVal(x2.pair.lhs)
    }
}

public func aggGenericStructConsumeGrandFieldOwnedArg2(_ x2: consuming AggGenericStruct<CopyableKlass>) {
    consumeVal(x2.pair.lhs)
    for _ in 0..<1024 {
        consumeVal(x2.pair.lhs)
    }
}

////////////////////////////////////////////////////////////
// Aggregate Generic Struct + Generic But Body is Trivial //
////////////////////////////////////////////////////////////

public func aggGenericStructSimpleChainTest<T>(_ x: borrowing AggGenericStruct<T>) { // expected-error {{'x' is borrowed and cannot be consumed}}
    let x2 = x // expected-note {{consumed here}}
    let y2 = x2
    let k2 = y2
    borrowVal(k2)
}

public func aggGenericStructSimpleChainTestArg<T>(_ x2: borrowing AggGenericStruct<T>) { // expected-error {{'x2' is borrowed and cannot be consumed}}
    let y2 = x2 // expected-note {{consumed here}}
    let k2 = y2
    borrowVal(k2)
}

public func aggGenericStructSimpleChainTestOwnedArg<T>(_ x2: __owned AggGenericStruct<T>) {
    let y2 = x2
    let k2 = y2
    borrowVal(k2)
}

public func aggGenericStructSimpleChainTestOwnedArg2<T>(_ x2: consuming AggGenericStruct<T>) {
    let y2 = x2
    let k2 = y2
    borrowVal(k2)
}

public func aggGenericStructSimpleNonConsumingUseTest<T>(_ x: borrowing AggGenericStruct<T>) { // expected-error {{'x' is borrowed and cannot be consumed}}
    let x2 = x // expected-note {{consumed here}}
    borrowVal(x2)
}

public func aggGenericStructSimpleNonConsumingUseTestArg<T>(_ x2: borrowing AggGenericStruct<T>) {
    borrowVal(x2)
}

public func aggGenericStructSimpleNonConsumingUseTestOwnedArg<T>(_ x2: __owned AggGenericStruct<T>) {
    borrowVal(x2)
}

public func aggGenericStructSimpleNonConsumingUseTestOwnedArg2<T>(_ x2: consuming AggGenericStruct<T>) {
    borrowVal(x2)
}

public func aggGenericStructMultipleNonConsumingUseTest<T>(_ x: borrowing AggGenericStruct<T>) { // expected-error {{'x' is borrowed and cannot be consumed}}
    let x2 = x // expected-note {{consumed here}}
    borrowVal(x2)
    borrowVal(x2)
    consumeVal(x2)
}

public func aggGenericStructMultipleNonConsumingUseTestArg<T>(_ x2: borrowing AggGenericStruct<T>) { //expected-error {{'x2' is borrowed and cannot be consumed}}
    borrowVal(x2)
    borrowVal(x2)
    consumeVal(x2) // expected-note {{consumed here}}
}

public func aggGenericStructMultipleNonConsumingUseTestOwnedArg<T>(_ x2: __owned AggGenericStruct<T>) {
    borrowVal(x2)
    borrowVal(x2)
    consumeVal(x2)
}

public func aggGenericStructMultipleNonConsumingUseTestOwnedArg2<T>(_ x2: consuming AggGenericStruct<T>) {
    borrowVal(x2)
    borrowVal(x2)
    consumeVal(x2)
}

public func aggGenericStructUseAfterConsume<T>(_ x: borrowing AggGenericStruct<T>) { // expected-error {{'x' is borrowed and cannot be consumed}}
    let x2 = x // expected-error {{'x2' consumed more than once}}
               // expected-note @-1 {{consumed here}}
    borrowVal(x2)
    consumeVal(x2) // expected-note {{consumed here}}
    consumeVal(x2) // expected-note {{consumed again here}}
}

public func aggGenericStructUseAfterConsumeArg<T>(_ x2: borrowing AggGenericStruct<T>) { // expected-error {{'x2' is borrowed and cannot be consumed}}
    borrowVal(x2)
    consumeVal(x2) // expected-note {{consumed here}}
    consumeVal(x2) // expected-note {{consumed here}}
}

public func aggGenericStructUseAfterConsumeOwnedArg<T>(_ x2: __owned AggGenericStruct<T>) { // expected-error {{'x2' consumed more than once}}
    borrowVal(x2)
    consumeVal(x2) // expected-note {{consumed here}}
    consumeVal(x2) // expected-note {{consumed again here}}
}

public func aggGenericStructUseAfterConsumeOwnedArg2<T>(_ x2: consuming AggGenericStruct<T>) { // expected-error {{'x2' consumed more than once}}
    borrowVal(x2)
    consumeVal(x2) // expected-note {{consumed here}}
    consumeVal(x2) // expected-note {{consumed again here}}
}

public func aggGenericStructDoubleConsume<T>(_ x: borrowing AggGenericStruct<T>) { // expected-error {{'x' is borrowed and cannot be consumed}}
    let x2 = x  // expected-error {{'x2' consumed more than once}}
                // expected-note @-1 {{consumed here}}
    consumeVal(x2) // expected-note {{consumed here}}
    consumeVal(x2) // expected-note {{consumed again here}}
}

public func aggGenericStructDoubleConsumeArg<T>(_ x2: borrowing AggGenericStruct<T>) { // expected-error {{'x2' is borrowed and cannot be consumed}}
    consumeVal(x2) // expected-note {{consumed here}}
    consumeVal(x2) // expected-note {{consumed here}}
}

public func aggGenericStructDoubleConsumeOwnedArg<T>(_ x2: __owned AggGenericStruct<T>) { // expected-error {{'x2' consumed more than once}}
    consumeVal(x2) // expected-note {{consumed here}}
    consumeVal(x2) // expected-note {{consumed again here}}
}

public func aggGenericStructDoubleConsumeOwnedArg2<T>(_ x2: consuming AggGenericStruct<T>) { // expected-error {{'x2' consumed more than once}}
    consumeVal(x2) // expected-note {{consumed here}}
    consumeVal(x2) // expected-note {{consumed again here}}
}

public func aggGenericStructLoopConsume<T>(_ x: borrowing AggGenericStruct<T>) { // expected-error {{'x' is borrowed and cannot be consumed}}
    let x2 = x // expected-error {{'x2' consumed in a loop}}
               // expected-note @-1 {{consumed here}}
    for _ in 0..<1024 {
        consumeVal(x2) // expected-note {{consumed here}}
    }
}

public func aggGenericStructLoopConsumeArg<T>(_ x2: borrowing AggGenericStruct<T>) { // expected-error {{'x2' is borrowed and cannot be consumed}}
    for _ in 0..<1024 {
        consumeVal(x2) // expected-note {{consumed here}}
    }
}

public func aggGenericStructLoopConsumeOwnedArg<T>(_ x2: __owned AggGenericStruct<T>) { // expected-error {{'x2' consumed in a loop}}
    for _ in 0..<1024 {
        consumeVal(x2) // expected-note {{consumed here}}
    }
}

public func aggGenericStructLoopConsumeOwnedArg2<T>(_ x2: consuming AggGenericStruct<T>) { // expected-error {{'x2' consumed in a loop}}
    for _ in 0..<1024 {
        consumeVal(x2) // expected-note {{consumed here}}
    }
}

public func aggGenericStructDiamond<T>(_ x: borrowing AggGenericStruct<T>) { // expected-error {{'x' is borrowed and cannot be consumed}}
    let x2 = x // expected-note {{consumed here}}
    if boolValue {
        consumeVal(x2)
    } else {
        consumeVal(x2)
    }
}

public func aggGenericStructDiamondArg<T>(_ x2: borrowing AggGenericStruct<T>) { // expected-error {{'x2' is borrowed and cannot be consumed}}
    if boolValue {
        consumeVal(x2) // expected-note {{consumed here}}
    } else {
        consumeVal(x2) // expected-note {{consumed here}}
    }
}

public func aggGenericStructDiamondOwnedArg<T>(_ x2: __owned AggGenericStruct<T>) {
    if boolValue {
        consumeVal(x2)
    } else {
        consumeVal(x2)
    }
}

public func aggGenericStructDiamondOwnedArg2<T>(_ x2: consuming AggGenericStruct<T>) {
    if boolValue {
        consumeVal(x2)
    } else {
        consumeVal(x2)
    }
}

public func aggGenericStructDiamondInLoop<T>(_ x: borrowing AggGenericStruct<T>) { // expected-error {{'x' is borrowed and cannot be consumed}}
    let x2 = x // expected-error {{'x2' consumed more than once}}
    // expected-note @-1 {{consumed here}}
    // expected-error @-2 {{'x2' consumed in a loop}}
    for _ in 0..<1024 {
      if boolValue {
          consumeVal(x2) // expected-note {{consumed here}}
      } else {
          consumeVal(x2) // expected-note {{consumed here}}
          // expected-note @-1 {{consumed again here}}
      }
    }
}

public func aggGenericStructDiamondInLoopArg<T>(_ x2: borrowing AggGenericStruct<T>) { // expected-error {{'x2' is borrowed and cannot be consumed}}
    for _ in 0..<1024 {
      if boolValue {
          consumeVal(x2) // expected-note {{consumed here}}
      } else {
          consumeVal(x2) // expected-note {{consumed here}}
      }
    }
}

public func aggGenericStructDiamondInLoopOwnedArg<T>(_ x2: __owned AggGenericStruct<T>) { // expected-error {{'x2' consumed more than once}}
    // expected-error @-1 {{'x2' consumed in a loop}}
    for _ in 0..<1024 {
      if boolValue {
          consumeVal(x2) // expected-note {{consumed here}}
      } else {
          consumeVal(x2) // expected-note {{consumed here}}
          // expected-note @-1 {{consumed again here}}
      }
    }
}

public func aggGenericStructDiamondInLoopOwnedArg2<T>(_ x2: consuming AggGenericStruct<T>) { // expected-error {{'x2' consumed more than once}}
    // expected-error @-1 {{'x2' consumed in a loop}}
    for _ in 0..<1024 {
      if boolValue {
          consumeVal(x2) // expected-note {{consumed here}}
      } else {
          consumeVal(x2) // expected-note {{consumed here}}
          // expected-note @-1 {{consumed again here}}
      }
    }
}

public func aggGenericStructAccessField<T>(_ x: borrowing AggGenericStruct<T>) { // expected-error {{'x' is borrowed and cannot be consumed}}
    let x2 = x // expected-note {{consumed here}}
    borrowVal(x2.lhs)
    for _ in 0..<1024 {
        borrowVal(x2.lhs)
    }
}

public func aggGenericStructAccessFieldArg<T>(_ x2: borrowing AggGenericStruct<T>) {
    borrowVal(x2.lhs)
    for _ in 0..<1024 {
        borrowVal(x2.lhs)
    }
}

public func aggGenericStructAccessFieldOwnedArg<T>(_ x2: __owned AggGenericStruct<T>) {
    borrowVal(x2.lhs)
    for _ in 0..<1024 {
        borrowVal(x2.lhs)
    }
}

public func aggGenericStructAccessFieldOwnedArg2<T>(_ x2: consuming AggGenericStruct<T>) {
    borrowVal(x2.lhs)
    for _ in 0..<1024 {
        borrowVal(x2.lhs)
    }
}

public func aggGenericStructConsumeField<T>(_ x: borrowing AggGenericStruct<T>) { // expected-error {{'x' is borrowed and cannot be consumed}}
    let x2 = x // expected-note {{consumed here}}
    consumeVal(x2.lhs)
    for _ in 0..<1024 {
        consumeVal(x2.lhs)
    }
}

public func aggGenericStructConsumeFieldArg<T>(_ x2: borrowing AggGenericStruct<T>) {
    consumeVal(x2.lhs)
    for _ in 0..<1024 {
        consumeVal(x2.lhs)
    }
}

public func aggGenericStructConsumeFieldOwnedArg<T>(_ x2: __owned AggGenericStruct<T>) {
    consumeVal(x2.lhs)
    for _ in 0..<1024 {
        consumeVal(x2.lhs)
    }
}

public func aggGenericStructConsumeFieldOwnedArg2<T>(_ x2: consuming AggGenericStruct<T>) {
    consumeVal(x2.lhs)
    for _ in 0..<1024 {
        consumeVal(x2.lhs)
    }
}

public func aggGenericStructAccessGrandField<T>(_ x: borrowing AggGenericStruct<T>) { // expected-error {{'x' is borrowed and cannot be consumed}}
    let x2 = x // expected-note {{consumed here}}
    borrowVal(x2.pair.lhs)
    for _ in 0..<1024 {
        borrowVal(x2.pair.lhs)
    }
}

public func aggGenericStructAccessGrandFieldArg<T>(_ x2: borrowing AggGenericStruct<T>) {
    borrowVal(x2.pair.lhs)
    for _ in 0..<1024 {
        borrowVal(x2.pair.lhs)
    }
}

public func aggGenericStructAccessGrandFieldOwnedArg<T>(_ x2: __owned AggGenericStruct<T>) {
    borrowVal(x2.pair.lhs)
    for _ in 0..<1024 {
        borrowVal(x2.pair.lhs)
    }
}

public func aggGenericStructAccessGrandFieldOwnedArg2<T>(_ x2: consuming AggGenericStruct<T>) {
    borrowVal(x2.pair.lhs)
    for _ in 0..<1024 {
        borrowVal(x2.pair.lhs)
    }
}

public func aggGenericStructConsumeGrandField<T>(_ x: borrowing AggGenericStruct<T>) { // expected-error {{'x' is borrowed and cannot be consumed}}
    let x2 = x // expected-note {{consumed here}}
    consumeVal(x2.pair.lhs)
    for _ in 0..<1024 {
        consumeVal(x2.pair.lhs)
    }
}

public func aggGenericStructConsumeGrandFieldArg<T>(_ x2: borrowing AggGenericStruct<T>) {
    consumeVal(x2.pair.lhs)
    for _ in 0..<1024 {
        consumeVal(x2.pair.lhs)
    }
}

public func aggGenericStructConsumeGrandFieldOwnedArg<T>(_ x2: __owned AggGenericStruct<T>) {
    consumeVal(x2.pair.lhs)
    for _ in 0..<1024 {
        consumeVal(x2.pair.lhs)
    }
}

public func aggGenericStructConsumeGrandFieldOwnedArg2<T>(_ x2: consuming AggGenericStruct<T>) {
    consumeVal(x2.pair.lhs)
    for _ in 0..<1024 {
        consumeVal(x2.pair.lhs)
    }
}

/////////////////////
// Enum Test Cases //
/////////////////////

@_moveOnly
public enum EnumTy {
    case klass(NonTrivialStruct)
    case int(Int)

    func doSomething() -> Bool { true }
}

public func enumSimpleChainTest(_ x: borrowing EnumTy) { // expected-error {{'x' is borrowed and cannot be consumed}}
    let x2 = x // expected-note {{consumed here}}
    let y2 = x2
    let k2 = y2
    borrowVal(k2)
}

public func enumSimpleChainTestArg(_ x2: borrowing EnumTy) { // expected-error {{'x2' is borrowed and cannot be consumed}}
    let y2 = x2 // expected-note {{consumed here}}
    let k2 = y2
    borrowVal(k2)
}

public func enumSimpleChainTestOwnedArg(_ x2: __owned EnumTy) {
    let y2 = x2
    let k2 = y2
    borrowVal(k2)
}

public func enumSimpleChainTestOwnedArg2(_ x2: consuming EnumTy) {
    let y2 = x2
    let k2 = y2
    borrowVal(k2)
}

public func enumSimpleNonConsumingUseTest(_ x: borrowing EnumTy) { // expected-error {{'x' is borrowed and cannot be consumed}}
    let x2 = x // expected-note {{consumed here}}
    borrowVal(x2)
}

public func enumSimpleNonConsumingUseTestArg(_ x2: borrowing EnumTy) {
    borrowVal(x2)
}

public func enumSimpleNonConsumingUseTestOwnedArg(_ x2: __owned EnumTy) {
    borrowVal(x2)
}

public func enumSimpleNonConsumingUseTestOwnedArg2(_ x2: consuming EnumTy) {
    borrowVal(x2)
}

public func enumMultipleNonConsumingUseTest(_ x: borrowing EnumTy) { // expected-error {{'x' is borrowed and cannot be consumed}}
    let x2 = x // expected-note {{consumed here}}
    borrowVal(x2)
    borrowVal(x2)
    consumeVal(x2)
}

public func enumMultipleNonConsumingUseTestArg(_ x2: borrowing EnumTy) { // expected-error {{'x2' is borrowed and cannot be consumed}}
    borrowVal(x2)
    borrowVal(x2)
    consumeVal(x2) // expected-note {{consumed here}}
}

public func enumMultipleNonConsumingUseTestOwnedArg(_ x2: __owned EnumTy) {
    borrowVal(x2)
    borrowVal(x2)
    consumeVal(x2)
}

public func enumMultipleNonConsumingUseTestOwnedArg2(_ x2: consuming EnumTy) {
    borrowVal(x2)
    borrowVal(x2)
    consumeVal(x2)
}

public func enumUseAfterConsume(_ x: borrowing EnumTy) { // expected-error {{'x' is borrowed and cannot be consumed}}
    let x2 = x // expected-error {{'x2' consumed more than once}}
               // expected-note @-1 {{consumed here}}
    borrowVal(x2)
    consumeVal(x2) // expected-note {{consumed here}}
    consumeVal(x2) // expected-note {{consumed again here}}
}

public func enumUseAfterConsumeArg(_ x2: borrowing EnumTy) { // expected-error {{'x2' is borrowed and cannot be consumed}}
    borrowVal(x2)
    consumeVal(x2) // expected-note {{consumed here}}
    consumeVal(x2) // expected-note {{consumed here}}
}

public func enumUseAfterConsumeOwnedArg(_ x2: __owned EnumTy) { // expected-error {{'x2' consumed more than once}}
    borrowVal(x2)
    consumeVal(x2) // expected-note {{consumed here}}
    consumeVal(x2) // expected-note {{consumed again here}}
}

public func enumUseAfterConsumeOwnedArg2(_ x2: consuming EnumTy) { // expected-error {{'x2' consumed more than once}}
    borrowVal(x2)
    consumeVal(x2) // expected-note {{consumed here}}
    consumeVal(x2) // expected-note {{consumed again here}}
}

public func enumDoubleConsume(_ x: borrowing EnumTy) { // expected-error {{'x' is borrowed and cannot be consumed}}
    let x2 = x  // expected-error {{'x2' consumed more than once}}
                // expected-note @-1 {{consumed here}}
    consumeVal(x2) // expected-note {{consumed here}}
    consumeVal(x2) // expected-note {{consumed again here}}
}

public func enumDoubleConsumeArg(_ x2: borrowing EnumTy) { // expected-error {{'x2' is borrowed and cannot be consumed}}
    consumeVal(x2) // expected-note {{consumed here}}
    consumeVal(x2) // expected-note {{consumed here}}
}

public func enumDoubleConsumeOwnedArg(_ x2: __owned EnumTy) { // expected-error {{'x2' consumed more than once}}
    consumeVal(x2) // expected-note {{consumed here}}
    consumeVal(x2) // expected-note {{consumed again here}}
}

public func enumDoubleConsumeOwnedArg2(_ x2: consuming EnumTy) { // expected-error {{'x2' consumed more than once}}
    consumeVal(x2) // expected-note {{consumed here}}
    consumeVal(x2) // expected-note {{consumed again here}}
}

public func enumLoopConsume(_ x: borrowing EnumTy) { // expected-error {{'x' is borrowed and cannot be consumed}}
    let x2 = x // expected-error {{'x2' consumed in a loop}}
               // expected-note @-1 {{consumed here}}
    for _ in 0..<1024 {
        consumeVal(x2) // expected-note {{consumed here}}
    }
}

public func enumLoopConsumeArg(_ x2: borrowing EnumTy) { // expected-error {{'x2' is borrowed and cannot be consumed}}
    for _ in 0..<1024 {
        consumeVal(x2) // expected-note {{consumed here}}
    }
}

public func enumLoopConsumeOwnedArg(_ x2: __owned EnumTy) { // expected-error {{'x2' consumed in a loop}}
    for _ in 0..<1024 {
        consumeVal(x2) // expected-note {{consumed here}};
    }
}

public func enumLoopConsumeOwnedArg2(_ x2: consuming EnumTy) { // expected-error {{'x2' consumed in a loop}}
    for _ in 0..<1024 {
        consumeVal(x2) // expected-note {{consumed here}};
    }
}

public func enumDiamond(_ x: borrowing EnumTy) { // expected-error {{'x' is borrowed and cannot be consumed}}
    let x2 = x // expected-note {{consumed here}}
    if boolValue {
        consumeVal(x2)
    } else {
        consumeVal(x2)
    }
}

public func enumDiamondArg(_ x2: borrowing EnumTy) { // expected-error {{'x2' is borrowed and cannot be consumed}}
    if boolValue {
        consumeVal(x2) // expected-note {{consumed here}}
    } else {
        consumeVal(x2) // expected-note {{consumed here}}
    }
}

public func enumDiamondOwnedArg(_ x2: __owned EnumTy) {
    if boolValue {
        consumeVal(x2)
    } else {
        consumeVal(x2)
    }
}

public func enumDiamondOwnedArg2(_ x2: consuming EnumTy) {
    if boolValue {
        consumeVal(x2)
    } else {
        consumeVal(x2)
    }
}

public func enumDiamondInLoop(_ x: borrowing EnumTy) { // expected-error {{'x' is borrowed and cannot be consumed}}
    let x2 = x // expected-error {{'x2' consumed more than once}}
    // expected-note @-1 {{consumed here}}
    // expected-error @-2 {{'x2' consumed in a loop}}
    for _ in 0..<1024 {
      if boolValue {
          consumeVal(x2) // expected-note {{consumed here}}
      } else {
          consumeVal(x2) // expected-note {{consumed here}}
          // expected-note @-1 {{consumed again here}}
      }
    }
}

public func enumDiamondInLoopArg(_ x2: borrowing EnumTy) { // expected-error {{'x2' is borrowed and cannot be consumed}}
    for _ in 0..<1024 {
      if boolValue {
          consumeVal(x2) // expected-note {{consumed here}}
      } else {
          consumeVal(x2) // expected-note {{consumed here}}
      }
    }
}

public func enumDiamondInLoopOwnedArg(_ x2: __owned EnumTy) { // expected-error {{'x2' consumed more than once}}
    // expected-error @-1 {{'x2' consumed in a loop}}
    for _ in 0..<1024 {
      if boolValue {
          consumeVal(x2) // expected-note {{consumed here}}
      } else {
          consumeVal(x2) // expected-note {{consumed here}}
          // expected-note @-1 {{consumed again here}}
      }
    }
}

public func enumDiamondInLoopOwnedArg2(_ x2: consuming EnumTy) { // expected-error {{'x2' consumed more than once}}
    // expected-error @-1 {{'x2' consumed in a loop}}
    for _ in 0..<1024 {
      if boolValue {
          consumeVal(x2) // expected-note {{consumed here}}
      } else {
          consumeVal(x2) // expected-note {{consumed here}}
          // expected-note @-1 {{consumed again here}}
      }
    }
}

public func enumAssignToVar1(_ x: borrowing EnumTy) { // expected-error {{'x' is borrowed and cannot be consumed}}
    let x2 = x // expected-error {{'x2' consumed more than once}}
               // expected-note @-1 {{consumed here}}
    var x3 = x2 // expected-note {{consumed here}}
    x3 = x2 // expected-note {{consumed again here}}
    x3 = x // expected-note {{consumed here}}
    consumeVal(x3)
}

public func enumAssignToVar1Arg(_ x: borrowing EnumTy, _ x2: borrowing EnumTy) { // expected-error {{'x2' is borrowed and cannot be consumed}}
                                                             // expected-error @-1 {{'x' is borrowed and cannot be consumed}}
    var x3 = x2 // expected-note {{consumed here}}
    x3 = x2 // expected-note {{consumed here}}
    x3 = x // expected-note {{consumed here}}
    consumeVal(x3)
}

public func enumAssignToVar1OwnedArg(_ x: borrowing EnumTy, _ x2: __owned EnumTy) { // expected-error {{'x2' consumed more than once}}
                                                                          // expected-error @-1 {{'x' is borrowed and cannot be consumed}}
    var x3 = x2 // expected-note {{consumed here}}
    x3 = x2 // expected-note {{consumed again here}}
    x3 = x // expected-note {{consumed here}}
    consumeVal(x3)
}

public func enumAssignToVar1OwnedArg2(_ x: borrowing EnumTy, _ x2: consuming EnumTy) { // expected-error {{'x2' consumed more than once}}
                                                                          // expected-error @-1 {{'x' is borrowed and cannot be consumed}}
    var x3 = x2 // expected-note {{consumed here}}
    x3 = x2 // expected-note {{consumed again here}}
    x3 = x // expected-note {{consumed here}}
    consumeVal(x3)
}

public func enumAssignToVar2(_ x: borrowing EnumTy) { // expected-error {{'x' is borrowed and cannot be consumed}}
    let x2 = x // expected-error {{'x2' consumed more than once}}
               // expected-note @-1 {{consumed here}}
    var x3 = x2 // expected-note {{consumed here}}
    x3 = x2 // expected-note {{consumed again here}}
    borrowVal(x3)
}

public func enumAssignToVar2Arg(_ x2: borrowing EnumTy) { // expected-error {{'x2' is borrowed and cannot be consumed}}
    var x3 = x2 // expected-note {{consumed here}}
    x3 = x2 // expected-note {{consumed here}}
    borrowVal(x3)
}

public func enumAssignToVar2OwnedArg(_ x2: __owned EnumTy) { // expected-error {{'x2' consumed more than once}}
    var x3 = x2 // expected-note {{consumed here}}
    x3 = x2 // expected-note {{consumed again here}}
    borrowVal(x3)
}

public func enumAssignToVar2OwnedArg2(_ x2: consuming EnumTy) { // expected-error {{'x2' consumed more than once}}
    var x3 = x2 // expected-note {{consumed here}}
    x3 = x2 // expected-note {{consumed again here}}
    borrowVal(x3)
}

public func enumAssignToVar3(_ x: borrowing EnumTy) { // expected-error {{'x' is borrowed and cannot be consumed}}
    let x2 = x // expected-note {{consumed here}}
    var x3 = x2
    x3 = x // expected-note {{consumed here}}
    consumeVal(x3)
}

public func enumAssignToVar3Arg(_ x: borrowing EnumTy, _ x2: borrowing EnumTy) { // expected-error {{'x2' is borrowed and cannot be consumed}}
                                                             // expected-error @-1 {{'x' is borrowed and cannot be consumed}}
    var x3 = x2 // expected-note {{consumed here}}
    x3 = x // expected-note {{consumed here}}
    consumeVal(x3)
}

public func enumAssignToVar3OwnedArg(_ x: borrowing EnumTy, _ x2: __owned EnumTy) { // expected-error {{'x' is borrowed and cannot be consumed}}
    var x3 = x2
    x3 = x // expected-note {{consumed here}}
    consumeVal(x3)
}

public func enumAssignToVar3OwnedArg2(_ x: borrowing EnumTy, _ x2: consuming EnumTy) { // expected-error {{'x' is borrowed and cannot be consumed}}
    var x3 = x2
    x3 = x // expected-note {{consumed here}}
    consumeVal(x3)
}

public func enumAssignToVar4(_ x: borrowing EnumTy) { // expected-error {{'x' is borrowed and cannot be consumed}}
    let x2 = x // expected-error {{'x2' consumed more than once}}
               // expected-note @-1 {{consumed here}}
    let x3 = x2 // expected-note {{consumed here}}
    consumeVal(x2) // expected-note {{consumed again here}}
    consumeVal(x3)
}

public func enumAssignToVar4Arg(_ x2: borrowing EnumTy) { // expected-error {{'x2' is borrowed and cannot be consumed}}
    let x3 = x2 // expected-note {{consumed here}}
    consumeVal(x2) // expected-note {{consumed here}}
    consumeVal(x3)
}

public func enumAssignToVar4OwnedArg(_ x2: __owned EnumTy) { // expected-error {{'x2' consumed more than once}}
    let x3 = x2 // expected-note {{consumed here}}
    consumeVal(x2) // expected-note {{consumed again here}}
    consumeVal(x3)
}

public func enumAssignToVar4OwnedArg2(_ x2: consuming EnumTy) { // expected-error {{'x2' consumed more than once}}
    let x3 = x2 // expected-note {{consumed here}}
    consumeVal(x2) // expected-note {{consumed again here}}
    consumeVal(x3)
}

public func enumAssignToVar5(_ x: borrowing EnumTy) { // expected-error {{'x' is borrowed and cannot be consumed}}
    let x2 = x // expected-error {{'x2' used after consume}}
               // expected-note @-1 {{consumed here}}
    var x3 = x2 // expected-note {{consumed here}}
    borrowVal(x2) // expected-note {{used here}}
    x3 = x // expected-note {{consumed here}}
    consumeVal(x3)
}

public func enumAssignToVar5Arg(_ x: borrowing EnumTy, _ x2: borrowing EnumTy) { // expected-error {{'x2' is borrowed and cannot be consumed}}
                                                             // expected-error @-1 {{'x' is borrowed and cannot be consumed}}
    var x3 = x2 // expected-note {{consumed here}}
    // TODO: Need to mark this as the lifetime extending use. We fail
    // appropriately though.
    borrowVal(x2)
    x3 = x // expected-note {{consumed here}}
    consumeVal(x3)
}

public func enumAssignToVar5OwnedArg(_ x: borrowing EnumTy, _ x2: __owned EnumTy) { // expected-error {{'x2' used after consume}}
                                                                          // expected-error @-1 {{'x' is borrowed and cannot be consumed}}
    var x3 = x2 // expected-note {{consumed here}}
    borrowVal(x2) // expected-note {{used here}}
    x3 = x // expected-note {{consumed here}}
    consumeVal(x3)
}

public func enumAssignToVar5OwnedArg2(_ x: borrowing EnumTy, _ x2: consuming EnumTy) { // expected-error {{'x2' used after consume}}
                                                                          // expected-error @-1 {{'x' is borrowed and cannot be consumed}}
    var x3 = x2 // expected-note {{consumed here}}
    borrowVal(x2) // expected-note {{used here}}
    x3 = x // expected-note {{consumed here}}
    consumeVal(x3)
}

public func enumPatternMatchIfLet1(_ x: borrowing EnumTy) { // expected-error {{'x' is borrowed and cannot be consumed}}
    let x2 = x // expected-error {{'x2' consumed more than once}}
               // expected-note @-1 {{consumed here}}
    if case let .klass(x) = consume x2 { // expected-note {{consumed here}}
        borrowVal(x.i)
    }
    if case let .klass(x) = consume x2 { // expected-note {{consumed again here}}
        borrowVal(x.i)
    }
}

public func enumPatternMatchIfLet1Arg(_ x2: borrowing EnumTy) { // expected-error {{'x2' is borrowed and cannot be consumed}}
    if case let .klass(x) = consume x2 { // expected-note {{consumed here}}
        borrowVal(x.i)
    }
    if case let .klass(x) = consume x2 { // expected-note {{consumed here}}
        borrowVal(x.i)
    }
}

public func enumPatternMatchIfLet1OwnedArg(_ x2: __owned EnumTy) { // expected-error {{'x2' consumed more than once}}
    if case let .klass(x) = consume x2 { // expected-note {{consumed here}}
        borrowVal(x)
    }
    if case let .klass(x) = consume x2 { // expected-note {{consumed again here}}
        borrowVal(x)
    }
}

public func enumPatternMatchIfLet1OwnedArg2(_ x2: consuming EnumTy) { // expected-error {{'x2' consumed more than once}}
    if case let .klass(x) = consume x2 { // expected-note {{consumed here}}
        borrowVal(x)
    }
    if case let .klass(x) = consume x2 { // expected-note {{consumed again here}}
        borrowVal(x)
    }
}

public func enumPatternMatchIfLet2(_ x: borrowing EnumTy) { // expected-error {{'x' is borrowed and cannot be consumed}}
    let x2 = x // expected-error {{'x2' consumed in a loop}}
               // expected-note @-1 {{consumed here}}
    for _ in 0..<1024 {
        if case let .klass(x) = consume x2 {  // expected-note {{consumed here}}
            borrowVal(x)
        }
    }
}

public func enumPatternMatchIfLet2Arg(_ x2: borrowing EnumTy) { // expected-error {{'x2' is borrowed and cannot be consumed}}
    for _ in 0..<1024 {
        if case let .klass(x) = consume x2 {  // expected-note {{consumed here}}
            borrowVal(x)
        }
    }
}

public func enumPatternMatchIfLet2OwnedArg(_ x2: __owned EnumTy) { // expected-error {{'x2' consumed in a loop}}
    for _ in 0..<1024 {
        if case let .klass(x) = consume x2 {  // expected-note {{consumed here}}
            borrowVal(x)
        }
    }
}

public func enumPatternMatchIfLet2OwnedArg2(_ x2: consuming EnumTy) { // expected-error {{'x2' consumed in a loop}}
    for _ in 0..<1024 {
        if case let .klass(x) = consume x2 {  // expected-note {{consumed here}}
            borrowVal(x)
        }
    }
}

public func enumPatternMatchSwitch1(_ x: borrowing EnumTy) { // expected-error {{'x' is borrowed and cannot be consumed}}
    let x2 = x // expected-error {{'x2' used after consume}}
               // expected-note @-1 {{consumed here}}
    switch consume x2 { // expected-note {{consumed here}}
    case let .klass(k):
        borrowVal(k)
        borrowVal(x2) // expected-note {{used here}}
    case .int:
        break
    }
}

public func enumPatternMatchSwitch1Arg(_ x2: borrowing EnumTy) { // expected-error {{'x2' is borrowed and cannot be consumed}}
    switch consume x2 { // expected-note {{consumed here}}
    case let .klass(k):
        borrowVal(k)
        // This should be flagged as the use after free use. We are atleast
        // erroring though.
        borrowVal(x2)
    case .int:
        break
    }
}

public func enumPatternMatchSwitch1OwnedArg(_ x2: __owned EnumTy) { // expected-error {{'x2' used after consume}}
    switch consume x2 { // expected-note {{consumed here}}
    case let .klass(k):
        borrowVal(k)
        borrowVal(x2) // expected-note {{used here}}
    case .int:
        break
    }
}

public func enumPatternMatchSwitch1OwnedArg2(_ x2: consuming EnumTy) { // expected-error {{'x2' used after consume}}
    switch consume x2 { // expected-note {{consumed here}}
    case let .klass(k):
        borrowVal(k)
        borrowVal(x2) // expected-note {{used here}}
    case .int:
        break
    }
}

public func enumPatternMatchSwitch2(_ x: borrowing EnumTy) { // expected-error {{'x' is borrowed and cannot be consumed}}
    let x2 = x // expected-note {{consumed here}}
    switch consume x2 {
    case let .klass(k):
        borrowVal(k)
    case .int:
        break
    }
}

public func enumPatternMatchSwitch2Arg(_ x2: borrowing EnumTy) { // expected-error {{'x2' is borrowed and cannot be consumed}}
    switch consume x2 { // expected-note {{consumed here}}
    case let .klass(k):
        borrowVal(k)
    case .int:
        break
    }
}

public func enumPatternMatchSwitch2OwnedArg(_ x2: __owned EnumTy) {
    switch consume x2 {
    case let .klass(k):
        borrowVal(k)
    case .int:
        break
    }
}

public func enumPatternMatchSwitch2OwnedArg2(_ x2: consuming EnumTy) {
    switch consume x2 {
    case let .klass(k):
        borrowVal(k)
    case .int:
        break
    }
}

// TODO: We can do better here. We should also flag x2
public func enumPatternMatchSwitch2WhereClause(_ x: borrowing EnumTy) { // expected-error {{'x' is borrowed and cannot be consumed}}
    let x2 = x // expected-error {{'x2' used after consume}}
               // expected-note @-1 {{consumed here}}
    switch consume x2 { // expected-note {{consumed here}}
    case let .klass(k)
           where x2.doSomething(): // expected-note {{used here}}
        borrowVal(k)
    case .int:
        break
    case .klass:
        break
    }
}

public func enumPatternMatchSwitch2WhereClauseArg(_ x2: borrowing EnumTy) { // expected-error {{'x2' is borrowed and cannot be consumed}}
    switch consume x2 { // expected-note {{consumed here}}
    case let .klass(k)
           where x2.doSomething():
        borrowVal(k)
    case .int:
        break
    case .klass:
        break
    }
}

public func enumPatternMatchSwitch2WhereClauseOwnedArg(_ x2: __owned EnumTy) { // expected-error {{'x2' used after consume}}
    switch consume x2 { // expected-note {{consumed here}}
    case let .klass(k)
           where x2.doSomething(): // expected-note {{used here}}
        borrowVal(k)
    case .int:
        break
    case .klass:
        break
    }
}

public func enumPatternMatchSwitch2WhereClauseOwnedArg2(_ x2: consuming EnumTy) { // expected-error {{'x2' used after consume}}
    switch consume x2 { // expected-note {{consumed here}}
    case let .klass(k)
           where x2.doSomething(): // expected-note {{used here}}
        borrowVal(k)
    case .int:
        break
    case .klass:
        break
    }
}

public func enumPatternMatchSwitch2WhereClause2(_ x: borrowing EnumTy) { // expected-error {{'x' is borrowed and cannot be consumed}}
    let x2 = x // expected-note {{consumed here}}
    switch consume x2 {
    case let .klass(k)
           where boolValue:
        borrowVal(k)
    case .int:
        break
    case .klass:
        break
    }
}

public func enumPatternMatchSwitch2WhereClause2Arg(_ x2: borrowing EnumTy) { // expected-error {{'x2' is borrowed and cannot be consumed}}
    switch consume x2 { // expected-note {{consumed here}}
    case let .klass(k)
           where boolValue:
        borrowVal(k)
    case .int:
        break
    case .klass:
        break
    }
}

public func enumPatternMatchSwitch2WhereClause2OwnedArg(_ x2: __owned EnumTy) {
    switch consume x2 {
    case let .klass(k)
           where boolValue:
        borrowVal(k)
    case .int:
        break
    case .klass:
        break
    }
}

public func enumPatternMatchSwitch2WhereClause2OwnedArg2(_ x2: consuming EnumTy) {
    switch consume x2 {
    case let .klass(k)
           where boolValue:
        borrowVal(k)
    case .int:
        break
    case .klass:
        break
    }
}

/////////////////////////////
// Closure and Defer Tests //
/////////////////////////////

public func closureClassUseAfterConsume1(_ x: borrowing NonTrivialStruct) {
    // expected-error @-1 {{noncopyable 'x' cannot be consumed when captured by a closure}}
    // expected-error @-2 {{'x' cannot be captured by an escaping closure since it is a borrowed parameter}}
    let f = { // expected-note {{closure capturing 'x' here}}
        let x2 = x // expected-error {{'x2' consumed more than once}}
        // expected-note @-1 {{consumed here}}
        borrowVal(x2)
        consumeVal(x2) // expected-note {{consumed here}}
        consumeVal(x2) // expected-note {{consumed again here}}
    }
    f()
}

public func closureClassUseAfterConsume2(_ argX: borrowing NonTrivialStruct) {
    let f = { (_ x: borrowing NonTrivialStruct) in // expected-error {{'x' is borrowed and cannot be consumed}}
        let x2 = x // expected-error {{'x2' consumed more than once}}
                   // expected-note @-1 {{consumed here}}
        borrowVal(x2)
        consumeVal(x2) // expected-note {{consumed here}}
        consumeVal(x2) // expected-note {{consumed again here}}
    }
    f(argX)
}

public func closureClassUseAfterConsumeArg(_ argX: borrowing NonTrivialStruct) {
    // TODO: Fix this
    let f = { (_ x2: borrowing NonTrivialStruct) in // expected-error {{'x2' is borrowed and cannot be consumed}}
        borrowVal(x2)
        consumeVal(x2) // expected-note {{consumed here}}
        consumeVal(x2) // expected-note {{consumed here}}
    }
    f(argX)
}

public func closureCaptureClassUseAfterConsume(_ x: borrowing NonTrivialStruct) { // expected-error {{'x' is borrowed and cannot be consumed}}
    let x2 = x // expected-note {{consumed here}}
    // expected-error @-1 {{noncopyable 'x2' cannot be consumed when captured by a closure}}
    // expected-error @-2 {{noncopyable 'x2' cannot be consumed when captured by a closure}}
    
    let f = {
        borrowVal(x2)
        consumeVal(x2) // expected-note {{consumed here}}
        consumeVal(x2) // expected-note {{consumed here}}
    }
    f()
}

public func closureCaptureClassUseAfterConsumeError(_ x: borrowing NonTrivialStruct) { // expected-error {{'x' is borrowed and cannot be consumed}}
    let x2 = x // expected-note {{consumed here}}
    // expected-error @-1 {{noncopyable 'x2' cannot be consumed when captured by a closure}}
    // expected-error @-2 {{noncopyable 'x2' cannot be consumed when captured by a closure}}
    let f = {
        borrowVal(x2)
        consumeVal(x2) // expected-note {{consumed here}}
        consumeVal(x2) // expected-note {{consumed here}}
    }
    f()
    let x3 = x2
    let _ = x3
}

public func closureCaptureClassArgUseAfterConsume(_ x2: borrowing NonTrivialStruct) {
    // expected-error @-1 {{noncopyable 'x2' cannot be consumed when captured by a closure}}
    // expected-error @-2 {{'x2' cannot be captured by an escaping closure since it is a borrowed parameter}}
    let f = { // expected-note {{closure capturing 'x2' here}}
        borrowVal(x2)
        consumeVal(x2) // expected-note {{consumed here}}
        consumeVal(x2) // expected-note {{consumed here}}
    }
    f()
}

public func closureCaptureClassOwnedArgUseAfterConsume(_ x2: __owned NonTrivialStruct) {
    // expected-error @-1 {{noncopyable 'x2' cannot be consumed when captured by a closure}}
    // expected-error @-2 {{noncopyable 'x2' cannot be consumed when captured by a closure}}
    let f = {
        borrowVal(x2)
        consumeVal(x2) // expected-note {{consumed here}}
        consumeVal(x2) // expected-note {{consumed here}}
    }
    f()
}

public func closureCaptureClassOwnedArgUseAfterConsume2(_ x2: consuming NonTrivialStruct) {
    // expected-error @-1 {{missing reinitialization of inout parameter 'x2' after consume}}
    // expected-error @-2 {{'x2' consumed more than once}}
    let f = {
        borrowVal(x2)
        consumeVal(x2) // expected-note {{consumed here}}
        consumeVal(x2) // expected-note {{consumed here}}
        // expected-note @-1 {{consumed again here}}
    }
    f()
}

public func closureCaptureClassOwnedArgUseAfterConsume3(_ x2: __owned NonTrivialStruct) {
    // expected-error @-1 {{noncopyable 'x2' cannot be consumed when captured by a closure}}
    // expected-error @-2 {{noncopyable 'x2' cannot be consumed when captured by a closure}}
    let f = {
        borrowVal(x2)
        consumeVal(x2) // expected-note {{consumed here}}
        consumeVal(x2) // expected-note {{consumed here}}
    }
    f()
    let x3 = x2
    let _ = x3
}

public func closureCaptureClassOwnedArgUseAfterConsume4(_ x2: consuming NonTrivialStruct) {
    // expected-error @-1 {{missing reinitialization of inout parameter 'x2' after consume}}
    // expected-error @-2 {{'x2' consumed more than once}}
    // expected-error @-3 {{'x2' consumed more than once}}
    let f = {
        borrowVal(x2)
        consumeVal(x2) // expected-note {{consumed here}}
        consumeVal(x2) // expected-note {{consumed here}}
        // expected-note @-1 {{consumed again here}}
    }
    f()
    let x3 = x2 // expected-note {{consumed here}}
    consumeVal(x2) // expected-note {{consumed again here}}
    let _ = x3
}

public func deferCaptureClassUseAfterConsume(_ x: borrowing NonTrivialStruct) { // expected-error {{'x' is borrowed and cannot be consumed}}
    let x2 = x // expected-note {{consumed here}}
    // expected-error @-1 {{noncopyable 'x2' cannot be consumed when captured by a closure}}
    defer {
        borrowVal(x2)
        consumeVal(x2) // expected-note {{consumed here}}
        consumeVal(x2) // expected-note {{consumed here}}
    }
    consumeVal(x) // expected-note {{consumed here}}
}

public func deferCaptureClassUseAfterConsume2(_ x: borrowing NonTrivialStruct) { // expected-error {{'x' is borrowed and cannot be consumed}}
    let x2 = x
    // expected-note @-1 {{consumed here}}
    // expected-error @-2 {{noncopyable 'x2' cannot be consumed when captured by a closure}}
    // expected-error @-3 {{'x2' used after consume}}
    defer { // expected-note {{used here}}
        borrowVal(x2)
        consumeVal(x2) // expected-note {{consumed here}}
        consumeVal(x2) // expected-note {{consumed here}}
    }
    let x3 = x2 // expected-note {{consumed here}}
    let _ = x3
}

public func deferCaptureClassArgUseAfterConsume(_ x2: borrowing NonTrivialStruct) {
    // expected-error @-1 {{noncopyable 'x2' cannot be consumed when captured by a closure}}
    borrowVal(x2)
    defer {
        borrowVal(x2)
        consumeVal(x2) // expected-note {{consumed here}}
        consumeVal(x2) // expected-note {{consumed here}}
    }
    consumeVal("foo")
}

public func deferCaptureClassOwnedArgUseAfterConsume(_ x2: __owned NonTrivialStruct) {
    // expected-error @-1 {{noncopyable 'x2' cannot be consumed when captured by a closure}}
    defer {
        borrowVal(x2)
        consumeVal(x2) // expected-note {{consumed here}}
        consumeVal(x2) // expected-note {{consumed here}}
    }
    consumeVal("foo")
}

public func deferCaptureClassOwnedArgUseAfterConsume2(_ x2: consuming NonTrivialStruct) {
    // expected-error @-1 {{'x2' consumed more than once}}
    // expected-error @-2 {{missing reinitialization of inout parameter 'x2' after consume}}
    defer {
        borrowVal(x2)
        consumeVal(x2) // expected-note {{consumed here}}
        consumeVal(x2) // expected-note {{consumed here}}
        // expected-note @-1 {{consumed again here}}
    }
    consumeVal("foo")
}

public func deferCaptureClassOwnedArgUseAfterConsume3(_ x2: __owned NonTrivialStruct) {
    // expected-error @-1 {{noncopyable 'x2' cannot be consumed when captured by a closure}}
    // expected-error @-2 {{'x2' used after consume}}
    defer { // expected-note {{used here}}
        borrowVal(x2)
        consumeVal(x2) // expected-note {{consumed here}}
        consumeVal(x2) // expected-note {{consumed here}}
    }
    consumeVal(x2) // expected-note {{consumed here}}
}

public func deferCaptureClassOwnedArgUseAfterConsume4(_ x2: consuming NonTrivialStruct) {
    // expected-error @-1 {{missing reinitialization of inout parameter 'x2' after consume}}
    // expected-error @-2 {{'x2' used after consume}}
    // expected-error @-3 {{'x2' consumed more than once}}
    defer { // expected-note {{used here}}
        borrowVal(x2)
        consumeVal(x2) // expected-note {{consumed here}}
        consumeVal(x2) // expected-note {{consumed here}}
        // expected-note @-1 {{consumed again here}}
    }
    consumeVal(x2) // expected-note {{consumed here}}
}

public func closureAndDeferCaptureClassUseAfterConsume(_ x: borrowing NonTrivialStruct) {
    // expected-error @-1 {{'x' is borrowed and cannot be consumed}}
    let x2 = x // expected-note {{consumed here}}
    // expected-error @-1 {{noncopyable 'x2' cannot be consumed when captured by a closure}}
    let f = {
        defer {
            borrowVal(x2)
            consumeVal(x2) // expected-note {{consumed here}}
            consumeVal(x2) // expected-note {{consumed here}}
        }
        consumeVal("foo")
    }
    f()
}

public func closureAndDeferCaptureClassUseAfterConsume2(_ x: borrowing NonTrivialStruct) { // expected-error {{'x' is borrowed and cannot be consumed}}
    let x2 = x // expected-note {{consumed here}}
    // expected-error @-1 {{noncopyable 'x2' cannot be consumed when captured by a closure}}
    // expected-error @-2 {{noncopyable 'x2' cannot be consumed when captured by a closure}}
    let f = {
        consumeVal(x2) // expected-note {{consumed here}}
        defer {
            borrowVal(x2)
            consumeVal(x2) // expected-note {{consumed here}}
            consumeVal(x2) // expected-note {{consumed here}}
        }
        consumeVal("foo")
    }
    f()
}

public func closureAndDeferCaptureClassUseAfterConsume3(_ x: borrowing NonTrivialStruct) { // expected-error {{'x' is borrowed and cannot be consumed}}
    let x2 = x
    // expected-note @-1 {{consumed here}}
    // expected-error @-2 {{noncopyable 'x2' cannot be consumed when captured by a closure}}
    // expected-error @-3 {{noncopyable 'x2' cannot be consumed when captured by a closure}}
    let f = {
        consumeVal(x2) // expected-note {{consumed here}}
        defer {
            borrowVal(x2)
            consumeVal(x2) // expected-note {{consumed here}}
            consumeVal(x2) // expected-note {{consumed here}}
        }
        consumeVal("foo")
    }
    f()
    consumeVal(x2)
}

public func closureAndDeferCaptureClassArgUseAfterConsume(_ x2: borrowing NonTrivialStruct) {
    // expected-error @-1 {{'x2' cannot be captured by an escaping closure since it is a borrowed parameter}}
    // expected-error @-2 {{noncopyable 'x2' cannot be consumed when captured by a closure}}
    let f = { // expected-note {{closure capturing 'x2' here}}
        defer {
            borrowVal(x2)
            consumeVal(x2) // expected-note {{consumed here}}
            consumeVal(x2) // expected-note {{consumed here}}
        }
        consumeVal("foo")
    }
    f()
}

public func closureAndDeferCaptureClassOwnedArgUseAfterConsume(_ x2: __owned NonTrivialStruct) {
    // expected-error @-1 {{noncopyable 'x2' cannot be consumed when captured by a closure}}
    let f = {
        defer {
            borrowVal(x2)
            consumeVal(x2) // expected-note {{consumed here}}
            consumeVal(x2) // expected-note {{consumed here}}
        }
        consumeVal("foo")
    }
    f()
}

public func closureAndDeferCaptureClassOwnedArgUseAfterConsume2(_ x2: consuming NonTrivialStruct) {
    // expected-error @-1 {{missing reinitialization of inout parameter 'x2' after consume}}
    // expected-error @-2 {{'x2' consumed more than once}}
    let f = {
        defer {
            borrowVal(x2)
            consumeVal(x2) // expected-note {{consumed here}}
            consumeVal(x2) // expected-note {{consumed here}}
            // expected-note @-1 {{consumed again here}}
        }
        consumeVal("foo")
    }
    f()
}

public func closureAndDeferCaptureClassOwnedArgUseAfterConsume3(_ x2: __owned NonTrivialStruct) {
    // expected-error @-1 {{noncopyable 'x2' cannot be consumed when captured by a closure}}
    let f = {
        defer {
            borrowVal(x2)
            consumeVal(x2) // expected-note {{consumed here}}
            consumeVal(x2) // expected-note {{consumed here}}
        }
        consumeVal("foo")
    }
    f()
    consumeVal(x2)
}

public func closureAndDeferCaptureClassOwnedArgUseAfterConsume4(_ x2: consuming NonTrivialStruct) {
    // expected-error @-1 {{missing reinitialization of inout parameter 'x2' after consume}}
    // expected-error @-2 {{'x2' consumed more than once}}
    let f = {
        defer {
            borrowVal(x2)
            consumeVal(x2) // expected-note {{consumed here}}
            consumeVal(x2) // expected-note {{consumed here}}
            // expected-note @-1 {{consumed again here}}
        }
        consumeVal("foo")
    }
    f()
    consumeVal(x2)
}

public func closureAndClosureCaptureClassUseAfterConsume(_ x: borrowing NonTrivialStruct) { // expected-error {{'x' is borrowed and cannot be consumed}}
    let x2 = x // expected-note {{consumed here}}
    // expected-error @-1 {{noncopyable 'x2' cannot be consumed when captured by a closure}}
    // expected-error @-2 {{noncopyable 'x2' cannot be consumed when captured by a closure}}
    let f = {
        let g = {
            borrowVal(x2)
            consumeVal(x2) // expected-note {{consumed here}}
            consumeVal(x2) // expected-note {{consumed here}}
        }
        g()
    }
    f()
}

public func closureAndClosureCaptureClassUseAfterConsume2(_ x: borrowing NonTrivialStruct) { // expected-error {{'x' is borrowed and cannot be consumed}}
    let x2 = x // expected-note {{consumed here}}
    // expected-error @-1 {{noncopyable 'x2' cannot be consumed when captured by a closure}}
    // expected-error @-2 {{noncopyable 'x2' cannot be consumed when captured by a closure}}
    let f = {
        let g = {
            borrowVal(x2)
            consumeVal(x2) // expected-note {{consumed here}}
            consumeVal(x2) // expected-note {{consumed here}}
        }
        g()
    }
    f()
    consumeVal(x2)
}


public func closureAndClosureCaptureClassArgUseAfterConsume(_ x2: borrowing NonTrivialStruct) {
    // expected-error @-1 {{noncopyable 'x2' cannot be consumed when captured by a closure}}
    // expected-error @-2 {{'x2' cannot be captured by an escaping closure since it is a borrowed parameter}}
    // expected-error @-3 {{'x2' cannot be captured by an escaping closure since it is a borrowed parameter}}
    let f = { // expected-note {{closure capturing 'x2' here}}
        let g = { // expected-note {{closure capturing 'x2' here}}
            borrowVal(x2)
            consumeVal(x2) // expected-note {{consumed here}}
            consumeVal(x2) // expected-note {{consumed here}}
        }
        g()
    }
    f()
}

public func closureAndClosureCaptureClassOwnedArgUseAfterConsume(_ x2: __owned NonTrivialStruct) {
    // expected-error @-1 {{noncopyable 'x2' cannot be consumed when captured by a closure}}
    // expected-error @-2 {{noncopyable 'x2' cannot be consumed when captured by a closure}}
    let f = {
        let g = {
            borrowVal(x2)
            consumeVal(x2) // expected-note {{consumed here}}
            consumeVal(x2) // expected-note {{consumed here}}
        }
        g()
    }
    f()
}

public func closureAndClosureCaptureClassOwnedArgUseAfterConsume2(_ x2: consuming NonTrivialStruct) {
    // expected-error @-1 {{'x2' consumed more than once}}
    // expected-error @-2 {{missing reinitialization of inout parameter 'x2' after consume}}
    let f = {
        let g = {
            borrowVal(x2)
            consumeVal(x2) // expected-note {{consumed here}}
            consumeVal(x2) // expected-note {{consumed here}}
            // expected-note @-1 {{consumed again here}}
        }
        g()
    }
    f()
}

public func closureAndClosureCaptureClassOwnedArgUseAfterConsume3(_ x2: __owned NonTrivialStruct) {
    // expected-error @-1 {{noncopyable 'x2' cannot be consumed when captured by a closure}}
    // expected-error @-2 {{noncopyable 'x2' cannot be consumed when captured by a closure}}
    let f = {
        let g = {
            borrowVal(x2)
            consumeVal(x2) // expected-note {{consumed here}}
            consumeVal(x2) // expected-note {{consumed here}}
        }
        g()
    }
    f()
    consumeVal(x2)
}

public func closureAndClosureCaptureClassOwnedArgUseAfterConsume4(_ x2: __owned NonTrivialStruct) {
    // expected-error @-1 {{noncopyable 'x2' cannot be consumed when captured by a closure}}
    // expected-error @-2 {{noncopyable 'x2' cannot be consumed when captured by a closure}}
    let f = {
        let g = {
            borrowVal(x2)
            consumeVal(x2) // expected-note {{consumed here}}
            consumeVal(x2) // expected-note {{consumed here}}
        }
        g()
    }
    f()
    consumeVal(x2)
}
