// RUN: %empty-directory(%t)
// RUN: %target-build-swift %s -parse-as-library -g -Onone -o %t/SymbolicatedBacktrace
// RUN: %target-codesign %t/SymbolicatedBacktrace
// RUN: %target-run %t/SymbolicatedBacktrace | %FileCheck %s

// UNSUPPORTED: use_os_stdlib
// UNSUPPORTED: back_deployment_runtime
// REQUIRES: executable_test
// REQUIRES: backtracing
// REQUIRES: OS=macosx || OS=linux-gnu

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

  // CHECK:      0{{[ \t]+}}0x{{[0-9a-f]+}} [ra] [0] SymbolicatedBacktrace pow()
  // CHECK:      1{{[ \t]+}}0x{{[0-9a-f]+}} [ra] [0] SymbolicatedBacktrace splat()
  // CHECK:      2{{[ \t]+}}0x{{[0-9a-f]+}} [ra] [0] SymbolicatedBacktrace zonk()
  // CHECK:      3{{[ \t]+}}0x{{[0-9a-f]+}} [ra] [0] SymbolicatedBacktrace whap()
  // CHECK:      4{{[ \t]+}}0x{{[0-9a-f]+}} [ra] [0] SymbolicatedBacktrace kerpow()
  // CHECK:      5{{[ \t]+}}0x{{[0-9a-f]+}} [ra] [0] SymbolicatedBacktrace kablam()
  // CHECK:      6{{[ \t]+}}0x{{[0-9a-f]+}} [ra] [0] SymbolicatedBacktrace static SymbolicatedBacktrace.main()

  print(backtrace)
}

@main
struct SymbolicatedBacktrace {
  static func main() {
    kablam()
  }
}
