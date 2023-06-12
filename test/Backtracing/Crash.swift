// RUN: %empty-directory(%t)
// RUN: %target-build-swift %s -parse-as-library -Onone -g -o %t/Crash
// RUN: %target-build-swift %s -parse-as-library -Onone -o %t/CrashNoDebug
// RUN: %target-build-swift %s -parse-as-library -O -g -o %t/CrashOpt
// RUN: %target-build-swift %s -parse-as-library -O -o %t/CrashOptNoDebug
// RUN: %target-codesign %t/Crash
// RUN: %target-codesign %t/CrashNoDebug
// RUN: %target-codesign %t/CrashOpt
// RUN: %target-codesign %t/CrashOptNoDebug
// RUN: (env SWIFT_BACKTRACE=enable=yes,cache=no %target-run %t/Crash 2>&1 || true) | %FileCheck %s
// RUN: (env SWIFT_BACKTRACE=preset=friendly,enable=yes,cache=no %target-run %t/Crash 2>&1 || true) | %FileCheck %s --check-prefix FRIENDLY
// RUN: (env SWIFT_BACKTRACE=enable=yes,cache=no %target-run %t/CrashNoDebug 2>&1 || true) | %FileCheck %s --check-prefix NODEBUG
// RUN: (env SWIFT_BACKTRACE=enable=yes,cache=no %target-run %t/CrashOpt 2>&1 || true) | %FileCheck %s --check-prefix OPTIMIZED
// RUN: (env SWIFT_BACKTRACE=enable=yes,cache=no %target-run %t/CrashOptNoDebug 2>&1 || true) | %FileCheck %s --check-prefix OPTNODEBUG

// UNSUPPORTED: use_os_stdlib
// UNSUPPORTED: back_deployment_runtime
// UNSUPPORTED: asan
// REQUIRES: executable_test
// REQUIRES: backtracing
// REQUIRES: OS=macosx || OS=linux-gnu

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
  print("About to crash")
  let ptr = UnsafeMutablePointer<Int>(bitPattern: 4)!
  ptr.pointee = 42
}

@main
struct Crash {
  static func main() {
    level1()
  }
}

// CHECK: *** Program crashed: Bad pointer dereference at 0x{{0+}}4 ***

// CHECK: Thread 0 {{(".*" )?}}crashed:

// CHECK: 0               0x{{[0-9a-f]+}} level5() + {{[0-9]+}} in Crash at {{.*}}/Crash.swift:42:15
// CHECK-NEXT: 1 [ra]          0x{{[0-9a-f]+}} level4() + {{[0-9]+}} in Crash at {{.*}}/Crash.swift:36:3
// CHECK-NEXT: 2 [ra]          0x{{[0-9a-f]+}} level3() + {{[0-9]+}} in Crash at {{.*}}/Crash.swift:32:3
// CHECK-NEXT: 3 [ra]          0x{{[0-9a-f]+}} level2() + {{[0-9]+}} in Crash at {{.*}}/Crash.swift:28:3
// CHECK-NEXT: 4 [ra]          0x{{[0-9a-f]+}} level1() + {{[0-9]+}} in Crash at {{.*}}/Crash.swift:24:3
// CHECK-NEXT: 5 [ra]          0x{{[0-9a-f]+}} static Crash.main() + {{[0-9]+}} in Crash at {{.*}}/Crash.swift:48:5
// CHECK-NEXT: 6 [ra] [system] 0x{{[0-9a-f]+}} static Crash.$main() + {{[0-9]+}} in Crash at {{.*}}/<compiler-generated>
// CHECK-NEXT: 7 [ra] 0x{{[0-9a-f]+}} main + {{[0-9]+}} in Crash at {{.*}}/Crash.swift

// CHECK: Registers:

// CHECK: Images ({{[0-9]+}} omitted):

// CHECK: {{0x[0-9a-f]+}}–{{0x[0-9a-f]+}}{{ +}}{{([0-9a-f]+|<no build ID>)}}{{ +}}Crash{{ +}}{{.*}}/Crash

// FRIENDLY: *** Program crashed: Bad pointer dereference at 0x{{0+}}4 ***

// FRIENDLY: Thread 0 {{(".*" )?}}crashed:

// FRIENDLY: 0 level5() + {{[0-9]+}} in Crash at {{.*}}/Crash.swift:42:15

// FRIENDLY: 40|   print("About to crash")
// FRIENDLY-NEXT: 41|   let ptr = UnsafeMutablePointer<Int>(bitPattern: 4)!
// FRIENDLY-NEXT: 42|   ptr.pointee = 42
// FRIENDLY-NEXT:   |               ^
// FRIENDLY-NEXT: 43| }
// FRIENDLY-NEXT: 44|

// FRIENDLY: 1 level4() + {{[0-9]+}} in Crash at {{.*}}/Crash.swift:36:3

// FRIENDLY: 34|
// FRIENDLY-NEXT: 35| func level4() {
// FRIENDLY-NEXT: 36|   level5()
// FRIENDLY-NEXT:   |   ^
// FRIENDLY-NEXT: 37| }
// FRIENDLY-NEXT: 38|

// FRIENDLY: 2 level3() + {{[0-9]+}} in Crash at {{.*}}/Crash.swift:32:3

// FRIENDLY: 30|
// FRIENDLY-NEXT: 31| func level3() {
// FRIENDLY-NEXT: 32|   level4()
// FRIENDLY-NEXT:   |   ^
// FRIENDLY-NEXT: 33| }
// FRIENDLY-NEXT: 34|

// FRIENDLY: 3 level2() + {{[0-9]+}} in Crash at {{.*}}/Crash.swift:28:3

// FRIENDLY: 26|
// FRIENDLY-NEXT: 27| func level2() {
// FRIENDLY-NEXT: 28|   level3()
// FRIENDLY-NEXT:   |   ^
// FRIENDLY-NEXT: 29| }
// FRIENDLY-NEXT: 30|

// FRIENDLY: 4 level1() + {{[0-9]+}} in Crash at {{.*}}/Crash.swift:24:3

// FRIENDLY:  22|
// FRIENDLY-NEXT: 23| func level1() {
// FRIENDLY-NEXT: 24|   level2()
// FRIENDLY-NEXT:   |   ^
// FRIENDLY-NEXT: 25| }
// FRIENDLY-NEXT: 26|

// FRIENDLY: 5 static Crash.main() + {{[0-9]+}} in Crash at {{.*}}/Crash.swift:48:5

// FRIENDLY: 46| struct Crash {
// FRIENDLY-NEXT: 47|   static func main() {
// FRIENDLY-NEXT: 48|     level1()
// FRIENDLY-NEXT:   |     ^
// FRIENDLY-NEXT: 49|   }
// FRIENDLY-NEXT: 50| }

// NODEBUG: *** Program crashed: Bad pointer dereference at 0x{{0*}}4 ***

// NODEBUG: Thread 0 {{(".*" )?}}crashed:

// NODEBUG: 0               0x{{[0-9a-f]+}} level5() + {{[0-9]+}} in CrashNoDebug
// NODEBUG: 1 [ra]          0x{{[0-9a-f]+}} level4() + {{[0-9]+}} in CrashNoDebug
// NODEBUG: 2 [ra]          0x{{[0-9a-f]+}} level3() + {{[0-9]+}} in CrashNoDebug
// NODEBUG: 3 [ra]          0x{{[0-9a-f]+}} level2() + {{[0-9]+}} in CrashNoDebug
// NODEBUG: 4 [ra]          0x{{[0-9a-f]+}} level1() + {{[0-9]+}} in CrashNoDebug
// NODEBUG: 5 [ra]          0x{{[0-9a-f]+}} static Crash.main() + {{[0-9]+}} in CrashNoDebug
// NODEBUG: 6 [ra] [system] 0x{{[0-9a-f]+}} static Crash.$main() + {{[0-9]+}} in CrashNoDebug
// NODEBUG: 7 [ra]          0x{{[0-9a-f]+}} main + {{[0-9]+}} in CrashNoDebug

// NODEBUG: Registers:

// NODEBUG: Images ({{[0-9]+}} omitted):

// NODEBUG: {{0x[0-9a-f]+}}–{{0x[0-9a-f]+}}{{ +}}{{([0-9a-f]+|<no build ID>)}}{{ +}}CrashNoDebug{{ +}}{{.*}}/CrashNoDebug

// OPTIMIZED: *** Program crashed: Bad pointer dereference at 0x{{0+}}4 ***

// OPTIMIZED: Thread 0 {{(".*" )?}}crashed:

// OPTIMIZED: 0 [inlined]          0x{{[0-9a-f]+}} level5() in CrashOpt at {{.*}}/Crash.swift:42:15
// OPTIMIZED-NEXT: 1 [inlined]          0x{{[0-9a-f]+}} level4() in CrashOpt at {{.*}}/Crash.swift:36:3
// OPTIMIZED-NEXT: 2 [inlined]          0x{{[0-9a-f]+}} level3() in CrashOpt at {{.*}}/Crash.swift:32:3
// OPTIMIZED-NEXT: 3 [inlined]          0x{{[0-9a-f]+}} level2() in CrashOpt at {{.*}}/Crash.swift:28:3
// OPTIMIZED-NEXT: 4 [inlined]          0x{{[0-9a-f]+}} level1() in CrashOpt at {{.*}}/Crash.swift:24:3
// OPTIMIZED-NEXT: 5 [inlined]          0x{{[0-9a-f]+}} static Crash.main() in CrashOpt at {{.*}}/Crash.swift:48:5
// OPTIMIZED: {{6|7}} [system]           0x{{[0-9a-f]+}} main + {{[0-9]+}} in CrashOpt at {{.*}}

// OPTIMIZED: Registers:

// OPTIMIZED: Images ({{[0-9]+}} omitted):

// OPTIMIZED: {{0x[0-9a-f]+}}–{{0x[0-9a-f]+}}{{ +}}{{([0-9a-f]+|<no build ID>)}}{{ +}}CrashOpt{{ +}}{{.*}}/CrashOpt

// OPTNODEBUG: *** Program crashed: Bad pointer dereference at 0x{{0*}}4 ***

// OPTNODEBUG: Thread 0 {{(".*" )?}}crashed:

// OPTNODEBUG: 0               0x{{[0-9a-f]+}} main + {{[0-9]+}} in CrashOptNoDebug

// OPTNODEBUG: Registers:

// OPTNODEBUG: Images ({{[0-9]+}} omitted):

// OPTNODEBUG: {{0x[0-9a-f]+}}–{{0x[0-9a-f]+}}{{ +}}{{([0-9a-f]+|<no build ID>)}}{{ +}}CrashOptNoDebug{{ +}}{{.*}}/CrashOptNoDebug

