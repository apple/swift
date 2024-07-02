// RUN: %target-build-swift %s -sanitize=address -g -o %t
// RUN: env %env-ASAN_OPTIONS=abort_on_error=0                           not %target-run %t 2>&1 | %FileCheck %s -check-prefix=OOP
// RUN: env %env-ASAN_OPTIONS=abort_on_error=0,external_symbolizer_path= not %target-run %t 2>&1 | %FileCheck %s -check-prefix=IP
// REQUIRES: executable_test
// REQUIRES: asan_runtime
// REQUIRES: VENDOR=apple

// We copy the binary but not the corresponding .dSYM for remote runs (e.g.,
// on-device testing), and hence online symbolication fails.
// UNSUPPORTED: remote_run

// The 32-bit iOS simulator is old and has an outdated version of atos that
// can't demangle current Swift symbol names (the mangling scheme has changed).
// UNSUPPORTED: CPU=i386 && OS=ios

// Check that Sanitizer reports are properly symbolicated on Apple platforms,
// both out-of-process (via `atos`) and when falling back to in-process
// symbolication.  Note that `atos` also demangles Swift symbol names.

@inline(never)
func foo() {
  let x = UnsafeMutablePointer<Int>.allocate(capacity: 1)
  x.deallocate()
  print(x.pointee)
}

@inline(never)
func bar() {
  foo()
  print("Prevent tail call optimization")
}

bar()

// Out-of-process
// FIXME: There is no instruction with the location of the failing `.` operator and it's used inside implicit setup code, thus the crash is associated with the previous line.
// OOP:      #0 0x{{[0-9a-f]+}} in foo() symbolication.swift
// OOP-NEXT: #1 0x{{[0-9a-f]+}} in bar() symbolication.swift:[[@LINE-9]]
// OOP-NEXT: #2 0x{{[0-9a-f]+}} in main symbolication.swift:[[@LINE-6]]

// In-process
// IP:      #0 0x{{[0-9a-f]+}} in main.foo() -> ()+0x
// IP-NEXT: #1 0x{{[0-9a-f]+}} in main.bar() -> ()+0x
// IP-NEXT: #2 0x{{[0-9a-f]+}} in main+0x
