// RUN: rm -rf %t
// RUN: split-file %s %t
// RUN: %target-swift-emit-irgen -I %t/Inputs -enable-experimental-cxx-interop  -Xcc -std=c++17 %t/test.swift -module-name Test | %FileCheck %s

// REQUIRES: OS=windows-msvc

//--- Inputs/module.modulemap
module MsvcUseVecIt {
    header "test.h"
    requires cplusplus
}

//--- Inputs/test.h

#pragma once

class str {
    int x;
};

class It {
public:
    int x;

    bool operator ==(const It &other) const ;
    bool operator !=(const It &other) const ;
};

template<class T> class vec {
    int x;
public:
    vec(const vec<T> &);
    ~vec();

    It begin() const;
    It end() const;
};

using VecStr = vec<str>;

struct LoadableIntWrapper {
    int value;

    LoadableIntWrapper operator+=(LoadableIntWrapper rhs) {
        value += rhs.value;
        return *this;
    }
};

//--- test.swift

import MsvcUseVecIt

public func test(_ result: VecStr) -> CInt {
    let begin = result.__beginUnsafe()
    return begin.x
}

// CHECK: swiftcc i32 @"$s4Test4testys5Int32VSo0014vecstr_yuJCataVF"({{.*}} %[[RESULT:.*]])
// CHECK: call void @"?begin@?$vec@Vstr@@@@QEBA?AVIt@@XZ"(ptr %[[RESULT]], ptr sret{{.*}}

public func passTempForIndirectRetToVoidCall() {
    var lhs = LoadableIntWrapper(value: 2)
    let rhs = LoadableIntWrapper(value: 2)
    lhs += rhs
}

// CHECK: void @"$sSo18LoadableIntWrapperV2peoiyyABz_ABtFZ"(ptr
// CHECK: %[[OPRESULT:.*]] = alloca %struct.LoadableIntWrapper, align 16
// CHECK: call void @"??YLoadableIntWrapper@@QEAA?AU0@U0@@Z"(ptr {{.*}}, ptr sret(%struct.LoadableIntWrapper) %[[OPRESULT]], i32
