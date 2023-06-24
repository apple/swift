// RUN: %target-swiftc_driver %s -Xfrontend -parse-as-library -g -sanitize=thread %import-libdispatch -o %t_tsan-binary
// RUN: %target-codesign %t_tsan-binary
// RUN: env %env-TSAN_OPTIONS=abort_on_error=0 %target-run %t_tsan-binary 2>&1 | %FileCheck %s --implicit-check-not='ThreadSanitizer'
// REQUIRES: executable_test
// REQUIRES: tsan_runtime

// rdar://101876380
// UNSUPPORTED: OS=ios

// FIXME: This should be covered by "tsan_runtime"; older versions of Apple OSs
// don't support TSan.
// UNSUPPORTED: remote_run

// Test that we do not report a race on initialization; Swift doesn't initialize
// globals at start-up, but rather uses `swift_once()`.  This is thread safe, but
// on some platforms TSan wasn't seeing the synchronization, so would report
// a false positive.

import Dispatch

var count = 0

// This initialization will be done via a call to `swift_once()`.  Prior to
// the fix for rdar://110665213, the addition to `count` would trigger a
// TSan message on Linux because the sanitizer couldn't see the lock we're
// using to make `swift_once()` thread safe.
let foo = {
  count += 1
  return count
}()

@main
struct Main {
  static func main() {
    let q = DispatchQueue(label: "q", attributes: .concurrent)
    let finished = DispatchSemaphore(value: 0)
    let count = 100

    for _ in 0..<count {
      q.async {
        print(foo)
        finished.signal()
      }
    }

    for _ in 0..<count {
      finished.wait()
    }

    print("Done!")
  }
}

// CHECK-NOT: ThreadSanitizer: data race
// CHECK: Done!
