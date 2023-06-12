// RUN: %target-swift-frontend(mock-sdk: %clang-importer-sdk) -parse-as-library -module-name Test -validate-tbd-against-ir=missing %s -emit-ir | %FileCheck %s --check-prefixes=CHECK,CHECK-NO-STRIP

// RUN: %target-swift-frontend(mock-sdk: %clang-importer-sdk) -parse-as-library -module-name Test -validate-tbd-against-ir=missing -unavailable-decl-optimization=complete %s -emit-ir | %FileCheck %s --check-prefixes=CHECK,CHECK-STRIP

// REQUIRES: objc_interop

import Foundation

public class Class: NSObject {
  // CHECK-NO-STRIP: define {{.*}} @"$s4Test5ClassC3fooyyF"
  // CHECK-STRIP-NOT: define {{.*}} @"$s4Test5ClassC3fooyyF"

  // CHECK-NO-STRIP: define {{.*}} @"$s4Test5ClassC3fooyyFTo"
  // CHECK-STRIP-NOT: define {{.*}} @"$s4Test5ClassC3fooyyFTo"
  @available(*, unavailable)
  @objc public func foo() {}
}
