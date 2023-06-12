// RUN: %empty-directory(%t)
// RUN: %target-build-swift %s -parse-as-library -Onone -g -o %t/StackOverflow
// RUN: %target-codesign %t/StackOverflow
// RUN: (env SWIFT_BACKTRACE=enable=yes,cache=no %target-run %t/StackOverflow 2>&1|| true) | %FileCheck %s
// RUN: (env SWIFT_BACKTRACE=limit=17,top=5,enable=yes,cache=no %target-run %t/StackOverflow 2>&1 || true) | %FileCheck %s --check-prefix LIMITED
// RUN: (env SWIFT_BACKTRACE=preset=friendly,enable=yes,cache=no %target-run %t/StackOverflow 2>&1 || true) | %FileCheck %s --check-prefix FRIENDLY

// UNSUPPORTED: use_os_stdlib
// UNSUPPORTED: back_deployment_runtime
// UNSUPPORTED: asan
// REQUIRES: executable_test
// REQUIRES: backtracing
// REQUIRES: OS=macosx || OS=linux-gnu

func recurse(_ level: Int) {
  if level % 100000 == 0 {
    print(level)
  }
  recurse(level + 1)
}

@main
struct StackOverflow {
  static func main() {
    recurse(1)
  }
}

// FIXME: We have to allow all the line numbers below to be off-by-one because
// of a CoreSymbolication bug.  This also means we have to skip checking the
// source code output because currently, it's pointing at the wrong line :-(

// CHECK: *** Program crashed: Bad pointer dereference at 0x{{[0-9a-f]+}} ***

// CHECK: Thread 0 {{(".*" )?}}crashed:

// CHECK:     0               0x{{[0-9a-f]+}} recurse(_:) + {{[0-9]+}} in StackOverflow at {{.*}}/StackOverflow.swift
// CHECK-NEXT:     1 [ra]          0x{{[0-9a-f]+}} recurse(_:) + {{[0-9]+}} in StackOverflow at {{.*}}/StackOverflow.swift:19:3
// CHECK-NEXT:     2 [ra]          0x{{[0-9a-f]+}} recurse(_:) + {{[0-9]+}} in StackOverflow at {{.*}}/StackOverflow.swift:19:3
// CHECK-NEXT:     3 [ra]          0x{{[0-9a-f]+}} recurse(_:) + {{[0-9]+}} in StackOverflow at {{.*}}/StackOverflow.swift:19:3
// CHECK-NEXT:     4 [ra]          0x{{[0-9a-f]+}} recurse(_:) + {{[0-9]+}} in StackOverflow at {{.*}}/StackOverflow.swift:19:3
// CHECK-NEXT:     5 [ra]          0x{{[0-9a-f]+}} recurse(_:) + {{[0-9]+}} in StackOverflow at {{.*}}/StackOverflow.swift:19:3
// CHECK-NEXT:     6 [ra]          0x{{[0-9a-f]+}} recurse(_:) + {{[0-9]+}} in StackOverflow at {{.*}}/StackOverflow.swift:19:3
// CHECK-NEXT:     7 [ra]          0x{{[0-9a-f]+}} recurse(_:) + {{[0-9]+}} in StackOverflow at {{.*}}/StackOverflow.swift:19:3
// CHECK-NEXT:     8 [ra]          0x{{[0-9a-f]+}} recurse(_:) + {{[0-9]+}} in StackOverflow at {{.*}}/StackOverflow.swift:19:3
// CHECK-NEXT:     9 [ra]          0x{{[0-9a-f]+}} recurse(_:) + {{[0-9]+}} in StackOverflow at {{.*}}/StackOverflow.swift:19:3
// CHECK-NEXT:    10 [ra]          0x{{[0-9a-f]+}} recurse(_:) + {{[0-9]+}} in StackOverflow at {{.*}}/StackOverflow.swift:19:3
// CHECK-NEXT:    11 [ra]          0x{{[0-9a-f]+}} recurse(_:) + {{[0-9]+}} in StackOverflow at {{.*}}/StackOverflow.swift:19:3
// CHECK-NEXT:    12 [ra]          0x{{[0-9a-f]+}} recurse(_:) + {{[0-9]+}} in StackOverflow at {{.*}}/StackOverflow.swift:19:3
// CHECK-NEXT:    13 [ra]          0x{{[0-9a-f]+}} recurse(_:) + {{[0-9]+}} in StackOverflow at {{.*}}/StackOverflow.swift:19:3
// CHECK-NEXT:    14 [ra]          0x{{[0-9a-f]+}} recurse(_:) + {{[0-9]+}} in StackOverflow at {{.*}}/StackOverflow.swift:19:3
// CHECK-NEXT:    15 [ra]          0x{{[0-9a-f]+}} recurse(_:) + {{[0-9]+}} in StackOverflow at {{.*}}/StackOverflow.swift:19:3
// CHECK-NEXT:    16 [ra]          0x{{[0-9a-f]+}} recurse(_:) + {{[0-9]+}} in StackOverflow at {{.*}}/StackOverflow.swift:19:3
// CHECK-NEXT:    17 [ra]          0x{{[0-9a-f]+}} recurse(_:) + {{[0-9]+}} in StackOverflow at {{.*}}/StackOverflow.swift:19:3
// CHECK-NEXT:    18 [ra]          0x{{[0-9a-f]+}} recurse(_:) + {{[0-9]+}} in StackOverflow at {{.*}}/StackOverflow.swift:19:3
// CHECK-NEXT:    19 [ra]          0x{{[0-9a-f]+}} recurse(_:) + {{[0-9]+}} in StackOverflow at {{.*}}/StackOverflow.swift:19:3
// CHECK-NEXT:    20 [ra]          0x{{[0-9a-f]+}} recurse(_:) + {{[0-9]+}} in StackOverflow at {{.*}}/StackOverflow.swift:19:3
// CHECK-NEXT:    21 [ra]          0x{{[0-9a-f]+}} recurse(_:) + {{[0-9]+}} in StackOverflow at {{.*}}/StackOverflow.swift:19:3
// CHECK-NEXT:    22 [ra]          0x{{[0-9a-f]+}} recurse(_:) + {{[0-9]+}} in StackOverflow at {{.*}}/StackOverflow.swift:19:3
// CHECK-NEXT:    23 [ra]          0x{{[0-9a-f]+}} recurse(_:) + {{[0-9]+}} in StackOverflow at {{.*}}/StackOverflow.swift:19:3
// CHECK-NEXT:    24 [ra]          0x{{[0-9a-f]+}} recurse(_:) + {{[0-9]+}} in StackOverflow at {{.*}}/StackOverflow.swift:19:3
// CHECK-NEXT:    25 [ra]          0x{{[0-9a-f]+}} recurse(_:) + {{[0-9]+}} in StackOverflow at {{.*}}/StackOverflow.swift:19:3
// CHECK-NEXT:    26 [ra]          0x{{[0-9a-f]+}} recurse(_:) + {{[0-9]+}} in StackOverflow at {{.*}}/StackOverflow.swift:19:3
// CHECK-NEXT:    27 [ra]          0x{{[0-9a-f]+}} recurse(_:) + {{[0-9]+}} in StackOverflow at {{.*}}/StackOverflow.swift:19:3
// CHECK-NEXT:    28 [ra]          0x{{[0-9a-f]+}} recurse(_:) + {{[0-9]+}} in StackOverflow at {{.*}}/StackOverflow.swift:19:3
// CHECK-NEXT:    29 [ra]          0x{{[0-9a-f]+}} recurse(_:) + {{[0-9]+}} in StackOverflow at {{.*}}/StackOverflow.swift:19:3
// CHECK-NEXT:    30 [ra]          0x{{[0-9a-f]+}} recurse(_:) + {{[0-9]+}} in StackOverflow at {{.*}}/StackOverflow.swift:19:3
// CHECK-NEXT:    31 [ra]          0x{{[0-9a-f]+}} recurse(_:) + {{[0-9]+}} in StackOverflow at {{.*}}/StackOverflow.swift:19:3
// CHECK-NEXT:    32 [ra]          0x{{[0-9a-f]+}} recurse(_:) + {{[0-9]+}} in StackOverflow at {{.*}}/StackOverflow.swift:19:3
// CHECK-NEXT:    33 [ra]          0x{{[0-9a-f]+}} recurse(_:) + {{[0-9]+}} in StackOverflow at {{.*}}/StackOverflow.swift:19:3
// CHECK-NEXT:    34 [ra]          0x{{[0-9a-f]+}} recurse(_:) + {{[0-9]+}} in StackOverflow at {{.*}}/StackOverflow.swift:19:3
// CHECK-NEXT:    35 [ra]          0x{{[0-9a-f]+}} recurse(_:) + {{[0-9]+}} in StackOverflow at {{.*}}/StackOverflow.swift:19:3
// CHECK-NEXT:    36 [ra]          0x{{[0-9a-f]+}} recurse(_:) + {{[0-9]+}} in StackOverflow at {{.*}}/StackOverflow.swift:19:3
// CHECK-NEXT:    37 [ra]          0x{{[0-9a-f]+}} recurse(_:) + {{[0-9]+}} in StackOverflow at {{.*}}/StackOverflow.swift:19:3
// CHECK-NEXT:    38 [ra]          0x{{[0-9a-f]+}} recurse(_:) + {{[0-9]+}} in StackOverflow at {{.*}}/StackOverflow.swift:19:3
// CHECK-NEXT:    39 [ra]          0x{{[0-9a-f]+}} recurse(_:) + {{[0-9]+}} in StackOverflow at {{.*}}/StackOverflow.swift:19:3
// CHECK-NEXT:    40 [ra]          0x{{[0-9a-f]+}} recurse(_:) + {{[0-9]+}} in StackOverflow at {{.*}}/StackOverflow.swift:19:3
// CHECK-NEXT:    41 [ra]          0x{{[0-9a-f]+}} recurse(_:) + {{[0-9]+}} in StackOverflow at {{.*}}/StackOverflow.swift:19:3
// CHECK-NEXT:    42 [ra]          0x{{[0-9a-f]+}} recurse(_:) + {{[0-9]+}} in StackOverflow at {{.*}}/StackOverflow.swift:19:3
// CHECK-NEXT:    43 [ra]          0x{{[0-9a-f]+}} recurse(_:) + {{[0-9]+}} in StackOverflow at {{.*}}/StackOverflow.swift:19:3
// CHECK-NEXT:    44 [ra]          0x{{[0-9a-f]+}} recurse(_:) + {{[0-9]+}} in StackOverflow at {{.*}}/StackOverflow.swift:19:3
// CHECK-NEXT:    45 [ra]          0x{{[0-9a-f]+}} recurse(_:) + {{[0-9]+}} in StackOverflow at {{.*}}/StackOverflow.swift:19:3
// CHECK-NEXT:    46 [ra]          0x{{[0-9a-f]+}} recurse(_:) + {{[0-9]+}} in StackOverflow at {{.*}}/StackOverflow.swift:19:3
// CHECK-NEXT:   ...
// CHECK-NEXT: {{[0-9]+}} [ra]          0x{{[0-9a-f]+}} recurse(_:) + {{[0-9]+}} in StackOverflow at {{.*}}/StackOverflow.swift:19:3
// CHECK-NEXT: {{[0-9]+}} [ra]          0x{{[0-9a-f]+}} recurse(_:) + {{[0-9]+}} in StackOverflow at {{.*}}/StackOverflow.swift:19:3
// CHECK-NEXT: {{[0-9]+}} [ra]          0x{{[0-9a-f]+}} recurse(_:) + {{[0-9]+}} in StackOverflow at {{.*}}/StackOverflow.swift:19:3
// CHECK-NEXT: {{[0-9]+}} [ra]          0x{{[0-9a-f]+}} recurse(_:) + {{[0-9]+}} in StackOverflow at {{.*}}/StackOverflow.swift:19:3
// CHECK-NEXT: {{[0-9]+}} [ra]          0x{{[0-9a-f]+}} recurse(_:) + {{[0-9]+}} in StackOverflow at {{.*}}/StackOverflow.swift:19:3
// CHECK-NEXT: {{[0-9]+}} [ra]          0x{{[0-9a-f]+}} recurse(_:) + {{[0-9]+}} in StackOverflow at {{.*}}/StackOverflow.swift:19:3
// CHECK-NEXT: {{[0-9]+}} [ra]          0x{{[0-9a-f]+}} recurse(_:) + {{[0-9]+}} in StackOverflow at {{.*}}/StackOverflow.swift:19:3
// CHECK-NEXT: {{[0-9]+}} [ra]          0x{{[0-9a-f]+}} recurse(_:) + {{[0-9]+}} in StackOverflow at {{.*}}/StackOverflow.swift:19:3
// CHECK-NEXT: {{[0-9]+}} [ra]          0x{{[0-9a-f]+}} recurse(_:) + {{[0-9]+}} in StackOverflow at {{.*}}/StackOverflow.swift:19:3
// CHECK-NEXT: {{[0-9]+}} [ra]          0x{{[0-9a-f]+}} recurse(_:) + {{[0-9]+}} in StackOverflow at {{.*}}/StackOverflow.swift:19:3
// CHECK-NEXT: {{[0-9]+}} [ra]          0x{{[0-9a-f]+}} recurse(_:) + {{[0-9]+}} in StackOverflow at {{.*}}/StackOverflow.swift:19:3

// The exact number of recursion frames varies from platform to platform;
// on macOS, there is a hidden dyld frame at the very top, which takes up one
// of the 16 frames.  On Linux, we may have a couple of libc frames as well.

// CHECK: {{[0-9]+}} [ra]          0x{{[0-9a-f]+}} static StackOverflow.main() + {{[0-9]+}} in StackOverflow at {{.*}}/StackOverflow.swift:25:5
// CHECK-NEXT: {{[0-9]+}} [ra] [system] 0x{{[0-9a-f]+}} static StackOverflow.$main() + {{[0-9]+}} in StackOverflow at {{.*}}/<compiler-generated>
// CHECK-NEXT: {{[0-9]+}} [ra]          0x{{[0-9a-f]+}} main + {{[0-9]+}} in StackOverflow at {{.*}}/StackOverflow.swift

// CHECK: Registers:

// CHECK: Images ({{[0-9]+}} omitted):

// CHECK: {{0x[0-9a-f]+}}–{{0x[0-9a-f]+}}{{ +}}{{[0-9a-f]+|<no build ID>}}{{ +}}StackOverflow{{ +}}{{.*}}/StackOverflow

// LIMITED: *** Program crashed: Bad pointer dereference at 0x{{[0-9a-f]+}} ***

// LIMITED: Thread 0 {{(".*" )?}}crashed:

// LIMITED:     0               0x{{[0-9a-f]+}} recurse(_:) + {{[0-9]+}} in StackOverflow at {{.*}}/StackOverflow.swift
// LIMITED-NEXT:     1 [ra]          0x{{[0-9a-f]+}} recurse(_:) + {{[0-9]+}} in StackOverflow at {{.*}}/StackOverflow.swift:19:3
// LIMITED-NEXT:     2 [ra]          0x{{[0-9a-f]+}} recurse(_:) + {{[0-9]+}} in StackOverflow at {{.*}}/StackOverflow.swift:19:3
// LIMITED-NEXT:     3 [ra]          0x{{[0-9a-f]+}} recurse(_:) + {{[0-9]+}} in StackOverflow at {{.*}}/StackOverflow.swift:19:3
// LIMITED-NEXT:     4 [ra]          0x{{[0-9a-f]+}} recurse(_:) + {{[0-9]+}} in StackOverflow at {{.*}}/StackOverflow.swift:19:3
// LIMITED-NEXT:     5 [ra]          0x{{[0-9a-f]+}} recurse(_:) + {{[0-9]+}} in StackOverflow at {{.*}}/StackOverflow.swift:19:3
// LIMITED-NEXT:     6 [ra]          0x{{[0-9a-f]+}} recurse(_:) + {{[0-9]+}} in StackOverflow at {{.*}}/StackOverflow.swift:19:3
// LIMITED-NEXT:     7 [ra]          0x{{[0-9a-f]+}} recurse(_:) + {{[0-9]+}} in StackOverflow at {{.*}}/StackOverflow.swift:19:3
// LIMITED-NEXT:     8 [ra]          0x{{[0-9a-f]+}} recurse(_:) + {{[0-9]+}} in StackOverflow at {{.*}}/StackOverflow.swift:19:3
// LIMITED-NEXT:     9 [ra]          0x{{[0-9a-f]+}} recurse(_:) + {{[0-9]+}} in StackOverflow at {{.*}}/StackOverflow.swift:19:3
// LIMITED-NEXT:    10 [ra]          0x{{[0-9a-f]+}} recurse(_:) + {{[0-9]+}} in StackOverflow at {{.*}}/StackOverflow.swift:19:3
// LIMITED-NEXT:   ...

// N.B. There can be platform differences surrounding the exact frame counts

// LIMITED: {{[0-9]+}} [ra]          0x{{[0-9a-f]+}} static StackOverflow.main() + {{[0-9]+}} in StackOverflow at {{.*}}/StackOverflow.swift:25:5
// LIMITED-NEXT: {{[0-9]+}} [ra] [system] 0x{{[0-9a-f]+}} static StackOverflow.$main() + {{[0-9]+}} in StackOverflow at {{.*}}/<compiler-generated>
// LIMITED-NEXT: {{[0-9]+}} [ra]          0x{{[0-9a-f]+}} main + {{[0-9]+}} in StackOverflow at {{.*}}/StackOverflow.swift

// FRIENDLY: *** Program crashed: Bad pointer dereference at 0x{{[0-9a-f]+}} ***

// FRIENDLY: Thread 0 {{(".*" )?}}crashed:

// FRIENDLY: {{[ ]}}0 recurse(_:) + {{[0-9]+}} in StackOverflow at {{.*}}/StackOverflow.swift

// SKIP-FRIENDLY:      9│ // REQUIRES: executable_test
// SKIP-FRIENDLY-NEXT:     10│ // REQUIRES: OS=macosx || OS=linux-gnu
// SKIP-FRIENDLY-NEXT:     11│
// SKIP-FRIENDLY-NEXT:     12│ func recurse(_ level: Int) {
// SKIP-FRIENDLY-NEXT:       │ ▲
// SKIP-FRIENDLY-NEXT:     13│   if level % 100000 == 0 {

// FRIENDLY:    1 recurse(_:) + {{[0-9]+}} in StackOverflow at {{.*}}/StackOverflow.swift:19:3

// SKIP-FRIENDLY:     13│   if level % 100000 == 0 {
// SKIP-FRIENDLY-NEXT:     14│     print(level)
// SKIP-FRIENDLY-NEXT:     15│   }
// SKIP-FRIENDLY-NEXT:     16│   recurse(level + 1)
// SKIP-FRIENDLY-NEXT:       │   ▲
// SKIP-FRIENDLY-NEXT:     17│ }

// FRIENDLY:     2 recurse(_:) + {{[0-9]+}} in StackOverflow at {{.*}}/StackOverflow.swift:19:3
// FRIENDLY-NEXT:     3 recurse(_:) + {{[0-9]+}} in StackOverflow at {{.*}}/StackOverflow.swift:19:3
// FRIENDLY-NEXT:     4 recurse(_:) + {{[0-9]+}} in StackOverflow at {{.*}}/StackOverflow.swift:19:3
// FRIENDLY-NEXT:     5 recurse(_:) + {{[0-9]+}} in StackOverflow at {{.*}}/StackOverflow.swift:19:3
// FRIENDLY-NEXT:     6 recurse(_:) + {{[0-9]+}} in StackOverflow at {{.*}}/StackOverflow.swift:19:3
// FRIENDLY-NEXT:     7 recurse(_:) + {{[0-9]+}} in StackOverflow at {{.*}}/StackOverflow.swift:19:3
// FRIENDLY-NEXT:     8 recurse(_:) + {{[0-9]+}} in StackOverflow at {{.*}}/StackOverflow.swift:19:3
// FRIENDLY-NEXT:     9 recurse(_:) + {{[0-9]+}} in StackOverflow at {{.*}}/StackOverflow.swift:19:3
// FRIENDLY-NEXT:    10 recurse(_:) + {{[0-9]+}} in StackOverflow at {{.*}}/StackOverflow.swift:19:3
// FRIENDLY-NEXT:    11 recurse(_:) + {{[0-9]+}} in StackOverflow at {{.*}}/StackOverflow.swift:19:3
// FRIENDLY-NEXT:    12 recurse(_:) + {{[0-9]+}} in StackOverflow at {{.*}}/StackOverflow.swift:19:3
// FRIENDLY-NEXT:    13 recurse(_:) + {{[0-9]+}} in StackOverflow at {{.*}}/StackOverflow.swift:19:3
// FRIENDLY-NEXT:    14 recurse(_:) + {{[0-9]+}} in StackOverflow at {{.*}}/StackOverflow.swift:19:3
// FRIENDLY-NEXT:    15 recurse(_:) + {{[0-9]+}} in StackOverflow at {{.*}}/StackOverflow.swift:19:3
// FRIENDLY-NEXT:    16 recurse(_:) + {{[0-9]+}} in StackOverflow at {{.*}}/StackOverflow.swift:19:3
// FRIENDLY-NEXT:    17 recurse(_:) + {{[0-9]+}} in StackOverflow at {{.*}}/StackOverflow.swift:19:3
// FRIENDLY-NEXT:    18 recurse(_:) + {{[0-9]+}} in StackOverflow at {{.*}}/StackOverflow.swift:19:3
// FRIENDLY-NEXT:    19 recurse(_:) + {{[0-9]+}} in StackOverflow at {{.*}}/StackOverflow.swift:19:3
// FRIENDLY-NEXT:    20 recurse(_:) + {{[0-9]+}} in StackOverflow at {{.*}}/StackOverflow.swift:19:3
// FRIENDLY-NEXT:    21 recurse(_:) + {{[0-9]+}} in StackOverflow at {{.*}}/StackOverflow.swift:19:3
// FRIENDLY-NEXT:    22 recurse(_:) + {{[0-9]+}} in StackOverflow at {{.*}}/StackOverflow.swift:19:3
// FRIENDLY-NEXT:    23 recurse(_:) + {{[0-9]+}} in StackOverflow at {{.*}}/StackOverflow.swift:19:3
// FRIENDLY-NEXT:    24 recurse(_:) + {{[0-9]+}} in StackOverflow at {{.*}}/StackOverflow.swift:19:3
// FRIENDLY-NEXT:    25 recurse(_:) + {{[0-9]+}} in StackOverflow at {{.*}}/StackOverflow.swift:19:3
// FRIENDLY-NEXT:    26 recurse(_:) + {{[0-9]+}} in StackOverflow at {{.*}}/StackOverflow.swift:19:3
// FRIENDLY-NEXT:    27 recurse(_:) + {{[0-9]+}} in StackOverflow at {{.*}}/StackOverflow.swift:19:3
// FRIENDLY-NEXT:    28 recurse(_:) + {{[0-9]+}} in StackOverflow at {{.*}}/StackOverflow.swift:19:3
// FRIENDLY-NEXT:    29 recurse(_:) + {{[0-9]+}} in StackOverflow at {{.*}}/StackOverflow.swift:19:3
// FRIENDLY-NEXT:    30 recurse(_:) + {{[0-9]+}} in StackOverflow at {{.*}}/StackOverflow.swift:19:3
// FRIENDLY-NEXT:    31 recurse(_:) + {{[0-9]+}} in StackOverflow at {{.*}}/StackOverflow.swift:19:3
// FRIENDLY-NEXT:    32 recurse(_:) + {{[0-9]+}} in StackOverflow at {{.*}}/StackOverflow.swift:19:3
// FRIENDLY-NEXT:    33 recurse(_:) + {{[0-9]+}} in StackOverflow at {{.*}}/StackOverflow.swift:19:3
// FRIENDLY-NEXT:    34 recurse(_:) + {{[0-9]+}} in StackOverflow at {{.*}}/StackOverflow.swift:19:3
// FRIENDLY-NEXT:    35 recurse(_:) + {{[0-9]+}} in StackOverflow at {{.*}}/StackOverflow.swift:19:3
// FRIENDLY-NEXT:    36 recurse(_:) + {{[0-9]+}} in StackOverflow at {{.*}}/StackOverflow.swift:19:3
// FRIENDLY-NEXT:    37 recurse(_:) + {{[0-9]+}} in StackOverflow at {{.*}}/StackOverflow.swift:19:3
// FRIENDLY-NEXT:    38 recurse(_:) + {{[0-9]+}} in StackOverflow at {{.*}}/StackOverflow.swift:19:3
// FRIENDLY-NEXT:    39 recurse(_:) + {{[0-9]+}} in StackOverflow at {{.*}}/StackOverflow.swift:19:3
// FRIENDLY-NEXT:    40 recurse(_:) + {{[0-9]+}} in StackOverflow at {{.*}}/StackOverflow.swift:19:3
// FRIENDLY-NEXT:    41 recurse(_:) + {{[0-9]+}} in StackOverflow at {{.*}}/StackOverflow.swift:19:3
// FRIENDLY-NEXT:    42 recurse(_:) + {{[0-9]+}} in StackOverflow at {{.*}}/StackOverflow.swift:19:3
// FRIENDLY-NEXT:    43 recurse(_:) + {{[0-9]+}} in StackOverflow at {{.*}}/StackOverflow.swift:19:3
// FRIENDLY-NEXT:    44 recurse(_:) + {{[0-9]+}} in StackOverflow at {{.*}}/StackOverflow.swift:19:3
// FRIENDLY-NEXT:    45 recurse(_:) + {{[0-9]+}} in StackOverflow at {{.*}}/StackOverflow.swift:19:3
// FRIENDLY-NEXT:    46 recurse(_:) + {{[0-9]+}} in StackOverflow at {{.*}}/StackOverflow.swift:19:3
// FRIENDLY-NEXT:   ...
// FRIENDLY-NEXT: {{[0-9]+}} recurse(_:) + {{[0-9]+}} in StackOverflow at {{.*}}/StackOverflow.swift:19:3
// FRIENDLY-NEXT: {{[0-9]+}} recurse(_:) + {{[0-9]+}} in StackOverflow at {{.*}}/StackOverflow.swift:19:3
// FRIENDLY-NEXT: {{[0-9]+}} recurse(_:) + {{[0-9]+}} in StackOverflow at {{.*}}/StackOverflow.swift:19:3
// FRIENDLY-NEXT: {{[0-9]+}} recurse(_:) + {{[0-9]+}} in StackOverflow at {{.*}}/StackOverflow.swift:19:3
// FRIENDLY-NEXT: {{[0-9]+}} recurse(_:) + {{[0-9]+}} in StackOverflow at {{.*}}/StackOverflow.swift:19:3
// FRIENDLY-NEXT: {{[0-9]+}} recurse(_:) + {{[0-9]+}} in StackOverflow at {{.*}}/StackOverflow.swift:19:3
// FRIENDLY-NEXT: {{[0-9]+}} recurse(_:) + {{[0-9]+}} in StackOverflow at {{.*}}/StackOverflow.swift:19:3
// FRIENDLY-NEXT: {{[0-9]+}} recurse(_:) + {{[0-9]+}} in StackOverflow at {{.*}}/StackOverflow.swift:19:3
// FRIENDLY-NEXT: {{[0-9]+}} recurse(_:) + {{[0-9]+}} in StackOverflow at {{.*}}/StackOverflow.swift:19:3
// FRIENDLY-NEXT: {{[0-9]+}} recurse(_:) + {{[0-9]+}} in StackOverflow at {{.*}}/StackOverflow.swift:19:3
// FRIENDLY-NEXT: {{[0-9]+}} recurse(_:) + {{[0-9]+}} in StackOverflow at {{.*}}/StackOverflow.swift:19:3

// N.B. There can be platform differences surrounding the exact frame counts

// FRIENDLY: {{[0-9]+}} static StackOverflow.main() + {{[0-9]+}} in StackOverflow at {{.*}}/StackOverflow.swift:25:5

// SKIP-FRIENDLY:     19│ @main
// SKIP-FRIENDLY-NEXT:     20│ struct StackOverflow {
// SKIP-FRIENDLY-NEXT:     21│   static func main() {
// SKIP-FRIENDLY-NEXT:     22│     recurse(1)
// SKIP-FRIENDLY-NEXT:       │     ▲
// SKIP-FRIENDLY-NEXT:     23│   }
