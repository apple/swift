// RUN: %target-typecheck-verify-swift -disable-objc-interop

// Test that we get a custom diagnostic for an ephemeral conversion to non-ephemeral param for an Unsafe[Mutable][Raw][Buffer]Pointer init.
func unsafePointerInitEphemeralConversions() {
  class C {}
  var foo = 0
  var str = ""
  var arr = [0]
  var optionalArr: [Int]? = [0]
  var c: C?

  _ = UnsafePointer(&foo) // expected-warning {{initialization of 'UnsafePointer<Int>' results in a dangling pointer}} {{educational-notes=temporary-pointers}}
  // expected-note@-1 {{implicit argument conversion from 'Int' to 'UnsafePointer<Int>' produces a pointer valid only for the duration of the call to 'init(_:)'}}
  // expected-note@-2 {{use 'withUnsafePointer' in order to explicitly convert argument to pointer valid for a defined scope}}

  _ = UnsafePointer(&foo + 1) // expected-warning {{inout expression creates a temporary pointer, but argument #1 should be a pointer that outlives the call to '+'}}
  // expected-note@-1 {{implicit argument conversion from 'Int' to 'UnsafePointer<Int>' produces a pointer valid only for the duration of the call to '+'}}
  // expected-note@-2 {{use 'withUnsafePointer' in order to explicitly convert argument to pointer valid for a defined scope}}

  _ = UnsafePointer.init(&foo) // expected-warning {{initialization of 'UnsafePointer<Int>' results in a dangling pointer}}
  // expected-note@-1 {{implicit argument conversion from 'Int' to 'UnsafePointer<Int>' produces a pointer valid only for the duration of the call to 'init(_:)'}}
  // expected-note@-2 {{use 'withUnsafePointer' in order to explicitly convert argument to pointer valid for a defined scope}}

  _ = UnsafePointer<Int8>("") // expected-warning {{initialization of 'UnsafePointer<Int8>' results in a dangling pointer}}
  // expected-note@-1 {{implicit argument conversion from 'String' to 'UnsafePointer<Int8>' produces a pointer valid only for the duration of the call to 'init(_:)'}}
  // expected-note@-2 {{use the 'withCString' method on String in order to explicitly convert argument to pointer valid for a defined scope}}

  _ = UnsafePointer<Int8>.init("") // expected-warning {{initialization of 'UnsafePointer<Int8>' results in a dangling pointer}}
  // expected-note@-1 {{implicit argument conversion from 'String' to 'UnsafePointer<Int8>' produces a pointer valid only for the duration of the call to 'init(_:)'}}
  // expected-note@-2 {{use the 'withCString' method on String in order to explicitly convert argument to pointer valid for a defined scope}}

  _ = UnsafePointer<Int8>(str) // expected-warning {{initialization of 'UnsafePointer<Int8>' results in a dangling pointer}}
  // expected-note@-1 {{implicit argument conversion from 'String' to 'UnsafePointer<Int8>' produces a pointer valid only for the duration of the call to 'init(_:)'}}
  // expected-note@-2 {{use the 'withCString' method on String in order to explicitly convert argument to pointer valid for a defined scope}}

  _ = UnsafePointer([0]) // expected-warning {{initialization of 'UnsafePointer<Int>' results in a dangling pointer}}
  // expected-note@-1 {{implicit argument conversion from '[Int]' to 'UnsafePointer<Int>' produces a pointer valid only for the duration of the call to 'init(_:)'}}
  // expected-note@-2 {{use the 'withUnsafeBufferPointer' method on Array in order to explicitly convert argument to buffer pointer valid for a defined scope}}

  _ = UnsafePointer(arr) // expected-warning {{initialization of 'UnsafePointer<Int>' results in a dangling pointer}}
  // expected-note@-1 {{implicit argument conversion from '[Int]' to 'UnsafePointer<Int>' produces a pointer valid only for the duration of the call to 'init(_:)'}}
  // expected-note@-2 {{use the 'withUnsafeBufferPointer' method on Array in order to explicitly convert argument to buffer pointer valid for a defined scope}}

  _ = UnsafePointer(&arr) // expected-warning {{initialization of 'UnsafePointer<Int>' results in a dangling pointer}}
  // expected-note@-1 {{implicit argument conversion from '[Int]' to 'UnsafePointer<Int>' produces a pointer valid only for the duration of the call to 'init(_:)'}}
  // expected-note@-2 {{use the 'withUnsafeBufferPointer' method on Array in order to explicitly convert argument to buffer pointer valid for a defined scope}}

  _ = UnsafePointer(optionalArr) // expected-warning {{initialization of 'UnsafePointer<Int>' results in a dangling pointer}}
  // expected-note@-1 {{implicit argument conversion from '[Int]?' to 'UnsafePointer<Int>?' produces a pointer valid only for the duration of the call to 'init(_:)'}}


  _ = UnsafeMutablePointer(&foo) // expected-warning {{initialization of 'UnsafeMutablePointer<Int>' results in a dangling pointer}}
  // expected-note@-1 {{implicit argument conversion from 'Int' to 'UnsafeMutablePointer<Int>' produces a pointer valid only for the duration of the call to 'init(_:)'}}
  // expected-note@-2 {{use 'withUnsafeMutablePointer' in order to explicitly convert argument to pointer valid for a defined scope}}

  _ = UnsafeMutablePointer(&arr) // expected-warning {{initialization of 'UnsafeMutablePointer<Int>' results in a dangling pointer}}
  // expected-note@-1 {{implicit argument conversion from '[Int]' to 'UnsafeMutablePointer<Int>' produces a pointer valid only for the duration of the call to 'init(_:)'}}
  // expected-note@-2 {{use the 'withUnsafeMutableBufferPointer' method on Array in order to explicitly convert argument to buffer pointer valid for a defined scope}}

  _ = UnsafeMutablePointer(&arr + 2) // expected-warning {{inout expression creates a temporary pointer, but argument #1 should be a pointer that outlives the call to '+'}}
  // expected-note@-1 {{implicit argument conversion from '[Int]' to 'UnsafeMutablePointer<Int>' produces a pointer valid only for the duration of the call to '+'}}
  // expected-note@-2 {{use the 'withUnsafeMutableBufferPointer' method on Array in order to explicitly convert argument to buffer pointer valid for a defined scope}}

  _ = UnsafeMutablePointer(mutating: &foo) // expected-warning {{initialization of 'UnsafeMutablePointer<Int>' results in a dangling pointer}}
  // expected-note@-1 {{implicit argument conversion from 'Int' to 'UnsafePointer<Int>' produces a pointer valid only for the duration of the call to 'init(mutating:)}}
  // expected-note@-2 {{use 'withUnsafePointer' in order to explicitly convert argument to pointer valid for a defined scope}}

  _ = UnsafeMutablePointer<Int8>(mutating: "") // expected-warning {{initialization of 'UnsafeMutablePointer<Int8>' results in a dangling pointer}}
  // expected-note@-1 {{implicit argument conversion from 'String' to 'UnsafePointer<Int8>' produces a pointer valid only for the duration of the call to 'init(mutating:)}}
  // expected-note@-2 {{use the 'withCString' method on String in order to explicitly convert argument to pointer valid for a defined scope}}

  _ = UnsafeMutablePointer<Int8>(mutating: str) // expected-warning {{initialization of 'UnsafeMutablePointer<Int8>' results in a dangling pointer}}
  // expected-note@-1 {{implicit argument conversion from 'String' to 'UnsafePointer<Int8>' produces a pointer valid only for the duration of the call to 'init(mutating:)}}
  // expected-note@-2 {{use the 'withCString' method on String in order to explicitly convert argument to pointer valid for a defined scope}}

  _ = UnsafeMutablePointer(mutating: [0]) // expected-warning {{initialization of 'UnsafeMutablePointer<Int>' results in a dangling pointer}}
  // expected-note@-1 {{implicit argument conversion from '[Int]' to 'UnsafePointer<Int>' produces a pointer valid only for the duration of the call to 'init(mutating:)}}
  // expected-note@-2 {{use the 'withUnsafeBufferPointer' method on Array in order to explicitly convert argument to buffer pointer valid for a defined scope}}

  _ = UnsafeMutablePointer(mutating: arr) // expected-warning {{initialization of 'UnsafeMutablePointer<Int>' results in a dangling pointer}}
  // expected-note@-1 {{implicit argument conversion from '[Int]' to 'UnsafePointer<Int>' produces a pointer valid only for the duration of the call to 'init(mutating:)}}
  // expected-note@-2 {{use the 'withUnsafeBufferPointer' method on Array in order to explicitly convert argument to buffer pointer valid for a defined scope}}

  _ = UnsafeMutablePointer(mutating: &arr) // expected-warning {{initialization of 'UnsafeMutablePointer<Int>' results in a dangling pointer}}
  // expected-note@-1 {{implicit argument conversion from '[Int]' to 'UnsafePointer<Int>' produces a pointer valid only for the duration of the call to 'init(mutating:)}}
  // expected-note@-2 {{use the 'withUnsafeBufferPointer' method on Array in order to explicitly convert argument to buffer pointer valid for a defined scope}}

  _ = UnsafeMutablePointer(mutating: optionalArr) // expected-warning {{initialization of 'UnsafeMutablePointer<Int>' results in a dangling pointer}}
  // expected-note@-1 {{implicit argument conversion from '[Int]?' to 'UnsafePointer<Int>?' produces a pointer valid only for the duration of the call to 'init(mutating:)}}


  _ = UnsafeRawPointer(&foo) // expected-warning {{initialization of 'UnsafeRawPointer' results in a dangling pointer}}
  // expected-note@-1 {{implicit argument conversion from 'Int' to 'UnsafeMutableRawPointer' produces a pointer valid only for the duration of the call to 'init(_:)'}}
  // expected-note@-2 {{use 'withUnsafeMutableBytes' in order to explicitly convert argument to buffer pointer valid for a defined scope}}

  _ = UnsafeRawPointer(str) // expected-warning {{initialization of 'UnsafeRawPointer' results in a dangling pointer}}
  // expected-note@-1 {{implicit argument conversion from 'String' to 'UnsafeRawPointer' produces a pointer valid only for the duration of the call to 'init(_:)'}}
  // expected-note@-2 {{use the 'withCString' method on String in order to explicitly convert argument to pointer valid for a defined scope}}

  _ = UnsafeRawPointer(arr) // expected-warning {{initialization of 'UnsafeRawPointer' results in a dangling pointer}}
  // expected-note@-1 {{implicit argument conversion from '[Int]' to 'UnsafeRawPointer' produces a pointer valid only for the duration of the call to 'init(_:)'}}
  // expected-note@-2 {{use the 'withUnsafeBytes' method on Array in order to explicitly convert argument to buffer pointer valid for a defined scope}}

  _ = UnsafeRawPointer(&arr) // expected-warning {{initialization of 'UnsafeRawPointer' results in a dangling pointer}}
  // expected-note@-1 {{implicit argument conversion from '[Int]' to 'UnsafeMutableRawPointer' produces a pointer valid only for the duration of the call to 'init(_:)'}}
  // expected-note@-2 {{use the 'withUnsafeMutableBytes' method on Array in order to explicitly convert argument to buffer pointer valid for a defined scope}}

  _ = UnsafeRawPointer(optionalArr) // expected-warning {{initialization of 'UnsafeRawPointer' results in a dangling pointer}}
  // expected-note@-1 {{implicit argument conversion from '[Int]?' to 'UnsafeRawPointer?' produces a pointer valid only for the duration of the call to 'init(_:)'}}


  _ = UnsafeMutableRawPointer(&foo) // expected-warning {{initialization of 'UnsafeMutableRawPointer' results in a dangling pointer}}
  // expected-note@-1 {{implicit argument conversion from 'Int' to 'UnsafeMutableRawPointer' produces a pointer valid only for the duration of the call to 'init(_:)'}}
  // expected-note@-2 {{use 'withUnsafeMutableBytes' in order to explicitly convert argument to buffer pointer valid for a defined scope}}

  _ = UnsafeMutableRawPointer(&arr) // expected-warning {{initialization of 'UnsafeMutableRawPointer' results in a dangling pointer}}
  // expected-note@-1 {{implicit argument conversion from '[Int]' to 'UnsafeMutableRawPointer' produces a pointer valid only for the duration of the call to 'init(_:)'}}
  // expected-note@-2 {{use the 'withUnsafeMutableBytes' method on Array in order to explicitly convert argument to buffer pointer valid for a defined scope}}

  _ = UnsafeMutableRawPointer(mutating: &foo) // expected-warning {{initialization of 'UnsafeMutableRawPointer' results in a dangling pointer}}
  // expected-note@-1 {{implicit argument conversion from 'Int' to 'UnsafeRawPointer' produces a pointer valid only for the duration of the call to 'init(mutating:)}}
  // expected-note@-2 {{use 'withUnsafeBytes' in order to explicitly convert argument to buffer pointer valid for a defined scope}}

  _ = UnsafeMutableRawPointer(mutating: str) // expected-warning {{initialization of 'UnsafeMutableRawPointer' results in a dangling pointer}}
  // expected-note@-1 {{implicit argument conversion from 'String' to 'UnsafeRawPointer' produces a pointer valid only for the duration of the call to 'init(mutating:)}}
  // expected-note@-2 {{use the 'withCString' method on String in order to explicitly convert argument to pointer valid for a defined scope}}

  _ = UnsafeMutableRawPointer(mutating: arr) // expected-warning {{initialization of 'UnsafeMutableRawPointer' results in a dangling pointer}}
  // expected-note@-1 {{implicit argument conversion from '[Int]' to 'UnsafeRawPointer' produces a pointer valid only for the duration of the call to 'init(mutating:)}}
  // expected-note@-2 {{use the 'withUnsafeBytes' method on Array in order to explicitly convert argument to buffer pointer valid for a defined scope}}

  _ = UnsafeMutableRawPointer(mutating: &arr) // expected-warning {{initialization of 'UnsafeMutableRawPointer' results in a dangling pointer}}
  // expected-note@-1 {{implicit argument conversion from '[Int]' to 'UnsafeRawPointer' produces a pointer valid only for the duration of the call to 'init(mutating:)}}
  // expected-note@-2 {{use the 'withUnsafeBytes' method on Array in order to explicitly convert argument to buffer pointer valid for a defined scope}}

  _ = UnsafeMutableRawPointer(mutating: optionalArr) // expected-warning {{initialization of 'UnsafeMutableRawPointer' results in a dangling pointer}}
  // expected-note@-1 {{implicit argument conversion from '[Int]?' to 'UnsafeRawPointer?' produces a pointer valid only for the duration of the call to 'init(mutating:)}}

  _ = UnsafeBufferPointer(start: &foo, count: 0) // expected-warning {{initialization of 'UnsafeBufferPointer<Int>' results in a dangling buffer pointer}}
  // expected-note@-1 {{implicit argument conversion from 'Int' to 'UnsafePointer<Int>?' produces a pointer valid only for the duration of the call to 'init(start:count:)'}}
  // expected-note@-2 {{use 'withUnsafePointer' in order to explicitly convert argument to pointer valid for a defined scope}}

  _ = UnsafeBufferPointer.init(start: &foo, count: 0) // expected-warning {{initialization of 'UnsafeBufferPointer<Int>' results in a dangling buffer pointer}}
  // expected-note@-1 {{implicit argument conversion from 'Int' to 'UnsafePointer<Int>?' produces a pointer valid only for the duration of the call to 'init(start:count:)'}}
  // expected-note@-2 {{use 'withUnsafePointer' in order to explicitly convert argument to pointer valid for a defined scope}}

  _ = UnsafeBufferPointer<Int8>(start: str, count: 0) // expected-warning {{initialization of 'UnsafeBufferPointer<Int8>' results in a dangling buffer pointer}}
  // expected-note@-1 {{implicit argument conversion from 'String' to 'UnsafePointer<Int8>?' produces a pointer valid only for the duration of the call to 'init(start:count:)'}}
  // expected-note@-2 {{use the 'withCString' method on String in order to explicitly convert argument to pointer valid for a defined scope}}

  _ = UnsafeBufferPointer<Int8>.init(start: str, count: 0) // expected-warning {{initialization of 'UnsafeBufferPointer<Int8>' results in a dangling buffer pointer}}
  // expected-note@-1 {{implicit argument conversion from 'String' to 'UnsafePointer<Int8>?' produces a pointer valid only for the duration of the call to 'init(start:count:)'}}
  // expected-note@-2 {{use the 'withCString' method on String in order to explicitly convert argument to pointer valid for a defined scope}}

  _ = UnsafeBufferPointer(start: arr, count: 0) // expected-warning {{initialization of 'UnsafeBufferPointer<Int>' results in a dangling buffer pointer}}
  // expected-note@-1 {{implicit argument conversion from '[Int]' to 'UnsafePointer<Int>?' produces a pointer valid only for the duration of the call to 'init(start:count:)'}}
  // expected-note@-2 {{use the 'withUnsafeBufferPointer' method on Array in order to explicitly convert argument to buffer pointer valid for a defined scope}}

  _ = UnsafeBufferPointer(start: &arr, count: 0) // expected-warning {{initialization of 'UnsafeBufferPointer<Int>' results in a dangling buffer pointer}}
  // expected-note@-1 {{implicit argument conversion from '[Int]' to 'UnsafePointer<Int>?' produces a pointer valid only for the duration of the call to 'init(start:count:)'}}
  // expected-note@-2 {{use the 'withUnsafeBufferPointer' method on Array in order to explicitly convert argument to buffer pointer valid for a defined scope}}

  _ = UnsafeBufferPointer(start: optionalArr, count: 0) // expected-warning {{initialization of 'UnsafeBufferPointer<Int>' results in a dangling buffer pointer}}
  // expected-note@-1 {{implicit argument conversion from '[Int]?' to 'UnsafePointer<Int>?' produces a pointer valid only for the duration of the call to 'init(start:count:)'}}

  _ = UnsafeMutableBufferPointer(start: &foo, count: 0) // expected-warning {{initialization of 'UnsafeMutableBufferPointer<Int>' results in a dangling buffer pointer}}
  // expected-note@-1 {{implicit argument conversion from 'Int' to 'UnsafeMutablePointer<Int>?' produces a pointer valid only for the duration of the call to 'init(start:count:)'}}
  // expected-note@-2 {{use 'withUnsafeMutablePointer' in order to explicitly convert argument to pointer valid for a defined scope}}

  _ = UnsafeMutableBufferPointer(start: &arr, count: 0) // expected-warning {{initialization of 'UnsafeMutableBufferPointer<Int>' results in a dangling buffer pointer}}
  // expected-note@-1 {{implicit argument conversion from '[Int]' to 'UnsafeMutablePointer<Int>?' produces a pointer valid only for the duration of the call to 'init(start:count:)'}}
  // expected-note@-2 {{use the 'withUnsafeMutableBufferPointer' method on Array in order to explicitly convert argument to buffer pointer valid for a defined scope}}


  _ = UnsafeRawBufferPointer(start: &foo, count: 0) // expected-warning {{initialization of 'UnsafeRawBufferPointer' results in a dangling buffer pointer}}
  // expected-note@-1 {{implicit argument conversion from 'Int' to 'UnsafeRawPointer?' produces a pointer valid only for the duration of the call to 'init(start:count:)'}}
  // expected-note@-2 {{use 'withUnsafeBytes' in order to explicitly convert argument to buffer pointer valid for a defined scope}}

  _ = UnsafeRawBufferPointer(start: str, count: 0) // expected-warning {{initialization of 'UnsafeRawBufferPointer' results in a dangling buffer pointer}}
  // expected-note@-1 {{implicit argument conversion from 'String' to 'UnsafeRawPointer?' produces a pointer valid only for the duration of the call to 'init(start:count:)'}}
  // expected-note@-2 {{use the 'withCString' method on String in order to explicitly convert argument to pointer valid for a defined scope}}

  _ = UnsafeRawBufferPointer(start: arr, count: 0) // expected-warning {{initialization of 'UnsafeRawBufferPointer' results in a dangling buffer pointer}}
  // expected-note@-1 {{implicit argument conversion from '[Int]' to 'UnsafeRawPointer?' produces a pointer valid only for the duration of the call to 'init(start:count:)'}}
  // expected-note@-2 {{use the 'withUnsafeBytes' method on Array in order to explicitly convert argument to buffer pointer valid for a defined scope}}

  _ = UnsafeRawBufferPointer(start: &arr, count: 0) // expected-warning {{initialization of 'UnsafeRawBufferPointer' results in a dangling buffer pointer}}
  // expected-note@-1 {{implicit argument conversion from '[Int]' to 'UnsafeRawPointer?' produces a pointer valid only for the duration of the call to 'init(start:count:)'}}
  // expected-note@-2 {{use the 'withUnsafeBytes' method on Array in order to explicitly convert argument to buffer pointer valid for a defined scope}}

  _ = UnsafeRawBufferPointer(start: optionalArr, count: 0) // expected-warning {{initialization of 'UnsafeRawBufferPointer' results in a dangling buffer pointer}}
  // expected-note@-1 {{implicit argument conversion from '[Int]?' to 'UnsafeRawPointer?' produces a pointer valid only for the duration of the call to 'init(start:count:)'}}


  _ = UnsafeMutableRawBufferPointer(start: &foo, count: 0) // expected-warning {{initialization of 'UnsafeMutableRawBufferPointer' results in a dangling buffer pointer}}
  // expected-note@-1 {{implicit argument conversion from 'Int' to 'UnsafeMutableRawPointer?' produces a pointer valid only for the duration of the call to 'init(start:count:)'}}
  // expected-note@-2 {{use 'withUnsafeMutableBytes' in order to explicitly convert argument to buffer pointer valid for a defined scope}}

  _ = UnsafeMutableRawBufferPointer(start: &arr, count: 0) // expected-warning {{initialization of 'UnsafeMutableRawBufferPointer' results in a dangling buffer pointer}}
  // expected-note@-1 {{implicit argument conversion from '[Int]' to 'UnsafeMutableRawPointer?' produces a pointer valid only for the duration of the call to 'init(start:count:)'}}
  // expected-note@-2 {{use the 'withUnsafeMutableBytes' method on Array in order to explicitly convert argument to buffer pointer valid for a defined scope}}

  // FIXME: This is currently ambiguous.
  _ = OpaquePointer(&foo) // expected-error {{ambiguous use of 'init(_:)'}}

  // FIXME: This is currently ambiguous.
  _ = OpaquePointer(&arr) // expected-error {{ambiguous use of 'init(_:)'}}

  _ = OpaquePointer(arr) // expected-warning {{initialization of 'OpaquePointer' results in a dangling pointer}}
  // expected-note@-1 {{implicit argument conversion from '[Int]' to 'UnsafeRawPointer' produces a pointer valid only for the duration of the call to 'init(_:)'}}
  // expected-note@-2 {{use the 'withUnsafeBytes' method on Array in order to explicitly convert argument to buffer pointer valid for a defined scope}}

  _ = OpaquePointer(str) // expected-warning {{initialization of 'OpaquePointer' results in a dangling pointer}}
  // expected-note@-1 {{implicit argument conversion from 'String' to 'UnsafeRawPointer' produces a pointer valid only for the duration of the call to 'init(_:)'}}
  // expected-note@-2 {{use the 'withCString' method on String in order to explicitly convert argument to pointer valid for a defined scope}}
}

var global = 0

// Test that we allow non-ephemeral conversions, such as inout-to-pointer for globals.
func unsafePointerInitNonEphemeralConversions() {
  _ = UnsafePointer(&global)
  _ = UnsafeMutablePointer(&global)
  _ = UnsafeRawPointer(&global)
  _ = UnsafeMutableRawPointer(&global)
  _ = UnsafeBufferPointer(start: &global, count: 0)
  _ = UnsafeMutableBufferPointer(start: &global, count: 0)
  _ = UnsafeRawBufferPointer(start: &global, count: 0)
  _ = UnsafeMutableRawBufferPointer(start: &global, count: 0)

  // FIXME: This is currently ambiguous.
  _ = OpaquePointer(&global) // expected-error {{ambiguous use of 'init(_:)'}}
}
