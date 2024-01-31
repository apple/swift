//===----------------------------------------------------------------------===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2017 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//

import SwiftShims

#if SWIFT_STDLIB_HAS_COMMANDLINE

@_silgen_name("_swift_stdlib_getUnsafeArgvArgc")
internal func _swift_stdlib_getUnsafeArgvArgc(_: UnsafeMutablePointer<Int32>)
  -> UnsafeMutablePointer<UnsafeMutablePointer<Int8>?>

/// Command-line arguments for the current process.
@frozen // namespace
public enum CommandLine {
  /// The backing static variable for argument count may come either from the
  /// entry point or it may need to be computed e.g. if we're in the REPL.
  ///
  /// Note: It is an abomination that this is marked nonisolated unsafe.
  /// In theory, this variable is only set once and threads attempting to race
  /// to set it will all update it to the same value. We hope there are no
  /// tears in the write.
  @usableFromInline
  internal nonisolated(unsafe) static var _argc: Int32 = 0

  /// The backing static variable for arguments may come either from the
  /// entry point or it may need to be computed e.g. if we're in the REPL.
  ///
  /// Care must be taken to ensure that `_swift_stdlib_getUnsafeArgvArgc` is
  /// not invoked more times than is necessary (at most once).
  ///
  /// Note: It is an abomination that this is marked nonisolated unsafe.
  /// In theory, this variable is only set once and threads attempting to race
  /// to set it will all update it to the same value. We hope there are no
  /// tears in the write.
  @usableFromInline
  internal nonisolated(unsafe) static var _unsafeArgv:
    UnsafeMutablePointer<UnsafeMutablePointer<Int8>?>
      =  _swift_stdlib_getUnsafeArgvArgc(&_argc)

  /// Access to the raw argc value from C.
  public static var argc: Int32 {
    _ = CommandLine.unsafeArgv // Force evaluation of argv.
    return _argc
  }

  /// Access to the raw argv value from C.
  ///
  /// The value of this property is a `nil`-terminated C array. Including the
  /// trailing `nil`, there are ``argc`` `+ 1` elements in the array.
  ///
  /// - Note: Accessing the argument vector through this pointer is unsafe.
  ///   Where possible, use ``arguments`` instead.
  public static var unsafeArgv:
    UnsafeMutablePointer<UnsafeMutablePointer<Int8>?> {
    return _unsafeArgv
  }

  /// Access to the Swift command line arguments.
  // Use lazy initialization of static properties to 
  // safely initialize the swift arguments.
  public static var arguments: [String]
    = (0..<Int(argc)).map { String(cString: _unsafeArgv[$0]!) }
}

#endif // SWIFT_STDLIB_HAS_COMMANDLINE
