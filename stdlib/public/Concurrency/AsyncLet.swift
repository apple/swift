//===----------------------------------------------------------------------===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2020 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//

import Swift
@_implementationOnly import _SwiftConcurrencyShims

// ==== Async Let -------------------------------------------------------------
// Only has internal / builtin functions as it is not really accessible directly

@available(SwiftStdlib 5.5, *)
@_silgen_name("swift_asyncLet_start")
public func _asyncLetStart<T>(
  asyncLet: Builtin.RawPointer,
  options: Builtin.RawPointer?,
  operation: @Sendable () async throws -> T
)

/// Similar to _taskFutureGet but for AsyncLet
@available(SwiftStdlib 5.5, *)
@_silgen_name("swift_asyncLet_wait_with_options")
public func _asyncLetGet<T>(
  asyncLet: Builtin.RawPointer,
  options:  Builtin.RawPointer?
) async -> T

///// Similar to _taskFutureGetThrowing but for AsyncLet
@available(SwiftStdlib 5.5, *)
@_silgen_name("swift_asyncLet_wait_throwing_with_options")
public func _asyncLetGetThrowing<T>(
  asyncLet: Builtin.RawPointer,
  options:  Builtin.RawPointer?
) async throws -> T

@available(SwiftStdlib 5.5, *)
@_silgen_name("swift_asyncLet_end_with_options")
public func _asyncLetEnd(
  asyncLet: Builtin.RawPointer, // TODO: should this take __owned?
  options:  Builtin.RawPointer?
)

@_silgen_name("swift_asyncLet_extractTask")
func _asyncLetExtractTask(
  of asyncLet: Builtin.RawPointer
) -> Builtin.NativeObject
