// RUN: %empty-directory(%t)
// RUN: %{python} %utils/split_file.py -o %t %s

// -enable-import-ptrauth-field-function-pointers is a no-op
// RUN: %target-swift-frontend -enable-import-ptrauth-field-function-pointers -O -emit-ir %t/Main.swift -enable-experimental-feature Embedded -import-objc-header %t/header.h | %FileCheck %s

// REQUIRES: swift_in_compiler
// REQUIRES: OS=macosx || OS=linux-gnu
// REQUIRES: CPU=arm64e

// BEGIN header.h

struct MyStruct {
    void (*fptr1)(void);
    void (*__ptrauth(0, 0, 0x4242) fptr2)(void);
};

// BEGIN Main.swift

public func test1(x: UnsafePointer<MyStruct>) {
    x.pointee.fptr1()
}

public func test2(x: UnsafePointer<MyStruct>) {
    x.pointee.fptr2()
}

// CHECK: define {{.*}}@"$s4Main5test11xySPySo8MyStructVG_tF"
// CHECK:   call {{.*}}[ "ptrauth"(i32 0, i64 0) ]
// CHECK: define {{.*}}@"$s4Main5test21xySPySo8MyStructVG_tF"
// CHECK:   call {{.*}}[ "ptrauth"(i32 0, i64 16962) ]
