// RUN: %target-swift-emit-sil -enable-experimental-feature MoveOnlyPartialConsumption -sil-verify-all -verify -enable-experimental-feature MoveOnlyClasses %s

//////////////////
// Declarations //
//////////////////

public class CopyableKlass {}

@_moveOnly
public final class Klass {
    var intField: Int
    var k: Klass
    init() {
        k = Klass()
        intField = 5
    }
}

var boolValue: Bool { return true }

public func borrowVal(_ x: borrowing Klass) {}
public func borrowVal(_ x: borrowing CopyableKlass) {}
public func borrowVal(_ x: borrowing FinalKlass) {}
public func borrowVal(_ x: borrowing AggStruct) {}
public func borrowVal(_ x: borrowing KlassPair) {}
public func borrowVal(_ x: borrowing AggGenericStruct<String>) {}
public func borrowVal<T>(_ x: borrowing AggGenericStruct<T>) {}
public func borrowVal(_ x: borrowing EnumTy) {}

public func consumeVal(_ x: __owned Klass) {}
public func consumeVal(_ x: __owned FinalKlass) {}
public func consumeVal(_ x: __owned AggStruct) {}
public func consumeVal(_ x: __owned AggGenericStruct<String>) {}
public func consumeVal<T>(_ x: __owned AggGenericStruct<T>) {}
public func consumeVal(_ x: __owned EnumTy) {}

@_moveOnly
public final class FinalKlass {
    var k: Klass = Klass()
}

///////////
// Tests //
///////////

/////////////////
// Class Tests //
/////////////////

public func classSimpleChainTest(_ x: borrowing Klass) { // expected-error {{'x' is borrowed and cannot be consumed}}
    let x2 = x // expected-note {{consumed here}}
               // expected-error @-1 {{'x2' consumed more than once}}
    let y2 = x2 // expected-note {{consumed here}}
    let k2 = y2
    let k3 = x2 // expected-note {{consumed again here}}
    let _ = k3
    borrowVal(k2)
}

public func classSimpleChainArgTest(_ x2: borrowing Klass) { // expected-error {{'x2' is borrowed and cannot be consumed}}
    let y2 = x2 // expected-note {{consumed here}}
    let k2 = y2
    borrowVal(k2)
}

public func classSimpleChainOwnedArgTest(_ x2: __owned Klass) {
    let y2 = x2
    let k2 = y2
    borrowVal(k2)
}

public func classSimpleChainOwnedArgTest2(_ x2: consuming Klass) {
    let y2 = x2
    let k2 = y2
    borrowVal(k2)
}

public func classSimpleNonConsumingUseTest(_ x: borrowing Klass) { // expected-error {{'x' is borrowed and cannot be consumed}}
    let x2 = x // expected-note {{consumed here}}
    borrowVal(x2)
}

public func classSimpleNonConsumingUseArgTest(_ x2: borrowing Klass) {
    borrowVal(x2)
}

public func classSimpleNonConsumingUseOwnedArgTest(_ x2: __owned Klass) {
    borrowVal(x2)
}

public func classSimpleNonConsumingUseOwnedArgTest2(_ x2: consuming Klass) {
    borrowVal(x2)
}

public func classMultipleNonConsumingUseTest(_ x: borrowing Klass) { // expected-error {{'x' is borrowed and cannot be consumed}}
    let x2 = x // expected-note {{consumed here}}
    borrowVal(x2)
    borrowVal(x2)
    consumeVal(x2)
}

public func classMultipleNonConsumingUseArgTest(_ x2: borrowing Klass) { // expected-error {{'x2' is borrowed and cannot be consumed}}
    borrowVal(x2)
    borrowVal(x2)
    consumeVal(x2) // expected-note {{consumed here}}
}

public func classMultipleNonConsumingUseOwnedArgTest(_ x2: __owned Klass) {
    borrowVal(x2)
    borrowVal(x2)
    consumeVal(x2)
}

public func classMultipleNonConsumingUseOwnedArgTest2(_ x2: consuming Klass) {
    borrowVal(x2)
    borrowVal(x2)
    consumeVal(x2)
}

public func classUseAfterConsume(_ x: borrowing Klass) { // expected-error {{'x' is borrowed and cannot be consumed}}
    let x2 = x // expected-error {{'x2' consumed more than once}}
               // expected-note @-1 {{consumed here}}
    borrowVal(x2)
    consumeVal(x2) // expected-note {{consumed here}}
    consumeVal(x2) // expected-note {{consumed again here}}
}

public func classUseAfterConsumeArg(_ x2: borrowing Klass) { // expected-error {{'x2' is borrowed and cannot be consumed}}
    borrowVal(x2)
    consumeVal(x2) // expected-note {{consumed here}}
    consumeVal(x2) // expected-note {{consumed here}}
}

public func classUseAfterConsumeOwnedArg(_ x2: __owned Klass) { // expected-error {{'x2' consumed more than once}}
    borrowVal(x2)
    consumeVal(x2) // expected-note {{consumed here}}
    consumeVal(x2) // expected-note {{consumed again here}}
}

public func classUseAfterConsumeOwnedArg2(_ x2: consuming Klass) { // expected-error {{'x2' consumed more than once}}
    borrowVal(x2)
    consumeVal(x2) // expected-note {{consumed here}}
    consumeVal(x2) // expected-note {{consumed again here}}
}

public func classDoubleConsume(_ x: borrowing Klass) { // expected-error {{'x' is borrowed and cannot be consumed}}
    let x2 = x  // expected-error {{'x2' consumed more than once}}
                // expected-note @-1 {{consumed here}}
    consumeVal(x2) // expected-note {{consumed here}}
    consumeVal(x2) // expected-note {{consumed again here}}
}

public func classDoubleConsumeArg(_ x2: borrowing Klass) { // expected-error {{'x2' is borrowed and cannot be consumed}}
    consumeVal(x2) // expected-note {{consumed here}}
    consumeVal(x2) // expected-note {{consumed here}}
}

public func classDoubleConsumeOwnedArg(_ x2: __owned Klass) { // expected-error {{'x2' consumed more than once}}
    consumeVal(x2) // expected-note {{consumed here}}
    consumeVal(x2) // expected-note {{consumed again here}}
}

public func classDoubleConsumeOwnedArg2(_ x2: consuming Klass) { // expected-error {{'x2' consumed more than once}}
    consumeVal(x2) // expected-note {{consumed here}}
    consumeVal(x2) // expected-note {{consumed again here}}
}

public func classLoopConsume(_ x: borrowing Klass) { // expected-error {{'x' is borrowed and cannot be consumed}}
    let x2 = x // expected-error {{'x2' consumed in a loop}}
               // expected-note @-1 {{consumed here}}
    for _ in 0..<1024 {
        consumeVal(x2) // expected-note {{consumed here}}
    }
}

public func classLoopConsumeArg(_ x2: borrowing Klass) { // expected-error {{'x2' is borrowed and cannot be consumed}}
    for _ in 0..<1024 {
        consumeVal(x2) // expected-note {{consumed here}}
    }
}

public func classLoopConsumeOwnedArg(_ x2: __owned Klass) { // expected-error {{'x2' consumed in a loop}}
    for _ in 0..<1024 {
        consumeVal(x2) // expected-note {{consumed here}}
    }
}

public func classLoopConsumeOwnedArg2(_ x2: consuming Klass) { // expected-error {{'x2' consumed in a loop}}
    for _ in 0..<1024 {
        consumeVal(x2) // expected-note {{consumed here}}
    }
}

public func classDiamond(_ x: borrowing Klass) { // expected-error {{'x' is borrowed and cannot be consumed}}
    let x2 = x // expected-note {{consumed here}}
    if boolValue {
        consumeVal(x2)
    } else {
        consumeVal(x2)
    }
}

public func classDiamondArg(_ x2: borrowing Klass) { // expected-error {{'x2' is borrowed and cannot be consumed}}
    if boolValue {
        consumeVal(x2) // expected-note {{consumed here}}
    } else {
        consumeVal(x2) // expected-note {{consumed here}}
    }
}

public func classDiamondOwnedArg(_ x2: __owned Klass) {
    if boolValue {
        consumeVal(x2)
    } else {
        consumeVal(x2)
    }
}

public func classDiamondOwnedArg2(_ x2: consuming Klass) {
    if boolValue {
        consumeVal(x2)
    } else {
        consumeVal(x2)
    }
}

public func classDiamondInLoop(_ x: borrowing Klass) { // expected-error {{'x' is borrowed and cannot be consumed}}
    let x2 = x // expected-error {{'x2' consumed in a loop}}
    // expected-note @-1 {{consumed here}}
    // expected-error @-2 {{'x2' consumed more than once}}
    for _ in 0..<1024 {
      if boolValue {
          consumeVal(x2) // expected-note {{consumed here}}
      } else {
          consumeVal(x2) // expected-note {{consumed here}}
          // expected-note @-1 {{consumed again here}}
      }
    }
}

public func classDiamondInLoopArg(_ x2: borrowing Klass) { // expected-error {{'x2' is borrowed and cannot be consumed}}
    for _ in 0..<1024 {
      if boolValue {
          consumeVal(x2) // expected-note {{consumed here}}
      } else {
          consumeVal(x2) // expected-note {{consumed here}}
      }
    }
}

public func classDiamondInLoopOwnedArg(_ x2: __owned Klass) { // expected-error {{'x2' consumed more than once}}
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

public func classDiamondInLoopOwnedArg2(_ x2: consuming Klass) { // expected-error {{'x2' consumed more than once}}
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

public func classAssignToVar1(_ x: borrowing Klass) { // expected-error {{'x' is borrowed and cannot be consumed}}
    let x2 = x // expected-error {{'x2' consumed more than once}}
               // expected-note @-1 {{consumed here}}
    var x3 = x2 // expected-note {{consumed here}}
    x3 = x2 // expected-note {{consumed again here}}
    x3 = x // expected-note {{consumed here}}
    consumeVal(x3)
}

public func classAssignToVar1Arg(_ x: borrowing Klass, _ x2: borrowing Klass) { // expected-error {{'x2' is borrowed and cannot be consumed}}
                // expected-error @-1 {{'x' is borrowed and cannot be consumed}}
    var x3 = x2 // expected-note {{consumed here}}
    x3 = x2 // expected-note {{consumed here}}
    x3 = x // expected-note {{consumed here}}
    consumeVal(x3)
}

// NOTE: consumeVal(x3) shouldn't be marked! This is most likely due to some form of
// load forwarding. We may need to make predictable mem opts more conservative
// with move only var.
public func classAssignToVar1OwnedArg(_ x: borrowing Klass, _ x2: __owned Klass) { // expected-error {{'x2' consumed more than once}}
    // expected-error @-1 {{'x' is borrowed and cannot be consumed}}
    var x3 = x2 // expected-note {{consumed here}}
    x3 = x2 // expected-note {{consumed again here}}
    x3 = x // expected-note {{consumed here}}
    consumeVal(x3)
}

public func classAssignToVar1OwnedArg2(_ x: borrowing Klass, _ x2: consuming Klass) { // expected-error {{'x2' consumed more than once}}
    // expected-error @-1 {{'x' is borrowed and cannot be consumed}}
    var x3 = x2 // expected-note {{consumed here}}
    x3 = x2 // expected-note {{consumed again here}}
    x3 = x // expected-note {{consumed here}}
    consumeVal(x3)
}

public func classAssignToVar2(_ x: borrowing Klass) { // expected-error {{'x' is borrowed and cannot be consumed}}
    let x2 = x // expected-error {{'x2' consumed more than once}}
               // expected-note @-1 {{consumed here}}
    var x3 = x2 // expected-note {{consumed here}}
    x3 = x2 // expected-note {{consumed again here}}
    borrowVal(x3)
}

public func classAssignToVar2Arg(_ x2: borrowing Klass) { // expected-error {{'x2' is borrowed and cannot be consumed}}
    var x3 = x2 // expected-note {{consumed here}}
    x3 = x2 // expected-note {{consumed here}}
    borrowVal(x3)
}

public func classAssignToVar2OwnedArg(_ x2: __owned Klass) { // expected-error {{'x2' consumed more than once}}
    var x3 = x2 // expected-note {{consumed here}}
    x3 = x2 // expected-note {{consumed again here}}
    borrowVal(x3)
}

public func classAssignToVar2OwnedArg2(_ x2: consuming Klass) { // expected-error {{'x2' consumed more than once}}
    var x3 = x2 // expected-note {{consumed here}}
    x3 = x2 // expected-note {{consumed again here}}
    borrowVal(x3)
}

// NOTE: consumeVal(x3) should not be marked.
public func classAssignToVar3(_ x: borrowing Klass) { // expected-error {{'x' is borrowed and cannot be consumed}}
    let x2 = x // expected-note {{consumed here}}
    var x3 = x2
    x3 = x // expected-note {{consumed here}}
    consumeVal(x3)
}

// NOTE: consumeVal(x3) is a bug.
public func classAssignToVar3Arg(_ x: borrowing Klass, _ x2: borrowing Klass) { // expected-error {{'x2' is borrowed and cannot be consumed}}
                                                            // expected-error @-1 {{'x' is borrowed and cannot be consumed}}
    var x3 = x2 // expected-note {{consumed here}}
    x3 = x // expected-note {{consumed here}}
    consumeVal(x3)
}

public func classAssignToVar3OwnedArg(_ x: borrowing Klass, _ x2: __owned Klass) { // expected-error {{'x' is borrowed and cannot be consumed}}
    var x3 = x2
    x3 = x // expected-note {{consumed here}}
    consumeVal(x3)
}

public func classAssignToVar3OwnedArg2(_ x: borrowing Klass, _ x2: consuming Klass) { // expected-error {{'x' is borrowed and cannot be consumed}}
    var x3 = x2
    x3 = x // expected-note {{consumed here}}
    consumeVal(x3)
}

public func classAssignToVar4(_ x: borrowing Klass) { // expected-error {{'x' is borrowed and cannot be consumed}}
    let x2 = x // expected-error {{'x2' consumed more than once}}
               // expected-note @-1 {{consumed here}}
    let x3 = x2 // expected-note {{consumed here}}
    consumeVal(x2) // expected-note {{consumed again here}}
    consumeVal(x3)
}

public func classAssignToVar4Arg(_ x2: borrowing Klass) { // expected-error {{'x2' is borrowed and cannot be consumed}}
    let x3 = x2 // expected-note {{consumed here}}
    consumeVal(x2) // expected-note {{consumed here}}
    consumeVal(x3)
}

public func classAssignToVar4OwnedArg(_ x2: __owned Klass) { // expected-error {{'x2' consumed more than once}}
    let x3 = x2 // expected-note {{consumed here}}
    consumeVal(x2) // expected-note {{consumed again here}}
    consumeVal(x3)
}

public func classAssignToVar4OwnedArg2(_ x2: consuming Klass) { // expected-error {{'x2' consumed more than once}}
    let x3 = x2 // expected-note {{consumed here}}
    consumeVal(x2) // expected-note {{consumed again here}}
    consumeVal(x3)
}

public func classAssignToVar5(_ x: borrowing Klass) {  // expected-error {{'x' is borrowed and cannot be consumed}}
    let x2 = x // expected-error {{'x2' used after consume}}
    // expected-note @-1 {{consumed here}}
    var x3 = x2 // expected-note {{consumed here}}
    borrowVal(x2) // expected-note {{used here}}
    x3 = x // expected-note {{consumed here}}
    consumeVal(x3)
}

public func classAssignToVar5Arg(_ x: borrowing Klass, _ x2: borrowing Klass) { // expected-error {{'x2' is borrowed and cannot be consumed}}
                                                            // expected-error @-1 {{'x' is borrowed and cannot be consumed}}
    var x3 = x2 // expected-note {{consumed here}}
    borrowVal(x2)
    x3 = x // expected-note {{consumed here}}
    consumeVal(x3)
}

public func classAssignToVar5OwnedArg(_ x: borrowing Klass, _ x2: __owned Klass) { // expected-error {{'x2' used after consume}}
                                                                         // expected-error @-1 {{'x' is borrowed and cannot be consumed}}
    var x3 = x2 // expected-note {{consumed here}}
    borrowVal(x2) // expected-note {{used here}}
    x3 = x // expected-note {{consumed here}}
    consumeVal(x3)
}

public func classAssignToVar5OwnedArg2(_ x: borrowing Klass, _ x2: consuming Klass) { // expected-error {{'x2' used after consume}}
                                                                         // expected-error @-1 {{'x' is borrowed and cannot be consumed}}
    var x3 = x2 // expected-note {{consumed here}}
    borrowVal(x2) // expected-note {{used here}}
    x3 = x // expected-note {{consumed here}}
    consumeVal(x3)
}

public func classAccessAccessField(_ x: borrowing Klass) { // expected-error {{'x' is borrowed and cannot be consumed}}
    let x2 = x // expected-note {{consumed here}}
    borrowVal(x2.k)
    for _ in 0..<1024 {
        borrowVal(x2.k)
    }
}

public func classAccessAccessFieldArg(_ x2: borrowing Klass) {
    borrowVal(x2.k)
    for _ in 0..<1024 {
        borrowVal(x2.k)
    }
}

public func classAccessAccessFieldOwnedArg(_ x2: __owned Klass) {
    borrowVal(x2.k)
    for _ in 0..<1024 {
        borrowVal(x2.k)
    }
}

public func classAccessAccessFieldOwnedArg2(_ x2: consuming Klass) {
    borrowVal(x2.k)
    for _ in 0..<1024 {
        borrowVal(x2.k)
    }
}

public func classAccessConsumeField(_ x: borrowing Klass) { // expected-error {{'x' is borrowed and cannot be consumed}}
    let x2 = x // expected-note {{consumed here}}
    consumeVal(x2.k) // expected-error {{cannot consume noncopyable stored property 'x2.k' of a class}}
    for _ in 0..<1024 {
        consumeVal(x2.k) // expected-error {{cannot consume noncopyable stored property 'x2.k' of a class}}
    }
}

public func classAccessConsumeFieldArg(_ x2: borrowing Klass) {
    consumeVal(x2.k) // expected-error {{cannot consume noncopyable stored property 'x2.k' of a class}}
    for _ in 0..<1024 {
        consumeVal(x2.k) // expected-error {{cannot consume noncopyable stored property 'x2.k' of a class}}
    }
}

public func classAccessConsumeFieldOwnedArg(_ x2: __owned Klass) {
    consumeVal(x2.k) // expected-error {{cannot consume noncopyable stored property 'x2.k' of a class}}
    for _ in 0..<1024 {
        consumeVal(x2.k) // expected-error {{cannot consume noncopyable stored property 'x2.k' of a class}}
    }
}

public func classAccessConsumeFieldOwnedArg2(_ x2: consuming Klass) {
    consumeVal(x2.k) // expected-error {{cannot consume noncopyable stored property 'x2.k' of a class}}
    for _ in 0..<1024 {
        consumeVal(x2.k) // expected-error {{cannot consume noncopyable stored property 'x2.k' of a class}}
    }
}

extension Klass {
    func testNoUseSelf() { // expected-error {{'self' is borrowed and cannot be consumed}}
        let x = self // expected-note {{consumed here}}
        let _ = x
    }
}

/////////////////
// Final Class //
/////////////////

public func finalClassSimpleChainTest(_ x: borrowing FinalKlass) { // expected-error {{'x' is borrowed and cannot be consumed}}
    let x2 = x // expected-note {{consumed here}}
    let y2 = x2
    let k2 = y2
    borrowVal(k2)
}

public func finalClassSimpleChainTestArg(_ x2: borrowing FinalKlass) { // expected-error {{'x2' is borrowed and cannot be consumed}}
    let y2 = x2 // expected-note {{consumed here}}
    let k2 = y2
    borrowVal(k2)
}

public func finalClassSimpleChainTestOwnedArg(_ x2: __owned FinalKlass) {
    let y2 = x2
    let k2 = y2
    borrowVal(k2)
}

public func finalClassSimpleChainTestOwnedArg2(_ x2: consuming FinalKlass) {
    let y2 = x2
    let k2 = y2
    borrowVal(k2)
}

public func finalClassSimpleNonConsumingUseTest(_ x: borrowing FinalKlass) { // expected-error {{'x' is borrowed and cannot be consumed}}
    let x2 = x // expected-note {{consumed here}}
    borrowVal(x2)
}

public func finalClassSimpleNonConsumingUseTestArg(_ x2: borrowing FinalKlass) {
    borrowVal(x2)
}

public func finalClassSimpleNonConsumingUseTestOwnedArg(_ x2: __owned FinalKlass) {
    borrowVal(x2)
}

public func finalClassSimpleNonConsumingUseTestOwnedArg2(_ x2: consuming FinalKlass) {
    borrowVal(x2)
}

public func finalClassMultipleNonConsumingUseTest(_ x: borrowing FinalKlass) { // expected-error {{'x' is borrowed and cannot be consumed}}
    let x2 = x // expected-note {{consumed here}}
    borrowVal(x2)
    borrowVal(x2)
    consumeVal(x2)
}

public func finalClassMultipleNonConsumingUseTestArg(_ x2: borrowing FinalKlass) { // expected-error {{'x2' is borrowed and cannot be consumed}}
    borrowVal(x2)
    borrowVal(x2)
    consumeVal(x2) // expected-note {{consumed here}}
}

public func finalClassMultipleNonConsumingUseTestownedArg(_ x2: __owned FinalKlass) {
    borrowVal(x2)
    borrowVal(x2)
    consumeVal(x2)
}

public func finalClassMultipleNonConsumingUseTestownedArg2(_ x2: consuming FinalKlass) {
    borrowVal(x2)
    borrowVal(x2)
    consumeVal(x2)
}

public func finalClassUseAfterConsume(_ x: borrowing FinalKlass) { // expected-error {{'x' is borrowed and cannot be consumed}}
    let x2 = x // expected-error {{'x2' consumed more than once}}
               // expected-note @-1 {{consumed here}}
    borrowVal(x2)
    consumeVal(x2) // expected-note {{consumed here}}
    consumeVal(x2) // expected-note {{consumed again here}}
}

public func finalClassUseAfterConsumeArg(_ x2: borrowing FinalKlass) { // expected-error {{'x2' is borrowed and cannot be consumed}}
    borrowVal(x2)
    consumeVal(x2) // expected-note {{consumed here}}
    consumeVal(x2) // expected-note {{consumed here}}
}

public func finalClassUseAfterConsumeOwnedArg(_ x2: __owned FinalKlass) { // expected-error {{'x2' consumed more than once}}
    borrowVal(x2)
    consumeVal(x2) // expected-note {{consumed here}}
    consumeVal(x2) // expected-note {{consumed again here}}
}

public func finalClassUseAfterConsumeOwnedArg2(_ x2: consuming FinalKlass) { // expected-error {{'x2' consumed more than once}}
    borrowVal(x2)
    consumeVal(x2) // expected-note {{consumed here}}
    consumeVal(x2) // expected-note {{consumed again here}}
}

public func finalClassDoubleConsume(_ x: borrowing FinalKlass) { // expected-error {{'x' is borrowed and cannot be consumed}}
    let x2 = x  // expected-error {{'x2' consumed more than once}}
                // expected-note @-1 {{consumed here}}
    consumeVal(x2) // expected-note {{consumed here}}
    consumeVal(x2) // expected-note {{consumed again here}}
}

public func finalClassDoubleConsumeArg(_ x2: borrowing FinalKlass) { // expected-error {{'x2' is borrowed and cannot be consumed}}
    consumeVal(x2) // expected-note {{consumed here}}
    consumeVal(x2) // expected-note {{consumed here}}
}

public func finalClassDoubleConsumeownedArg(_ x2: __owned FinalKlass) { // expected-error {{'x2' consumed more than once}}
    consumeVal(x2) // expected-note {{consumed here}}
    consumeVal(x2) // expected-note {{consumed again here}}
}

public func finalClassDoubleConsumeownedArg2(_ x2: consuming FinalKlass) { // expected-error {{'x2' consumed more than once}}
    consumeVal(x2) // expected-note {{consumed here}}
    consumeVal(x2) // expected-note {{consumed again here}}
}

public func finalClassLoopConsume(_ x: borrowing FinalKlass) { // expected-error {{'x' is borrowed and cannot be consumed}}
    let x2 = x // expected-error {{'x2' consumed in a loop}}
               // expected-note @-1 {{consumed here}}
    for _ in 0..<1024 {
        consumeVal(x2) // expected-note {{consumed here}}
    }
}

public func finalClassLoopConsumeArg(_ x2: borrowing FinalKlass) { // expected-error {{'x2' is borrowed and cannot be consumed}}
    for _ in 0..<1024 {
        consumeVal(x2) // expected-note {{consumed here}}
    }
}

public func finalClassLoopConsumeOwnedArg(_ x2: __owned FinalKlass) { // expected-error {{'x2' consumed in a loop}}
    for _ in 0..<1024 {
        consumeVal(x2) // expected-note {{consumed here}}
    }
}

public func finalClassLoopConsumeOwnedArg2(_ x2: consuming FinalKlass) { // expected-error {{'x2' consumed in a loop}}
    for _ in 0..<1024 {
        consumeVal(x2) // expected-note {{consumed here}}
    }
}

public func finalClassDiamond(_ x: borrowing FinalKlass) { // expected-error {{'x' is borrowed and cannot be consumed}}
    let x2 = x // expected-note {{consumed here}}
    if boolValue {
        consumeVal(x2)
    } else {
        consumeVal(x2)
    }
}

public func finalClassDiamondArg(_ x2: borrowing FinalKlass) { // expected-error {{'x2' is borrowed and cannot be consumed}}
    if boolValue {
        consumeVal(x2) // expected-note {{consumed here}}
    } else {
        consumeVal(x2) // expected-note {{consumed here}}
    }
}

public func finalClassDiamondOwnedArg(_ x2: __owned FinalKlass) {
    if boolValue {
        consumeVal(x2)
    } else {
        consumeVal(x2)
    }
}

public func finalClassDiamondOwnedArg2(_ x2: consuming FinalKlass) {
    if boolValue {
        consumeVal(x2)
    } else {
        consumeVal(x2)
    }
}

public func finalClassDiamondInLoop(_ x: borrowing FinalKlass) { // expected-error {{'x' is borrowed and cannot be consumed}}
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

public func finalClassDiamondInLoopArg(_ x2: borrowing FinalKlass) { // expected-error {{'x2' is borrowed and cannot be consumed}}
    for _ in 0..<1024 {
      if boolValue {
          consumeVal(x2) // expected-note {{consumed here}}
      } else {
          consumeVal(x2) // expected-note {{consumed here}}
      }
    }
}

public func finalClassDiamondInLoopOwnedArg(_ x2: __owned FinalKlass) { // expected-error {{'x2' consumed more than once}}
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

public func finalClassDiamondInLoopOwnedArg2(_ x2: consuming FinalKlass) { // expected-error {{'x2' consumed more than once}}
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

public func finalClassAssignToVar1(_ x: borrowing FinalKlass) { // expected-error {{'x' is borrowed and cannot be consumed}}
    let x2 = x // expected-error {{'x2' consumed more than once}}
               // expected-note @-1 {{consumed here}}
    var x3 = x2 // expected-note {{consumed here}}
    x3 = x2 // expected-note {{consumed again here}}
    x3 = x // expected-note {{consumed here}}
    consumeVal(x3)
}

public func finalClassAssignToVar1Arg(_ x: borrowing FinalKlass, _ x2: borrowing FinalKlass) { // expected-error {{'x2' is borrowed and cannot be consumed}}
                                                                           // expected-error @-1 {{'x' is borrowed and cannot be consumed}}
    var x3 = x2 // expected-note {{consumed here}}
    x3 = x2 // expected-note {{consumed here}}
    x3 = x // expected-note {{consumed here}}
    consumeVal(x3)
}

public func finalClassAssignToVar1OwnedArg(_ x: borrowing FinalKlass, _ x2: __owned FinalKlass) { // expected-error {{'x2' consumed more than once}}
                                                                                        // expected-error @-1 {{'x' is borrowed and cannot be consumed}}
    var x3 = x2 // expected-note {{consumed here}}
    x3 = x2 // expected-note {{consumed again here}}
    x3 = x // expected-note {{consumed here}}
    consumeVal(x3)
}

public func finalClassAssignToVar1OwnedArg2(_ x: borrowing FinalKlass, _ x2: consuming FinalKlass) { // expected-error {{'x2' consumed more than once}}
                                                                                        // expected-error @-1 {{'x' is borrowed and cannot be consumed}}
    var x3 = x2 // expected-note {{consumed here}}
    x3 = x2 // expected-note {{consumed again here}}
    x3 = x // expected-note {{consumed here}}
    consumeVal(x3)
}

public func finalClassAssignToVar2(_ x: borrowing FinalKlass) { // expected-error {{'x' is borrowed and cannot be consumed}}
    let x2 = x // expected-error {{'x2' consumed more than once}}
               // expected-note @-1 {{consumed here}}
    var x3 = x2 // expected-note {{consumed here}}
    x3 = x2 // expected-note {{consumed again here}}
    borrowVal(x3)
}

public func finalClassAssignToVar2Arg(_ x2: borrowing FinalKlass) { // expected-error {{'x2' is borrowed and cannot be consumed}}
    var x3 = x2 // expected-note {{consumed here}}
    x3 = x2 // expected-note {{consumed here}}
    borrowVal(x3)
}

public func finalClassAssignToVar2OwnedArg(_ x2: __owned FinalKlass) { // expected-error {{'x2' consumed more than once}}
    var x3 = x2 // expected-note {{consumed here}}
    x3 = x2 // expected-note {{consumed again here}}
    borrowVal(x3)
}

public func finalClassAssignToVar2OwnedArg2(_ x2: consuming FinalKlass) { // expected-error {{'x2' consumed more than once}}
    var x3 = x2 // expected-note {{consumed here}}
    x3 = x2 // expected-note {{consumed again here}}
    borrowVal(x3)
}

public func finalClassAssignToVar3(_ x: borrowing FinalKlass) { // expected-error {{'x' is borrowed and cannot be consumed}}
    let x2 = x // expected-note {{consumed here}}
    var x3 = x2
    x3 = x // expected-note {{consumed here}}
    consumeVal(x3)
}

public func finalClassAssignToVar3Arg(_ x: borrowing FinalKlass, _ x2: borrowing FinalKlass) { // expected-error {{'x2' is borrowed and cannot be consumed}}
                                                                           // expected-error @-1 {{'x' is borrowed and cannot be consumed}}
    var x3 = x2 // expected-note {{consumed here}}
    x3 = x // expected-note {{consumed here}}
    consumeVal(x3)
}

public func finalClassAssignToVar3OwnedArg(_ x: borrowing FinalKlass, _ x2: __owned FinalKlass) { // expected-error {{'x' is borrowed and cannot be consumed}}
    var x3 = x2
    x3 = x // expected-note {{consumed here}}
    consumeVal(x3)
}

public func finalClassAssignToVar3OwnedArg2(_ x: borrowing FinalKlass, _ x2: consuming FinalKlass) { // expected-error {{'x' is borrowed and cannot be consumed}}
    var x3 = x2
    x3 = x // expected-note {{consumed here}}
    consumeVal(x3)
}

public func finalClassAssignToVar4(_ x: borrowing FinalKlass) { // expected-error {{'x' is borrowed and cannot be consumed}}
    let x2 = x // expected-error {{'x2' consumed more than once}}
               // expected-note @-1 {{consumed here}}
    let x3 = x2 // expected-note {{consumed here}}
    consumeVal(x2) // expected-note {{consumed again here}}
    consumeVal(x3)
}

public func finalClassAssignToVar4Arg(_ x2: borrowing FinalKlass) { // expected-error {{'x2' is borrowed and cannot be consumed}}
    let x3 = x2 // expected-note {{consumed here}}
    consumeVal(x2) // expected-note {{consumed here}}
    consumeVal(x3)
}

public func finalClassAssignToVar4OwnedArg(_ x2: __owned FinalKlass) { // expected-error {{'x2' consumed more than once}}
    let x3 = x2 // expected-note {{consumed here}}
    consumeVal(x2) // expected-note {{consumed again here}}
    consumeVal(x3)
}

public func finalClassAssignToVar4OwnedArg2(_ x2: consuming FinalKlass) { // expected-error {{'x2' consumed more than once}}
    let x3 = x2 // expected-note {{consumed here}}
    consumeVal(x2) // expected-note {{consumed again here}}
    consumeVal(x3)
}

public func finalClassAssignToVar5(_ x: borrowing FinalKlass) { // expected-error {{'x' is borrowed and cannot be consumed}}
    let x2 = x // expected-error {{'x2' used after consume}}
               // expected-note @-1 {{consumed here}}
    var x3 = x2 // expected-note {{consumed here}}
    borrowVal(x2) // expected-note {{used here}}
    x3 = x // expected-note {{consumed here}}
    consumeVal(x3)
}

public func finalClassAssignToVar5Arg(_ x: borrowing FinalKlass, _ x2: borrowing FinalKlass) { // expected-error {{'x2' is borrowed and cannot be consumed}}
                                                                           // expected-error @-1 {{'x' is borrowed and cannot be consumed}}
    var x3 = x2 // expected-note {{consumed here}}
    borrowVal(x2)
    x3 = x // expected-note {{consumed here}}
    consumeVal(x3)
}

public func finalClassAssignToVar5OwnedArg(_ x: borrowing FinalKlass, _ x2: __owned FinalKlass) { // expected-error {{'x2' used after consume}}
                                                                                        // expected-error @-1 {{'x' is borrowed and cannot be consumed}}
    var x3 = x2 // expected-note {{consumed here}}
    borrowVal(x2) // expected-note {{used here}}
    x3 = x // expected-note {{consumed here}}
    consumeVal(x3)
}

public func finalClassAssignToVar5OwnedArg2(_ x: borrowing FinalKlass, _ x2: consuming FinalKlass) { // expected-error {{'x2' used after consume}}
                                                                                        // expected-error @-1 {{'x' is borrowed and cannot be consumed}}
    var x3 = x2 // expected-note {{consumed here}}
    borrowVal(x2) // expected-note {{used here}}
    x3 = x // expected-note {{consumed here}}
    consumeVal(x3)
}

public func finalClassAccessField(_ x: borrowing FinalKlass) { // expected-error {{'x' is borrowed and cannot be consumed}}
    let x2 = x // expected-note {{consumed here}}
    borrowVal(x2.k)
    for _ in 0..<1024 {
        borrowVal(x2.k)
    }
}

public func finalClassAccessFieldArg(_ x2: borrowing FinalKlass) {
    borrowVal(x2.k)
    for _ in 0..<1024 {
        borrowVal(x2.k)
    }
}

public func finalClassAccessFieldOwnedArg(_ x2: __owned FinalKlass) {
    borrowVal(x2.k)
    for _ in 0..<1024 {
        borrowVal(x2.k)
    }
}

public func finalClassAccessFieldOwnedArg2(_ x2: consuming FinalKlass) {
    borrowVal(x2.k)
    for _ in 0..<1024 {
        borrowVal(x2.k)
    }
}

public func finalClassConsumeField(_ x: borrowing FinalKlass) { // expected-error {{'x' is borrowed and cannot be consumed}}
    let x2 = x // expected-note {{consumed here}}
    consumeVal(x2.k) // expected-error {{cannot consume noncopyable stored property 'x2.k' of a class}}
    for _ in 0..<1024 {
        consumeVal(x2.k) // expected-error {{cannot consume noncopyable stored property 'x2.k' of a class}}
    }
}

public func finalClassConsumeFieldArg(_ x2: borrowing FinalKlass) {
    consumeVal(x2.k) // expected-error {{cannot consume noncopyable stored property 'x2.k' of a class}}
    for _ in 0..<1024 {
        consumeVal(x2.k) // expected-error {{cannot consume noncopyable stored property 'x2.k' of a class}}
    }
}

public func finalClassConsumeFieldArg(_ x2: __owned FinalKlass) {
    // No diagnostic here since class is a reference type and we are not copying
    // the class, we are copying its field.
    consumeVal(x2.k) // expected-error {{cannot consume noncopyable stored property 'x2.k' of a class}}
    for _ in 0..<1024 {
        consumeVal(x2.k) // expected-error {{cannot consume noncopyable stored property 'x2.k' of a class}}
    }
}

public func finalClassConsumeFieldArg2(_ x2: consuming FinalKlass) {
    // No diagnostic here since class is a reference type and we are not copying
    // the class, we are copying its field.
    consumeVal(x2.k) // expected-error {{cannot consume noncopyable stored property 'x2.k' of a class}}
    for _ in 0..<1024 {
        consumeVal(x2.k) // expected-error {{cannot consume noncopyable stored property 'x2.k' of a class}}
    }
}

//////////////////////
// Aggregate Struct //
//////////////////////

@_moveOnly
public struct KlassPair {
    var lhs: Klass
    var rhs: Klass
}

@_moveOnly
public struct AggStruct {
    var lhs: Klass
    var center: Int32
    var rhs: Klass
    var pair: KlassPair
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
    // expected-error @-1 {{'x2' consumed in a loop}}
    // expected-error @-2 {{'x2' consumed more than once}}
    consumeVal(x2.lhs) // expected-note {{consumed here}}
    for _ in 0..<1024 {
        consumeVal(x2.lhs) // expected-note {{consumed here}}
        // expected-note @-1 {{consumed again here}}
    }
}

public func aggStructConsumeFieldArg(_ x2: borrowing AggStruct) {
    // expected-error @-1 {{cannot use 'x2' after partial consume}}
    // expected-error @-2 {{cannot use 'x2' after partial consume}}
    consumeVal(x2.lhs) // expected-note {{partially consumed here}}
    for _ in 0..<1024 {
        consumeVal(x2.lhs) // expected-note {{partially consumed here}}
    }
}

public func aggStructConsumeFieldOwnedArg(_ x2: __owned AggStruct) {
    // expected-error @-1 {{'x2' consumed in a loop}}
    // expected-error @-2 {{'x2' consumed more than once}}
    consumeVal(x2.lhs) // expected-note {{consumed here}}
    for _ in 0..<1024 {
        consumeVal(x2.lhs) // expected-note {{consumed here}}
        // expected-note @-1 {{consumed again here}}
    }
}

public func aggStructConsumeFieldOwnedArg2(_ x2: consuming AggStruct) {
    // expected-error @-1 {{'x2' consumed in a loop}}
    // expected-error @-2 {{'x2' consumed more than once}}
    consumeVal(x2.lhs) // expected-note {{consumed here}}
    for _ in 0..<1024 {
        consumeVal(x2.lhs) // expected-note {{consumed here}}
        // expected-note @-1 {{consumed again here}}
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
    // expected-error @-1 {{'x2' consumed in a loop}}
    // expected-error @-2 {{'x2' consumed more than once}}
    consumeVal(x2.pair.lhs) // expected-note {{consumed here}}
    for _ in 0..<1024 {
        consumeVal(x2.pair.lhs) // expected-note {{consumed here}}
        // expected-note @-1 {{consumed again here}}
    }
}

public func aggStructConsumeGrandFieldArg(_ x2: borrowing AggStruct) {
    // expected-error @-1 {{cannot use 'x2' after partial consume}}
    // expected-error @-2 {{cannot use 'x2' after partial consume}}
    consumeVal(x2.pair.lhs) // expected-note {{partially consumed here}}
    for _ in 0..<1024 {
        consumeVal(x2.pair.lhs) // expected-note {{partially consumed here}}
    }
}

public func aggStructConsumeGrandFieldOwnedArg(_ x2: __owned AggStruct) {
    // expected-error @-1 {{'x2' consumed in a loop}}
    // expected-error @-2 {{'x2' consumed more than once}}
    consumeVal(x2.pair.lhs) // expected-note {{consumed here}}
    for _ in 0..<1024 {
        consumeVal(x2.pair.lhs) // expected-note {{consumed here}}
        // expected-note @-1 {{consumed again here}}
    }
}

public func aggStructConsumeGrandFieldOwnedArg2(_ x2: consuming AggStruct) {
    // expected-error @-1 {{'x2' consumed in a loop}}
    // expected-error @-2 {{'x2' consumed more than once}}
    consumeVal(x2.pair.lhs) // expected-note {{consumed here}}
    for _ in 0..<1024 {
        consumeVal(x2.pair.lhs) // expected-note {{consumed here}}
        // expected-note @-1 {{consumed again here}}
    }
}

public func aggStructConsumeFieldNoError(_ x2: __owned AggStruct) {
    if boolValue {
        consumeVal(x2.pair.lhs)
    } else {
        consumeVal(x2.pair.rhs)
    }
    consumeVal(x2.lhs)
}

public func aggStructConsumeFieldNoError2(_ x2: consuming AggStruct) {
    if boolValue {
        consumeVal(x2.pair.lhs)
    } else {
        consumeVal(x2.pair.rhs)
    }
    consumeVal(x2.lhs)
}

public func aggStructConsumeFieldError(_ x2: __owned AggStruct) {
    // expected-error @-1 {{'x2' used after consume}}
    if boolValue {
        consumeVal(x2.lhs)
    } else {
        consumeVal(x2.pair.rhs) // expected-note {{consumed here}}
    }
    borrowVal(x2.pair) // expected-note {{used here}}
}

public func aggStructConsumeFieldError2(_ x2: consuming AggStruct) {
    // expected-error @-1 {{'x2' used after consume}}
    if boolValue {
        consumeVal(x2.lhs)
    } else {
        consumeVal(x2.pair.rhs) // expected-note {{consumed here}}
    }
    borrowVal(x2.pair) // expected-note {{used here}}
}

public func aggStructConsumeFieldError3(_ x2: __owned AggStruct) {
    // expected-error @-1 {{'x2' consumed more than once}}
    // expected-error @-2 {{'x2' consumed more than once}}
    if boolValue {
        consumeVal(x2.lhs) // expected-note {{consumed here}}
    } else {
        consumeVal(x2.pair.rhs) // expected-note {{consumed here}}
    }
    consumeVal(x2)
    // expected-note @-1 {{consumed again here}}
    // expected-note @-2 {{consumed again here}}
}

public func aggStructConsumeFieldError4(_ x2: __owned AggStruct) {
    // expected-error @-1 {{'x2' consumed more than once}}
    // expected-error @-2 {{'x2' consumed more than once}}
    if boolValue {
        consumeVal(x2.lhs) // expected-note {{consumed here}}
    } else {
        consumeVal(x2.pair.rhs) // expected-note {{consumed here}}
    }
    consumeVal(x2)
    // expected-note @-1 {{consumed again here}}
    // expected-note @-2 {{consumed again here}}
}

//////////////////////////////
// Aggregate Generic Struct //
//////////////////////////////

@_moveOnly
public struct AggGenericStruct<T> { // FIXME: for better test coverage this should probably use the generic parameter!
    var lhs: Klass
    var rhs: UnsafeRawPointer
    var pair: KlassPair
}

public func aggGenericStructSimpleChainTest(_ x: borrowing AggGenericStruct<String>) { // expected-error {{'x' is borrowed and cannot be consumed}}
    let x2 = x // expected-note {{consumed here}}
    let y2 = x2
    let k2 = y2
    borrowVal(k2)
}

public func aggGenericStructSimpleChainTestArg(_ x2: borrowing AggGenericStruct<String>) { // expected-error {{'x2' is borrowed and cannot be consumed}}
    let y2 = x2 // expected-note {{consumed here}}
    let k2 = y2
    borrowVal(k2)
}

public func aggGenericStructSimpleChainTestOwnedArg(_ x2: __owned AggGenericStruct<String>) {
    let y2 = x2
    let k2 = y2
    borrowVal(k2)
}

public func aggGenericStructSimpleChainTestOwnedArg2(_ x2: consuming AggGenericStruct<String>) {
    let y2 = x2
    let k2 = y2
    borrowVal(k2)
}

public func aggGenericStructSimpleNonConsumingUseTest(_ x: borrowing AggGenericStruct<String>) { // expected-error {{'x' is borrowed and cannot be consumed}}
    let x2 = x // expected-note {{consumed here}}
    borrowVal(x2)
}

public func aggGenericStructSimpleNonConsumingUseTestArg(_ x2: borrowing AggGenericStruct<String>) {
    borrowVal(x2)
}

public func aggGenericStructSimpleNonConsumingUseTestOwnedArg(_ x2: __owned AggGenericStruct<String>) {
    borrowVal(x2)
}

public func aggGenericStructSimpleNonConsumingUseTestOwnedArg2(_ x2: consuming AggGenericStruct<String>) {
    borrowVal(x2)
}

public func aggGenericStructMultipleNonConsumingUseTest(_ x: borrowing AggGenericStruct<String>) { // expected-error {{'x' is borrowed and cannot be consumed}}
    let x2 = x // expected-note {{consumed here}}
    borrowVal(x2)
    borrowVal(x2)
    consumeVal(x2)
}

public func aggGenericStructMultipleNonConsumingUseTestArg(_ x2: borrowing AggGenericStruct<String>) { // expected-error {{'x2' is borrowed and cannot be consumed}}
    borrowVal(x2)
    borrowVal(x2)
    consumeVal(x2) // expected-note {{consumed here}}
}

public func aggGenericStructMultipleNonConsumingUseTestOwnedArg(_ x2: __owned AggGenericStruct<String>) {
    borrowVal(x2)
    borrowVal(x2)
    consumeVal(x2)
}

public func aggGenericStructMultipleNonConsumingUseTestOwnedArg2(_ x2: consuming AggGenericStruct<String>) {
    borrowVal(x2)
    borrowVal(x2)
    consumeVal(x2)
}

public func aggGenericStructUseAfterConsume(_ x: borrowing AggGenericStruct<String>) { // expected-error {{'x' is borrowed and cannot be consumed}}
    let x2 = x // expected-error {{'x2' consumed more than once}}
               // expected-note @-1 {{consumed here}}
    borrowVal(x2)
    consumeVal(x2) // expected-note {{consumed here}}
    consumeVal(x2) // expected-note {{consumed again here}}
}

public func aggGenericStructUseAfterConsumeArg(_ x2: borrowing AggGenericStruct<String>) { // expected-error {{'x2' is borrowed and cannot be consumed}}
    borrowVal(x2)
    consumeVal(x2) // expected-note {{consumed here}}
    consumeVal(x2) // expected-note {{consumed here}}
}

public func aggGenericStructUseAfterConsumeOwnedArg(_ x2: __owned AggGenericStruct<String>) { // expected-error {{'x2' consumed more than once}}
    borrowVal(x2)
    consumeVal(x2) // expected-note {{consumed here}}
    consumeVal(x2) // expected-note {{consumed again here}}
}

public func aggGenericStructUseAfterConsumeOwnedArg2(_ x2: consuming AggGenericStruct<String>) { // expected-error {{'x2' consumed more than once}}
    borrowVal(x2)
    consumeVal(x2) // expected-note {{consumed here}}
    consumeVal(x2) // expected-note {{consumed again here}}
}

public func aggGenericStructDoubleConsume(_ x: borrowing AggGenericStruct<String>) { // expected-error {{'x' is borrowed and cannot be consumed}}
    let x2 = x  // expected-error {{'x2' consumed more than once}}
                // expected-note @-1 {{consumed here}}
    consumeVal(x2) // expected-note {{consumed here}}
    consumeVal(x2) // expected-note {{consumed again here}}
}

public func aggGenericStructDoubleConsumeArg(_ x2: borrowing AggGenericStruct<String>) { // expected-error {{'x2' is borrowed and cannot be consumed}}
    consumeVal(x2) // expected-note {{consumed here}}
    consumeVal(x2) // expected-note {{consumed here}}
}

public func aggGenericStructDoubleConsumeOwnedArg(_ x2: __owned AggGenericStruct<String>) { // expected-error {{'x2' consumed more than once}}
    consumeVal(x2) // expected-note {{consumed here}}
    consumeVal(x2) // expected-note {{consumed again here}}
}

public func aggGenericStructDoubleConsumeOwnedArg2(_ x2: consuming AggGenericStruct<String>) { // expected-error {{'x2' consumed more than once}}
    consumeVal(x2) // expected-note {{consumed here}}
    consumeVal(x2) // expected-note {{consumed again here}}
}

public func aggGenericStructLoopConsume(_ x: borrowing AggGenericStruct<String>) { // expected-error {{'x' is borrowed and cannot be consumed}}
    let x2 = x // expected-error {{'x2' consumed in a loop}}
               // expected-note @-1 {{consumed here}}
    for _ in 0..<1024 {
        consumeVal(x2) // expected-note {{consumed here}}
    }
}

public func aggGenericStructLoopConsumeArg(_ x2: borrowing AggGenericStruct<String>) { // expected-error {{'x2' is borrowed and cannot be consumed}}
    for _ in 0..<1024 {
        consumeVal(x2) // expected-note {{consumed here}}
    }
}

public func aggGenericStructLoopConsumeOwnedArg(_ x2: __owned AggGenericStruct<String>) { // expected-error {{'x2' consumed in a loop}}
    for _ in 0..<1024 {
        consumeVal(x2) // expected-note {{consumed here}}
    }
}

public func aggGenericStructLoopConsumeOwnedArg2(_ x2: consuming AggGenericStruct<String>) { // expected-error {{'x2' consumed in a loop}}
    for _ in 0..<1024 {
        consumeVal(x2) // expected-note {{consumed here}}
    }
}

public func aggGenericStructDiamond(_ x: borrowing AggGenericStruct<String>) { // expected-error {{'x' is borrowed and cannot be consumed}}
    let x2 = x // expected-note {{consumed here}}
    if boolValue {
        consumeVal(x2)
    } else {
        consumeVal(x2)
    }
}

public func aggGenericStructDiamondArg(_ x2: borrowing AggGenericStruct<String>) { // expected-error {{'x2' is borrowed and cannot be consumed}}
    if boolValue {
        consumeVal(x2) // expected-note {{consumed here}}
    } else {
        consumeVal(x2) // expected-note {{consumed here}}
    }
}

public func aggGenericStructDiamondOwnedArg(_ x2: __owned AggGenericStruct<String>) {
    if boolValue {
        consumeVal(x2)
    } else {
        consumeVal(x2)
    }
}

public func aggGenericStructDiamondOwnedArg2(_ x2: consuming AggGenericStruct<String>) {
    if boolValue {
        consumeVal(x2)
    } else {
        consumeVal(x2)
    }
}

public func aggGenericStructDiamondInLoop(_ x: borrowing AggGenericStruct<String>) { // expected-error {{'x' is borrowed and cannot be consumed}}
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

public func aggGenericStructDiamondInLoopArg(_ x2: borrowing AggGenericStruct<String>) { // expected-error {{'x2' is borrowed and cannot be consumed}}
    for _ in 0..<1024 {
      if boolValue {
          consumeVal(x2) // expected-note {{consumed here}}
      } else {
          consumeVal(x2) // expected-note {{consumed here}}
      }
    }
}

public func aggGenericStructDiamondInLoopOwnedArg(_ x2: __owned AggGenericStruct<String>) { // expected-error {{'x2' consumed more than once}}
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

public func aggGenericStructDiamondInLoopOwnedArg2(_ x2: consuming AggGenericStruct<String>) { // expected-error {{'x2' consumed more than once}}
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

public func aggGenericStructAccessField(_ x: borrowing AggGenericStruct<String>) { // expected-error {{'x' is borrowed and cannot be consumed}}
    let x2 = x // expected-note {{consumed here}}
    borrowVal(x2.lhs)
    for _ in 0..<1024 {
        borrowVal(x2.lhs)
    }
}

public func aggGenericStructAccessFieldArg(_ x2: borrowing AggGenericStruct<String>) {
    borrowVal(x2.lhs)
    for _ in 0..<1024 {
        borrowVal(x2.lhs)
    }
}

public func aggGenericStructAccessFieldOwnedArg(_ x2: __owned AggGenericStruct<String>) {
    borrowVal(x2.lhs)
    for _ in 0..<1024 {
        borrowVal(x2.lhs)
    }
}

public func aggGenericStructAccessFieldOwnedArg2(_ x2: consuming AggGenericStruct<String>) {
    borrowVal(x2.lhs)
    for _ in 0..<1024 {
        borrowVal(x2.lhs)
    }
}

public func aggGenericStructConsumeField(_ x: borrowing AggGenericStruct<String>) { // expected-error {{'x' is borrowed and cannot be consumed}}
    let x2 = x // expected-note {{consumed here}}
    // expected-error @-1 {{'x2' consumed in a loop}}
    // expected-error @-2 {{'x2' consumed more than once}}
    consumeVal(x2.lhs) // expected-note {{consumed here}}
    for _ in 0..<1024 {
        consumeVal(x2.lhs) // expected-note {{consumed here}}
        // expected-note @-1 {{consumed again here}}
    }
}

public func aggGenericStructConsumeFieldArg(_ x2: borrowing AggGenericStruct<String>) {
    // expected-error @-1 {{cannot use 'x2' after partial consume}}
    // expected-error @-2 {{cannot use 'x2' after partial consume}}
    consumeVal(x2.lhs) // expected-note {{partially consumed here}}
    for _ in 0..<1024 {
        consumeVal(x2.lhs) // expected-note {{partially consumed here}}
    }
}

public func aggGenericStructConsumeFieldOwnedArg(_ x2: __owned AggGenericStruct<String>) {
    // expected-error @-1 {{'x2' consumed in a loop}}
    // expected-error @-2 {{'x2' consumed more than once}}
    consumeVal(x2.lhs) // expected-note {{consumed here}}
    for _ in 0..<1024 {
        consumeVal(x2.lhs) // expected-note {{consumed here}}
        // expected-note @-1 {{consumed again here}}
    }
}

public func aggGenericStructConsumeFieldOwnedArg2(_ x2: consuming AggGenericStruct<String>) {
    // expected-error @-1 {{'x2' consumed in a loop}}
    // expected-error @-2 {{'x2' consumed more than once}}
    consumeVal(x2.lhs) // expected-note {{consumed here}}
    for _ in 0..<1024 {
        consumeVal(x2.lhs) // expected-note {{consumed here}}
        // expected-note @-1 {{consumed again here}}
    }
}

public func aggGenericStructAccessGrandField(_ x: borrowing AggGenericStruct<String>) { // expected-error {{'x' is borrowed and cannot be consumed}}
    let x2 = x // expected-note {{consumed here}}
    borrowVal(x2.pair.lhs)
    for _ in 0..<1024 {
        borrowVal(x2.pair.lhs)
    }
}

public func aggGenericStructAccessGrandFieldArg(_ x2: borrowing AggGenericStruct<String>) {
    borrowVal(x2.pair.lhs)
    for _ in 0..<1024 {
        borrowVal(x2.pair.lhs)
    }
}

public func aggGenericStructAccessGrandFieldOwnedArg(_ x2: __owned AggGenericStruct<String>) {
    borrowVal(x2.pair.lhs)
    for _ in 0..<1024 {
        borrowVal(x2.pair.lhs)
    }
}

public func aggGenericStructAccessGrandFieldOwnedArg2(_ x2: consuming AggGenericStruct<String>) {
    borrowVal(x2.pair.lhs)
    for _ in 0..<1024 {
        borrowVal(x2.pair.lhs)
    }
}

public func aggGenericStructConsumeGrandField(_ x: borrowing AggGenericStruct<String>) { // expected-error {{'x' is borrowed and cannot be consumed}}
    let x2 = x // expected-note {{consumed here}}
    // expected-error @-1 {{'x2' consumed in a loop}}
    // expected-error @-2 {{'x2' consumed more than once}}
    consumeVal(x2.pair.lhs) // expected-note {{consumed here}}
    for _ in 0..<1024 {
        consumeVal(x2.pair.lhs) // expected-note {{consumed here}}
        // expected-note @-1 {{consumed again here}}
    }
}

public func aggGenericStructConsumeGrandFieldArg(_ x2: borrowing AggGenericStruct<String>) {
    // expected-error @-1 {{cannot use 'x2' after partial consume}}
    // expected-error @-2 {{cannot use 'x2' after partial consume}}
    consumeVal(x2.pair.lhs) // expected-note {{partially consumed here}}
    for _ in 0..<1024 {
        consumeVal(x2.pair.lhs) // expected-note {{partially consumed here}}
    }
}

public func aggGenericStructConsumeGrandFieldOwnedArg(_ x2: __owned AggGenericStruct<String>) {
    // expected-error @-1 {{'x2' consumed in a loop}}
    // expected-error @-2 {{'x2' consumed more than once}}
    consumeVal(x2.pair.lhs) // expected-note {{consumed here}}
    for _ in 0..<1024 {
        consumeVal(x2.pair.lhs) // expected-note {{consumed here}}
        // expected-note @-1 {{consumed again here}}
    }
}

public func aggGenericStructConsumeGrandFieldOwnedArg2(_ x2: consuming AggGenericStruct<String>) {
    // expected-error @-1 {{'x2' consumed in a loop}}
    // expected-error @-2 {{'x2' consumed more than once}}
    consumeVal(x2.pair.lhs) // expected-note {{consumed here}}
    for _ in 0..<1024 {
        consumeVal(x2.pair.lhs) // expected-note {{consumed here}}
        // expected-note @-1 {{consumed again here}}
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
    // expected-error @-1 {{'x2' consumed in a loop}}
    // expected-error @-2 {{'x2' consumed more than once}}
    consumeVal(x2.lhs) // expected-note {{consumed here}}
    for _ in 0..<1024 {
        consumeVal(x2.lhs) // expected-note {{consumed here}}
        // expected-note @-1 {{consumed again here}}
    }
}

public func aggGenericStructConsumeFieldArg<T>(_ x2: borrowing AggGenericStruct<T>) {
    // expected-error @-1 {{cannot use 'x2' after partial consume}}
    // expected-error @-2 {{cannot use 'x2' after partial consume}}
    consumeVal(x2.lhs) // expected-note {{partially consumed here}}
    for _ in 0..<1024 {
        consumeVal(x2.lhs) // expected-note {{partially consumed here}}
    }
}

public func aggGenericStructConsumeFieldOwnedArg<T>(_ x2: __owned AggGenericStruct<T>) {
    // expected-error @-1 {{'x2' consumed in a loop}}
    // expected-error @-2 {{'x2' consumed more than once}}
    consumeVal(x2.lhs) // expected-note {{consumed here}}
    for _ in 0..<1024 {
        consumeVal(x2.lhs) // expected-note {{consumed here}}
        // expected-note @-1 {{consumed again here}}
    }
}

public func aggGenericStructConsumeFieldOwnedArg2<T>(_ x2: consuming AggGenericStruct<T>) {
    // expected-error @-1 {{'x2' consumed in a loop}}
    // expected-error @-2 {{'x2' consumed more than once}}
    consumeVal(x2.lhs) // expected-note {{consumed here}}
    for _ in 0..<1024 {
        consumeVal(x2.lhs) // expected-note {{consumed here}}
        // expected-note @-1 {{consumed again here}}
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
    // expected-error @-1 {{'x2' consumed in a loop}}
    // expected-error @-2 {{'x2' consumed more than once}}
    consumeVal(x2.pair.lhs) // expected-note {{consumed here}}
    for _ in 0..<1024 {
        consumeVal(x2.pair.lhs) // expected-note {{consumed here}}
        // expected-note @-1 {{consumed again here}}
    }
}

public func aggGenericStructConsumeGrandFieldArg<T>(_ x2: borrowing AggGenericStruct<T>) {
    // expected-error @-1 {{cannot use 'x2' after partial consume}}
    // expected-error @-2 {{cannot use 'x2' after partial consume}}
    consumeVal(x2.pair.lhs) // expected-note {{partially consumed here}}
    for _ in 0..<1024 {
        consumeVal(x2.pair.lhs) // expected-note {{partially consumed here}}
    }
}

public func aggGenericStructConsumeGrandFieldOwnedArg<T>(_ x2: __owned AggGenericStruct<T>) {
    // expected-error @-1 {{'x2' consumed in a loop}}
    // expected-error @-2 {{'x2' consumed more than once}}
    consumeVal(x2.pair.lhs) // expected-note {{consumed here}}
    for _ in 0..<1024 {
        consumeVal(x2.pair.lhs) // expected-note {{consumed here}}
        // expected-note @-1 {{consumed again here}}
    }
}

public func aggGenericStructConsumeGrandFieldOwnedArg2<T>(_ x2: consuming AggGenericStruct<T>) {
    // expected-error @-1 {{'x2' consumed in a loop}}
    // expected-error @-2 {{'x2' consumed more than once}}
    consumeVal(x2.pair.lhs) // expected-note {{consumed here}}
    for _ in 0..<1024 {
        consumeVal(x2.pair.lhs) // expected-note {{consumed here}}
        // expected-note @-1 {{consumed again here}}
    }
}

/////////////////////
// Enum Test Cases //
/////////////////////

@_moveOnly
public enum EnumTy {
    case klass(Klass)
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
        consumeVal(x2) // expected-note {{consumed here}}
    }
}

public func enumLoopConsumeOwnedArg2(_ x2: consuming EnumTy) { // expected-error {{'x2' consumed in a loop}}
    for _ in 0..<1024 {
        consumeVal(x2) // expected-note {{consumed here}}
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
        borrowVal(x)
    }
    if case let .klass(x) = consume x2 { // expected-note {{consumed again here}}
        borrowVal(x)
    }
}

public func enumPatternMatchIfLet1Arg(_ x2: borrowing EnumTy) { // expected-error {{'x2' is borrowed and cannot be consumed}}
    if case let .klass(x) = consume x2 { // expected-note {{consumed here}}
        borrowVal(x)
    }
    if case let .klass(x) = consume x2 { // expected-note {{consumed here}}
        borrowVal(x)
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

// QOI: We can do better here. We should also flag x2
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
// MARK: Closure Let Tests //
/////////////////////////////

public func closureLetClassUseAfterConsume1(_ x: borrowing Klass) {
    // expected-error @-1 {{'x' cannot be captured by an escaping closure since it is a borrowed parameter}}
    // expected-error @-2 {{noncopyable 'x' cannot be consumed when captured by an escaping closure}}
    let f = { // expected-note {{closure capturing 'x' here}}
        let x2 = x // expected-error {{'x2' consumed more than once}}
        // expected-note @-1 {{consumed here}}
        borrowVal(x2)
        consumeVal(x2) // expected-note {{consumed here}}
        consumeVal(x2) // expected-note {{consumed again here}}
    }
    f()
}

public func closureLetClassUseAfterConsume2(_ argX: borrowing Klass) {
    let f = { (_ x: borrowing Klass) in // expected-error {{'x' is borrowed and cannot be consumed}}
        let x2 = x // expected-error {{'x2' consumed more than once}}
                   // expected-note @-1 {{consumed here}}
        borrowVal(x2)
        consumeVal(x2) // expected-note {{consumed here}}
        consumeVal(x2) // expected-note {{consumed again here}}
    }
    f(argX)
}

public func closureLetClassUseAfterConsumeArg(_ argX: borrowing Klass) {
    let f = { (_ x2: borrowing Klass) in // expected-error {{'x2' is borrowed and cannot be consumed}}
        borrowVal(x2)
        consumeVal(x2) // expected-note {{consumed here}}
        consumeVal(x2) // expected-note {{consumed here}}
    }
    f(argX)
}

public func closureLetCaptureClassUseAfterConsume(_ x: consuming Klass) {
    let x2 = x // expected-error {{noncopyable 'x2' cannot be consumed when captured by an escaping closure}}
    // expected-error @-1 {{noncopyable 'x2' cannot be consumed when captured by an escaping closure}}
    let f = {
        borrowVal(x2)
        consumeVal(x2) // expected-note {{consumed here}}
        consumeVal(x2) // expected-note {{consumed here}}
    }
    f()
}

public func closureLetCaptureClassUseAfterConsume1(_ x: borrowing Klass) { // expected-error {{'x' is borrowed and cannot be consumed}}
    var x2 = x // expected-note {{consumed here}}
    // expected-error @-1 {{'x2' consumed more than once}}
    // expected-error @-2 {{missing reinitialization of closure capture 'x2' after consume}}
    x2 = x  // expected-note {{consumed here}}

    let f = {
        borrowVal(x2)
        consumeVal(x2) // expected-note {{consumed here}}
        consumeVal(x2) // expected-note {{consumed here}}
        // expected-note @-1 {{consumed again here}}
    }
    f()
}

public func closureLetCaptureClassUseAfterConsume2(_ x2: inout Klass) {
    // expected-note @-1 {{'x2' is declared 'inout'}}
    let f = { // expected-error {{escaping closure captures 'inout' parameter 'x2'}}
        borrowVal(x2) // expected-note {{captured here}}
        consumeVal(x2) // expected-note {{captured here}}
        consumeVal(x2)  // expected-note {{captured here}}
    }
    f()
}

// TODO: We are considering this to be an escaping use.
public func closureLetCaptureClassUseAfterConsume3(_ x2: inout Klass) {
    // expected-error @-1 {{'x2' consumed more than once}}
    // expected-error @-2 {{missing reinitialization of closure capture 'x2' after consume}}
    func useClosure(_ x: () -> ()) {}

    useClosure {
        borrowVal(x2)
        consumeVal(x2) // expected-note {{consumed here}}
        consumeVal(x2) // expected-note {{consumed here}}
        // expected-note @-1 {{consumed again here}}
    }
}


public func closureLetCaptureClassUseAfterConsumeError(_ x: borrowing Klass) { // expected-error {{'x' is borrowed and cannot be consumed}}
    let x2 = x // expected-note {{consumed here}}
    // expected-error @-1 {{noncopyable 'x2' cannot be consumed when captured by an escaping closure}}
    // expected-error @-2 {{noncopyable 'x2' cannot be consumed when captured by an escaping closure}}
    let f = {
        borrowVal(x2)
        consumeVal(x2) // expected-note {{consumed here}}
        consumeVal(x2) // expected-note {{consumed here}}
    }
    f()
    let x3 = x2
    let _ = x3
}

public func closureLetCaptureClassArgUseAfterConsume(_ x2: borrowing Klass) {
    // expected-error @-1 {{noncopyable 'x2' cannot be consumed when captured by an escaping closure}}
    // expected-error @-2 {{'x2' cannot be captured by an escaping closure since it is a borrowed parameter}}
    let f = { // expected-note {{closure capturing 'x2' here}}
        borrowVal(x2)
        consumeVal(x2) // expected-note {{consumed here}}
        consumeVal(x2) // expected-note {{consumed here}}
    }
    f()
}

public func closureLetCaptureClassOwnedArgUseAfterConsume(_ x2: __owned Klass) {
    // expected-error @-1 {{noncopyable 'x2' cannot be consumed when captured by an escaping closure}}
    // expected-error @-2 {{noncopyable 'x2' cannot be consumed when captured by an escaping closure}}
    let f = {
        borrowVal(x2)
        consumeVal(x2) // expected-note {{consumed here}}
        consumeVal(x2) // expected-note {{consumed here}}
    }
    f()
}

public func closureLetCaptureClassOwnedArgUseAfterConsume2(_ x2: consuming Klass) {
    // expected-error @-1 {{'x2' consumed more than once}}
    // expected-error @-2 {{missing reinitialization of closure capture 'x2' after consume}}
    let f = {
        borrowVal(x2)
        consumeVal(x2) // expected-note {{consumed here}}
        consumeVal(x2) // expected-note {{consumed here}}
        // expected-note @-1 {{consumed again here}}
    }
    f()
}

public func closureLetCaptureClassOwnedArgUseAfterConsume3(_ x2: __owned Klass) {
    // expected-error @-1 {{noncopyable 'x2' cannot be consumed when captured by an escaping closure}}
    // expected-error @-2 {{noncopyable 'x2' cannot be consumed when captured by an escaping closure}}
    let f = {
        borrowVal(x2)
        consumeVal(x2) // expected-note {{consumed here}}
        consumeVal(x2) // expected-note {{consumed here}}
    }
    f()
    let x3 = x2
    let _ = x3
}

public func closureLetCaptureClassOwnedArgUseAfterConsume4(_ x2: consuming Klass) {
    // expected-error @-1 {{'x2' consumed more than once}}
    // expected-error @-2 {{'x2' consumed more than once}}
    // expected-error @-3 {{missing reinitialization of closure capture 'x2' after consume}}
    let f = {
        borrowVal(x2)
        consumeVal(x2) // expected-note {{consumed here}}
        consumeVal(x2) // expected-note {{consumed here}}
        // expected-note @-1 {{consumed again here}}
    }
    f()
    let x3 = x2 // expected-note {{consumed here}}
    consumeVal(x2) // expected-note {{consumed again here}}
    x2 = Klass()
    let _ = x3
}

/////////////////////////////
// MARK: Closure Var tests //
/////////////////////////////

public func closureVarClassUseAfterConsume1(_ x: borrowing Klass) {
    // expected-error @-1 {{'x' cannot be captured by an escaping closure since it is a borrowed parameter}}
    // expected-error @-2 {{noncopyable 'x' cannot be consumed when captured by an escaping closure}}
    var f = {}
    f = { // expected-note {{closure capturing 'x' here}}
        let x2 = x // expected-error {{'x2' consumed more than once}}
        // expected-note @-1 {{consumed here}}
        borrowVal(x2)
        consumeVal(x2) // expected-note {{consumed here}}
        consumeVal(x2) // expected-note {{consumed again here}}
    }
    f()
}

public func closureVarClassUseAfterConsume2(_ argX: borrowing Klass) {
    var f = {(_ x: borrowing Klass) in }
    f = { (_ x: borrowing Klass) in // expected-error {{'x' is borrowed and cannot be consumed}}
        let x2 = x // expected-error {{'x2' consumed more than once}}
        // expected-note @-1 {{consumed here}}
        borrowVal(x2)
        consumeVal(x2) // expected-note {{consumed here}}
        consumeVal(x2) // expected-note {{consumed again here}}
    }
    f(argX)
}

public func closureVarClassUseAfterConsumeArg(_ argX: borrowing Klass) {
    var f = {(_ x2: borrowing Klass) in}
    f = { (_ x2: borrowing Klass) in // expected-error {{'x2' is borrowed and cannot be consumed}}
        borrowVal(x2)
        consumeVal(x2) // expected-note {{consumed here}}
        consumeVal(x2) // expected-note {{consumed here}}
    }
    f(argX)
}

public func closureVarCaptureClassUseAfterConsume(_ x: borrowing Klass) { // expected-error {{'x' is borrowed and cannot be consumed}}
    let x2 = x // expected-note {{consumed here}}
    var f = {}
    f = { 
        borrowVal(x2)
        consumeVal(x2) // expected-error {{noncopyable 'x2' cannot be consumed when captured by an escaping closure}}
        consumeVal(x2) // expected-error {{noncopyable 'x2' cannot be consumed when captured by an escaping closure}}
    }
    f()
}

public func closureVarCaptureClassUseAfterConsume1(_ x: borrowing Klass) { // expected-error {{'x' is borrowed and cannot be consumed}}
    var x2 = x // expected-note {{consumed here}}
    x2 = x  // expected-note {{consumed here}}

    var f = {}
    f = { 
        borrowVal(x2)
        consumeVal(x2) // expected-error {{noncopyable 'x2' cannot be consumed when captured by an escaping closure}}
        consumeVal(x2) // expected-error {{noncopyable 'x2' cannot be consumed when captured by an escaping closure}}
    }
    f()
}

public func closureVarCaptureClassUseAfterConsume2(_ x2: inout Klass) {
    // expected-note @-1 {{'x2' is declared 'inout'}}
    var f = {}
    f = { // expected-error {{escaping closure captures 'inout' parameter 'x2'}}
        borrowVal(x2) // expected-note {{captured here}}
        consumeVal(x2) // expected-note {{captured here}}
        consumeVal(x2)  // expected-note {{captured here}}
    }
    f()
}

public func closureVarCaptureClassUseAfterConsume3(_ x2: inout Klass) {
    // expected-note @-1 {{'x2' is declared 'inout'}}
    func useClosure(_ x: @escaping () -> ()) {}

    useClosure { // expected-error {{escaping closure captures 'inout' parameter 'x2'}}
        borrowVal(x2) // expected-note {{captured here}}
        consumeVal(x2) // expected-note {{captured here}}
        consumeVal(x2) // expected-note {{captured here}}
    }
}

public func closureVarCaptureClassUseAfterConsume4(_ x2: inout Klass) {
    // expected-error @-1 {{'x2' consumed more than once}}
    // expected-error @-2 {{missing reinitialization of closure capture 'x2' after consume}}
    func useClosure(_ x: () -> ()) {}

    useClosure {
        borrowVal(x2)
        consumeVal(x2) // expected-note {{consumed here}}
        consumeVal(x2) // expected-note {{consumed here}}
        // expected-note @-1 {{consumed again here}}
    }
}


public func closureVarCaptureClassUseAfterConsumeError(_ x: borrowing Klass) { // expected-error {{'x' is borrowed and cannot be consumed}}
    let x2 = x // expected-note {{consumed here}}
    var f = {}
    f = { 
        borrowVal(x2)
        consumeVal(x2) // expected-error {{noncopyable 'x2' cannot be consumed when captured by an escaping closure}}
        consumeVal(x2) // expected-error {{noncopyable 'x2' cannot be consumed when captured by an escaping closure}}
    }
    f()
    let x3 = x2 // expected-error {{noncopyable 'x2' cannot be consumed when captured by an escaping closure}}
    let _ = x3
}

public func closureVarCaptureClassArgUseAfterConsume(_ x2: borrowing Klass) {
    // expected-error @-1 {{noncopyable 'x2' cannot be consumed when captured by an escaping closure}}
    // expected-error @-2 {{'x2' cannot be captured by an escaping closure since it is a borrowed parameter}}
    var f = {}
    f = { // expected-note {{closure capturing 'x2' here}}
        borrowVal(x2)
        consumeVal(x2) // expected-note {{consumed here}}
        consumeVal(x2) // expected-note {{consumed here}}
    }
    f()
}

public func closureVarCaptureClassOwnedArgUseAfterConsume(_ x2: __owned Klass) {
    var f = {}
    f = { 
        borrowVal(x2)
        consumeVal(x2) // expected-error {{noncopyable 'x2' cannot be consumed when captured by an escaping closure}}
        consumeVal(x2) // expected-error {{noncopyable 'x2' cannot be consumed when captured by an escaping closure}}
    }
    f()
}

public func closureVarCaptureClassOwnedArgUseAfterConsume2(_ x2: consuming Klass) {
    var f = {}
    f = { 
        borrowVal(x2)
        consumeVal(x2) // expected-error {{noncopyable 'x2' cannot be consumed when captured by an escaping closure}}
        consumeVal(x2) // expected-error {{noncopyable 'x2' cannot be consumed when captured by an escaping closure}}
    }
    f()
}

public func closureVarCaptureClassOwnedArgUseAfterConsume3(_ x2: __owned Klass) {
    var f = {}
    f = { 
        borrowVal(x2)
        consumeVal(x2) // expected-error {{noncopyable 'x2' cannot be consumed when captured by an escaping closure}}
        consumeVal(x2) // expected-error {{noncopyable 'x2' cannot be consumed when captured by an escaping closure}}
    }
    f()
    let x3 = x2 // expected-error {{noncopyable 'x2' cannot be consumed when captured by an escaping closure}}
    let _ = x3
}

public func closureVarCaptureClassOwnedArgUseAfterConsume4(_ x2: consuming Klass) {
    var f = {}
    f = { 
        borrowVal(x2)
        consumeVal(x2) // expected-error {{noncopyable 'x2' cannot be consumed when captured by an escaping closure}}
        consumeVal(x2) // expected-error {{noncopyable 'x2' cannot be consumed when captured by an escaping closure}}
    }
    f()
    let x3 = x2 // expected-error {{noncopyable 'x2' cannot be consumed when captured by an escaping closure}}
    consumeVal(x2) // expected-error {{noncopyable 'x2' cannot be consumed when captured by an escaping closure}}
    x2 = Klass()
    let _ = x3
}

///////////////////////
// MARK: Defer Tests //
///////////////////////

public func deferCaptureClassUseAfterConsume(_ x: borrowing Klass) { // expected-error {{'x' is borrowed and cannot be consumed}}
    let x2 = x // expected-note {{consumed here}}
    // expected-error @-1 {{noncopyable 'x2' cannot be consumed when captured by an escaping closure}}
    defer {
        borrowVal(x2)
        // TODO: Defer can only run once, so this error shouldn't occur.
        consumeVal(x2) // expected-note {{consumed here}}
        consumeVal(x2) // expected-note {{consumed here}}
    }
    consumeVal(x) // expected-note {{consumed here}}
}

public func deferCaptureClassUseAfterConsume2(_ x: borrowing Klass) { // expected-error {{'x' is borrowed and cannot be consumed}}
    let x2 = x
    // expected-note @-1 {{consumed here}}
    // expected-error @-2 {{noncopyable 'x2' cannot be consumed when captured by an escaping closure}}
    // expected-error @-3 {{'x2' used after consume}}
    defer { // expected-note {{used here}}
        borrowVal(x2)
        consumeVal(x2) // expected-note {{consumed here}}
        consumeVal(x2) // expected-note {{consumed here}}
    }
    let x3 = x2 // expected-note {{consumed here}}
    let _ = x3
}

public func deferCaptureClassArgUseAfterConsume(_ x2: borrowing Klass) {
    // expected-error @-1 {{noncopyable 'x2' cannot be consumed when captured by an escaping closure}}
    borrowVal(x2)
    defer {
        borrowVal(x2)
        consumeVal(x2) // expected-note {{consumed here}}
        consumeVal(x2) // expected-note {{consumed here}}
    }
    print("foo")
}

public func deferCaptureClassOwnedArgUseAfterConsume(_ x2: __owned Klass) {
    // expected-error @-1 {{noncopyable 'x2' cannot be consumed when captured by an escaping closure}}
    defer {
        borrowVal(x2)
        consumeVal(x2) // expected-note {{consumed here}}
        consumeVal(x2) // expected-note {{consumed here}}
    }
    print("foo")
}

public func deferCaptureClassOwnedArgUseAfterConsume2(_ x2: consuming Klass) {
    // expected-error @-1 {{missing reinitialization of closure capture 'x2' after consume}}
    // expected-error @-2 {{'x2' consumed more than once}}
    defer {
        borrowVal(x2)
        consumeVal(x2) // expected-note {{consumed here}}
        consumeVal(x2) // expected-note {{consumed here}}
        // expected-note @-1 {{consumed again here}}
    }
    print("foo")
}

public func deferCaptureClassOwnedArgUseAfterConsume3(_ x2: __owned Klass) {
    // expected-error @-1 {{noncopyable 'x2' cannot be consumed when captured by an escaping closure}}
    // expected-error @-2 {{'x2' used after consume}}
    defer { // expected-note {{used here}}
        borrowVal(x2)
        consumeVal(x2) // expected-note {{consumed here}}
        consumeVal(x2) // expected-note {{consumed here}}
    }
    consumeVal(x2) // expected-note {{consumed here}}
}

public func deferCaptureClassOwnedArgUseAfterConsume4(_ x2: consuming Klass) {
    // expected-error @-1 {{missing reinitialization of closure capture 'x2' after consume}}
    // expected-error @-2 {{'x2' consumed more than once}}
    // expected-error @-3 {{'x2' used after consume}}
    defer { // expected-note {{used here}}
        borrowVal(x2)
        consumeVal(x2) // expected-note {{consumed here}}
        consumeVal(x2) // expected-note {{consumed here}}
        // expected-note @-1 {{consumed again here}}
    }
    consumeVal(x2) // expected-note {{consumed here}}
}

/////////////////////////////////
// MARK: Defer and Let Closure //
/////////////////////////////////

public func closureLetAndDeferCaptureClassUseAfterConsume(_ x: borrowing Klass) { // expected-error {{'x' is borrowed and cannot be consumed}}
    let x2 = x // expected-note {{consumed here}}
    // expected-error @-1 {{noncopyable 'x2' cannot be consumed when captured by an escaping closure}}
    let f = {
        defer {
            borrowVal(x2)
            consumeVal(x2) // expected-note {{consumed here}}
            consumeVal(x2) // expected-note {{consumed here}}
        }
        print("foo")
    }
    f()
}

public func closureLetAndDeferCaptureClassUseAfterConsume2(_ x: borrowing Klass) { // expected-error {{'x' is borrowed and cannot be consumed}}
    // TODO: This is wrong
    let x2 = x // expected-error {{noncopyable 'x2' cannot be consumed when captured by an escaping closure}}
    // expected-note @-1 {{consumed here}}
    // expected-error @-2 {{noncopyable 'x2' cannot be consumed when captured by an escaping closure}}
    let f = {
        consumeVal(x2) // expected-note {{consumed here}}
        defer {
            borrowVal(x2)
            consumeVal(x2) // expected-note {{consumed here}}
            consumeVal(x2) // expected-note {{consumed here}}
        }
        print("foo")
    }
    f()
}

// TODO: MG
public func closureLetAndDeferCaptureClassUseAfterConsume3(_ x: borrowing Klass) { // expected-error {{'x' is borrowed and cannot be consumed}}
    let x2 = x
    // expected-note @-1 {{consumed here}}
    // expected-error @-2 {{noncopyable 'x2' cannot be consumed when captured by an escaping closure}}
    // expected-error @-3 {{noncopyable 'x2' cannot be consumed when captured by an escaping closure}}
    let f = {
        consumeVal(x2) // expected-note {{consumed here}}
        defer {
            borrowVal(x2)
            consumeVal(x2) // expected-note {{consumed here}}
            consumeVal(x2) // expected-note {{consumed here}}
        }
        print("foo")
    }
    f()
    consumeVal(x2)
}

public func closureLetAndDeferCaptureClassArgUseAfterConsume(_ x2: borrowing Klass) {
    // expected-error @-1 {{noncopyable 'x2' cannot be consumed when captured by an escaping closure}}
    // expected-error @-2 {{'x2' cannot be captured by an escaping closure since it is a borrowed parameter}}
    let f = { // expected-note {{closure capturing 'x2' here}}
        defer {
            borrowVal(x2)
            consumeVal(x2) // expected-note {{consumed here}}
            consumeVal(x2) // expected-note {{consumed here}}
        }
        print("foo")
    }
    f()
}

public func closureLetAndDeferCaptureClassOwnedArgUseAfterConsume(_ x2: __owned Klass) {
    // expected-error @-1 {{noncopyable 'x2' cannot be consumed when captured by an escaping closure}}
    let f = {
        defer {
            borrowVal(x2)
            consumeVal(x2) // expected-note {{consumed here}}
            consumeVal(x2) // expected-note {{consumed here}}
        }
        print("foo")
    }
    f()
}

public func closureLetAndDeferCaptureClassOwnedArgUseAfterConsume2(_ x2: consuming Klass) {
    // expected-error @-1 {{missing reinitialization of closure capture 'x2' after consume}}
    // expected-error @-2 {{'x2' consumed more than once}}
    let f = {
        defer {
            borrowVal(x2)
            consumeVal(x2) // expected-note {{consumed here}}
            consumeVal(x2) // expected-note {{consumed here}}
            // expected-note @-1 {{consumed again here}}
        }
        print("foo")
    }
    f()
}

// TODO: MG
public func closureLetAndDeferCaptureClassOwnedArgUseAfterConsume3(_ x2: __owned Klass) {
    // expected-error @-1 {{noncopyable 'x2' cannot be consumed when captured by an escaping closure}}
    let f = {
        defer {
            borrowVal(x2)
            consumeVal(x2) // expected-note {{consumed here}}
            consumeVal(x2) // expected-note {{consumed here}}
        }
        print("foo")
    }
    f()
    consumeVal(x2)
}

public func closureLetAndDeferCaptureClassOwnedArgUseAfterConsume4(_ x2: consuming Klass) {
    // expected-error @-1 {{'x2' consumed more than once}}
    // expected-error @-2 {{missing reinitialization of closure capture 'x2' after consume}}
    let f = {
        defer {
            borrowVal(x2)
            consumeVal(x2) // expected-note {{consumed here}}
            consumeVal(x2) // expected-note {{consumed here}}
            // expected-note @-1 {{consumed again here}}
        }
        print("foo")
    }
    f()
    consumeVal(x2)
}

///////////////////////////////////////////
// MARK: Multiple Levels of Let Closures //
///////////////////////////////////////////

public func closureLetAndClosureCaptureClassUseAfterConsume(_ x: borrowing Klass) { // expected-error {{'x' is borrowed and cannot be consumed}}
    let x2 = x // expected-note {{consumed here}}
    // expected-error @-1 {{noncopyable 'x2' cannot be consumed when captured by an escaping closure}}
    // expected-error @-2 {{noncopyable 'x2' cannot be consumed when captured by an escaping closure}}
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

public func closureLetAndClosureCaptureClassUseAfterConsume2(_ x: borrowing Klass) { // expected-error {{'x' is borrowed and cannot be consumed}}
    let x2 = x
    // expected-note @-1 {{consumed here}}
    // expected-error @-2 {{noncopyable 'x2' cannot be consumed when captured by an escaping closure}}
    // expected-error @-3 {{noncopyable 'x2' cannot be consumed when captured by an escaping closure}}
    // expected-error @-4 {{'x2' consumed more than once}}

    let f = {
        let g = {
            borrowVal(x2)
            consumeVal(x2) // expected-note {{consumed here}}
            consumeVal(x2) // expected-note {{consumed here}}
        }
        g()
    }
    f()
    consumeVal(x2) // expected-note {{consumed here}}
    let x3 = x2 // expected-note {{consumed again here}}
    _ = x3
}

public func closureLetAndClosureCaptureClassUseAfterConsume3(_ x: borrowing Klass) { // expected-error {{'x' is borrowed and cannot be consumed}}
    var x2 = x
    // expected-note @-1 {{consumed here}}
    // expected-error @-2 {{'x2' consumed more than once}}
    // expected-error @-3 {{missing reinitialization of closure capture 'x2' after consume}}
    // expected-error @-4 {{'x2' consumed more than once}}
    x2 = x
    // expected-note @-1 {{consumed here}}

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
    consumeVal(x2) // expected-note {{consumed here}}
    let x3 = x2 // expected-note {{consumed again here}}
    _ = x3
}

public func closureLetAndClosureCaptureClassArgUseAfterConsume(_ x2: borrowing Klass) {
    // expected-error @-1 {{noncopyable 'x2' cannot be consumed when captured by an escaping closure}}
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

public func closureLetAndClosureCaptureClassOwnedArgUseAfterConsume(_ x2: __owned Klass) {
    // expected-error @-1 {{noncopyable 'x2' cannot be consumed when captured by an escaping closure}}
    // expected-error @-2 {{noncopyable 'x2' cannot be consumed when captured by an escaping closure}}
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

public func closureLetAndClosureCaptureClassOwnedArgUseAfterConsume2(_ x2: consuming Klass) {
    // expected-error @-1 {{'x2' consumed more than once}}
    // expected-error @-2 {{missing reinitialization of closure capture 'x2' after consume}}
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

public func closureLetAndClosureCaptureClassOwnedArgUseAfterConsume3(_ x2: __owned Klass) {
    // expected-error @-1 {{noncopyable 'x2' cannot be consumed when captured by an escaping closure}}
    // expected-error @-2 {{noncopyable 'x2' cannot be consumed when captured by an escaping closure}}
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

public func closureLetAndClosureCaptureClassOwnedArgUseAfterConsume4(_ x2: consuming Klass) {
    // expected-error @-1 {{missing reinitialization of closure capture 'x2' after consume}}
    // expected-error @-2 {{'x2' consumed more than once}}
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
    consumeVal(x2)
}

public func closureLetAndClosureCaptureClassOwnedArgUseAfterConsume5(_ x2: consuming Klass) {
    // expected-error @-1 {{missing reinitialization of closure capture 'x2' after consume}}
    // expected-error @-2 {{'x2' consumed more than once}}
    // expected-error @-3 {{'x2' used after consume}}
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
    consumeVal(x2) // expected-note {{consumed here}}
    f() // expected-note {{used here}}
}

/////////////////////////////////
// MARK: Defer and Var Closure //
/////////////////////////////////

public func closureVarAndDeferCaptureClassUseAfterConsume(_ x: borrowing Klass) { // expected-error {{'x' is borrowed and cannot be consumed}}
    let x2 = x // expected-note {{consumed here}}
    // expected-error @-1 {{noncopyable 'x2' cannot be consumed when captured by an escaping closure}}
    var f = {}
    f = {
        defer {
            borrowVal(x2)
            consumeVal(x2) // expected-note {{consumed here}}
            consumeVal(x2) // expected-note {{consumed here}}
        }
        print("foo")
    }
    f()
}

public func closureVarAndDeferCaptureClassUseAfterConsume2(_ x: borrowing Klass) { // expected-error {{'x' is borrowed and cannot be consumed}}
    let x2 = x // expected-error {{noncopyable 'x2' cannot be consumed when captured by an escaping closure}}
    // expected-note @-1 {{consumed here}}
    var f = {}
    f = {
        consumeVal(x2) // expected-error {{noncopyable 'x2' cannot be consumed when captured by an escaping closure}}
        defer {
            borrowVal(x2)
            consumeVal(x2) // expected-note {{consumed here}}
            consumeVal(x2) // expected-note {{consumed here}}
        }
        print("foo")
    }
    f()
}

// TODO: MG
public func closureVarAndDeferCaptureClassUseAfterConsume3(_ x: borrowing Klass) { // expected-error {{'x' is borrowed and cannot be consumed}}
    let x2 = x
    // expected-note @-1 {{consumed here}}
    // expected-error @-2 {{noncopyable 'x2' cannot be consumed when captured by an escaping closure}}
    var f = {}
    f = {
        consumeVal(x2) // expected-error {{noncopyable 'x2' cannot be consumed when captured by an escaping closure}}
        defer {
            borrowVal(x2)
            consumeVal(x2) // expected-note {{consumed here}}
            consumeVal(x2) // expected-note {{consumed here}}
        }
        print("foo")
    }
    f()
    consumeVal(x2) // expected-error {{noncopyable 'x2' cannot be consumed when captured by an escaping closure}}
}

public func closureVarAndDeferCaptureClassArgUseAfterConsume(_ x2: borrowing Klass) {
    // expected-error @-1 {{noncopyable 'x2' cannot be consumed when captured by an escaping closure}}
    // expected-error @-2 {{'x2' cannot be captured by an escaping closure since it is a borrowed parameter}}
    var f = {}
    f = {// expected-note {{closure capturing 'x2' here}}
        defer {
            borrowVal(x2)
            consumeVal(x2) // expected-note {{consumed here}}
            consumeVal(x2) // expected-note {{consumed here}}
        }
        print("foo")
    }
    f()
}

public func closureVarAndDeferCaptureClassOwnedArgUseAfterConsume(_ x2: __owned Klass) {
    // expected-error @-1 {{noncopyable 'x2' cannot be consumed when captured by an escaping closure}}
    var f = {}
    f = {
        defer {
            borrowVal(x2)
            consumeVal(x2) // expected-note {{consumed here}}
            consumeVal(x2) // expected-note {{consumed here}}
        }
        print("foo")
    }
    f()
}

public func closureVarAndDeferCaptureClassOwnedArgUseAfterConsume2(_ x2: consuming Klass) {
    // expected-error @-1 {{missing reinitialization of closure capture 'x2' after consume}}
    // expected-error @-2 {{'x2' consumed more than once}}
    var f = {}
    f = {
        defer {
            borrowVal(x2)
            consumeVal(x2) // expected-note {{consumed here}}
            consumeVal(x2) // expected-note {{consumed here}}
            // expected-note @-1 {{consumed again here}}
        }
        print("foo")
    }
    f()
}

// TODO: MG
public func closureVarAndDeferCaptureClassOwnedArgUseAfterConsume3(_ x2: __owned Klass) {
    // expected-error @-1 {{noncopyable 'x2' cannot be consumed when captured by an escaping closure}}
    var f = {}
    f = {
        defer {
            borrowVal(x2)
            consumeVal(x2) // expected-note {{consumed here}}
            consumeVal(x2) // expected-note {{consumed here}}
        }
        print("foo")
    }
    f()
    consumeVal(x2) // expected-error {{noncopyable 'x2' cannot be consumed when captured by an escaping closure}}
}

public func closureVarAndDeferCaptureClassOwnedArgUseAfterConsume4(_ x2: consuming Klass) {
    // expected-error @-1 {{'x2' consumed more than once}}
    // expected-error @-2 {{missing reinitialization of closure capture 'x2' after consume}}
    var f = {}
    f = {
        defer {
            borrowVal(x2)
            consumeVal(x2) // expected-note {{consumed here}}
            consumeVal(x2) // expected-note {{consumed here}}
            // expected-note @-1 {{consumed again here}}
        }
        print("foo")
    }
    f()
    consumeVal(x2) // expected-error {{noncopyable 'x2' cannot be consumed when captured by an escaping closure}}
}

///////////////////////////////////////////
// MARK: Multiple Levels of Var Closures //
///////////////////////////////////////////

public func closureVarAndClosureCaptureClassUseAfterConsume(_ x: borrowing Klass) { // expected-error {{'x' is borrowed and cannot be consumed}}
    let x2 = x // expected-note {{consumed here}}
    var f = {}
    f = {
        var g = {}
        g = {
            borrowVal(x2)
            consumeVal(x2) // expected-error {{noncopyable 'x2' cannot be consumed when captured by an escaping closure}}
            consumeVal(x2) // expected-error {{noncopyable 'x2' cannot be consumed when captured by an escaping closure}}
        }
        g()
        consumeVal(x2) // expected-error {{noncopyable 'x2' cannot be consumed when captured by an escaping closure}}
    }
    f()
}

public func closureVarAndClosureCaptureClassUseAfterConsume2(_ x: borrowing Klass) { // expected-error {{'x' is borrowed and cannot be consumed}}
    let x2 = x
    // expected-note @-1 {{consumed here}}

    var f = {}
    f = {
        var g = {}
        g = {
            borrowVal(x2)
            consumeVal(x2) // expected-error {{noncopyable 'x2' cannot be consumed when captured by an escaping closure}}
            consumeVal(x2) // expected-error {{noncopyable 'x2' cannot be consumed when captured by an escaping closure}}
        }
        g()
    }
    f()
    consumeVal(x2) // expected-error {{noncopyable 'x2' cannot be consumed when captured by an escaping closure}}
    let x3 = x2 // expected-error {{noncopyable 'x2' cannot be consumed when captured by an escaping closure}}
    _ = x3
}

public func closureVarAndClosureCaptureClassUseAfterConsume3(_ x: borrowing Klass) { // expected-error {{'x' is borrowed and cannot be consumed}}
    var x2 = x
    // expected-note @-1 {{consumed here}}
    x2 = x
    // expected-note @-1 {{consumed here}}

    var f = {}
    f = {
        var g = {}
        g = {
            borrowVal(x2)
            consumeVal(x2) // expected-error {{noncopyable 'x2' cannot be consumed when captured by an escaping closure}}
            consumeVal(x2) // expected-error {{noncopyable 'x2' cannot be consumed when captured by an escaping closure}}
        }
        g()
    }
    f()
    consumeVal(x2) // expected-error {{noncopyable 'x2' cannot be consumed when captured by an escaping closure}}
    let x3 = x2 // expected-error {{noncopyable 'x2' cannot be consumed when captured by an escaping closure}}
    _ = x3
}

public func closureVarAndClosureCaptureClassArgUseAfterConsume(_ x2: borrowing Klass) {
    // expected-error @-1 {{noncopyable 'x2' cannot be consumed when captured by an escaping closure}}
    // expected-error @-2 {{'x2' cannot be captured by an escaping closure since it is a borrowed parameter}}
    // expected-error @-3 {{'x2' cannot be captured by an escaping closure since it is a borrowed parameter}}
    var f = {}
    f = {// expected-note {{closure capturing 'x2' here}}
        var g = {}
        g = {// expected-note {{closure capturing 'x2' here}}
            borrowVal(x2)
            consumeVal(x2) // expected-note {{consumed here}}
            consumeVal(x2) // expected-note {{consumed here}}
        }
        g()
    }
    f()
}

public func closureVarAndClosureCaptureClassOwnedArgUseAfterConsume(_ x2: __owned Klass) {
    var f = {}
    f = {
        var g = {}
        g = {
            borrowVal(x2)
            consumeVal(x2) // expected-error {{noncopyable 'x2' cannot be consumed when captured by an escaping closure}}
            consumeVal(x2) // expected-error {{noncopyable 'x2' cannot be consumed when captured by an escaping closure}}
        }
        g()
    }
    f()
}

public func closureVarAndClosureCaptureClassOwnedArgUseAfterConsume2(_ x2: consuming Klass) {
    var f = {}
    f = {
        var g = {}
        g = {
            borrowVal(x2)
            consumeVal(x2) // expected-error {{noncopyable 'x2' cannot be consumed when captured by an escaping closure}}
            consumeVal(x2) // expected-error {{noncopyable 'x2' cannot be consumed when captured by an escaping closure}}
        }
        g()
    }
    f()
}

public func closureVarAndClosureCaptureClassOwnedArgUseAfterConsume3(_ x2: __owned Klass) {
    var f = {}
    f = {
        var g = {}
        g = {
            borrowVal(x2)
            consumeVal(x2) // expected-error {{noncopyable 'x2' cannot be consumed when captured by an escaping closure}}
            consumeVal(x2) // expected-error {{noncopyable 'x2' cannot be consumed when captured by an escaping closure}}
        }
        g()
    }
    f()
    consumeVal(x2) // expected-error {{noncopyable 'x2' cannot be consumed when captured by an escaping closure}}
}

public func closureVarAndClosureCaptureClassOwnedArgUseAfterConsume4(_ x2: consuming Klass) {
    var f = {}
    f = {
        var g = {}
        g = {
            borrowVal(x2)
            consumeVal(x2) // expected-error {{noncopyable 'x2' cannot be consumed when captured by an escaping closure}}
            consumeVal(x2) // expected-error {{noncopyable 'x2' cannot be consumed when captured by an escaping closure}}
        }
        g()
    }
    f()
    consumeVal(x2) // expected-error {{noncopyable 'x2' cannot be consumed when captured by an escaping closure}}
}

/////////////////////////////////
// MARK: Var and Let Functions //
/////////////////////////////////

public func closureVarAndClosureLetCaptureClassUseAfterConsume(_ x: borrowing Klass) { // expected-error {{'x' is borrowed and cannot be consumed}}
    let x2 = x // expected-note {{consumed here}}
    var f = {}
    f = {
        let g = {
            borrowVal(x2)
            consumeVal(x2) // expected-error {{noncopyable 'x2' cannot be consumed when captured by an escaping closure}}
            consumeVal(x2) // expected-error {{noncopyable 'x2' cannot be consumed when captured by an escaping closure}}
        }
        g()
        consumeVal(x2) // expected-error {{noncopyable 'x2' cannot be consumed when captured by an escaping closure}}
    }
    f()
}

public func closureVarAndClosureLetCaptureClassUseAfterConsume2(_ x: borrowing Klass) { // expected-error {{'x' is borrowed and cannot be consumed}}
    let x2 = x
    // expected-note @-1 {{consumed here}}

    var f = {}
    f = {
        let g = {
            borrowVal(x2)
            consumeVal(x2) // expected-error {{noncopyable 'x2' cannot be consumed when captured by an escaping closure}}
            consumeVal(x2) // expected-error {{noncopyable 'x2' cannot be consumed when captured by an escaping closure}}
        }
        g()
    }
    f()
    consumeVal(x2) // expected-error {{noncopyable 'x2' cannot be consumed when captured by an escaping closure}}
    let x3 = x2 // expected-error {{noncopyable 'x2' cannot be consumed when captured by an escaping closure}}
    _ = x3
}

public func closureVarAndClosureLetCaptureClassUseAfterConsume3(_ x: borrowing Klass) { // expected-error {{'x' is borrowed and cannot be consumed}}
    var x2 = x
    // expected-note @-1 {{consumed here}}
    x2 = x
    // expected-note @-1 {{consumed here}}

    var f = {}
    f = {
        let g = {
            borrowVal(x2)
            consumeVal(x2) // expected-error {{noncopyable 'x2' cannot be consumed when captured by an escaping closure}}
            consumeVal(x2) // expected-error {{noncopyable 'x2' cannot be consumed when captured by an escaping closure}}
        }
        g()
    }
    f()
    consumeVal(x2) // expected-error {{noncopyable 'x2' cannot be consumed when captured by an escaping closure}}
    let x3 = x2 // expected-error {{noncopyable 'x2' cannot be consumed when captured by an escaping closure}}
    _ = x3
}

public func closureVarAndClosureLetCaptureClassArgUseAfterConsume(_ x2: borrowing Klass) {
    // expected-error @-1 {{noncopyable 'x2' cannot be consumed when captured by an escaping closure}}
    // expected-error @-2 {{'x2' cannot be captured by an escaping closure since it is a borrowed parameter}}
    // expected-error @-3 {{'x2' cannot be captured by an escaping closure since it is a borrowed parameter}}
    var f = {}
    f = {// expected-note {{closure capturing 'x2' here}}
let g = {// expected-note {{closure capturing 'x2' here}}
            borrowVal(x2)
            consumeVal(x2) // expected-note {{consumed here}}
            consumeVal(x2) // expected-note {{consumed here}}
        }
        g()
    }
    f()
}

public func closureVarAndClosureLetCaptureClassOwnedArgUseAfterConsume(_ x2: __owned Klass) {
    var f = {}
    f = {
        let g = {
            borrowVal(x2)
            consumeVal(x2) // expected-error {{noncopyable 'x2' cannot be consumed when captured by an escaping closure}}
            consumeVal(x2) // expected-error {{noncopyable 'x2' cannot be consumed when captured by an escaping closure}}
        }
        g()
    }
    f()
}

public func closureVarAndClosureLetCaptureClassOwnedArgUseAfterConsume2(_ x2: consuming Klass) {
    var f = {}
    f = {
        let g = {
            borrowVal(x2)
            consumeVal(x2) // expected-error {{noncopyable 'x2' cannot be consumed when captured by an escaping closure}}
            consumeVal(x2) // expected-error {{noncopyable 'x2' cannot be consumed when captured by an escaping closure}}
        }
        g()
    }
    f()
}

public func closureVarAndClosureLetCaptureClassOwnedArgUseAfterConsume3(_ x2: __owned Klass) {
    var f = {}
    f = {
        let g = {
            borrowVal(x2)
            consumeVal(x2) // expected-error {{noncopyable 'x2' cannot be consumed when captured by an escaping closure}}
            consumeVal(x2) // expected-error {{noncopyable 'x2' cannot be consumed when captured by an escaping closure}}
        }
        g()
    }
    f()
    consumeVal(x2) // expected-error {{noncopyable 'x2' cannot be consumed when captured by an escaping closure}}
}

public func closureVarAndClosureLetCaptureClassOwnedArgUseAfterConsume4(_ x2: consuming Klass) {
    var f = {}
    f = {
        let g = {
            borrowVal(x2)
            consumeVal(x2) // expected-error {{noncopyable 'x2' cannot be consumed when captured by an escaping closure}}
            consumeVal(x2) // expected-error {{noncopyable 'x2' cannot be consumed when captured by an escaping closure}}
        }
        g()
    }
    f()
    consumeVal(x2) // expected-error {{noncopyable 'x2' cannot be consumed when captured by an escaping closure}}
}

public func closureLetAndClosureVarCaptureClassUseAfterConsume(_ x: borrowing Klass) { // expected-error {{'x' is borrowed and cannot be consumed}}
    let x2 = x // expected-note {{consumed here}}
    let f = {
        var g = {}
        g = {
            borrowVal(x2)
            consumeVal(x2) // expected-error {{noncopyable 'x2' cannot be consumed when captured by an escaping closure}}
            consumeVal(x2) // expected-error {{noncopyable 'x2' cannot be consumed when captured by an escaping closure}}
        }
        g()
        consumeVal(x2) // expected-error {{noncopyable 'x2' cannot be consumed when captured by an escaping closure}}
    }
    f()
}

public func closureLetAndClosureVarCaptureClassUseAfterConsume2(_ x: borrowing Klass) { // expected-error {{'x' is borrowed and cannot be consumed}}
    let x2 = x
    // expected-note @-1 {{consumed here}}

    let f = {
        let h = {
            var g = {}
            g = {
                borrowVal(x2)
                consumeVal(x2) // expected-error {{noncopyable 'x2' cannot be consumed when captured by an escaping closure}}
                consumeVal(x2) // expected-error {{noncopyable 'x2' cannot be consumed when captured by an escaping closure}}
            }
            g()
        }
        h()
        consumeVal(x2) // expected-error {{noncopyable 'x2' cannot be consumed when captured by an escaping closure}}
    }
    f()
    consumeVal(x2) // expected-error {{noncopyable 'x2' cannot be consumed when captured by an escaping closure}}
    let x3 = x2 // expected-error {{noncopyable 'x2' cannot be consumed when captured by an escaping closure}}
    _ = x3
}

public func closureLetAndClosureVarCaptureClassUseAfterConsume3(_ x: borrowing Klass) { // expected-error {{'x' is borrowed and cannot be consumed}}
    var x2 = x
    // expected-note @-1 {{consumed here}}
    x2 = x
    // expected-note @-1 {{consumed here}}

    let f = {
        var g = {}
        g = {
            borrowVal(x2)
            consumeVal(x2) // expected-error {{noncopyable 'x2' cannot be consumed when captured by an escaping closure}}
            consumeVal(x2) // expected-error {{noncopyable 'x2' cannot be consumed when captured by an escaping closure}}
        }
        g()
    }
    f()
    consumeVal(x2) // expected-error {{noncopyable 'x2' cannot be consumed when captured by an escaping closure}}
    let x3 = x2 // expected-error {{noncopyable 'x2' cannot be consumed when captured by an escaping closure}}
    _ = x3
}

public func closureLetAndClosureVarCaptureClassArgUseAfterConsume(_ x2: borrowing Klass) {
    // expected-error @-1 {{noncopyable 'x2' cannot be consumed when captured by an escaping closure}}
    // expected-error @-2 {{'x2' cannot be captured by an escaping closure since it is a borrowed parameter}}
    // expected-error @-3 {{'x2' cannot be captured by an escaping closure since it is a borrowed parameter}}
    let f = {// expected-note {{closure capturing 'x2' here}}
        var g = {}
        g = {// expected-note {{closure capturing 'x2' here}}
            borrowVal(x2)
            consumeVal(x2) // expected-note {{consumed here}}
            consumeVal(x2) // expected-note {{consumed here}}
        }
        g()
    }
    f()
}

public func closureLetAndClosureVarCaptureClassOwnedArgUseAfterConsume(_ x2: __owned Klass) {
    let f = {
        var g = {}
        g = {
            borrowVal(x2)
            consumeVal(x2) // expected-error {{noncopyable 'x2' cannot be consumed when captured by an escaping closure}}
            consumeVal(x2) // expected-error {{noncopyable 'x2' cannot be consumed when captured by an escaping closure}}
        }
        g()
    }
    f()
}

public func closureLetAndClosureVarCaptureClassOwnedArgUseAfterConsume2(_ x2: consuming Klass) {
    let f = {
        var g = {}
        g = {
            borrowVal(x2)
            consumeVal(x2) // expected-error {{noncopyable 'x2' cannot be consumed when captured by an escaping closure}}
            consumeVal(x2) // expected-error {{noncopyable 'x2' cannot be consumed when captured by an escaping closure}}
        }
        g()
    }
    f()
}

public func closureLetAndClosureVarCaptureClassOwnedArgUseAfterConsume3(_ x2: __owned Klass) {
    let f = {
        var g = {}
        g = {
            borrowVal(x2)
            consumeVal(x2) // expected-error {{noncopyable 'x2' cannot be consumed when captured by an escaping closure}}
            consumeVal(x2) // expected-error {{noncopyable 'x2' cannot be consumed when captured by an escaping closure}}
        }
        g()
    }
    f()
    consumeVal(x2) // expected-error {{noncopyable 'x2' cannot be consumed when captured by an escaping closure}}
}

public func closureLetAndClosureVarCaptureClassOwnedArgUseAfterConsume4(_ x2: consuming Klass) {
    let f = {
        var g = {}
        g = {
            borrowVal(x2)
            consumeVal(x2) // expected-error {{noncopyable 'x2' cannot be consumed when captured by an escaping closure}}
            consumeVal(x2) // expected-error {{noncopyable 'x2' cannot be consumed when captured by an escaping closure}}
        }
        g()
    }
    f()
    consumeVal(x2) // expected-error {{noncopyable 'x2' cannot be consumed when captured by an escaping closure}}
}

///////////////////////////////////
// MARK: Tests For Move Operator //
///////////////////////////////////

func moveOperatorTest(_ k: __owned Klass) {
    let k2 = k // expected-error {{'k2' consumed more than once}}
    let k3 = consume k2 // expected-note {{consumed here}}
    let _ = consume k2 // expected-note {{consumed again here}}
    let _ = k3
}

func moveOperatorTest2(_ k: consuming Klass) {
    let k2 = k // expected-error {{'k2' consumed more than once}}
    let k3 = consume k2 // expected-note {{consumed here}}
    let _ = consume k2 // expected-note {{consumed again here}}
    let _ = k3
}

/////////////////////////////////////////
// Black hole initialization test case//
/////////////////////////////////////////

func blackHoleTestCase(_ k: __owned Klass) {
    let k2 = k // expected-error {{'k2' consumed more than once}}
    let _ = k2 // expected-note {{consumed here}}
    let _ = k2 // expected-note {{consumed again here}}
}

func blackHoleTestCase2(_ k: consuming Klass) {
    let k2 = k // expected-error {{'k2' consumed more than once}}
    let _ = k2 // expected-note {{consumed here}}
    let _ = k2 // expected-note {{consumed again here}}
}

////////////////////////////////////////////
// Multiple Use by Same CallSite TestCase //
////////////////////////////////////////////

func sameCallSiteTestConsumeTwice(_ k: __owned Klass) { // expected-error {{'k' consumed more than once}}
    func consumeKlassTwice(_ k: __owned Klass, _ k2: __owned Klass) {}
    consumeKlassTwice(k, k)
    // expected-note @-1 {{consumed here}}
    // expected-note @-2 {{consumed again here}}
}

func sameCallSiteConsumeAndUse(_ k: __owned Klass) { // expected-error {{'k' used after consume}}
    func consumeKlassAndUseKlass(_ k: __owned Klass, _ k2: borrowing Klass) {}
    consumeKlassAndUseKlass(k, k)
    // expected-note @-1 {{consumed here}}
    // expected-note @-2 {{used here}}
}

////////////////////////////////
// Recursive Enum Switch Test //
////////////////////////////////

enum EnumSwitchTests {
    @_moveOnly
    enum E2 {
        case lhs(CopyableKlass)
        case rhs(Klass)
    }

    @_moveOnly
    enum E {
        case first(KlassPair)
        case second(AggStruct)
        case third(CopyableKlass)
        case fourth(E2)
    }
}

func consumeVal(_ e: __owned EnumSwitchTests.E2) {}

func enumSwitchTest1(_ e: __owned EnumSwitchTests.E) {
    switch consume e {
    case .first:
        break
    case .second(let x):
        borrowVal(x)
        break
    case .third(let y):
        borrowVal(y)
        break
    case .fourth where boolValue:
        break
    case .fourth(.lhs(let lhs)):
        borrowVal(lhs)
        break
    case .fourth(.rhs(let rhs)):
        consumeVal(rhs)
        break
    }
}

func enumSwitchTest2(_ e: consuming EnumSwitchTests.E) {
    switch consume e {
    case .first:
        break
    case .second(let x):
        borrowVal(x)
        break
    case .third(let y):
        borrowVal(y)
        break
    case .fourth where boolValue:
        break
    case .fourth(.lhs(let lhs)):
        borrowVal(lhs)
        break
    case .fourth(.rhs(let rhs)):
        consumeVal(rhs)
        break
    }
}

///////////////////////////////////////////
// Empty Struct Guaranteed Argument Test //
///////////////////////////////////////////

@_moveOnly
struct EmptyStruct {
  var bool: Bool { false }
  func doSomething() {}
  mutating func doSomething2() {}
  consuming func doSomething3() {}
}

func borrow(_ x: borrowing EmptyStruct) {}
func consume(_ x: consuming EmptyStruct) {}

func testEmptyStruct() {
  func testGuaranteedArg1(_ x: borrowing EmptyStruct) {
    borrow(x)
  }

  func testGuaranteedArg2(_ x: borrowing EmptyStruct) {
    // expected-error @-1 {{'x' is borrowed and cannot be consumed}}
    consume(x) // expected-note {{consumed here}}
  }

  func testGuaranteedArg3(_ x: borrowing EmptyStruct) {
    // expected-error @-1 {{'x' is borrowed and cannot be consumed}}
    let _ = x // expected-note {{consumed here}}
  }

  func testGuaranteedArg4(_ x: borrowing EmptyStruct) {
    _ = x
  }

  func testGuaranteedArg5(_ x: borrowing EmptyStruct) {
    // expected-error @-1 {{'x' is borrowed and cannot be consumed}}
    let y = x // expected-note {{consumed here}}
    _ = y
  }

  func testGuaranteedArg6(_ x: borrowing EmptyStruct) {
    x.doSomething()
  }

  func testGuaranteedArg7(_ x: borrowing EmptyStruct) {
    // expected-error @-1 {{'x' is borrowed and cannot be consumed}}
    x.doSomething3() // expected-note {{consumed here}}
  }

  func testGuaranteedArg7a(_ x: borrowing EmptyStruct) {
    // expected-error @-1 {{'x' is borrowed and cannot be consumed}}
    x.doSomething3() // expected-note {{consumed here}}
    x.doSomething3() // expected-note {{consumed here}}
  }
}

////////////////////////////////////
// Struct Containing Empty Struct //
////////////////////////////////////

// Make sure that we handle a struct that recursively holds an empty struct
// correctly.
@_moveOnly
struct StructContainingEmptyStruct {
  var x: EmptyStruct
}

func borrow(_ x: borrowing StructContainingEmptyStruct) {}
func consume(_ x: consuming StructContainingEmptyStruct) {}

func testStructContainingEmptyStruct() {
  func testGuaranteedArg1(_ x: borrowing StructContainingEmptyStruct) {
    borrow(x)
  }

  func testGuaranteedArg2(_ x: borrowing StructContainingEmptyStruct) {
    // expected-error @-1 {{'x' is borrowed and cannot be consumed}}
    consume(x) // expected-note {{consumed here}}
  }

  func testGuaranteedArg3(_ x: borrowing StructContainingEmptyStruct) {
    // expected-error @-1 {{'x' is borrowed and cannot be consumed}}
    let _ = x // expected-note {{consumed here}}
  }

  func testGuaranteedArg4(_ x: borrowing StructContainingEmptyStruct) {
    _ = x
  }

  func testGuaranteedArg5(_ x: borrowing StructContainingEmptyStruct) {
    // expected-error @-1 {{'x' is borrowed and cannot be consumed}}
    let y = x // expected-note {{consumed here}}
    _ = y
  }

  func testGuaranteedArg6(_ x: borrowing StructContainingEmptyStruct) {
    x.x.doSomething()
  }

  func testGuaranteedArg7(_ x: borrowing StructContainingEmptyStruct) {
    // expected-error @-1 {{'x' is borrowed and cannot be consumed}}
    x.x.doSomething3() // expected-note {{consumed here}}
  }

  func testGuaranteedArg7a(_ x: borrowing StructContainingEmptyStruct) {
    // expected-error @-1 {{cannot use 'x' after partial consume}}
    x.x.doSomething3() // expected-note {{partially consumed here}}
    x.x.doSomething3() // expected-note {{used here}}
  }
}

////////////////////////////////////
// Struct Containing Empty Struct //
////////////////////////////////////

// Make sure that we handle a struct that recursively holds an empty struct
// correctly.
@_moveOnly
struct StructContainingTwoEmptyStruct {
  var x: EmptyStruct
  var y: EmptyStruct
}

func borrow(_ x: borrowing StructContainingTwoEmptyStruct) {}
func consume(_ x: consuming StructContainingTwoEmptyStruct) {}

func testStructContainingTwoEmptyStruct() {
  func testGuaranteedArg1(_ x: borrowing StructContainingTwoEmptyStruct) {
    borrow(x)
  }

  func testGuaranteedArg2(_ x: borrowing StructContainingTwoEmptyStruct) {
    // expected-error @-1 {{'x' is borrowed and cannot be consumed}}
    consume(x) // expected-note {{consumed here}}
  }

  func testGuaranteedArg3(_ x: borrowing StructContainingTwoEmptyStruct) {
    // expected-error @-1 {{'x' is borrowed and cannot be consumed}}
    let _ = x // expected-note {{consumed here}}
  }

  func testGuaranteedArg4(_ x: borrowing StructContainingTwoEmptyStruct) {
    _ = x
  }

  func testGuaranteedArg5(_ x: borrowing StructContainingTwoEmptyStruct) {
    // expected-error @-1 {{'x' is borrowed and cannot be consumed}}
    let y = x // expected-note {{consumed here}}
    _ = y
  }

  func testGuaranteedArg6(_ x: borrowing StructContainingTwoEmptyStruct) {
    x.x.doSomething()
  }

  func testGuaranteedArg7(_ x: borrowing StructContainingTwoEmptyStruct) {
    // expected-error @-1 {{'x' is borrowed and cannot be consumed}}
    x.x.doSomething3() // expected-note {{consumed here}}
  }

  func testGuaranteedArg8(_ x: borrowing StructContainingTwoEmptyStruct) {
    // expected-error @-1 {{'x' is borrowed and cannot be consumed}}
    x.y.doSomething3() // expected-note {{consumed here}}
  }
}

//////////////////////////////////
// Enum Containing Empty Struct //
//////////////////////////////////

@_moveOnly
enum MyEnum2 {
case first(EmptyStruct)
case second(String)
}

@_moveOnly
enum MyEnum {
case first(EmptyStruct)
case second(String)
case third(MyEnum2)
}

func testMyEnum() {
  func test1(_ x: borrowing MyEnum) { // expected-error {{'x' is borrowed and cannot be consumed}}
    if case let .first(y) = consume x { // expected-note {{consumed here}}
      _ = y
    }
  }

  func test2(_ x: borrowing MyEnum) { // expected-error {{'x' is borrowed and cannot be consumed}}
    if case let .third(.first(y)) = consume x { // expected-note {{consumed here}}
      _ = y
    }
  }
}
