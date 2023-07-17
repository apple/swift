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

@_section("__TEXT,__mysection") var s: StaticString = "hello"

struct MyStruct1 {
    var a: [Int]
}
@_section("__TEXT,__mysection") var g_MyStruct1: MyStruct1 = MyStruct1(a: [1, 2, 3]) // expected-error {{global variable must be a compile-time constant to use @_section attribute}}

struct MyStruct2 {
    struct SubStruct { var x: Int }
    var a: Int
    var b: SubStruct
    init(a: Int) {
        self.a = a
        self.b = SubStruct(x: self.a)
    }
}
@_section("__TEXT,__mysection") var g_MyStruct2: MyStruct2 = MyStruct2(a: 42)

struct MyStruct3 {
    struct SubStruct { var x: Int }
    var a: Int
    var b: SubStruct
    init(a: Int, b: SubStruct) {
        self.a = a
        self.b = b
    }
}
@_section("__TEXT,__mysection") var g_MyStruct3: MyStruct3 = MyStruct3(a: 42, b: MyStruct3.SubStruct(x: 77))

// Check that we don't end up infinitely inlining
struct MyStruct4 {
    var a: Int
    init(a: Int) {
        if a > 0 { _ = MyStruct4(a: a + 1) }
        self.a = a
    }
}
@_section("__TEXT,__mysection") var g_MyStruct4: MyStruct4 = MyStruct4(a: 42) // expected-error {{global variable must be a compile-time constant to use @_section attribute}}

struct MyStruct5 {
    struct SubStruct { var x: Int }
    var a: (SubStruct, SubStruct)
    init(a: Int) {
        self.a = (SubStruct(x: a), SubStruct(x: a))
    }
}
@_section("__TEXT,__mysection") var g_MyStruct5: MyStruct5 = MyStruct5(a: 42)

// Check that we don't end up infinitely inlining
struct MyStruct6 {
    struct SubStruct {
        var x: Int
        init(x: Int) {
            if x > 0 { _ = SubStruct(x: x + 1) }
            self.x = x
        }
    }
    var a: (SubStruct, SubStruct)
    init(a: Int) {
        self.a = (SubStruct(x: a), SubStruct(x: a))
    }
}
@_section("__TEXT,__mysection") var g_MyStruct6: MyStruct6 = MyStruct6(a: 42) // expected-error {{global variable must be a compile-time constant to use @_section attribute}}

@_section("__TEXT,__mysection") var gp1: UnsafeMutablePointer<Int>? = nil
@_section("__TEXT,__mysection") var gp2: UnsafeMutablePointer<Int>? = UnsafeMutablePointer(bitPattern: 0x42424242)
