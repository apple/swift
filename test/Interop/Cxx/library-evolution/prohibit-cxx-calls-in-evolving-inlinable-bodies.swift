// RUN: %empty-directory(%t)
// RUN: split-file %s %t
// RUN: %target-swift-frontend %t/test.swift -I %t/Inputs -typecheck -enable-library-evolution -enable-experimental-cxx-interop -verify
 
//--- Inputs/module.modulemap
module CxxModule {
    header "header.h"
    requires cplusplus
}

//--- Inputs/header.h

class CxxStruct {
public:
    int x; int y;

    void method() const;
};

CxxStruct createStruct();

void freeCxxFunction();
int *freeCxxFunctionReturnsIntPointer();
void freeCxxFunctionTakesIntPointer(int * _Nullable);

void freeCxxFunctionTakesStructPointer(int, CxxStruct * _Nullable);

//--- test.swift

import CxxModule

@inlinable
public func publicFuncPublicBody() {
    // ok
    freeCxxFunction()
    // ok
    let _ = freeCxxFunctionReturnsIntPointer()
    freeCxxFunctionTakesIntPointer(nil)

    // expected-error@+1 {{global function 'createStruct()' cannot be used in an '@inlinable' function because C++ APIs from imported module 'CxxModule' do not support library evolution}}
    createStruct()
    // expected-error@+2 {{struct 'CxxStruct' cannot be used in an '@inlinable' function because C++ APIs from imported module 'CxxModule' do not support library evolution}}
    // expected-error@+1 {{instance method 'method()' cannot be used in an '@inlinable' function because C++ APIs from imported module 'CxxModule' do not support library evolution}}
    CxxStruct(x: 0, y: 0).method()

    // expected-error@+1 {{global function 'freeCxxFunctionTakesStructPointer' cannot be used in an '@inlinable' function because C++ APIs from imported module 'CxxModule' do not support library evolution}}
    freeCxxFunctionTakesStructPointer(0, nil)
}
