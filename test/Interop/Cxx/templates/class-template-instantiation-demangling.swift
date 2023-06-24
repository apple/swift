// RUN: %target-swift-emit-ir %use_no_opaque_pointers %s -I %S/Inputs -enable-experimental-cxx-interop | grep 'define.*swiftcc.*$' | xargs %swift-demangle | %FileCheck %s
// RUN: %target-swift-emit-ir %s -I %S/Inputs -enable-experimental-cxx-interop | grep 'define.*swiftcc.*$' 

import Mangling

public func receiveInstantiation(_ i: inout WrappedMagicInt) {}

public func returnInstantiation() -> WrappedMagicInt {
  return WrappedMagicInt()
}

// CHECK: $s4main20receiveInstantiationyySo0025MagicWrapperInt32_lsFCfibVzF(%TSo0025MagicWrapperInt32_lsFCfibV* ---> @$s4main20receiveInstantiationyySo0025MagicWrapperInt32_lsFCfibVzF(%TSo0025MagicWrapperInt32_lsFCfibV*
// CHECK: $s4main19returnInstantiationSo0025MagicWrapperInt32_lsFCfibVyF() ---> @$s4main19returnInstantiationSo0025MagicWrapperInt32_lsFCfibVyF()
