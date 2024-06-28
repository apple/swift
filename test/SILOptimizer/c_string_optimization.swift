// RUN: %target-build-swift -O %s -module-name=test -emit-sil | %FileCheck %s

// RUN: %empty-directory(%t) 
// RUN: %target-build-swift -O -module-name=test %s -o %t/a.out
// RUN: %target-run %t/a.out | %FileCheck %s -check-prefix=CHECK-OUTPUT

// REQUIRES: executable_test,swift_stdlib_no_asserts

#if canImport(Darwin)
  import Darwin
#elseif canImport(Glibc)
  import Glibc
#elseif canImport(Android)
  import Android
#elseif os(Windows)
  import CRT
#else
#error("Unsupported platform")
#endif

// CHECK-LABEL: sil [noinline] @$s4test0A26StringConstantForCFunctionyyF
// CHECK-NOT: apply
// CHECK:    [[L:%[0-9]+]] = string_literal utf8 "Hello world!"
// CHECK-NOT: apply
// CHECK:    [[P:%[0-9]+]] = struct $UnsafePointer<Int8> ([[L]] : $Builtin.RawPointer)
// CHECK-NOT: apply
// CHECK:    [[O:%[0-9]+]] = enum $Optional<UnsafePointer<Int8>>, #Optional.some!enumelt, [[P]]
// CHECK-NOT: apply
// CHECK:    [[F:%[0-9]+]] = function_ref @puts
// CHECK:    apply [[F]]([[O]])
// CHECK: } // end sil function '$s4test0A26StringConstantForCFunctionyyF'
@inline(never)
public func testStringConstantForCFunction() {
  puts("Hello " + "world!")
}

// CHECK-LABEL: sil [noinline] @$s4test0A17TypeInterpolationyyF
// CHECK-NOT: apply
// CHECK:    [[L:%[0-9]+]] = string_literal utf8 "String"
// CHECK-NOT: apply
// CHECK:    [[P:%[0-9]+]] = struct $UnsafePointer<Int8> ([[L]] : $Builtin.RawPointer)
// CHECK-NOT: apply
// CHECK:    [[O:%[0-9]+]] = enum $Optional<UnsafePointer<Int8>>, #Optional.some!enumelt, [[P]]
// CHECK-NOT: apply
// CHECK:    [[F:%[0-9]+]] = function_ref @puts
// CHECK:    apply [[F]]([[O]])
// CHECK: } // end sil function '$s4test0A17TypeInterpolationyyF'
@inline(never)
public func testTypeInterpolation() {
  puts("\(String.self)")
}

// CHECK-OUTPUT: Hello world!
testStringConstantForCFunction()

// CHECK-OUTPUT: String
testTypeInterpolation()

