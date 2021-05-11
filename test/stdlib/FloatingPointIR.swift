// RUN: %target-build-swift -emit-ir %s | %FileCheck -check-prefix=%target-cpu %s
// RUN: %target-build-swift -O -emit-ir %s | %FileCheck -check-prefix=%target-cpu %s
// RUN: %target-build-swift -Ounchecked -emit-ir %s | %FileCheck -check-prefix=%target-cpu %s

var globalFloat32 : Float32 = 0.0
var globalFloat64 : Float64 = 0.0

@inline(never)
func acceptFloat32(_ a: Float32) {
  globalFloat32 = a
}

@inline(never)
func acceptFloat64(_ a: Float64) {
  globalFloat64 = a
}

func testConstantFoldFloatLiterals() {
  acceptFloat32(1.0)
  acceptFloat64(1.0)
}

// i386: call swiftcc void @"$s15FloatingPointIR13acceptFloat32yySfF{{.*}}"(float 1.000000e+00)
// i386: call swiftcc void @"$s15FloatingPointIR13acceptFloat64yySdF{{.*}}"(double 1.000000e+00)

// x86_64: call swiftcc void @"$s15FloatingPointIR13acceptFloat32yySfF{{.*}}"(float 1.000000e+00)
// x86_64: call swiftcc void @"$s15FloatingPointIR13acceptFloat64yySdF{{.*}}"(double 1.000000e+00)

// armv7: call swiftcc void @"$s15FloatingPointIR13acceptFloat32yySfF{{.*}}"(float 1.000000e+00)
// armv7: call swiftcc void @"$s15FloatingPointIR13acceptFloat64yySdF{{.*}}"(double 1.000000e+00)

// armv7s: call swiftcc void @"$s15FloatingPointIR13acceptFloat32yySfF{{.*}}"(float 1.000000e+00)
// armv7s: call swiftcc void @"$s15FloatingPointIR13acceptFloat64yySdF{{.*}}"(double 1.000000e+00)

// armv7k: call swiftcc void @"$s15FloatingPointIR13acceptFloat32yySfF{{.*}}"(float 1.000000e+00)
// armv7k: call swiftcc void @"$s15FloatingPointIR13acceptFloat64yySdF{{.*}}"(double 1.000000e+00)

// arm64: call swiftcc void @"$s15FloatingPointIR13acceptFloat32yySfF{{.*}}"(float 1.000000e+00)
// arm64: call swiftcc void @"$s15FloatingPointIR13acceptFloat64yySdF{{.*}}"(double 1.000000e+00)

// arm64e: call swiftcc void @"$s15FloatingPointIR13acceptFloat32yySfF{{.*}}"(float 1.000000e+00)
// arm64e: call swiftcc void @"$s15FloatingPointIR13acceptFloat64yySdF{{.*}}"(double 1.000000e+00)

// arm64_32: call swiftcc void @"$s15FloatingPointIR13acceptFloat32yySfF{{.*}}"(float 1.000000e+00)
// arm64_32: call swiftcc void @"$s15FloatingPointIR13acceptFloat64yySdF{{.*}}"(double 1.000000e+00)


// aarch64: call swiftcc void @"$s15FloatingPointIR13acceptFloat32yySfF{{.*}}"(float 1.000000e+00)
// aarch64: call swiftcc void @"$s15FloatingPointIR13acceptFloat64yySdF{{.*}}"(double 1.000000e+00)

// powerpc64: call swiftcc void @"$s15FloatingPointIR13acceptFloat32yySfF{{.*}}"(float 1.000000e+00)
// powerpc64: call swiftcc void @"$s15FloatingPointIR13acceptFloat64yySdF{{.*}}"(double 1.000000e+00)

// powerpc64le: call swiftcc void @"$s15FloatingPointIR13acceptFloat32yySfF{{.*}}"(float 1.000000e+00)
// powerpc64le: call swiftcc void @"$s15FloatingPointIR13acceptFloat64yySdF{{.*}}"(double 1.000000e+00)

// s390x: call swiftcc void @"$s15FloatingPointIR13acceptFloat32yySfF{{.*}}"(float 1.000000e+00)
// s390x: call swiftcc void @"$s15FloatingPointIR13acceptFloat64yySdF{{.*}}"(double 1.000000e+00)

// wasm32: call swiftcc void @"$s15FloatingPointIR13acceptFloat32yySfF{{.*}}"(float 1.000000e+00)
// wasm32: call swiftcc void @"$s15FloatingPointIR13acceptFloat64yySdF{{.*}}"(double 1.000000e+00)
