// RUN: %target-typecheck-verify-swift

protocol P {
  @differentiable(reverse) // expected-error {{@differentiable attribute used without importing module '_Differentiation'}}
  func req()
}

struct S: P {
  // Had the `_Differentiation` module been imported, this conformance 
  // requirement would have generated an error, complaining about the 
  // missing `@derivative` attribute. However, because the `_Differentiation`
  // module import is missing, the `@differential` attribute on `P` is marked
  // invalid and therefore not used while validating witnesses (specifically `S.req()`) 
  // for the `P.req()` requirement.
  public func req() {}
}
