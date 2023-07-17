// RUN: rm -rf %t
// RUN: split-file %s %t
// RUN: %target-swift-ide-test -print-module -module-to-print=CxxModule -I %t/Inputs -source-filename=x -enable-experimental-cxx-interop | %FileCheck %s

// RUN: %target-swift-frontend -typecheck -verify -I %t/Inputs -enable-experimental-cxx-interop %t/test.swift

//--- Inputs/module.modulemap
module CxxTypes {
    header "types.h"
    requires cplusplus
}

module CxxModule {
    header "header.h"
    requires cplusplus
}

//--- Inputs/types.h

template<class T>
class TemplateInTypesModule {
public:
    T x, y;
};

//--- Inputs/header.h

#pragma clang module import CxxTypes

class Struct {
public:
    int x, y;

    TemplateInTypesModule<int> returnsClassInTypesModules() const;

    void takesClassInTypesModules(TemplateInTypesModule<int>) const;
    void takesClassInTypesModulesRef(const TemplateInTypesModule<int> &) const;
};

// CHECK: struct Struct {
// CHECK-NEXT:   init()
// CHECK-NEXT:   init(x: Int32, y: Int32)
// CHECK-NEXT:   func returnsClassInTypesModules() -> Never
// CHECK-NEXT:   var x: Int32
// CHECK-NEXT:   var y: Int32
// CHECK-NEXT: }

TemplateInTypesModule<int> funcWithClassInTypesModules();
void funcWithClassInTypesModulesParam(TemplateInTypesModule<int>);
void funcWithClassInTypesModulesParamRef(const TemplateInTypesModule<int> &);

class StructPrivateDestructor {
public:
    StructPrivateDestructor();
private:
    ~StructPrivateDestructor();
};

StructPrivateDestructor returnsStructPrivateDestructor();

//--- test.swift

import CxxModule

func test() {
    funcWithClassInTypesModules() // expected-error {{'funcWithClassInTypesModules()' is unavailable: return type is unavailable in Swift}}
    Struct().returnsClassInTypesModules() // expected-error {{'returnsClassInTypesModules()' is unavailable: return type is unavailable in Swift}}
}

func test2() {
    funcWithClassInTypesModules() // expected-error {{'funcWithClassInTypesModules()' is unavailable: return type is unavailable in Swift}}
}

func testPrivateDesType() {
    returnsStructPrivateDestructor() // expected-error {{'returnsStructPrivateDestructor()' is unavailable: return type is unavailable in Swift}}
}
