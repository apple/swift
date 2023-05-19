// RUN: %target-swift-frontend  -primary-file %s -O -sil-verify-all -module-name=test -emit-sil | %FileCheck %s

// Make a runtime test to check that the values are correct.
// RUN: %empty-directory(%t) 
// RUN: %target-build-swift -O -module-name=test %s -o %t/a.out
// RUN: %target-run %t/a.out | %FileCheck %s -check-prefix=CHECK-OUTPUT

// REQUIRES: executable_test,swift_stdlib_no_asserts,optimized_stdlib
// REQUIRES: swift_in_compiler


struct S {
  var i: Int
  var b: Int8

  // CHECK-LABEL: sil_global hidden [let] @$s4test1SV4sizeSivpZ : $Int = {
  // CHECK-NEXT:    integer_literal
  static let size = MemoryLayout<S>.size

  // CHECK-LABEL: sil_global hidden [let] @$s4test1SV9alignmentSivpZ : $Int = {
  // CHECK-NEXT:    integer_literal
  static let alignment = MemoryLayout<S>.alignment

  // CHECK-LABEL: sil_global hidden [let] @$s4test1SV6strideSivpZ : $Int = {
  // CHECK-NEXT:    integer_literal
  static let stride = MemoryLayout<S>.stride

  static let doubleSize = MemoryLayout<S>.size * 2
}

// CHECK-LABEL: sil @$s4test14noConstantSizeySixmlF
// CHECK:         builtin "sizeof"<T>
// CHECK:       } // end sil function '$s4test14noConstantSizeySixmlF'
public func noConstantSize<T>(_ t: T.Type) -> Int {
  return MemoryLayout<T>.size
}

// It's important to not constant propagate here to make sure that we are comparing
// SIL constant propagated values with IRGen values.

// CHECK-LABEL: sil {{.*}} @$s4test7getSizeySixmlF
// CHECK:         builtin "sizeof"<T>
// CHECK:       } // end sil function '$s4test7getSizeySixmlF' 
@_optimize(none)
func getSize<T>(_ t: T.Type) -> Int {
  return MemoryLayout<T>.size
}

// CHECK-LABEL: sil {{.*}} @$s4test12getAlignmentySixmlF
// CHECK:         builtin "alignof"<T>
// CHECK:       } // end sil function '$s4test12getAlignmentySixmlF' 
@_optimize(none)
func getAlignment<T>(_ t: T.Type) -> Int {
  return MemoryLayout<T>.alignment
}

// CHECK-LABEL: sil {{.*}} @$s4test9getStrideySixmlF
// CHECK:         builtin "strideof"<T>
// CHECK:       } // end sil function '$s4test9getStrideySixmlF' 
@_optimize(none)
func getStride<T>(_ t: T.Type) -> Int {
  return MemoryLayout<T>.stride
}

@inline(never)
func testit() {
  // CHECK-OUTPUT: size: true
  print("size: \(S.size == getSize(S.self))")

  // CHECK-OUTPUT: alignment: true
  print("alignment: \(S.alignment == getAlignment(S.self))")

  // CHECK-OUTPUT: stride: true
  print("stride: \(S.stride == getStride(S.self))")

  // CHECK-OUTPUT: doubleSize: true
  print("doubleSize: \(S.doubleSize == getSize(S.self) * 2)")

  // CHECK-OUTPUT: metatype-size-1: true
  print("metatype-size-1: \(MemoryLayout<S.Type>.size == MemoryLayout<UnsafeRawPointer>.size)")
  // CHECK-OUTPUT: metatype-size-2: true
  print("metatype-size-2: \(getSize(S.Type.self) == MemoryLayout<UnsafeRawPointer>.size)")
  // CHECK-OUTPUT: metatype-alignment: true
  print("metatype-alignment: \(getAlignment(S.Type.self) == MemoryLayout<UnsafeRawPointer>.alignment)")
  // CHECK-OUTPUT: metatype-stride: true
  print("metatype-stride: \(getStride(S.Type.self) == MemoryLayout<UnsafeRawPointer>.stride)")
}

testit()

