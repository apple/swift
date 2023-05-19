// RUN: rm -rf %t
// RUN: split-file %s %t
// RUN: %target-swift-frontend -typecheck -I %t/Inputs  %t/test.swift  -enable-experimental-cxx-interop -disable-availability-checking 2>&1

//--- Inputs/module.modulemap
module Test {
    header "test.h"
    requires cplusplus
    
    module sub {
        header "subtest.h"

        export *
    }
    
    export *
}

//--- Inputs/test.h
// empty file

//--- Inputs/subtest.h
struct
    __attribute__((swift_attr("import_reference")))
    __attribute__((swift_attr("retain:retainFn")))
    __attribute__((swift_attr("release:releaseFn")))
RefCounted {
    static RefCounted *create();
};

void retainFn(RefCounted *);
void releaseFn(RefCounted *);

//--- test.swift

import Test

let x = RefCounted.create()
