// RUN: %target-run-simple-swift( -Xfrontend -disable-availability-checking %import-libdispatch)
// REQUIRES: concurrency
// REQUIRES: executable_test

// rdar://76038845
// REQUIRES: concurrency_runtime
// UNSUPPORTED: back_deployment_runtime
// UNSUPPORTED: freestanding

// for sleep
#if canImport(Darwin)
    import Darwin
#elseif canImport(Glibc)
    import Glibc
#elseif canImport(WASILibc)
    import WASILibc
#elseif os(Windows)
    import WinSDK
#endif

class Canary {
  deinit {
    print("canary died")
  }
}

if #available(SwiftStdlib 5.1, *) {
  let task = detach {
    let canary = Canary()
    _ = await Task.withCancellationHandler {
      print(canary)
    } operation: {
      await Task.sleep(1_000_000)
    }
  }
  task.cancel()
#if os(Windows)
  Sleep(1 * 1000)
#else
  sleep(1)
#endif
  detach {
    await Task.withCancellationHandler {
        print("Task was cancelled!")
    }
    operation: {
        print("Running the operation...")
    }
  }
#if os(Windows)
  Sleep(10 * 1000)
#else
  sleep(10)
#endif
} else {
  // Fake prints to satisfy FileCheck.
  print("Canary")
  print("canary died")
  print("Running the operation...")
}
// CHECK: Canary
// CHECK-NEXT: canary died
// CHECK-NEXT: Running the operation...
