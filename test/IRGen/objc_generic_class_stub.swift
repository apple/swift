
// RUN: %empty-directory(%t)
// RUN: %build-irgen-test-overlays
// RUN: %target-swift-frontend(mock-sdk: -sdk %S/Inputs -I %t) %use_no_opaque_pointers -primary-file %s -emit-ir | %FileCheck %s
// RUN: %target-swift-frontend(mock-sdk: -sdk %S/Inputs -I %t) -primary-file %s -emit-ir

// REQUIRES: objc_interop

import Foundation

public class GenericNSObjectSubclass<T> : NSObject {}

public class ConcreteNSObjectSubclass : GenericNSObjectSubclass<Int> {}

// Note the stub here is internal; it's only purpose is to appear in the stub list
// so that it can be realized by objc_copyClassList():

// CHECK-LABEL: @"$s23objc_generic_class_stub24ConcreteNSObjectSubclassCMt" = internal global %objc_full_class_stub {{.*}} @"$s23objc_generic_class_stub24ConcreteNSObjectSubclassCMU{{(\.ptrauth)?}}"

// CHECK-LABEL: @objc_class_stubs = internal global {{.*}} @"$s23objc_generic_class_stub24ConcreteNSObjectSubclassCMt" {{.*}}, section "__DATA,__objc_stublist,regular,no_dead_strip"
