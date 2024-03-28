//===--- Bincompat.h - Binary compatibility checks. -------------*- C++ -*-===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2024 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//
//
// Checks for enabling binary compatibility workarounds.
//
//===----------------------------------------------------------------------===//

namespace swift {

namespace runtime {

namespace bincompat {

/// Whether protocol conformance iteration should be reversed, to prefer
/// conformances from images that are later in the list over earlier ones.
/// Default is false starting with Swift 5.4.
bool useLegacyProtocolConformanceReverseIteration();

/// Whether we should crash when we encounter a non-nullable Obj-C
/// reference with a null value as the source of a cast.
/// Default is true starting with Swift 5.4.
bool useLegacyPermissiveObjCNullSemanticsInCasting();

/// Whether we should use the legacy semantics for casting nil optionals
/// to nested optionals
bool useLegacyOptionalNilInjectionInCasting();

/// Whether to use legacy semantics when boxing Swift values for
/// Obj-C interop
bool useLegacyObjCBoxingInCasting();

/// Whether to use legacy semantics when unboxing __SwiftValue
bool useLegacySwiftValueUnboxingInCasting();

/// Legacy semantics use trivial implementations for -hashValue/-isEqual:
/// requests from ObjC to Swift values.
/// New semantics attempt to dispatch to Swift Hashable/Equatable conformances
/// if present.
bool useLegacySwiftObjCHashing();

/// Legacy semantics allowed for the `swift_task_reportUnexpectedExecutor` to
/// only log a warning. This changes in future releases and this function
/// will fatal error always.
///
/// Similarly, the internal runtime function
/// `swift_task_isCurrentExecutor(expected)` was previously allowed to return
/// `false`. In future releases it will call into `checkIsolated`, and CRASH
/// when previously it would have returned false.
///
/// Because some applications were running with "isolation warnings" and
/// those call into the `isCurrentExecutor` API and expected warnings to be
/// logged, but they ignored those warnings we cannot make them crashing,
/// and must check if the app was built against a new.
///
/// Old behavior:
/// - `swift_task_isCurrentExecutorImpl` cannot crash and does NOT invoke
///     `SerialExecutor.checkIsolated`
/// - `swift_task_isCurrentExecutorImpl` does not invoke `checkIsolated`
/// - logging a warning on concurrency violation is allowed
///
/// New behavior:
/// - always fatal error in `swift_task_reportUnexpectedExecutor`
/// - `swift_task_isCurrentExecutorImpl` will crash when it would have returned
///     false
/// - `swift_task_isCurrentExecutorImpl` does invoke `checkIsolated` when other
///     checks failed
///
/// This can be overridden by using `SWIFT_UNEXPECTED_EXECUTOR_LOG_LEVEL=1`
/// or `SWIFT_IS_CURRENT_EXECUTOR_LEGACY_MODE_OVERRIDE=crash|nocrash`
SWIFT_RUNTIME_STDLIB_SPI
bool swift_bincompat_useLegacyNonCrashingExecutorChecks();

} // namespace bincompat

} // namespace runtime

} // namespace swift
