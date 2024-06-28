// RUN: %target-swift-emit-ir -verify %s -enable-experimental-feature Embedded -wmo

// REQUIRES: swift_in_compiler
// REQUIRES: optimized_stdlib
// REQUIRES: OS=macosx || OS=linux-gnu

public struct MyStruct {
  var a, b, c: Int
}

public func foo(s: MyStruct) {
  let mirror = Mirror(reflecting: s) // expected-error {{'Mirror' is unavailable}}
  _ = mirror.children
}