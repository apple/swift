// RUN: %target-typecheck-verify-swift

// FIXME: No simd module on linux rdar://problem/20795411
// XFAIL: linux, windows, openbsd

import simd

let a = SIMD4<Int32>(repeating: 0) + SIMD4<Int32>(repeating: 0) // expected-error{{'+' is unavailable: integer vector types do not support checked arithmetic; use the wrapping operator '&+' instead}}
let b = SIMD4<Int32>(repeating: 0) - SIMD4<Int32>(repeating: 0) // expected-error{{'-' is unavailable: integer vector types do not support checked arithmetic; use the wrapping operator '&-' instead}}
let c = SIMD4<Int32>(repeating: 0) * SIMD4<Int32>(repeating: 0) // expected-error{{'*' is unavailable: integer vector types do not support checked arithmetic; use the wrapping operator '&*' instead}}
let x = SIMD4<Int32>(repeating: 0) * (0 as Int32) // expected-error{{'*' is unavailable: integer vector types do not support checked arithmetic; use the wrapping operator '&*' instead}}
let y = (0 as Int32) * SIMD4<Int32>(repeating: 0) // expected-error{{'*' is unavailable: integer vector types do not support checked arithmetic; use the wrapping operator '&*' instead}}

var d = SIMD4<Int32>(repeating: 0)
d += SIMD4<Int32>(repeating: 0) // expected-error{{'+=' is unavailable: integer vector types do not support checked arithmetic; use the wrapping operator '&+=' instead}}
d -= SIMD4<Int32>(repeating: 0) // expected-error{{'-=' is unavailable: integer vector types do not support checked arithmetic; use the wrapping operator '&-=' instead}}
d *= SIMD4<Int32>(repeating: 0) // expected-error{{'*=' is unavailable: integer vector types do not support checked arithmetic; use the wrapping operator '&*=' instead}}
d *= 0 // expected-error{{'*=' is unavailable: integer vector types do not support checked arithmetic; use the wrapping operator '&*=' instead}}

