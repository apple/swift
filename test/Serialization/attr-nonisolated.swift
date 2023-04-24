// RUN: %empty-directory(%t)
// RUN: %target-swift-frontend  -disable-availability-checking -emit-module-path %t/a.swiftmodule -module-name a %s
// RUN: llvm-bcanalyzer -dump %t/a.swiftmodule | %FileCheck --check-prefix BC-CHECK --implicit-check-not UnknownCode %s
// RUN: %target-swift-ide-test -print-module -module-to-print a -source-filename x -I %t | %FileCheck --check-prefix MODULE-CHECK %s

// REQUIRES: concurrency

///////////
// This test checks for correct serialization & deserialization of
// nonisolated

// look for correct annotation after first deserialization's module print:

// MODULE-CHECK:      actor UnsafeCounter {
// MODULE-CHECK-NEXT:   var storage: Int
// MODULE-CHECK-NEXT:   nonisolated var count: Int
// MODULE-CHECK-NEXT:   deinit
// MODULE-CHECK-NEXT:   init()
// MODULE-CHECK-NEXT: }

// and look for nonisolated

// BC-CHECK: <Nonisolated_DECL_ATTR abbrevid={{[0-9]+}} op0=0 op1=0/>


actor UnsafeCounter {

  private var storage : Int = 0

  nonisolated
  var count : Int {
    get { 0 }
    set {  }
  }
}
