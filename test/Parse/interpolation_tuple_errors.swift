// RUN: %target-typecheck-verify-swift

let str = "a"

// Test with too many interpolated values.
func papaBear() {
  _ = "\(str, str)" // expected-error{{interpolations should be a single expression}} expected-note {{insert parentheses to form a tuple}} {{9-9=(}} {{19-19=)}}
}

// Test with too few interpolated values.
func mamaBear() {
  _ = "\()" // expected-error{{interpolations should be a single expression}} expected-note {{delete empty interpolation}} {{8-11=}}
}

// Test with the correct number of interpolated values.
func babyBear() {
  _ = "\(str)"
}

// Test with an argument label
func funkyBear() {
  _ = "\(describing: str)" // expected-error{{interpolations cannot start with a keyword argument}} {{10-22=}}
  _ = "\(format: str)" // expected-error{{interpolations cannot start with a keyword argument}} {{10-18=}}
  _ = "\(validatingUTF8: str)" // expected-error{{interpolations cannot start with a keyword argument}} {{10-26=}}
  _ = "\(fnord: str)" // expected-error{{interpolations cannot start with a keyword argument}} {{10-17=}}
}
