// RUN: %target-swift-frontend -enable-experimental-feature SymbolLinkageMarkers -parse-as-library -emit-ir %s -o - | %FileCheck %s

// REQUIRES: swift_in_compiler

struct MyStruct1 {
    var a, b: Int
}
@_section("__TEXT,__mysection") var g_MyStruct1: MyStruct1 = MyStruct1(a: 42, b: 66)

struct MyStruct2 {
    var a: Int
    var b: (Int, Int)
}
@_section("__TEXT,__mysection") var g_MyStruct2: MyStruct2 = MyStruct2(a: 42, b: (66, 67))

struct MyStruct3 {
    var a, b: Int
    public init(a: Int, b: Int) {
        self.a = a
        self.b = b
    }
}
@_section("__TEXT,__mysection") var g_MyStruct3: MyStruct3 = MyStruct3(a: 42, b: 77)

struct MyStruct4 {
    var a: Int
    var s: MyStruct1
}
@_section("__TEXT,__mysection") var g_MyStruct4: MyStruct4 = MyStruct4(a: 42, s: MyStruct1(a: 43, b: 44))

struct MyStruct5 {
    var q: MyStruct4
    var r: MyStruct4
    public init(q: MyStruct4, r: MyStruct4) {
        self.q = q
        self.r = r
    }
}
@_section("__TEXT,__mysection") var g_MyStruct5: MyStruct5 = MyStruct5(q: MyStruct4(a: 42, s: MyStruct1(a: 43, b: 44)), r: MyStruct4(a: 42, s: MyStruct1(a: 43, b: 44)))

// CHECK: @"{{.*}}g_MyStruct1{{.*}}Vvp" = hidden global {{.*}} <{ %TSi <{ {{(i32|i64)}} 42 }>, %TSi <{ {{(i32|i64)}} 66 }> }>
// CHECK: @"{{.*}}g_MyStruct2{{.*}}Vvp" = hidden global {{.*}} <{ %TSi <{ {{(i32|i64)}} 42 }>, <{ %TSi, %TSi }> <{ %TSi <{ {{(i32|i64)}} 66 }>, %TSi <{ {{(i32|i64)}} 67 }> }> }>
// CHECK: @"{{.*}}g_MyStruct3{{.*}}Vvp" = hidden global {{.*}} <{ %TSi <{ {{(i32|i64)}} 42 }>, %TSi <{ {{(i32|i64)}} 77 }> }>
// CHECK: @"{{.*}}g_MyStruct4{{.*}}Vvp" = hidden global {{.*}} <{ %TSi <{ {{(i32|i64)}} 42 }>, {{.*}} <{ %TSi <{ {{(i32|i64)}} 43 }>, %TSi <{ {{(i32|i64)}} 44 }> }> }>
// CHECK: @"{{.*}}g_MyStruct5{{.*}}Vvp" = hidden global {{.*}} <{ {{.*}} <{ %TSi <{ {{(i32|i64)}} 42 }>, {{.*}} <{ %TSi <{ {{(i32|i64)}} 43 }>, %TSi <{ {{(i32|i64)}} 44 }> }> }>, {{.*}} <{ %TSi <{ {{(i32|i64)}} 42 }>, {{.*}} <{ %TSi <{ {{(i32|i64)}} 43 }>, %TSi <{ {{(i32|i64)}} 44 }> }> }> }>
