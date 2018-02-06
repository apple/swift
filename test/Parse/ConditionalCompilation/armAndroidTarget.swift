// RUN: %swift -typecheck %s -verify -D FOO -D BAR -target armv7-none-linux-androideabi -disable-objc-interop -D FOO -parse-stdlib
// RUN: %swift-ide-test -test-input-complete -source-filename=%s -target armv7-none-linux-androideabi

#if os(Linux)
// This block should not parse.
// os(Android) does not imply os(Linux).
let i: Int = "Hello"
#endif

#if arch(arm) && os(Android) && _runtime(_Native) && _endian(little) && _pointer_bit_width(32)
class C {}
var x = C()
#endif
var y = x
