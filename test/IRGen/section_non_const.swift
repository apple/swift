// RUN: %target-swift-frontend -enable-experimental-feature SymbolLinkageMarkers -parse-as-library -emit-sil %s -o /dev/null -verify

// REQUIRES: swift_in_compiler

@_section("__TEXT,__mysection") var g0: Int = 1
@_section("__TEXT,__mysection") var g1: (Int, Int) = (1, 2)
@_section("__TEXT,__mysection") var g2: [Int] = [1, 2, 3] // expected-error {{global variable must be a compile-time constant to use @_section attribute}}
@_section("__TEXT,__mysection") var g3: [Int:Int] = [:] // expected-error {{global variable must be a compile-time constant to use @_section attribute}}
@_section("__TEXT,__mysection") var g4: UInt = 42
@_section("__TEXT,__mysection") var g5: String = "hello" // expected-error {{global variable must be a compile-time constant to use @_section attribute}}
@_section("__TEXT,__mysection") var g6: Any = 1 // expected-error {{global variable must be a compile-time constant to use @_section attribute}}
@_section("__TEXT,__mysection") var g7: UInt8 = 42
@_section("__TEXT,__mysection") var g8: Int = 5 * 5
