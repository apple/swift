// RUN: %target-swift-frontend -target armv7-apple-none-macho -module-name main -parse-as-library -emit-ir %s -enable-experimental-feature Embedded | %FileCheck %s
// RUN: %target-swift-frontend -target arm64-apple-none-macho -module-name main -parse-as-library -emit-ir %s -enable-experimental-feature Embedded | %FileCheck %s
// REQUIRES: swift_in_compiler
// REQUIRES: OS=macosx || OS=linux-gnu
// REQUIRES: CODEGENERATOR=ARM
// REQUIRES: embedded_stdlib_cross_compiling

@main
public struct Application {
    public static func main() {
        var x: UInt64 = 0
        x <<= 8
    }
}

enum MyEnum: UInt8 {
    case a = 0
}

// CHECK: define {{.*}}@main(

