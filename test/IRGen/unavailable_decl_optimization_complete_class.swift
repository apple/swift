// RUN: %target-swift-frontend -parse-as-library -module-name Test -validate-tbd-against-ir=missing %s -emit-ir | %FileCheck %s --check-prefixes=CHECK,CHECK-NO-STRIP

// RUN: %target-swift-frontend -parse-as-library -module-name Test -validate-tbd-against-ir=missing -unavailable-decl-optimization=complete %s -emit-ir | %FileCheck %s --check-prefixes=CHECK,CHECK-STRIP

public class AvailableClass<T> {
  // CHECK-NO-STRIP: s4Test14AvailableClassC19unavailablePropertyxvg
  // CHECK-NO-STRIP: s4Test14AvailableClassC19unavailablePropertyxvs
  // CHECK-NO-STRIP: s4Test14AvailableClassC19unavailablePropertyxvM
  // CHECK-STRIP-NOT: s4Test14AvailableClassC19unavailablePropertyxvg
  // CHECK-STRIP-NOT: s4Test14AvailableClassC19unavailablePropertyxvs
  // CHECK-STRIP-NOT: s4Test14AvailableClassC19unavailablePropertyxvM
  @available(*, unavailable)
  public var unavailableProperty: T {
    get { fatalError() }
    set { fatalError() }
    _modify { fatalError() }
  }

  // CHECK-NO-STRIP: s4Test14AvailableClassCyACyxGxcfC
  // CHECK-NO-STRIP: s4Test14AvailableClassCyACyxGxcfc
  // CHECK-STRIP-NOT: s4Test14AvailableClassCyACyxGxcfC
  // CHECK-STRIP-NOT: s4Test14AvailableClassCyACyxGxcfc
  @available(*, unavailable)
  public init(_ t: T) { fatalError() }

  // CHECK: s4Test14AvailableClassCfd
  // CHECK: s4Test14AvailableClassCfD
  deinit {}
}

@available(*, unavailable)
public class UnavailableClass<T> {
  // CHECK-NO-STRIP: s4Test16UnavailableClassC8propertyxvg
  // CHECK-NO-STRIP: s4Test16UnavailableClassC8propertyxvs
  // CHECK-NO-STRIP: s4Test16UnavailableClassC8propertyxvM
  // CHECK-STRIP-NOT: s4Test16UnavailableClassC8propertyxvg
  // CHECK-STRIP-NOT: s4Test16UnavailableClassC8propertyxvs
  // CHECK-STRIP-NOT: s4Test16UnavailableClassC8propertyxvM
  public var property: T

  // CHECK-NO-STRIP: s4Test16UnavailableClassCyACyxGxcfC
  // CHECK-NO-STRIP: s4Test16UnavailableClassCyACyxGxcfc
  // CHECK-STRIP-NOT: s4Test16UnavailableClassCyACyxGxcfC
  // CHECK-STRIP-NOT: s4Test16UnavailableClassCyACyxGxcfc
  public init(_ t: T) {
    self.property = t
  }

  // CHECK-NO-STRIP: s4Test16UnavailableClassCfd
  // CHECK-NO-STRIP: s4Test16UnavailableClassCfD
  // CHECK-STRIP-NOT: s4Test16UnavailableClassCfd
  // CHECK-STRIP-NOT: s4Test16UnavailableClassCfD
  deinit {}
}

// CHECK: s4Test14AvailableClassCMa

// CHECK-NO-STRIP: s4Test16UnavailableClassCMa
// CHECK-STRIP-NOT: s4Test16UnavailableClassCMa
