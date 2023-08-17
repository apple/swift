// RUN: %empty-directory(%t)
// RUN: split-file %s %t
// RUN: %target-swift-frontend %t/test.swift -I %t/Inputs -typecheck -enable-library-evolution -enable-experimental-cxx-interop -disable-availability-checking -verify
 
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

enum class CxxEnum {
  A, B
};

template<class T>
class CxxTemplate {
    T v;
};

using CxxTemplateInt = CxxTemplate<int>;

class
__attribute__((swift_attr("import_reference")))
__attribute__((swift_attr("retain:immortal")))
__attribute__((swift_attr("release:immortal")))
SingletonReference {
public:
    SingletonReference(const SingletonReference &) = delete;
    
    static SingletonReference * _Nonnull create();

    void method();
};

CxxStruct createStruct();

void freeCxxFunction();

using BuiltinIntTypealis = int;

//--- test.swift

import CxxModule

// ok
public func usesBuiltinIntTypealis() -> BuiltinIntTypealis {
    return 21
}

// expected-error@+1 {{cannot use class 'SingletonReference' here; C++ types from imported module 'CxxModule' do not support library evolution}}
public func usesCxxSingletonReference() -> SingletonReference {
    return SingletonReference.create()
}

// expected-error@+1 {{cannot use struct 'CxxStruct' here; C++ types from imported module 'CxxModule' do not support library evolution}}
public func usesCxxStruct(_ x: CxxStruct) {
}

// expected-error@+1 {{cannot use enum 'CxxEnum' here; C++ types from imported module 'CxxModule' do not support library evolution}}
public typealias EnumT = CxxEnum

extension CxxTemplateInt {
    // ok
    func testInternal() {
        
    }
}

// expected-error@+1 {{cannot use type alias 'CxxTemplateInt' in an extension with public or '@usableFromInline' members; C++ types from imported module 'CxxModule' do not support library evolution}}
extension CxxTemplateInt {
    public func testPublicExt() {
    }
}

public func publicFuncInternalBody() {
    // ok
    let s = createStruct()
    s.method()
}

@inlinable
public func publicFuncPublicBody() {
    // expected-error@+1 {{class 'SingletonReference' cannot be used in an '@inlinable' function because C++ APIs from imported module 'CxxModule' do not support library evolution}}
    let value = SingletonReference.create()
    value.method()
}
