 // RUN: %empty-directory(%t)

// RUN: %target-build-swift -module-name=File -package-name Pkg -I%t -emit-ir %s | %FileCheck %s --check-prefix=CHECK-IR-NONRES
// RUN: %target-build-swift -module-name=File -package-name Pkg -I%t -emit-ir %s -enable-library-evolution | %FileCheck %s --check-prefix=CHECK-IR-RES
// RUN: %target-build-swift -module-name=File -package-name Pkg -I%t -emit-ir %s -enable-library-evolution -Xfrontend -experimental-allow-non-resilient-access -Xfrontend -experimental-package-cmo | %FileCheck %s --check-prefix=CHECK-IR-RES

public struct S {
  public static var x = "hello world"
}

// CHECK-IR-NONRES: define{{( dllexport)?}}{{( protected)?}} swiftcc ptr @"$s4File1SV1xSSvau"()
// CHECK-IR-RES: define hidden swiftcc ptr @"$s4File1SV1xSSvau"()

