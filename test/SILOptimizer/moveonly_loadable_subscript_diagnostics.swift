// RUN: %target-swift-emit-sil -enable-experimental-feature MoveOnlyPartialConsumption -sil-verify-all -verify %s

class CopyableKlass {}

public struct NonTrivialStruct : ~Copyable {
    var k = CopyableKlass()
}

public struct LoadableMoveOnlyContainingProtocol : ~Copyable {
    var moveOnly = NonTrivialStruct()

    func nonMutatingFunc() {}
    mutating func mutatingFunc() {}
}

// MARK: Getter Only

public struct LoadableSubscriptGetOnlyTester : ~Copyable {
    subscript(_ i: Int) -> LoadableMoveOnlyContainingProtocol {
        get {
            fatalError()
        }
    }
}

public func testSubscriptGetOnly_BaseLoadable_ResultLoadable_Var() {
    var m = LoadableSubscriptGetOnlyTester()
    m = LoadableSubscriptGetOnlyTester()
    m[0].nonMutatingFunc()
}

public func testSubscriptGetOnly_BaseLoadable_ResultLoadable_Let() {
    let m = LoadableSubscriptGetOnlyTester()
    m[0].nonMutatingFunc()
}

public func testSubscriptGetOnly_BaseLoadable_ResultLoadable_InOut(m: inout LoadableSubscriptGetOnlyTester) {
    m[0].nonMutatingFunc()
}

var globalLoadableSubscriptGetOnlyTester = LoadableSubscriptGetOnlyTester()
public func testSubscriptGetOnly_BaseLoadable_ResultLoadable_Global() {
    globalLoadableSubscriptGetOnlyTester[0].nonMutatingFunc()
}

// Make sure that we get the same behavior when we access through another noncopyable struct.
public struct LoadableSubscriptGetOnlyTesterNonCopyableStructParent : ~Copyable {
    var tester = LoadableSubscriptGetOnlyTester()
    var computedTester: LoadableSubscriptGetOnlyTester { fatalError() }
}

public func testSubscriptGetOnlyThroughNonCopyableParentStruct_BaseLoadable_ResultLoadable_Var() {
    var m = LoadableSubscriptGetOnlyTesterNonCopyableStructParent()
    m = LoadableSubscriptGetOnlyTesterNonCopyableStructParent()
    m.tester[0].nonMutatingFunc()
    m.computedTester[0].nonMutatingFunc()
}

public func testSubscriptGetOnlyThroughNonCopyableParentStruct_BaseLoadable_ResultLoadable_Let() {
    let m = LoadableSubscriptGetOnlyTesterNonCopyableStructParent()
    m.tester[0].nonMutatingFunc()
}

public func testSubscriptGetOnlyThroughNonCopyableParentStruct_BaseLoadable_ResultLoadable_InOut(m: inout LoadableSubscriptGetOnlyTesterNonCopyableStructParent) {
    m.tester[0].nonMutatingFunc()
}

var globalLoadableSubscriptGetOnlyTesterNonCopyableStructParent = LoadableSubscriptGetOnlyTesterNonCopyableStructParent()
public func testSubscriptGetOnlyThroughNonCopyableParentStruct_BaseLoadable_ResultLoadable_Global() {
    globalLoadableSubscriptGetOnlyTesterNonCopyableStructParent.tester[0].nonMutatingFunc()
}

public class LoadableSubscriptGetOnlyTesterClassParent {
    var tester = LoadableSubscriptGetOnlyTester()
    var computedTester: LoadableSubscriptGetOnlyTester { fatalError() }
    var testerParent = LoadableSubscriptGetOnlyTesterNonCopyableStructParent()
}

public func testSubscriptGetOnlyThroughParentClass_BaseLoadable_ResultLoadable_Var() {
    var m = LoadableSubscriptGetOnlyTesterClassParent()
    m = LoadableSubscriptGetOnlyTesterClassParent()
    m.tester[0].nonMutatingFunc()
    m.testerParent.tester[0].nonMutatingFunc()
    m.computedTester[0].nonMutatingFunc()
}

// MARK: Getter + Setter.
// This is different since adding a setter changes how we codegen.

public struct LoadableSubscriptGetSetTester : ~Copyable {
    subscript(_ i: Int) -> LoadableMoveOnlyContainingProtocol {
        get {
            fatalError()
        }
        set {
            fatalError()
        }
    }
}

public func testSubscriptGetSet_BaseLoadable_ResultLoadable_Var() {
    var m = LoadableSubscriptGetSetTester()
    m = LoadableSubscriptGetSetTester()
    m[0].nonMutatingFunc()
    m[0] = LoadableMoveOnlyContainingProtocol()
    m[0].mutatingFunc()
}

public func testSubscriptGetSet_BaseLoadable_ResultLoadable_Let() {
    let m = LoadableSubscriptGetSetTester()
    m[0].nonMutatingFunc()
}

public func testSubscriptGetSet_BaseLoadable_ResultLoadable_InOut(m: inout LoadableSubscriptGetSetTester) {
    m[0].nonMutatingFunc()
    m[0] = LoadableMoveOnlyContainingProtocol()
    m[0].mutatingFunc()
}

var globalLoadableSubscriptGetSetTester = LoadableSubscriptGetSetTester()
public func testSubscriptGetSet_BaseLoadable_ResultLoadable_Global() {
    globalLoadableSubscriptGetSetTester[0].nonMutatingFunc()
    globalLoadableSubscriptGetSetTester[0] = LoadableMoveOnlyContainingProtocol()
    globalLoadableSubscriptGetSetTester[0].mutatingFunc()
}

// Make sure that we get the same behavior when we access through another noncopyable struct.
public struct LoadableSubscriptGetSetTesterNonCopyableStructParent : ~Copyable {
    var tester = LoadableSubscriptGetSetTester()
    var computedTester: LoadableSubscriptGetSetTester { fatalError() }
}

public func testSubscriptGetSetThroughNonCopyableParentStruct_BaseLoadable_ResultLoadable_Var() {
    var m = LoadableSubscriptGetSetTesterNonCopyableStructParent()
    m = LoadableSubscriptGetSetTesterNonCopyableStructParent()
    m.tester[0].nonMutatingFunc()
    m.tester[0].mutatingFunc()
    m.computedTester[0].nonMutatingFunc()
}

public func testSubscriptGetSetThroughNonCopyableParentStruct_BaseLoadable_ResultLoadable_Let() {
    let m = LoadableSubscriptGetSetTesterNonCopyableStructParent()
    m.tester[0].nonMutatingFunc()
}

public func testSubscriptGetSetThroughNonCopyableParentStruct_BaseLoadable_ResultLoadable_InOut(m: inout LoadableSubscriptGetSetTesterNonCopyableStructParent) {
    m.tester[0].nonMutatingFunc()
    m.tester[0].mutatingFunc()
}

var globalLoadableSubscriptGetSetTesterNonCopyableStructParent = LoadableSubscriptGetSetTesterNonCopyableStructParent()
public func testSubscriptGetSetThroughNonCopyableParentStruct_BaseLoadable_ResultLoadable_Global() {
    globalLoadableSubscriptGetSetTesterNonCopyableStructParent.tester[0].nonMutatingFunc()
    globalLoadableSubscriptGetSetTesterNonCopyableStructParent.tester[0].mutatingFunc()
}

public class LoadableSubscriptGetSetTesterClassParent {
    var tester = LoadableSubscriptGetSetTester()
    var computedTester: LoadableSubscriptGetSetTester { fatalError() }
    var computedTester2: LoadableSubscriptGetSetTester {
        get { fatalError() }
        set { fatalError() }
    }
    var testerParent = LoadableSubscriptGetSetTesterNonCopyableStructParent()
}

public func testSubscriptGetSetThroughParentClass_BaseLoadable_ResultLoadable_Var() {
    var m = LoadableSubscriptGetSetTesterClassParent()
    m = LoadableSubscriptGetSetTesterClassParent()
    m.tester[0].nonMutatingFunc()
    m.tester[0].mutatingFunc()
    m.testerParent.tester[0].nonMutatingFunc()
    m.testerParent.tester[0].mutatingFunc()
    m.computedTester[0].nonMutatingFunc()
    m.computedTester2[0].nonMutatingFunc()
    m.computedTester2[0].mutatingFunc()
}

// MARK: _read and _modify

public struct LoadableSubscriptReadModifyTester : ~Copyable {
    subscript(_ i: Int) -> LoadableMoveOnlyContainingProtocol {
        _read {
            fatalError()
        }
        _modify {
            fatalError()
        }
    }
}

public func testSubscriptReadModify_BaseLoadable_ResultLoadable_Var() {
    var m = LoadableSubscriptReadModifyTester()
    m = LoadableSubscriptReadModifyTester()
    m[0].nonMutatingFunc()
    m[0] = LoadableMoveOnlyContainingProtocol()
    m[0].mutatingFunc()
}

public func testSubscriptReadModify_BaseLoadable_ResultLoadable_Let() {
    let m = LoadableSubscriptReadModifyTester()
    m[0].nonMutatingFunc()
}

public func testSubscriptReadModify_BaseLoadable_ResultLoadable_InOut(m: inout LoadableSubscriptReadModifyTester) {
    m[0].nonMutatingFunc()
    m[0] = LoadableMoveOnlyContainingProtocol()
    m[0].mutatingFunc()
}

var globalLoadableSubscriptReadModifyTester = LoadableSubscriptReadModifyTester()
public func testSubscriptReadModify_BaseLoadable_ResultLoadable_Global() {
    globalLoadableSubscriptReadModifyTester[0].nonMutatingFunc()
    globalLoadableSubscriptReadModifyTester[0] = LoadableMoveOnlyContainingProtocol()
    globalLoadableSubscriptReadModifyTester[0].mutatingFunc()
}

// Make sure that we get the same behavior when we access through another noncopyable struct.
public struct LoadableSubscriptReadModifyTesterNonCopyableStructParent : ~Copyable {
    var tester = LoadableSubscriptReadModifyTester()
    var computedTester: LoadableSubscriptReadModifyTester { fatalError() }
}

public func testSubscriptReadModifyThroughNonCopyableParentStruct_BaseLoadable_ResultLoadable_Var() {
    var m = LoadableSubscriptReadModifyTesterNonCopyableStructParent()
    m = LoadableSubscriptReadModifyTesterNonCopyableStructParent()
    m.tester[0].nonMutatingFunc()
    m.tester[0].mutatingFunc()
    m.computedTester[0].nonMutatingFunc()
}


public func testSubscriptReadModifyThroughNonCopyableParentStruct_BaseLoadable_ResultLoadable_Let() {
    let m = LoadableSubscriptReadModifyTesterNonCopyableStructParent()
    m.tester[0].nonMutatingFunc()
}

public func testSubscriptReadModifyThroughNonCopyableParentStruct_BaseLoadable_ResultLoadable_InOut(m: inout LoadableSubscriptReadModifyTesterNonCopyableStructParent) {
    m.tester[0].nonMutatingFunc()
    m.tester[0].mutatingFunc()
}

var globalLoadableSubscriptReadModifyTesterNonCopyableStructParent = LoadableSubscriptReadModifyTesterNonCopyableStructParent()
public func testSubscriptReadModifyThroughNonCopyableParentStruct_BaseLoadable_ResultLoadable_Global() {
    globalLoadableSubscriptReadModifyTesterNonCopyableStructParent.tester[0].nonMutatingFunc()
    globalLoadableSubscriptReadModifyTesterNonCopyableStructParent.tester[0].mutatingFunc()
}

public class LoadableSubscriptReadModifyTesterClassParent {
    var tester = LoadableSubscriptReadModifyTester()
    var computedTester: LoadableSubscriptReadModifyTester { fatalError() }
    var computedTester2: LoadableSubscriptReadModifyTester {
        get { fatalError() }
        set { fatalError() }
    }
    var testerParent = LoadableSubscriptReadModifyTesterNonCopyableStructParent()
}

public func testSubscriptReadModifyThroughParentClass_BaseLoadable_ResultLoadable_Var() {
    var m = LoadableSubscriptReadModifyTesterClassParent()
    m = LoadableSubscriptReadModifyTesterClassParent()
    m.tester[0].nonMutatingFunc()
    m.tester[0].mutatingFunc()
    m.testerParent.tester[0].nonMutatingFunc()
    m.testerParent.tester[0].mutatingFunc()
    m.computedTester[0].nonMutatingFunc()
    m.computedTester2[0].nonMutatingFunc()
    m.computedTester2[0].mutatingFunc()
}

// MARK: get and _modify

public struct LoadableSubscriptGetModifyTester : ~Copyable {
    subscript(_ i: Int) -> LoadableMoveOnlyContainingProtocol {
        get {
            fatalError()
        }
        _modify {
            fatalError()
        }
    }
}

public func testSubscriptGetModify_BaseLoadable_ResultLoadable_Var() {
    var m = LoadableSubscriptGetModifyTester()
    m = LoadableSubscriptGetModifyTester()
    m[0].nonMutatingFunc()
    m[0] = LoadableMoveOnlyContainingProtocol()
    m[0].mutatingFunc()
}

public func testSubscriptGetModify_BaseLoadable_ResultLoadable_Let() {
    let m = LoadableSubscriptGetModifyTester()
    m[0].nonMutatingFunc()
}

public func testSubscriptGetModify_BaseLoadable_ResultLoadable_InOut(m: inout LoadableSubscriptGetModifyTester) {
    m[0].nonMutatingFunc()
    m[0] = LoadableMoveOnlyContainingProtocol()
    m[0].mutatingFunc()
}

var globalLoadableSubscriptGetModifyTester = LoadableSubscriptGetModifyTester()
public func testSubscriptGetModify_BaseLoadable_ResultLoadable_Global() {
    globalLoadableSubscriptGetModifyTester[0].nonMutatingFunc()
    globalLoadableSubscriptGetModifyTester[0] = LoadableMoveOnlyContainingProtocol()
    globalLoadableSubscriptGetModifyTester[0].mutatingFunc()
}

// Make sure that we get the same behavior when we access through another noncopyable struct.
public struct LoadableSubscriptGetModifyTesterNonCopyableStructParent : ~Copyable {
    var tester = LoadableSubscriptGetModifyTester()
    var computedTester: LoadableSubscriptGetModifyTester { fatalError() }
}

public func testSubscriptGetModifyThroughNonCopyableParentStruct_BaseLoadable_ResultLoadable_Var() {
    var m = LoadableSubscriptGetModifyTesterNonCopyableStructParent()
    m = LoadableSubscriptGetModifyTesterNonCopyableStructParent()
    m.tester[0].nonMutatingFunc()
    m.tester[0].mutatingFunc()
    m.computedTester[0].nonMutatingFunc()
}

public func testSubscriptGetModifyThroughNonCopyableParentStruct_BaseLoadable_ResultLoadable_Let() {
    let m = LoadableSubscriptGetModifyTesterNonCopyableStructParent()
    m.tester[0].nonMutatingFunc()
}

var globalLoadableSubscriptGetModifyTesterNonCopyableStructParent = LoadableSubscriptGetModifyTesterNonCopyableStructParent()
public func testSubscriptGetModifyThroughNonCopyableParentStruct_BaseLoadable_ResultLoadable_Global() {
    globalLoadableSubscriptGetModifyTesterNonCopyableStructParent.tester[0].nonMutatingFunc()
    globalLoadableSubscriptGetModifyTesterNonCopyableStructParent.tester[0].mutatingFunc()
}

public class LoadableSubscriptGetModifyTesterClassParent {
    var tester = LoadableSubscriptGetModifyTester()
    var computedTester: LoadableSubscriptGetModifyTester { fatalError() }
    var computedTester2: LoadableSubscriptGetModifyTester {
        get { fatalError() }
        set { fatalError() }
    }
    var testerParent = LoadableSubscriptGetModifyTesterNonCopyableStructParent()
}

public func testSubscriptGetModifyThroughParentClass_BaseLoadable_ResultLoadable_Var() {
    var m = LoadableSubscriptGetModifyTesterClassParent()
    m = LoadableSubscriptGetModifyTesterClassParent()
    m.tester[0].nonMutatingFunc()
    m.tester[0].mutatingFunc()
    m.testerParent.tester[0].nonMutatingFunc()
    m.testerParent.tester[0].mutatingFunc()
    m.computedTester[0].nonMutatingFunc()
    m.computedTester2[0].nonMutatingFunc()
    m.computedTester2[0].mutatingFunc()
}
