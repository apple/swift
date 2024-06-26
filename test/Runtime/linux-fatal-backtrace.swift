// RUN: %empty-directory(%t)
// RUN: %target-build-swift %s -o %t/a.out
// RUN: %{python} %S/../Inputs/not.py "%target-run %t/a.out" 2>&1 | %{lldb-python} %utils/symbolicate-linux-fatal %t/a.out - | %{python} %utils/backtrace-check -u
// REQUIRES: executable_test
// REQUIRES: OS=linux-gnu
// REQUIRES: lldb
// XFAIL: CPU=s390x

// NOTE: not.py is used above instead of "not --crash" because %target-run
// doesn't pass through the crash, and `not` may not be available when running
// on a remote host.

// Backtraces are not emitted when optimizations are enabled. This test can not
// run when optimizations are enabled.
// REQUIRES: swift_test_mode_optimize_none

func funcB() {
    fatalError("linux-fatal-backtrace");
}

func funcA() {
    funcB();
}

print("bla")
funcA()
