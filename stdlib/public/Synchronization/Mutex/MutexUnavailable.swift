//===----------------------------------------------------------------------===//
//
// This source file is part of the Swift Atomics open source project
//
// Copyright (c) 2024 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//

/// A synchronization primitive that protects shared mutable state via
/// mutual exclusion.
///
/// The `Mutex` type offers non-recursive exclusive access to the state
/// it is protecting by blocking threads attempting to acquire the lock.
/// Only one execution context at a time has access to the value stored
/// within the `Mutex` allowing for exclusive access.
///
/// An example use of `Mutex` in a class used simultaneously by many
/// threads protecting a `Dictionary` value:
///
///     class Manager {
///       let cache = Mutex<[Key: Resource]>([:])
///
///       func saveResouce(_ resource: Resouce, as key: Key) {
///         cache.withLock {
///           $0[key] = resource
///         }
///       }
///     }
///
@available(*, unavailable, message: "Mutex is not available on this platform")
@frozen
@_staticExclusiveOnly
public struct Mutex<Value: ~Copyable>: ~Copyable {}
