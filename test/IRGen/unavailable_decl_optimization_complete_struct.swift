// RUN: %target-swift-frontend -parse-as-library -module-name Test -validate-tbd-against-ir=missing %s -emit-ir | %FileCheck %s --check-prefixes=CHECK,CHECK-NO-STRIP

// RUN: %target-swift-frontend -parse-as-library -module-name Test -validate-tbd-against-ir=missing -unavailable-decl-optimization=complete %s -emit-ir | %FileCheck %s --check-prefixes=CHECK,CHECK-STRIP

// CHECK: s4Test15AvailableStructVMn

// CHECK-NO-STRIP: s4Test17UnavailableStructVMn
// CHECK-STRIP-NOT: s4Test17UnavailableStructVMn

public struct AvailableStruct<T> {
  // CHECK-NO-STRIP: s4Test15AvailableStructV19unavailablePropertyxvg
  // CHECK-NO-STRIP: s4Test15AvailableStructV19unavailablePropertyxvs
  // CHECK-NO-STRIP: s4Test15AvailableStructV19unavailablePropertyxvM
  // CHECK-STRIP-NOT: s4Test15AvailableStructV19unavailablePropertyxvg
  // CHECK-STRIP-NOT: s4Test15AvailableStructV19unavailablePropertyxvs
  // CHECK-STRIP-NOT: s4Test15AvailableStructV19unavailablePropertyxvM
  @available(*, unavailable)
  public var unavailableProperty: T {
    get { fatalError() }
    set { fatalError() }
    _modify { fatalError() }
  }

  // CHECK-NO-STRIP: s4Test15AvailableStructVyACyxGxcfC
  // CHECK-STRIP-NOT: s4Test15AvailableStructVyACyxGxcfC
  @available(*, unavailable)
  public init(_ t: T) { fatalError() }

  // CHECK: s4Test15AvailableStructVMa

  // CHECK-NO-STRIP: s4Test15AvailableStructV17unavailableMethodyyF
  // CHECK-STRIP-NOT: s4Test15AvailableStructV17unavailableMethodyyF
  @available(*, unavailable)
  public func unavailableMethod() {}
}

@available(*, unavailable)
extension AvailableStruct {
  // CHECK-NO-STRIP: s4Test15AvailableStructV28methodInUnavailableExtensionyyF
  // CHECK-STRIP-NOT: s4Test15AvailableStructV28methodInUnavailableExtensionyyF
  public func methodInUnavailableExtension() {}
}

@available(*, unavailable)
public struct UnavailableStruct<T> {
  // CHECK-NO-STRIP: s4Test17UnavailableStructV8propertyxvg
  // CHECK-NO-STRIP: s4Test17UnavailableStructV8propertyxvs
  // CHECK-NO-STRIP: s4Test17UnavailableStructV8propertyxvM
  // CHECK-STRIP-NOT: s4Test17UnavailableStructV8propertyxvg
  // CHECK-STRIP-NOT: s4Test17UnavailableStructV8propertyxvs
  // CHECK-STRIP-NOT: s4Test17UnavailableStructV8propertyxvM
  public var property: T

  // CHECK-NO-STRIP: s4Test17UnavailableStructVyACyxGxcfC
  // CHECK-NO-STRIP: s4Test17UnavailableStructVMa
  // CHECK-STRIP-NOT: s4Test17UnavailableStructVyACyxGxcfC
  // CHECK-STRIP-NOT: s4Test17UnavailableStructVMa
  public init(_ t: T) {
    self.property = t
  }

  // CHECK-NO-STRIP: s4Test17UnavailableStructV6methodyyF
  // CHECK-STRIP-NOT: s4Test17UnavailableStructV6methodyyF
  public func method() {}
}

@available(*, unavailable)
extension UnavailableStruct {
  // CHECK-NO-STRIP: s4Test17UnavailableStructV15extensionMethodyyF
  // CHECK-STRIP-NOT: s4Test17UnavailableStructV15extensionMethodyyF
  public func extensionMethod() {}
}

// CHECK-NO-STRIP: s4Test17UnavailableStructVMa
// CHECK-STRIP-NOT: s4Test17UnavailableStructVMa
