// RUN: %target-swift-frontend  -primary-file %s -O -module-name=test -emit-ir | %FileCheck %s

// Also do an end-to-end test.
// RUN: %empty-directory(%t) 
// RUN: %target-build-swift -O -module-name=test %s -o %t/a.out
// RUN: %target-codesign %t/a.out
// RUN: %target-run %t/a.out | %FileCheck %s -check-prefix=CHECK-OUTPUT
// REQUIRES: executable_test,optimized_stdlib
// REQUIRES: swift_in_compiler

struct Empty { }

struct Mystruct {
  var a: Int
  var b: Empty
  var c: Int

  // CHECK: @{{[^ ]*structglobal[^ ]*}} = hidden global %{{[^ ]*}} <{ %TSi <{ i{{[0-9]+}} 3 }>, %TSi <{ i{{[0-9]+}} 4 }> }>
  static var structglobal = Mystruct(a: 3, b: Empty(), c: 4)
  // CHECK: @{{[^ ]*tupleglobal[^ ]*}} = hidden global <{ %TSi, %TSi }> <{ %TSi <{ i{{[0-9]+}} 5 }>, %TSi <{ i{{[0-9]+}} 6 }> }>
  static var tupleglobal = (a: 5, b: Empty(), c: 6)

  static var empty: () = ()
  static var ptrToEmpty = UnsafePointer(&empty)
}

// CHECK-OUTPUT:      3
print(Mystruct.structglobal.a)
// CHECK-OUTPUT-NEXT: Empty()
print(Mystruct.structglobal.b)
// CHECK-OUTPUT-NEXT: 4
print(Mystruct.structglobal.c)
// CHECK-OUTPUT-NEXT: 5
print(Mystruct.tupleglobal.a)
// CHECK-OUTPUT-NEXT: Empty()
print(Mystruct.tupleglobal.b)
// CHECK-OUTPUT-NEXT: 6
print(Mystruct.tupleglobal.c)
// CHECK-OUTPUT-NEXT: {{0x0+$}}
print(Mystruct.ptrToEmpty)

