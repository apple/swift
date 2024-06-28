// RUN: rm -rf %t
// RUN: split-file %s %t
// RUN: %target-swiftxx-frontend -emit-ir -I %t/Inputs -validate-tbd-against-ir=none %t/test.swift | %FileCheck %s

//--- Inputs/module.modulemap
module ThrowingConstructorDestructorCleanupRef {
  header "test.h"
  requires cplusplus
}
//--- Inputs/test.h

void testFunc(int x);

template<class T>
class HasDestructor {
    T x = 0;
public:

    HasDestructor() { }
    HasDestructor(const HasDestructor &other) : x(other.x) {}
    inline ~HasDestructor() { testFunc(42); }
};

template<class T>
class HasThrowingConstructor {
    HasDestructor<T> m;
public:
    HasThrowingConstructor();
    ~HasThrowingConstructor();
    inline HasThrowingConstructor(const HasThrowingConstructor &f) : m(f.m) {
        doSomethingThatMightThrow();
    }

    void doSomethingThatMightThrow();
};

inline void test33(const HasThrowingConstructor<int> x) {
    
}

using HasThrowingConstructorInt = HasThrowingConstructor<int>;

//--- test.swift

import ThrowingConstructorDestructorCleanupRef

public func test() {
  let x = HasThrowingConstructorInt()
  test33(x)
}

// Make sure we reach the destructor of 'HasDestructor'
// CHECK: define linkonce_odr {{.*}} @{{_ZN13HasDestructorIiED2Ev|"\?\?1\?\$HasDestructor@H@@QEAA@XZ"}}
