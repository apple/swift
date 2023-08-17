// RUN: %empty-directory(%t) 
// RUN: %target-build-swift %S/Inputs/struct_with_fields.swift -parse-as-library -wmo -enable-library-evolution -module-name=Test -emit-module -emit-module-path=%t/Test.swiftmodule -c -o %t/test.o

// RUN: %target-build-swift -O %s -module-name=test -Xfrontend -sil-verify-all -I%t -emit-sil | %FileCheck %s

// RUN: %target-build-swift -Onone %s -I%t %t/test.o -o %t/Onone.out
// RUN: %target-build-swift -O %s -I%t %t/test.o -o %t/O.out
// RUN: %target-run %t/Onone.out > %t/Onone.txt
// RUN: %target-run %t/O.out > %t/O.txt
// RUN: diff  %t/Onone.txt %t/O.txt

// REQUIRES: executable_test,swift_stdlib_no_asserts,optimized_stdlib

import Test

final class C<T> {
  var x: Int
  var z: T
  let immutable: String
  private(set) var secretlyMutable: String

  init(x: Int, z: T) {
    self.x = x
    self.z = z
    self.immutable = "somestring"
    self.secretlyMutable = immutable
  }
}

struct Point {
  var x: Double
  var y: Double
}

struct S<T> {
  var x: Int
  var y: Int?
  var z: T
  var p: Point
  var op: Point?
  var c: C<T>
}

struct NonOffsetableProperties {
  // observers
  var x: Int { didSet {} }
  // reabstracted
  var y: () -> ()
  // computed
  var z: Int { return 0 }
}

struct TupleProperties {
  // unlabeled
  var a: (Int, String)
  // labeled
  let b: (x: String, y: Int)
  // reference writable
  let c: (m: C<Int>, n: C<String>)
}

typealias Tuple<T, U> = (S<T>, C<U>)

typealias TupleOfFunctions = (a: @convention(c) () -> (), b: @convention(c) () -> ())

func getIdentityKeyPathOfType<T>(_: T.Type) -> KeyPath<T, T> {
  return \.self
}


@inline(never)
func printOffset(_ o: Int?) {
  print(o as Any)
}

// CHECK-LABEL: sil {{.*}} @$s4test0A13StructOffsetsyyF
// CHECK-NOT:     _storedInlineOffset
// CHECK-NOT:     class_method
// CHECK:       } // end sil function '$s4test0A13StructOffsetsyyF'
@inline(never)
func testStructOffsets() {
  let SLayout = MemoryLayout<S<Int>>.self
  printOffset(SLayout.offset(of: \S<Int>.x))
  printOffset(SLayout.offset(of: \S<Int>.y))
  printOffset(SLayout.offset(of: \S<Int>.z))
  printOffset(SLayout.offset(of: \S<Int>.p))
  printOffset(SLayout.offset(of: \S<Int>.p.x))
  printOffset(SLayout.offset(of: \S<Int>.p.y))
  printOffset(SLayout.offset(of: \S<Int>.c))
}

// CHECK-LABEL: sil {{.*}} @$s4test0A20GenericStructOffsetsyyxmlF
// CHECK-NOT:     _storedInlineOffset
// CHECK-NOT:     class_method
// CHECK:       } // end sil function '$s4test0A20GenericStructOffsetsyyxmlF'
@inline(never)
@_semantics("optimize.sil.specialize.generic.never")
func testGenericStructOffsets<T>(_ t: T.Type) {
  let SLayout = MemoryLayout<S<T>>.self
  printOffset(SLayout.offset(of: \S<T>.x))
  printOffset(SLayout.offset(of: \S<T>.y))
  printOffset(SLayout.offset(of: \S<T>.z))
  printOffset(SLayout.offset(of: \S<T>.p))
  printOffset(SLayout.offset(of: \S<T>.p.x))
  printOffset(SLayout.offset(of: \S<T>.p.y))
  printOffset(SLayout.offset(of: \S<T>.c))
}

// CHECK-LABEL: sil {{.*}} @$s4test0A10NonOffsetsyyF
// CHECK-NOT:     _storedInlineOffset
// CHECK-NOT:     class_method
// CHECK:       } // end sil function '$s4test0A10NonOffsetsyyF'
@inline(never)
func testNonOffsets() {
  let NOPLayout = MemoryLayout<NonOffsetableProperties>.self
  printOffset(NOPLayout.offset(of: \NonOffsetableProperties.x))
  printOffset(NOPLayout.offset(of: \NonOffsetableProperties.y))
  printOffset(NOPLayout.offset(of: \NonOffsetableProperties.z))
  printOffset(MemoryLayout<C<Int>>.offset(of: \C<Int>.x))
  let SLayout = MemoryLayout<S<Int>>.self
  printOffset(SLayout.offset(of: \S<Int>.c.x))
  printOffset(SLayout.offset(of: \S<Int>.op!.x))
  printOffset(SLayout.offset(of: \S<Int>.op?.x))
}

// CHECK-LABEL: sil {{.*}} @$s4test0A11SelfOffsetsyyF
// CHECK-NOT:     _storedInlineOffset
// CHECK-NOT:     class_method
// CHECK:       } // end sil function '$s4test0A11SelfOffsetsyyF'
@inline(never)
func testSelfOffsets() {
  let SLayout = MemoryLayout<S<Int>>.self
  printOffset(SLayout.offset(of: \.self))
  printOffset(SLayout.offset(of: getIdentityKeyPathOfType(S<Int>.self)))
}

// CHECK-LABEL: sil {{.*}} @$s4test0A12TupleOffsetsyyF
// CHECK-NOT:     _storedInlineOffset
// CHECK-NOT:     class_method
// CHECK:       } // end sil function '$s4test0A12TupleOffsetsyyF'
@inline(never)
func testTupleOffsets() {
  let TPLayout = MemoryLayout<TupleProperties>.self
  printOffset(TPLayout.offset(of: \TupleProperties.self))
  printOffset(TPLayout.offset(of: \TupleProperties.a))
  printOffset(TPLayout.offset(of: \TupleProperties.a.0))
  printOffset(TPLayout.offset(of: \TupleProperties.a.1))
  printOffset(TPLayout.offset(of: \TupleProperties.b))
  printOffset(TPLayout.offset(of: \TupleProperties.b.x))
  printOffset(TPLayout.offset(of: \TupleProperties.b.y))
  printOffset(TPLayout.offset(of: \TupleProperties.c))
  printOffset(TPLayout.offset(of: \TupleProperties.c.m))
  printOffset(TPLayout.offset(of: \TupleProperties.c.n))

  let TLayout = MemoryLayout<Tuple<Int, Int>>.self
  printOffset(TLayout.offset(of: \Tuple<Int, Int>.self))
  printOffset(TLayout.offset(of: \Tuple<Int, Int>.0))
  printOffset(TLayout.offset(of: \Tuple<Int, Int>.0.x))
  printOffset(TLayout.offset(of: \Tuple<Int, Int>.1))
}

// CHECK-LABEL: sil {{.*}} @$s4test0A16TupleOfFunctionsyyF
// CHECK-NOT:     _storedInlineOffset
// CHECK-NOT:     class_method
// CHECK:       } // end sil function '$s4test0A16TupleOfFunctionsyyF'
@inline(never)
func testTupleOfFunctions() {
  printOffset(MemoryLayout<TupleOfFunctions>.offset(of: \.b))
}

// CHECK-LABEL: sil {{.*}} @$s4test0A19GenericTupleOffsetsyyxmlF
// CHECK-NOT:     _storedInlineOffset
// CHECK-NOT:     class_method
// CHECK:       } // end sil function '$s4test0A19GenericTupleOffsetsyyxmlF'
@inline(never)
@_semantics("optimize.sil.specialize.generic.never")
func testGenericTupleOffsets<T>(_ t: T.Type) {
  let TLayout = MemoryLayout<Tuple<T, T>>.self
  printOffset(TLayout.offset(of: \Tuple<T, T>.self))
  printOffset(TLayout.offset(of: \Tuple<T, T>.0))
  printOffset(TLayout.offset(of: \Tuple<T, T>.0.x))
  printOffset(TLayout.offset(of: \Tuple<T, T>.1))
}

// CHECK-LABEL: sil {{.*}} @$s4test0A16ResilientOffsetsyyF
// CHECK:         class_method {{.*}}_storedInlineOffset
// CHECK:       } // end sil function '$s4test0A16ResilientOffsetsyyF'
@inline(never)
func testResilientOffsets() {
  let TLayout = MemoryLayout<TestStruct>.self
  printOffset(TLayout.offset(of: \TestStruct.x))
}

@inline(never)
func testit() {
  print("### testStructOffsets")
  testStructOffsets()
  print("### testGenericStructOffsets")
  testGenericStructOffsets(Int.self)
  print("### testNonOffsets")
  testNonOffsets()
  print("### testSelfOffsets")
  testSelfOffsets()
  print("### testTupleOffsets")
  testTupleOffsets()
  print("### testGenericTupleOffsets")
  testGenericTupleOffsets(Int.self)
  print("### testResilientOffsets")
  testResilientOffsets()
  print("### testTupleOfFunctions")
  testTupleOfFunctions()
}

testit()
