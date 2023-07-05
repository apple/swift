// RUN: %target-swift-emit-ir %s -I %S/Inputs -enable-experimental-cxx-interop | grep 'define.*swiftcc.*$' | xargs %swift-demangle | %FileCheck %s

import Mangling

public func receiveInstantiation(_ i: inout WrappedMagicInt) {}

public func returnInstantiation() -> WrappedMagicInt {
  return WrappedMagicInt()
}

// CHECK: $s4main20receiveInstantiationyySo0025MagicWrapperInt32_lsFCfibVzF(ptr ---> @$s4main20receiveInstantiationyySo0025MagicWrapperInt32_lsFCfibVzF(ptr
// CHECK: $s4main19returnInstantiationSo0025MagicWrapperInt32_lsFCfibVyF() ---> @$s4main19returnInstantiationSo0025MagicWrapperInt32_lsFCfibVyF()
