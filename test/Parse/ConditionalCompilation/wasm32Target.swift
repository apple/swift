// RUN: %swift -typecheck %s -verify -target wasm32-unknown-wasi -disable-objc-interop -parse-stdlib
// RUN: %swift-ide-test -test-input-complete -source-filename %s -target wasm32-unknown-wasi

#if arch(wasm32) && os(WASI) && _runtime(_Native) && _endian(little) && _pointerBitWidth(_32)
#if _hasAtomicBitWidth(_8) && _hasAtomicBitWidth(_16) && _hasAtomicBitWidth(_32) && _hasAtomicBitWidth(_64)
class C {}
var x = C()
#endif
#endif
var y = x

#if !targetEnvironment(threads)
  let z = xx // expected-error {{cannot find 'xx' in scope}}
#endif
