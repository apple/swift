// RUN: %empty-directory(%t)
// RUN: %target-build-swift %s -parse-as-library -Onone -g -o %t/CrashWithThunk
// RUN: %target-codesign %t/CrashWithThunk
// RUN: (env SWIFT_BACKTRACE=enable=yes,cache=no %target-run %t/CrashWithThunk 2>&1 || true) | %FileCheck %s
// RUN: (env SWIFT_BACKTRACE=preset=friendly,enable=yes,cache=no %target-run %t/CrashWithThunk 2>&1 || true) | %FileCheck %s --check-prefix FRIENDLY

// UNSUPPORTED: use_os_stdlib
// UNSUPPORTED: back_deployment_runtime
// UNSUPPORTED: asan
// REQUIRES: executable_test
// REQUIRES: backtracing
// REQUIRES: OS=macosx || OS=linux-gnu

struct Foo<T> {
  var value: T
}

func crash() {
  print("I'm going to crash here")
  let ptr = UnsafeMutablePointer<Int>(bitPattern: 4)!
  ptr.pointee = 42
}

@main
struct CrashWithThunk {
  static func main() {
    let foo = Foo(value: crash)

    foo.value()
  }
}

// CHECK: *** Program crashed: Bad pointer dereference at 0x{{0+}}4 ***

// CHECK: Thread 0 {{(".*" )?}}crashed:

// CHECK: 0                    0x{{[0-9a-f]+}} crash() + {{[0-9]+}} in CrashWithThunk at {{.*}}/CrashWithThunk.swift:21:15
// CHECK-NEXT: 1 [ra] [thunk]  0x{{[0-9a-f]+}} thunk for @escaping @callee_guaranteed () -> () + {{[0-9]+}} in CrashWithThunk at {{.*}}/Backtracing/<compiler-generated>
// CHECK-NEXT: 2 [ra]          0x{{[0-9a-f]+}} static CrashWithThunk.main() + {{[0-9]+}} in CrashWithThunk at {{.*}}/CrashWithThunk.swift:29:9
// CHECK-NEXT: 3 [ra] [system] 0x{{[0-9a-f]+}} static CrashWithThunk.$main() + {{[0-9]+}} in CrashWithThunk at {{.*}}/<compiler-generated>
// CHECK-NEXT: 4 [ra] 0x{{[0-9a-f]+}} main + {{[0-9]+}} in CrashWithThunk at {{.*}}/CrashWithThunk.swift

// CHECK: Registers:

// CHECK: Images ({{[0-9]+}} omitted):

// CHECK: {{0x[0-9a-f]+}}–{{0x[0-9a-f]+}}{{ +}}{{[0-9a-f]+|<no build ID>}}{{ +}}CrashWithThunk{{ +}}{{.*}}/CrashWithThunk

// FRIENDLY: *** Program crashed: Bad pointer dereference at 0x{{0+}}4 ***

// FRIENDLY: Thread 0 {{(".*" )?}}crashed:

// FRIENDLY: 0 crash() + {{[0-9]+}} in CrashWithThunk at {{.*}}/CrashWithThunk.swift:21:15

// FRIENDLY: 19|   print("I'm going to crash here")
// FRIENDLY-NEXT: 20|   let ptr = UnsafeMutablePointer<Int>(bitPattern: 4)!
// FRIENDLY-NEXT: 21|   ptr.pointee = 42
// FRIENDLY-NEXT:   |               ^
// FRIENDLY-NEXT: 22| }
// FRIENDLY-NEXT: 23|

// FRIENDLY: 1 static CrashWithThunk.main() + {{[0-9]+}} in CrashWithThunk at {{.*}}/CrashWithThunk.swift:29:9

// FRIENDLY: 27|     let foo = Foo(value: crash)
// FRIENDLY-NEXT: 28|
// FRIENDLY-NEXT: 29|     foo.value()
// FRIENDLY-NEXT:   |         ^
// FRIENDLY-NEXT: 30|   }
// FRIENDLY-NEXT: 31| }
