// RUN: %target-typecheck-verify-swift

func debugPrint<each T>(_ items: repeat each T)
  where repeat each T: CustomDebugStringConvertible
{
  /*for (item: T) in items {
    stdout.write(item.debugDescription)
  }*/
}

func max<each T>(_ values: repeat each T) -> (repeat each T)?
  where repeat each T: Comparable
{
  return nil
}

func min<each T: Comparable>(_ values: repeat each T) -> (repeat each T)? {
  return nil
}

func invalidPacks() {
  func monovariadic1() -> (each String) {} // expected-error {{'each' cannot be applied to non-pack type 'String'}}{{28-32=}}
  func monovariadic2<T>() -> (repeat T) {} // expected-error {{pack expansion 'T' must contain at least one pack reference}}
  func monovariadic3<T, U>() -> (T, repeat U) {} // expected-error {{pack expansion 'U' must contain at least one pack reference}}
}

func call() {
  func multipleParameters<each T>(xs: repeat each T, ys: repeat each T) -> (repeat each T) {
    return (repeat each xs)
  }
  multipleParameters()

  let x: String = multipleParameters(xs: "", ys: "")
  let (one, two) = multipleParameters(xs: "", 5.0, ys: "", 5.0)
  multipleParameters(xs: "", 5.0, ys: 5.0, "") // expected-error {{conflicting arguments to generic parameter 'each T' ('Pack{Double, String}' vs. 'Pack{String, String}' vs. 'Pack{String, Double}' vs. 'Pack{Double, Double}')}}

  func multipleSequences<each T, each U>(xs: repeat each T, ys: repeat each U) -> (repeat each T) {
    return (repeat each ys)
    // expected-error@-1 {{pack expansion requires that 'each U' and 'each T' have the same shape}}
    // expected-error@-2 {{cannot convert return expression of type '(repeat each U)' to return type '(repeat each T)'}}
  }

  func multipleSequencesWithSameShape<each T, each U>(xs: repeat each T, ys: repeat each U) -> (repeat each T) where (repeat (each T, each U)): Any {
    return (repeat each ys)
    // expected-error@-1 {{cannot convert return expression of type '(repeat each U)' to return type '(repeat each T)'}}
  }

  multipleSequences()
  _ = multipleSequences(xs: "", ys: "")
  _ = multipleSequences(xs: "", 5.0, ys: 5.0, "")
}

func contextualTyping() {
  func firsts<each T>(_ seqs: repeat [each T]) -> (repeat (each T)?) {
    fatalError()
  }

  let (_, _): (Int?, String?) = firsts([42], [""]) // OK
  let (_, _): (String?, String?) = firsts([42], [""]) // expected-error {{cannot convert value of type '((Int)?, (String)?)' to specified type '(String?, String?)'}}
  let (_, _): ([Int], String?) = firsts([42], [""]) // expected-error {{cannot convert value of type '((Int)?, (String)?)' to specified type '([Int], String?)'}}
  let (_, _, _): (String?, String?, Int) = firsts([42], [""]) // expected-error {{'((Int)?, (String)?)' is not convertible to '(String?, String?, Int)', tuples have a different number of elements}}

  func dependent<each T>(_ seqs: repeat Array<each T>) -> (repeat Array<each T>.Element?) {
    fatalError()
  }

  let (_, _): (Int?, String?) = dependent([42], [""]) // OK
  let (_, _): (String?, String?) = dependent([42], [""]) // expected-error {{cannot convert value of type '(Int?, String?)' to specified type '(String?, String?)'}}
  let (_, _): ([Int], String?) = dependent([42], [""]) // expected-error {{cannot convert value of type '(Int?, String?)' to specified type '([Int], String?)'}}
  let (_, _, _): (String?, String?, Int) = dependent([42], [""]) // expected-error {{'(Int?, String?)' is not convertible to '(String?, String?, Int)', tuples have a different number of elements}}
}

// rdar://106737972 - crash-on-invalid with default argument
do {
  func foo<each T>(_: repeat each T = bar().element) {} // expected-note {{in call to function 'foo'}}
  // expected-error@-1 {{variadic parameter cannot have a default value}}
  // expected-error@-2 {{value pack expansion can only appear inside a function argument list or tuple element}}
  // expected-error@-3 {{generic parameter 'each T' could not be inferred}}

  func bar<each T>() -> (repeat each T) {}
}
