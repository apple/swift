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

/// A type representing an error value that can be thrown.
///
/// Any type that declares conformance to the `Error` protocol can be used to
/// represent an error in Swift's error handling system. Because the `Error`
/// protocol has no requirements of its own, you can declare conformance on
/// any custom type you create.
///
/// Using Enumerations as Errors
/// ============================
///
/// Swift's enumerations are well suited to represent simple errors. Create an
/// enumeration that conforms to the `Error` protocol with a case for each
/// possible error. If there are additional details about the error that could
/// be helpful for recovery, use associated values to include that
/// information.
///
/// The following example shows an `IntParsingError` enumeration that captures
/// two different kinds of errors that can occur when parsing an integer from
/// a string: overflow, where the value represented by the string is too large
/// for the integer data type, and invalid input, where nonnumeric characters
/// are found within the input.
///
///     enum IntParsingError: Error {
///         case overflow
///         case invalidInput(Character)
///     }
///
/// The `invalidInput` case includes the invalid character as an associated
/// value.
///
/// The next code sample shows a possible extension to the `Int` type that
/// parses the integer value of a `String` instance, throwing an error when
/// there is a problem during parsing.
///
///     extension Int {
///         init(validating input: String) throws {
///             // ...
///             let c = _nextCharacter(from: input)
///             if !_isValid(c) {
///                 throw IntParsingError.invalidInput(c)
///             }
///             // ...
///         }
///     }
///
/// When calling the new `Int` initializer within a `do` statement, you can use
/// pattern matching to match specific cases of your custom error type and
/// access their associated values, as in the example below.
///
///     do {
///         let price = try Int(validating: "$100")
///     } catch IntParsingError.invalidInput(let invalid) {
///         print("Invalid character: '\(invalid)'")
///     } catch IntParsingError.overflow {
///         print("Overflow error")
///     } catch {
///         print("Other error")
///     }
///     // Prints "Invalid character: '$'"
///
/// Including More Data in Errors
/// =============================
///
/// Sometimes you may want different error states to include the same common
/// data, such as the position in a file or some of your application's state.
/// When you do, use a structure to represent errors. The following example
/// uses a structure to represent an error when parsing an XML document,
/// including the line and column numbers where the error occurred:
///
///     struct XMLParsingError: Error {
///         enum ErrorKind {
///             case invalidCharacter
///             case mismatchedTag
///             case internalError
///         }
///
///         let line: Int
///         let column: Int
///         let kind: ErrorKind
///     }
///
///     func parse(_ source: String) throws -> XMLDoc {
///         // ...
///         throw XMLParsingError(line: 19, column: 5, kind: .mismatchedTag)
///         // ...
///     }
///
/// Once again, use pattern matching to conditionally catch errors. Here's how
/// you can catch any `XMLParsingError` errors thrown by the `parse(_:)`
/// function:
///
///     do {
///         let xmlDoc = try parse(myXMLData)
///     } catch let e as XMLParsingError {
///         print("Parsing error: \(e.kind) [\(e.line):\(e.column)]")
///     } catch {
///         print("Other error: \(error)")
///     }
///     // Prints "Parsing error: mismatchedTag [19:5]"
public protocol Error: Sendable {
#if !$Embedded
  var _domain: String { get }
  var _code: Int { get }

  // Note: _userInfo is always an NSDictionary, but we cannot use that type here
  // because the standard library cannot depend on Foundation. However, the
  // underscore implies that we control all implementations of this requirement.
  var _userInfo: AnyObject? { get }
#endif

#if _runtime(_ObjC)
  func _getEmbeddedNSError() -> AnyObject?
#endif
}

#if _runtime(_ObjC)
extension Error {
  /// Default implementation: there is no embedded NSError.
  public func _getEmbeddedNSError() -> AnyObject? { return nil }
}
#endif

#if _runtime(_ObjC)
// Helper functions for the C++ runtime to have easy access to embedded error,
// domain, code, and userInfo as Objective-C values.
@_silgen_name("")
internal func _getErrorDomainNSString<T: Error>(_ x: UnsafePointer<T>)
-> AnyObject {
  return x.pointee._domain._bridgeToObjectiveCImpl()
}

@_silgen_name("")
internal func _getErrorCode<T: Error>(_ x: UnsafePointer<T>) -> Int {
  return x.pointee._code
}

@_silgen_name("")
internal func _getErrorUserInfoNSDictionary<T: Error>(_ x: UnsafePointer<T>)
-> AnyObject? {
  return x.pointee._userInfo.map { $0 }
}

// Called by the casting machinery to extract an NSError from an Error value.
@_silgen_name("")
internal func _getErrorEmbeddedNSErrorIndirect<T: Error>(
    _ x: UnsafePointer<T>) -> AnyObject? {
  return x.pointee._getEmbeddedNSError()
}

/// Called by compiler-generated code to extract an NSError from an Error value.
public // COMPILER_INTRINSIC
func _getErrorEmbeddedNSError<T: Error>(_ x: T)
-> AnyObject? {
  return x._getEmbeddedNSError()
}

/// Provided by the ErrorObject implementation.
@_silgen_name("_swift_stdlib_getErrorDefaultUserInfo")
internal func _getErrorDefaultUserInfo<T: Error>(_ error: T) -> AnyObject?

/// Provided by the ErrorObject implementation.
/// Called by the casting machinery and by the Foundation overlay.
@_silgen_name("_swift_stdlib_bridgeErrorToNSError")
public func _bridgeErrorToNSError(_ error: __owned Error) -> AnyObject
#endif

/// Called to indicate that a typed error will be thrown.
@_silgen_name("swift_willThrowTypedImpl")
@available(SwiftStdlib 6.0, *)
@usableFromInline
func _willThrowTypedImpl<E: Error>(_ error: E)

#if !$Embedded
/// Called when a typed error will be thrown.
///
/// On new-enough platforms, this will call through to the runtime to invoke
/// the thrown error handler (if one is set).
///
/// On older platforms, the error will not be passed into the runtime, because
/// doing so would require memory allocation (to create the 'any Error').
@inlinable
@_alwaysEmitIntoClient
@_silgen_name("swift_willThrowTyped")
public func _willThrowTyped<E: Error>(_ error: E) {
  if #available(macOS 15.0, iOS 18.0, watchOS 11.0, tvOS 18.0, visionOS 2.0, *) {
    _willThrowTypedImpl(error)
  }
}
#endif

/// Invoked by the compiler when the subexpression of a `try!` expression
/// throws an error.
@_silgen_name("swift_unexpectedError")
public func _unexpectedError(
  _ error: __owned Error,
  filenameStart: Builtin.RawPointer,
  filenameLength: Builtin.Word,
  filenameIsASCII: Builtin.Int1,
  line: Builtin.Word
) {
  #if !$Embedded
  preconditionFailure(
    "'try!' expression unexpectedly raised an error: \(String(reflecting: error))",
    file: StaticString(
      _start: filenameStart,
      utf8CodeUnitCount: filenameLength,
      isASCII: filenameIsASCII),
    line: UInt(line))
  #else
  Builtin.int_trap()
  #endif
}

/// Invoked by the compiler when the subexpression of a `try!` expression
/// throws an error.
@_silgen_name("swift_unexpectedErrorTyped")
@_alwaysEmitIntoClient
@inlinable
public func _unexpectedErrorTyped<E: Error>(
  _ error: __owned E,
  filenameStart: Builtin.RawPointer,
  filenameLength: Builtin.Word,
  filenameIsASCII: Builtin.Int1,
  line: Builtin.Word
) {
  #if !$Embedded
  _unexpectedError(
    error, filenameStart: filenameStart, filenameLength: filenameLength,
    filenameIsASCII: filenameIsASCII, line: line
  )
  #else
  Builtin.int_trap()
  #endif
}

/// Invoked by the compiler when code at top level throws an uncaught error.
@_silgen_name("swift_errorInMain")
public func _errorInMain(_ error: Error) {
  #if !$Embedded
  fatalError("Error raised at top level: \(String(reflecting: error))")
  #else
  Builtin.int_trap()
  #endif
}

/// Invoked by the compiler when code at top level throws an uncaught, typed error.
@_alwaysEmitIntoClient
public func _errorInMainTyped<Failure: Error>(_ error: Failure) -> Never {
  #if !$Embedded
  fatalError("Error raised at top level: \(String(reflecting: error))")
  #else
  Builtin.int_trap()
  #endif
}

/// Runtime function to determine the default code for an Error-conforming type.
/// Called by the Foundation overlay.
@_silgen_name("_swift_stdlib_getDefaultErrorCode")
public func _getDefaultErrorCode<T: Error>(_ error: T) -> Int

#if !$Embedded
extension Error {
  public var _code: Int {
    return _getDefaultErrorCode(self)
  }

  public var _domain: String {
    return String(reflecting: type(of: self))
  }

  public var _userInfo: AnyObject? {
#if _runtime(_ObjC)
    return _getErrorDefaultUserInfo(self)
#else
    return nil
#endif
  }
}
#endif

extension Error where Self: RawRepresentable, Self.RawValue: FixedWidthInteger {
  // The error code of Error with integral raw values is the raw value.
  public var _code: Int {
    if Self.RawValue.isSigned {
      return numericCast(self.rawValue)
    }

    let uintValue: UInt = numericCast(self.rawValue)
    return Int(bitPattern: uintValue)
  }
}
