// RUN: %target-typecheck-verify-swift

// FIXME: No simd module on linux rdar://problem/20795411
// XFAIL: linux, windows, openbsd
// XFAIL: OS=wasi

import simd

let a = int4(0) + int4(0) // expected-error{{'+' is unavailable: integer vector types do not support checked arithmetic; use the wrapping operator '&+' instead}}
let b = int4(0) - int4(0) // expected-error{{'-' is unavailable: integer vector types do not support checked arithmetic; use the wrapping operator '&-' instead}}
let c = int4(0) * int4(0) // expected-error{{'*' is unavailable: integer vector types do not support checked arithmetic; use the wrapping operator '&*' instead}}
let x = int4(0) * (0 as Int32) // expected-error{{'*' is unavailable: integer vector types do not support checked arithmetic; use the wrapping operator '&*' instead}}
let y = (0 as Int32) * int4(0) // expected-error{{'*' is unavailable: integer vector types do not support checked arithmetic; use the wrapping operator '&*' instead}}

var d = int4(0)
d += int4(0) // expected-error{{'+=' is unavailable: integer vector types do not support checked arithmetic; use the wrapping operator '&+=' instead}}
d -= int4(0) // expected-error{{'-=' is unavailable: integer vector types do not support checked arithmetic; use the wrapping operator '&-=' instead}}
d *= int4(0) // expected-error{{'*=' is unavailable: integer vector types do not support checked arithmetic; use the wrapping operator '&*=' instead}}
d *= 0 // expected-error{{'*=' is unavailable: integer vector types do not support checked arithmetic; use the wrapping operator '&*=' instead}}

