// Check that there are no extra newlines in the driver diagnostics output due to
// double LF -> CR LF conversions on child processes output on Windows.
//
// Compile more than one source files to trigger the use of the driver
// task queue and child processes.

// RUN: %line-directive %s %S/Inputs/extra-newlines-2.swift -- %swiftc_driver -c -diagnostic-style=swift -no-color-diagnostics %s %S/Inputs/extra-newlines-2.swift -module-name Foo 2>&1 | %FileCheck %s

// UNSUPPORTED: swift_swift_parser

// Check that there are no extra newlines between diagnostics lines

// CHECK:      {{[=]+}} SOURCE_DIR{{[/\]+}}test{{[/\]+}}diagnostics{{[/\]+}}extra-newlines.swift:[[#LINE:]]:5
// CHECK-NEXT: [[#LINE-1]] | func foo(a: Int, b: Int) {
// CHECK-NEXT: [[#LINE]]   |   a + b
// CHECK-NEXT:             |   ~   ~
// CHECK-NEXT:             |     ^ warning: result of operator '+' is unused
// CHECK-NEXT: [[#LINE+1]] | }

func foo(a: Int, b: Int) {
  a + b
}