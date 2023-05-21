// RUN: %target-swift-frontend -parse -verify %s

#macro

#macro(1)

#macro(1, 2)

_ = #macro(1, 2, 3)

_ = #trailing {
  1
}

_ = #trailing() {
  1
}

_ = #trailing(x) {
  1
}

_ = #trailing(x, y, z) {
  1
} again: {
  2
} yetAgain: {
  3
}

_ = #another {
  // expected-error @+1 {{expected a macro identifier}}
  #-
}

// expected-error @+1 {{expected a macro identifier for a pound literal expression}}
_ = #()

do {
  _ = # // expected-error {{expected a macro identifier for a pound literal expression}}
  name()
}
do {
  _ = # macro() // expected-error {{extraneous whitespace between '#' and macro name is not permitted}} {{8-9=}}
}
do {
  _ = #public() // expected-error {{keyword 'public' cannot be used as an identifier here}} expected-note {{if this name is unavoidable, use backticks to escape it}} {{8-14=`public`}}
}
do {
  _ = # public() // expected-error {{expected a macro identifier for a pound literal expression}}
}
