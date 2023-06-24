// Test that we emit LLVM IR for inline functions that are called directly or
// transitively from Swift.
//
// Test that we don't emit LLVM IR for inline functions that are not called from
// Swift.

// RUN: %empty-directory(%t)
// RUN: %target-swift-frontend %use_no_opaque_pointers %s -I %S/Inputs -Xcc -std=c99 -emit-ir -o - | %FileCheck %s -check-prefix C99 --implicit-check-not notCalled
// RUN: %target-swift-frontend %s -I %S/Inputs -Xcc -std=c99 -emit-ir -o -
// RUN: %target-swiftxx-frontend %use_no_opaque_pointers %s -I %S/Inputs -emit-ir -o - | %FileCheck %s -check-prefix CXX --implicit-check-not notCalled
// RUN: %target-swiftxx-frontend %s -I %S/Inputs -emit-ir -o -

import EmitCalledInlineFunction

// C99-DAG: define internal i32 @calledFromSwift()
// C99-DAG: define internal i32 @calledTransitively()

// CXX-DAG: define {{.*}}i32 @{{_Z15calledFromSwiftv|"\?calledFromSwift@@YAHXZ"}}()
// CXX-DAG: define {{.*}}i32 @{{_Z18calledTransitivelyv|"\?calledTransitively@@YAHXZ"}}()
// CXX-DAG: define {{.*}}i32 @{{_ZN1C32memberFunctionCalledTransitivelyEv|"\?memberFunctionCalledTransitively@C@@QEAAHXZ"}}(%class.C* {{.*}})
// CXX-DAG: define {{.*}}i32 @{{_Z29calledTransitivelyFromVarInitv|"\?calledTransitivelyFromVarInit@@YAHXZ"}}()

calledFromSwift()

let _ = varUsedFromSwift
