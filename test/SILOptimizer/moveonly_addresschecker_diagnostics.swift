// RUN: %target-swift-emit-sil -sil-verify-all -verify -enable-experimental-feature NoImplicitCopy -enable-experimental-feature MoveOnlyClasses %s

//////////////////
// Declarations //
//////////////////

public class CopyableKlass {}

public func borrowVal(_ x: borrowing CopyableKlass) {}
public func borrowVal(_ x: borrowing Klass) {}
public func borrowVal(_ s: borrowing NonTrivialStruct) {}
public func borrowVal(_ s: borrowing NonTrivialStruct2) {}
public func borrowVal(_ s: borrowing NonTrivialCopyableStruct) {}
public func borrowVal(_ s: borrowing NonTrivialCopyableStruct2) {}
public func borrowVal(_ e : borrowing NonTrivialEnum) {}
public func borrowVal(_ x: borrowing FinalKlass) {}
public func borrowVal(_ x: borrowing AggStruct) {}
public func borrowVal(_ x: borrowing AggGenericStruct<CopyableKlass>) {}
public func borrowVal<T>(_ x: borrowing AggGenericStruct<T>) {}
public func borrowVal(_ x: borrowing EnumTy) {}
public func borrowVal<T>(_ x: borrowing AddressOnlyGeneric<T>) {}
public func borrowVal(_ x: borrowing AddressOnlyProtocol) {}
public func borrowVal<T>(_ x: borrowing T) {}

public func consumeVal(_ x: __owned CopyableKlass) {}
public func consumeVal(_ x: __owned Klass) {}
public func consumeVal(_ x: __owned FinalKlass) {}
public func consumeVal(_ x: __owned AggStruct) {}
public func consumeVal(_ x: __owned AggGenericStruct<CopyableKlass>) {}
public func consumeVal<T>(_ x: __owned AggGenericStruct<T>) {}
public func consumeVal(_ x: __owned EnumTy) {}
public func consumeVal(_ x: __owned NonTrivialStruct) {}
public func consumeVal(_ x: __owned NonTrivialStruct2) {}
public func consumeVal<T>(_ x: __owned AddressOnlyGeneric<T>) {}
public func consumeVal(_ x: __owned AddressOnlyProtocol) {}
public func consumeVal<T>(_ x: __owned T) {}

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

@_moveOnly
public struct NonTrivialStruct {
    var k = Klass()
    var copyableK = CopyableKlass()
    var nonTrivialStruct2 = NonTrivialStruct2()
    var nonTrivialCopyableStruct = NonTrivialCopyableStruct()

    var computedCopyableK: CopyableKlass { CopyableKlass() }
}

@_moveOnly
public struct NonTrivialStruct2 {
    var copyableKlass = CopyableKlass()
}

public struct NonTrivialCopyableStruct {
    var copyableKlass = CopyableKlass()
    var nonTrivialCopyableStruct2 = NonTrivialCopyableStruct2()
}

public struct NonTrivialCopyableStruct2 {
    var copyableKlass = CopyableKlass()
    var computedCopyableKlass: CopyableKlass { CopyableKlass() }
}

@_moveOnly
public enum NonTrivialEnum {
    case first
    case second(Klass)
    case third(NonTrivialStruct)
    case fourth(CopyableKlass)
}

@_moveOnly
public final class FinalKlass {
    var k: Klass = Klass()
}

public final class CopyableKlassWithMoveOnlyField {
    var moveOnlyVarStruct = NonTrivialStruct()
    let moveOnlyLetStruct = NonTrivialStruct()
}

public protocol P {
    static var value: Self { get }
    var name: CopyableKlass { get }
    static var value2: any P { get }
}

@_moveOnly
public struct AddressOnlyGeneric<T : P> {
    var copyable: T
    var moveOnly = NonTrivialStruct()

    init() {
        self.copyable = T.value
    }

    init(_ input1: T) {
        copyable = input1
        moveOnly = NonTrivialStruct()
    }
}

extension CopyableKlass : P {
    public static var value: Self { fatalError() }
    public static var value2: any P { CopyableKlass() }
    public var name: CopyableKlass { CopyableKlass() }
}

@_moveOnly
public struct AddressOnlyProtocol {
    var copyable: any P = CopyableKlass.value2
    var moveOnly = NonTrivialStruct()
}

///////////
// Tests //
///////////

/////////////////
// Class Tests //
/////////////////

public func classSimpleChainTest(_ x: borrowing Klass) { // expected-error {{'x' has guaranteed ownership but was consumed}}
    var x2 = x // expected-note {{consuming use here}}
               // expected-error @-1 {{'x2' consumed more than once}}
    x2 = x // expected-note {{consuming use here}}
    let y2 = x2 // expected-note {{consuming use here}}
    let k2 = y2
    let k3 = x2 // expected-note {{consuming use here}}
    let _ = k3
    borrowVal(k2)
}

public func classSimpleChainArgTest(_ x2: inout Klass) {
    // expected-error @-1 {{'x2' consumed more than once}}
    // expected-error @-2 {{'x2' consumed but not reinitialized before end of function}}
    var y2 = x2 // expected-note {{consuming use here}}
    y2 = x2 // expected-note {{consuming use here}}
    // expected-note @-1 {{consuming use here}}
    let k2 = y2
    borrowVal(k2)
}

public func classSimpleNonConsumingUseTest(_ x: borrowing Klass) { // expected-error {{'x' has guaranteed ownership but was consumed}}
    var x2 = x // expected-note {{consuming use here}}
    x2 = x // expected-note {{consuming use here}}
    borrowVal(x2)
}

public func classSimpleNonConsumingUseArgTest(_ x2: inout Klass) {
    borrowVal(x2)
}

public func classMultipleNonConsumingUseTest(_ x: borrowing Klass) { // expected-error {{'x' has guaranteed ownership but was consumed}}
    var x2 = x // expected-note {{consuming use here}}
    x2 = x // expected-note {{consuming use here}}
    borrowVal(x2)
    borrowVal(x2)
    consumeVal(x2)
}

public func classMultipleNonConsumingUseArgTest(_ x2: inout Klass) { // expected-error {{'x2' consumed but not reinitialized before end of function}}
    borrowVal(x2)
    borrowVal(x2)
    consumeVal(x2) // expected-note {{consuming use here}}
}

public func classMultipleNonConsumingUseArgTest2(_ x2: inout Klass) { // expected-error {{'x2' used after consume}}
    borrowVal(x2)
    borrowVal(x2)
    consumeVal(x2) // expected-note {{consuming use here}}
    borrowVal(x2) // expected-note {{non-consuming use here}}
}

public func classMultipleNonConsumingUseArgTest3(_ x2: inout Klass) {  // expected-error {{'x2' consumed but not reinitialized before end of function}}
                                                                       // expected-error @-1 {{'x2' consumed more than once}}
    borrowVal(x2)
    borrowVal(x2)
    consumeVal(x2) // expected-note {{consuming use here}}
    consumeVal(x2) // expected-note {{consuming use here}}
                   // expected-note @-1 {{consuming use here}}
}

public func classMultipleNonConsumingUseArgTest4(_ x2: inout Klass) { // expected-error {{'x2' used after consume}}
    borrowVal(x2)
    borrowVal(x2)
    consumeVal(x2) // expected-note {{consuming use here}}
    borrowVal(x2) // expected-note {{non-consuming use here}}
    x2 = Klass()
}


public func classUseAfterConsume(_ x: borrowing Klass) { // expected-error {{'x' has guaranteed ownership but was consumed}}
    var x2 = x // expected-error {{'x2' consumed more than once}}
               // expected-note @-1 {{consuming use here}}
    x2 = x // expected-note {{consuming use here}}
    borrowVal(x2)
    consumeVal(x2) // expected-note {{consuming use here}}
    consumeVal(x2) // expected-note {{consuming use here}}
}

public func classUseAfterConsumeArg(_ x2: inout Klass) { // expected-error {{'x2' consumed but not reinitialized before end of function}}
                                                         // expected-error @-1 {{'x2' consumed more than once}}
    borrowVal(x2)
    consumeVal(x2) // expected-note {{consuming use here}}
    consumeVal(x2) // expected-note {{consuming use here}}
                   // expected-note @-1 {{consuming use here}}
}

public func classDoubleConsume(_ x: borrowing Klass) { // expected-error {{'x' has guaranteed ownership but was consumed}}
    var x2 = x  // expected-error {{'x2' consumed more than once}}
                // expected-note @-1 {{consuming use here}}
    x2 = Klass()
    consumeVal(x2) // expected-note {{consuming use here}}
    consumeVal(x2) // expected-note {{consuming use here}}
}

public func classDoubleConsumeArg(_ x2: inout Klass) { // expected-error {{'x2' consumed but not reinitialized before end of function}}
                                                       // expected-error @-1 {{'x2' consumed more than once}}
    consumeVal(x2) // expected-note {{consuming use here}}
    consumeVal(x2) // expected-note {{consuming use here}}
                     // expected-note @-1 {{consuming use here}}
}

public func classLoopConsume(_ x: borrowing Klass) { // expected-error {{'x' has guaranteed ownership but was consumed}}
    var x2 = x // expected-error {{'x2' consumed by a use in a loop}}
               // expected-note @-1 {{consuming use here}}
    x2 = Klass()
    for _ in 0..<1024 {
        consumeVal(x2) // expected-note {{consuming use here}}
    }
}

public func classLoopConsumeArg(_ x2: inout Klass) { // expected-error {{'x2' consumed but not reinitialized before end of function}}
    for _ in 0..<1024 {
        consumeVal(x2) // expected-note {{consuming use here}}
    }
}

public func classLoopConsumeArg2(_ x2: inout Klass) { // expected-error {{'x2' consumed by a use in a loop}}
    for _ in 0..<1024 {
        consumeVal(x2) // expected-note {{consuming use here}}
    }
    x2 = Klass()
}

public func classDiamond(_ x: borrowing Klass) { // expected-error {{'x' has guaranteed ownership but was consumed}}
    var x2 = x // expected-note {{consuming use here}}
    x2 = Klass()
    if boolValue {
        consumeVal(x2)
    } else {
        consumeVal(x2)
    }
}

public func classDiamondArg(_ x2: inout Klass) { // expected-error {{'x2' consumed but not reinitialized before end of function}}
                                                 // expected-error @-1 {{'x2' consumed but not reinitialized before end of function}}
    if boolValue {
        consumeVal(x2) // expected-note {{consuming use here}}
    } else {
        consumeVal(x2) // expected-note {{consuming use here}}
    }
}

public func classDiamondInLoop(_ x: borrowing Klass) { // expected-error {{'x' has guaranteed ownership but was consumed}}
    var x2 = x // expected-error {{'x2' consumed by a use in a loop}}
               // expected-error @-1 {{'x2' consumed more than once}}
               // expected-note @-2 {{consuming use here}}
    x2 = Klass()
    for _ in 0..<1024 {
      if boolValue {
          consumeVal(x2) // expected-note {{consuming use here}}
      } else {
          consumeVal(x2) // expected-note {{consuming use here}}
                           // expected-note @-1 {{consuming use here}}
      }
    }
}

public func classDiamondInLoopArg(_ x2: inout Klass) { // expected-error {{'x2' consumed but not reinitialized before end of function}}
                                                       // expected-error @-1 {{'x2' consumed but not reinitialized before end of function}}
    for _ in 0..<1024 {
      if boolValue {
          consumeVal(x2) // expected-note {{consuming use here}}
      } else {
          consumeVal(x2) // expected-note {{consuming use here}}
      }
    }
}

public func classDiamondInLoopArg2(_ x2: inout Klass) { // expected-error {{'x2' consumed by a use in a loop}}
                                                       // expected-error @-1 {{'x2' consumed more than once}}
    for _ in 0..<1024 {
      if boolValue {
          consumeVal(x2) // expected-note {{consuming use here}}
      } else {
          consumeVal(x2) // expected-note {{consuming use here}}
                           // expected-note @-1 {{consuming use here}}
      }
    }
    x2 = Klass()
}

public func classAssignToVar1(_ x: borrowing Klass) { // expected-error {{'x' has guaranteed ownership but was consumed}}
    var x2 = x // expected-error {{'x2' consumed more than once}}
               // expected-note @-1 {{consuming use here}}
    x2 = Klass()
    var x3 = x2 // expected-note {{consuming use here}}
    x3 = x2 // expected-note {{consuming use here}}
    x3 = x // expected-note {{consuming use here}}
    consumeVal(x3)
}

public func classAssignToVar1Arg(_ x2: inout Klass) { // expected-error {{'x2' consumed more than once}}
                                                      // expected-error @-1 {{'x2' consumed but not reinitialized before end of function}}
    var x3 = x2 // expected-note {{consuming use here}}
    x3 = x2 // expected-note {{consuming use here}}
            // expected-note @-1 {{consuming use here}}
    x3 = Klass()
    consumeVal(x3)
}

public func classAssignToVar2(_ x: borrowing Klass) { // expected-error {{'x' has guaranteed ownership but was consumed}}
    var x2 = x // expected-error {{'x2' consumed more than once}}
               // expected-note @-1 {{consuming use here}}
    x2 = Klass()
    var x3 = x2 // expected-note {{consuming use here}}
    x3 = x2 // expected-note {{consuming use here}}
    borrowVal(x3)
}

public func classAssignToVar2Arg(_ x2: inout Klass) { // expected-error {{'x2' consumed more than once}}
                                                      // expected-error @-1 {{'x2' consumed but not reinitialized before end of function}}
    var x3 = x2 // expected-note {{consuming use here}}
    x3 = x2 // expected-note {{consuming use here}}
            // expected-note @-1 {{consuming use here}}
    borrowVal(x3)
}

public func classAssignToVar3(_ x: borrowing Klass) { // expected-error {{'x' has guaranteed ownership but was consumed}}
    var x2 = x // expected-note {{consuming use here}}
    x2 = Klass()
    var x3 = x2
    x3 = x // expected-note {{consuming use here}}
    consumeVal(x3)
}

public func classAssignToVar3Arg(_ x: borrowing Klass, _ x2: inout Klass) { // expected-error {{'x2' consumed but not reinitialized before end of function}}
                                                            // expected-error @-1 {{'x' has guaranteed ownership but was consumed}}
    var x3 = x2 // expected-note {{consuming use here}}
    x3 = x // expected-note {{consuming use here}}
    consumeVal(x3)
}

public func classAssignToVar3Arg2(_ x: borrowing Klass, _ x2: inout Klass) { // expected-error {{'x2' consumed but not reinitialized before end of function}}
                                                                   // expected-error @-1 {{'x' has guaranteed ownership but was consumed}}
    var x3 = x2 // expected-note {{consuming use here}}
    x3 = x // expected-note {{consuming use here}}
    consumeVal(x3)
}

public func classAssignToVar4(_ x: borrowing Klass) { // expected-error {{'x' has guaranteed ownership but was consumed}}
    var x2 = x // expected-error {{'x2' consumed more than once}}
               // expected-note @-1 {{consuming use here}}
    x2 = Klass()
    let x3 = x2 // expected-note {{consuming use here}}
    consumeVal(x2) // expected-note {{consuming use here}}
    consumeVal(x3)
}

public func classAssignToVar4Arg(_ x2: inout Klass) { // expected-error {{'x2' consumed more than once}}
                                                      // expected-error @-1 {{'x2' consumed but not reinitialized before end of function}}
    let x3 = x2 // expected-note {{consuming use here}}
    consumeVal(x2)   // expected-note {{consuming use here}}
                // expected-note @-1 {{consuming use here}}
    consumeVal(x3)
}

public func classAssignToVar5() {
    var x2 = Klass() // expected-error {{'x2' used after consume}}
    x2 = Klass()
    var x3 = x2 // expected-note {{consuming use here}}
    borrowVal(x2) // expected-note {{non-consuming use here}}
    x3 = Klass()
    consumeVal(x3)
}

public func classAssignToVar5Arg(_ x: borrowing Klass, _ x2: inout Klass) {
    // expected-error @-1 {{'x2' used after consume}}
    // expected-error @-2 {{'x' has guaranteed ownership but was consumed}}
    var x3 = x2 // expected-note {{consuming use here}}
    borrowVal(x2) // expected-note {{non-consuming use here}}
    x3 = x // expected-note {{consuming use here}}
    consumeVal(x3)
}

public func classAssignToVar5Arg2(_ x: borrowing Klass, _ x2: inout Klass) { // expected-error {{'x' has guaranteed ownership but was consumed}}
                                                                   // expected-error @-1 {{'x2' used after consume}}
    var x3 = x2 // expected-note {{consuming use here}}
    borrowVal(x2) // expected-note {{non-consuming use here}}
    x3 = x // expected-note {{consuming use here}}
    consumeVal(x3)
    x2 = Klass()
}

public func classAccessAccessField(_ x: borrowing Klass) { // expected-error {{'x' has guaranteed ownership but was consumed}}
    var x2 = x // expected-note {{consuming use here}}
    x2 = Klass()
    borrowVal(x2.k)
    for _ in 0..<1024 {
        borrowVal(x2.k)
    }
}

public func classAccessAccessFieldArg(_ x2: inout Klass) {
    for _ in 0..<1024 {
        borrowVal(x2.k)
    }
}

public func classAccessConsumeField(_ x: borrowing Klass) { // expected-error {{'x' has guaranteed ownership but was consumed}}
    var x2 = x // expected-note {{consuming use here}}
    x2 = Klass()
    // Since a class is a reference type, we do not emit an error here.
    consumeVal(x2.k)
    // expected-error @-1 {{'x2.k' was consumed but it is illegal to consume a noncopyable class var field. One can only read from it or assign to it}}
    for _ in 0..<1024 {
        consumeVal(x2.k)
        // expected-error @-1 {{'x2.k' was consumed but it is illegal to consume a noncopyable class var field. One can only read from it or assign to it}}
    }
}

public func classAccessConsumeFieldArg(_ x2: inout Klass) {
    // Since a class is a reference type, we do not emit an error here.
    consumeVal(x2.k)
    // expected-error @-1 {{'x2.k' was consumed but it is illegal to consume a noncopyable class var field. One can only read from it or assign to it}}

    for _ in 0..<1024 {
        consumeVal(x2.k)
        // expected-error @-1 {{'x2.k' was consumed but it is illegal to consume a noncopyable class var field. One can only read from it or assign to it}}
    }
}

extension Klass {
    func testNoUseSelf() { // expected-error {{'self' has guaranteed ownership but was consumed}}
        let x = self // expected-note {{consuming use here}}
        let _ = x
    }
}

/////////////////
// Final Class //
/////////////////

public func finalClassSimpleChainTest() {
    var x2 = FinalKlass()
    x2 = FinalKlass()
    let y2 = x2
    let k2 = y2
    borrowVal(k2)
}

public func finalClassSimpleChainTestArg(_ x2: inout FinalKlass) { // expected-error {{'x2' consumed but not reinitialized before end of function}}
    let y2 = x2 // expected-note {{consuming use here}}
    let k2 = y2
    borrowVal(k2)
}

public func finalClassSimpleChainTestArg2(_ x2: inout FinalKlass) {
    let y2 = x2
    let k2 = y2
    borrowVal(k2)
    x2 = FinalKlass()
}

public func finalClassSimpleChainTestArg3(_ x2: inout FinalKlass) {
    for _ in 0..<1024 {}
    let y2 = x2
    let k2 = y2
    borrowVal(k2)
    x2 = FinalKlass()
}

public func finalClassSimpleNonConsumingUseTest(_ x: __owned FinalKlass) {
    var x2 = x
    x2 = FinalKlass()
    borrowVal(x2)
}

public func finalClassSimpleNonConsumingUseTest2(_ x: consuming FinalKlass) {
    var x2 = x
    x2 = FinalKlass()
    borrowVal(x2)
}

public func finalClassSimpleNonConsumingUseTestArg(_ x2: inout FinalKlass) {
    borrowVal(x2)
}

public func finalClassMultipleNonConsumingUseTest() {
    var x2 = FinalKlass()
    x2 = FinalKlass()
    borrowVal(x2)
    borrowVal(x2)
    consumeVal(x2)
}

public func finalClassMultipleNonConsumingUseTestArg(_ x2: inout FinalKlass) { // expected-error {{'x2' consumed but not reinitialized before end of function}}
    borrowVal(x2)
    borrowVal(x2)
    consumeVal(x2) // expected-note {{consuming use here}}
}

public func finalClassUseAfterConsume() {
    var x2 = FinalKlass() // expected-error {{'x2' consumed more than once}}
    x2 = FinalKlass()
    borrowVal(x2)
    consumeVal(x2) // expected-note {{consuming use here}}
    consumeVal(x2) // expected-note {{consuming use here}}
}

public func finalClassUseAfterConsumeArg(_ x2: inout FinalKlass) { // expected-error {{'x2' consumed more than once}}
                                                                   // expected-error @-1 {{'x2' consumed but not reinitialized before end of function}}
    borrowVal(x2)
    consumeVal(x2) // expected-note {{consuming use here}}
    consumeVal(x2) // expected-note {{consuming use here}}
              // expected-note @-1 {{consuming use here}}
}

public func finalClassDoubleConsume() {
    var x2 = FinalKlass()  // expected-error {{'x2' consumed more than once}}
    x2 = FinalKlass()
    consumeVal(x2) // expected-note {{consuming use here}}
    consumeVal(x2) // expected-note {{consuming use here}}
}

public func finalClassDoubleConsumeArg(_ x2: inout FinalKlass) { // expected-error {{'x2' consumed more than once}}
                                                                 // expected-error @-1 {{'x2' consumed but not reinitialized before end of function}}    
    consumeVal(x2) // expected-note {{consuming use here}}
    consumeVal(x2) // expected-note {{consuming use here}}
                          // expected-note @-1 {{consuming use here}}
}

public func finalClassLoopConsume() {
    var x2 = FinalKlass() // expected-error {{'x2' consumed by a use in a loop}}
    x2 = FinalKlass()
    for _ in 0..<1024 {
        consumeVal(x2) // expected-note {{consuming use here}}
    }
}

public func finalClassLoopConsumeArg(_ x2: inout FinalKlass) { // expected-error {{'x2' consumed but not reinitialized before end of function}}
    for _ in 0..<1024 {
        consumeVal(x2) // expected-note {{consuming use here}}
    }
}

public func finalClassDiamond() {
    var x2 = FinalKlass()
    x2 = FinalKlass()
    if boolValue {
        consumeVal(x2)
    } else {
        consumeVal(x2)
    }
}

public func finalClassDiamondArg(_ x2: inout FinalKlass) { // expected-error {{'x2' consumed but not reinitialized before end of function}}
                                                           // expected-error @-1 {{'x2' consumed but not reinitialized before end of function}}
    if boolValue {
        consumeVal(x2) // expected-note {{consuming use here}}
    } else {
        consumeVal(x2) // expected-note {{consuming use here}}
    }
}

public func finalClassDiamondInLoop() {
    var x2 = FinalKlass() // expected-error {{'x2' consumed by a use in a loop}}
                          // expected-error @-1 {{'x2' consumed more than once}}
    x2 = FinalKlass()
    for _ in 0..<1024 {
      if boolValue {
          consumeVal(x2) // expected-note {{consuming use here}}
      } else {
          consumeVal(x2) // expected-note {{consuming use here}}
                                // expected-note @-1 {{consuming use here}}
      }
    }
}

public func finalClassDiamondInLoopArg(_ x2: inout FinalKlass) { // expected-error {{'x2' consumed but not reinitialized before end of function}}
                                                                 // expected-error @-1 {{'x2' consumed but not reinitialized before end of function}}
    for _ in 0..<1024 {
      if boolValue {
          consumeVal(x2) // expected-note {{consuming use here}}
      } else {
          consumeVal(x2) // expected-note {{consuming use here}}
      }
    }
}

public func finalClassDiamondInLoopArg2(_ x2: inout FinalKlass) { // expected-error {{consumed by a use in a loop}}
                                                                  // expected-error @-1 {{'x2' consumed more than once}}
    for _ in 0..<1024 {
      if boolValue {
          consumeVal(x2) // expected-note {{consuming use here}}
      } else {
          consumeVal(x2) // expected-note {{consuming use here}}
                                // expected-note @-1 {{consuming use here}}
      }
    }

    x2 = FinalKlass()
}

public func finalClassAssignToVar1() {
    var x2 = FinalKlass() // expected-error {{'x2' consumed more than once}}
    x2 = FinalKlass()
    var x3 = x2 // expected-note {{consuming use here}}
    x3 = x2 // expected-note {{consuming use here}}
    x3 = FinalKlass()
    consumeVal(x3)
}

public func finalClassAssignToVar1Arg(_ x2: inout FinalKlass) {
    // expected-error @-1 {{'x2' consumed more than once}}
    // expected-error @-2 {{'x2' consumed but not reinitialized before end of function}}
    var x3 = x2 // expected-note {{consuming use here}}
    x3 = x2 // expected-note {{consuming use here}}
    // expected-note @-1 {{consuming use here}}
    x3 = FinalKlass()
    consumeVal(x3)
}

public func finalClassAssignToVar2() {
    var x2 = FinalKlass() // expected-error {{'x2' consumed more than once}}
    x2 = FinalKlass()
    var x3 = x2 // expected-note {{consuming use here}}
    x3 = x2 // expected-note {{consuming use here}}
    borrowVal(x3)
}

public func finalClassAssignToVar2Arg(_ x2: inout FinalKlass) {
    // expected-error @-1 {{'x2' consumed but not reinitialized before end of function}}
    // expected-error @-2 {{'x2' consumed more than once}}
    var x3 = x2 // expected-note {{consuming use here}}
    x3 = x2 // expected-note {{consuming use here}}
            // expected-note @-1 {{consuming use here}}
    borrowVal(x3)
}

public func finalClassAssignToVar3() {
    var x2 = Klass()
    x2 = Klass()
    var x3 = x2
    x3 = Klass()
    consumeVal(x3)
}

public func finalClassAssignToVar3Arg(_ x2: inout FinalKlass) {
    // expected-error @-1 {{'x2' consumed but not reinitialized before end of function}}
    var x3 = x2 // expected-note {{consuming use here}}
    x3 = FinalKlass()
    consumeVal(x3)
}

public func finalClassAssignToVar4() {
    var x2 = FinalKlass() // expected-error {{'x2' consumed more than once}}
    x2 = FinalKlass()
    let x3 = x2 // expected-note {{consuming use here}}
    consumeVal(x2) // expected-note {{consuming use here}}
    consumeVal(x3)
}

public func finalClassAssignToVar4Arg(_ x2: inout FinalKlass) {
    // expected-error @-1 {{'x2' consumed but not reinitialized before end of function}}
    // expected-error @-2 {{'x2' consumed more than once}}
    let x3 = x2 // expected-note {{consuming use here}}
    consumeVal(x2) // expected-note {{consuming use here}}
              // expected-note @-1 {{consuming use here}}
    consumeVal(x3)
}

public func finalClassAssignToVar5() {
    var x2 = FinalKlass() // expected-error {{'x2' used after consume}}
    x2 = FinalKlass()
    var x3 = x2 // expected-note {{consuming use here}}
    borrowVal(x2) // expected-note {{non-consuming use here}}
    x3 = FinalKlass()
    consumeVal(x3)
}

public func finalClassAssignToVar5Arg(_ x2: inout FinalKlass) {
    // expected-error @-1 {{'x2' used after consume}}
    var x3 = x2 // expected-note {{consuming use here}}
    borrowVal(x2) // expected-note {{non-consuming use here}}
    x3 = FinalKlass()
    consumeVal(x3)
}

public func finalClassAssignToVar5Arg2(_ x2: inout FinalKlass) {
    // expected-error @-1 {{'x2' consumed but not reinitialized before end of function}}
    var x3 = x2 // expected-note {{consuming use here}}
    x3 = FinalKlass()
    consumeVal(x3)
}

public func finalClassAccessField() {
    var x2 = FinalKlass()
    x2 = FinalKlass()
    borrowVal(x2.k)
    for _ in 0..<1024 {
        borrowVal(x2.k)
    }
}

public func finalClassAccessFieldArg(_ x2: inout FinalKlass) {
    borrowVal(x2.k)
    for _ in 0..<1024 {
        borrowVal(x2.k)
    }
}

public func finalClassConsumeField() {
    var x2 = FinalKlass()
    x2 = FinalKlass()

    consumeVal(x2.k)
    // expected-error @-1 {{'x2.k' was consumed but it is illegal to consume a noncopyable class var field. One can only read from it or assign to it}}
    for _ in 0..<1024 {
        consumeVal(x2.k)
        // expected-error @-1 {{'x2.k' was consumed but it is illegal to consume a noncopyable class var field. One can only read from it or assign to it}}
    }
}

public func finalClassConsumeFieldArg(_ x2: inout FinalKlass) {
    consumeVal(x2.k)
    // expected-error @-1 {{'x2.k' was consumed but it is illegal to consume a noncopyable class var field. One can only read from it or assign to it}}
    for _ in 0..<1024 {
        consumeVal(x2.k)
        // expected-error @-1 {{'x2.k' was consumed but it is illegal to consume a noncopyable class var field. One can only read from it or assign to it}}
    }
}

//////////////////////
// Aggregate Struct //
//////////////////////

@_moveOnly
public struct KlassPair {
    var lhs: Klass = Klass()
    var rhs: Klass = Klass()
}

@_moveOnly
public struct AggStruct {
    var lhs: Klass = Klass()
    var center: Int = 5
    var rhs: Klass = Klass()
    var pair: KlassPair = KlassPair()

    init() {}

    // Testing that DI ignores normal init. We also get an error on our return
    // value from the function since we do not reinitialize self.
    //
    // TODO: Improve error message!
    init(myInit: Int) { // expected-error {{'self' consumed more than once}}
        let x = self // expected-note {{consuming use here}}
        let _ = x
    } // expected-note {{consuming use here}}

    // Make sure we can reinitialize successfully.
    init(myInit2: Int) {
        let x = self
        let _ = x
        self = AggStruct(myInit: myInit2)
    }

    // Testing delegating init.
    //
    // TODO: Improve error to say need to reinitialize self.lhs before end of
    // function.
    init(myInit3: Int) { // expected-error {{'self' consumed more than once}}
        self.init()
        self.center = myInit3
        let x = self.lhs // expected-note {{consuming use here}}
        let _ = x
    } // expected-note {{consuming use here}}

    init(myInit4: Int) {
        self.init()
        self.center = myInit4
        let x = self.lhs
        let _ = x
        self = AggStruct(myInit: myInit4)
    }

    init(myInit5: Int) {
        self.init()
        self.center = myInit5
        let x = self.lhs
        let _ = x
        self.lhs = Klass()
    }
}

public func aggStructSimpleChainTest() {
    var x2 = AggStruct()
    x2 = AggStruct()
    let y2 = x2
    let k2 = y2
    borrowVal(k2)
}

public func aggStructSimpleChainTestArg(_ x2: inout AggStruct) {
    // expected-error @-1 {{'x2' consumed more than once}}
    // expected-error @-2 {{'x2' consumed but not reinitialized before end of function}}
    var y2 = x2 // expected-note {{consuming use here}}
    y2 = x2 // expected-note {{consuming use here}}
    // expected-note @-1 {{consuming use here}}
    let k2 = y2
    borrowVal(k2)
}

public func aggStructSimpleNonConsumingUseTest() {
    var x2 = AggStruct()
    x2 = AggStruct()
    borrowVal(x2)
}

public func aggStructSimpleNonConsumingUseTestArg(_ x2: inout AggStruct) {
    borrowVal(x2)
}

public func aggStructMultipleNonConsumingUseTest() {
    var x2 = AggStruct()
    x2 = AggStruct()
    borrowVal(x2)
    borrowVal(x2)
    consumeVal(x2)
}

public func aggStructMultipleNonConsumingUseTestArg(_ x2: inout AggStruct) { // expected-error {{'x2' consumed but not reinitialized before end of function}}
    borrowVal(x2)
    borrowVal(x2)
    consumeVal(x2) // expected-note {{consuming use here}}
}

public func aggStructUseAfterConsume() {
    var x2 = AggStruct() // expected-error {{'x2' consumed more than once}}
    x2 = AggStruct()
    borrowVal(x2)
    consumeVal(x2) // expected-note {{consuming use here}}
    consumeVal(x2) // expected-note {{consuming use here}}
}

public func aggStructUseAfterConsumeArg(_ x2: inout AggStruct) {
    // expected-error @-1 {{'x2' consumed more than once}}
    // expected-error @-2 {{'x2' consumed but not reinitialized before end of function}}
    borrowVal(x2)
    consumeVal(x2) // expected-note {{consuming use here}}
    consumeVal(x2) // expected-note {{consuming use here}}
              // expected-note @-1 {{consuming use here}}
}

public func aggStructDoubleConsume() {
    var x2 = AggStruct()  // expected-error {{'x2' consumed more than once}}
    x2 = AggStruct()
    consumeVal(x2) // expected-note {{consuming use here}}
    consumeVal(x2) // expected-note {{consuming use here}}
}

public func aggStructDoubleConsumeArg(_ x2: inout AggStruct) {
    // expected-error @-1 {{'x2' consumed but not reinitialized before end of function}}
    // expected-error @-2 {{'x2' consumed more than once}}
    consumeVal(x2) // expected-note {{consuming use here}}
    consumeVal(x2) // expected-note {{consuming use here}}
    // expected-note @-1 {{consuming use here}}
}

public func aggStructLoopConsume() {
    var x2 = AggStruct() // expected-error {{'x2' consumed by a use in a loop}}
    x2 = AggStruct()
    for _ in 0..<1024 {
        consumeVal(x2) // expected-note {{consuming use here}}
    }
}

public func aggStructLoopConsumeArg(_ x2: inout AggStruct) { // expected-error {{'x2' consumed but not reinitialized before end of function}}
    for _ in 0..<1024 {
        consumeVal(x2) // expected-note {{consuming use here}}
    }
}

public func aggStructDiamond() {
    var x2 = AggStruct()
    x2 = AggStruct()
    if boolValue {
        consumeVal(x2)
    } else {
        consumeVal(x2)
    }
}

public func aggStructDiamondArg(_ x2: inout AggStruct) {
    // expected-error @-1 {{'x2' consumed but not reinitialized before end of function}}
    // expected-error @-2 {{'x2' consumed but not reinitialized before end of function}}
    if boolValue {
        consumeVal(x2) // expected-note {{consuming use here}}
    } else {
        consumeVal(x2) // expected-note {{consuming use here}}
    }
}

public func aggStructDiamondInLoop() {
    var x2 = AggStruct()
    // expected-error @-1 {{'x2' consumed by a use in a loop}}
    // expected-error @-2 {{'x2' consumed more than once}}
    x2 = AggStruct()
    for _ in 0..<1024 {
      if boolValue {
          consumeVal(x2) // expected-note {{consuming use here}}
      } else {
          consumeVal(x2) // expected-note {{consuming use here}}
          // expected-note @-1 {{consuming use here}}
      }
    }
}

public func aggStructDiamondInLoopArg(_ x2: inout AggStruct) {
    // expected-error @-1 {{'x2' consumed but not reinitialized before end of function}}
    // expected-error @-2 {{'x2' consumed but not reinitialized before end of function}}
    for _ in 0..<1024 {
      if boolValue {
          consumeVal(x2) // expected-note {{consuming use here}}
      } else {
          consumeVal(x2) // expected-note {{consuming use here}}
      }
    }
}

public func aggStructAccessField() {
    var x2 = AggStruct()
    x2 = AggStruct()
    borrowVal(x2.lhs)
    for _ in 0..<1024 {
        borrowVal(x2.lhs)
    }
}

public func aggStructAccessFieldArg(_ x2: inout AggStruct) {
    borrowVal(x2.lhs)
    for _ in 0..<1024 {
        borrowVal(x2.lhs)
    }
}

public func aggStructConsumeField() {
    var x2 = AggStruct() // expected-error {{'x2' consumed by a use in a loop}}
    // expected-error @-1 {{'x2' consumed more than once}}
    x2 = AggStruct()
    consumeVal(x2.lhs) // expected-note {{consuming use here}}
    for _ in 0..<1024 {
        consumeVal(x2.lhs) // expected-note {{consuming use here}}
        // expected-note @-1 {{consuming use here}}
    }
}

public func aggStructConsumeFieldArg(_ x2: inout AggStruct) {
    // expected-error @-1 {{'x2' consumed but not reinitialized before end of function}}
    // expected-error @-2 {{'x2' consumed but not reinitialized before end of function}}
    consumeVal(x2.lhs) // expected-note {{consuming use here}}
    for _ in 0..<1024 {
        consumeVal(x2.lhs) // expected-note {{consuming use here}}
    }
}

public func aggStructAccessGrandField() {
    var x2 = AggStruct()
    x2 = AggStruct()
    borrowVal(x2.pair.lhs)
    for _ in 0..<1024 {
        borrowVal(x2.pair.lhs)
    }
}

public func aggStructAccessGrandFieldArg(_ x2: inout AggStruct) {
    borrowVal(x2.pair.lhs)
    for _ in 0..<1024 {
        borrowVal(x2.pair.lhs)
    }
}

public func aggStructConsumeGrandField() {
    var x2 = AggStruct() // expected-error {{'x2' consumed by a use in a loop}}
    // expected-error @-1 {{'x2' consumed more than once}}
    x2 = AggStruct()
    consumeVal(x2.pair.lhs) // expected-note {{consuming use here}}
    for _ in 0..<1024 {
        consumeVal(x2.pair.lhs) // expected-note {{consuming use here}}
        // expected-note @-1 {{consuming use here}}
    }
}

public func aggStructConsumeGrandFieldArg(_ x2: inout AggStruct) {
    // expected-error @-1 {{'x2' consumed but not reinitialized before end of function}}
    // expected-error @-2 {{'x2' consumed but not reinitialized before end of function}}
    consumeVal(x2.pair.lhs) // expected-note {{consuming use here}}
    for _ in 0..<1024 {
        consumeVal(x2.pair.lhs) // expected-note {{consuming use here}}
    }
}

//////////////////////////////
// Aggregate Generic Struct //
//////////////////////////////

@_moveOnly
public struct AggGenericStruct<T> {
    var lhs: Klass = Klass()
    var rhs: UnsafeRawPointer? = nil
    var pair: KlassPair = KlassPair()

    // FIXME: this is the only use of the generic parameter and it's totally unused!
    // What are we testing here that's not covered by the non-generic one?
    var ptr2: UnsafePointer<T>? = nil

    init() {}

    // Testing that DI ignores normal init. We also get an error on our return
    // value from the function since we do not reinitialize self.
    //
    // TODO: Improve error message!
    init(myInit: UnsafeRawPointer) { // expected-error {{'self' consumed more than once}}
        let x = self // expected-note {{consuming use here}}
        let _ = x
    } // expected-note {{consuming use here}}

    // Make sure we can reinitialize successfully.
    init(myInit2: UnsafeRawPointer) {
        let x = self
        let _ = x
        self = AggGenericStruct(myInit: myInit2)
    }

    // Testing delegating init.
    //
    // TODO: Improve error to say need to reinitialize self.lhs before end of
    // function.
    init(myInit3: UnsafeRawPointer) { // expected-error {{'self' consumed more than once}}
        self.init()
        self.rhs = myInit3
        let x = self.lhs // expected-note {{consuming use here}}
        let _ = x
    } // expected-note {{consuming use here}}

    init(myInit4: UnsafeRawPointer) {
        self.init()
        self.rhs = myInit4
        let x = self.lhs
        let _ = x
        self = AggGenericStruct(myInit: myInit4)
    }

    init(myInit5: UnsafeRawPointer) {
        self.init()
        self.rhs = myInit5
        let x = self.lhs
        let _ = x
        self.lhs = Klass()
    }
}

public func aggGenericStructSimpleChainTest() {
    var x2 = AggGenericStruct<CopyableKlass>()
    x2 = AggGenericStruct<CopyableKlass>()
    let y2 = x2
    let k2 = y2
    borrowVal(k2)
}

public func aggGenericStructSimpleChainTestArg(_ x2: inout AggGenericStruct<CopyableKlass>) { // expected-error {{'x2' consumed but not reinitialized before end of function}}
    let y2 = x2 // expected-note {{consuming use here}}
    let k2 = y2
    borrowVal(k2)
}

public func aggGenericStructSimpleNonConsumingUseTest() {
    var x2 = AggGenericStruct<CopyableKlass>()
    x2 = AggGenericStruct<CopyableKlass>()
    borrowVal(x2)
}

public func aggGenericStructSimpleNonConsumingUseTestArg(_ x2: inout AggGenericStruct<CopyableKlass>) {
    borrowVal(x2)
}

public func aggGenericStructMultipleNonConsumingUseTest() {
    var x2 = AggGenericStruct<CopyableKlass>()
    x2 = AggGenericStruct<CopyableKlass>()
    borrowVal(x2)
    borrowVal(x2)
    consumeVal(x2)
}

public func aggGenericStructMultipleNonConsumingUseTestArg(_ x2: inout AggGenericStruct<CopyableKlass>) { // expected-error {{'x2' consumed but not reinitialized before end of function}}
    borrowVal(x2)
    borrowVal(x2)
    consumeVal(x2) // expected-note {{consuming use here}}
}

public func aggGenericStructUseAfterConsume() {
    var x2 = AggGenericStruct<CopyableKlass>() // expected-error {{'x2' consumed more than once}}
    x2 = AggGenericStruct<CopyableKlass>()
    borrowVal(x2)
    consumeVal(x2) // expected-note {{consuming use here}}
    consumeVal(x2) // expected-note {{consuming use here}}
}

public func aggGenericStructUseAfterConsumeArg(_ x2: inout AggGenericStruct<CopyableKlass>) {
    // expected-error @-1 {{'x2' consumed but not reinitialized before end of function}}
    // expected-error @-2 {{'x2' consumed more than once}}
    borrowVal(x2)
    consumeVal(x2) // expected-note {{consuming use here}}
    consumeVal(x2) // expected-note {{consuming use here}}
    // expected-note @-1 {{consuming use here}}x
}

public func aggGenericStructDoubleConsume() {
    var x2 = AggGenericStruct<CopyableKlass>()  // expected-error {{'x2' consumed more than once}}
    x2 = AggGenericStruct<CopyableKlass>()
    consumeVal(x2) // expected-note {{consuming use here}}
    consumeVal(x2) // expected-note {{consuming use here}}
}

public func aggGenericStructDoubleConsumeArg(_ x2: inout AggGenericStruct<CopyableKlass>) {
    // expected-error @-1 {{'x2' consumed but not reinitialized before end of function}}
    // expected-error @-2 {{'x2' consumed more than once}}
    consumeVal(x2) // expected-note {{consuming use here}}
    consumeVal(x2) // expected-note {{consuming use here}}
    // expected-note @-1 {{consuming use here}}
}

public func aggGenericStructLoopConsume() {
    var x2 = AggGenericStruct<CopyableKlass>() // expected-error {{'x2' consumed by a use in a loop}}
    x2 = AggGenericStruct<CopyableKlass>()
    for _ in 0..<1024 {
        consumeVal(x2) // expected-note {{consuming use here}}
    }
}

public func aggGenericStructLoopConsumeArg(_ x2: inout AggGenericStruct<CopyableKlass>) { // expected-error {{'x2' consumed but not reinitialized before end of function}}
    for _ in 0..<1024 {
        consumeVal(x2) // expected-note {{consuming use here}}
    }
}

public func aggGenericStructDiamond() {
    var x2 = AggGenericStruct<CopyableKlass>()
    x2 = AggGenericStruct<CopyableKlass>()
    if boolValue {
        consumeVal(x2)
    } else {
        consumeVal(x2)
    }
}

public func aggGenericStructDiamondArg(_ x2: inout AggGenericStruct<CopyableKlass>) {
    // expected-error @-1 {{'x2' consumed but not reinitialized before end of function}}
    // expected-error @-2 {{'x2' consumed but not reinitialized before end of function}}
    if boolValue {
        consumeVal(x2) // expected-note {{consuming use here}}
    } else {
        consumeVal(x2) // expected-note {{consuming use here}}
    }
}

public func aggGenericStructDiamondInLoop() {
    var x2 = AggGenericStruct<CopyableKlass>() // expected-error {{'x2' consumed by a use in a loop}}
    // expected-error @-1 {{'x2' consumed more than once}}
    x2 = AggGenericStruct<CopyableKlass>()
    for _ in 0..<1024 {
      if boolValue {
          consumeVal(x2) // expected-note {{consuming use here}}
      } else {
          consumeVal(x2) // expected-note {{consuming use here}}
          // expected-note @-1 {{consuming use here}}
      }
    }
}

public func aggGenericStructDiamondInLoopArg(_ x2: inout AggGenericStruct<CopyableKlass>) {
    // expected-error @-1 {{'x2' consumed but not reinitialized before end of function}}
    // expected-error @-2 {{'x2' consumed but not reinitialized before end of function}}
    for _ in 0..<1024 {
      if boolValue {
          consumeVal(x2) // expected-note {{consuming use here}}
      } else {
          consumeVal(x2) // expected-note {{consuming use here}}
      }
    }
}

public func aggGenericStructAccessField() {
    var x2 = AggGenericStruct<CopyableKlass>()
    x2 = AggGenericStruct<CopyableKlass>()
    borrowVal(x2.lhs)
    for _ in 0..<1024 {
        borrowVal(x2.lhs)
    }
}

public func aggGenericStructAccessFieldArg(_ x2: inout AggGenericStruct<CopyableKlass>) {
    borrowVal(x2.lhs)
    for _ in 0..<1024 {
        borrowVal(x2.lhs)
    }
}

public func aggGenericStructConsumeField() {
    var x2 = AggGenericStruct<CopyableKlass>() // expected-error {{'x2' consumed by a use in a loop}}
    // expected-error @-1 {{'x2' consumed more than once}}
    x2 = AggGenericStruct<CopyableKlass>()
    consumeVal(x2.lhs) // expected-note {{consuming use here}}
    for _ in 0..<1024 {
        consumeVal(x2.lhs) // expected-note {{consuming use here}}
        // expected-note @-1 {{consuming use here}}
    }
}

public func aggGenericStructConsumeFieldArg(_ x2: inout AggGenericStruct<CopyableKlass>) {
    // expected-error @-1 {{'x2' consumed but not reinitialized before end of function}}
    // expected-error @-2 {{'x2' consumed but not reinitialized before end of function}}
    consumeVal(x2.lhs) // expected-note {{consuming use here}}
    for _ in 0..<1024 {
        consumeVal(x2.lhs) // expected-note {{consuming use here}}
    }
}

public func aggGenericStructAccessGrandField() {
    var x2 = AggGenericStruct<CopyableKlass>()
    x2 = AggGenericStruct<CopyableKlass>()
    borrowVal(x2.pair.lhs)
    for _ in 0..<1024 {
        borrowVal(x2.pair.lhs)
    }
}

public func aggGenericStructAccessGrandFieldArg(_ x2: inout AggGenericStruct<CopyableKlass>) {
    borrowVal(x2.pair.lhs)
    for _ in 0..<1024 {
        borrowVal(x2.pair.lhs)
    }
}

public func aggGenericStructConsumeGrandField() {
    var x2 = AggGenericStruct<CopyableKlass>() // expected-error {{'x2' consumed by a use in a loop}}
    // expected-error @-1 {{'x2' consumed more than once}}
    x2 = AggGenericStruct<CopyableKlass>()
    consumeVal(x2.pair.lhs) // expected-note {{consuming use here}}
    for _ in 0..<1024 {
        consumeVal(x2.pair.lhs) // expected-note {{consuming use here}}
        // expected-note @-1 {{consuming use here}}
    }
}

public func aggGenericStructConsumeGrandField2() {
    var x2 = AggGenericStruct<CopyableKlass>() // expected-error {{'x2' consumed more than once}}
    x2 = AggGenericStruct<CopyableKlass>()
    consumeVal(x2.pair.lhs) // expected-note {{consuming use here}}
    for _ in 0..<1024 {
    }
    consumeVal(x2.pair.lhs) // expected-note {{consuming use here}}
}

public func aggGenericStructConsumeGrandFieldArg(_ x2: inout AggGenericStruct<CopyableKlass>) {
    // expected-error @-1 {{'x2' consumed but not reinitialized before end of function}}
    // expected-error @-2 {{'x2' consumed but not reinitialized before end of function}}
    consumeVal(x2.pair.lhs) // expected-note {{consuming use here}}
    for _ in 0..<1024 {
        consumeVal(x2.pair.lhs) // expected-note {{consuming use here}}
    }
}

////////////////////////////////////////////////////////////
// Aggregate Generic Struct + Generic But Body is Trivial //
////////////////////////////////////////////////////////////

public func aggGenericStructSimpleChainTest<T>(_ x: T.Type) {
    var x2 = AggGenericStruct<T>()
    x2 = AggGenericStruct<T>()
    let y2 = x2
    let k2 = y2
    borrowVal(k2)
}

public func aggGenericStructSimpleChainTestArg<T>(_ x2: inout AggGenericStruct<T>) { // expected-error {{'x2' consumed but not reinitialized before end of function}}
    let y2 = x2 // expected-note {{consuming use here}}
    let k2 = y2
    borrowVal(k2)
}

public func aggGenericStructSimpleNonConsumingUseTest<T>(_ x: T.Type) {
    var x2 = AggGenericStruct<T>()
    x2 = AggGenericStruct<T>()
    borrowVal(x2)
}

public func aggGenericStructSimpleNonConsumingUseTestArg<T>(_ x2: inout AggGenericStruct<T>) {
    borrowVal(x2)
}

public func aggGenericStructMultipleNonConsumingUseTest<T>(_ x: T.Type) {
    var x2 = AggGenericStruct<T>()
    x2 = AggGenericStruct<T>()
    borrowVal(x2)
    borrowVal(x2)
    consumeVal(x2)
}

public func aggGenericStructMultipleNonConsumingUseTestArg<T>(_ x2: inout AggGenericStruct<T>) { //expected-error {{'x2' consumed but not reinitialized before end of function}}
    borrowVal(x2)
    borrowVal(x2)
    consumeVal(x2) // expected-note {{consuming use here}}
}

public func aggGenericStructUseAfterConsume<T>(_ x: T.Type) {
    var x2 = AggGenericStruct<T>() // expected-error {{'x2' consumed more than once}}
    x2 = AggGenericStruct<T>()
    borrowVal(x2)
    consumeVal(x2) // expected-note {{consuming use here}}
    consumeVal(x2) // expected-note {{consuming use here}}
}

public func aggGenericStructUseAfterConsumeArg<T>(_ x2: inout AggGenericStruct<T>) {
    // expected-error @-1 {{'x2' consumed but not reinitialized before end of function}}
    // expected-error @-2 {{'x2' consumed more than once}}
    borrowVal(x2)
    consumeVal(x2) // expected-note {{consuming use here}}
    consumeVal(x2) // expected-note {{consuming use here}}
    // expected-note @-1 {{consuming use here}}
}

public func aggGenericStructDoubleConsume<T>(_ x: T.Type) {
    var x2 = AggGenericStruct<T>() // expected-error {{'x2' consumed more than once}}
    x2 = AggGenericStruct<T>()
    consumeVal(x2) // expected-note {{consuming use here}}
    consumeVal(x2) // expected-note {{consuming use here}}
}

public func aggGenericStructDoubleConsumeArg<T>(_ x2: inout AggGenericStruct<T>) {
    // expected-error @-1 {{'x2' consumed but not reinitialized before end of function}}
    // expected-error @-2 {{'x2' consumed more than once}}
    consumeVal(x2) // expected-note {{consuming use here}}
    consumeVal(x2) // expected-note {{consuming use here}}
    // expected-note @-1 {{consuming use here}}
}

public func aggGenericStructLoopConsume<T>(_ x: T.Type) {
    var x2 = AggGenericStruct<T>() // expected-error {{'x2' consumed by a use in a loop}}
    x2 = AggGenericStruct<T>()
    for _ in 0..<1024 {
        consumeVal(x2) // expected-note {{consuming use here}}
    }
}

public func aggGenericStructLoopConsumeArg<T>(_ x2: inout AggGenericStruct<T>) { // expected-error {{'x2' consumed but not reinitialized before end of function}}
    for _ in 0..<1024 {
        consumeVal(x2) // expected-note {{consuming use here}}
    }
}

public func aggGenericStructDiamond<T>(_ x: T.Type) {
    var x2 = AggGenericStruct<T>()
    x2 = AggGenericStruct<T>()
    if boolValue {
        consumeVal(x2)
    } else {
        consumeVal(x2)
    }
}

public func aggGenericStructDiamondArg<T>(_ x2: inout AggGenericStruct<T>) {
    // expected-error @-1 {{'x2' consumed but not reinitialized before end of function}}
    // expected-error @-2 {{'x2' consumed but not reinitialized before end of function}}
    if boolValue {
        consumeVal(x2) // expected-note {{consuming use here}}
    } else {
        consumeVal(x2) // expected-note {{consuming use here}}
    }
}

public func aggGenericStructDiamondInLoop<T>(_ x: T.Type) {
    var x2 = AggGenericStruct<T>() // expected-error {{'x2' consumed more than once}}
    // expected-error @-1 {{'x2' consumed by a use in a loop}}
    x2 = AggGenericStruct<T>()
    for _ in 0..<1024 {
      if boolValue {
          consumeVal(x2) // expected-note {{consuming use here}}
      } else {
          consumeVal(x2) // expected-note {{consuming use here}}
          // expected-note @-1 {{consuming use here}}
      }
    }
}

public func aggGenericStructDiamondInLoopArg<T>(_ x2: inout AggGenericStruct<T>) {
    // expected-error @-1 {{'x2' consumed but not reinitialized before end of function}}
    // expected-error @-2 {{'x2' consumed but not reinitialized before end of function}}
    for _ in 0..<1024 {
      if boolValue {
          consumeVal(x2) // expected-note {{consuming use here}}
      } else {
          consumeVal(x2) // expected-note {{consuming use here}}
      }
    }
}

public func aggGenericStructAccessField<T>(_ x: T.Type) {
    var x2 = AggGenericStruct<T>()
    x2 = AggGenericStruct<T>()
    borrowVal(x2.lhs)
    for _ in 0..<1024 {
        borrowVal(x2.lhs)
    }
}

public func aggGenericStructAccessFieldArg<T>(_ x2: inout AggGenericStruct<T>) {
    borrowVal(x2.lhs)
    for _ in 0..<1024 {
        borrowVal(x2.lhs)
    }
}

public func aggGenericStructConsumeField<T>(_ x: T.Type) {
    var x2 = AggGenericStruct<T>() // expected-error {{'x2' consumed by a use in a loop}}
    // expected-error @-1 {{'x2' consumed more than once}}
    x2 = AggGenericStruct<T>()
    consumeVal(x2.lhs) // expected-note {{consuming use here}}
    for _ in 0..<1024 {
        consumeVal(x2.lhs) // expected-note {{consuming use here}}
        // expected-note @-1 {{consuming use here}}
    }
}

public func aggGenericStructConsumeFieldArg<T>(_ x2: inout AggGenericStruct<T>) {
    // expected-error @-1 {{'x2' consumed but not reinitialized before end of function}}
    // expected-error @-2 {{'x2' consumed but not reinitialized before end of function}}
    consumeVal(x2.lhs) // expected-note {{consuming use here}}
    for _ in 0..<1024 {
        consumeVal(x2.lhs) // expected-note {{consuming use here}}
    }
}

public func aggGenericStructAccessGrandField<T>(_ x: T.Type) {
    var x2 = AggGenericStruct<T>()
    x2 = AggGenericStruct<T>()
    borrowVal(x2.pair.lhs)
    for _ in 0..<1024 {
        borrowVal(x2.pair.lhs)
    }
}

public func aggGenericStructAccessGrandFieldArg<T>(_ x2: inout AggGenericStruct<T>) {
    borrowVal(x2.pair.lhs)
    for _ in 0..<1024 {
        borrowVal(x2.pair.lhs)
    }
}

public func aggGenericStructConsumeGrandField<T>(_ x: T.Type) {
    var x2 = AggGenericStruct<T>() // expected-error {{'x2' consumed by a use in a loop}}
    // expected-error @-1 {{'x2' consumed more than once}}
    x2 = AggGenericStruct<T>()
    consumeVal(x2.pair.lhs) // expected-note {{consuming use here}}
    for _ in 0..<1024 {
        consumeVal(x2.pair.lhs) // expected-note {{consuming use here}}
        // expected-note @-1 {{consuming use here}}
    }
}

public func aggGenericStructConsumeGrandFieldArg<T>(_ x2: inout AggGenericStruct<T>) {
    // expected-error @-1 {{'x2' consumed but not reinitialized before end of function}}
    // expected-error @-2 {{'x2' consumed but not reinitialized before end of function}}
    consumeVal(x2.pair.lhs) // expected-note {{consuming use here}}
    for _ in 0..<1024 {
        consumeVal(x2.pair.lhs) // expected-note {{consuming use here}}
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

public func enumSimpleChainTest() {
    var x2 = EnumTy.klass(Klass())
    x2 = EnumTy.klass(Klass())
    let y2 = x2
    let k2 = y2
    borrowVal(k2)
}

public func enumSimpleChainTestArg(_ x2: inout EnumTy) { // expected-error {{'x2' consumed but not reinitialized before end of function}}
    let y2 = x2 // expected-note {{consuming use here}}
    let k2 = y2
    borrowVal(k2)
}

public func enumSimpleNonConsumingUseTest() {
    var x2 = EnumTy.klass(Klass())
    x2 = EnumTy.klass(Klass())
    borrowVal(x2)
}

public func enumSimpleNonConsumingUseTestArg(_ x2: inout EnumTy) {
    borrowVal(x2)
}

public func enumMultipleNonConsumingUseTest() {
    var x2 = EnumTy.klass(Klass())
    x2 = EnumTy.klass(Klass())
    borrowVal(x2)
    borrowVal(x2)
    consumeVal(x2)
}

public func enumMultipleNonConsumingUseTestArg(_ x2: inout EnumTy) { // expected-error {{'x2' consumed but not reinitialized before end of function}}
    borrowVal(x2)
    borrowVal(x2)
    consumeVal(x2) // expected-note {{consuming use here}}
}

public func enumUseAfterConsume() {
    var x2 = EnumTy.klass(Klass()) // expected-error {{'x2' consumed more than once}}
    x2 = EnumTy.klass(Klass())
    borrowVal(x2)
    consumeVal(x2) // expected-note {{consuming use here}}
    consumeVal(x2) // expected-note {{consuming use here}}
}

public func enumUseAfterConsumeArg(_ x2: inout EnumTy) {
    // expected-error @-1 {{'x2' consumed but not reinitialized before end of function}}
    // expected-error @-2 {{'x2' consumed more than once}}
    borrowVal(x2)
    consumeVal(x2) // expected-note {{consuming use here}}
    consumeVal(x2) // expected-note {{consuming use here}}
    // expected-note @-1 {{consuming use here}}
}

public func enumDoubleConsume() {
    var x2 = EnumTy.klass(Klass())  // expected-error {{'x2' consumed more than once}}
    x2 = EnumTy.klass(Klass())
    consumeVal(x2) // expected-note {{consuming use here}}
    consumeVal(x2) // expected-note {{consuming use here}}
}

public func enumDoubleConsumeArg(_ x2: inout EnumTy) {
    // expected-error @-1 {{'x2' consumed but not reinitialized before end of function}}
    // expected-error @-2 {{'x2' consumed more than once}} 
    consumeVal(x2) // expected-note {{consuming use here}}
    consumeVal(x2) // expected-note {{consuming use here}}
    // expected-note @-1 {{consuming use here}}
}

public func enumLoopConsume() {
    var x2 = EnumTy.klass(Klass()) // expected-error {{'x2' consumed by a use in a loop}}
    x2 = EnumTy.klass(Klass())
    for _ in 0..<1024 {
        consumeVal(x2) // expected-note {{consuming use here}}
    }
}

public func enumLoopConsumeArg(_ x2: inout EnumTy) { // expected-error {{'x2' consumed but not reinitialized before end of function}}
    for _ in 0..<1024 {
        consumeVal(x2) // expected-note {{consuming use here}}
    }
}

public func enumDiamond() {
    var x2 = EnumTy.klass(Klass())
    x2 = EnumTy.klass(Klass())
    if boolValue {
        consumeVal(x2)
    } else {
        consumeVal(x2)
    }
}

public func enumDiamondArg(_ x2: inout EnumTy) {
    // expected-error @-1 {{'x2' consumed but not reinitialized before end of function}}
    // expected-error @-2 {{'x2' consumed but not reinitialized before end of function}}
    if boolValue {
        consumeVal(x2) // expected-note {{consuming use here}}
    } else {
        consumeVal(x2) // expected-note {{consuming use here}}
    }
}

public func enumDiamondInLoop() {
    var x2 = EnumTy.klass(Klass())
    // expected-error @-1 {{'x2' consumed by a use in a loop}}
    // expected-error @-2 {{'x2' consumed more than once}} 
    x2 = EnumTy.klass(Klass())
    for _ in 0..<1024 {
      if boolValue {
          consumeVal(x2) // expected-note {{consuming use here}}
      } else {
          consumeVal(x2) // expected-note {{consuming use here}}
          // expected-note @-1 {{consuming use here}}
      }
    }
}

public func enumDiamondInLoopArg(_ x2: inout EnumTy) {
    // expected-error @-1 {{'x2' consumed but not reinitialized before end of function}}
    // expected-error @-2 {{'x2' consumed but not reinitialized before end of function}}
    for _ in 0..<1024 {
      if boolValue {
          consumeVal(x2) // expected-note {{consuming use here}}
      } else {
          consumeVal(x2) // expected-note {{consuming use here}}
      }
    }
}

public func enumAssignToVar1() {
    var x2 = EnumTy.klass(Klass()) // expected-error {{'x2' consumed more than once}}
    x2 = EnumTy.klass(Klass())
    var x3 = x2 // expected-note {{consuming use here}}
    x3 = x2 // expected-note {{consuming use here}}
    x3 = EnumTy.klass(Klass())
    consumeVal(x3)
}

public func enumAssignToVar1Arg(_ x2: inout EnumTy) {
    // expected-error @-1 {{'x2' consumed but not reinitialized before end of function}}
    // expected-error @-2 {{'x2' consumed more than once}} 
                                                            
    var x3 = x2 // expected-note {{consuming use here}}
    x3 = x2 // expected-note {{consuming use here}}
    // expected-note @-1 {{consuming use here}}
    x3 = EnumTy.klass(Klass())
    consumeVal(x3)
}

public func enumAssignToVar2() {
    var x2 = EnumTy.klass(Klass()) // expected-error {{'x2' consumed more than once}}
    x2 = EnumTy.klass(Klass())
    var x3 = x2 // expected-note {{consuming use here}}
    x3 = x2 // expected-note {{consuming use here}}
    borrowVal(x3)
}

public func enumAssignToVar2Arg(_ x2: inout EnumTy) {
    // expected-error @-1 {{'x2' consumed but not reinitialized before end of function}}
    // expected-error @-2 {{'x2' consumed more than once}} 
    var x3 = x2 // expected-note {{consuming use here}}
    x3 = x2 // expected-note {{consuming use here}}
     // expected-note @-1 {{consuming use here}}
    borrowVal(x3)
}

public func enumAssignToVar3() {
    var x2 = EnumTy.klass(Klass())
    x2 = EnumTy.klass(Klass())
    var x3 = x2
    x3 = EnumTy.klass(Klass())
    consumeVal(x3)
}

public func enumAssignToVar3Arg(_ x2: inout EnumTy) { // expected-error {{'x2' consumed but not reinitialized before end of function}}
                                                            
    var x3 = x2 // expected-note {{consuming use here}}
    x3 = EnumTy.klass(Klass())
    consumeVal(x3)
}

public func enumAssignToVar4() {
    var x2 = EnumTy.klass(Klass()) // expected-error {{'x2' consumed more than once}}
    x2 = EnumTy.klass(Klass())
    let x3 = x2 // expected-note {{consuming use here}}
    consumeVal(x2) // expected-note {{consuming use here}}
    consumeVal(x3)
}

public func enumAssignToVar4Arg(_ x2: inout EnumTy) {
    // expected-error @-1 {{'x2' consumed but not reinitialized before end of function}}
    // expected-error @-2 {{'x2' consumed more than once}} 
    let x3 = x2 // expected-note {{consuming use here}}
    consumeVal(x2) // expected-note {{consuming use here}}
    // expected-note @-1 {{consuming use here}}
    consumeVal(x3)
}

public func enumAssignToVar5() {
    var x2 = EnumTy.klass(Klass()) // expected-error {{'x2' used after consume}}
    x2 = EnumTy.klass(Klass())
    var x3 = x2 // expected-note {{consuming use here}}
    borrowVal(x2) // expected-note {{non-consuming use here}}
    x3 = EnumTy.klass(Klass())
    consumeVal(x3)
}

public func enumAssignToVar5Arg(_ x2: inout EnumTy) {
    // expected-error @-1 {{'x2' used after consume}}
    var x3 = x2 // expected-note {{consuming use here}}
    borrowVal(x2) // expected-note {{non-consuming use here}}
    x3 = EnumTy.klass(Klass())
    consumeVal(x3)
}

public func enumAssignToVar5Arg2(_ x2: inout EnumTy) { // expected-error {{'x2' consumed but not reinitialized before end of function}}
                                                            
    var x3 = x2 // expected-note {{consuming use here}}
    x3 = EnumTy.klass(Klass())
    consumeVal(x3)
}


public func enumPatternMatchIfLet1() {
    var x2 = EnumTy.klass(Klass()) // expected-error {{'x2' consumed more than once}}
    x2 = EnumTy.klass(Klass())
    if case let EnumTy.klass(x) = x2 { // expected-note {{consuming use here}}
        borrowVal(x)
    }
    if case let EnumTy.klass(x) = x2 { // expected-note {{consuming use here}}
        borrowVal(x)
    }
}

public func enumPatternMatchIfLet1Arg(_ x2: inout EnumTy) {
    // expected-error @-1 {{'x2' consumed but not reinitialized before end of function}}
    // expected-error @-2 {{'x2' consumed more than once}}
    if case let EnumTy.klass(x) = x2 { // expected-note {{consuming use here}}
        borrowVal(x)
    }
    if case let EnumTy.klass(x) = x2 { // expected-note {{consuming use here}}
        // expected-note @-1 {{consuming use here}}
        borrowVal(x)
    }
}

public func enumPatternMatchIfLet2() {
    var x2 = EnumTy.klass(Klass()) // expected-error {{'x2' consumed by a use in a loop}}
    x2 = EnumTy.klass(Klass())
    for _ in 0..<1024 {
        if case let EnumTy.klass(x) = x2 {  // expected-note {{consuming use here}}
            borrowVal(x)
        }
    }
}

public func enumPatternMatchIfLet2Arg(_ x2: inout EnumTy) { // expected-error {{'x2' consumed but not reinitialized before end of function}}
    for _ in 0..<1024 {
        if case let EnumTy.klass(x) = x2 {  // expected-note {{consuming use here}}
            borrowVal(x)
        }
    }
}

public func enumPatternMatchSwitch1() {
    var x2 = EnumTy.klass(Klass()) // expected-error {{'x2' used after consume}}
    x2 = EnumTy.klass(Klass())
    switch x2 { // expected-note {{consuming use here}}
    case let EnumTy.klass(k):
        borrowVal(k)
        borrowVal(x2) // expected-note {{non-consuming use here}}
    case .int:
        break
    }
}

public func enumPatternMatchSwitch1Arg(_ x2: inout EnumTy) { // expected-error {{'x2' consumed but not reinitialized before end of function}}
    switch x2 { // expected-note {{consuming use here}}
    case let EnumTy.klass(k):
        borrowVal(k)
        borrowVal(x2)
    case .int:
        break
    }
}

public func enumPatternMatchSwitch2() {
    var x2 = EnumTy.klass(Klass())
    x2 = EnumTy.klass(Klass())
    switch x2 {
    case let EnumTy.klass(k):
        borrowVal(k)
    case .int:
        break
    }
}

public func enumPatternMatchSwitch2Arg(_ x2: inout EnumTy) { // expected-error {{'x2' consumed but not reinitialized before end of function}}
    switch x2 { // expected-note {{consuming use here}}
    case let EnumTy.klass(k):
        borrowVal(k)
    case .int:
        break
    }
}

// QOI: We can do better here. We should also flag x2
public func enumPatternMatchSwitch2WhereClause() {
    var x2 = EnumTy.klass(Klass()) // expected-error {{'x2' used after consume}}
    x2 = EnumTy.klass(Klass())
    switch x2 { // expected-note {{consuming use here}}
    case let EnumTy.klass(k)
           where x2.doSomething(): // expected-note {{non-consuming use here}}
        borrowVal(k)
    case .int:
        break
    case EnumTy.klass:
        break
    }
}

public func enumPatternMatchSwitch2WhereClauseArg(_ x2: inout EnumTy) { // expected-error {{'x2' consumed but not reinitialized before end of function}}
    switch x2 { // expected-note {{consuming use here}}
    case let EnumTy.klass(k)
           where x2.doSomething():
        borrowVal(k)
    case .int:
        break
    case EnumTy.klass:
        break
    }
}

public func enumPatternMatchSwitch2WhereClause2() {
    var x2 = EnumTy.klass(Klass())
    x2 = EnumTy.klass(Klass())
    switch x2 {
    case let EnumTy.klass(k)
           where boolValue:
        borrowVal(k)
    case .int:
        break
    case EnumTy.klass:
        break
    }
}

public func enumPatternMatchSwitch2WhereClause2Arg(_ x2: inout EnumTy) { // expected-error {{'x2' consumed but not reinitialized before end of function}}
    switch x2 { // expected-note {{consuming use here}}
    case let EnumTy.klass(k)
           where boolValue:
        borrowVal(k)
    case .int:
        break
    case EnumTy.klass:
        break
    }
}

////////////////////////////////
// Address Only Generic Tests //
////////////////////////////////

public func addressOnlyGenericSimpleChainTest<T>(_ x: borrowing AddressOnlyGeneric<T>) { // expected-error {{'x' has guaranteed ownership but was consumed}}
    // expected-error @-1 {{'x' has guaranteed ownership but was consumed}}
    var x2 = x // expected-note {{consuming use here}}
               // expected-error @-1 {{'x2' consumed more than once}}
    x2 = x // expected-note {{consuming use here}}
    let y2 = x2 // expected-note {{consuming use here}}
    let k2 = y2
    let k3 = x2 // expected-note {{consuming use here}}
    let _ = k3
    borrowVal(k2)
}

public func addressOnlyGenericSimpleChainArgTest<T>(_ x2: inout AddressOnlyGeneric<T>) {
    // expected-error @-1 {{'x2' consumed more than once}}
    // expected-error @-2 {{'x2' consumed but not reinitialized before end of function}}
    var y2 = x2 // expected-note {{consuming use here}}
    y2 = x2 // expected-note {{consuming use here}}
    // expected-note @-1 {{consuming use here}}
    let k2 = y2
    borrowVal(k2)
}

public func addressOnlyGenericSimpleChainConsumingArgTest<T>(_ x2: consuming AddressOnlyGeneric<T>) {
    // expected-error @-1 {{'x2' consumed more than once}}
    var y2 = x2 // expected-note {{consuming use here}}
    y2 = x2 // expected-note {{consuming use here}}
    let k2 = y2
    borrowVal(k2)
}

public func addressOnlyGenericSimpleNonConsumingUseTest<T>(_ x: borrowing AddressOnlyGeneric<T>) {
    // expected-error @-1 {{'x' has guaranteed ownership but was consumed}}
    // expected-error @-2 {{'x' has guaranteed ownership but was consumed}}
    var x2 = x // expected-note {{consuming use here}}
    x2 = x // expected-note {{consuming use here}}
    borrowVal(x2)
}

public func addressOnlyGenericSimpleNonConsumingUseArgTest<T>(_ x2: inout AddressOnlyGeneric<T>) {
    borrowVal(x2)
}

public func addressOnlyGenericMultipleNonConsumingUseTest<T>(_ x: borrowing AddressOnlyGeneric<T>) {
    // expected-error @-1 {{'x' has guaranteed ownership but was consumed}}
    // expected-error @-2 {{'x' has guaranteed ownership but was consumed}}
    var x2 = x // expected-note {{consuming use here}}
    x2 = x // expected-note {{consuming use here}}
    borrowVal(x2)
    borrowVal(x2)
    consumeVal(x2)
}

public func addressOnlyGenericMultipleNonConsumingUseArgTest<T>(_ x2: inout AddressOnlyGeneric<T>) { // expected-error {{'x2' consumed but not reinitialized before end of function}}
    borrowVal(x2)
    borrowVal(x2)
    consumeVal(x2) // expected-note {{consuming use here}}
}

public func addressOnlyGenericMultipleNonConsumingUseArgTest2<T>(_ x2: inout AddressOnlyGeneric<T>) { // expected-error {{'x2' used after consume}}
    borrowVal(x2)
    borrowVal(x2)
    consumeVal(x2) // expected-note {{consuming use here}}
    borrowVal(x2) // expected-note {{non-consuming use here}}
}

public func addressOnlyGenericMultipleNonConsumingUseArgTest3<T>(_ x2: inout AddressOnlyGeneric<T>) {  // expected-error {{'x2' consumed but not reinitialized before end of function}}
                                                                       // expected-error @-1 {{'x2' consumed more than once}}
    borrowVal(x2)
    borrowVal(x2)
    consumeVal(x2) // expected-note {{consuming use here}}
    consumeVal(x2) // expected-note {{consuming use here}}
                   // expected-note @-1 {{consuming use here}}
}

public func addressOnlyGenericMultipleNonConsumingUseArgTest4<T>(_ x2: inout AddressOnlyGeneric<T>) { // expected-error {{'x2' used after consume}}
    borrowVal(x2)
    borrowVal(x2)
    consumeVal(x2) // expected-note {{consuming use here}}
    borrowVal(x2) // expected-note {{non-consuming use here}}
    x2 = AddressOnlyGeneric<T>()
}

public func addressOnlyGenericMultipleNonConsumingUseConsumingArgTest<T>(_ x2: consuming AddressOnlyGeneric<T>) {
    borrowVal(x2)
    borrowVal(x2)
    consumeVal(x2)
}

public func addressOnlyGenericMultipleNonConsumingUseConsumingArgTest2<T>(_ x2: consuming AddressOnlyGeneric<T>) { // expected-error {{'x2' used after consume}}
    borrowVal(x2)
    borrowVal(x2)
    consumeVal(x2) // expected-note {{consuming use here}}
    borrowVal(x2) // expected-note {{non-consuming use here}}
}

public func addressOnlyGenericMultipleNonConsumingUseConsumingArgTest3<T>(_ x2: consuming AddressOnlyGeneric<T>) {
    // expected-error @-1 {{'x2' consumed more than once}}
    borrowVal(x2)
    borrowVal(x2)
    consumeVal(x2) // expected-note {{consuming use here}}
    consumeVal(x2) // expected-note {{consuming use here}}
}

public func addressOnlyGenericMultipleNonConsumingUseConsumingArgTest4<T>(_ x2: consuming AddressOnlyGeneric<T>) { // expected-error {{'x2' used after consume}}
    borrowVal(x2)
    borrowVal(x2)
    consumeVal(x2) // expected-note {{consuming use here}}
    borrowVal(x2) // expected-note {{non-consuming use here}}
    x2 = AddressOnlyGeneric<T>()
}

public func addressOnlyGenericUseAfterConsume<T>(_ x: borrowing AddressOnlyGeneric<T>) {
    // expected-error @-1 {{'x' has guaranteed ownership but was consumed}}
    // expected-error @-2 {{'x' has guaranteed ownership but was consumed}}
    var x2 = x // expected-error {{'x2' consumed more than once}}
               // expected-note @-1 {{consuming use here}}
    x2 = x // expected-note {{consuming use here}}
    borrowVal(x2)
    consumeVal(x2) // expected-note {{consuming use here}}
    consumeVal(x2) // expected-note {{consuming use here}}
}

public func addressOnlyGenericUseAfterConsumeArg<T>(_ x2: inout AddressOnlyGeneric<T>) { // expected-error {{'x2' consumed but not reinitialized before end of function}}
                                                         // expected-error @-1 {{'x2' consumed more than once}}
    borrowVal(x2)
    consumeVal(x2) // expected-note {{consuming use here}}
    consumeVal(x2) // expected-note {{consuming use here}}
                   // expected-note @-1 {{consuming use here}}
}

public func addressOnlyGenericUseAfterConsumeArg2<T>(_ x2: consuming AddressOnlyGeneric<T>) {
    // expected-error @-1 {{'x2' consumed more than once}}
    borrowVal(x2)
    consumeVal(x2) // expected-note {{consuming use here}}
    consumeVal(x2) // expected-note {{consuming use here}}
}

public func addressOnlyGenericDoubleConsume<T>(_ x: borrowing AddressOnlyGeneric<T>) { // expected-error {{'x' has guaranteed ownership but was consumed}}
    var x2 = x  // expected-error {{'x2' consumed more than once}}
                // expected-note @-1 {{consuming use here}}
    x2 = AddressOnlyGeneric<T>()
    consumeVal(x2) // expected-note {{consuming use here}}
    consumeVal(x2) // expected-note {{consuming use here}}
}

public func addressOnlyGenericDoubleConsumeArg<T>(_ x2: inout AddressOnlyGeneric<T>) { // expected-error {{'x2' consumed but not reinitialized before end of function}}
                                                       // expected-error @-1 {{'x2' consumed more than once}}
    consumeVal(x2) // expected-note {{consuming use here}}
    consumeVal(x2) // expected-note {{consuming use here}}
                     // expected-note @-1 {{consuming use here}}
}

public func addressOnlyGenericDoubleConsumeArg2<T>(_ x2: consuming AddressOnlyGeneric<T>) {
                                                       // expected-error @-1 {{'x2' consumed more than once}}
    consumeVal(x2) // expected-note {{consuming use here}}
    consumeVal(x2) // expected-note {{consuming use here}}
}

public func addressOnlyGenericLoopConsume<T>(_ x: borrowing AddressOnlyGeneric<T>) { // expected-error {{'x' has guaranteed ownership but was consumed}}
    var x2 = x // expected-error {{'x2' consumed by a use in a loop}}
               // expected-note @-1 {{consuming use here}}
    x2 = AddressOnlyGeneric<T>()
    for _ in 0..<1024 {
        consumeVal(x2) // expected-note {{consuming use here}}
    }
}

public func addressOnlyGenericLoopConsumeArg<T>(_ x2: inout AddressOnlyGeneric<T>) { // expected-error {{'x2' consumed but not reinitialized before end of function}}
    for _ in 0..<1024 {
        consumeVal(x2) // expected-note {{consuming use here}}
    }
}

public func addressOnlyGenericLoopConsumeArg2<T>(_ x2: inout AddressOnlyGeneric<T>) { // expected-error {{'x2' consumed by a use in a loop}}
    for _ in 0..<1024 {
        consumeVal(x2) // expected-note {{consuming use here}}
    }
    x2 = AddressOnlyGeneric<T>()
}

public func addressOnlyGenericLoopConsumeArg3<T>(_ x2: consuming AddressOnlyGeneric<T>) { // expected-error {{'x2' consumed by a use in a loop}}
    for _ in 0..<1024 {
        consumeVal(x2) // expected-note {{consuming use here}}
    }
    x2 = AddressOnlyGeneric<T>()
}

public func addressOnlyGenericDiamond<T>(_ x: borrowing AddressOnlyGeneric<T>) { // expected-error {{'x' has guaranteed ownership but was consumed}}
    var x2 = x // expected-note {{consuming use here}}
    x2 = AddressOnlyGeneric<T>()
    if boolValue {
        consumeVal(x2)
    } else {
        consumeVal(x2)
    }
}

public func addressOnlyGenericDiamondArg<T>(_ x2: inout AddressOnlyGeneric<T>) { // expected-error {{'x2' consumed but not reinitialized before end of function}}
                                                 // expected-error @-1 {{'x2' consumed but not reinitialized before end of function}}
    if boolValue {
        consumeVal(x2) // expected-note {{consuming use here}}
    } else {
        consumeVal(x2) // expected-note {{consuming use here}}
    }
}

public func addressOnlyGenericDiamondArg2<T>(_ x2: consuming AddressOnlyGeneric<T>) {
    if boolValue {
        consumeVal(x2)
    } else {
        consumeVal(x2)
    }
}

public func addressOnlyGenericDiamondInLoop<T>(_ x: borrowing AddressOnlyGeneric<T>) { // expected-error {{'x' has guaranteed ownership but was consumed}}
    var x2 = x // expected-error {{'x2' consumed by a use in a loop}}
               // expected-error @-1 {{'x2' consumed more than once}}
               // expected-note @-2 {{consuming use here}}
    x2 = AddressOnlyGeneric<T>()
    for _ in 0..<1024 {
      if boolValue {
          consumeVal(x2) // expected-note {{consuming use here}}
      } else {
          consumeVal(x2) // expected-note {{consuming use here}}
                           // expected-note @-1 {{consuming use here}}
      }
    }
}

public func addressOnlyGenericDiamondInLoopArg<T>(_ x2: inout AddressOnlyGeneric<T>) { // expected-error {{'x2' consumed but not reinitialized before end of function}}
                                                       // expected-error @-1 {{'x2' consumed but not reinitialized before end of function}}
    for _ in 0..<1024 {
      if boolValue {
          consumeVal(x2) // expected-note {{consuming use here}}
      } else {
          consumeVal(x2) // expected-note {{consuming use here}}
      }
    }
}

public func addressOnlyGenericDiamondInLoopArg2<T>(_ x2: inout AddressOnlyGeneric<T>) { // expected-error {{'x2' consumed by a use in a loop}}
                                                       // expected-error @-1 {{'x2' consumed more than once}}
    for _ in 0..<1024 {
      if boolValue {
          consumeVal(x2) // expected-note {{consuming use here}}
      } else {
          consumeVal(x2) // expected-note {{consuming use here}}
                           // expected-note @-1 {{consuming use here}}
      }
    }
    x2 = AddressOnlyGeneric<T>()
}

public func addressOnlyGenericDiamondInLoopArg3<T>(_ x2: consuming AddressOnlyGeneric<T>) {
    // expected-error @-1 {{'x2' consumed by a use in a loop}}
    // expected-error @-2 {{'x2' consumed more than once}}

    for _ in 0..<1024 {
      if boolValue {
          consumeVal(x2) // expected-note {{consuming use here}}
      } else {
          consumeVal(x2) // expected-note {{consuming use here}}
          // expected-note @-1 {{consuming use here}}
      }
    }
}

public func addressOnlyGenericDiamondInLoopArg4<T>(_ x2: consuming AddressOnlyGeneric<T>) { // expected-error {{'x2' consumed by a use in a loop}}
                                                       // expected-error @-1 {{'x2' consumed more than once}}
    for _ in 0..<1024 {
      if boolValue {
          consumeVal(x2) // expected-note {{consuming use here}}
      } else {
          consumeVal(x2) // expected-note {{consuming use here}}
                           // expected-note @-1 {{consuming use here}}
      }
    }
    x2 = AddressOnlyGeneric<T>()
}

public func addressOnlyGenericAssignToVar1<T>(_ x: borrowing AddressOnlyGeneric<T>) {
    // expected-error @-1 {{'x' has guaranteed ownership but was consumed}}
    // expected-error @-2 {{'x' has guaranteed ownership but was consumed}}
    var x2 = x // expected-error {{'x2' consumed more than once}}
               // expected-note @-1 {{consuming use here}}
    x2 = AddressOnlyGeneric<T>()
    var x3 = x2 // expected-note {{consuming use here}}
    x3 = x2 // expected-note {{consuming use here}}
    x3 = x // expected-note {{consuming use here}}
    consumeVal(x3)
}

public func addressOnlyGenericAssignToVar1Arg<T>(_ x2: inout AddressOnlyGeneric<T>) { // expected-error {{'x2' consumed more than once}}
                                                      // expected-error @-1 {{'x2' consumed but not reinitialized before end of function}}
    var x3 = x2 // expected-note {{consuming use here}}
    x3 = x2 // expected-note {{consuming use here}}
            // expected-note @-1 {{consuming use here}}
    x3 = AddressOnlyGeneric<T>()
    consumeVal(x3)
}

public func addressOnlyGenericAssignToVar1Arg2<T>(_ x2: consuming AddressOnlyGeneric<T>) { // expected-error {{'x2' consumed more than once}}
    var x3 = x2 // expected-note {{consuming use here}}
    x3 = x2 // expected-note {{consuming use here}}
    x3 = AddressOnlyGeneric<T>()
    consumeVal(x3)
}

public func addressOnlyGenericAssignToVar2<T>(_ x: borrowing AddressOnlyGeneric<T>) { // expected-error {{'x' has guaranteed ownership but was consumed}}
    var x2 = x // expected-error {{'x2' consumed more than once}}
               // expected-note @-1 {{consuming use here}}
    x2 = AddressOnlyGeneric<T>()
    var x3 = x2 // expected-note {{consuming use here}}
    x3 = x2 // expected-note {{consuming use here}}
    borrowVal(x3)
}

public func addressOnlyGenericAssignToVar2Arg<T>(_ x2: inout AddressOnlyGeneric<T>) { // expected-error {{'x2' consumed more than once}}
                                                      // expected-error @-1 {{'x2' consumed but not reinitialized before end of function}}
    var x3 = x2 // expected-note {{consuming use here}}
    x3 = x2 // expected-note {{consuming use here}}
            // expected-note @-1 {{consuming use here}}
    borrowVal(x3)
}

public func addressOnlyGenericAssignToVar2Arg2<T>(_ x2: consuming AddressOnlyGeneric<T>) { // expected-error {{'x2' consumed more than once}}
    var x3 = x2 // expected-note {{consuming use here}}
    x3 = x2 // expected-note {{consuming use here}}
    borrowVal(x3)
}

public func addressOnlyGenericAssignToVar3<T>(_ x: borrowing AddressOnlyGeneric<T>) {
    // expected-error @-1 {{'x' has guaranteed ownership but was consumed}}
    // expected-error @-2 {{'x' has guaranteed ownership but was consumed}}
    var x2 = x // expected-note {{consuming use here}}
    x2 = AddressOnlyGeneric<T>()
    var x3 = x2
    x3 = x // expected-note {{consuming use here}}
    consumeVal(x3)
}

public func addressOnlyGenericAssignToVar3Arg<T>(_ x: borrowing AddressOnlyGeneric<T>, _ x2: inout AddressOnlyGeneric<T>) { // expected-error {{'x2' consumed but not reinitialized before end of function}}
                                                            // expected-error @-1 {{'x' has guaranteed ownership but was consumed}}
    var x3 = x2 // expected-note {{consuming use here}}
    x3 = x // expected-note {{consuming use here}}
    consumeVal(x3)
}

public func addressOnlyGenericAssignToVar3Arg2<T>(_ x: borrowing AddressOnlyGeneric<T>, _ x2: inout AddressOnlyGeneric<T>) { // expected-error {{'x2' consumed but not reinitialized before end of function}}
                                                                   // expected-error @-1 {{'x' has guaranteed ownership but was consumed}}
    var x3 = x2 // expected-note {{consuming use here}}
    x3 = x // expected-note {{consuming use here}}
    consumeVal(x3)
}

public func addressOnlyGenericAssignToVar4<T>(_ x: borrowing AddressOnlyGeneric<T>) { // expected-error {{'x' has guaranteed ownership but was consumed}}
    var x2 = x // expected-error {{'x2' consumed more than once}}
               // expected-note @-1 {{consuming use here}}
    x2 = AddressOnlyGeneric<T>()
    let x3 = x2 // expected-note {{consuming use here}}
    consumeVal(x2) // expected-note {{consuming use here}}
    consumeVal(x3)
}

public func addressOnlyGenericAssignToVar4Arg<T>(_ x2: inout AddressOnlyGeneric<T>) { // expected-error {{'x2' consumed more than once}}
                                                      // expected-error @-1 {{'x2' consumed but not reinitialized before end of function}}
    let x3 = x2 // expected-note {{consuming use here}}
    consumeVal(x2)   // expected-note {{consuming use here}}
                // expected-note @-1 {{consuming use here}}
    consumeVal(x3)
}

public func addressOnlyGenericAssignToVar4Arg2<T>(_ x2: consuming AddressOnlyGeneric<T>) { // expected-error {{'x2' consumed more than once}}
    let x3 = x2 // expected-note {{consuming use here}}
    consumeVal(x2)   // expected-note {{consuming use here}}
    consumeVal(x3)
}

public func addressOnlyGenericAssignToVar5<T : P>(_ ty: T.Type) {
    var x2 = AddressOnlyGeneric<T>() // expected-error {{'x2' used after consume}}
    x2 = AddressOnlyGeneric<T>()
    var x3 = x2 // expected-note {{consuming use here}}
    borrowVal(x2) // expected-note {{non-consuming use here}}
    x3 = AddressOnlyGeneric<T>()
    consumeVal(x3)
}

public func addressOnlyGenericAssignToVar5Arg<T>(_ x: borrowing AddressOnlyGeneric<T>, _ x2: inout AddressOnlyGeneric<T>) {
    // expected-error @-1 {{'x2' used after consume}}
    // expected-error @-2 {{'x' has guaranteed ownership but was consumed}}
    var x3 = x2 // expected-note {{consuming use here}}
    borrowVal(x2) // expected-note {{non-consuming use here}}
    x3 = x // expected-note {{consuming use here}}
    consumeVal(x3)
}

public func addressOnlyGenericAssignToVar5Arg2<T>(_ x: borrowing AddressOnlyGeneric<T>, _ x2: inout AddressOnlyGeneric<T>) { // expected-error {{'x' has guaranteed ownership but was consumed}}
                                                                   // expected-error @-1 {{'x2' used after consume}}
    var x3 = x2 // expected-note {{consuming use here}}
    borrowVal(x2) // expected-note {{non-consuming use here}}
    x3 = x // expected-note {{consuming use here}}
    consumeVal(x3)
    x2 = AddressOnlyGeneric<T>()
}

// MG: We are calling these consuming uses since I have not taught the checker
// that a use of a copy_addr that is copyable is not a consuming use. I will
// remove them when I fix it in the next commit.
public func addressOnlyGenericAccessAccessField<T>(_ x: borrowing AddressOnlyGeneric<T>) { // expected-error {{'x' has guaranteed ownership but was consumed}}
    var x2 = x // expected-note {{consuming use here}}
    x2 = AddressOnlyGeneric<T>()
    borrowVal(x2.copyable)
    for _ in 0..<1024 {
        borrowVal(x2.copyable)
    }
}

public func addressOnlyGenericAccessAccessField2<T>(_ x: borrowing AddressOnlyGeneric<T>) { // expected-error {{'x' has guaranteed ownership but was consumed}}
    var x2 = x // expected-note {{consuming use here}}
    x2 = AddressOnlyGeneric<T>()
    borrowVal(x2.moveOnly)
    for _ in 0..<1024 {
        borrowVal(x2.moveOnly)
    }
}

public func addressOnlyGenericAccessAccessFieldArg<T>(_ x2: inout AddressOnlyGeneric<T>) {
    borrowVal(x2.copyable)
    for _ in 0..<1024 {
        borrowVal(x2.copyable)
    }
}

public func addressOnlyGenericAccessAccessFieldArg2<T>(_ x2: inout AddressOnlyGeneric<T>) {
    borrowVal(x2.moveOnly)
    for _ in 0..<1024 {
        borrowVal(x2.moveOnly)
    }
}

public func addressOnlyGenericAccessAccessFieldArg3<T>(_ x2: consuming AddressOnlyGeneric<T>) {
    borrowVal(x2.copyable)
    for _ in 0..<1024 {
        borrowVal(x2.copyable)
    }
}

public func addressOnlyGenericAccessAccessFieldArg4<T>(_ x2: consuming AddressOnlyGeneric<T>) {
    borrowVal(x2.moveOnly)
    for _ in 0..<1024 {
        borrowVal(x2.moveOnly)
    }
}

public func addressOnlyGenericAccessConsumeField<T>(_ x: borrowing AddressOnlyGeneric<T>) { // expected-error {{'x' has guaranteed ownership but was consumed}}
    var x2 = x // expected-note {{consuming use here}}
    x2 = AddressOnlyGeneric<T>()

    consumeVal(x2.copyable)
    for _ in 0..<1024 {
        consumeVal(x2.copyable)
    }
}

public func addressOnlyGenericAccessConsumeField2<T>(_ x: borrowing AddressOnlyGeneric<T>) { // expected-error {{'x' has guaranteed ownership but was consumed}}
    var x2 = x // expected-note {{consuming use here}}
    // expected-error @-1 {{'x2' consumed by a use in a loop}}
    // expected-error @-2 {{'x2' consumed more than once}}
    x2 = AddressOnlyGeneric<T>()

    consumeVal(x2.moveOnly) // expected-note {{consuming use here}}
    for _ in 0..<1024 {
        consumeVal(x2.moveOnly) // expected-note {{consuming use here}}
        // expected-note @-1 {{consuming use here}}
    }
}

public func addressOnlyGenericAccessConsumeGrandField<T>(_ x: borrowing AddressOnlyGeneric<T>) { // expected-error {{'x' has guaranteed ownership but was consumed}}
    var x2 = x // expected-note {{consuming use here}}
    x2 = AddressOnlyGeneric<T>()

    consumeVal(x2.copyable.name)
    for _ in 0..<1024 {
        consumeVal(x2.copyable.name)
    }
}

public func addressOnlyGenericAccessConsumeGrandField2<T>(_ x: borrowing AddressOnlyGeneric<T>) { // expected-error {{'x' has guaranteed ownership but was consumed}}
    var x2 = x // expected-note {{consuming use here}}
    // expected-error @-1 {{'x2' consumed more than once}}
    // expected-error @-2 {{'x2' consumed by a use in a loop}}
    x2 = AddressOnlyGeneric<T>()

    consumeVal(x2.moveOnly.k) // expected-note {{consuming use here}}
    for _ in 0..<1024 {
        consumeVal(x2.moveOnly.k) // expected-note {{consuming use here}}
        // expected-note @-1 {{consuming use here}}
    }
}

public func addressOnlyGenericAccessConsumeGrandField2a<T>(_ x: borrowing AddressOnlyGeneric<T>) { // expected-error {{'x' has guaranteed ownership but was consumed}}
    var x2 = x // expected-note {{consuming use here}}
    x2 = AddressOnlyGeneric<T>()

    consumeVal(x2.moveOnly.copyableK)
    for _ in 0..<1024 {
        consumeVal(x2.moveOnly.copyableK)
    }
}

public func addressOnlyGenericAccessConsumeFieldArg<T>(_ x2: inout AddressOnlyGeneric<T>) {
    consumeVal(x2.copyable)
    for _ in 0..<1024 {
        consumeVal(x2.copyable)
    }
}

public func addressOnlyGenericAccessConsumeFieldArg2<T>(_ x2: inout AddressOnlyGeneric<T>) {
    // expected-error @-1 {{'x2' consumed but not reinitialized before end of function}}
    // expected-error @-2 {{'x2' consumed but not reinitialized before end of function}}
    consumeVal(x2.moveOnly) // expected-note {{consuming use here}}

    for _ in 0..<1024 {
        consumeVal(x2.moveOnly) // expected-note {{consuming use here}}
    }
}

public func addressOnlyGenericAccessConsumeFieldArg3<T>(_ x2: consuming AddressOnlyGeneric<T>) {
    consumeVal(x2.copyable)

    for _ in 0..<1024 {
        consumeVal(x2.copyable)
    }
}

public func addressOnlyGenericAccessConsumeFieldArg4<T>(_ x2: consuming AddressOnlyGeneric<T>) {
    // expected-error @-1 {{'x2' consumed by a use in a loop}}
    // expected-error @-2 {{'x2' consumed more than once}}
    consumeVal(x2.moveOnly) // expected-note {{consuming use here}}

    for _ in 0..<1024 {
        consumeVal(x2.moveOnly) // expected-note {{consuming use here}}
        // expected-note @-1 {{consuming use here}}
    }
}

public func addressOnlyGenericAccessConsumeGrandFieldArg<T>(_ x2: inout AddressOnlyGeneric<T>) {
    consumeVal(x2.copyable.name)
    for _ in 0..<1024 {
        consumeVal(x2.copyable.name)
    }
}

public func addressOnlyGenericAccessConsumeGrandFieldArg2<T>(_ x2: inout AddressOnlyGeneric<T>) {
    // expected-error @-1 {{'x2' consumed but not reinitialized before end of function}}
    // expected-error @-2 {{'x2' consumed but not reinitialized before end of function}}
    consumeVal(x2.moveOnly.k) // expected-note {{consuming use here}}

    for _ in 0..<1024 {
        consumeVal(x2.moveOnly.k) // expected-note {{consuming use here}}
    }
}

public func addressOnlyGenericAccessConsumeGrandFieldArg2a<T>(_ x2: inout AddressOnlyGeneric<T>) {
    consumeVal(x2.moveOnly.copyableK)

    for _ in 0..<1024 {
        consumeVal(x2.moveOnly.copyableK)
    }
}

public func addressOnlyGenericAccessConsumeGrandFieldArg3<T>(_ x2: consuming AddressOnlyGeneric<T>) {
    consumeVal(x2.copyable.name)

    for _ in 0..<1024 {
        consumeVal(x2.copyable.name)
    }
}

public func addressOnlyGenericAccessConsumeGrandFieldArg4<T>(_ x2: consuming AddressOnlyGeneric<T>) {
    // expected-error @-1 {{'x2' consumed by a use in a loop}}
    // expected-error @-2 {{'x2' consumed more than once}}
    consumeVal(x2.moveOnly.k) // expected-note {{consuming use here}}

    for _ in 0..<1024 {
        consumeVal(x2.moveOnly.k) // expected-note {{consuming use here}}
        // expected-note @-1 {{consuming use here}}
    }
}

public func addressOnlyGenericAccessConsumeGrandFieldArg4a<T>(_ x2: consuming AddressOnlyGeneric<T>) {
    consumeVal(x2.moveOnly.copyableK)

    for _ in 0..<1024 {
        consumeVal(x2.moveOnly.copyableK)
    }
}

extension AddressOnlyGeneric {
    func testNoUseSelf() { // expected-error {{'self' has guaranteed ownership but was consumed}}
        let x = self // expected-note {{consuming use here}}
        let _ = x
    }

    mutating func testNoUseSelf2() { // expected-error {{'self' consumed but not reinitialized before end of function}}
        let x = self // expected-note {{consuming use here}}
        let _ = x
    }
}

@_moveOnly
struct AddressOnlyGenericInit<T : P> {
    var copyable: T
    var moveOnly: NonTrivialStruct
    var moveOnly2: AddressOnlyGeneric<T>

    init(_ input1: T, _ input2: consuming NonTrivialStruct) {
        if boolValue {
            copyable = input1
        } else {
            copyable = T.value
        }
        moveOnly2 = AddressOnlyGeneric<T>(T.value)
        moveOnly = input2
    }

    init(_ input1: T, _ input2: consuming NonTrivialStruct, _ input3 : consuming AddressOnlyGeneric<T>) {
        copyable = input1
        if boolValue {
            moveOnly2 = input3
        } else {
            moveOnly2 = AddressOnlyGeneric<T>(T.value)
        }
        moveOnly = input2
    }
}

///////////////////////////
// Address Only Protocol //
///////////////////////////

public func addressOnlyProtocolSimpleChainTest(_ x: borrowing AddressOnlyProtocol) { // expected-error {{'x' has guaranteed ownership but was consumed}}
    // expected-error @-1 {{'x' has guaranteed ownership but was consumed}}
    var x2 = x // expected-note {{consuming use here}}
               // expected-error @-1 {{'x2' consumed more than once}}
    x2 = x // expected-note {{consuming use here}}
    let y2 = x2 // expected-note {{consuming use here}}
    let k2 = y2
    let k3 = x2 // expected-note {{consuming use here}}
    let _ = k3
    borrowVal(k2)
}

public func addressOnlyProtocolSimpleChainArgTest(_ x2: inout AddressOnlyProtocol) {
    // expected-error @-1 {{'x2' consumed more than once}}
    // expected-error @-2 {{'x2' consumed but not reinitialized before end of function}}
    var y2 = x2 // expected-note {{consuming use here}}
    y2 = x2 // expected-note {{consuming use here}}
    // expected-note @-1 {{consuming use here}}
    let k2 = y2
    borrowVal(k2)
}

public func addressOnlyProtocolSimpleChainConsumingArgTest(_ x2: consuming AddressOnlyProtocol) {
    // expected-error @-1 {{'x2' consumed more than once}}
    var y2 = x2 // expected-note {{consuming use here}}
    y2 = x2 // expected-note {{consuming use here}}
    let k2 = y2
    borrowVal(k2)
}

public func addressOnlyProtocolSimpleNonConsumingUseTest(_ x: borrowing AddressOnlyProtocol) {
    // expected-error @-1 {{'x' has guaranteed ownership but was consumed}}
    // expected-error @-2 {{'x' has guaranteed ownership but was consumed}}
    var x2 = x // expected-note {{consuming use here}}
    x2 = x // expected-note {{consuming use here}}
    borrowVal(x2)
}

public func addressOnlyProtocolSimpleNonConsumingUseArgTest(_ x2: inout AddressOnlyProtocol) {
    borrowVal(x2)
}

public func addressOnlyProtocolMultipleNonConsumingUseTest(_ x: borrowing AddressOnlyProtocol) {
    // expected-error @-1 {{'x' has guaranteed ownership but was consumed}}
    // expected-error @-2 {{'x' has guaranteed ownership but was consumed}}
    var x2 = x // expected-note {{consuming use here}}
    x2 = x // expected-note {{consuming use here}}
    borrowVal(x2)
    borrowVal(x2)
    consumeVal(x2)
}

public func addressOnlyProtocolMultipleNonConsumingUseArgTest(_ x2: inout AddressOnlyProtocol) { // expected-error {{'x2' consumed but not reinitialized before end of function}}
    borrowVal(x2)
    borrowVal(x2)
    consumeVal(x2) // expected-note {{consuming use here}}
}

public func addressOnlyProtocolMultipleNonConsumingUseArgTest2(_ x2: inout AddressOnlyProtocol) { // expected-error {{'x2' used after consume}}
    borrowVal(x2)
    borrowVal(x2)
    consumeVal(x2) // expected-note {{consuming use here}}
    borrowVal(x2) // expected-note {{non-consuming use here}}
}

public func addressOnlyProtocolMultipleNonConsumingUseArgTest3(_ x2: inout AddressOnlyProtocol) {  // expected-error {{'x2' consumed but not reinitialized before end of function}}
                                                                       // expected-error @-1 {{'x2' consumed more than once}}
    borrowVal(x2)
    borrowVal(x2)
    consumeVal(x2) // expected-note {{consuming use here}}
    consumeVal(x2) // expected-note {{consuming use here}}
                   // expected-note @-1 {{consuming use here}}
}

public func addressOnlyProtocolMultipleNonConsumingUseArgTest4(_ x2: inout AddressOnlyProtocol) { // expected-error {{'x2' used after consume}}
    borrowVal(x2)
    borrowVal(x2)
    consumeVal(x2) // expected-note {{consuming use here}}
    borrowVal(x2) // expected-note {{non-consuming use here}}
    x2 = AddressOnlyProtocol()
}

public func addressOnlyProtocolMultipleNonConsumingUseConsumingArgTest(_ x2: consuming AddressOnlyProtocol) {
    borrowVal(x2)
    borrowVal(x2)
    consumeVal(x2)
}

public func addressOnlyProtocolMultipleNonConsumingUseConsumingArgTest2(_ x2: consuming AddressOnlyProtocol) { // expected-error {{'x2' used after consume}}
    borrowVal(x2)
    borrowVal(x2)
    consumeVal(x2) // expected-note {{consuming use here}}
    borrowVal(x2) // expected-note {{non-consuming use here}}
}

public func addressOnlyProtocolMultipleNonConsumingUseConsumingArgTest3(_ x2: consuming AddressOnlyProtocol) {
    // expected-error @-1 {{'x2' consumed more than once}}
    borrowVal(x2)
    borrowVal(x2)
    consumeVal(x2) // expected-note {{consuming use here}}
    consumeVal(x2) // expected-note {{consuming use here}}
}

public func addressOnlyProtocolMultipleNonConsumingUseConsumingArgTest4(_ x2: consuming AddressOnlyProtocol) { // expected-error {{'x2' used after consume}}
    borrowVal(x2)
    borrowVal(x2)
    consumeVal(x2) // expected-note {{consuming use here}}
    borrowVal(x2) // expected-note {{non-consuming use here}}
    x2 = AddressOnlyProtocol()
}

public func addressOnlyProtocolUseAfterConsume(_ x: borrowing AddressOnlyProtocol) {
    // expected-error @-1 {{'x' has guaranteed ownership but was consumed}}
    // expected-error @-2 {{'x' has guaranteed ownership but was consumed}}
    var x2 = x // expected-error {{'x2' consumed more than once}}
               // expected-note @-1 {{consuming use here}}
    x2 = x // expected-note {{consuming use here}}
    borrowVal(x2)
    consumeVal(x2) // expected-note {{consuming use here}}
    consumeVal(x2) // expected-note {{consuming use here}}
}

public func addressOnlyProtocolUseAfterConsumeArg(_ x2: inout AddressOnlyProtocol) { // expected-error {{'x2' consumed but not reinitialized before end of function}}
                                                         // expected-error @-1 {{'x2' consumed more than once}}
    borrowVal(x2)
    consumeVal(x2) // expected-note {{consuming use here}}
    consumeVal(x2) // expected-note {{consuming use here}}
                   // expected-note @-1 {{consuming use here}}
}

public func addressOnlyProtocolUseAfterConsumeArg2(_ x2: consuming AddressOnlyProtocol) {
    // expected-error @-1 {{'x2' consumed more than once}}
    borrowVal(x2)
    consumeVal(x2) // expected-note {{consuming use here}}
    consumeVal(x2) // expected-note {{consuming use here}}
}

public func addressOnlyProtocolDoubleConsume(_ x: borrowing AddressOnlyProtocol) { // expected-error {{'x' has guaranteed ownership but was consumed}}
    var x2 = x  // expected-error {{'x2' consumed more than once}}
                // expected-note @-1 {{consuming use here}}
    x2 = AddressOnlyProtocol()
    consumeVal(x2) // expected-note {{consuming use here}}
    consumeVal(x2) // expected-note {{consuming use here}}
}

public func addressOnlyProtocolDoubleConsumeArg(_ x2: inout AddressOnlyProtocol) { // expected-error {{'x2' consumed but not reinitialized before end of function}}
                                                       // expected-error @-1 {{'x2' consumed more than once}}
    consumeVal(x2) // expected-note {{consuming use here}}
    consumeVal(x2) // expected-note {{consuming use here}}
                     // expected-note @-1 {{consuming use here}}
}

public func addressOnlyProtocolDoubleConsumeArg2(_ x2: consuming AddressOnlyProtocol) {
                                                       // expected-error @-1 {{'x2' consumed more than once}}
    consumeVal(x2) // expected-note {{consuming use here}}
    consumeVal(x2) // expected-note {{consuming use here}}
}

public func addressOnlyProtocolLoopConsume(_ x: borrowing AddressOnlyProtocol) { // expected-error {{'x' has guaranteed ownership but was consumed}}
    var x2 = x // expected-error {{'x2' consumed by a use in a loop}}
               // expected-note @-1 {{consuming use here}}
    x2 = AddressOnlyProtocol()
    for _ in 0..<1024 {
        consumeVal(x2) // expected-note {{consuming use here}}
    }
}

public func addressOnlyProtocolLoopConsumeArg(_ x2: inout AddressOnlyProtocol) { // expected-error {{'x2' consumed but not reinitialized before end of function}}
    for _ in 0..<1024 {
        consumeVal(x2) // expected-note {{consuming use here}}
    }
}

public func addressOnlyProtocolLoopConsumeArg2(_ x2: inout AddressOnlyProtocol) { // expected-error {{'x2' consumed by a use in a loop}}
    for _ in 0..<1024 {
        consumeVal(x2) // expected-note {{consuming use here}}
    }
    x2 = AddressOnlyProtocol()
}

public func addressOnlyProtocolLoopConsumeArg3(_ x2: consuming AddressOnlyProtocol) { // expected-error {{'x2' consumed by a use in a loop}}
    for _ in 0..<1024 {
        consumeVal(x2) // expected-note {{consuming use here}}
    }
    x2 = AddressOnlyProtocol()
}

public func addressOnlyProtocolDiamond(_ x: borrowing AddressOnlyProtocol) { // expected-error {{'x' has guaranteed ownership but was consumed}}
    var x2 = x // expected-note {{consuming use here}}
    x2 = AddressOnlyProtocol()
    if boolValue {
        consumeVal(x2)
    } else {
        consumeVal(x2)
    }
}

public func addressOnlyProtocolDiamondArg(_ x2: inout AddressOnlyProtocol) { // expected-error {{'x2' consumed but not reinitialized before end of function}}
                                                 // expected-error @-1 {{'x2' consumed but not reinitialized before end of function}}
    if boolValue {
        consumeVal(x2) // expected-note {{consuming use here}}
    } else {
        consumeVal(x2) // expected-note {{consuming use here}}
    }
}

public func addressOnlyProtocolDiamondArg2(_ x2: consuming AddressOnlyProtocol) {
    if boolValue {
        consumeVal(x2)
    } else {
        consumeVal(x2)
    }
}

public func addressOnlyProtocolDiamondInLoop(_ x: borrowing AddressOnlyProtocol) { // expected-error {{'x' has guaranteed ownership but was consumed}}
    var x2 = x // expected-error {{'x2' consumed by a use in a loop}}
               // expected-error @-1 {{'x2' consumed more than once}}
               // expected-note @-2 {{consuming use here}}
    x2 = AddressOnlyProtocol()
    for _ in 0..<1024 {
      if boolValue {
          consumeVal(x2) // expected-note {{consuming use here}}
      } else {
          consumeVal(x2) // expected-note {{consuming use here}}
                           // expected-note @-1 {{consuming use here}}
      }
    }
}

public func addressOnlyProtocolDiamondInLoopArg(_ x2: inout AddressOnlyProtocol) { // expected-error {{'x2' consumed but not reinitialized before end of function}}
                                                       // expected-error @-1 {{'x2' consumed but not reinitialized before end of function}}
    for _ in 0..<1024 {
      if boolValue {
          consumeVal(x2) // expected-note {{consuming use here}}
      } else {
          consumeVal(x2) // expected-note {{consuming use here}}
      }
    }
}

public func addressOnlyProtocolDiamondInLoopArg2(_ x2: inout AddressOnlyProtocol) { // expected-error {{'x2' consumed by a use in a loop}}
                                                       // expected-error @-1 {{'x2' consumed more than once}}
    for _ in 0..<1024 {
      if boolValue {
          consumeVal(x2) // expected-note {{consuming use here}}
      } else {
          consumeVal(x2) // expected-note {{consuming use here}}
                           // expected-note @-1 {{consuming use here}}
      }
    }
    x2 = AddressOnlyProtocol()
}

public func addressOnlyProtocolDiamondInLoopArg3(_ x2: consuming AddressOnlyProtocol) {
    // expected-error @-1 {{'x2' consumed by a use in a loop}}
    // expected-error @-2 {{'x2' consumed more than once}}

    for _ in 0..<1024 {
      if boolValue {
          consumeVal(x2) // expected-note {{consuming use here}}
      } else {
          consumeVal(x2) // expected-note {{consuming use here}}
          // expected-note @-1 {{consuming use here}}
      }
    }
}

public func addressOnlyProtocolDiamondInLoopArg4(_ x2: consuming AddressOnlyProtocol) { // expected-error {{'x2' consumed by a use in a loop}}
                                                       // expected-error @-1 {{'x2' consumed more than once}}
    for _ in 0..<1024 {
      if boolValue {
          consumeVal(x2) // expected-note {{consuming use here}}
      } else {
          consumeVal(x2) // expected-note {{consuming use here}}
                           // expected-note @-1 {{consuming use here}}
      }
    }
    x2 = AddressOnlyProtocol()
}

public func addressOnlyProtocolAssignToVar1(_ x: borrowing AddressOnlyProtocol) {
    // expected-error @-1 {{'x' has guaranteed ownership but was consumed}}
    // expected-error @-2 {{'x' has guaranteed ownership but was consumed}}
    var x2 = x // expected-error {{'x2' consumed more than once}}
               // expected-note @-1 {{consuming use here}}
    x2 = AddressOnlyProtocol()
    var x3 = x2 // expected-note {{consuming use here}}
    x3 = x2 // expected-note {{consuming use here}}
    x3 = x // expected-note {{consuming use here}}
    consumeVal(x3)
}

public func addressOnlyProtocolAssignToVar1Arg(_ x2: inout AddressOnlyProtocol) { // expected-error {{'x2' consumed more than once}}
                                                      // expected-error @-1 {{'x2' consumed but not reinitialized before end of function}}
    var x3 = x2 // expected-note {{consuming use here}}
    x3 = x2 // expected-note {{consuming use here}}
            // expected-note @-1 {{consuming use here}}
    x3 = AddressOnlyProtocol()
    consumeVal(x3)
}

public func addressOnlyProtocolAssignToVar1Arg2(_ x2: consuming AddressOnlyProtocol) { // expected-error {{'x2' consumed more than once}}
    var x3 = x2 // expected-note {{consuming use here}}
    x3 = x2 // expected-note {{consuming use here}}
    x3 = AddressOnlyProtocol()
    consumeVal(x3)
}

public func addressOnlyProtocolAssignToVar2(_ x: borrowing AddressOnlyProtocol) { // expected-error {{'x' has guaranteed ownership but was consumed}}
    var x2 = x // expected-error {{'x2' consumed more than once}}
               // expected-note @-1 {{consuming use here}}
    x2 = AddressOnlyProtocol()
    var x3 = x2 // expected-note {{consuming use here}}
    x3 = x2 // expected-note {{consuming use here}}
    borrowVal(x3)
}

public func addressOnlyProtocolAssignToVar2Arg(_ x2: inout AddressOnlyProtocol) { // expected-error {{'x2' consumed more than once}}
                                                      // expected-error @-1 {{'x2' consumed but not reinitialized before end of function}}
    var x3 = x2 // expected-note {{consuming use here}}
    x3 = x2 // expected-note {{consuming use here}}
            // expected-note @-1 {{consuming use here}}
    borrowVal(x3)
}

public func addressOnlyProtocolAssignToVar2Arg2(_ x2: consuming AddressOnlyProtocol) { // expected-error {{'x2' consumed more than once}}
    var x3 = x2 // expected-note {{consuming use here}}
    x3 = x2 // expected-note {{consuming use here}}
    borrowVal(x3)
}

public func addressOnlyProtocolAssignToVar3(_ x: borrowing AddressOnlyProtocol) {
    // expected-error @-1 {{'x' has guaranteed ownership but was consumed}}
    // expected-error @-2 {{'x' has guaranteed ownership but was consumed}}
    var x2 = x // expected-note {{consuming use here}}
    x2 = AddressOnlyProtocol()
    var x3 = x2
    x3 = x // expected-note {{consuming use here}}
    consumeVal(x3)
}

public func addressOnlyProtocolAssignToVar3Arg(_ x: borrowing AddressOnlyProtocol, _ x2: inout AddressOnlyProtocol) { // expected-error {{'x2' consumed but not reinitialized before end of function}}
                                                            // expected-error @-1 {{'x' has guaranteed ownership but was consumed}}
    var x3 = x2 // expected-note {{consuming use here}}
    x3 = x // expected-note {{consuming use here}}
    consumeVal(x3)
}

public func addressOnlyProtocolAssignToVar3Arg2(_ x: borrowing AddressOnlyProtocol, _ x2: inout AddressOnlyProtocol) { // expected-error {{'x2' consumed but not reinitialized before end of function}}
                                                                   // expected-error @-1 {{'x' has guaranteed ownership but was consumed}}
    var x3 = x2 // expected-note {{consuming use here}}
    x3 = x // expected-note {{consuming use here}}
    consumeVal(x3)
}

public func addressOnlyProtocolAssignToVar4(_ x: borrowing AddressOnlyProtocol) { // expected-error {{'x' has guaranteed ownership but was consumed}}
    var x2 = x // expected-error {{'x2' consumed more than once}}
               // expected-note @-1 {{consuming use here}}
    x2 = AddressOnlyProtocol()
    let x3 = x2 // expected-note {{consuming use here}}
    consumeVal(x2) // expected-note {{consuming use here}}
    consumeVal(x3)
}

public func addressOnlyProtocolAssignToVar4Arg(_ x2: inout AddressOnlyProtocol) { // expected-error {{'x2' consumed more than once}}
                                                      // expected-error @-1 {{'x2' consumed but not reinitialized before end of function}}
    let x3 = x2 // expected-note {{consuming use here}}
    consumeVal(x2)   // expected-note {{consuming use here}}
                // expected-note @-1 {{consuming use here}}
    consumeVal(x3)
}

public func addressOnlyProtocolAssignToVar4Arg2(_ x2: consuming AddressOnlyProtocol) { // expected-error {{'x2' consumed more than once}}
    let x3 = x2 // expected-note {{consuming use here}}
    consumeVal(x2)   // expected-note {{consuming use here}}
    consumeVal(x3)
}

public func addressOnlyProtocolAssignToVar5<T : P>(_ ty: T.Type) {
    var x2 = AddressOnlyProtocol() // expected-error {{'x2' used after consume}}
    x2 = AddressOnlyProtocol()
    var x3 = x2 // expected-note {{consuming use here}}
    borrowVal(x2) // expected-note {{non-consuming use here}}
    x3 = AddressOnlyProtocol()
    consumeVal(x3)
}

public func addressOnlyProtocolAssignToVar5Arg(_ x: borrowing AddressOnlyProtocol, _ x2: inout AddressOnlyProtocol) {
    // expected-error @-1 {{'x2' used after consume}}
    // expected-error @-2 {{'x' has guaranteed ownership but was consumed}}
    var x3 = x2 // expected-note {{consuming use here}}
    borrowVal(x2) // expected-note {{non-consuming use here}}
    x3 = x // expected-note {{consuming use here}}
    consumeVal(x3)
}

public func addressOnlyProtocolAssignToVar5Arg2(_ x: borrowing AddressOnlyProtocol, _ x2: inout AddressOnlyProtocol) { // expected-error {{'x' has guaranteed ownership but was consumed}}
                                                                   // expected-error @-1 {{'x2' used after consume}}
    var x3 = x2 // expected-note {{consuming use here}}
    borrowVal(x2) // expected-note {{non-consuming use here}}
    x3 = x // expected-note {{consuming use here}}
    consumeVal(x3)
    x2 = AddressOnlyProtocol()
}

// MG: We are calling these consuming uses since I have not taught the checker
// that a use of a copy_addr that is copyable is not a consuming use. I will
// remove them when I fix it in the next commit.
public func addressOnlyProtocolAccessAccessField(_ x: borrowing AddressOnlyProtocol) { // expected-error {{'x' has guaranteed ownership but was consumed}}
    var x2 = x // expected-note {{consuming use here}}
    x2 = AddressOnlyProtocol()
    borrowVal(x2.copyable)
    for _ in 0..<1024 {
        borrowVal(x2.copyable)
    }
}

public func addressOnlyProtocolAccessAccessField2(_ x: borrowing AddressOnlyProtocol) { // expected-error {{'x' has guaranteed ownership but was consumed}}
    var x2 = x // expected-note {{consuming use here}}
    x2 = AddressOnlyProtocol()
    borrowVal(x2.moveOnly)
    for _ in 0..<1024 {
        borrowVal(x2.moveOnly)
    }
}

public func addressOnlyProtocolAccessAccessFieldArg(_ x2: inout AddressOnlyProtocol) {
    borrowVal(x2.copyable)
    for _ in 0..<1024 {
        borrowVal(x2.copyable)
    }
}

public func addressOnlyProtocolAccessAccessFieldArg2(_ x2: inout AddressOnlyProtocol) {
    borrowVal(x2.moveOnly)
    for _ in 0..<1024 {
        borrowVal(x2.moveOnly)
    }
}

public func addressOnlyProtocolAccessAccessFieldArg3(_ x2: consuming AddressOnlyProtocol) {
    borrowVal(x2.copyable)
    for _ in 0..<1024 {
        borrowVal(x2.copyable)
    }
}

public func addressOnlyProtocolAccessAccessFieldArg4(_ x2: consuming AddressOnlyProtocol) {
    borrowVal(x2.moveOnly)
    for _ in 0..<1024 {
        borrowVal(x2.moveOnly)
    }
}

public func addressOnlyProtocolAccessConsumeField(_ x: borrowing AddressOnlyProtocol) { // expected-error {{'x' has guaranteed ownership but was consumed}}
    var x2 = x // expected-note {{consuming use here}}
    x2 = AddressOnlyProtocol()

    consumeVal(x2.copyable)
    for _ in 0..<1024 {
        consumeVal(x2.copyable)
    }
}

public func addressOnlyProtocolAccessConsumeField2(_ x: borrowing AddressOnlyProtocol) { // expected-error {{'x' has guaranteed ownership but was consumed}}
    var x2 = x // expected-note {{consuming use here}}
    // expected-error @-1 {{'x2' consumed by a use in a loop}}
    // expected-error @-2 {{'x2' consumed more than once}}
    x2 = AddressOnlyProtocol()

    consumeVal(x2.moveOnly) // expected-note {{consuming use here}}
    for _ in 0..<1024 {
        consumeVal(x2.moveOnly) // expected-note {{consuming use here}}
        // expected-note @-1 {{consuming use here}}
    }
}

public func addressOnlyProtocolAccessConsumeFieldArg(_ x2: inout AddressOnlyProtocol) {
    consumeVal(x2.copyable)
    for _ in 0..<1024 {
        consumeVal(x2.copyable)
    }
}

public func addressOnlyProtocolAccessConsumeFieldArg2(_ x2: inout AddressOnlyProtocol) {
    // expected-error @-1 {{'x2' consumed but not reinitialized before end of function}}
    // expected-error @-2 {{'x2' consumed but not reinitialized before end of function}}
    consumeVal(x2.moveOnly) // expected-note {{consuming use here}}

    for _ in 0..<1024 {
        consumeVal(x2.moveOnly) // expected-note {{consuming use here}}
    }
}

public func addressOnlyProtocolAccessConsumeFieldArg3(_ x2: consuming AddressOnlyProtocol) {
    consumeVal(x2.copyable)

    for _ in 0..<1024 {
        consumeVal(x2.copyable)
    }
}

public func addressOnlyProtocolAccessConsumeFieldArg4(_ x2: consuming AddressOnlyProtocol) {
    // expected-error @-1 {{'x2' consumed by a use in a loop}}
    // expected-error @-2 {{'x2' consumed more than once}}
    consumeVal(x2.moveOnly) // expected-note {{consuming use here}}

    for _ in 0..<1024 {
        consumeVal(x2.moveOnly) // expected-note {{consuming use here}}
        // expected-note @-1 {{consuming use here}}
    }
}

extension AddressOnlyProtocol {
    func testNoUseSelf() { // expected-error {{'self' has guaranteed ownership but was consumed}}
        let x = self // expected-note {{consuming use here}}
        let _ = x
    }
}

/////////////////////////////
// MARK: Closure Let Tests //
/////////////////////////////

// These are considered to be non-escaping since we are not storing them into
// memory.

public func closureLetClassUseAfterConsume1() {
    let f = {
        var x2 = Klass() // expected-error {{'x2' consumed more than once}}
        x2 = Klass()
        borrowVal(x2)
        consumeVal(x2) // expected-note {{consuming use here}}
        consumeVal(x2) // expected-note {{consuming use here}}
    }
    f()
}

public func closureLetClassUseAfterConsume2() {
    let f = { () in
        var x2 = Klass() // expected-error {{'x2' consumed more than once}}
        x2 = Klass()
        borrowVal(x2)
        consumeVal(x2) // expected-note {{consuming use here}}
        consumeVal(x2) // expected-note {{consuming use here}}
    }
    f()
}

public func closureLetClassUseAfterConsumeArg(_ argX: inout Klass) {
    // TODO: Fix this
    let f = { (_ x2: inout Klass) in
        // expected-error @-1 {{'x2' consumed but not reinitialized before end of function}}
        // expected-error @-2 {{'x2' consumed more than once}}
        borrowVal(x2)
        consumeVal(x2) // expected-note {{consuming use here}}
        consumeVal(x2) // expected-note {{consuming use here}}
        // expected-note @-1 {{consuming use here}}
    }
    f(&argX)
}

// We do not support captures of vars by closures today.
public func closureLetCaptureClassUseAfterConsume() {
    var x2 = Klass() // expected-error {{'x2' consumed in closure but not reinitialized before end of closure}}
    // expected-error @-1 {{'x2' consumed more than once}}
    x2 = Klass()
    let f = {
        borrowVal(x2)
        consumeVal(x2) // expected-note {{consuming use here}}
        consumeVal(x2) // expected-note {{consuming use here}}
        // expected-note @-1 {{consuming use here}}
    }
    f()
}

public func closureLetCaptureClassUseAfterConsume2() {
    var x2 = Klass() // expected-error {{'x2' consumed in closure but not reinitialized before end of closure}}
    x2 = Klass()
    let f = {
        borrowVal(x2)
        consumeVal(x2) // expected-note {{consuming use here}}
    }
    f()
}

public func closureLetCaptureClassUseAfterConsumeError() {
    var x2 = Klass() // expected-error {{'x2' consumed in closure but not reinitialized before end of closure}}
    // expected-error @-1 {{'x2' consumed more than once}}
    // expected-error @-2 {{'x2' consumed more than once}}
    x2 = Klass()
    let f = {
        borrowVal(x2)
        consumeVal(x2) // expected-note {{consuming use here}}
        consumeVal(x2) // expected-note {{consuming use here}}
        // expected-note @-1 {{consuming use here}}
    }
    f()
    consumeVal(x2) // expected-note {{consuming use here}}
    let x3 = x2 // expected-note {{consuming use here}}
    x2 = Klass()
    let _ = x3
}

public func closureLetCaptureClassArgUseAfterConsume(_ x2: inout Klass) {
    // expected-note @-1 {{'x2' is declared 'inout'}}
    let f = {
        // expected-error @-1 {{escaping closure captures 'inout' parameter 'x2'}}
        borrowVal(x2) // expected-note {{captured here}}
        consumeVal(x2) // expected-note {{captured here}}
        consumeVal(x2) // expected-note {{captured here}}
    }
    f()
}

func closureLetStoreClosureInVariableIsEscape() {
    let s = NonTrivialStruct()

    struct StoreClosure {
        var f: () -> ()
    }

    let f = {
        borrowVal(s)
        consumeVal(s) // expected-error {{'s' was consumed but it is illegal to consume a noncopyable immutable capture of an escaping closure. One can only read from it}}
    }
    let c = StoreClosure(f: f)
    _ = c
    consumeVal(s) // expected-error {{'s' was consumed but it is illegal to consume a noncopyable immutable capture of an escaping closure. One can only read from it}}
}

/////////////////////////////
// MARK: Closure Var Tests //
/////////////////////////////

// These are considered to be escaping since we are storing them into a
// var. This matches the behavior of how we emit inout diagnostics.

public func closureVarClassUseAfterConsume1() {
    var f = {}
    f = {
        var x2 = Klass() // expected-error {{'x2' consumed more than once}}
        x2 = Klass()
        borrowVal(x2)
        consumeVal(x2) // expected-note {{consuming use here}}
        consumeVal(x2) // expected-note {{consuming use here}}
    }
    f()
}

public func closureVarClassUseAfterConsume2() {
    var f = { () in}
    f = { () in
        var x2 = Klass() // expected-error {{'x2' consumed more than once}}
        x2 = Klass()
        borrowVal(x2)
        consumeVal(x2) // expected-note {{consuming use here}}
        consumeVal(x2) // expected-note {{consuming use here}}
    }
    f()
}

public func closureVarClassUseAfterConsumeArg(_ argX: inout Klass) {
    // TODO: Fix this
    var f = { (_ x2: inout Klass) in}
    f = { (_ x2: inout Klass) in
        // expected-error @-1 {{'x2' consumed but not reinitialized before end of function}}
        // expected-error @-2 {{'x2' consumed more than once}}
        borrowVal(x2)
        consumeVal(x2) // expected-note {{consuming use here}}
        consumeVal(x2) // expected-note {{consuming use here}}
        // expected-note @-1 {{consuming use here}}
    }
    f(&argX)
}

// We do not support captures of vars by closures today.
public func closureVarCaptureClassUseAfterConsume() {
    var x2 = Klass()
    x2 = Klass()
    var f = {}
    f = {
        borrowVal(x2)
        consumeVal(x2) // expected-error {{'x2' was consumed but it is illegal to consume a noncopyable mutable capture of an escaping closure. One can only read from it or assign over it}}
        consumeVal(x2) // expected-error {{'x2' was consumed but it is illegal to consume a noncopyable mutable capture of an escaping closure. One can only read from it or assign over it}}
    }
    f()
}

public func closureVarCaptureClassUseAfterConsume2() {
    var x2 = Klass()
    x2 = Klass()
    var f = {}
    f = {
        borrowVal(x2)
        consumeVal(x2) // expected-error {{'x2' was consumed but it is illegal to consume a noncopyable mutable capture of an escaping closure. One can only read from it or assign over it}}
    }
    f()
}

public func closureVarCaptureClassUseAfterConsumeError() {
    var x2 = Klass() 
    x2 = Klass()
    var f = {}
    f = {
        borrowVal(x2)
        consumeVal(x2) // expected-error {{'x2' was consumed but it is illegal to consume a noncopyable mutable capture of an escaping closure. One can only read from it or assign over it}}
        consumeVal(x2) // expected-error {{'x2' was consumed but it is illegal to consume a noncopyable mutable capture of an escaping closure. One can only read from it or assign over it}}
    }
    f()
    consumeVal(x2) // expected-error {{'x2' was consumed but it is illegal to consume a noncopyable mutable capture of an escaping closure. One can only read from it or assign over it}}
    let x3 = x2 // expected-error {{'x2' was consumed but it is illegal to consume a noncopyable mutable capture of an escaping closure. One can only read from it or assign over it}}
    x2 = Klass()
    let _ = x3
}

public func closureVarCaptureClassArgUseAfterConsume(_ x2: inout Klass) {
    // expected-note @-1 {{'x2' is declared 'inout'}}
    var f = {}
    f = {
        // expected-error @-1 {{escaping closure captures 'inout' parameter 'x2'}}
        borrowVal(x2) // expected-note {{captured here}}
        consumeVal(x2) // expected-note {{captured here}}
        consumeVal(x2) // expected-note {{captured here}}
    }
    f()
}

///////////////////////
// MARK: Defer Tests //
///////////////////////

public func deferCaptureClassUseAfterConsume() {
    var x2 = Klass()
    // expected-error @-1 {{'x2' used after consume}}
    // expected-error @-2 {{'x2' consumed in closure but not reinitialized before end of closure}}
    // expected-error @-3 {{'x2' consumed more than once}}
    x2 = Klass()
    defer { // expected-note {{non-consuming use here}}
        borrowVal(x2)
        consumeVal(x2) // expected-note {{consuming use here}}
        consumeVal(x2)
        // expected-note @-1 {{consuming use here}}
        // expected-note @-2 {{consuming use here}}
    }
    consumeVal(x2) // expected-note {{consuming use here}}
}

public func deferCaptureClassUseAfterConsume2() {
    var x2 = Klass()
    // expected-error @-1 {{'x2' consumed in closure but not reinitialized before end of closure}}
    // expected-error @-2 {{'x2' consumed more than once}}
    // expected-error @-3 {{'x2' used after consume}}
    x2 = Klass()
    defer { // expected-note {{non-consuming use here}}
        borrowVal(x2)
        consumeVal(x2) // expected-note {{consuming use here}}
        consumeVal(x2) // expected-note {{consuming use here}}
        // expected-note @-1 {{consuming use here}}
    }
    let x3 = x2 // expected-note {{consuming use here}}
    let _ = x3
}

public func deferCaptureClassArgUseAfterConsume(_ x2: inout Klass) {
    // expected-error @-1 {{'x2' consumed in closure but not reinitialized before end of closure}}
    // expected-error @-2 {{'x2' consumed more than once}}
    borrowVal(x2)
    defer {
        borrowVal(x2)
        consumeVal(x2) // expected-note {{consuming use here}}
        consumeVal(x2) // expected-note {{consuming use here}}
        // expected-note @-1 {{consuming use here}}
    }
    print("foo")
}

public func closureLetAndDeferCaptureClassUseAfterConsume() {
    var x2 = Klass()
    // expected-error @-1 {{'x2' consumed in closure but not reinitialized before end of closure}}
    // expected-error @-2 {{'x2' consumed more than once}}
    x2 = Klass()
    let f = {
        defer {
            borrowVal(x2)
            consumeVal(x2) // expected-note {{consuming use here}}
            consumeVal(x2)
            // expected-note @-1 {{consuming use here}}
            // expected-note @-2 {{consuming use here}}
        }
        print("foo")
    }
    f()
}

public func closureLetAndDeferCaptureClassUseAfterConsume2() {
    var x2 = Klass() // expected-error {{'x2' used after consume}}
    // expected-error @-1 {{'x2' consumed in closure but not reinitialized before end of closure}}
    // expected-error @-2 {{'x2' consumed more than once}}
    x2 = Klass()
    let f = {
        consumeVal(x2) // expected-note {{consuming use here}}
        defer { // expected-note {{non-consuming use here}}
            borrowVal(x2)
            consumeVal(x2) // expected-note {{consuming use here}}
            consumeVal(x2)
            // expected-note @-1 {{consuming use here}}
            // expected-note @-2 {{consuming use here}}
        }
        print("foo")
    }
    f()
}

public func closureLetAndDeferCaptureClassUseAfterConsume3() {
    var x2 = Klass() // expected-error {{'x2' used after consume}}
    // expected-error @-1 {{'x2' consumed in closure but not reinitialized before end of closure}}
    // expected-error @-2 {{'x2' consumed more than once}}
    x2 = Klass()
    let f = { 
        consumeVal(x2) // expected-note {{consuming use here}}
        defer { // expected-note {{non-consuming use here}}
            borrowVal(x2)
            consumeVal(x2) // expected-note {{consuming use here}}
            consumeVal(x2)
            // expected-note @-1 {{consuming use here}}
            // expected-note @-2 {{consuming use here}}
        }
        print("foo")
    }
    f()
    consumeVal(x2)
}

public func closureLetAndDeferCaptureClassArgUseAfterConsume(_ x2: inout Klass) {
    // expected-error @-1 {{'x2' consumed in closure but not reinitialized before end of closure}}
    // expected-error @-2 {{'x2' consumed more than once}}
    // expected-note @-3 {{'x2' is declared 'inout'}}
    let f = { // expected-error {{escaping closure captures 'inout' parameter 'x2'}}
        defer { // expected-note {{captured indirectly by this call}}
            borrowVal(x2) // expected-note {{captured here}}
            consumeVal(x2) // expected-note {{captured here}}
            // expected-note @-1 {{consuming use here}}
            consumeVal(x2) // expected-note {{captured here}}
            // expected-note @-1 {{consuming use here}}
            // expected-note @-2 {{consuming use here}}
        }
        print("foo")
    }
    f()
}

///////////////////////////////////////////
// MARK: Multiple Levels of Let Closures //
///////////////////////////////////////////

public func closureLetAndClosureCaptureClassUseAfterConsume() {
    var x2 = Klass() // expected-error {{'x2' consumed more than once}}
    // expected-error @-1 {{'x2' consumed in closure but not reinitialized before end of closure}}
    x2 = Klass()
    let f = {
        let g = {
            borrowVal(x2)
            consumeVal(x2) // expected-note {{consuming use here}}
            consumeVal(x2) // expected-note {{consuming use here}}
            // expected-note @-1 {{consuming use here}}
        }
        g()
    }
    f()
}

public func closureLetAndClosureCaptureClassUseAfterConsume2() {
    var x2 = Klass() // expected-error {{'x2' consumed more than once}}
    // expected-error @-1 {{'x2' consumed in closure but not reinitialized before end of closure}}
    x2 = Klass()
    let f = {
        let g = {
            borrowVal(x2)
            consumeVal(x2) // expected-note {{consuming use here}}
            consumeVal(x2) // expected-note {{consuming use here}}
            // expected-note @-1 {{consuming use here}}
        }
        g()
    }
    f()
    consumeVal(x2)
}


public func closureLetAndClosureCaptureClassArgUseAfterConsume(_ x2: inout Klass) {
    // expected-note @-1 {{'x2' is declared 'inout'}}
    // expected-note @-2 {{'x2' is declared 'inout'}}
    let f = { // expected-error {{escaping closure captures 'inout' parameter 'x2'}}
        let g = { // expected-error {{escaping closure captures 'inout' parameter 'x2'}}
            // expected-note @-1 {{captured indirectly by this call}}
            borrowVal(x2)
            // expected-note @-1 {{captured here}}
            // expected-note @-2 {{captured here}}
            consumeVal(x2)
            // expected-note @-1 {{captured here}}
            // expected-note @-2 {{captured here}}
            consumeVal(x2)
            // expected-note @-1 {{captured here}}
            // expected-note @-2 {{captured here}}
        }
        g()
    }
    f()
}

/////////////////////////////////
// MARK: Defer and Var Closure //
/////////////////////////////////

public func closureVarAndDeferCaptureClassUseAfterConsume(_ x: borrowing Klass) { // expected-error {{'x' has guaranteed ownership but was consumed}}
    var x2 = Klass() // expected-error {{'x2' consumed more than once}}
    // expected-error @-1 {{'x2' consumed in closure but not reinitialized before end of closure}}
    x2 = x // expected-note {{consuming use here}}
    var f = {}
    f = {
        defer {
            borrowVal(x2)
            consumeVal(x2) // expected-note {{consuming use here}}
            consumeVal(x2) // expected-note {{consuming use here}}
            // expected-note @-1 {{consuming use here}}
        }
        print("foo")
    }
    f()
}

public func closureVarAndDeferCaptureClassUseAfterConsume2(_ x: borrowing Klass) { // expected-error {{'x' has guaranteed ownership but was consumed}}
    var x2 = Klass()
    // expected-error @-1 {{'x2' consumed in closure but not reinitialized before end of closure}}
    // expected-error @-2 {{'x2' consumed more than once}}
    x2 = x // expected-note {{consuming use here}}
    var f = {}
    f = {
        consumeVal(x2) // expected-error {{'x2' was consumed but it is illegal to consume a noncopyable mutable capture of an escaping closure. One can only read from it or assign over it}}
        defer {
            borrowVal(x2)
            consumeVal(x2) // expected-note {{consuming use here}}
            consumeVal(x2) // expected-note {{consuming use here}}
            // expected-note @-1 {{consuming use here}}
        }
        print("foo")
    }
    f()
}

// TODO: MG
public func closureVarAndDeferCaptureClassUseAfterConsume3(_ x: borrowing Klass) { // expected-error {{'x' has guaranteed ownership but was consumed}}
    var x2 = Klass()
    // expected-error @-1 {{'x2' consumed more than once}}
    // expected-error @-2 {{'x2' consumed in closure but not reinitialized before end of closure}}
    x2 = x
    // expected-note @-1 {{consuming use here}}
    var f = {}
    f = {
        consumeVal(x2) // expected-error {{'x2' was consumed but it is illegal to consume a noncopyable mutable capture of an escaping closure. One can only read from it or assign over it}}
        defer {
            borrowVal(x2)
            consumeVal(x2) // expected-note {{consuming use here}}
            consumeVal(x2) // expected-note {{consuming use here}}
            // expected-note @-1 {{consuming use here}}
        }
        print("foo")
    }
    f()
    consumeVal(x2) // expected-error {{'x2' was consumed but it is illegal to consume a noncopyable mutable capture of an escaping closure. One can only read from it or assign over it}}
}

public func closureVarAndDeferCaptureClassArgUseAfterConsume(_ x2: borrowing Klass) {
    // expected-error @-1 {{'x2' consumed in closure. This is illegal since if the closure is invoked more than once the binding will be uninitialized on later invocations}}
    // expected-error @-2 {{'x2' has guaranteed ownership but was consumed due to being captured by a closure}}
    var f = {}
    f = {// expected-note {{closure capture here}}
        defer {
            borrowVal(x2)
            consumeVal(x2) // expected-note {{consuming use here}}
            consumeVal(x2) // expected-note {{consuming use here}}
        }
        print("foo")
    }
    f()
}

public func closureVarAndDeferCaptureClassOwnedArgUseAfterConsume(_ x2: __owned Klass) {
    // expected-error @-1 {{'x2' consumed in closure. This is illegal since if the closure is invoked more than once the binding will be uninitialized on later invocations}}
    var f = {}
    f = {
        defer {
            borrowVal(x2)
            consumeVal(x2) // expected-note {{consuming use here}}
            consumeVal(x2) // expected-note {{consuming use here}}
        }
        print("foo")
    }
    f()
}

public func closureVarAndDeferCaptureClassOwnedArgUseAfterConsume2(_ x2: consuming Klass) {
    // expected-error @-1 {{'x2' consumed in closure but not reinitialized before end of closure}}
    // expected-error @-2 {{'x2' consumed more than once}}
    var f = {}
    f = {
        defer {
            borrowVal(x2)
            consumeVal(x2) // expected-note {{consuming use here}}
            consumeVal(x2) // expected-note {{consuming use here}}
            // expected-note @-1 {{consuming use here}}
        }
        print("foo")
    }
    f()
}

// TODO: MG
public func closureVarAndDeferCaptureClassOwnedArgUseAfterConsume3(_ x2: __owned Klass) {
    // expected-error @-1 {{'x2' consumed in closure. This is illegal since if the closure is invoked more than once the binding will be uninitialized on later invocations}}
    var f = {}
    f = {
        defer {
            borrowVal(x2)
            consumeVal(x2) // expected-note {{consuming use here}}
            consumeVal(x2) // expected-note {{consuming use here}}
        }
        print("foo")
    }
    f()
    consumeVal(x2) // expected-error {{'x2' was consumed but it is illegal to consume a noncopyable immutable capture of an escaping closure. One can only read from it}}
}

public func closureVarAndDeferCaptureClassOwnedArgUseAfterConsume4(_ x2: consuming Klass) {
    // expected-error @-1 {{'x2' consumed more than once}}
    // expected-error @-2 {{'x2' consumed in closure but not reinitialized before end of closure}}
    var f = {}
    f = {
        defer {
            borrowVal(x2)
            consumeVal(x2) // expected-note {{consuming use here}}
            consumeVal(x2) // expected-note {{consuming use here}}
            // expected-note @-1 {{consuming use here}}
        }
        print("foo")
    }
    f()
    consumeVal(x2) // expected-error {{'x2' was consumed but it is illegal to consume a noncopyable mutable capture of an escaping closure. One can only read from it or assign over it}}
}

///////////////////////////////////////////
// MARK: Multiple Levels of Var Closures //
///////////////////////////////////////////

public func closureVarAndClosureCaptureClassUseAfterConsume(_ x: borrowing Klass) { // expected-error {{'x' has guaranteed ownership but was consumed}}
    var x2 = Klass()
    x2 = x // expected-note {{consuming use here}}
    var f = {}
    f = {
        var g = {}
        g = {
            borrowVal(x2)
            consumeVal(x2) // expected-error {{'x2' was consumed but it is illegal to consume a noncopyable mutable capture of an escaping closure. One can only read from it or assign over it}}
            consumeVal(x2) // expected-error {{'x2' was consumed but it is illegal to consume a noncopyable mutable capture of an escaping closure. One can only read from it or assign over it}}
        }
        g()
        consumeVal(x2) // expected-error {{'x2' was consumed but it is illegal to consume a noncopyable mutable capture of an escaping closure. One can only read from it or assign over it}}
    }
    f()
}

public func closureVarAndClosureCaptureClassUseAfterConsume2(_ x: borrowing Klass) { // expected-error {{'x' has guaranteed ownership but was consumed}}
    var x2 = Klass()
    x2 = x
    // expected-note @-1 {{consuming use here}}

    var f = {}
    f = {
        var g = {}
        g = {
            borrowVal(x2)
            consumeVal(x2) // expected-error {{'x2' was consumed but it is illegal to consume a noncopyable mutable capture of an escaping closure. One can only read from it or assign over it}}
            consumeVal(x2) // expected-error {{'x2' was consumed but it is illegal to consume a noncopyable mutable capture of an escaping closure. One can only read from it or assign over it}}
        }
        g()
    }
    f()
    consumeVal(x2) // expected-error {{'x2' was consumed but it is illegal to consume a noncopyable mutable capture of an escaping closure. One can only read from it or assign over it}}
    let x3 = x2 // expected-error {{'x2' was consumed but it is illegal to consume a noncopyable mutable capture of an escaping closure. One can only read from it or assign over it}}
    _ = x3
}

public func closureVarAndClosureCaptureClassUseAfterConsume3(_ x: borrowing Klass) { // expected-error {{'x' has guaranteed ownership but was consumed}}
    var x2 = x
    // expected-note @-1 {{consuming use here}}
    x2 = x
    // expected-note @-1 {{consuming use here}}

    var f = {}
    f = {
        var g = {}
        g = {
            borrowVal(x2)
            consumeVal(x2) // expected-error {{'x2' was consumed but it is illegal to consume a noncopyable mutable capture of an escaping closure. One can only read from it or assign over it}}
            consumeVal(x2) // expected-error {{'x2' was consumed but it is illegal to consume a noncopyable mutable capture of an escaping closure. One can only read from it or assign over it}}
        }
        g()
    }
    f()
    consumeVal(x2) // expected-error {{'x2' was consumed but it is illegal to consume a noncopyable mutable capture of an escaping closure. One can only read from it or assign over it}}
    let x3 = x2 // expected-error {{'x2' was consumed but it is illegal to consume a noncopyable mutable capture of an escaping closure. One can only read from it or assign over it}}
    _ = x3
}

public func closureVarAndClosureCaptureClassArgUseAfterConsume(_ x2: borrowing Klass) {
    // expected-error @-1 {{'x2' consumed in closure. This is illegal since if the closure is invoked more than once the binding will be uninitialized on later invocations}}
    // expected-error @-2 {{'x2' has guaranteed ownership but was consumed due to being captured by a closure}}
    // expected-error @-3 {{'x2' has guaranteed ownership but was consumed due to being captured by a closure}}
    var f = {}
    f = {// expected-note {{closure capture here}}
        var g = {}
        g = {// expected-note {{closure capture here}}
            borrowVal(x2)
            consumeVal(x2) // expected-note {{consuming use here}}
            consumeVal(x2) // expected-note {{consuming use here}}
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
            consumeVal(x2) // expected-error {{'x2' was consumed but it is illegal to consume a noncopyable immutable capture of an escaping closure. One can only read from it}}
            consumeVal(x2) // expected-error {{'x2' was consumed but it is illegal to consume a noncopyable immutable capture of an escaping closure. One can only read from it}}
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
            consumeVal(x2) // expected-error {{'x2' was consumed but it is illegal to consume a noncopyable mutable capture of an escaping closure. One can only read from it or assign over it}}
            consumeVal(x2) // expected-error {{'x2' was consumed but it is illegal to consume a noncopyable mutable capture of an escaping closure. One can only read from it or assign over it}}
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
            consumeVal(x2) // expected-error {{'x2' was consumed but it is illegal to consume a noncopyable immutable capture of an escaping closure. One can only read from it}}
            consumeVal(x2) // expected-error {{'x2' was consumed but it is illegal to consume a noncopyable immutable capture of an escaping closure. One can only read from it}}
        }
        g()
    }
    f()
    consumeVal(x2) // expected-error {{'x2' was consumed but it is illegal to consume a noncopyable immutable capture of an escaping closure. One can only read from it}}
}

public func closureVarAndClosureCaptureClassOwnedArgUseAfterConsume4(_ x2: consuming Klass) {
    var f = {}
    f = {
        var g = {}
        g = {
            borrowVal(x2)
            consumeVal(x2) // expected-error {{'x2' was consumed but it is illegal to consume a noncopyable mutable capture of an escaping closure. One can only read from it or assign over it}}
            consumeVal(x2) // expected-error {{'x2' was consumed but it is illegal to consume a noncopyable mutable capture of an escaping closure. One can only read from it or assign over it}}
        }
        g()
    }
    f()
    consumeVal(x2) // expected-error {{'x2' was consumed but it is illegal to consume a noncopyable mutable capture of an escaping closure. One can only read from it or assign over it}}
}

/////////////////////////////
// Tests For Move Operator //
/////////////////////////////

func moveOperatorTest(_ k: __owned Klass) {
    var k2 = k
    // expected-error @-1 {{'k2' consumed more than once}}
    // expected-error @-2 {{'k2' consumed more than once}}
    // expected-error @-3 {{'k2' consumed more than once}}
    k2 = Klass()
    let k3 = consume k2 // expected-note {{consuming use here}}
    let _ = consume k2
    // expected-note @-1 {{consuming use here}}
    // expected-note @-2 {{consuming use here}}
    _ = k2
    // expected-note @-1 {{consuming use here}}
    // expected-note @-2 {{consuming use here}}
    let _ = k2
    // expected-note @-1 {{consuming use here}}
    let _ = k3
}

func moveOperatorTest2(_ k: consuming Klass) {
    var k2 = k
    // expected-error @-1 {{'k2' consumed more than once}}
    // expected-error @-2 {{'k2' consumed more than once}}
    // expected-error @-3 {{'k2' consumed more than once}}
    k2 = Klass()
    let k3 = consume k2 // expected-note {{consuming use here}}
    let _ = consume k2
    // expected-note @-1 {{consuming use here}}
    // expected-note @-2 {{consuming use here}}
    _ = k2
    // expected-note @-1 {{consuming use here}}
    // expected-note @-2 {{consuming use here}}
    let _ = k2
    // expected-note @-1 {{consuming use here}}
    let _ = k3
}

/////////////////////////////////////////
// Black hole initialization test case//
/////////////////////////////////////////

func blackHoleKlassTestCase(_ k: __owned Klass) {
    var k2 = k
    // expected-error @-1 {{'k2' consumed more than once}}
    // expected-error @-2 {{'k2' consumed more than once}}
    // expected-error @-3 {{'k2' consumed more than once}}
    // expected-error @-4 {{'k2' consumed more than once}}
    let _ = k2 // expected-note {{consuming use here}}
    let _ = k2 // expected-note {{consuming use here}}

    k2 = Klass()
    var _ = k2 // expected-note {{consuming use here}}
    var _ = k2
    // expected-note @-1 {{consuming use here}}
    // expected-note @-2 {{consuming use here}}

    _ = k2
    // expected-note @-1 {{consuming use here}}
    // expected-note @-2 {{consuming use here}}

    // TODO: Why do we not also get 2 errors here?
    _ = k2
    // expected-note @-1 {{consuming use here}}
}

func blackHoleKlassTestCase2(_ k: consuming Klass) {
    var k2 = k
    // expected-error @-1 {{'k2' consumed more than once}}
    // expected-error @-2 {{'k2' consumed more than once}}
    // expected-error @-3 {{'k2' consumed more than once}}
    // expected-error @-4 {{'k2' consumed more than once}}
    let _ = k2 // expected-note {{consuming use here}}
    let _ = k2 // expected-note {{consuming use here}}

    k2 = Klass()
    var _ = k2 // expected-note {{consuming use here}}
    var _ = k2
    // expected-note @-1 {{consuming use here}}
    // expected-note @-2 {{consuming use here}}

    _ = k2
    // expected-note @-1 {{consuming use here}}
    // expected-note @-2 {{consuming use here}}

    // TODO: Why do we not also get 2 errors here?
    _ = k2
    // expected-note @-1 {{consuming use here}}
}

///////////////////////////////////////
// Copyable Type in a Move Only Type //
///////////////////////////////////////

func copyableKlassInAMoveOnlyStruct() {
    var a = NonTrivialStruct()
    a = NonTrivialStruct()
    borrowVal(a.copyableK)
    consumeVal(a.copyableK)
}

// This shouldn't error since we are consuming a copyable type.
func copyableKlassInAMoveOnlyStruct2() {
    var a = NonTrivialStruct()
    a = NonTrivialStruct()
    borrowVal(a.copyableK)
    consumeVal(a.copyableK)
    consumeVal(a.copyableK)
}

// This shouldn't error since we are working with a copyable type.
func copyableKlassInAMoveOnlyStruct3() {
    var a = NonTrivialStruct()
    a = NonTrivialStruct()
    borrowVal(a.copyableK)
    consumeVal(a.copyableK)
    borrowVal(a.copyableK)
}

// This used to error, but no longer errors since we are using a true field
// sensitive model.
func copyableKlassInAMoveOnlyStruct4() {
    var a = NonTrivialStruct()
    a = NonTrivialStruct()
    borrowVal(a.copyableK)
    consumeVal(a.copyableK)
    borrowVal(a.nonTrivialStruct2)
}

func copyableStructsInMoveOnlyStructNonConsuming() {
    var a = NonTrivialStruct()
    a = NonTrivialStruct()
    borrowVal(a)
    borrowVal(a.nonTrivialStruct2)
    borrowVal(a.nonTrivialCopyableStruct)
    borrowVal(a.nonTrivialCopyableStruct.nonTrivialCopyableStruct2)
    borrowVal(a.nonTrivialCopyableStruct.nonTrivialCopyableStruct2.copyableKlass)
}

func computedCopyableKlassInAMoveOnlyStruct() {
    var a = NonTrivialStruct()
    a = NonTrivialStruct()
    borrowVal(a.computedCopyableK)
    consumeVal(a.computedCopyableK)
}

// This shouldn't error since we are consuming a copyable type.
func computedCopyableKlassInAMoveOnlyStruct2() {
    var a = NonTrivialStruct()
    a = NonTrivialStruct()
    borrowVal(a.computedCopyableK)
    consumeVal(a.computedCopyableK)
    consumeVal(a.computedCopyableK)
}

// This shouldn't error since we are working with a copyable type.
func computedCopyableKlassInAMoveOnlyStruct3() {
    var a = NonTrivialStruct()
    a = NonTrivialStruct()
    borrowVal(a.computedCopyableK)
    consumeVal(a.computedCopyableK)
    borrowVal(a.computedCopyableK)
}

// This used to error, but no longer errors since we are using a true field
// sensitive model.
func computedCopyableKlassInAMoveOnlyStruct4() {
    var a = NonTrivialStruct()
    a = NonTrivialStruct()
    borrowVal(a.computedCopyableK)
    consumeVal(a.computedCopyableK)
    borrowVal(a.nonTrivialStruct2)
}

func computedCopyableStructsInMoveOnlyStructNonConsuming() {
    var a = NonTrivialStruct()
    a = NonTrivialStruct()
    borrowVal(a)
    borrowVal(a.computedCopyableK)
    borrowVal(a.nonTrivialCopyableStruct.nonTrivialCopyableStruct2.computedCopyableKlass)
}

///////////////////////////
// Field Sensitive Tests //
///////////////////////////

func fieldSensitiveTestReinitField() {
    var a = NonTrivialStruct()
    a = NonTrivialStruct()
    consumeVal(a.k)
    a.k = Klass()
    borrowVal(a.k)
}

func fieldSensitiveTestReinitFieldMultiBlock1() {
    var a = NonTrivialStruct()
    a = NonTrivialStruct()
    consumeVal(a.k)

    if boolValue {
        a.k = Klass()
        borrowVal(a.k)
    }
}

func fieldSensitiveTestReinitFieldMultiBlock2() {
    var a = NonTrivialStruct() // expected-error {{'a' used after consume}}
    a = NonTrivialStruct()
    consumeVal(a.k) // expected-note {{consuming use here}}

    if boolValue {
        a.k = Klass()
    }

    borrowVal(a.k) // expected-note {{non-consuming use here}}
}

func fieldSensitiveTestReinitFieldMultiBlock3() {
    var a = NonTrivialStruct()
    a = NonTrivialStruct()
    consumeVal(a.k)

    if boolValue {
        a.k = Klass()
    } else {
        a.k = Klass()
    }

    borrowVal(a.k)
}

// This test sees what happens if we partially reinit along one path and do a
// full reinit along another path.
func fieldSensitiveTestReinitFieldMultiBlock4() {
    var a = NonTrivialStruct()
    a = NonTrivialStruct()
    consumeVal(a.k)

    if boolValue {
        a.k = Klass()
    } else {
        a = NonTrivialStruct()
    }

    borrowVal(a.k)
}

func fieldSensitiveTestReinitEnumMultiBlock() {
    var e = NonTrivialEnum.first // expected-error {{'e' used after consume}}
    e = NonTrivialEnum.second(Klass())
    switch e { // expected-note {{consuming use here}}
    case .second:
        e = NonTrivialEnum.third(NonTrivialStruct())
    default:
        break
    }
    borrowVal(e) // expected-note {{non-consuming use here}}
}

func fieldSensitiveTestReinitEnumMultiBlock1() {
    var e = NonTrivialEnum.first
    e = NonTrivialEnum.second(Klass())
    switch e {
    case .second:
        e = NonTrivialEnum.third(NonTrivialStruct())
    default:
        e = NonTrivialEnum.fourth(CopyableKlass())
    }
    borrowVal(e)
}

func fieldSensitiveTestReinitEnumMultiBlock2() {
    var e = NonTrivialEnum.first
    e = NonTrivialEnum.second(Klass())
    if boolValue {
        switch e {
        case .second:
            e = NonTrivialEnum.third(NonTrivialStruct())
        default:
            e = NonTrivialEnum.fourth(CopyableKlass())
        }
    } else {
        e = NonTrivialEnum.third(NonTrivialStruct())
    }
    borrowVal(e)
}

////////////////////////////////////////////
// Multiple Use by Same CallSite TestCase //
////////////////////////////////////////////

func sameCallSiteTestConsumeTwice(_ k: inout Klass) { // expected-error {{'k' consumed more than once}}
    func consumeKlassTwice(_ k: __owned Klass, _ k2: __owned Klass) {}
    consumeKlassTwice(k, k) // expected-error {{overlapping accesses to 'k', but deinitialization requires exclusive access}}
    // expected-note @-1 {{consuming use here}}
    // expected-note @-2 {{consuming use here}}
    // expected-note @-3 {{conflicting access is here}}
    k = Klass()
}

func sameCallSiteConsumeAndUse(_ k: inout Klass) { // expected-error {{'k' used after consume}}
    func consumeKlassAndUseKlass(_ k: __owned Klass, _ k2: borrowing Klass) {}
    consumeKlassAndUseKlass(k, k) // expected-error {{overlapping accesses to 'k', but deinitialization requires exclusive access}}
    // expected-note @-1 {{consuming use here}}
    // expected-note @-2 {{non-consuming use here}}
    // expected-note @-3 {{conflicting access is here}}
    k = Klass()
}

func inoutAndConsumingUse(_ k: inout Klass) { // expected-error {{'k' used after consume}}
    func consumeKlassAndInoutUseKlass(_ k: __owned Klass, _ k2: inout Klass) {}
    consumeKlassAndInoutUseKlass(k, &k) // expected-error {{overlapping accesses to 'k', but deinitialization requires exclusive access}}
    // expected-note @-1 {{non-consuming use here}}
    // expected-note @-2 {{consuming use here}}
    // expected-note @-3 {{conflicting access is here}}
}

////////////////////////////
// Ref Element Addr Tests //
////////////////////////////

func copyableKlassWithMoveOnlyFieldBorrowValue(_ x: CopyableKlassWithMoveOnlyField) {
    borrowVal(x.moveOnlyVarStruct)
    borrowVal(x.moveOnlyVarStruct)
    borrowVal(x.moveOnlyVarStruct.nonTrivialStruct2)
    borrowVal(x.moveOnlyLetStruct)
    borrowVal(x.moveOnlyLetStruct)
    borrowVal(x.moveOnlyLetStruct.nonTrivialStruct2)
}

func copyableKlassWithMoveOnlyFieldConsumeValue(_ x: CopyableKlassWithMoveOnlyField) {
    consumeVal(x.moveOnlyVarStruct)
    // expected-error @-1 {{'x.moveOnlyVarStruct' was consumed but it is illegal to consume a noncopyable class var field. One can only read from it or assign to it}}
    consumeVal(x.moveOnlyVarStruct.nonTrivialStruct2) // expected-error {{'x.moveOnlyVarStruct' was consumed but it is illegal to consume a noncopyable class var field. One can only read from it or assign to it}}
    // TODO: We should place a note on x. We need to make the diagnostic part of
    // this a little smarter.
    consumeVal(x.moveOnlyLetStruct) // expected-error {{'x.moveOnlyLetStruct' was consumed but it is illegal to consume a noncopyable class let field. One can only read from it}}
    consumeVal(x.moveOnlyLetStruct.nonTrivialStruct2) // expected-error {{'x.moveOnlyLetStruct' was consumed but it is illegal to consume a noncopyable class let field. One can only read from it}}
}

func copyableKlassWithMoveOnlyFieldAssignValue(_ x: CopyableKlassWithMoveOnlyField) {
    x.moveOnlyVarStruct = NonTrivialStruct()
    x.moveOnlyVarStruct = NonTrivialStruct()
}

///////////////////////
// Global Addr Tests //
///////////////////////

var varGlobal = NonTrivialStruct()
let letGlobal = NonTrivialStruct()

func moveOnlyGlobalBorrowValue() {
    borrowVal(varGlobal)
    borrowVal(varGlobal.nonTrivialStruct2)
    borrowVal(letGlobal)
    borrowVal(letGlobal.nonTrivialStruct2)
}

func moveOnlyGlobalConsumeValue() {
    consumeVal(varGlobal) // expected-error {{'varGlobal' was consumed but it is illegal to consume a noncopyable global var. One can only read from it or assign to it}}
    // TODO: Fix error to say that it is from nonTrivialStruct2
    consumeVal(varGlobal.nonTrivialStruct2) // expected-error {{'varGlobal' was consumed but it is illegal to consume a noncopyable global var. One can only read from it or assign to it}}
    consumeVal(letGlobal) // expected-error {{'letGlobal' was consumed but it is illegal to consume a noncopyable global let. One can only read from it}}
    // TODO: Fix error to say that it is from nonTrivialStruct2
    consumeVal(letGlobal.nonTrivialStruct2) // expected-error {{'letGlobal' was consumed but it is illegal to consume a noncopyable global let. One can only read from it}}
}

func moveOnlyGlobalAssignValue() {
    varGlobal = NonTrivialStruct()
    varGlobal.nonTrivialStruct2 = NonTrivialStruct2()
}

///////////////////
// InOut Capture //
///////////////////

func inoutCaptureTest() -> (() -> ()) {
    var x = NonTrivialStruct()
    x = NonTrivialStruct()

    func useInOut(_ x: inout NonTrivialStruct) {}
    let f = {
        useInOut(&x)
    }

    borrowVal(x)
    consumeVal(x)
    // expected-error @-1 {{'x' was consumed but it is illegal to consume a noncopyable mutable capture of an escaping closure. One can only read from it or assign over it}}
    x = NonTrivialStruct()

    let g = {
        x = NonTrivialStruct()
        useInOut(&x)
        consumeVal(x)
        // expected-error @-1 {{'x' was consumed but it is illegal to consume a noncopyable mutable capture of an escaping closure. One can only read from it or assign over it}}
    }
    g()

    return f
}

func inoutCaptureTestAddressOnlyGeneric<T : P>(_ t: T.Type) -> (() -> ()) {
    var x = AddressOnlyGeneric<T>()
    x = AddressOnlyGeneric<T>()

    func useInOut(_ x: inout AddressOnlyGeneric<T>) {}
    let f = {
        useInOut(&x)
    }

    borrowVal(x)
    consumeVal(x) // expected-error {{'x' was consumed but it is illegal to consume a noncopyable mutable capture of an escaping closure. One can only read from it or assign over it}}
    x = AddressOnlyGeneric<T>()

    let g = {
        x = AddressOnlyGeneric<T>()
        useInOut(&x)
        consumeVal(x) // expected-error {{'x' was consumed but it is illegal to consume a noncopyable mutable capture of an escaping closure. One can only read from it or assign over it}}
    }
    g()

    return f
}

////////////////
// Misc Tests //
////////////////

func borrowAndConsumeAtSameTime(_: borrowing NonTrivialStruct, consume _: __owned NonTrivialStruct) {}

func borrowAndConsumeAtSameTimeTest(x: __owned NonTrivialStruct) { // expected-error {{'x' used after consume}}
    borrowAndConsumeAtSameTime(x, consume: x)
    // expected-note @-1 {{consuming use here}}
    // expected-note @-2 {{non-consuming use here}}
}

func borrowAndConsumeAtSameTimeTest2(x: consuming NonTrivialStruct) { // expected-error {{'x' used after consume}}
    borrowAndConsumeAtSameTime(x, consume: x)
    // expected-note @-1 {{consuming use here}}
    // expected-note @-2 {{non-consuming use here}}
    // expected-error @-3 {{overlapping accesses to 'x', but deinitialization requires exclusive access}}
    // expected-note @-4 {{conflicting access is here}}
}

////////////////
// Yield Test //
////////////////

func yieldTest() {  
  // Make sure we do not crash on this.
  @_moveOnly
  struct S {
    var c = CopyableKlass()
    var c2: CopyableKlass {
      _read { yield c }
    }
  }
}

///////////////////////
// Empty Struct Test //
///////////////////////

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
  func testArg1(_ x: consuming EmptyStruct) {
    borrow(x)
  }

  func testArg2(_ x: consuming EmptyStruct) {
    consume(x)
  }

  func testArg2a(_ x: consuming EmptyStruct) {
    // expected-error @-1 {{'x' consumed more than once}}
    consume(x) // expected-note {{consuming use here}}
    consume(x) // expected-note {{consuming use here}}
  }

  func testArg2b(_ x: consuming EmptyStruct) {
    // expected-error @-1 {{'x' used after consume}}
    borrow(x)
    consume(x) // expected-note {{consuming use here}}
    borrow(x) // expected-note {{non-consuming use here}}
  }

  func testArg3(_ x: consuming EmptyStruct) {
    let _ = x
  }

  func testArg3a(_ x: consuming EmptyStruct) {
    // expected-error @-1 {{'x' consumed more than once}}
    let _ = x // expected-note {{consuming use here}}
    let _ = x // expected-note {{consuming use here}}
  }

  func testArg4(_ x: consuming EmptyStruct) {
    _ = x
  }

  func testArg4a(_ x: consuming EmptyStruct) {
    // expected-error @-1 {{'x' consumed more than once}}
    _ = x // expected-note {{consuming use here}}
    _ = x // expected-note {{consuming use here}}
  }

  func testArg4b(_ x: consuming EmptyStruct) {
    // expected-error @-1 {{'x' consumed more than once}}
    // expected-error @-2 {{'x' consumed more than once}}
    _ = x // expected-note {{consuming use here}}
    _ = x // expected-note {{consuming use here}}
    // expected-note @-1 {{consuming use here}}
    let _ = x // expected-note {{consuming use here}}
  }

  func testArg5(_ x: consuming EmptyStruct) {
    let y = x
    _ = y
  }

  func testArg6(_ x: consuming EmptyStruct) {
    x.doSomething()
  }

  func testArg7(_ x: consuming EmptyStruct) {
    x.doSomething3()
  }

  func testArg7a(_ x: consuming EmptyStruct) {
    // expected-error @-1 {{'x' consumed more than once}}
    x.doSomething3() // expected-note {{consuming use here}}
    x.doSomething3() // expected-note {{consuming use here}}
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

func borrow(_ x: consuming StructContainingEmptyStruct) {}
func consume(_ x: consuming StructContainingEmptyStruct) {}

func testStructContainingEmptyStruct() {
  func testArg1(_ x: consuming StructContainingEmptyStruct) {
    borrow(x)
  }

  func testArg2(_ x: consuming StructContainingEmptyStruct) {
    consume(x)
  }

  func testArg3(_ x: consuming StructContainingEmptyStruct) {
    let _ = x
  }

  func testArg4(_ x: consuming StructContainingEmptyStruct) {
    _ = x
  }

  func testArg5(_ x: consuming StructContainingEmptyStruct) {
    let y = x
    _ = y
  }

  func testArg6(_ x: consuming StructContainingEmptyStruct) {
    x.x.doSomething()
  }

  func testArg7(_ x: consuming StructContainingEmptyStruct) {
    x.x.doSomething3()
  }

  func testArg7a(_ x: consuming StructContainingEmptyStruct) {
    // expected-error @-1 {{'x' consumed more than once}}
    x.x.doSomething3() // expected-note {{consuming use here}}
    x.x.doSomething3() // expected-note {{consuming use here}}
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

func borrow(_ x: consuming StructContainingTwoEmptyStruct) {}
func consume(_ x: consuming StructContainingTwoEmptyStruct) {}

func testStructContainingTwoEmptyStruct() {
  func testArg1(_ x: consuming StructContainingTwoEmptyStruct) {
    borrow(x)
  }

  func testArg2(_ x: consuming StructContainingTwoEmptyStruct) {
    consume(x)
  }

  func testArg3(_ x: consuming StructContainingTwoEmptyStruct) {
    let _ = x
  }

  func testArg4(_ x: consuming StructContainingTwoEmptyStruct) {
    _ = x
  }

  func testArg5(_ x: consuming StructContainingTwoEmptyStruct) {
    let y = x
    _ = y
  }

  func testArg6(_ x: consuming StructContainingTwoEmptyStruct) {
    x.x.doSomething()
  }

  func testArg7(_ x: consuming StructContainingTwoEmptyStruct) {
    x.x.doSomething3()
  }

  func testArg8(_ x: consuming StructContainingTwoEmptyStruct) {
    x.y.doSomething3()
  }

  func testArg9(_ x: consuming StructContainingTwoEmptyStruct) {
    x.x.doSomething3()
    x.y.doSomething3()
  }

  func testArg10(_ x: consuming StructContainingTwoEmptyStruct) {
    // expected-error @-1 {{'x' consumed more than once}}
    x.x.doSomething3() // expected-note {{consuming use here}}
    x.y.doSomething3()
    x.x.doSomething3() // expected-note {{consuming use here}}
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

func borrow(_ x: borrowing MyEnum) {}

func testMyEnum() {
  func test1(_ x: consuming MyEnum) {
    if case let .first(y) = x {
      _ = y
    }
  }

  func test1a(_ x: consuming MyEnum) { // expected-error {{'x' consumed more than once}}
    if case let .first(y) = x { // expected-note {{consuming use here}}
      _ = consume x // expected-note {{consuming use here}}
      _ = y
    }
  }

  func test1b(_ x: consuming MyEnum) { // expected-error {{'x' consumed more than once}}
    if case let .first(y) = x { // expected-note {{consuming use here}}
      _ = y
    }
    _ = consume x // expected-note {{consuming use here}}
  }

  func test2(_ x: consuming MyEnum) {
    if case let .third(.first(y)) = x {
      _ = y
    }
  }

  func test2a(_ x: consuming MyEnum) { // expected-error {{'x' consumed more than once}}
    if case let .third(.first(y)) = x { // expected-note {{consuming use here}}
      _ = consume x // expected-note {{consuming use here}}
      _ = y
    }
  }

  func test2b(_ x: consuming MyEnum) { // expected-error {{'x' consumed more than once}}
    if case let .third(.first(y)) = x { // expected-note {{consuming use here}}
      _ = y
    }
    _ = consume x // expected-note {{consuming use here}}
  }

  func test2c(_ x: consuming MyEnum) { // expected-error {{'x' used after consume}}
    if case let .third(.first(y)) = x { // expected-note {{consuming use here}}
      _ = y
    }
    borrow(x) // expected-note {{non-consuming use here}}
  }

  func test3(_ x: consuming MyEnum) {
    switch x {
    case let .first(y):
      _ = y
      break
    default:
      break
    }
  }

  func test3a(_ x: consuming MyEnum) { // expected-error {{'x' consumed more than once}}
    switch x { // expected-note {{consuming use here}}
    case let .first(y):
      _ = y
      break
    default:
      break
    }
    _ = consume x // expected-note {{consuming use here}}
  }

  func test4(_ x: consuming MyEnum) {
    switch x {
    case let .third(.first(y)):
      _ = y
      break
    default:
      break
    }
  }

  func test4a(_ x: consuming MyEnum) { // expected-error {{'x' consumed more than once}}
    switch x { // expected-note {{consuming use here}}
    case let .third(.first(y)):
      _ = y
      break
    default:
      break
    }
    _ = consume x // expected-note {{consuming use here}}
  }
}
