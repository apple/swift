// RUN: %empty-directory(%t)
// RUN: %target-build-swift %s -parse-as-library -Onone -g -o %t/Overflow
// RUN: %target-codesign %t/Overflow
// RUN: (env SWIFT_BACKTRACE=enable=yes,cache=no %target-run %t/Overflow 2>&1 || true) | %FileCheck %s
// RUN: (env SWIFT_BACKTRACE=preset=friendly,enable=yes,cache=no %target-run %t/Overflow 2>&1 || true) | %FileCheck %s --check-prefix FRIENDLY

// UNSUPPORTED: use_os_stdlib
// UNSUPPORTED: back_deployment_runtime
// REQUIRES: executable_test
// REQUIRES: backtracing
// REQUIRES: OS=macosx || OS=linux-gnu
var x: UInt = 0

func level1() {
  level2()
}

func level2() {
  level3()
}

func level3() {
  level4()
}

func level4() {
  level5()
}

func level5() {
  print("About to overflow")

  x -= 1
}

@main
struct Overflow {
  static func main() {
    level1()
  }
}

// CHECK: *** Swift runtime failure: arithmetic overflow ***

// CHECK: Thread 0 {{(".*" )?}}crashed:

// CHECK:      0 [inlined] [system] 0x{{[0-9a-f]+}} Swift runtime failure: arithmetic overflow in Overflow at {{.*}}/<compiler-generated>
// CHECK-NEXT: 1                    0x{{[0-9a-f]+}} level5() + {{[0-9]+}} in Overflow at {{.*}}/Overflow.swift:33:5
// CHECK-NEXT: 2 [ra]               0x{{[0-9a-f]+}} level4() + {{[0-9]+}} in Overflow at {{.*}}/Overflow.swift:27:3
// CHECK-NEXT: 3 [ra]               0x{{[0-9a-f]+}} level3() + {{[0-9]+}} in Overflow at {{.*}}/Overflow.swift:23:3
// CHECK-NEXT: 4 [ra]               0x{{[0-9a-f]+}} level2() + {{[0-9]+}} in Overflow at {{.*}}/Overflow.swift:19:3
// CHECK-NEXT: 5 [ra]               0x{{[0-9a-f]+}} level1() + {{[0-9]+}} in Overflow at {{.*}}/Overflow.swift:15:3
// CHECK-NEXT: 6 [ra]               0x{{[0-9a-f]+}} static Overflow.main() + {{[0-9]+}} in Overflow at {{.*}}/Overflow.swift:39:5
// CHECK-NEXT: 7 [ra] [system]      0x{{[0-9a-f]+}} static Overflow.$main() + {{[0-9]+}} in Overflow at {{.*}}/<compiler-generated>
// CHECK-NEXT: 8 [ra]               0x{{[0-9a-f]+}} main + {{[0-9]+}} in Overflow at {{.*}}/Overflow.swift

// CHECK: Registers:

// CHECK: Images ({{[0-9]+}} omitted):

// CHECK: {{0x[0-9a-f]+}}–{{0x[0-9a-f]+}}{{ +}}{{[0-9a-f]+|<no build ID>}}{{ +}}Overflow{{ +}}{{.*}}/Overflow

// FRIENDLY: *** Swift runtime failure: arithmetic overflow ***

// FRIENDLY: Thread 0 {{(".*" )?}}crashed:

// FRIENDLY: 0 level5() + {{[0-9]+}} in Overflow at {{.*}}/Overflow.swift:33:5

// FRIENDLY: 31|   print("About to overflow")
// FRIENDLY-NEXT: 32|
// FRIENDLY-NEXT: 33|   x -= 1
// FRIENDLY-NEXT:   |     ^
// FRIENDLY-NEXT: 34| }

// FRIENDLY: 1 level4() + {{[0-9]+}} in Overflow at {{.*}}/Overflow.swift:27:3

// FRIENDLY: 25|
// FRIENDLY-NEXT: 26| func level4() {
// FRIENDLY-NEXT: 27|   level5()
// FRIENDLY-NEXT:   |   ^
// FRIENDLY-NEXT: 28| }
// FRIENDLY-NEXT: 29|

// FRIENDLY: 2 level3() + {{[0-9]+}} in Overflow at {{.*}}/Overflow.swift:23:3

// FRIENDLY: 21|
// FRIENDLY-NEXT: 22| func level3() {
// FRIENDLY-NEXT: 23|   level4()
// FRIENDLY-NEXT:   |   ^
// FRIENDLY-NEXT: 24| }
// FRIENDLY-NEXT: 25|

// FRIENDLY: 3 level2() + {{[0-9]+}} in Overflow at {{.*}}/Overflow.swift:19:3

// FRIENDLY: 17|
// FRIENDLY-NEXT: 18| func level2() {
// FRIENDLY-NEXT: 19|   level3()
// FRIENDLY-NEXT:   |   ^
// FRIENDLY-NEXT: 20| }
// FRIENDLY-NEXT: 21|

// FRIENDLY: 4 level1() + {{[0-9]+}} in Overflow at {{.*}}/Overflow.swift:15:3

// FRIENDLY: 13|
// FRIENDLY-NEXT: 14| func level1() {
// FRIENDLY-NEXT: 15|   level2()
// FRIENDLY-NEXT:   |   ^
// FRIENDLY-NEXT: 16| }
// FRIENDLY-NEXT: 17|

// FRIENDLY: 5 static Overflow.main() + {{[0-9]+}} in Overflow at {{.*}}/Overflow.swift

