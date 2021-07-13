// RUN: %target-swiftc_driver %s -target %sanitizers-target-triple -g -sanitize=thread %import-libdispatch -o %t_tsan-binary
// RUN: %target-codesign %t_tsan-binary
// RUN: env %env-TSAN_OPTIONS="abort_on_error=0" not %target-run %t_tsan-binary 2>&1 | %FileCheck %s
// REQUIRES: executable_test
// REQUIRES: tsan_runtime
// UNSUPPORTED: OS=tvos
// UNSUPPORTED: CPU=powerpc64le

// FIXME: This should be covered by "tsan_runtime"; older versions of Apple OSs
// don't support TSan.
// UNSUPPORTED: remote_run

// XFAIL: freebsd

#if canImport(Darwin)
  import Darwin
#elseif canImport(Glibc)
  import Glibc
#elseif os(Windows)
  import CRT
#else
#error("Unsupported platform")
#endif

// Make sure we can handle swifterror and don't bail during the LLVM
// threadsanitizer pass.

enum MyError : Error {
    case A
}

public func foobar(_ x: Int) throws {
  if x == 0 {
    throw MyError.A
  }
}

public func call_foobar() {
  do {
    try foobar(1)
  } catch(_) { }
}

// Test ThreadSanitizer execution end-to-end.

var threads: [pthread_t] = []
var racey_x: Int;

// TSan %deflake as part of the test.
for _ in 1...5 {
#if os(macOS) || os(iOS) || os(FreeBSD)
  var t : pthread_t?
#else
  var t : pthread_t = 0
#endif
  pthread_create(&t, nil, { _ in
    print("pthread ran")
    racey_x = 5;

    return nil
  }, nil)
#if os(macOS) || os(iOS) || os(FreeBSD)
  threads.append(t!)
#else
  threads.append(t)
#endif
}

for t in threads {
  if t == nil {
    print("nil thread")
    continue
  }
  print("Thread \(t)")
  pthread_join(t, nil)
}

// CHECK: ThreadSanitizer: data race
