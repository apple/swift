// RUN: %empty-directory(%t)
// RUN: %target-build-swift %s -Xfrontend -disable-availability-checking -o %t/voucher_propagation
// RUN: %target-codesign %t/voucher_propagation
// RUN: MallocStackLogging=1 %target-run %t/voucher_propagation

// REQUIRES: executable_test
// REQUIRES: concurrency

// Use objc_interop as a proxy for voucher support in the OS.
// REQUIRES: objc_interop

// REQUIRES: concurrency_runtime
// UNSUPPORTED: back_deployment_runtime

// REQUIRES: OS=macosx

import Darwin
import Dispatch // expected-warning {{add '@preconcurrency' to suppress 'Sendable'-related warnings from module 'Dispatch'}}
import StdlibUnittest

// These are an attempt to simulate some kind of async work, and the
// computations being performed are not intended to actually make any kind of
// sense.
actor Accumulator {
  let label: String
  var x = 0

  init(label: String) {
    self.label = label
  }

  func add(_ n: Int, _ voucher: voucher_t?) async {
    let currentVoucher = voucher_copy()
    expectEqual(voucher, currentVoucher)
    print("\(n) \(label)", voucher: currentVoucher)
    os_release(currentVoucher)
    x += n
    usleep(1000)
  }

  func add<T: Sequence>(sequence: T, _ voucher: voucher_t?) async -> Int where T.Element == Int {
    for n in sequence {
      await add(n, voucher)
    }
    return x
  }

  func get() -> Int { x }
}

actor Combiner {
  var accumulators: [Accumulator] = []

  func add(accumulator: Accumulator) {
    accumulators.append(accumulator)
  }

  func sum() async -> Int {
    var sum = 0
    for a in accumulators {
      fputs("Accumulating from \(ptrstr(a))\n", stderr)
      sum += await a.get()
    }
    return sum
  }
}

actor Counter {
  var n = 0

  func increment() {
    n += 1
  }

  func get() -> Int { n }
}

actor ActorWithSelfIsolatedDeinit {
  let expectedVoucher: voucher_t?
  let group: DispatchGroup
  
  init(expectedVoucher: voucher_t?, group: DispatchGroup) {
    self.expectedVoucher = expectedVoucher
    self.group = group
  }
  
  isolated deinit {
    expectTrue(isCurrentExecutor(self.unownedExecutor))
    let currentVoucher = voucher_copy()
    expectEqual(expectedVoucher, currentVoucher)
    os_release(currentVoucher)
    group.leave()
  }
}

@globalActor actor AnotherActor: GlobalActor {
  static let shared = AnotherActor()
  
  func performTesting(_ work: @Sendable () -> Void) {
    work()
  }
}

actor ActorWithDeinitIsolatedOnAnother {
  let expectedVoucher: voucher_t?
  let group: DispatchGroup
  
  init(expectedVoucher: voucher_t?, group: DispatchGroup) {
    self.expectedVoucher = expectedVoucher
    self.group = group
  }
  
  @AnotherActor
  deinit {
    expectTrue(isCurrentExecutor(AnotherActor.shared.unownedExecutor))
    let currentVoucher = voucher_copy()
    expectEqual(expectedVoucher, currentVoucher)
    os_release(currentVoucher)
    group.leave()
  }
}

class ClassWithIsolatedDeinit {
  let expectedVoucher: voucher_t?
  let group: DispatchGroup
  
  init(expectedVoucher: voucher_t?, group: DispatchGroup) {
    self.expectedVoucher = expectedVoucher
    self.group = group
  }
  
  @AnotherActor
  deinit {
    expectTrue(isCurrentExecutor(AnotherActor.shared.unownedExecutor))
    let currentVoucher = voucher_copy()
    expectEqual(expectedVoucher, currentVoucher)
    os_release(currentVoucher)
    group.leave()
  }
}

// Make a nice string for a pointer, like what %p would produce in printf.
func ptrstr<T>(_ ptr: T) -> String {
  "0x" + String(unsafeBitCast(ptr, to: UInt.self), radix: 16)
}

// Print a voucher to stderr with an optional label.
func print(_ s: String = "", voucher: voucher_t?) {
  fputs("\(s) - \(ptrstr(voucher))\n", stderr)
}

// Look up a symbol using dlsym and cast the result to an arbitrary type.
func lookup<T>(_ name: String) -> T {
  let RTLD_DEFAULT = UnsafeMutableRawPointer(bitPattern: -2)
  let result = dlsym(RTLD_DEFAULT, name)
  return unsafeBitCast(result, to: T.self)
}

// We'll use os_activity calls to make our test vouchers. The calls aren't in
// the headers so we need to look them up dynamically.
let _os_activity_create = lookup("_os_activity_create")
    as @convention(c) (UnsafeRawPointer, UnsafePointer<CChar>, UnsafeRawPointer,
                       UInt32) -> voucher_t?
let OS_ACTIVITY_NONE = lookup("_os_activity_none") as UnsafeRawPointer
let OS_ACTIVITY_FLAG_DETACHED = 1 as UInt32

// Look up the voucher calls we'll be using. Vouchers are ObjC objects, but we
// want total control over their memory management, so we'll treat them as raw
// pointers instead, and manually manage their memory.
typealias voucher_t = UnsafeMutableRawPointer
let voucher_copy = lookup("voucher_copy") as @convention(c) () -> voucher_t?
let voucher_adopt = lookup("voucher_adopt") as @convention(c) (voucher_t?)
    -> voucher_t?
let os_retain = lookup("os_retain") as @convention(c) (voucher_t?) -> voucher_t?
let os_release = lookup("os_release") as @convention(c) (voucher_t?) -> Void

let isCurrentExecutor = lookup("swift_task_isCurrentExecutor") as @convention(thin) (UnownedSerialExecutor) -> Bool

// Run some async code with test vouchers. Wait for the async code to complete,
// then verify that the vouchers aren't leaked.
func withVouchers(call: @Sendable @escaping (voucher_t?, voucher_t?, voucher_t?)
    async -> Void) {
  // We'll store weak pointers to the vouchers to verify that they don't leak.
  // If the voucher was deallocated after we're done, then the weak references
  // will be nil. We can't use Swift `weak` references since we're treating
  // vouchers as raw pointers. We'll use the ObjC runtime APIs directly. Those
  // require a stable address for the storage, so we'll allocate that manually.
  let weakPtrsAllocation = UnsafeMutablePointer<AnyObject?>.allocate(capacity: 3)
  weakPtrsAllocation.initialize(repeating: nil, count: 3)
  let weakPtrs = AutoreleasingUnsafeMutablePointer<AnyObject?>(weakPtrsAllocation)

  // Helper function to store three vouchers into the weak storage.
  func storeWeak(vouchers: [voucher_t?]) {
    for (i, voucher) in vouchers.enumerated() {
      objc_storeWeak(weakPtrs + i, unsafeBitCast(voucher, to: AnyObject?.self))
    }
  }

  // Make convenient pointers to the individual weak variables.
  let weak1 = AutoreleasingUnsafeMutablePointer<AnyObject?>(weakPtrs + 0)
  let weak2 = AutoreleasingUnsafeMutablePointer<AnyObject?>(weakPtrs + 1)
  let weak3 = AutoreleasingUnsafeMutablePointer<AnyObject?>(weakPtrs + 2)
  do {
    let v1 = _os_activity_create(#dsohandle, "Swift Test Voucher 1", OS_ACTIVITY_NONE, OS_ACTIVITY_FLAG_DETACHED)
    let v2 = _os_activity_create(#dsohandle, "Swift Test Voucher 2", OS_ACTIVITY_NONE, OS_ACTIVITY_FLAG_DETACHED)
    let v3 = _os_activity_create(#dsohandle, "Swift Test Voucher 3", OS_ACTIVITY_NONE, OS_ACTIVITY_FLAG_DETACHED)

    print("Created v1", voucher: v1)
    print("Created v2", voucher: v2)
    print("Created v3", voucher: v3)

    // Place the vouchers in the weak variables and verify that it worked.
    storeWeak(vouchers: [v1, v2, v3])
    autoreleasepool {
      expectNotNil(objc_loadWeak(weak1))
      expectNotNil(objc_loadWeak(weak2))
      expectNotNil(objc_loadWeak(weak3))
    }

    // Start the async call in the background, waiting for it to complete.
    let group = DispatchGroup()
    group.enter()
    Task {
      await call(v1, v2, v3)

      // Clear any voucher that the call adopted.
      adopt(voucher: nil)
      group.leave() // expected-complete-tns-warning {{capture of 'group' with non-sendable type 'DispatchGroup' in a `@Sendable` closure}}
    }
    group.wait()

    // Release what should be the last reference to the vouchers.
    os_release(v1)
    os_release(v2)
    os_release(v3)
  }
  func now() -> UInt64 {
    return clock_gettime_nsec_np(CLOCK_UPTIME_RAW)
  }
  // The vouchers may take a moment to be destroyed, as background threads
  // finish what they're doing. Wait for the voucher weak references to become
  // nil. We'll give them ten seconds, and otherwise assume they've leaked.
  let start = now();
  var allNil = false;
  while !allNil && now() - start < 10_000_000_000 {
    autoreleasepool {
      allNil = objc_loadWeak(weak1) == nil
            && objc_loadWeak(weak2) == nil
            && objc_loadWeak(weak3) == nil
    }
  }

  // If any weak reference contains non-nil at this point, a voucher has leaked.
  expectNil(objc_loadWeak(weak1))
  expectNil(objc_loadWeak(weak2))
  expectNil(objc_loadWeak(weak3))

  // Clean up the weak references.
  storeWeak(vouchers: [nil, nil, nil])
  weakPtrsAllocation.deallocate()
}

// Adopt the given voucher on the current thread. This takes the voucher at +0
// and handles the memory management around voucher_adopt.
func adopt(voucher: voucher_t?) {
  os_release(voucher_adopt(os_retain(voucher)))
}

let tests = TestSuite("Voucher Propagation")

if #available(SwiftStdlib 5.1, *) {
  tests.test("simple voucher propagation") {
    withVouchers { v1, v2, v3 in
      let a = Accumulator(label: "a: ")
      adopt(voucher: v1)
      await a.add(42, v1)
    }
  }

  tests.test("voucher propagation with a simple async let") {
    withVouchers { v1, v2, v3 in
      let a1 = Accumulator(label: "a1: ")
      let a2 = Accumulator(label: "a2: ")

      adopt(voucher: v1)
      async let r1: () = a1.add(42, v1)
      adopt(voucher: v2)
      async let r2: () = a2.add(42, v2)
      _ = await (r1, r2)
    }
  }

  tests.test("Task {} voucher propagation") {
    withVouchers { v1, v2, v3 in
      let a = Accumulator(label: "a: ")
      adopt(voucher: v1)

      let group = DispatchGroup()
      group.enter()
      Task {
        await a.add(42, v1)
        group.leave()
      }
      group.wait()
    }
  }

  tests.test("Task.detached {} voucher propagation") {
    withVouchers { v1, v2, v3 in
      let a = Accumulator(label: "a: ")
      adopt(voucher: v1)

      let group = DispatchGroup()
      group.enter()
      Task.detached {
        // Task.detached should NOT propagate vouchers, so tell add to look for
        // nil.
        await a.add(42, nil)
        group.leave()
      }
      group.wait()
    }
  }

  tests.test("complex voucher propagation") {
    withVouchers { v1, v2, v3 in
      fputs("Hello, whirled.\n", stderr)

      let a1 = Accumulator(label: "a1: ")
      let a2 = Accumulator(label: "a2: ")

      adopt(voucher: v1)

      @Sendable
      func go(_ log: String, _ v: voucher_t?, _ a: Accumulator, _ n: Int) async -> Int {
        print("Starting \(log): ", voucher: v)
        return await a.add(sequence: (n*10)..<(n*10+10), v)
      }

      adopt(voucher: v1)
      async let r1 = go("1 a1", v1, a1, 1)
      adopt(voucher: v2)
      async let r2 = go("2 a2", v2, a2, 2)

      async let r3 = go("3 a1", v2, a1, 3)
      adopt(voucher: v1)
      async let r4 = go("4 a2", v1, a2, 4)

      adopt(voucher: v1)
      async let r5 = go("5 a1", v1, a1, 5)
      async let r6 = go("6 a2", v1, a2, 6)

      adopt(voucher: v2)
      async let r7 = go("7 a1", v2, a1, 7)
      async let r8 = go("8 a2", v2, a2, 8)

      adopt(voucher: v3)
      async let r9 = go("9 a1", v3, a1, 9)
      async let r10 = go("10 a2", v3, a2, 10)
      fputs("async let results: \((await r1, await r2, await r3, await r4, await r5, await r6, await r7, await r8, await r9, await r10))\n", stderr)

      fputs("\(await a2.get())\n", stderr)

      let combiner = Combiner()
      async let add1: () = await combiner.add(accumulator: a1)
      async let add2: () = await combiner.add(accumulator: a2)
      _ = await (add1, add2)
      fputs("combiner 1 - \(await combiner.sum())\n", stderr)
      async let add3: () = await combiner.add(accumulator: a1)
      async let add4: () = await combiner.add(accumulator: a2)
      _ = await (add3, add4)
      fputs("combiner 2 - \(await combiner.sum())\n", stderr)
      for n in 0..<10 {
        async let a = combiner.sum()
        async let b = combiner.sum()
        fputs("combiner loop \(n) - \(await a + b)\n", stderr)
      }
    }
  }

  tests.test("voucher propagation with mixed concurrency/dispatch code") {
    withVouchers {v1, v2, v3 in
     let a = Accumulator(label: "a: ")
     let group = DispatchGroup()
     let n = Counter()
     let limit = 100
     group.enter()
     @Sendable func detachedTask() async {
       let voucher = [v1, v2, v3][await n.get() % 3]
       adopt(voucher: voucher)
       let currentVoucher = voucher_copy()
       expectEqual(voucher, currentVoucher)
       os_release(currentVoucher)
       DispatchQueue.global().async {
         let currentVoucher = voucher_copy()
         expectEqual(voucher, currentVoucher)
         os_release(currentVoucher)

         Task {
           let currentVoucher = voucher_copy()
           expectEqual(voucher, currentVoucher)
           os_release(currentVoucher)

           async let g: () = withTaskGroup(of: Void.self, body: { group in
             for _ in 0..<10 {
               group.async {
                 let currentVoucher = voucher_copy()
                 expectEqual(voucher, currentVoucher)
                 os_release(currentVoucher)
               }
             }
           })
           async let add: () = a.add(42, voucher)
           _ = await (g, add)

           if await n.get() >= limit {
             group.leave() // expected-warning 2{{capture of 'group' with non-sendable type 'DispatchGroup' in a `@Sendable` closure}}
           } else {
             await n.increment()
             await detachedTask()
           }
         }
       }
     }
     await detachedTask()
     group.wait()
   }
  }
  
  tests.test("voucher propagation in isolated deinit [fast path]") {
    withVouchers { v1, v2, v3 in
      let group = DispatchGroup()
      group.enter()
      group.enter()
      group.enter()
      Task {
        await AnotherActor.shared.performTesting {
          adopt(voucher: v1)
          _ = ClassWithIsolatedDeinit(expectedVoucher: v1, group: group)
        }
        await AnotherActor.shared.performTesting {
          adopt(voucher: v2)
          _ = ActorWithSelfIsolatedDeinit(expectedVoucher: v2, group: group)
        }
        await AnotherActor.shared.performTesting {
          adopt(voucher: v3)
          _ = ActorWithDeinitIsolatedOnAnother(expectedVoucher: v3, group: group)
        }
      }
      group.wait()
    }
  }
  
  tests.test("voucher propagation in isolated deinit [slow path]") {
    withVouchers { v1, v2, v3 in
      let group = DispatchGroup()
      group.enter()
      group.enter()
      Task {
        do {
          adopt(voucher: v1)
          _ = ActorWithDeinitIsolatedOnAnother(expectedVoucher: v1, group: group)
        }
        do {
          adopt(voucher: v2)
          _ = ClassWithIsolatedDeinit(expectedVoucher: v2, group: group)
        }
      }
      group.wait()
    }
  }
}

runAllTests()
