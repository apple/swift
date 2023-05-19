// RUN: %target-typecheck-verify-swift

let _ = "foo\(#"bar"##)baz"
// expected-error@-1{{too many '#' characters in closing delimiter}}
// expected-error@-2{{expected ',' separator}}
// expected-error@-3{{expected a macro identifier}}

let _ = #"\##("invalid")"#
// expected-error@-1{{too many '#' characters in delimited escape}}
// expected-error@-2{{invalid escape sequence in literal}}

let _ = ###"""invalid"######
// expected-error@-1{{too many '#' characters in closing delimiter}}{{26-29=}}
// expected-error@-2{{consecutive statements on a line must be separated by ';'}}
// expected-error@-3{{expected a macro identifier}}

let _ = ####"invalid"###
// expected-error@-1{{unterminated string literal}}

let _ = ###"invalid"######
// expected-error@-1{{too many '#' characters in closing delimiter}}{{24-27=}}
// expected-error@-2{{consecutive statements on a line must be separated by ';'}}
// expected-error@-3{{expected a macro identifier}}

let _ = ##"""aa
  foobar
  aa"""##
// expected-error@-3{{multi-line string literal content must begin on a new line}}{{14-14=\n}}
// expected-error@-2{{multi-line string literal closing delimiter must begin on a new line}}{{5-5=\n}}

let _ = #""" foo "bar" #baz
  """#
// expected-error@-2{{multi-line string literal content must begin on a new line}}{{13-13=\n}}

let _ = ###""" "# "##
  """###
// expected-error@-2{{multi-line string literal content must begin on a new line}}{{15-15=\n}}
