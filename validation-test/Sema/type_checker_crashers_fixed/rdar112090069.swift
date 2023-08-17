// RUN: %target-typecheck-verify-swift -disable-availability-checking

func test<each Before,
          each At,
          each After,
          each Return>(
      _ fn: (repeat each Before, repeat each At, repeat each After) -> (repeat each Return),
      at: repeat each At) -> (repeat each Before, repeat each After) -> (repeat each Return) {
  return { (before: repeat each Before, after: repeat each After) in // expected-error {{no parameters may follow a variadic parameter in a closure}}
    return fn(repeat each before, repeat each at, repeat each after)
    // expected-error@-1 {{pack expansion requires that 'each At' and 'each Before' have the same shape}}
    // expected-error@-2 {{cannot convert value of type 'each At' to expected argument type 'each Before'}}
    // expected-error@-3 {{pack expansion requires that 'each After' and 'each Before' have the same shape}}
    // expected-error@-4 {{cannot convert value of type 'each After' to expected argument type 'each Before'}}
  }
}
