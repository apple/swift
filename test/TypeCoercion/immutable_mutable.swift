// RUN: %target-typecheck-verify-swift -parse-stdlib

import Swift

let optionalMutableRawPointer = UnsafeMutableRawPointer(bitPattern: -3)
let mutableRawPointer = optionalMutableRawPointer!
let immutable: UnsafeRawPointer = mutableRawPointer
let immutable2: UnsafeRawPointer? = optionalMutableRawPointer
let optionalImmutable: UnsafeRawPointer? = mutableRawPointer
let mutable: UnsafeMutableRawPointer = immutable // expected-error {{cannot convert value of type 'UnsafeRawPointer' to specified type 'UnsafeMutableRawPointer'}}

if mutableRawPointer == immutable || immutable == mutableRawPointer ||
    mutableRawPointer == optionalImmutable || optionalImmutable == mutableRawPointer {
}

func unmutable(immutable: UnsafeRawPointer) -> UnsafeRawPointer {
    return mutableRawPointer
}
func unmutable(optionalImmutable: UnsafeRawPointer?) -> UnsafeRawPointer? {
    return optionalMutableRawPointer
}

_ = unmutable(immutable: mutableRawPointer)
_ = unmutable(optionalImmutable: mutableRawPointer)
_ = unmutable(optionalImmutable: optionalMutableRawPointer)

var i = 99
let mutable3 = UnsafeMutablePointer<Int>(&i)
let immutable3 = UnsafePointer<Int>(&i)
let immutable4: UnsafePointer<Int> = mutable3
let immutable5: UnsafePointer<Int>? = mutable3
let immutable6: UnsafePointer<Double> = mutable3 // expected-error {{cannot convert value of type 'UnsafeMutablePointer<Int>' to specified type 'UnsafePointer<Double>'}}
let mutable4: UnsafeMutablePointer<Int> = immutable3 // expected-error {{cannot convert value of type 'UnsafePointer<Int>' to specified type 'UnsafeMutablePointer<Int>'}}
if mutable3 == immutable3 || immutable3 == mutable3 ||
    immutable == immutable3 || immutable == mutable3 {
}

func demutable(immutable: UnsafePointer<Int>) -> UnsafeRawPointer {
    return mutableRawPointer
}
func demutable(optionalImmutable: UnsafePointer<Int>?) -> UnsafeRawPointer? {
    return optionalMutableRawPointer
}

_ = demutable(immutable: mutable3)
_ = demutable(optionalImmutable: mutable3)

_ = unmutable(immutable: mutable3)
_ = unmutable(optionalImmutable: mutable3)

var array = [1]
array.withUnsafeMutableBufferPointer {
    _ = demutable(immutable: $0.baseAddress!)
    _ = demutable(optionalImmutable: $0.baseAddress)
}
