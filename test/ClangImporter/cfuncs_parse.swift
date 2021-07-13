// XFAIL: CPU=powerpc64le
// RUN: %target-swift-frontend(mock-sdk: %clang-importer-sdk) -typecheck -verify -I %S/Inputs %s

import cfuncs

func test_cfunc1(_ i: Int) {
  cfunc1() // okay
  cfunc1(i) // expected-error{{argument passed to call that takes no arguments}}
}

func test_cfunc2(_ i: Int) {
#if os(Windows) && (arch(arm64) || arch(x86_64))
  // LLP64 targets will import `long` as `Int32`
  let f = cfunc2(Int32(i), 17)
#else
  let f = cfunc2(i, 17)
#endif
  _ = f as Float
  cfunc2(b:17, a:i) // expected-error{{extraneous argument labels 'b:a:' in call}}
  // expected-error@-1 {{cannot convert value of type 'Int' to expected argument type 'Int32'}}
  cfunc2(17, i) // expected-error{{cannot convert value of type 'Int' to expected argument type 'Int32'}}
}

func test_cfunc3_a() {
  let b = cfunc3( { (a : Double, b : Double) -> Double in a + b } )
  _ = b(1.5, 2.5) as Double // expected-error{{value of optional type 'double_bin_op_block?' (aka 'Optional<(Double, Double) -> Double>') must be unwrapped to a value of type 'double_bin_op_block' (aka '(Double, Double) -> Double')}}
  // expected-note@-1{{coalesce}}
  // expected-note@-2{{force-unwrap}}
  _ = b!(1.5, 2.5) as Double
  _ = b as Double// expected-error{{cannot convert value of type 'double_bin_op_block?' (aka 'Optional<(Double, Double) -> Double>') to type 'Double' in coercion}}
}

func test_cfunc3_b() {
  let b = cfunc3( { a, b in a + b } )
  _ = b(1.5, 2.5) as Double // expected-error{{value of optional type 'double_bin_op_block?' (aka 'Optional<(Double, Double) -> Double>') must be unwrapped to a value of type 'double_bin_op_block' (aka '(Double, Double) -> Double')}}
  // expected-note@-1{{coalesce}}
  // expected-note@-2{{force-unwrap}}
  _ = b!(1.5, 2.5) as Double
  _ = b as Double// expected-error{{cannot convert value of type 'double_bin_op_block?' (aka 'Optional<(Double, Double) -> Double>') to type 'Double' in coercion}}
}

func test_cfunc3_c() {
  let b = cfunc3({ $0 + $1 })
  _ = b(1.5, 2.5) as Double // expected-error{{value of optional type 'double_bin_op_block?' (aka 'Optional<(Double, Double) -> Double>') must be unwrapped to a value of type 'double_bin_op_block' (aka '(Double, Double) -> Double')}}
  // expected-note@-1{{coalesce}}
  // expected-note@-2{{force-unwrap}}
  _ = b!(1.5, 2.5) as Double
  _ = b as Double// expected-error{{cannot convert value of type 'double_bin_op_block?' (aka 'Optional<(Double, Double) -> Double>') to type 'Double' in coercion}}
}

func test_cfunc3_d() {
  let x: Double = 0
  let y: Double = 0
  _ = cfunc3(nil)?(x, y) as Double?
  _ = cfunc3(nil)!(x, y) as Double
}

func test_cfunc4() {
  // Okay: has no prototype, so assume no parameters.
  cfunc4()
}

func test_pow() {
  pow(1.5, 2.5)
}

#if !((os(Android) || os(Linux)) && arch(arm64)) && !os(FreeBSD)
// long doubles in AAPCS64 are 128 bits, which is not supported by
// Swift, so don't test this. SR-9072.
func test_powl() {
  powl(1.5, 2.5)
}
#endif

#if !os(macOS) && !(os(iOS) && targetEnvironment(macCatalyst))
@available(iOS 14.0, watchOS 7.0, tvOS 14.0, *)
func test_f16() {
  var x = Float16.zero
  f16ptrfunc(&x)
  #if arch(arm) || arch(arm64)
  f16func(x)
  #endif
}
#endif

func test_puts(_ s: String) {
  _ = s.withCString { puts($0) + 32 };
}

func test_fopen(_ filename: String) -> CInt {
  let file = filename.withCString { fopen($0, "r")! }
  return file.pointee.inode
}

func test_cfunc_in_swift() -> Int {
  return cfunc_in_swift(5)
}

func test_inline_available() {
  createSomething()
}

func test_pointer() {
  var i: CInt = 0
  var ia: [CInt] = [1, 2, 3]
  var f: CFloat = 0
  var fa: [CFloat] = [1, 2, 3]

  param_pointer(nil as UnsafeMutablePointer<CInt>?)
  param_pointer(&i)
  param_pointer(&ia)

  param_const_pointer(nil as UnsafeMutablePointer<CInt>?)
  param_const_pointer(&i)
  param_const_pointer(ia)
  param_const_pointer([1, 2, 3])

  param_void_pointer(nil as UnsafeMutableRawPointer?)
  param_void_pointer(nil as UnsafeMutablePointer<CInt>?)
  param_void_pointer(nil as UnsafeMutablePointer<CFloat>?)
  param_void_pointer(&i)
  param_void_pointer(&ia)
  param_void_pointer(&f)
  param_void_pointer(&fa)

  param_const_void_pointer(nil as UnsafeMutableRawPointer?)
  param_const_void_pointer(nil as UnsafeMutablePointer<CInt>?)
  param_const_void_pointer(nil as UnsafeMutablePointer<CFloat>?)
  param_const_void_pointer(nil as UnsafeRawPointer?)
  param_const_void_pointer(nil as UnsafePointer<CInt>?)
  param_const_void_pointer(nil as UnsafePointer<CFloat>?)
  param_const_void_pointer(&i)
  param_const_void_pointer(ia)
  // FIXME: param_const_void_pointer([1, 2, 3])
  param_const_void_pointer(&f)
  param_const_void_pointer(fa)
  // FIXME: param_const_void_pointer([1.0, 2.0, 3.0])

  let op: OpaquePointer?
  opaque_pointer_param(op)
}

func test_pointer_nonnull() {
  var i: CInt = 0
  var ia: [CInt] = [1, 2, 3]
  var f: CFloat = 0
  var fa: [CFloat] = [1, 2, 3]

  nonnull_param_pointer(&i)
  nonnull_param_pointer(&ia)

  nonnull_param_const_pointer(&i)
  nonnull_param_const_pointer(ia)
  nonnull_param_const_pointer([1, 2, 3])

  nonnull_param_void_pointer(&i)
  nonnull_param_void_pointer(&ia)
  nonnull_param_void_pointer(&f)
  nonnull_param_void_pointer(&fa)

  nonnull_param_const_void_pointer(&i)
  nonnull_param_const_void_pointer(ia)
  // FIXME: nonnull_param_const_void_pointer([1, 2, 3])
  nonnull_param_const_void_pointer(&f)
  nonnull_param_const_void_pointer(fa)
  // FIXME: nonnull_param_const_void_pointer([1.0, 2.0, 3.0])
}

func test_decay() {
  decay_param_array(nil as UnsafeMutablePointer<CInt>?)
  var i: CInt = 0
  var a: [CInt] = [1, 2, 3]
  decay_param_array(&i)
  decay_param_array(&a)
  decay_param_const_array(nil as UnsafeMutablePointer<CInt>?)
  decay_param_const_array(&i)
  decay_param_const_array(a)
  decay_param_const_array([1, 2, 3])
}

func test_nested_pointers() {
  nested_pointer(nil)
  nested_pointer_audited(nil)
  nested_pointer_audited2(nil) // expected-error {{'nil' is not compatible with expected argument type 'UnsafePointer<UnsafePointer<Int32>?>'}}

  nested_pointer(0) // expected-error {{expected argument type 'UnsafePointer<UnsafePointer<Int32>?>?'}}
  nested_pointer_audited(0) // expected-error {{expected argument type 'UnsafePointer<UnsafePointer<Int32>>?'}}
  nested_pointer_audited2(0) // expected-error {{expected argument type 'UnsafePointer<UnsafePointer<Int32>?>'}}
}

func exit(_: Float) {} // expected-note {{found this candidate}}
func test_ambiguous() {
  exit(5) // expected-error {{ambiguous use of 'exit'}}
}

