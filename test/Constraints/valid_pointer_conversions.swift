// RUN: %target-typecheck-verify-swift

func foo(_ a: [[UInt8]], _ p: [UnsafeRawPointer]) {
  foo(a, a) // expect-warning {{all paths through this function will call itself}}
}

// rdar://problem/44658089
func takesPtr(_: UnsafePointer<UInt8>) {}

func takesDoubleOptionalPtr(_ x: UnsafeRawPointer??) {}
func takesMutableDoubleOptionalPtr(_ x: UnsafeMutableRawPointer??) {}
func takesMutableDoubleOptionalTypedPtr(_ x: UnsafeMutablePointer<Double>??) {}

func givesPtr(_ str: String) {
  takesPtr(UnsafePointer(str)) // expected-warning {{initialization of 'UnsafePointer<UInt8>' results in a dangling pointer}}
  // expected-note @-1 {{implicit argument conversion from 'String' to 'UnsafePointer<UInt8>' produces a pointer valid only for the duration of the call to 'init(_:)'}}
  // expected-note@-2 {{use the 'withCString' method on String in order to explicitly convert argument to pointer valid for a defined scope}}

  var i = 0
  var d = 0.0
  var arr = [1, 2, 3]

  // SR-9090: Allow double optional promotion for pointer conversions.
  takesDoubleOptionalPtr(&arr)
  takesDoubleOptionalPtr(arr)
  takesDoubleOptionalPtr(str)
  takesMutableDoubleOptionalPtr(&i)
  takesMutableDoubleOptionalPtr(&arr)
  takesMutableDoubleOptionalTypedPtr(&d)

  takesDoubleOptionalPtr(i) // expected-error {{cannot convert value of type 'Int' to expected argument type 'UnsafeRawPointer??'}}
  takesMutableDoubleOptionalPtr(arr) // expected-error {{cannot convert value of type '[Int]' to expected argument type 'UnsafeMutableRawPointer??'}}

  // FIXME(SR-12382): Poor diagnostic.
  takesMutableDoubleOptionalTypedPtr(&i) // expected-error {{type of expression is ambiguous without more context}}
}
