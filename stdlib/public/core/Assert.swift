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

/// Performs a traditional C-style assert with an optional message.
///
/// Use this function for internal consistency checks that are active during testing
/// but do not impact performance of shipping code. To check for invalid usage
/// in Release builds, see `precondition(_:_:file:line:)`.
///
/// * In playgrounds and `-Onone` builds (the default for Xcode's Debug
///   configuration): If `condition` evaluates to `false`, stop program
///   execution in a debuggable state after printing `message`.
///
/// * In `-O` builds (the default for Xcode's Release configuration),
///   `condition` is not evaluated, and there are no effects.
///
/// * In `-Ounchecked` builds, `condition` is not evaluated, but the optimizer
///   may assume that it *always* evaluates to `true`. Failure to satisfy that
///   assumption is a serious programming error.
///
/// - Parameters:
///   - condition: The condition to test. `condition` is only evaluated in
///     playgrounds and `-Onone` builds.
///   - message: A string to print if `condition` is evaluated to `false`. The
///     default is an empty string.
///   - file: The file name to print with `message` if the assertion fails. The
///     default is the file where `assert(_:_:file:line:)` is called.
///   - line: The line number to print along with `message` if the assertion
///     fails. The default is the line number where `assert(_:_:file:line:)`
///     is called.
@_transparent
@_unavailableInEmbedded
public func assert(
  _ condition: @autoclosure () -> Bool,
  _ message: @autoclosure () -> String = String(),
  file: StaticString = #file, line: UInt = #line
) {
  // Only assert in debug mode.
  guard _isDebugAssertConfiguration() else {
    return
  }

  guard !_fastPath(condition()) else {
    return
  }

  _assertionFailure("Assertion failed", message(), file: file, line: line, flags: _fatalErrorFlags())
}

#if $Embedded
@_transparent
public func assert(
  _ condition: @autoclosure () -> Bool,
  _ message: @autoclosure () -> StaticString = StaticString(),
  file: StaticString = #file, line: UInt = #line
) {
  // Only assert in debug mode.
  guard _isDebugAssertConfiguration() else {
    return
  }
  
  guard !_fastPath(condition()) else {
    return
  }

  _assertionFailure("Assertion failed", message(), file: file, line: line, flags: _fatalErrorFlags())
}
#endif

/// Checks a necessary condition for making forward progress.
///
/// Use this function to detect conditions that must prevent the program from
/// proceeding, even in shipping code.
///
/// * In playgrounds and `-Onone` builds (the default for Xcode's Debug
///   configuration): If `condition` evaluates to `false`, stop program
///   execution in a debuggable state after printing `message`.
///
/// * In `-O` builds (the default for Xcode's Release configuration): If
///   `condition` evaluates to `false`, stop program execution.
///
/// * In `-Ounchecked` builds, `condition` is not evaluated, but the optimizer
///   may assume that it *always* evaluates to `true`. Failure to satisfy that
///   assumption is a serious programming error.
///
/// - Parameters:
///   - condition: The condition to test. `condition` is not evaluated in
///     `-Ounchecked` builds.
///   - message: A string to print if `condition` is evaluated to `false` in a
///     playground or `-Onone` build. The default is an empty string.
///   - file: The file name to print with `message` if the precondition fails.
///     The default is the file where `precondition(_:_:file:line:)` is
///     called.
///   - line: The line number to print along with `message` if the assertion
///     fails. The default is the line number where
///     `precondition(_:_:file:line:)` is called.
@_transparent
@_unavailableInEmbedded
public func precondition(
  _ condition: @autoclosure () -> Bool,
  _ message: @autoclosure () -> String = String(),
  file: StaticString = #file, line: UInt = #line
) {
  // Only assert in debug mode. In release mode just trap.
  guard _isDebugAssertConfiguration() else {
    if _isReleaseAssertConfiguration() {
      let error = !condition()
      Builtin.condfail_message(error._value, StaticString("precondition failure").unsafeRawPointer)
    }
    return
  }

  guard !_fastPath(condition()) else {
    return
  }
  
  _assertionFailure("Precondition failed", message(), file: file, line: line, flags: _fatalErrorFlags())
}

#if $Embedded
@_transparent
public func precondition(
  _ condition: @autoclosure () -> Bool,
  _ message: @autoclosure () -> StaticString = StaticString(),
  file: StaticString = #file, line: UInt = #line
) {
  // Only check in debug and release mode. In release mode just trap.
  guard _isDebugAssertConfiguration() else {
    if _isReleaseAssertConfiguration() {
      let error = !condition()
      Builtin.condfail_message(error._value, StaticString("precondition failure").unsafeRawPointer)
    }
    return
  }
  
  guard !_fastPath(condition()) else {
    return
  }
  
  _assertionFailure("Precondition failed", message(), file: file, line: line, flags: _fatalErrorFlags())
}
#endif

/// Indicates that an internal consistency check failed.
///
/// This function's effect varies depending on the build flag used:
///
/// * In playgrounds and `-Onone` builds (the default for Xcode's Debug
///   configuration), stop program execution in a debuggable state after
///   printing `message`.
///
/// * In `-O` builds, has no effect.
///
/// * In `-Ounchecked` builds, the optimizer may assume that this function is
///   never called. Failure to satisfy that assumption is a serious
///   programming error.
///
/// - Parameters:
///   - message: A string to print in a playground or `-Onone` build. The
///     default is an empty string.
///   - file: The file name to print with `message`. The default is the file
///     where `assertionFailure(_:file:line:)` is called.
///   - line: The line number to print along with `message`. The default is the
///     line number where `assertionFailure(_:file:line:)` is called.
@inlinable
@inline(__always)
@_unavailableInEmbedded
public func assertionFailure(
  _ message: @autoclosure () -> String = String(),
  file: StaticString = #file, line: UInt = #line
) {
  guard _isDebugAssertConfiguration() else {
    if _isFastAssertConfiguration() {
      _conditionallyUnreachable()
    }
    return
  }

  _assertionFailure("Fatal error", message(), file: file, line: line, flags: _fatalErrorFlags())
}

#if $Embedded
@inlinable
@inline(__always)
public func assertionFailure(
  _ message: @autoclosure () -> StaticString = StaticString(),
  file: StaticString = #file, line: UInt = #line
) {
  guard _isDebugAssertConfiguration() else {
    if _isFastAssertConfiguration() {
      _conditionallyUnreachable()
    }
    return
  }

  _assertionFailure("Fatal error", message(), file: file, line: line, flags: _fatalErrorFlags())
}
#endif

/// Indicates that a precondition was violated.
///
/// Use this function to stop the program when control flow can only reach the
/// call if your API was improperly used and execution flow is not expected to
/// reach the call---for example, in the `default` case of a `switch` where
/// you have knowledge that one of the other cases must be satisfied.
///
/// This function's effect varies depending on the build flag used:
///
/// * In playgrounds and `-Onone` builds (the default for Xcode's Debug
///   configuration), stops program execution in a debuggable state after
///   printing `message`.
///
/// * In `-O` builds (the default for Xcode's Release configuration), stops
///   program execution.
///
/// * In `-Ounchecked` builds, the optimizer may assume that this function is
///   never called. Failure to satisfy that assumption is a serious
///   programming error.
///
/// - Parameters:
///   - message: A string to print in a playground or `-Onone` build. The
///     default is an empty string.
///   - file: The file name to print with `message`. The default is the file
///     where `preconditionFailure(_:file:line:)` is called.
///   - line: The line number to print along with `message`. The default is the
///     line number where `preconditionFailure(_:file:line:)` is called.
@_transparent
@_unavailableInEmbedded
public func preconditionFailure(
  _ message: @autoclosure () -> String = String(),
  file: StaticString = #file, line: UInt = #line
) -> Never {
  // Only check in debug and release mode.  In release mode just trap.
  guard _isDebugAssertConfiguration() else {
    if _isReleaseAssertConfiguration() {
      Builtin.condfail_message(true._value, StaticString("precondition failure").unsafeRawPointer)
    }
    return _conditionallyUnreachable()
  }

  _assertionFailure("Fatal error", message(), file: file, line: line, flags: _fatalErrorFlags())

  return _conditionallyUnreachable()
}

#if $Embedded
@_transparent
public func preconditionFailure(
  _ message: @autoclosure () -> StaticString = StaticString(),
  file: StaticString = #file, line: UInt = #line
) -> Never {
  defer { _conditionallyUnreachable() }

  // Only check in debug and release mode.  In release mode just trap.
  if _isDebugAssertConfiguration() {
    _assertionFailure("Fatal error", message(), file: file, line: line, flags: _fatalErrorFlags())
  } else if _isReleaseAssertConfiguration() {
    Builtin.condfail_message(true._value, StaticString("precondition failure").unsafeRawPointer)
  }
}
#endif

/// Unconditionally prints a given message and stops execution.
///
/// - Parameters:
///   - message: The string to print. The default is an empty string.
///   - file: The file name to print with `message`. The default is the file
///     where `fatalError(_:file:line:)` is called.
///   - line: The line number to print along with `message`. The default is the
///     line number where `fatalError(_:file:line:)` is called.
@_transparent
@_unavailableInEmbedded
public func fatalError(
  _ message: @autoclosure () -> String = String(),
  file: StaticString = #file, line: UInt = #line
) -> Never {
  _assertionFailure("Fatal error", message(), file: file, line: line,
    flags: _fatalErrorFlags())
}

#if $Embedded
@_transparent
public func fatalError(
  _ message: @autoclosure () -> StaticString = StaticString(),
  file: StaticString = #file, line: UInt = #line
) -> Never {
  _assertionFailure("Fatal error", message(), file: file, line: line,
    flags: _fatalErrorFlags())
}
#endif

/// Library precondition checks.
///
/// Library precondition checks are enabled in debug mode and release mode. When
/// building in fast mode they are disabled.  In release mode they don't print
/// an error message but just trap. In debug mode they print an error message
/// and abort.
@usableFromInline @_transparent
internal func _precondition(
  _ condition: @autoclosure () -> Bool, _ message: StaticString = StaticString(),
  file: StaticString = #file, line: UInt = #line
) {
  // Only check in debug and release mode. In release mode just trap.
  guard _isDebugAssertConfiguration() else {
    if _isReleaseAssertConfiguration()  {
      let error = !condition()
      Builtin.condfail_message(error._value, message.unsafeRawPointer)
    }
    return
  }

  guard !_fastPath(condition()) else {
    return
  }

  _assertionFailure("Fatal error", message, file: file, line: line, flags: _fatalErrorFlags())
}

@usableFromInline @_transparent
internal func _preconditionFailure(
  _ message: StaticString = StaticString(),
  file: StaticString = #file, line: UInt = #line
) -> Never {
  _precondition(false, message, file: file, line: line)
  _conditionallyUnreachable()
}

/// If `error` is true, prints an error message in debug mode, traps in release
/// mode, and returns an undefined error otherwise.
/// Otherwise returns `result`.
@_transparent
public func _overflowChecked<T>(
  _ args: (T, Bool),
  file: StaticString = #file, line: UInt = #line
) -> T {
  let (result, error) = args

  guard _isDebugAssertConfiguration() else {
    Builtin.condfail_message(error._value, StaticString("_overflowChecked failure").unsafeRawPointer)
    return
  }

  guard _slowPath(error) else {
    return
  }
  
  _fatalErrorMessage("Fatal error", "Overflow/underflow", file: file, line: line, flags: _fatalErrorFlags())

  return result
}


/// Debug library precondition checks.
///
/// Debug library precondition checks are only on in debug mode. In release and
/// in fast mode they are disabled. In debug mode they print an error message
/// and abort.
/// They are meant to be used when the check is not comprehensively checking for
/// all possible errors.
@usableFromInline @_transparent
internal func _debugPrecondition(
  _ condition: @autoclosure () -> Bool, _ message: StaticString = StaticString(),
  file: StaticString = #file, line: UInt = #line
) {
#if SWIFT_STDLIB_ENABLE_DEBUG_PRECONDITIONS_IN_RELEASE
  _precondition(condition(), message, file: file, line: line)
#else
  // Only check in debug mode.
  guard _slowPath(_isDebugAssertConfiguration()) else {
    return
  }

  guard !_fastPath(condition()) else {
    return
  }
  
  _fatalErrorMessage("Fatal error", message, file: file, line: line, flags: _fatalErrorFlags())
#endif
}

@usableFromInline @_transparent
internal func _debugPreconditionFailure(
  _ message: StaticString = StaticString(),
  file: StaticString = #file, line: UInt = #line
) -> Never {
#if SWIFT_STDLIB_ENABLE_DEBUG_PRECONDITIONS_IN_RELEASE
  _preconditionFailure(message, file: file, line: line)
#else
  guard _slowPath(_isDebugAssertConfiguration()) else {
    _conditionallyUnreachable()
    return
  }

  _precondition(false, message, file: file, line: line)
#endif
}

/// Internal checks.
///
/// Internal checks are to be used for checking correctness conditions in the
/// standard library. They are only enable when the standard library is built
/// with the build configuration INTERNAL_CHECKS_ENABLED enabled. Otherwise, the
/// call to this function is a noop.
@usableFromInline @_transparent
internal func _internalInvariant(
  _ condition: @autoclosure () -> Bool, _ message: StaticString = StaticString(),
  file: StaticString = #file, line: UInt = #line
) {
#if INTERNAL_CHECKS_ENABLED
  guard !_fastPath(condition()) else {
    return
  }

  _fatalErrorMessage("Fatal error", message, file: file, line: line, flags: _fatalErrorFlags())
#endif
}

// Only perform the invariant check on Swift 5.1 and later
@_alwaysEmitIntoClient // Swift 5.1
@_transparent
internal func _internalInvariant_5_1(
  _ condition: @autoclosure () -> Bool, _ message: StaticString = StaticString(),
  file: StaticString = #file, line: UInt = #line
) {
#if INTERNAL_CHECKS_ENABLED
  // FIXME: The below won't run the assert on 5.1 stdlib if testing on older
  // OSes, which means that testing may not test the assertion. We need a real
  // solution to this.
  guard #available(macOS 10.15, iOS 13, tvOS 13, watchOS 6, *) //SwiftStdlib 5.1
  else { return }
  _internalInvariant(condition(), message, file: file, line: line)
#endif
}

/// Library precondition checks with a linked-on-or-after check, allowing the
/// addition of new preconditions while maintaining compatibility with older
/// binaries.
///
/// This version of `_precondition` only traps if the condition returns false
/// **and** the current executable was built with a Swift Standard Library
/// version equal to or greater than the supplied version.
@_transparent
@_unavailableInEmbedded
internal func _precondition(
  ifLinkedOnOrAfter version: _SwiftStdlibVersion,
  _ condition: @autoclosure () -> Bool,
  _ message: StaticString = StaticString(),
  file: StaticString = #file, line: UInt = #line
) {
  // Delay the linked-on-or-after check until after we know we have a failed
  // condition, so that we don't slow down the usual case too much.

  // Note: this is an internal function, so `_isDebugAssertConfiguration` is
  // expected to evaluate (at compile time) to true in production builds of the
  // stdlib. The other branches are kept in case the stdlib is built with an
  // unusual configuration.
  guard _isDebugAssertConfiguration() else {
    if _isReleaseAssertConfiguration() {
      let error = (!condition() && _isExecutableLinkedOnOrAfter(version))
      Builtin.condfail_message(error._value, message.unsafeRawPointer)
    }
    return
  }

  guard _slowPath(!condition()) else {
    return
  }
  
  guard _isExecutableLinkedOnOrAfter(version) else { return }
  _assertionFailure("Fatal error", message, file: file, line: line, flags: _fatalErrorFlags())
}


@usableFromInline @_transparent
internal func _internalInvariantFailure(
  _ message: StaticString = StaticString(),
  file: StaticString = #file, line: UInt = #line
) -> Never {
  _internalInvariant(false, message, file: file, line: line)
  _conditionallyUnreachable()
}
