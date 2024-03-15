// RUN: %target-swift-frontend -parse-as-library -emit-sil -enforce-exclusivity=checked -primary-file %s -o /dev/null -verify

// AccessEnforcementWMO assumes that the only way to address a global or static
// property is via a formal begin_access. If we ever allow keypaths for static
// properties, which I think is conceivable, then this test will fail to produce
// a diagnostic error and AccessEnforcementWMO must be fixed to handle "meta"
// keypaths.
public class C {
  public static var globalProp: Int = 0
}

public func testGlobalProp() {
  let a: AnyKeyPath = \C.globalProp // expected-error{{static member 'globalProp' cannot be used on instance of type 'C'}}
}
