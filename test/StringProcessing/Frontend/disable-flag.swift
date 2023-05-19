// RUN: %target-typecheck-verify-swift -enable-bare-slash-regex -disable-experimental-string-processing
// RUN: %target-typecheck-verify-swift -disable-experimental-string-processing -enable-bare-slash-regex
// RUN: %target-typecheck-verify-swift -enable-experimental-string-processing -disable-experimental-string-processing -enable-bare-slash-regex

prefix operator /

_ = /x/
// expected-error@-1 {{'/' is not a prefix unary operator}}
// expected-error@-2 {{cannot find 'x' in scope}}
// expected-error@-3 {{'/' is not a postfix unary operator}}

_ = #/x/# // expected-error {{expected a macro identifier}}

func foo(_ x: Regex<Substring>) {} // expected-error {{cannot find type 'Regex' in scope}}
