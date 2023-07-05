// RUN: %target-swift-emit-ir -I %S/Inputs -enable-experimental-cxx-interop %s -Xcc -fignore-exceptions | %FileCheck %s

import FreeFunctions

// CHECK-LABEL: define {{.*}}void @"$s4main10basicTestsyyF"()
// CHECK: call ptr @{{_ZN12FunctionsNS121basicFunctionTopLevelEv|"\?basicFunctionTopLevel@FunctionsNS1@@YAPEBDXZ"}}()
// CHECK: call ptr @{{_ZN12FunctionsNS112FunctionsNS224basicFunctionSecondLevelEv|"\?basicFunctionSecondLevel@FunctionsNS2@FunctionsNS1@@YAPEBDXZ"}}()
// CHECK: call ptr @{{_ZN12FunctionsNS112FunctionsNS212FunctionsNS324basicFunctionLowestLevelEv|"\?basicFunctionLowestLevel@FunctionsNS3@FunctionsNS2@FunctionsNS1@@YAPEBDXZ"}}()
// CHECK: ret void
public func basicTests() {
  FunctionsNS1.basicFunctionTopLevel()
  FunctionsNS1.FunctionsNS2.basicFunctionSecondLevel()
  FunctionsNS1.FunctionsNS2.FunctionsNS3.basicFunctionLowestLevel()
}

// CHECK-LABEL: define {{.*}}void @"$s4main20forwardDeclaredFuncsyyF"()
// CHECK: call ptr @{{_ZN12FunctionsNS115forwardDeclaredEv|"\?forwardDeclared@FunctionsNS1@@YAPEBDXZ"}}()
// CHECK: call ptr @{{_ZN12FunctionsNS116definedOutOfLineEv|"\?definedOutOfLine@FunctionsNS1@@YAPEBDXZ"}}()
// CHECK: ret void
public func forwardDeclaredFuncs() {
  FunctionsNS1.forwardDeclared()
  FunctionsNS1.definedOutOfLine()
}

// CHECK-LABEL: define {{.*}}void @"$s4main9sameNamesyyF"()
// CHECK: call ptr @{{_ZN12FunctionsNS115sameNameInChildEv|"\?sameNameInChild@FunctionsNS1@@YAPEBDXZ"}}()
// CHECK: call ptr @{{_ZN12FunctionsNS117sameNameInSiblingEv|"\?sameNameInSibling@FunctionsNS1@@YAPEBDXZ"}}()
// CHECK: call ptr @{{_ZN12FunctionsNS112FunctionsNS215sameNameInChildEv|"\?sameNameInChild@FunctionsNS2@FunctionsNS1@@YAPEBDXZ"}}()
// CHECK: call ptr @{{_ZN12FunctionsNS417sameNameInSiblingEv|"\?sameNameInSibling@FunctionsNS4@@YAPEBDXZ"}}()
// CHECK: ret void
public func sameNames() {
  FunctionsNS1.sameNameInChild()
  FunctionsNS1.sameNameInSibling()
  FunctionsNS1.FunctionsNS2.sameNameInChild()
  FunctionsNS4.sameNameInSibling()
}

