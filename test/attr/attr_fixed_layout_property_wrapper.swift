// RUN: %target-typecheck-verify-swift -swift-version 5 -package-name myPkg

private class PrivateType {} // expected-note {{class 'PrivateType' is not '@usableFromInline' or public}}
// expected-note@-1 {{initializer 'init()' is not '@usableFromInline' or public}}
// expected-note@-2 {{type declared here}}

package class PackageType {
  // expected-note@-1 {{class 'PackageType' is not '@usableFromInline' or public}}
  // expected-note@-2 {{type declared here}}
  package init() {}
  // expected-note@-1 {{initializer 'init()' is not '@usableFromInline' or public}}
}

@usableFromInline
package class PackageTypeForInline {
  @usableFromInline
  package init() {}
}

@propertyWrapper
public struct Wrapper<T> {
  public init(wrappedValue: T) {}

  public var wrappedValue: T { fatalError() }
}

@frozen public struct UsesPrivateType {
  @Wrapper private var y1: PrivateType
  // expected-error@-1 {{type referenced from a stored property in a '@frozen' struct must be '@usableFromInline' or public}}

  @Wrapper private var y2 = PrivateType()
  // expected-error@-1 {{class 'PrivateType' is private and cannot be referenced from a property initializer in a '@frozen' type}}
  // expected-error@-2 {{initializer 'init()' is private and cannot be referenced from a property initializer in a '@frozen' type}}
  // expected-error@-3 {{type referenced from a stored property with inferred type 'PrivateType' in a '@frozen' struct must be '@usableFromInline' or public}}

  @Wrapper private var x1: PackageType
  // expected-error@-1 {{type referenced from a stored property in a '@frozen' struct must be '@usableFromInline' or public}}

  @Wrapper private var x2 = PackageType()
  // expected-error@-1 {{class 'PackageType' is package and cannot be referenced from a property initializer in a '@frozen' type}}
  // expected-error@-2 {{initializer 'init()' is package and cannot be referenced from a property initializer in a '@frozen' type}}
  // expected-error@-3 {{type referenced from a stored property with inferred type 'PackageType' in a '@frozen' struct must be '@usableFromInline' or public}}

  @Wrapper private var z1: PackageTypeForInline
  @Wrapper private var z2 = PackageTypeForInline()
}
