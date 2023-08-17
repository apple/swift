// RUN: %empty-directory(%t)
// RUN: %target-build-swift -enable-experimental-feature RawLayout -enable-experimental-feature BuiltinModule %s -o %t/a.out
// RUN: %target-codesign %t/a.out
// RUN: %target-run %t/a.out | %FileCheck %s

// REQUIRES: OS=macosx
// REQUIRES: executable_test

import Builtin
import Darwin

@_rawLayout(like: os_unfair_lock_s)
struct UnfairLock: ~Copyable, @unchecked Sendable {
    // TODO: Clang importer can't handle the OS_UNFAIR_LOCK_INIT macro definition
    private static let OS_UNFAIR_LOCK_INIT = os_unfair_lock_s()

    // The lock is at a stable address as long as someone is borrowing it.
    // If the address is gotten, it should only be used within the scope
    // of a borrowing method.
    @inline(__always)
    private var _address: os_unfair_lock_t {
        os_unfair_lock_t(Builtin.addressOfBorrow(self))
    }

    @inline(__always)
    init() {
        _address.initialize(to: UnfairLock.OS_UNFAIR_LOCK_INIT)
    }

    @inline(__always)
    borrowing func withLock<R>(_ body: () throws -> R) rethrows -> R {
        let address = _address
        os_unfair_lock_lock(address)
        defer { os_unfair_lock_unlock(address) }

        return try body()
    }
}

final class Locked<T>: @unchecked Sendable {
    private let lock = UnfairLock()

    // Don't need exclusivity checking since accesses always go through the
    // lock
    @exclusivity(unchecked)
    private var value: T

    init(initialValue: T) {
        self.value = consume initialValue
    }

    func withLock<R>(_ body: (inout T) throws -> R) rethrows -> R {
        return try lock.withLock { try body(&value) }
    }
}

let myString = Locked(initialValue: "")

@Sendable
func thread1(_: UnsafeMutableRawPointer) -> UnsafeMutableRawPointer? {
    usleep(1)
    myString.withLock { $0 += "apple\n" }
    usleep(1)
    myString.withLock { $0 += "banana\n" }
    usleep(1)
    myString.withLock { $0 += "grapefruit\n" }
    return nil
}

@Sendable
func thread2(_: UnsafeMutableRawPointer) -> UnsafeMutableRawPointer? {
    usleep(1)
    myString.withLock { $0 += "BRUSSELS SPROUTS\n" }
    usleep(1)
    myString.withLock { $0 += "CAULIFLOWER\n" }
    usleep(1)
    myString.withLock { $0 += "BROCCOLI\n" }
    return nil
}

@Sendable
func thread3(_: UnsafeMutableRawPointer) -> UnsafeMutableRawPointer? {
    usleep(1)
    myString.withLock { $0 += "Croissant\n" }
    usleep(1)
    myString.withLock { $0 += "Boule\n" }
    usleep(1)
    myString.withLock { $0 += "Batard\n" }
    return nil
}

func createPthread(
    _ body: @Sendable @convention(c) (UnsafeMutableRawPointer) -> UnsafeMutableRawPointer?
) -> pthread_t? {
    var thread: pthread_t? = nil
    let r = pthread_create(&thread, nil, body, nil)
    if r != 0 {
        return nil
    }
    return thread
}

var t1 = createPthread(thread1)!
var t2 = createPthread(thread2)!
var t3 = createPthread(thread3)!

pthread_join(t1, nil)
pthread_join(t2, nil)
pthread_join(t3, nil)

// CHECK-DAG: apple
// CHECK-DAG: banana
// CHECK-DAG: grapefruit
// CHECK-DAG: BRUSSELS SPROUTS
// CHECK-DAG: CAULIFLOWER
// CHECK-DAG: BROCCOLI
// CHECK-DAG: Croissant
// CHECK-DAG: Boule
// CHECK-DAG: Batard
myString.withLock { print($0) }
