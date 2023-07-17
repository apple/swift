// RUN: %target-typecheck-verify-swift -enable-experimental-feature ReferenceBindings

var globalValue = String()
class Klass {
    var sStored: String = ""
    var sGetter: String { fatalError() }
    var sModify: String {
        _read {
            fatalError()
        }
        _modify {
            fatalError()
        }
    }
}

struct S : ~Copyable {
    var sStored: String = ""
    var sGetter: String { fatalError() }
    var sModify: String {
        _read {
            fatalError()
        }
        _modify {
            fatalError()
        }
    }    
}

func foo(_ arg: String, _ consumingArg: consuming String, _ borrowingArg: borrowing String, _ inoutArg: inout String) {
    let letValue = 5
    var varValue = 5

    inout noInitialValue: String // expected-error {{inout bindings must have an initial value}}
    inout literalX = 5 // expected-error {{inout bindings must be bound to an lvalue}}
    inout consumingArgX = consumingArg
    inout borrowingArgX = borrowingArg // expected-error {{inout bindings must be bound to an lvalue}}
    inout inoutArgX = inoutArg
    inout letValueX = letValue // expected-error {{inout bindings must be bound to an lvalue}}
    inout varValueX = varValue
    inout globalValueX = globalValue
    let k = Klass()
    inout storedKlassFieldX = k.sStored
    inout getterKlassFieldX = k.sGetter // expected-error {{inout bindings must be bound to an lvalue}}
    inout modifyKlassFieldX = k.sModify
    let letStruct = S()
    inout storedStructLetFieldX = letStruct.sStored // expected-error {{inout bindings must be bound to an lvalue}}
    inout getterStructLetFieldX = letStruct.sGetter // expected-error {{inout bindings must be bound to an lvalue}}
    inout modifyStructLetFieldX = letStruct.sModify // expected-error {{inout bindings must be bound to an lvalue}}
    var varStruct = S()
    inout storedStructVarFieldX = varStruct.sStored
    inout getterStructVarFieldX = varStruct.sGetter // expected-error {{inout bindings must be bound to an lvalue}}
    inout modifyStructVarFieldX = varStruct.sModify
}

// Make sure that we get never used to diagnostics, but not never written to diagnostics.
func neverWrittenMutatedDiagnostics() {
    var x = "123"
    x = "223"
    do {
        inout x2 = x // expected-warning {{initialization of variable 'x2' was never used; consider replacing with assignment to '_' or removing it}}
    }
    do {
        inout x2 = x
        let _ = x2
    }
}
