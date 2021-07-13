// RUN: %target-build-swift -emit-ir %s | %FileCheck -check-prefix=%target-cpu %s
// RUN: %target-build-swift -O -emit-ir %s | %FileCheck -check-prefix=%target-cpu %s
// RUN: %target-build-swift -Ounchecked -emit-ir %s | %FileCheck -check-prefix=%target-cpu %s

// REQUIRES: CPU=i386 || CPU=x86_64
// UNSUPPORTED: OS=windows-msvc
// UNSUPPORTED: OS=freebsd

var globalFloat80 : Float80 = 0.0

@inline(never)
func acceptFloat80(_ a: Float80) {
  globalFloat80 = a
}

func testConstantFoldFloatLiterals() {
  acceptFloat80(1.0)
}

// i386: call swiftcc void @"$s20FloatingPointIR_FP8013acceptFloat80yys0F0VF{{.*}}"(x86_fp80 0xK3FFF8000000000000000)
// x86_64: call swiftcc void @"$s20FloatingPointIR_FP8013acceptFloat80yys0F0VF{{.*}}"(x86_fp80 0xK3FFF8000000000000000)
