// RUN: %empty-directory(%t)
// RUN: %target-build-swift %s -parse-as-library -g -O -o %t/SymbolicatedBacktraceInline
// RUN: %target-codesign %t/SymbolicatedBacktraceInline
// RUN: %target-run %t/SymbolicatedBacktraceInline | %FileCheck %s

// UNSUPPORTED: use_os_stdlib
// UNSUPPORTED: back_deployment_runtime
// REQUIRES: executable_test
// REQUIRES: backtracing
// REQUIRES: OS=macosx

// This currently doesn't work on Linux because the unwind finishes at pow(),
// which presumably doesn't have a frame pointer.  When we add the Dwarf EH
// unwinder, we should be able to turn this test on.

import _Backtracing

func kablam() {
  kerpow()
}

func kerpow() {
  whap()
}

func whap() {
  zonk()
}

func zonk() {
  splat()
}

func splat() {
  pow()
}

func pow() {
  let backtrace = try! Backtrace.capture().symbolicated(useSymbolCache: false)!

  // CHECK:      0{{[ \t]+}}0x{{[0-9a-f]+}} [ra] [0] SymbolicatedBacktraceInline pow()
  // CHECK:      1{{[ \t]+}}0x{{[0-9a-f]+}} [ra] [inlined] [0] SymbolicatedBacktraceInline splat()
  // CHECK:      2{{[ \t]+}}0x{{[0-9a-f]+}} [ra] [inlined] [0] SymbolicatedBacktraceInline zonk()
  // CHECK:      3{{[ \t]+}}0x{{[0-9a-f]+}} [ra] [inlined] [0] SymbolicatedBacktraceInline whap()
  // CHECK:      4{{[ \t]+}}0x{{[0-9a-f]+}} [ra] [inlined] [0] SymbolicatedBacktraceInline kerpow()
  // CHECK:      5{{[ \t]+}}0x{{[0-9a-f]+}} [ra] [inlined] [0] SymbolicatedBacktraceInline kablam()
  // CHECK:      6{{[ \t]+}}0x{{[0-9a-f]+}} [ra] [inlined] [0] SymbolicatedBacktraceInline static SymbolicatedBacktraceInline.main()

  print(backtrace)
}

@main
struct SymbolicatedBacktraceInline {
  static func main() {
    kablam()
  }
}
