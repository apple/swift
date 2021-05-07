// RUN: %cxx-all-targets(-I %S/Inputs %s -emit-ir | %FileCheck %s)

import Mangling

public func receiveInstantiation(_ i: inout WrappedMagicInt) {}

// Don't forget to update manglings.txt when changing s4main20receiveInstantiationyySo34__CxxTemplateInst12MagicWrapperIiEVzF
// CHECK: define {{(protected |dllexport )?}}swiftcc void @"$s4main20receiveInstantiationyySo34__CxxTemplateInst12MagicWrapperIiEVzF"(%TSo34__CxxTemplateInst12MagicWrapperIiEV* nocapture dereferenceable(1) %0)

public func receiveInstantiation(_ i: inout WrappedMagicBool) {}

// CHECK: define {{(protected |dllexport )?}}swiftcc void @"$s4main20receiveInstantiationyySo34__CxxTemplateInst12MagicWrapperIbEVzF"(%TSo34__CxxTemplateInst12MagicWrapperIbEV* nocapture dereferenceable(1) %0)

public func returnInstantiation() -> WrappedMagicInt {
  return WrappedMagicInt()
}

// Don't forget to update manglings.txt when changing s4main19returnInstantiationSo34__CxxTemplateInst12MagicWrapperIiEVyF
// CHECK: define {{(protected |dllexport )?}}swiftcc void @"$s4main19returnInstantiationSo34__CxxTemplateInst12MagicWrapperIiEVyF"()

