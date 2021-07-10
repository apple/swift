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
/// Use this function for internal sanity checks that are active during testing
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
public func assert(
  _ condition: @autoclosure () -> Bool,
  _ message: @autoclosure () -> String = String(),
  file: StaticString = #file, line: UInt = #line
) {
  // Only assert in debug mode.
  if _isDebugAssertConfiguration() {
    if !_fastPath(condition()) {
      _assertionFailure("Assertion failed", message(), file: file, line: line,
        flags: _fatalErrorFlags())
    }
  }
}

/// Checks a necessary condition for making forward progress.
///
/// Use this function to detect conditions that must prevent the program from
/// proceeding, even in shipping code.
///
/// * In playgrounds and `-Onone` builds (the default for Xcode's Debug
///   configuration): If `condition` evaluates to `false`, stop program
///   execution in a debuggable state after printing `message`
///   along with the file and line information..
///
/// * In `-O` builds (the default for Xcode's Release configuration): If
///   `condition` evaluates to `false`, stop program execution state after
///   printing `message`.
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
public func precondition(
  _ condition: @autoclosure () -> Bool,
  _ message: @autoclosure () -> String,
  file: StaticString = #file, line: UInt = #line
) {
  // Only check in debug and release mode. In release mode just trap.
  if _isDebugAssertConfiguration() {
    if !_fastPath(condition()) {
      _assertionFailure("Precondition failed", message(), file: file, line: line,
        flags: _fatalErrorFlags())
    }
  } else if _isReleaseAssertConfiguration() {
    if !_fastPath(condition()) {
      _assertionFailure("Precondition failed", message(),
        flags: _fatalErrorFlags())
    }
  }
}

/// Checks a necessary condition for making forward progress.
///
/// Use this function to detect conditions that must prevent the program from
/// proceeding, even in shipping code, but without a failure message.
///
/// * In playgrounds and `-Onone` builds (the default for Xcode's Debug
///   configuration): If `condition` evaluates to `false`, stop program
///   execution in a debuggable state after printing "Precondition failed",
///   along with the file and line information.
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
///   - file: The file name to print with `message` if the precondition fails.
///     The default is the file where `precondition(_:_:file:line:)` is
///     called.
///   - line: The line number to print along with `message` if the assertion
///     fails. The default is the line number where
///     `precondition(_:_:file:line:)` is called.
@_transparent
public func precondition(
  _ condition: @autoclosure () -> Bool,
  file: StaticString = #file, line: UInt = #line
) {
  // Only check in debug and release mode. In release mode just trap.
  if _isDebugAssertConfiguration() {
    if !_fastPath(condition()) {
      _assertionFailure("Precondition failed", String(), file: file, line: line,
        flags: _fatalErrorFlags())
    }
  } else if _isReleaseAssertConfiguration() {
    let error = !condition()
    Builtin.condfail_message(error._value,
      StaticString("Precondition failed").unsafeRawPointer)
  }
}

/// Indicates that an internal sanity check failed.
///
/// Use this function to stop the program, without impacting the performance of
/// shipping code, when control flow is not expected to reach the call---for
/// example, in the `default` case of a `switch` where you have knowledge that
/// one of the other cases must be satisfied. To protect code from invalid
/// usage in Release builds, see `preconditionFailure(_:file:line:)`.
///
/// * In playgrounds and -Onone builds (the default for Xcode's Debug
///   configuration), stop program execution in a debuggable state after
///   printing `message`.
///
/// * In -O builds, has no effect.
///
/// * In -Ounchecked builds, the optimizer may assume that this function is
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
public func assertionFailure(
  _ message: @autoclosure () -> String = String(),
  file: StaticString = #file, line: UInt = #line
) {
  if _isDebugAssertConfiguration() {
    _assertionFailure("Fatal error", message(), file: file, line: line,
      flags: _fatalErrorFlags())
  }
  else if _isFastAssertConfiguration() {
    _conditionallyUnreachable()
  }
}

/// Indicates that a precondition was violated.
///
/// Use this function to stop the program when control flow can only reach the
/// call if your API was improperly used. This function's effects vary
/// depending on the build flag used:
///
/// * In playgrounds and `-Onone` builds (the default for Xcode's Debug
///   configuration), stops program execution in a debuggable state after
///   printing `message` along with the file and line information.
///
/// * In `-O` builds (the default for Xcode's Release configuration), stops
///   program execution after printing `message`.
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
public func preconditionFailure(
  _ message: @autoclosure () -> String,
  file: StaticString = #file, line: UInt = #line
) -> Never {
  // Only check in debug and release mode.  In release mode just trap.
  if _isDebugAssertConfiguration() {
    _assertionFailure("Fatal error", message(), file: file, line: line,
      flags: _fatalErrorFlags())
  } else if _isReleaseAssertConfiguration() {
    _assertionFailure("Precondition failed", message(),
      flags: _fatalErrorFlags())
  }
  _conditionallyUnreachable()
}

/// Indicates that a precondition was violated, without a message.
///
/// Use this function to stop the program when control flow can only reach the
/// call if your API was improperly used. This function's effects vary
/// depending on the build flag used:
///
/// * In playgrounds and `-Onone` builds (the default for Xcode's Debug
///   configuration), stops program execution in a debuggable state after
///   printing "Fatal error".
///
/// * In `-O` builds (the default for Xcode's Release configuration), stops
///   program execution.
///
/// * In `-Ounchecked` builds, the optimizer may assume that this function is
///   never called. Failure to satisfy that assumption is a serious
///   programming error.
///
/// - Parameters:
///   - file: The file name to print with `message`. The default is the file
///     where `preconditionFailure(file:line:)` is called.
///   - line: The line number to print. The default is the
///     line number where `preconditionFailure(file:line:)` is called.
@_transparent
public func preconditionFailure(
  file: StaticString = #file, line: UInt = #line
) -> Never {
  // Only check in debug and release mode.  In release mode just trap.
  if _isDebugAssertConfiguration() {
    _assertionFailure("Fatal error", String(), file: file, line: line,
      flags: _fatalErrorFlags())
  } else if _isReleaseAssertConfiguration() {
    Builtin.condfail_message(true._value,
      StaticString("Precondition failed").unsafeRawPointer)
  }
  _conditionallyUnreachable()
}

/// Unconditionally prints a given message and stops execution.
///
/// - Parameters:
///   - message: The string to print. The default is an empty string.
///   - file: The file name to print with `message`. The default is the file
///     where `fatalError(_:file:line:)` is called.
///   - line: The line number to print along with `message`. The default is the
///     line number where `fatalError(_:file:line:)` is called.
@_transparent
public func fatalError(
  _ message: @autoclosure () -> String = String(),
  file: StaticString = #file, line: UInt = #line
) -> Never {
  _assertionFailure("Fatal error", message(), file: file, line: line,
    flags: _fatalErrorFlags())
}

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
  if _isDebugAssertConfiguration() {
    if !_fastPath(condition()) {
      _assertionFailure("Fatal error", message, file: file, line: line,
        flags: _fatalErrorFlags())
    }
  } else if _isReleaseAssertConfiguration() {
    let error = !condition()
    Builtin.condfail_message(error._value, message.unsafeRawPointer)
  }
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
  if _isDebugAssertConfiguration() {
    if _slowPath(error) {
      _fatalErrorMessage("Fatal error", "Overflow/underflow",
        file: file, line: line, flags: _fatalErrorFlags())
    }
  } else {
    Builtin.condfail_message(error._value,
      StaticString("_overflowChecked failure").unsafeRawPointer)
  }
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
  // Only check in debug mode.
  if _slowPath(_isDebugAssertConfiguration()) {
    if !_fastPath(condition()) {
      _fatalErrorMessage("Fatal error", message, file: file, line: line,
        flags: _fatalErrorFlags())
    }
  }
}

@usableFromInline @_transparent
internal func _debugPreconditionFailure(
  _ message: StaticString = StaticString(),
  file: StaticString = #file, line: UInt = #line
) -> Never {
  if _slowPath(_isDebugAssertConfiguration()) {
    _precondition(false, message, file: file, line: line)
  }
  _conditionallyUnreachable()
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
  if !_fastPath(condition()) {
    _fatalErrorMessage("Fatal error", message, file: file, line: line,
      flags: _fatalErrorFlags())
  }
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
  guard #available(macOS 10.15, iOS 13, tvOS 13, watchOS 6, *) else { return }
  _internalInvariant(condition(), message, file: file, line: line)
#endif
}

@usableFromInline @_transparent
internal func _internalInvariantFailure(
  _ message: StaticString = StaticString(),
  file: StaticString = #file, line: UInt = #line
) -> Never {
  _internalInvariant(false, message, file: file, line: line)
  _conditionallyUnreachable()
}
