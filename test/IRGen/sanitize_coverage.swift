// REQUIRES: asan_runtime
// RUN: %target-swift-frontend -emit-ir -sanitize=address -sanitize-coverage=func %s | %FileCheck %s -check-prefix=SANCOV
// RUN: %target-swift-frontend -emit-ir -sanitize=address -sanitize-coverage=bb %s | %FileCheck %s -check-prefix=SANCOV
// RUN: %target-swift-frontend -emit-ir -sanitize=address -sanitize-coverage=edge %s | %FileCheck %s -check-prefix=SANCOV
// RUN: %target-swift-frontend -emit-ir -sanitize=fuzzer %s | %FileCheck %s -check-prefix=SANCOV
// RUN: %target-swift-frontend -emit-ir -sanitize=address -sanitize-coverage=edge,trace-cmp %s | %FileCheck %s -check-prefix=SANCOV -check-prefix=SANCOV_TRACE_CMP
// RUN: %target-swift-frontend -emit-ir -sanitize=address -sanitize-coverage=edge,trace-bb %s | %FileCheck %s -check-prefix=SANCOV -check-prefix=SANCOV_TRACE_BB
// RUN: %target-swift-frontend -emit-ir -sanitize=address -sanitize-coverage=edge,indirect-calls %s | %FileCheck %s -check-prefix=SANCOV -check-prefix=SANCOV_INDIRECT_CALLS
// RUN: %target-swift-frontend -emit-ir -sanitize=address -sanitize-coverage=edge,8bit-counters %s | %FileCheck %s -check-prefix=SANCOV -check-prefix=SANCOV_8BIT_COUNTERS
// RUN: %target-swift-frontend -emit-ir -sanitize=fuzzer %s | %FileCheck %s -check-prefix=SANCOV -check-prefix=SANCOV_TRACE_CMP
// These sanitizers aren't supported when targeting Wasm.
// UNSUPPORTED: wasm

#if canImport(Darwin)
  import Darwin
#elseif canImport(Glibc)
  import Glibc
#elseif canImport(Android)
  import Android
#elseif os(Windows)
  import CRT
#else
#error("Unsupported platform")
#endif

// FIXME: We should have a reliable way of triggering an indirect call in the
// LLVM IR generated from this code.
func test() {
  // Use random numbers so the compiler can't constant fold
#if canImport(Darwin)
  let x = arc4random()
  let y = arc4random()
#else
  let x = rand()
  let y = rand()
#endif
  // Comparison is to trigger insertion of __sanitizer_cov_trace_cmp
  let z = x == y
  print("\(z)")
}

test()

// FIXME: We need a way to distinguish the different types of coverage instrumentation
// that isn't really fragile. For now just check there's at least one call to the function
// used to increment coverage count at a particular PC.
// SANCOV_TRACE_CMP: call void @__sanitizer_cov_trace_cmp

// SANCOV: call void @__sanitizer_cov
