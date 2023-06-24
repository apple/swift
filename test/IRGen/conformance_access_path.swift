// RUN: %target-swift-frontend -primary-file %s -emit-ir > %t.ll
// RUN: %FileCheck %s < %t.ll


// https://github.com/apple/swift/issues/48752
// Canonicalizing a conformance access path that was built without the
// requirement signature.

public struct Valid<V> {}

extension Valid where V: ValidationSuite {}

public protocol Validatable {}

extension Validatable {
    public func tested() {}

    // CHECK-LABEL: define{{.*}}$s23conformance_access_path11ValidatablePAAE6tested2byyqd__m_t9InputTypeQyd__RszAA15ValidationSuiteRd__lF
  public func tested<S: ValidationSuite>(by suite: S.Type) where S.InputType == Self {
      // CHECK:   [[S_AS_VALIDATION_SUITE_GEP:%[0-9]+]] = getelementptr inbounds ptr, ptr %S.ValidationSuite, i32 1
      // CHECK:   [[S_AS_VALIDATION_SUITE:%.*]] = load ptr, ptr [[S_AS_VALIDATION_SUITE_GEP]]
      // CHECK-NEXT: call swiftcc ptr @swift_getAssociatedConformanceWitness(ptr %S.Validator, ptr %S, ptr %Self,
      tested()
    }
}

public protocol Validator {
    associatedtype InputType: Validatable
}

public protocol ValidationSuite: Validator {
    associatedtype InputType: Validatable
}
