// RUN: %target-typecheck-verify-swift -verify-ignore-unknown

// Automatic synthesis of Comparable is only supported for enums for now.

struct NotComparableStruct: Comparable {
  // expected-error@-1 {{type 'NotComparableStruct' does not conform to protocol 'Comparable'}}
  // expected-note@-2 {{automatic synthesis of 'Comparable' is not supported for struct declarations}}
  var value = 0
}

class NotComparableClass: Comparable {
  // expected-error@-1 {{type 'NotComparableClass' does not conform to protocol 'Comparable'}}
  // expected-note@-2 {{automatic synthesis of 'Comparable' is not supported for class declarations}}
  // expected-error@-3 {{type 'NotComparableClass' does not conform to protocol 'Equatable'}}
  // expected-note@-4 {{automatic synthesis of 'Equatable' is not supported for class declarations}}
  var value = 1
}

// Automatic synthesis of Comparable requires enum without raw type.

enum NotComparableEnumOne: Int, Comparable {
  // expected-error@-1 {{type 'NotComparableEnumOne' does not conform to protocol 'Comparable'}}
  // expected-note@-2 {{enum declares raw type 'Int', preventing synthesized conformance of 'NotComparableEnumOne' to 'Comparable'}}
  case value
}

// A potentially unavailable (or unconditionally unavailable) enum case prevents
// automatic synthesis of Comparable requirements.
// FIXME: This should be diagnosed explicitly.

enum EnumWithUnavailableCase: Comparable {
  // expected-error@-1 {{type 'EnumWithUnavailableCase' does not conform to protocol 'Comparable'}}
  case available

  @available(*, unavailable)
  case unavailable
}

enum EnumWithUnavailableCaseAndAssociatedValue: Comparable {
  // expected-error@-1 {{type 'EnumWithUnavailableCaseAndAssociatedValue' does not conform to protocol 'Comparable'}}
  enum SomeComparable: Comparable {}

  case none

  @available(*, unavailable)
  case some(SomeComparable)
}

enum EnumWithUnavailableCaseAndAssociatedValue2: Comparable {
  // expected-error@-1 {{type 'EnumWithUnavailableCaseAndAssociatedValue2' does not conform to protocol 'Comparable'}}
  enum SomeComparable: Comparable {}

  case this(SomeComparable)

  @available(*, unavailable)
  case that(SomeComparable)
}

// Automatic synthesis of Comparable requires associated values to be Comparable as well.

enum NotComparableEnumTwo: Comparable {
  // expected-error@-1 {{type 'NotComparableEnumTwo' does not conform to protocol 'Comparable'}}
  struct NotComparable: Equatable {}
  case value(NotComparable)
  // expected-note@-1 {{associated value type 'NotComparableEnumTwo.NotComparable' does not conform to protocol 'Comparable', preventing synthesized conformance of 'NotComparableEnumTwo' to 'Comparable'}}
}
