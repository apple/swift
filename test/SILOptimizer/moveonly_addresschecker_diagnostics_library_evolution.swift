// RUN: %target-swift-emit-sil -enable-experimental-feature NoImplicitCopy -sil-verify-all -verify -enable-library-evolution -enable-experimental-feature MoveOnlyResilientTypes %s

// This test is used to validate that we properly handle library evolution code
// until we can get all of the normal moveonly_addresschecker_diagnostics test
// case to pass.

////////////////////////
// MARK: Declarations //
////////////////////////

@_moveOnly public struct EmptyStruct {}
@_moveOnly public struct NonEmptyStruct {
    var e = EmptyStruct()
}
public class CopyableKlass {
    var varS = NonEmptyStruct()
    var letS = NonEmptyStruct()
}

public struct CopyableStruct {
    var k = CopyableKlass()
}

public func borrowVal(_ x: borrowing NonEmptyStruct) {}
public func borrowVal(_ x: borrowing EmptyStruct) {}
public func borrowVal(_ x: borrowing CopyableKlass) {}
public func borrowVal(_ x: borrowing CopyableStruct) {}
public func consumeVal(_ x: consuming CopyableKlass) {}
public func consumeVal(_ x: consuming NonEmptyStruct) {}
public func consumeVal(_ x: consuming EmptyStruct) {}

let copyableKlassLetGlobal = CopyableKlass()
var copyableKlassVarGlobal = CopyableKlass()

/////////////////
// MARK: Tests //
/////////////////

public struct DeinitTest : ~Copyable {
    deinit {}
}

public protocol P {}

public struct GenericDeinitTest<T : P> : ~Copyable {
    deinit {}
}

//////////////////////////////////////////
// MARK: Caller Argument Let Spill Test //
//////////////////////////////////////////

public func callerBorrowClassLetFieldForArgumentSpillingTestLet() {
    let x = CopyableKlass()
    borrowVal(x.letS.e)
}

public func callerBorrowClassLetFieldForArgumentSpillingTestVar() {
    var x = CopyableKlass()
    x = CopyableKlass()
    borrowVal(x.letS.e)
}

public func callerBorrowClassLetFieldForArgumentSpillingTestArg(_ x: CopyableKlass) {
    borrowVal(x.letS.e)
}

public func callerBorrowClassLetFieldForArgumentSpillingTestInOutArg(_ x: inout CopyableKlass) {
    borrowVal(x.letS.e)
}

public func callerBorrowClassLetFieldForArgumentSpillingTestConsumingArg(_ x: consuming CopyableKlass) {
    borrowVal(x.letS.e)
}

public func callerBorrowClassLetFieldForArgumentSpillingTestLetGlobal() {
    borrowVal(copyableKlassLetGlobal.letS.e)
}

public func callerBorrowClassLetFieldForArgumentSpillingTestVarGlobal() {
    borrowVal(copyableKlassVarGlobal.letS.e)
}

public func callerConsumeClassLetFieldForArgumentSpillingTestLet() {
    let x = CopyableKlass()
    consumeVal(x.letS.e)
}

public func callerConsumeClassLetFieldForArgumentSpillingTestVar() {
    var x = CopyableKlass()
    x = CopyableKlass()
    consumeVal(x.letS.e)
}

public func callerConsumeClassLetFieldForArgumentSpillingTestArg(_ x: CopyableKlass) {
    consumeVal(x.letS.e)
}

public func callerConsumeClassLetFieldForArgumentSpillingTestInOutArg(_ x: inout CopyableKlass) {
    consumeVal(x.letS.e)
}

public func callerConsumeClassLetFieldForArgumentSpillingTestConsumingArg(_ x: consuming CopyableKlass) {
    consumeVal(x.letS.e)
}

public func callerConsumeClassLetFieldForArgumentSpillingTestLetGlobal() {
    consumeVal(copyableKlassLetGlobal.letS.e)
}

public func callerConsumeClassLetFieldForArgumentSpillingTestVarGlobal() {
    consumeVal(copyableKlassVarGlobal.letS.e)
}

////////////////////
// MARK: Var Test //
////////////////////

public func callerBorrowClassVarFieldForArgumentSpillingTestLet() {
    let x = CopyableKlass()
    borrowVal(x.varS.e)
}

public func callerBorrowClassVarFieldForArgumentSpillingTestVar() {
    var x = CopyableKlass()
    x = CopyableKlass()
    borrowVal(x.varS.e)
}

public func callerBorrowClassVarFieldForArgumentSpillingTestArg(_ x: CopyableKlass) {
    borrowVal(x.varS.e)
}

public func callerBorrowClassVarFieldForArgumentSpillingTestInOutArg(_ x: inout CopyableKlass) {
    borrowVal(x.varS.e)
}

public func callerBorrowClassVarFieldForArgumentSpillingTestConsumingArg(_ x: consuming CopyableKlass) {
    borrowVal(x.varS.e)
}

public func callerBorrowClassVarFieldForArgumentSpillingTestLetGlobal() {
    borrowVal(copyableKlassLetGlobal.varS.e)
}

public func callerBorrowClassVarFieldForArgumentSpillingTestVarGlobal() {
    borrowVal(copyableKlassVarGlobal.varS.e)
}

public func callerConsumeClassVarFieldForArgumentSpillingTestLet() {
    let x = CopyableKlass()
    consumeVal(x.varS.e)
}

public func callerConsumeClassVarFieldForArgumentSpillingTestVar() {
    var x = CopyableKlass()
    x = CopyableKlass()
    consumeVal(x.varS.e)
}

public func callerConsumeClassVarFieldForArgumentSpillingTestArg(_ x: CopyableKlass) {
    consumeVal(x.varS.e)
}

public func callerConsumeClassVarFieldForArgumentSpillingTestInOutArg(_ x: inout CopyableKlass) {
    consumeVal(x.varS.e)
}

public func callerConsumeClassVarFieldForArgumentSpillingTestConsumingArg(_ x: consuming CopyableKlass) {
    consumeVal(x.varS.e)
}

public func callerConsumeClassVarFieldForArgumentSpillingTestLetGlobal() {
    consumeVal(copyableKlassLetGlobal.varS.e)
}

public func callerConsumeClassVarFieldForArgumentSpillingTestVarGlobal() {
    consumeVal(copyableKlassVarGlobal.varS.e)
}
