// RUN: %empty-directory(%t)
// RUN: %target-build-swift %s -parse-as-library -Onone -g -o %t/CrashAsync
// RUN: %target-codesign %t/CrashAsync

// Demangling is disabled for now because older macOS can't demangle async
// function names.  We test demangling elsewhere, so this is no big deal.

// RUN: (env SWIFT_BACKTRACE=enable=yes,demangle=no,cache=no %target-run %t/CrashAsync 2>&1 || true) | %FileCheck %s
// RUN: (env SWIFT_BACKTRACE=preset=friendly,enable=yes,demangle=no,cache=no %target-run %t/CrashAsync 2>&1 || true) | %FileCheck %s --check-prefix FRIENDLY

// UNSUPPORTED: use_os_stdlib
// UNSUPPORTED: back_deployment_runtime
// UNSUPPORTED: asan
// REQUIRES: executable_test
// REQUIRES: backtracing
// REQUIRES: OS=macosx || OS=linux-gnu

@available(SwiftStdlib 5.1, *)
func crash() {
  let ptr = UnsafeMutablePointer<Int>(bitPattern: 4)!
  ptr.pointee = 42
}

@available(SwiftStdlib 5.1, *)
func level(_ n: Int) async {
  if n < 5 {
    await level(n + 1)
  } else {
    crash()
  }
}

@available(SwiftStdlib 5.1, *)
@main
struct CrashAsync {
  static func main() async {
    await level(1)
  }
}

// CHECK: *** Program crashed: Bad pointer dereference at 0x{{0+}}4 ***

// CHECK: Thread {{[0-9]+( ".*")?}} crashed:

// CHECK:  0                  0x{{[0-9a-f]+}} {{_?}}$s10CrashAsync5crashyyF + {{[0-9]+}} in CrashAsync at {{.*}}/CrashAsync.swift:21:15
// CHECK-NEXT:  1 [ra]             0x{{[0-9a-f]+}} {{_?}}$s10CrashAsync5levelyySiYaFTY0_ + {{[0-9]+}} in CrashAsync at {{.*}}/CrashAsync.swift:29:5
// CHECK-NEXT:  2 [async]          0x{{[0-9a-f]+}} {{_?}}$s10CrashAsync5levelyySiYaFTQ1_ in CrashAsync at {{.*}}/CrashAsync.swift:27
// CHECK-NEXT:  3 [async]          0x{{[0-9a-f]+}} {{_?}}$s10CrashAsync5levelyySiYaFTQ1_ in CrashAsync at {{.*}}/CrashAsync.swift:27
// CHECK-NEXT:  4 [async]          0x{{[0-9a-f]+}} {{_?}}$s10CrashAsync5levelyySiYaFTQ1_ in CrashAsync at {{.*}}/CrashAsync.swift:27
// CHECK-NEXT:  5 [async]          0x{{[0-9a-f]+}} {{_?}}$s10CrashAsync5levelyySiYaFTQ1_ in CrashAsync at {{.*}}/CrashAsync.swift:27
// CHECK-NEXT:  6 [async]          0x{{[0-9a-f]+}} {{_?}}$s10CrashAsyncAAV4mainyyYaFZ{{TQ0_| \+ [0-9]+}} in CrashAsync at {{.*}}/CrashAsync.swift:37
// CHECK-NEXT:  7 [async] [system] 0x{{[0-9a-f]+}} {{_?}}$s10CrashAsyncAAV5$mainyyYaFZ{{TQ0_| \+ [0-9]+}} in CrashAsync at {{.*}}/<compiler-generated>
// CHECK-NEXT:  8 [async] [system] 0x{{[0-9a-f]+}} {{_?}}async_Main{{TQ0_| \+ [0-9]+}} in CrashAsync at {{.*}}/<compiler-generated>
// CHECK-NEXT:  9 [async] [thunk]  0x{{[0-9a-f]+}} {{_?}}$sIetH_yts5Error_pIegHrzo_TR{{TQ0_| \+ [0-9]+}} in CrashAsync at {{.*}}/<compiler-generated>
// CHECK-NEXT: 10 [async] [thunk]  0x{{[0-9a-f]+}} {{_?}}$sIetH_yts5Error_pIegHrzo_TRTA{{TQ0_| \+ [0-9]+}} in CrashAsync at {{.*}}/<compiler-generated>
// CHECK-NEXT: 11 [async] [system] 0x{{[0-9a-f]+}} {{_?}}_ZL23completeTaskWithClosurePN5swift12AsyncContextEPNS_10SwiftErrorE in libswift_Concurrency.{{dylib|so}}

// FRIENDLY: *** Program crashed: Bad pointer dereference at 0x{{0+}}4 ***

// FRIENDLY: Thread {{[0-9]+( ".*")?}} crashed:

// FRIENDLY: 0 {{_?}}$s10CrashAsync5crashyyF + {{[0-9]+}} in CrashAsync at {{.*}}CrashAsync.swift:21:15

// FRIENDLY:     19| func crash() {
// FRIENDLY-NEXT:     20|   let ptr = UnsafeMutablePointer<Int>(bitPattern: 4)!
// FRIENDLY-NEXT:  *  21|   ptr.pointee = 42
// FRIENDLY-NEXT:       |               ^
// FRIENDLY-NEXT:     22| }
// FRIENDLY-NEXT:     23|

// FRIENDLY: 1 {{_?}}$s10CrashAsync5levelyySiYaFTY0_ + {{[0-9]+}} in CrashAsync at {{.*}}CrashAsync.swift:29:5

// FRIENDLY:     27|     await level(n + 1)
// FRIENDLY-NEXT:     28|   } else {
// FRIENDLY-NEXT:  *  29|     crash()
// FRIENDLY-NEXT:       |     ^
// FRIENDLY-NEXT:     30|   }
// FRIENDLY-NEXT:     31| }

// FRIENDLY:2 {{_?}}$s10CrashAsync5levelyySiYaFTQ1_ in CrashAsync at {{.*}}CrashAsync.swift:27

// FRIENDLY:     25| func level(_ n: Int) async {
// FRIENDLY-NEXT:     26|   if n < 5 {
// FRIENDLY-NEXT:  *  27|     await level(n + 1)
// FRIENDLY-NEXT:       |     ^
// FRIENDLY-NEXT:     28|   } else {
// FRIENDLY-NEXT:     29|     crash()

// FRIENDLY: 3 {{_?}}$s10CrashAsync5levelyySiYaFTQ1_ in CrashAsync at {{.*}}CrashAsync.swift:27
// FRIENDLY: 4 {{_?}}$s10CrashAsync5levelyySiYaFTQ1_ in CrashAsync at {{.*}}CrashAsync.swift:27
// FRIENDLY: 5 {{_?}}$s10CrashAsync5levelyySiYaFTQ1_ in CrashAsync at {{.*}}CrashAsync.swift:27
// FRIENDLY: 6 {{_?}}$s10CrashAsyncAAV4mainyyYaFZ{{TQ0_| \+ [0-9]+}} in CrashAsync at {{.*}}CrashAsync.swift:37

// FRIENDLY:     35| struct CrashAsync {
// FRIENDLY-NEXT:     36|   static func main() async {
// FRIENDLY-NEXT:  *  37|     await level(1)
// FRIENDLY-NEXT:       |     ^
// FRIENDLY-NEXT:     38|   }
// FRIENDLY-NEXT:     39| }

