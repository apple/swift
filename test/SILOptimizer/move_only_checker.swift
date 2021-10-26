// RUN: %target-swift-frontend -enable-experimental-lexical-lifetimes -verify %s -parse-stdlib -emit-sil

import Swift

public class Klass {
    var k: Klass? = nil
}

var boolValue: Bool { return true }

public func classUseMoveOnlyWithoutEscaping(_ x: Klass) {
}
public func classConsume(_ x: __owned Klass) {
}

public func classSimpleChainTest(_ x: Klass) {
    @_noImplicitCopy let x2 = x
    let y2 = x2
    let k2 = y2
    classUseMoveOnlyWithoutEscaping(k2)
}

public func classSimpleNonConsumingUseTest(_ x: Klass) {
    @_noImplicitCopy let x2 = x
    classUseMoveOnlyWithoutEscaping(x2)
}

public func classMultipleNonConsumingUseTest(_ x: Klass) {
    @_noImplicitCopy let x2 = x
    classUseMoveOnlyWithoutEscaping(x2)
    classUseMoveOnlyWithoutEscaping(x2)
    print(x2)
}

public func classUseAfterConsume(_ x: Klass) {
    @_noImplicitCopy let x2 = x // expected-error {{'x2' consumed more than once}}
    classUseMoveOnlyWithoutEscaping(x2)
    classConsume(x2) // expected-note {{consuming use}}
    print(x2) // expected-note {{consuming use}}
}

public func classDoubleConsume(_ x: Klass) {
    @_noImplicitCopy let x2 = x  // expected-error {{'x2' consumed more than once}}
    classConsume(x2) // expected-note {{consuming use}}
    classConsume(x2) // expected-note {{consuming use}}
}

public func classLoopConsume(_ x: Klass) {
    @_noImplicitCopy let x2 = x // expected-error {{'x2' consumed more than once}}
    for _ in 0..<1024 {
        classConsume(x2) // expected-note {{consuming use}}
    }
}

public func classDiamond(_ x: Klass) {
    @_noImplicitCopy let x2 = x
    if boolValue {
        classConsume(x2)
    } else {
        classConsume(x2)
    }
}

public func classDiamondInLoop(_ x: Klass) {
    @_noImplicitCopy let x2 = x // expected-error {{'x2' consumed more than once}}
    for _ in 0..<1024 {
      if boolValue {
          classConsume(x2) // expected-note {{consuming use}}
      } else {
          classConsume(x2) // expected-note {{consuming use}}
      }
    }
}

public func classAssignToVar1(_ x: Klass) {
    @_noImplicitCopy let x2 = x // expected-error {{'x2' consumed more than once}}
    var x3 = x2 // expected-note {{consuming use}}
    x3 = x2 // expected-note {{consuming use}}
    x3 = x
    print(x3)
}

public func classAssignToVar2(_ x: Klass) {
    @_noImplicitCopy let x2 = x // expected-error {{'x2' consumed more than once}}
    var x3 = x2 // expected-note {{consuming use}}
    x3 = x2 // expected-note {{consuming use}}
    classUseMoveOnlyWithoutEscaping(x3)
}

public func classAssignToVar3(_ x: Klass) {
    @_noImplicitCopy let x2 = x
    var x3 = x2
    x3 = x
    print(x3)
}

public func classAssignToVar4(_ x: Klass) {
    @_noImplicitCopy let x2 = x // expected-error {{'x2' consumed more than once}}
    let x3 = x2 // expected-note {{consuming use}}
    print(x2) // expected-note {{consuming use}}
    print(x3)
}

public func classAssignToVar5(_ x: Klass) {
    @_noImplicitCopy let x2 = x // expected-error {{'x2' consumed more than once}}
    var x3 = x2 // expected-note {{consuming use}}    
    // TODO: Need to mark this as the lifetime extending use. We fail
    // appropriately though.
    classUseMoveOnlyWithoutEscaping(x2)
    x3 = x
    print(x3)
}

public func classAccessField(_ x: Klass) {
    @_noImplicitCopy let x2 = x
    print(x2.k!)
    for _ in 0..<1024 {
        print(x2.k!)
    }
}

//////////////////////
// Aggregate Struct //
//////////////////////

public struct AggStruct {
    var lhs: Klass
    var center: Builtin.Int32
    var rhs: Klass
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

public func aggStructSimpleNonConsumingUseTest(_ x: AggStruct) {
    @_noImplicitCopy let x2 = x
    aggStructUseMoveOnlyWithoutEscaping(x2)
}

public func aggStructMultipleNonConsumingUseTest(_ x: AggStruct) {
    @_noImplicitCopy let x2 = x
    aggStructUseMoveOnlyWithoutEscaping(x2)
    aggStructUseMoveOnlyWithoutEscaping(x2)
    print(x2)
}

public func aggStructUseAfterConsume(_ x: AggStruct) {
    @_noImplicitCopy let x2 = x // expected-error {{'x2' consumed more than once}}
    aggStructUseMoveOnlyWithoutEscaping(x2)
    aggStructConsume(x2) // expected-note {{consuming use}}
    print(x2) // expected-note {{consuming use}}
}

public func aggStructDoubleConsume(_ x: AggStruct) {
    @_noImplicitCopy let x2 = x  // expected-error {{'x2' consumed more than once}}
    aggStructConsume(x2) // expected-note {{consuming use}}
    aggStructConsume(x2) // expected-note {{consuming use}}
}

public func aggStructLoopConsume(_ x: AggStruct) {
    @_noImplicitCopy let x2 = x // expected-error {{'x2' consumed more than once}}
    for _ in 0..<1024 {
        aggStructConsume(x2) // expected-note {{consuming use}}
    }
}

public func aggStructDiamond(_ x: AggStruct) {
    @_noImplicitCopy let x2 = x
    if boolValue {
        aggStructConsume(x2)
    } else {
        aggStructConsume(x2)
    }
}

public func aggStructDiamondInLoop(_ x: AggStruct) {
    @_noImplicitCopy let x2 = x // expected-error {{'x2' consumed more than once}}
    for _ in 0..<1024 {
      if boolValue {
          aggStructConsume(x2) // expected-note {{consuming use}}
      } else {
          aggStructConsume(x2) // expected-note {{consuming use}}
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
