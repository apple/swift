// RUN: %empty-directory(%t)
// RUN: split-file %s %t

// RUN: %target-swift-frontend -typecheck %t/swiftMod.swift -typecheck -module-name SwiftMod -emit-clang-header-path %t/swiftMod.h -I %t -enable-experimental-cxx-interop -Xcc -DFIRSTPASS

// RUN: %target-swift-frontend %t/swiftMod.swift -module-name SwiftMod -emit-module -o %t/SwiftMod.swiftmodule -I %t -enable-experimental-cxx-interop -Xcc -DFIRSTPASS

// RUN: %target-swift-ide-test -print-module -module-to-print=SwiftMod -module-to-print=SwiftToCxxTest -I %t -source-filename=x -enable-experimental-cxx-interop -Xcc -DSWIFT_CXX_INTEROP_HIDE_SWIFT_ERROR | %FileCheck --check-prefix=INTERFACE %s

// RUN: %target-swift-frontend -typecheck %t/swiftMod.swift -typecheck -module-name SwiftMod -I %t -enable-experimental-cxx-interop -Xcc -DSWIFT_CXX_INTEROP_HIDE_SWIFT_ERROR -DSECOND_PASS -emit-sil -o - | %FileCheck --check-prefix=SIL %s

// UNSUPPORTED: OS=windows-msvc

//--- header.h
#ifndef FIRSTPASS
#include "swiftMod.h"

SwiftMod::ExposedToCxx createSwiftClassInCxx();

void passSwiftClassToCxx(SwiftMod::ExposedToCxx value);

SwiftMod::ExposedToCxx * returnClassByPtr();
SwiftMod::ExposedToCxx & returnClassByRef();
const SwiftMod::ExposedToCxx & returnClassByRefConst();

// INTERFACE: func createSwiftClassInCxx() -> ExposedToCxx
// INTERFACE: func passSwiftClassToCxx(_ value: ExposedToCxx)
// INTERFACE: func returnClassByPtr() -> OpaquePointer!
// INTERFACE-NOT: returnClassByRef

class __attribute__((swift_attr("import_owned"))) InClass {
public:
    SwiftMod::ExposedToCxx value;

    InClass(SwiftMod::ExposedToCxx value) : value(value) {}

    inline SwiftMod::ExposedToCxx getValue() const {
        return value;
    }
};

// INTERFACE: struct InClass {
// INTERFACE:   init(_ value: ExposedToCxx)
// INTERFACE:   func getValue() -> ExposedToCxx
// INTERFACE: }

#endif

//--- module.modulemap
module SwiftToCxxTest {
    header "header.h"
    requires cplusplus
}

//--- swiftMod.swift
import SwiftToCxxTest

public class ExposedToCxx {
    public init() {
        i = 0
        print("ExposedToCxx.init")
    }
    deinit {
        print("ExposedToCxx\(i).deinit")
    }

    public final func testMethod() {
        print("ExposedToCxx\(i).testMethod")
    }

    public var i: Int
}

#if SECOND_PASS

func testSwiftClassFromCxxInSwift() {
    let classInstance = createSwiftClassInCxx()
    passSwiftClassToCxx(classInstance)
}

testSwiftClassFromCxxInSwift()

func testSwiftClassInClass() {
    let v = InClass(ExposedToCxx())
    v.getValue().testMethod()
}

testSwiftClassInClass()

#endif

// SIL-LABEL: @$s8SwiftMod04testa14ClassFromCxxInA0yyF : $@convention(thin) () -> ()
// SIL: function_ref @{{_Z21createSwiftClassInCxxv|"\?createSwiftClassInCxx@@YA?AVExposedToCxx@SwiftMod@@XZ"}} : $@convention(c) () -> @owned ExposedToCxx
// SIL: apply {{.*}} : $@convention(c) () -> @owned ExposedToCxx
// SIL: function_ref @{{_Z19passSwiftClassToCxxN8SwiftMod12ExposedToCxxE|"\?passSwiftClassToCxx@@YAXVExposedToCxx@SwiftMod@@@Z"}} : $@convention(c) (@in_guaranteed ExposedToCxx) -> ()
// SIL: apply {{.*}} : $@convention(c) (@in_guaranteed ExposedToCxx) -> ()

// SIL-LABEL: @$s8SwiftMod04testa7ClassInD0yyF : $@convention(thin) () -> () {
// SIL: $@convention(c) (@in_guaranteed ExposedToCxx) -> @out InClass
// SIL: $@convention(cxx_method) (@in_guaranteed InClass) -> @owned ExposedToCxx
