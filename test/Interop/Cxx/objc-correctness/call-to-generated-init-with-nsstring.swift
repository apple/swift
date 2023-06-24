// RUN: %target-swift-ide-test -print-module -module-to-print=CxxClassWithNSStringInit -I %S/Inputs -source-filename=x -enable-experimental-cxx-interop -enable-objc-interop | %FileCheck -check-prefix=CHECK-IDE-TEST %s
// RUN: %target-swift-frontend -I %S/Inputs -enable-experimental-cxx-interop -emit-sil %s -Xcc -fignore-exceptions | %FileCheck --check-prefix=SIL-TRIVIAL %s
// RUN: %target-swift-frontend -I %S/Inputs -enable-experimental-cxx-interop -emit-sil %s -Xcc -fignore-exceptions -Xcc -DS_NONTRIVIAL_DESTRUCTOR | %FileCheck --check-prefix=SIL-NONTRIVIAL %s
// RUN: %target-swift-frontend -I %S/Inputs -enable-experimental-cxx-interop -emit-ir %s -Xcc -fignore-exceptions | %FileCheck --check-prefix=IR-TRIVIAL %s
// RUN: %target-swift-frontend -I %S/Inputs -enable-experimental-cxx-interop -emit-ir %s -Xcc -fignore-exceptions -Xcc -DS_NONTRIVIAL_DESTRUCTOR | %FileCheck --check-prefix=IR-NONTRIVIAL %s

// REQUIRES: objc_interop

import Foundation
import CxxClassWithNSStringInit

// CHECK-IDE-TEST: struct S {
// CHECK-IDE-TEST:   init()
// CHECK-IDE-TEST:   init(A: NSString?, B: NSString?, C: NSString?)
// CHECK-IDE-TEST:   var A: NSString?
// CHECK-IDE-TEST:   var B: NSString?
// CHECK-IDE-TEST:   var C: NSString?
// CHECK-IDE-TEST: }

func testSdump() {
  var foo: NSString? = "foo"
  var bar: NSString? = "bar"
  var baz: NSString? = "baz"
  var s = S(A: foo, B: bar, C: baz)
  s.dump()
  ClassWithNonTrivialDestructorIvar().takesS(s)
  takeSFunc(s)
}

testSdump()

// SIL-TRIVIAL:   function_ref @_ZNK1S4dumpEv : $@convention(cxx_method) (@in_guaranteed S) -> ()
// SIL-TRIVIAL-NEXT:   apply %{{.*}}(%{{.*}}) : $@convention(cxx_method) (@in_guaranteed S) -> ()
// SIL-TRIVIAL:      $@convention(objc_method) (@owned S, ClassWithNonTrivialDestructorIvar) -> ()
// SIL-TRIVIAL-NEXT: apply %{{.*}}(%{{.*}}) : $@convention(objc_method) (@owned S, ClassWithNonTrivialDestructorIvar) -> ()
// SIL-TRIVIAL: function_ref @_Z9takeSFunc : $@convention(c) (@owned S) -> ()
// SIL-TRIVIAL-NEXT: apply %{{.*}}(%{{.*}}) : $@convention(c) (@owned S) -> ()

// SIL-NONTRIVIAL:   function_ref @_ZNK1S4dumpEv : $@convention(cxx_method) (@in_guaranteed S) -> ()
// SIL-NONTRIVIAL-NEXT:   apply %{{.*}}(%{{.*}}) : $@convention(cxx_method) (@in_guaranteed S) -> ()
// SIL-NONTRIVIAL:      $@convention(objc_method) (@in S, ClassWithNonTrivialDestructorIvar) -> ()
// SIL-NONTRIVIAL-NEXT: apply %{{.*}}(%{{.*}}) : $@convention(objc_method) (@in S, ClassWithNonTrivialDestructorIvar) -> ()
// SIL-NONTRIVIAL: function_ref @_Z9takeSFunc : $@convention(c) (@in S) -> ()
// SIL-NONTRIVIAL-NEXT: apply %{{.*}}(%{{.*}}) : $@convention(c) (@in S) -> ()


// IR-TRIVIAL-LABEL: define {{.*}} swiftcc void @"$s4main9testSdumpyyF"()
// IR-TRIVIAL-NOT: @_ZN1SC1ERKS_
// IR-TRIVIAL: call {{.*}} @_ZNK1S4dumpEv
// IR-TRIVIAL: call {{.*}} @"$sSo1SVWOh"

// IR-TRIVIAL-LABEL: define linkonce_odr {{.*}} @"$sSo1SVWOh"(
// IR-TRIVIAL: @llvm.objc.release
// IR-TRIVIAL: @llvm.objc.release
// IR-TRIVIAL: @llvm.objc.release
// IR-TRIVIAL: }

// IR-NONTRIVIAL-LABEL: define {{.*}} swiftcc void @"$s4main9testSdumpyyF"()
// IR-NONTRIVIAL: call {{.*}} @_ZN1SC1ERKS_
// IR-NONTRIVIAL: call {{.*}} @_ZNK1S4dumpEv
// IR-NONTRIVIAL: call {{.*}} @_ZN1SD1Ev
