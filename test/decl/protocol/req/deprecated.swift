// RUN: %target-typecheck-verify-swift

protocol DeprecatedRequirement {
  @available(*, deprecated)
  func f()
}

extension DeprecatedRequirement {
  @available(*, deprecated)
  func f() {}
}

// No warning if both the requirement and the default implementation are deprecated
struct S1: DeprecatedRequirement {}

protocol DeprecatedDefault {
  func f() // expected-note {{requirement 'f()' declared here}}
}

extension DeprecatedDefault {
  @available(*, deprecated)
  func f() {}  // expected-note {{'f()' declared here}}
}

// expected-warning@+1 {{deprecated default implementation is used to satisfy instance method 'f()' required by protocol 'DeprecatedDefault'}}
struct S2: DeprecatedDefault {}

// No warning if the conformance itself is deprecated
@available(*, deprecated)
struct S3: DeprecatedDefault {
}

struct S4: DeprecatedDefault {
  func f() {}
}

struct S5 {}

// No warning if the conformance itself is deprecated
@available(*, deprecated)
extension S5: DeprecatedDefault {}

@available(*, deprecated)
enum UnavailableEnum {
  struct Nested: DeprecatedDefault {}
}

// Include message string from @available attribute if provided
protocol DeprecatedDefaultWithMessage {
  func f() // expected-note {{requirement 'f()' declared here}}
}

extension DeprecatedDefaultWithMessage {
  @available(*, deprecated, message: "write it yourself")
  func f() {} // expected-note {{'f()' declared here}}
}


// expected-warning@+1 {{deprecated default implementation is used to satisfy instance method 'f()' required by protocol 'DeprecatedDefaultWithMessage': write it yourself}}
struct S6: DeprecatedDefaultWithMessage {}
