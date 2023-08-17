// RUN: rm -rf %t
// RUN: split-file %s %t
// RUN: %target-swiftxx-frontend -emit-ir -I %t/Inputs -validate-tbd-against-ir=none %t/test.swift | %FileCheck %s

//--- Inputs/module.modulemap
module DestroyedUsingDelete {
  header "test.h"
  requires cplusplus
}
//--- Inputs/test.h

extern void referencedSymbol();
inline void emittedIntoSwiftObject() { referencedSymbol(); }

class BaseClass {
public:
    inline ~BaseClass() {
        emittedIntoSwiftObject();
    }

    int x;
};

class Container {
public:
    Container() : pointer(new BaseClass) {}
    Container(const Container &other) : pointer(new BaseClass) {}
    ~Container() { delete pointer; }

    inline int method() const {
        return 42;
    }
private:
    BaseClass *pointer;
};

class ForwardClassDecl;

template<class T>
class ContainerWithForward {
public:
    inline ~ContainerWithForward() {
        if (sizeof(T) > 0)
            referencedSymbol();
    }
};

class ClassWithOutOfLineDestructor {
public:
    ~ClassWithOutOfLineDestructor();

    ContainerWithForward<ForwardClassDecl> field;
};

ClassWithOutOfLineDestructor *getClassWithOutOfLineDestructorValue();

inline void testMethodDestructorFwdDecl() {
    delete getClassWithOutOfLineDestructorValue();
}

//--- test.swift

import DestroyedUsingDelete

public func test() {
  let i = Container()
  i.method()
  testMethodDestructorFwdDecl()
}

// Make sure we reach destructor accessible from `delete` statement.

// CHECK: define linkonce_odr{{( dso_local)?}} void @{{_Z22emittedIntoSwiftObjectv|"\?emittedIntoSwiftObject@@YAXXZ"}}
