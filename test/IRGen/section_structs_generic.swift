// RUN: %target-swift-frontend -enable-experimental-feature SymbolLinkageMarkers -parse-as-library -emit-ir %s -o - | %FileCheck %s

// REQUIRES: swift_in_compiler

struct MyStruct1<T> {
    var a, b: T
}
@_section("__TEXT,__mysection") var g_MyStruct1 = MyStruct1<Int>(a: 42, b: 66)

struct MyStruct2<T> {
    var a: T
    var b: (T, T)
}
@_section("__TEXT,__mysection") var g_MyStruct2 = MyStruct2<Int>(a: 42, b: (66, 67))

struct MyStruct3<T> {
    var a, b: T
    public init(a: T, b: T) {
        self.a = a
        self.b = b
    }
}
@_section("__TEXT,__mysection") var g_MyStruct3 = MyStruct3<Int>(a: 42, b: 77)

struct MyStruct4<T> {
    var a: T
    var s: MyStruct1<T>
}
@_section("__TEXT,__mysection") var g_MyStruct4 = MyStruct4<Int>(a: 42, s: MyStruct1<Int>(a: 43, b: 44))

struct MyStruct5<T> {
    var q: MyStruct4<T>
    var r: MyStruct4<T>
    public init(q: MyStruct4<T>, r: MyStruct4<T>) {
        self.q = q
        self.r = r
    }
}
@_section("__TEXT,__mysection") var g_MyStruct5 = MyStruct5<Int>(q: MyStruct4<Int>(a: 42, s: MyStruct1<Int>(a: 43, b: 44)), r: MyStruct4<Int>(a: 42, s: MyStruct1<Int>(a: 43, b: 44)))

// CHECK: @"{{.*}}g_MyStruct1{{.*}}Gvp" = hidden global {{.*}} <{ %TSi <{ {{(i32|i64)}} 42 }>, %TSi <{ {{(i32|i64)}} 66 }> }>
// CHECK: @"{{.*}}g_MyStruct2{{.*}}Gvp" = hidden global {{.*}} <{ %TSi <{ {{(i32|i64)}} 42 }>, <{ %TSi, %TSi }> <{ %TSi <{ {{(i32|i64)}} 66 }>, %TSi <{ {{(i32|i64)}} 67 }> }> }>
// CHECK: @"{{.*}}g_MyStruct3{{.*}}Gvp" = hidden global {{.*}} <{ %TSi <{ {{(i32|i64)}} 42 }>, %TSi <{ {{(i32|i64)}} 77 }> }>
// CHECK: @"{{.*}}g_MyStruct4{{.*}}Gvp" = hidden global {{.*}} <{ %TSi <{ {{(i32|i64)}} 42 }>, {{.*}} <{ %TSi <{ {{(i32|i64)}} 43 }>, %TSi <{ {{(i32|i64)}} 44 }> }> }>
// CHECK: @"{{.*}}g_MyStruct5{{.*}}Gvp" = hidden global {{.*}} <{ {{.*}} <{ %TSi <{ {{(i32|i64)}} 42 }>, {{.*}} <{ %TSi <{ {{(i32|i64)}} 43 }>, %TSi <{ {{(i32|i64)}} 44 }> }> }>, {{.*}} <{ %TSi <{ {{(i32|i64)}} 42 }>, {{.*}} <{ %TSi <{ {{(i32|i64)}} 43 }>, %TSi <{ {{(i32|i64)}} 44 }> }> }> }>
