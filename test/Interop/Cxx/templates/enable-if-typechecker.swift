// RUN: not %target-swift-emit-ir %s -I %S/Inputs -enable-experimental-cxx-interop -enable-objc-interop 2>&1 | %FileCheck %s
// REQUIRES: objc_interop

import StdlibUnittest
import EnableIf

let x = HasMethodWithEnableIf()
x.onlyEnabledForBool("a")
// CHECK: error: could not substitute parameters for C++ function template 'HasMethodWithEnableIf::onlyEnabledForBool': NSString *
