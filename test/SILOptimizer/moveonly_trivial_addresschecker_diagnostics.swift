// RUN: %target-swift-emit-sil -sil-verify-all -verify %s

//////////////////
// Declarations //
//////////////////

var boolValue: Bool { return true }

@_moveOnly
public struct NonTrivialStruct {
    var i: Int = 0
}

public func borrowVal(_ x: borrowing Int) {}
public func borrowVal(_ x: borrowing MoveOnlyInt) {}
public func borrowVal(_ x: borrowing NonTrivialStruct) {}
public func borrowVal(_ x: borrowing AggStruct) {}
public func borrowVal(_ x: borrowing AggGenericStruct<String>) {}
public func borrowVal<T>(_ x: borrowing AggGenericStruct<T>) {}
public func borrowVal(_ x: borrowing EnumTy) {}

public func consumeVal(_ x: __owned Int) {}
public func consumeVal(_ x: __owned MoveOnlyInt) {}
public func consumeVal(_ x: __owned NonTrivialStruct) {}
public func consumeVal(_ x: __owned EnumTy) {}
public func consumeVal(_ x: __owned AggStruct) {}
public func consumeVal(_ x: __owned AggGenericStruct<String>) {}
public func consumeVal<T>(_ x: __owned AggGenericStruct<T>) {}

@_moveOnly
public enum NonTrivialEnum {
    case first
    case second(Int)
    case third(NonTrivialStruct)
}

///////////
// Tests //
///////////

//////////////////////
// Aggregate Struct //
//////////////////////

@_moveOnly
public struct MoveOnlyInt {
    var value: Int
}

@_moveOnly
public struct KlassPair {
    var lhs: MoveOnlyInt
    var rhs: Int
}

@_moveOnly
public struct AggStruct {
    var lhs = MoveOnlyInt(value: 5)
    var center: Int = 6
    var rhs: Int = 7
    var pair = KlassPair(lhs: MoveOnlyInt(value: 1), rhs: 2)

    init() {}

    // Testing that DI ignores normal init. We also get an error on our return
    // value from the function since we do not reinitialize self.
    //
    // TODO: Improve error message!
    init(myInit: Int) { // expected-error {{'self' consumed more than once}}
        let x = self // expected-note {{consumed here}}
        let _ = x
    } // expected-note {{consumed again here}}

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
        let x = self.pair // expected-note {{consumed here}}
        let _ = x
    } // expected-note {{consumed again here}}

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
        self.lhs = MoveOnlyInt(value: 5)
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
    // expected-error @-2 {{missing reinitialization of inout parameter 'x2' after consume}}
    var y2 = x2 // expected-note {{consumed here}}
    y2 = x2 // expected-note {{consumed here}}
    // expected-note @-1 {{consumed again here}}
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

public func aggStructMultipleNonConsumingUseTestArg(_ x2: inout AggStruct) { // expected-error {{missing reinitialization of inout parameter 'x2' after consume}}
    borrowVal(x2)
    borrowVal(x2)
    consumeVal(x2) // expected-note {{consumed here}}
}

public func aggStructUseAfterConsume() {
    var x2 = AggStruct() // expected-error {{'x2' consumed more than once}}
    x2 = AggStruct()
    borrowVal(x2)
    consumeVal(x2) // expected-note {{consumed here}}
    consumeVal(x2) // expected-note {{consumed again here}}
}

public func aggStructUseAfterConsumeArg(_ x2: inout AggStruct) {
    // expected-error @-1 {{'x2' consumed more than once}}
    // expected-error @-2 {{missing reinitialization of inout parameter 'x2' after consume}}
    borrowVal(x2)
    consumeVal(x2) // expected-note {{consumed here}}
    consumeVal(x2) // expected-note {{consumed here}}
              // expected-note @-1 {{consumed again here}}
}

public func aggStructDoubleConsume() {
    var x2 = AggStruct()  // expected-error {{'x2' consumed more than once}}
    x2 = AggStruct()
    consumeVal(x2) // expected-note {{consumed here}}
    consumeVal(x2) // expected-note {{consumed again here}}
}

public func aggStructDoubleConsumeArg(_ x2: inout AggStruct) {
    // expected-error @-1 {{missing reinitialization of inout parameter 'x2' after consume}}
    // expected-error @-2 {{'x2' consumed more than once}}
    consumeVal(x2) // expected-note {{consumed here}}
    consumeVal(x2) // expected-note {{consumed here}}
    // expected-note @-1 {{consumed again here}}
}

public func aggStructLoopConsume() {
    var x2 = AggStruct() // expected-error {{'x2' consumed in a loop}}
    x2 = AggStruct()
    for _ in 0..<1024 {
        consumeVal(x2) // expected-note {{consumed here}}
    }
}

public func aggStructLoopConsumeArg(_ x2: inout AggStruct) { // expected-error {{missing reinitialization of inout parameter 'x2' after consume}}
    for _ in 0..<1024 {
        consumeVal(x2) // expected-note {{consumed here}}
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
    // expected-error @-1 {{missing reinitialization of inout parameter 'x2' after consume}}
    // expected-error @-2 {{missing reinitialization of inout parameter 'x2' after consume}}
    if boolValue {
        consumeVal(x2) // expected-note {{consumed here}}
    } else {
        consumeVal(x2) // expected-note {{consumed here}}
    }
}

public func aggStructDiamondInLoop() {
    var x2 = AggStruct()
    // expected-error @-1 {{'x2' consumed in a loop}}
    // expected-error @-2 {{'x2' consumed more than once}}
    x2 = AggStruct()
    for _ in 0..<1024 {
      if boolValue {
          consumeVal(x2) // expected-note {{consumed here}}
      } else {
          consumeVal(x2) // expected-note {{consumed here}}
          // expected-note @-1 {{consumed again here}}
      }
    }
}

public func aggStructDiamondInLoopArg(_ x2: inout AggStruct) {
    // expected-error @-1 {{missing reinitialization of inout parameter 'x2' after consume}}
    // expected-error @-2 {{missing reinitialization of inout parameter 'x2' after consume}}
    for _ in 0..<1024 {
      if boolValue {
          consumeVal(x2) // expected-note {{consumed here}}
      } else {
          consumeVal(x2) // expected-note {{consumed here}}
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
    var x2 = AggStruct() // expected-error {{'x2' consumed in a loop}}
    // expected-error @-1 {{'x2' consumed more than once}}
    x2 = AggStruct()
    consumeVal(x2.lhs) // expected-note {{consumed here}}
    for _ in 0..<1024 {
        consumeVal(x2.lhs) // expected-note {{consumed here}}
        // expected-note @-1 {{consumed again here}}
    }
}

public func aggStructConsumeFieldArg(_ x2: inout AggStruct) {
    // expected-error @-1 {{missing reinitialization of inout parameter 'x2' after consume}}
    // expected-error @-2 {{missing reinitialization of inout parameter 'x2' after consume}}
    consumeVal(x2.lhs) // expected-note {{consumed here}}
    for _ in 0..<1024 {
        consumeVal(x2.lhs) // expected-note {{consumed here}}
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
    var x2 = AggStruct() // expected-error {{'x2' consumed in a loop}}
    // expected-error @-1 {{'x2' consumed more than once}}
    x2 = AggStruct()
    consumeVal(x2.pair.lhs) // expected-note {{consumed here}}
    for _ in 0..<1024 {
        consumeVal(x2.pair.lhs) // expected-note {{consumed here}}
        // expected-note @-1 {{consumed again here}}
    }
}

public func aggStructConsumeGrandFieldArg(_ x2: inout AggStruct) {
    // expected-error @-1 {{missing reinitialization of inout parameter 'x2' after consume}}
    // expected-error @-2 {{missing reinitialization of inout parameter 'x2' after consume}}
    consumeVal(x2.pair.lhs) // expected-note {{consumed here}}
    for _ in 0..<1024 {
        consumeVal(x2.pair.lhs) // expected-note {{consumed here}}
    }
}

//////////////////////////////
// Aggregate Generic Struct //
//////////////////////////////

@_moveOnly
public struct AggGenericStruct<T> { // FIXME: this generic parameter should probably be used for better coverage.
    var lhs = MoveOnlyInt(value: 5)
    var rhs: UnsafeRawPointer? = nil
    var pair = KlassPair(lhs: MoveOnlyInt(value: 5), rhs: 6)
    var ptr2: UnsafePointer<T>? = nil

    init() {}


    // Testing that DI ignores normal init. We also get an error on our return
    // value from the function since we do not reinitialize self.
    //
    // TODO: Improve error message!
    init(myInit: UnsafeRawPointer) { // expected-error {{'self' consumed more than once}}
        let x = self // expected-note {{consumed here}}
        let _ = x
    } // expected-note {{consumed again here}}

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
        let x = self.lhs // expected-note {{consumed here}}
        let _ = x
    } // expected-note {{consumed again here}}

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
        self.lhs = MoveOnlyInt(value: 5)
    }
}

public func aggGenericStructSimpleChainTest() {
    var x2 = AggGenericStruct<String>()
    x2 = AggGenericStruct<String>()
    let y2 = x2
    let k2 = y2
    borrowVal(k2)
}

public func aggGenericStructSimpleChainTestArg(_ x2: inout AggGenericStruct<String>) { // expected-error {{missing reinitialization of inout parameter 'x2' after consume}}
    let y2 = x2 // expected-note {{consumed here}}
    let k2 = y2
    borrowVal(k2)
}

public func aggGenericStructSimpleNonConsumingUseTest() {
    var x2 = AggGenericStruct<String>()
    x2 = AggGenericStruct<String>()
    borrowVal(x2)
}

public func aggGenericStructSimpleNonConsumingUseTestArg(_ x2: inout AggGenericStruct<String>) {
    borrowVal(x2)
}

public func aggGenericStructMultipleNonConsumingUseTest() {
    var x2 = AggGenericStruct<String>()
    x2 = AggGenericStruct<String>()
    borrowVal(x2)
    borrowVal(x2)
    consumeVal(x2)
}

public func aggGenericStructMultipleNonConsumingUseTestArg(_ x2: inout AggGenericStruct<String>) { // expected-error {{missing reinitialization of inout parameter 'x2' after consume}}
    borrowVal(x2)
    borrowVal(x2)
    consumeVal(x2) // expected-note {{consumed here}}
}

public func aggGenericStructUseAfterConsume() {
    var x2 = AggGenericStruct<String>() // expected-error {{'x2' consumed more than once}}
    x2 = AggGenericStruct<String>()
    borrowVal(x2)
    consumeVal(x2) // expected-note {{consumed here}}
    consumeVal(x2) // expected-note {{consumed again here}}
}

public func aggGenericStructUseAfterConsumeArg(_ x2: inout AggGenericStruct<String>) {
    // expected-error @-1 {{missing reinitialization of inout parameter 'x2' after consume}}
    // expected-error @-2 {{'x2' consumed more than once}}
    borrowVal(x2)
    consumeVal(x2) // expected-note {{consumed here}}
    consumeVal(x2) // expected-note {{consumed here}}
    // expected-note @-1 {{consumed again here}}x
}

public func aggGenericStructDoubleConsume() {
    var x2 = AggGenericStruct<String>()  // expected-error {{'x2' consumed more than once}}
    x2 = AggGenericStruct<String>()
    consumeVal(x2) // expected-note {{consumed here}}
    consumeVal(x2) // expected-note {{consumed again here}}
}

public func aggGenericStructDoubleConsumeArg(_ x2: inout AggGenericStruct<String>) {
    // expected-error @-1 {{missing reinitialization of inout parameter 'x2' after consume}}
    // expected-error @-2 {{'x2' consumed more than once}}
    consumeVal(x2) // expected-note {{consumed here}}
    consumeVal(x2) // expected-note {{consumed here}}
    // expected-note @-1 {{consumed again here}}
}

public func aggGenericStructLoopConsume() {
    var x2 = AggGenericStruct<String>() // expected-error {{'x2' consumed in a loop}}
    x2 = AggGenericStruct<String>()
    for _ in 0..<1024 {
        consumeVal(x2) // expected-note {{consumed here}}
    }
}

public func aggGenericStructLoopConsumeArg(_ x2: inout AggGenericStruct<String>) { // expected-error {{missing reinitialization of inout parameter 'x2' after consume}}
    for _ in 0..<1024 {
        consumeVal(x2) // expected-note {{consumed here}}
    }
}

public func aggGenericStructDiamond() {
    var x2 = AggGenericStruct<String>()
    x2 = AggGenericStruct<String>()
    if boolValue {
        consumeVal(x2)
    } else {
        consumeVal(x2)
    }
}

public func aggGenericStructDiamondArg(_ x2: inout AggGenericStruct<String>) {
    // expected-error @-1 {{missing reinitialization of inout parameter 'x2' after consume}}
    // expected-error @-2 {{missing reinitialization of inout parameter 'x2' after consume}}
    if boolValue {
        consumeVal(x2) // expected-note {{consumed here}}
    } else {
        consumeVal(x2) // expected-note {{consumed here}}
    }
}

public func aggGenericStructDiamondInLoop() {
    var x2 = AggGenericStruct<String>() // expected-error {{'x2' consumed in a loop}}
    // expected-error @-1 {{'x2' consumed more than once}}
    x2 = AggGenericStruct<String>()
    for _ in 0..<1024 {
      if boolValue {
          consumeVal(x2) // expected-note {{consumed here}}
      } else {
          consumeVal(x2) // expected-note {{consumed here}}
          // expected-note @-1 {{consumed again here}}
      }
    }
}

public func aggGenericStructDiamondInLoopArg(_ x2: inout AggGenericStruct<String>) {
    // expected-error @-1 {{missing reinitialization of inout parameter 'x2' after consume}}
    // expected-error @-2 {{missing reinitialization of inout parameter 'x2' after consume}}
    for _ in 0..<1024 {
      if boolValue {
          consumeVal(x2) // expected-note {{consumed here}}
      } else {
          consumeVal(x2) // expected-note {{consumed here}}
      }
    }
}

public func aggGenericStructAccessField() {
    var x2 = AggGenericStruct<String>()
    x2 = AggGenericStruct<String>()
    borrowVal(x2.lhs)
    for _ in 0..<1024 {
        borrowVal(x2.lhs)
    }
}

public func aggGenericStructAccessFieldArg(_ x2: inout AggGenericStruct<String>) {
    borrowVal(x2.lhs)
    for _ in 0..<1024 {
        borrowVal(x2.lhs)
    }
}

public func aggGenericStructConsumeField() {
    var x2 = AggGenericStruct<String>() // expected-error {{'x2' consumed in a loop}}
    // expected-error @-1 {{'x2' consumed more than once}}
    x2 = AggGenericStruct<String>()
    consumeVal(x2.lhs) // expected-note {{consumed here}}
    for _ in 0..<1024 {
        consumeVal(x2.lhs) // expected-note {{consumed here}}
        // expected-note @-1 {{consumed again here}}
    }
}

public func aggGenericStructConsumeFieldArg(_ x2: inout AggGenericStruct<String>) {
    // expected-error @-1 {{missing reinitialization of inout parameter 'x2' after consume}}
    // expected-error @-2 {{missing reinitialization of inout parameter 'x2' after consume}}
    consumeVal(x2.lhs) // expected-note {{consumed here}}
    for _ in 0..<1024 {
        consumeVal(x2.lhs) // expected-note {{consumed here}}
    }
}

public func aggGenericStructAccessGrandField() {
    var x2 = AggGenericStruct<String>()
    x2 = AggGenericStruct<String>()
    borrowVal(x2.pair.lhs)
    for _ in 0..<1024 {
        borrowVal(x2.pair.lhs)
    }
}

public func aggGenericStructAccessGrandFieldArg(_ x2: inout AggGenericStruct<String>) {
    borrowVal(x2.pair.lhs)
    for _ in 0..<1024 {
        borrowVal(x2.pair.lhs)
    }
}

public func aggGenericStructConsumeGrandField() {
    var x2 = AggGenericStruct<String>() // expected-error {{'x2' consumed in a loop}}
    // expected-error @-1 {{'x2' consumed more than once}}
    x2 = AggGenericStruct<String>()
    consumeVal(x2.pair.lhs) // expected-note {{consumed here}}
    for _ in 0..<1024 {
        consumeVal(x2.pair.lhs) // expected-note {{consumed here}}
        // expected-note @-1 {{consumed again here}}
    }
}

public func aggGenericStructConsumeGrandField2() {
    var x2 = AggGenericStruct<String>() // expected-error {{'x2' consumed more than once}}
    x2 = AggGenericStruct<String>()
    consumeVal(x2.pair.lhs) // expected-note {{consumed here}}
    for _ in 0..<1024 {
    }
    consumeVal(x2.pair.lhs) // expected-note {{consumed again here}}
}

public func aggGenericStructConsumeGrandFieldArg(_ x2: inout AggGenericStruct<String>) {
    // expected-error @-1 {{missing reinitialization of inout parameter 'x2' after consume}}
    // expected-error @-2 {{missing reinitialization of inout parameter 'x2' after consume}}
    consumeVal(x2.pair.lhs) // expected-note {{consumed here}}
    for _ in 0..<1024 {
        consumeVal(x2.pair.lhs) // expected-note {{consumed here}}
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

public func aggGenericStructSimpleChainTestArg<T>(_ x2: inout AggGenericStruct<T>) { // expected-error {{missing reinitialization of inout parameter 'x2' after consume}}
    let y2 = x2 // expected-note {{consumed here}}
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

public func aggGenericStructMultipleNonConsumingUseTestArg<T>(_ x2: inout AggGenericStruct<T>) { //expected-error {{missing reinitialization of inout parameter 'x2' after consume}}
    borrowVal(x2)
    borrowVal(x2)
    consumeVal(x2) // expected-note {{consumed here}}
}

public func aggGenericStructUseAfterConsume<T>(_ x: T.Type) {
    var x2 = AggGenericStruct<T>() // expected-error {{'x2' consumed more than once}}
    x2 = AggGenericStruct<T>()
    borrowVal(x2)
    consumeVal(x2) // expected-note {{consumed here}}
    consumeVal(x2) // expected-note {{consumed again here}}
}

public func aggGenericStructUseAfterConsumeArg<T>(_ x2: inout AggGenericStruct<T>) {
    // expected-error @-1 {{missing reinitialization of inout parameter 'x2' after consume}}
    // expected-error @-2 {{'x2' consumed more than once}}
    borrowVal(x2)
    consumeVal(x2) // expected-note {{consumed here}}
    consumeVal(x2) // expected-note {{consumed here}}
    // expected-note @-1 {{consumed again here}}
}

public func aggGenericStructDoubleConsume<T>(_ x: T.Type) {
    var x2 = AggGenericStruct<T>() // expected-error {{'x2' consumed more than once}}
    x2 = AggGenericStruct<T>()
    consumeVal(x2) // expected-note {{consumed here}}
    consumeVal(x2) // expected-note {{consumed again here}}
}

public func aggGenericStructDoubleConsumeArg<T>(_ x2: inout AggGenericStruct<T>) {
    // expected-error @-1 {{missing reinitialization of inout parameter 'x2' after consume}}
    // expected-error @-2 {{'x2' consumed more than once}}
    consumeVal(x2) // expected-note {{consumed here}}
    consumeVal(x2) // expected-note {{consumed here}}
    // expected-note @-1 {{consumed again here}}
}

public func aggGenericStructLoopConsume<T>(_ x: T.Type) {
    var x2 = AggGenericStruct<T>() // expected-error {{'x2' consumed in a loop}}
    x2 = AggGenericStruct<T>()
    for _ in 0..<1024 {
        consumeVal(x2) // expected-note {{consumed here}}
    }
}

public func aggGenericStructLoopConsumeArg<T>(_ x2: inout AggGenericStruct<T>) { // expected-error {{missing reinitialization of inout parameter 'x2' after consume}}
    for _ in 0..<1024 {
        consumeVal(x2) // expected-note {{consumed here}}
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
    // expected-error @-1 {{missing reinitialization of inout parameter 'x2' after consume}}
    // expected-error @-2 {{missing reinitialization of inout parameter 'x2' after consume}}
    if boolValue {
        consumeVal(x2) // expected-note {{consumed here}}
    } else {
        consumeVal(x2) // expected-note {{consumed here}}
    }
}

public func aggGenericStructDiamondInLoop<T>(_ x: T.Type) {
    var x2 = AggGenericStruct<T>() // expected-error {{'x2' consumed more than once}}
    // expected-error @-1 {{'x2' consumed in a loop}}
    x2 = AggGenericStruct<T>()
    for _ in 0..<1024 {
      if boolValue {
          consumeVal(x2) // expected-note {{consumed here}}
      } else {
          consumeVal(x2) // expected-note {{consumed here}}
          // expected-note @-1 {{consumed again here}}
      }
    }
}

public func aggGenericStructDiamondInLoopArg<T>(_ x2: inout AggGenericStruct<T>) {
    // expected-error @-1 {{missing reinitialization of inout parameter 'x2' after consume}}
    // expected-error @-2 {{missing reinitialization of inout parameter 'x2' after consume}}
    for _ in 0..<1024 {
      if boolValue {
          consumeVal(x2) // expected-note {{consumed here}}
      } else {
          consumeVal(x2) // expected-note {{consumed here}}
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
    var x2 = AggGenericStruct<T>() // expected-error {{'x2' consumed in a loop}}
    // expected-error @-1 {{'x2' consumed more than once}}
    x2 = AggGenericStruct<T>()
    consumeVal(x2.lhs) // expected-note {{consumed here}}
    for _ in 0..<1024 {
        consumeVal(x2.lhs) // expected-note {{consumed here}}
        // expected-note @-1 {{consumed again here}}
    }
}

public func aggGenericStructConsumeFieldArg<T>(_ x2: inout AggGenericStruct<T>) {
    // expected-error @-1 {{missing reinitialization of inout parameter 'x2' after consume}}
    // expected-error @-2 {{missing reinitialization of inout parameter 'x2' after consume}}
    consumeVal(x2.lhs) // expected-note {{consumed here}}
    for _ in 0..<1024 {
        consumeVal(x2.lhs) // expected-note {{consumed here}}
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
    var x2 = AggGenericStruct<T>() // expected-error {{'x2' consumed in a loop}}
    // expected-error @-1 {{'x2' consumed more than once}}
    x2 = AggGenericStruct<T>()
    consumeVal(x2.pair.lhs) // expected-note {{consumed here}}
    for _ in 0..<1024 {
        consumeVal(x2.pair.lhs) // expected-note {{consumed here}}
        // expected-note @-1 {{consumed again here}}
    }
}

public func aggGenericStructConsumeGrandFieldArg<T>(_ x2: inout AggGenericStruct<T>) {
    // expected-error @-1 {{missing reinitialization of inout parameter 'x2' after consume}}
    // expected-error @-2 {{missing reinitialization of inout parameter 'x2' after consume}}
    consumeVal(x2.pair.lhs) // expected-note {{consumed here}}
    for _ in 0..<1024 {
        consumeVal(x2.pair.lhs) // expected-note {{consumed here}}
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

public func enumSimpleChainTest() {
    var x2 = EnumTy.klass(NonTrivialStruct())
    x2 = EnumTy.klass(NonTrivialStruct())
    let y2 = x2
    let k2 = y2
    borrowVal(k2)
}

public func enumSimpleChainTestArg(_ x2: inout EnumTy) { // expected-error {{missing reinitialization of inout parameter 'x2' after consume}}
    let y2 = x2 // expected-note {{consumed here}}
    let k2 = y2
    borrowVal(k2)
}

public func enumSimpleNonConsumingUseTest() {
    var x2 = EnumTy.klass(NonTrivialStruct())
    x2 = EnumTy.klass(NonTrivialStruct())
    borrowVal(x2)
}

public func enumSimpleNonConsumingUseTestArg(_ x2: inout EnumTy) {
    borrowVal(x2)
}

public func enumMultipleNonConsumingUseTest() {
    var x2 = EnumTy.klass(NonTrivialStruct())
    x2 = EnumTy.klass(NonTrivialStruct())
    borrowVal(x2)
    borrowVal(x2)
    consumeVal(x2)
}

public func enumMultipleNonConsumingUseTestArg(_ x2: inout EnumTy) { // expected-error {{missing reinitialization of inout parameter 'x2' after consume}}
    borrowVal(x2)
    borrowVal(x2)
    consumeVal(x2) // expected-note {{consumed here}}
}

public func enumUseAfterConsume() {
    var x2 = EnumTy.klass(NonTrivialStruct()) // expected-error {{'x2' consumed more than once}}
    x2 = EnumTy.klass(NonTrivialStruct())
    borrowVal(x2)
    consumeVal(x2) // expected-note {{consumed here}}
    consumeVal(x2) // expected-note {{consumed again here}}
}

public func enumUseAfterConsumeArg(_ x2: inout EnumTy) {
    // expected-error @-1 {{missing reinitialization of inout parameter 'x2' after consume}}
    // expected-error @-2 {{'x2' consumed more than once}}
    borrowVal(x2)
    consumeVal(x2) // expected-note {{consumed here}}
    consumeVal(x2) // expected-note {{consumed here}}
    // expected-note @-1 {{consumed again here}}
}

public func enumDoubleConsume() {
    var x2 = EnumTy.klass(NonTrivialStruct())  // expected-error {{'x2' consumed more than once}}
    x2 = EnumTy.klass(NonTrivialStruct())
    consumeVal(x2) // expected-note {{consumed here}}
    consumeVal(x2) // expected-note {{consumed again here}}
}

public func enumDoubleConsumeArg(_ x2: inout EnumTy) {
    // expected-error @-1 {{missing reinitialization of inout parameter 'x2' after consume}}
    // expected-error @-2 {{'x2' consumed more than once}} 
    consumeVal(x2) // expected-note {{consumed here}}
    consumeVal(x2) // expected-note {{consumed here}}
    // expected-note @-1 {{consumed again here}}
}

public func enumLoopConsume() {
    var x2 = EnumTy.klass(NonTrivialStruct()) // expected-error {{'x2' consumed in a loop}}
    x2 = EnumTy.klass(NonTrivialStruct())
    for _ in 0..<1024 {
        consumeVal(x2) // expected-note {{consumed here}}
    }
}

public func enumLoopConsumeArg(_ x2: inout EnumTy) { // expected-error {{missing reinitialization of inout parameter 'x2' after consume}}
    for _ in 0..<1024 {
        consumeVal(x2) // expected-note {{consumed here}}
    }
}

public func enumDiamond() {
    var x2 = EnumTy.klass(NonTrivialStruct())
    x2 = EnumTy.klass(NonTrivialStruct())
    if boolValue {
        consumeVal(x2)
    } else {
        consumeVal(x2)
    }
}

public func enumDiamondArg(_ x2: inout EnumTy) {
    // expected-error @-1 {{missing reinitialization of inout parameter 'x2' after consume}}
    // expected-error @-2 {{missing reinitialization of inout parameter 'x2' after consume}}
    if boolValue {
        consumeVal(x2) // expected-note {{consumed here}}
    } else {
        consumeVal(x2) // expected-note {{consumed here}}
    }
}

public func enumDiamondInLoop() {
    var x2 = EnumTy.klass(NonTrivialStruct())
    // expected-error @-1 {{'x2' consumed in a loop}}
    // expected-error @-2 {{'x2' consumed more than once}} 
    x2 = EnumTy.klass(NonTrivialStruct())
    for _ in 0..<1024 {
      if boolValue {
          consumeVal(x2) // expected-note {{consumed here}}
      } else {
          consumeVal(x2) // expected-note {{consumed here}}
          // expected-note @-1 {{consumed again here}}
      }
    }
}

public func enumDiamondInLoopArg(_ x2: inout EnumTy) {
    // expected-error @-1 {{missing reinitialization of inout parameter 'x2' after consume}}
    // expected-error @-2 {{missing reinitialization of inout parameter 'x2' after consume}}
    for _ in 0..<1024 {
      if boolValue {
          consumeVal(x2) // expected-note {{consumed here}}
      } else {
          consumeVal(x2) // expected-note {{consumed here}}
      }
    }
}

public func enumAssignToVar1() {
    var x2 = EnumTy.klass(NonTrivialStruct()) // expected-error {{'x2' consumed more than once}}
    x2 = EnumTy.klass(NonTrivialStruct())
    var x3 = x2 // expected-note {{consumed here}}
    x3 = x2 // expected-note {{consumed again here}}
    x3 = EnumTy.klass(NonTrivialStruct())
    consumeVal(x3)
}

public func enumAssignToVar1Arg(_ x2: inout EnumTy) {
    // expected-error @-1 {{missing reinitialization of inout parameter 'x2' after consume}}
    // expected-error @-2 {{'x2' consumed more than once}} 
                                                            
    var x3 = x2 // expected-note {{consumed here}}
    x3 = x2 // expected-note {{consumed here}}
    // expected-note @-1 {{consumed again here}}
    x3 = EnumTy.klass(NonTrivialStruct())
    consumeVal(x3)
}

public func enumAssignToVar2() {
    var x2 = EnumTy.klass(NonTrivialStruct()) // expected-error {{'x2' consumed more than once}}
    x2 = EnumTy.klass(NonTrivialStruct())
    var x3 = x2 // expected-note {{consumed here}}
    x3 = x2 // expected-note {{consumed again here}}
    borrowVal(x3)
}

public func enumAssignToVar2Arg(_ x2: inout EnumTy) {
    // expected-error @-1 {{missing reinitialization of inout parameter 'x2' after consume}}
    // expected-error @-2 {{'x2' consumed more than once}} 
    var x3 = x2 // expected-note {{consumed here}}
    x3 = x2 // expected-note {{consumed here}}
     // expected-note @-1 {{consumed again here}}
    borrowVal(x3)
}

public func enumAssignToVar3() {
    var x2 = EnumTy.klass(NonTrivialStruct())
    x2 = EnumTy.klass(NonTrivialStruct())
    var x3 = x2
    x3 = EnumTy.klass(NonTrivialStruct())
    consumeVal(x3)
}

public func enumAssignToVar3Arg(_ x2: inout EnumTy) { // expected-error {{missing reinitialization of inout parameter 'x2' after consume}}
                                                            
    var x3 = x2 // expected-note {{consumed here}}
    x3 = EnumTy.klass(NonTrivialStruct())
    consumeVal(x3)
}

public func enumAssignToVar4() {
    var x2 = EnumTy.klass(NonTrivialStruct()) // expected-error {{'x2' consumed more than once}}
    x2 = EnumTy.klass(NonTrivialStruct())
    let x3 = x2 // expected-note {{consumed here}}
    consumeVal(x2) // expected-note {{consumed again here}}
    consumeVal(x3)
}

public func enumAssignToVar4Arg(_ x2: inout EnumTy) {
    // expected-error @-1 {{missing reinitialization of inout parameter 'x2' after consume}}
    // expected-error @-2 {{'x2' consumed more than once}} 
    let x3 = x2 // expected-note {{consumed here}}
    consumeVal(x2) // expected-note {{consumed here}}
    // expected-note @-1 {{consumed again here}}
    consumeVal(x3)
}

public func enumAssignToVar5() {
    var x2 = EnumTy.klass(NonTrivialStruct()) // expected-error {{'x2' used after consume}}
    x2 = EnumTy.klass(NonTrivialStruct())
    var x3 = x2 // expected-note {{consumed here}}
    borrowVal(x2) // expected-note {{used here}}
    x3 = EnumTy.klass(NonTrivialStruct())
    consumeVal(x3)
}

public func enumAssignToVar5Arg(_ x2: inout EnumTy) { // expected-error {{'x2' used after consume}}
                                                            
    var x3 = x2 // expected-note {{consumed here}}
    borrowVal(x2) // expected-note {{used here}}
    x3 = EnumTy.klass(NonTrivialStruct())
    consumeVal(x3)
}

public func enumPatternMatchIfLet1() {
    var x2 = EnumTy.klass(NonTrivialStruct()) // expected-error {{'x2' consumed more than once}}
    x2 = EnumTy.klass(NonTrivialStruct())
    if case let EnumTy.klass(x) = consume x2 { // expected-note {{consumed here}}
        borrowVal(x)
    }
    if case let EnumTy.klass(x) = consume x2 { // expected-note {{consumed again here}}
        borrowVal(x)
    }
}

public func enumPatternMatchIfLet1Arg(_ x2: inout EnumTy) {
    // expected-error @-1 {{missing reinitialization of inout parameter 'x2' after consume}}
    // expected-error @-2 {{'x2' consumed more than once}}
    if case let EnumTy.klass(x) = consume x2 { // expected-note {{consumed here}}
        borrowVal(x)
    }
    if case let EnumTy.klass(x) = consume x2 { // expected-note {{consumed here}}
        // expected-note @-1 {{consumed again here}}
        borrowVal(x)
    }
}

public func enumPatternMatchIfLet2() {
    var x2 = EnumTy.klass(NonTrivialStruct()) // expected-error {{'x2' consumed in a loop}}
    x2 = EnumTy.klass(NonTrivialStruct())
    for _ in 0..<1024 {
        if case let EnumTy.klass(x) = consume x2 {  // expected-note {{consumed here}}
            borrowVal(x)
        }
    }
}

public func enumPatternMatchIfLet2Arg(_ x2: inout EnumTy) { // expected-error {{missing reinitialization of inout parameter 'x2' after consume}}
    for _ in 0..<1024 {
        if case let EnumTy.klass(x) = consume x2 {  // expected-note {{consumed here}}
            borrowVal(x)
        }
    }
}

// This is wrong.
public func enumPatternMatchSwitch1() {
    var x2 = EnumTy.klass(NonTrivialStruct()) // expected-error {{'x2' used after consume}}
    x2 = EnumTy.klass(NonTrivialStruct())
    switch consume x2 { // expected-note {{consumed here}}
    case let EnumTy.klass(k):
        borrowVal(k)
        // This should be flagged as the use after free use. We are atleast
        // erroring though.
        borrowVal(x2) // expected-note {{used here}}
    case .int:
        break
    }
}

public func enumPatternMatchSwitch1Arg(_ x2: inout EnumTy) { // expected-error {{missing reinitialization of inout parameter 'x2' after consume}}
    switch consume x2 { // expected-note {{consumed here}}
    case let EnumTy.klass(k):
        borrowVal(k)
        // This should be flagged as the use after free use. We are atleast
        // erroring though.
        borrowVal(x2)
    case .int:
        break
    }
}

public func enumPatternMatchSwitch2() {
    var x2 = EnumTy.klass(NonTrivialStruct())
    x2 = EnumTy.klass(NonTrivialStruct())
    switch consume x2 {
    case let EnumTy.klass(k):
        borrowVal(k)
    case .int:
        break
    }
}

public func enumPatternMatchSwitch2Arg(_ x2: inout EnumTy) { // expected-error {{missing reinitialization of inout parameter 'x2' after consume}}
    switch consume x2 { // expected-note {{consumed here}}
    case let EnumTy.klass(k):
        borrowVal(k)
    case .int:
        break
    }
}

// QOI: We can do better here. We should also flag x2
public func enumPatternMatchSwitch2WhereClause() {
    var x2 = EnumTy.klass(NonTrivialStruct()) // expected-error {{'x2' used after consume}}
    x2 = EnumTy.klass(NonTrivialStruct())
    switch consume x2 { // expected-note {{consumed here}}
    case let EnumTy.klass(k)
           where x2.doSomething(): // expected-note {{used here}}
        borrowVal(k)
    case .int:
        break
    case EnumTy.klass:
        break
    }
}

public func enumPatternMatchSwitch2WhereClauseArg(_ x2: inout EnumTy) { // expected-error {{missing reinitialization of inout parameter 'x2' after consume}}
    switch consume x2 { // expected-note {{consumed here}}
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
    var x2 = EnumTy.klass(NonTrivialStruct())
    x2 = EnumTy.klass(NonTrivialStruct())
    switch consume x2 {
    case let EnumTy.klass(k)
           where boolValue:
        borrowVal(k)
    case .int:
        break
    case EnumTy.klass:
        break
    }
}

public func enumPatternMatchSwitch2WhereClause2Arg(_ x2: inout EnumTy) { // expected-error {{missing reinitialization of inout parameter 'x2' after consume}}
    switch consume x2 { // expected-note {{consumed here}}
    case let EnumTy.klass(k)
           where boolValue:
        borrowVal(k)
    case .int:
        break
    case EnumTy.klass:
        break
    }
}

/////////////////////////////
// Closure and Defer Tests //
/////////////////////////////

public func closureClassUseAfterConsume1() {
    let f = {
        var x2 = NonTrivialStruct() // expected-error {{'x2' consumed more than once}}
        x2 = NonTrivialStruct()
        borrowVal(x2)
        consumeVal(x2) // expected-note {{consumed here}}
        consumeVal(x2) // expected-note {{consumed again here}}
    }
    f()
}

public func closureClassUseAfterConsume2() {
    let f = { () in
        var x2 = NonTrivialStruct() // expected-error {{'x2' consumed more than once}}
        x2 = NonTrivialStruct()
        borrowVal(x2)
        consumeVal(x2) // expected-note {{consumed here}}
        consumeVal(x2) // expected-note {{consumed again here}}
    }
    f()
}

public func closureClassUseAfterConsumeArg(_ argX: inout NonTrivialStruct) {
    // TODO: Fix this
    let f = { (_ x2: inout NonTrivialStruct) in
        // expected-error @-1 {{missing reinitialization of inout parameter 'x2' after consume}}
        // expected-error @-2 {{'x2' consumed more than once}}
        borrowVal(x2)
        consumeVal(x2) // expected-note {{consumed here}}
        consumeVal(x2) // expected-note {{consumed here}}
        // expected-note @-1 {{consumed again here}}
    }
    f(&argX)
}

// We do not support captures of vars by closures today.
public func closureCaptureClassUseAfterConsume() {
    var x2 = NonTrivialStruct()
    // expected-error @-1 {{missing reinitialization of closure capture 'x2' after consume}}
    // expected-error @-2 {{'x2' consumed more than once}}
    x2 = NonTrivialStruct()
    let f = {
        borrowVal(x2)
        consumeVal(x2) // expected-note {{consumed here}}
        consumeVal(x2) // expected-note {{consumed here}}
        // expected-note @-1 {{consumed again here}}
    }
    f()
}

public func closureCaptureClassUseAfterConsumeError() {
    var x2 = NonTrivialStruct()
    // expected-error @-1 {{'x2' consumed more than once}}
    // expected-error @-2 {{'x2' consumed more than once}}
    // expected-error @-3 {{missing reinitialization of closure capture 'x2' after consume}}
    // expected-error @-4 {{'x2' consumed more than once}}
    // expected-error @-5 {{'x2' consumed more than once}}
    x2 = NonTrivialStruct()
    let f = {
        borrowVal(x2)
        consumeVal(x2) // expected-note {{consumed here}}
        consumeVal(x2) // expected-note {{consumed here}}
        // expected-note @-1 {{consumed again here}}
    }
    f()
    let x3 = x2 // expected-note {{consumed here}}
    consumeVal(x2) // expected-note {{consumed here}}
    // expected-note @-1 {{consumed again here}}
    var x4 = x2 // expected-note {{consumed here}}
    // expected-note @-1 {{consumed again here}}
    x4 = x2 // expected-note {{consumed again here}}
    _ = x4
    let _ = x3
}

public func closureCaptureClassArgUseAfterConsume(_ x2: inout NonTrivialStruct) {
    // expected-note @-1 {{'x2' is declared 'inout'}}
    let f = {
        // expected-error @-1 {{escaping closure captures 'inout' parameter 'x2'}}
        borrowVal(x2) // expected-note {{captured here}}
        consumeVal(x2) // expected-note {{captured here}}
        consumeVal(x2) // expected-note {{captured here}}
    }
    f()
}

// TODO: Improve error msg here to make it clear the use is due to the defer.
public func deferCaptureClassUseAfterConsume() {
    var x2 = NonTrivialStruct()
    // expected-error @-1 {{missing reinitialization of closure capture 'x2' after consume}}
    // expected-error @-2 {{'x2' consumed more than once}}
    // expected-error @-3 {{'x2' used after consume}}
    x2 = NonTrivialStruct()
    defer { // expected-note {{used here}}
        borrowVal(x2)
        consumeVal(x2) // expected-note {{consumed here}}
        consumeVal(x2) // expected-note {{consumed here}}
        // expected-note @-1 {{consumed again here}}
    }
    consumeVal(x2) // expected-note {{consumed here}}
}

public func deferCaptureClassUseAfterConsume2() {
    var x2 = NonTrivialStruct()
    // expected-error @-1 {{missing reinitialization of closure capture 'x2' after consume}}
    // expected-error @-2 {{'x2' consumed more than once}}
    // expected-error @-3 {{'x2' used after consume}}
    x2 = NonTrivialStruct()
    defer { //  expected-note {{used here}}
        borrowVal(x2)
        consumeVal(x2) // expected-note {{consumed here}}
        consumeVal(x2) // expected-note {{consumed here}}
        // expected-note @-1 {{consumed again here}}
    }
    let x3 = x2 // expected-note {{consumed here}}
    let _ = x3
}

public func deferCaptureClassArgUseAfterConsume(_ x2: inout NonTrivialStruct) {
    // expected-error @-1 {{missing reinitialization of closure capture 'x2' after consume}}
    // expected-error @-2 {{'x2' consumed more than once}}
    borrowVal(x2)
    defer {
        borrowVal(x2)
        consumeVal(x2) // expected-note {{consumed here}}
        consumeVal(x2) // expected-note {{consumed here}}
        // expected-note @-1 {{consumed again here}}
    }
    print("foo")
}

public func closureAndDeferCaptureClassUseAfterConsume() {
    var x2 = NonTrivialStruct()
    // expected-error @-1 {{missing reinitialization of closure capture 'x2' after consume}}
    // expected-error @-2 {{'x2' consumed more than once}}
    x2 = NonTrivialStruct()
    let f = {
        defer {
            borrowVal(x2)
            consumeVal(x2) // expected-note {{consumed here}}
            consumeVal(x2)
            // expected-note @-1 {{consumed here}}
            // expected-note @-2 {{consumed again here}}
        }
        print("foo")
    }
    f()
}

public func closureAndDeferCaptureClassUseAfterConsume2() {
    var x2 = NonTrivialStruct()
    // expected-error @-1 {{missing reinitialization of closure capture 'x2' after consume}}
    // expected-error @-2 {{'x2' consumed more than once}}
    // expected-error @-3 {{'x2' used after consume}}
    x2 = NonTrivialStruct()
    let f = {
        consumeVal(x2) // expected-note {{consumed here}}
        defer { // expected-note {{used here}}
            borrowVal(x2)
            consumeVal(x2) // expected-note {{consumed here}}
            consumeVal(x2)
            // expected-note @-1 {{consumed here}}
            // expected-note @-2 {{consumed again here}}
        }
        print("foo")
    }
    f()
}

public func closureAndDeferCaptureClassUseAfterConsume3() {
    var x2 = NonTrivialStruct()
    // expected-error @-1 {{missing reinitialization of closure capture 'x2' after consume}}
    // expected-error @-2 {{'x2' consumed more than once}}
    // expected-error @-3 {{'x2' used after consume}}
    x2 = NonTrivialStruct()
    let f = {
        consumeVal(x2) // expected-note {{consumed here}}
        defer { // expected-note {{used here}}
            borrowVal(x2)
            consumeVal(x2) // expected-note {{consumed here}}
            consumeVal(x2)
            // expected-note @-1 {{consumed here}}
            // expected-note @-2 {{consumed again here}}
        }
        print("foo")
    }
    f()
    consumeVal(x2)
}

public func closureAndDeferCaptureClassArgUseAfterConsume(_ x2: inout NonTrivialStruct) {
    // expected-error @-1 {{missing reinitialization of closure capture 'x2' after consume}}
    // expected-error @-2 {{'x2' consumed more than once}}
    // expected-note @-3 {{'x2' is declared 'inout'}}
    let f = { // expected-error {{escaping closure captures 'inout' parameter 'x2'}}
        defer { // expected-note {{captured indirectly by this call}}
            borrowVal(x2) // expected-note {{captured here}}
            consumeVal(x2) // expected-note {{captured here}}
            // expected-note @-1 {{consumed here}}
            consumeVal(x2) // expected-note {{captured here}}
            // expected-note @-1 {{consumed here}}
            // expected-note @-2 {{consumed again here}}
        }
        print("foo")
    }
    f()
}

public func closureAndClosureCaptureClassUseAfterConsume() {
    var x2 = NonTrivialStruct()
    // expected-error @-1 {{'x2' consumed more than once}}
    // expected-error @-2 {{missing reinitialization of closure capture 'x2' after consume}}
    x2 = NonTrivialStruct()
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

public func closureAndClosureCaptureClassUseAfterConsume2() {
    var x2 = NonTrivialStruct()
    // expected-error @-1 {{'x2' consumed more than once}}
    // expected-error @-2 {{missing reinitialization of closure capture 'x2' after consume}}
    x2 = NonTrivialStruct()
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

public func closureAndClosureCaptureClassUseAfterConsume3() {
    var x2 = NonTrivialStruct()
    // expected-error @-1 {{'x2' consumed more than once}}
    // expected-error @-2 {{missing reinitialization of closure capture 'x2' after consume}}
    // expected-error @-3 {{'x2' used after consume}}
    x2 = NonTrivialStruct()
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

public func closureAndClosureCaptureClassArgUseAfterConsume(_ x2: inout NonTrivialStruct) {
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
