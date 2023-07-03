// RUN: %target-swift-frontend -enable-experimental-feature SymbolLinkageMarkers -parse-as-library -emit-sil %s -o /dev/null -verify

// REQUIRES: swift_in_compiler

@_silgen_name("g0") var g0: Int = 1
@_silgen_name("g1") var g1: (Int, Int) = (1, 2)
@_silgen_name("g2") var g2: [Int] = [1, 2, 3] // expected-error {{global variable must be a compile-time constant to use @_silgen_name attribute}}
@_silgen_name("g3") var g3: [Int:Int] = [:] // expected-error {{global variable must be a compile-time constant to use @_silgen_name attribute}}
@_silgen_name("g4") var g4: UInt = 42
@_silgen_name("g5") var g5: String = "hello" // expected-error {{global variable must be a compile-time constant to use @_silgen_name attribute}}
@_silgen_name("g6") var g6: Any = 1 // expected-error {{global variable must be a compile-time constant to use @_silgen_name attribute}}
@_silgen_name("g7") var g7: UInt8 = 42
@_silgen_name("g8") var g8: Int = 5 * 5

@_silgen_name("fwd1") var fwd1: Int
