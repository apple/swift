// RUN: %target-swift-frontend -target armv7em-none-none-eabi -parse-as-library -module-name main -O -emit-ir %s -enable-experimental-feature Embedded -Xllvm -link-embedded-runtime=0 | %FileCheck %s
// RUN: %target-swift-frontend -target armv7em-none-none-eabi -parse-as-library -module-name main -Osize -emit-ir %s -enable-experimental-feature Embedded -Xllvm -link-embedded-runtime=0 | %FileCheck %s

// REQUIRES: swift_in_compiler
// REQUIRES: optimized_stdlib
// REQUIRES: OS=macosx || OS=linux-gnu
// REQUIRES: CODEGENERATOR=ARM
// REQUIRES: embedded_stdlib_cross_compiling

let pots = [41,52,99,1,4,4]

public enum ValidState {
    case yes
    case no
    case undefined
}

public func checkPot(i: UInt8, testValue: UInt8) -> ValidState {
    guard i < pots.count else { return .undefined }
    return pots[Int(i)] < testValue ? .yes : .no
}

// CHECK: @"$s4main4pots_Wz" = {{.*}}global i32 0
// CHECK: @"$ss20__StaticArrayStorageCN" = {{.*}}global ptr null
// CHECK: @"$s4main4pots_WZTv_r" = {{.*}}constant %Ts23_ContiguousArrayStorageCySiG_tailelems0 {{.*}}@"$ss20__StaticArrayStorageCN"
// CHECK: @"$s4main4potsSaySiGvp" = {{.*}}constant %TSa {{.*}}@"$s4main4pots_WZTv_r"
