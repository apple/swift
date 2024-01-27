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

//===----------------------------------------------------------------------===//
// Input/Output interfaces
//===----------------------------------------------------------------------===//

/// A type that can be the target of text-streaming operations.
///
/// You can send the output of the standard library's `print(_:to:)` and
/// `dump(_:to:)` functions to an instance of a type that conforms to the
/// `TextOutputStream` protocol instead of to standard output. Swift's
/// `String` type conforms to `TextOutputStream` already, so you can capture
/// the output from `print(_:to:)` and `dump(_:to:)` in a string instead of
/// logging it to standard output.
///
///     var s = ""
///     for n in 1...5 {
///         print(n, terminator: "", to: &s)
///     }
///     // s == "12345"
///
/// Conforming to the TextOutputStream Protocol
/// ===========================================
///
/// To make your custom type conform to the `TextOutputStream` protocol,
/// implement the required `write(_:)` method. Functions that use a
/// `TextOutputStream` target may call `write(_:)` multiple times per writing
/// operation.
///
/// As an example, here's an implementation of an output stream that converts
/// any input to its plain ASCII representation before sending it to standard
/// output.
///
///     struct ASCIILogger: TextOutputStream {
///         mutating func write(_ string: String) {
///             let ascii = string.unicodeScalars.lazy.map { scalar in
///                 scalar == "\n"
///                   ? "\n"
///                   : scalar.escaped(asASCII: true)
///             }
///             print(ascii.joined(separator: ""), terminator: "")
///         }
///     }
///
/// The `ASCIILogger` type's `write(_:)` method processes its string input by
/// escaping each Unicode scalar, with the exception of `"\n"` line returns.
/// By sending the output of the `print(_:to:)` function to an instance of
/// `ASCIILogger`, you invoke its `write(_:)` method.
///
///     let s = "Hearts ♡ and Diamonds ♢"
///     print(s)
///     // Prints "Hearts ♡ and Diamonds ♢"
///
///     var asciiLogger = ASCIILogger()
///     print(s, to: &asciiLogger)
///     // Prints "Hearts \u{2661} and Diamonds \u{2662}"
public protocol TextOutputStream {
  mutating func _lock()
  mutating func _unlock()

  /// Appends the given string to the stream.
  mutating func write(_ string: String)

  mutating func _writeASCII(_ buffer: UnsafeBufferPointer<UInt8>)
}

extension TextOutputStream {
  public mutating func _lock() {}
  public mutating func _unlock() {}

  public mutating func _writeASCII(_ buffer: UnsafeBufferPointer<UInt8>) {
    write(String._fromASCII(buffer))
  }
}

/// A source of text-streaming operations.
///
/// Instances of types that conform to the `TextOutputStreamable` protocol can
/// write their value to instances of any type that conforms to the
/// `TextOutputStream` protocol. The Swift standard library's text-related
/// types, `String`, `Character`, and `Unicode.Scalar`, all conform to
/// `TextOutputStreamable`.
///
/// Conforming to the TextOutputStreamable Protocol
/// =====================================
///
/// To add `TextOutputStreamable` conformance to a custom type, implement the
/// required `write(to:)` method. Call the given output stream's `write(_:)`
/// method in your implementation.
public protocol TextOutputStreamable {
  /// Writes a textual representation of this instance into the given output
  /// stream.
  func write<Target: TextOutputStream>(to target: inout Target)
}

/// A type with a customized textual representation.
///
/// Types that conform to the `CustomStringConvertible` protocol can provide
/// their own representation to be used when converting an instance to a
/// string. The `String(describing:)` initializer is the preferred way to
/// convert an instance of *any* type to a string. If the passed instance
/// conforms to `CustomStringConvertible`, the `String(describing:)`
/// initializer and the `print(_:)` function use the instance's custom
/// `description` property.
///
/// Accessing a type's `description` property directly or using
/// `CustomStringConvertible` as a generic constraint is discouraged.
///
/// Conforming to the CustomStringConvertible Protocol
/// ==================================================
///
/// Add `CustomStringConvertible` conformance to your custom types by defining
/// a `description` property.
///
/// For example, this custom `Point` struct uses the default representation
/// supplied by the standard library:
///
///     struct Point {
///         let x: Int, y: Int
///     }
///
///     let p = Point(x: 21, y: 30)
///     print(p)
///     // Prints "Point(x: 21, y: 30)"
///
/// After implementing the `description` property and declaring
/// `CustomStringConvertible` conformance, the `Point` type provides its own
/// custom representation.
///
///     extension Point: CustomStringConvertible {
///         var description: String {
///             return "(\(x), \(y))"
///         }
///     }
///
///     print(p)
///     // Prints "(21, 30)"
public protocol CustomStringConvertible {
  /// A textual representation of this instance.
  ///
  /// Calling this property directly is discouraged. Instead, convert an
  /// instance of any type to a string by using the `String(describing:)`
  /// initializer. This initializer works with any type, and uses the custom
  /// `description` property for types that conform to
  /// `CustomStringConvertible`:
  ///
  ///     struct Point: CustomStringConvertible {
  ///         let x: Int, y: Int
  ///
  ///         var description: String {
  ///             return "(\(x), \(y))"
  ///         }
  ///     }
  ///
  ///     let p = Point(x: 21, y: 30)
  ///     let s = String(describing: p)
  ///     print(s)
  ///     // Prints "(21, 30)"
  ///
  /// The conversion of `p` to a string in the assignment to `s` uses the
  /// `Point` type's `description` property.
  var description: String { get }
}

/// A type that can be represented as a string in a lossless, unambiguous way.
///
/// For example, the integer value 1050 can be represented in its entirety as
/// the string "1050".
///
/// The description property of a conforming type must be a value-preserving
/// representation of the original value. As such, it should be possible to
/// re-create an instance from its string representation.
public protocol LosslessStringConvertible: CustomStringConvertible {
  /// Instantiates an instance of the conforming type from a string
  /// representation.
  init?(_ description: String)
}

/// A type with a customized textual representation suitable for debugging
/// purposes.
///
/// Swift provides a default debugging textual representation for any type.
/// That default representation is used by the `String(reflecting:)`
/// initializer and the `debugPrint(_:)` function for types that don't provide
/// their own. To customize that representation, make your type conform to the
/// `CustomDebugStringConvertible` protocol.
///
/// Because the `String(reflecting:)` initializer works for instances of *any*
/// type, returning an instance's `debugDescription` if the value passed
/// conforms to `CustomDebugStringConvertible`, accessing a type's
/// `debugDescription` property directly or using
/// `CustomDebugStringConvertible` as a generic constraint is discouraged.
///
/// - Note: Calling the `dump(_:_:_:_:)` function and printing in the debugger
///   uses both `String(reflecting:)` and `Mirror(reflecting:)` to collect
///   information about an instance. If you implement
///   `CustomDebugStringConvertible` conformance for your custom type, you may
///   want to consider providing a custom mirror by implementing
///   `CustomReflectable` conformance, as well.
///
/// Conforming to the CustomDebugStringConvertible Protocol
/// =======================================================
///
/// Add `CustomDebugStringConvertible` conformance to your custom types by
/// defining a `debugDescription` property.
///
/// For example, this custom `Point` struct uses the default representation
/// supplied by the standard library:
///
///     struct Point {
///         let x: Int, y: Int
///     }
///
///     let p = Point(x: 21, y: 30)
///     print(String(reflecting: p))
///     // Prints "Point(x: 21, y: 30)"
///
/// After adding `CustomDebugStringConvertible` conformance by implementing the
/// `debugDescription` property, `Point` provides its own custom debugging
/// representation.
///
///     extension Point: CustomDebugStringConvertible {
///         var debugDescription: String {
///             return "(\(x), \(y))"
///         }
///     }
///
///     print(String(reflecting: p))
///     // Prints "(21, 30)"
public protocol CustomDebugStringConvertible {
  /// A textual representation of this instance, suitable for debugging.
  ///
  /// Calling this property directly is discouraged. Instead, convert an
  /// instance of any type to a string by using the `String(reflecting:)`
  /// initializer. This initializer works with any type, and uses the custom
  /// `debugDescription` property for types that conform to
  /// `CustomDebugStringConvertible`:
  ///
  ///     struct Point: CustomDebugStringConvertible {
  ///         let x: Int, y: Int
  ///
  ///         var debugDescription: String {
  ///             return "(\(x), \(y))"
  ///         }
  ///     }
  ///
  ///     let p = Point(x: 21, y: 30)
  ///     let s = String(reflecting: p)
  ///     print(s)
  ///     // Prints "(21, 30)"
  ///
  /// The conversion of `p` to a string in the assignment to `s` uses the
  /// `Point` type's `debugDescription` property.
  var debugDescription: String { get }
}

//===----------------------------------------------------------------------===//
// Default (ad-hoc) printing
//===----------------------------------------------------------------------===//

@_silgen_name("swift_EnumCaseName")
internal func _getEnumCaseName<T>(_ value: T) -> UnsafePointer<CChar>?

@_silgen_name("swift_OpaqueSummary")
internal func _opaqueSummary(_ metadata: Any.Type) -> UnsafePointer<CChar>?

/// Obtain a fallback raw value for an enum type without metadata; this
/// should be OK for enums from C/C++ until they have metadata (see
/// <rdar://22036374>).  Note that if this turns out to be a Swift Enum
/// with missing metadata, the raw value may be misleading.
@_semantics("optimize.sil.specialize.generic.never")
internal func _fallbackEnumRawValue<T>(_ value: T) -> Int64? {
  switch MemoryLayout.size(ofValue: value) {
  case 8:
    return unsafeBitCast(value, to:Int64.self)
  case 4:
    return Int64(unsafeBitCast(value, to:Int32.self))
  case 2:
    return Int64(unsafeBitCast(value, to:Int16.self))
  case 1:
    return Int64(unsafeBitCast(value, to:Int8.self))
  default:
    return nil
  }
}

#if SWIFT_ENABLE_REFLECTION
/// Do our best to print a value that cannot be printed directly.
@_semantics("optimize.sil.specialize.generic.never")
internal func _adHocPrint_unlocked<T, TargetStream: TextOutputStream>(
    _ value: T, _ mirror: Mirror, _ target: inout TargetStream,
    isDebugPrint: Bool
) {
  func printTypeName(_ type: Any.Type) {
    // Print type names without qualification, unless we're debugPrint'ing.
    target.write(_typeName(type, qualified: isDebugPrint))
  }

  if let displayStyle = mirror.displayStyle {
    switch displayStyle {
      case .optional:
        if let child = mirror.children.first {
          _debugPrint_unlocked(child.1, &target)
        } else {
          _debugPrint_unlocked("nil", &target)
        }
      case .tuple:
        target.write("(")
        var first = true
        for (label, value) in mirror.children {
          if first {
            first = false
          } else {
            target.write(", ")
          }

          if let label = label {
            if !label.isEmpty && label[label.startIndex] != "." {
              target.write(label)
              target.write(": ")
            }
          }

          _debugPrint_unlocked(value, &target)
        }
        target.write(")")
      case .struct:
        printTypeName(mirror.subjectType)
        target.write("(")
        var first = true
        for (label, value) in mirror.children {
          if let label = label {
            if first {
              first = false
            } else {
              target.write(", ")
            }
            target.write(label)
            target.write(": ")
            _debugPrint_unlocked(value, &target)
          }
        }
        target.write(")")
      case .enum:
        if let cString = _getEnumCaseName(value),
            let caseName = String(validatingCString: cString) {
          // Write the qualified type name in debugPrint.
          if isDebugPrint {
            printTypeName(mirror.subjectType)
            target.write(".")
          }
          target.write(caseName)
        } else {
          printTypeName(mirror.subjectType)
          // The case name is garbage; this might be a C/C++ enum without
          // metadata, so see if we can get a raw value
          if let rawValue = _fallbackEnumRawValue(value) {
            target.write("(rawValue: ")
            _debugPrint_unlocked(rawValue, &target);
            target.write(")")
          }
        }
        if let (_, value) = mirror.children.first {
          if Mirror(reflecting: value).displayStyle == .tuple {
            _debugPrint_unlocked(value, &target)
          } else {
            target.write("(")
            _debugPrint_unlocked(value, &target)
            target.write(")")
          }
        }
      default:
        target.write(_typeName(mirror.subjectType))
    }
  } else if let metatypeValue = value as? Any.Type {
    // Metatype
    printTypeName(metatypeValue)
  } else {
    // Fall back to the type or an opaque summary of the kind
    if let cString = _opaqueSummary(mirror.subjectType),
        let opaqueSummary = String(validatingCString: cString) {
      target.write(opaqueSummary)
    } else {
      target.write(_typeName(mirror.subjectType, qualified: true))
    }
  }
}
#endif

@usableFromInline
@_semantics("optimize.sil.specialize.generic.never")
@_unavailableInEmbedded
internal func _print_unlocked<T, TargetStream: TextOutputStream>(
  _ value: T, _ target: inout TargetStream
) {
  // Optional has no representation suitable for display; therefore,
  // values of optional type should be printed as a debug
  // string. Check for Optional first, before checking protocol
  // conformance below, because an Optional value is convertible to a
  // protocol if its wrapped type conforms to that protocol.
  // Note: _isOptional doesn't work here when T == Any, hence we
  // use a more elaborate formulation:
  if _openExistential(type(of: value as Any), do: _isOptional) {
    let debugPrintable = value as! CustomDebugStringConvertible
    debugPrintable.debugDescription.write(to: &target)
    return
  }

  if let string = value as? String {
    target.write(string)
    return
  }

  if case let streamableObject as TextOutputStreamable = value {
    streamableObject.write(to: &target)
    return
  }

  if case let printableObject as CustomStringConvertible = value {
    printableObject.description.write(to: &target)
    return
  }

  if case let debugPrintableObject as CustomDebugStringConvertible = value {
    debugPrintableObject.debugDescription.write(to: &target)
    return
  }

#if SWIFT_ENABLE_REFLECTION
  let mirror = Mirror(reflecting: value)
  _adHocPrint_unlocked(value, mirror, &target, isDebugPrint: false)
#else
  target.write("(value cannot be printed without reflection)")
#endif
}

//===----------------------------------------------------------------------===//
// `debugPrint`
//===----------------------------------------------------------------------===//

@_semantics("optimize.sil.specialize.generic.never")
@inline(never)
@_unavailableInEmbedded
public func _debugPrint_unlocked<T, TargetStream: TextOutputStream>(
    _ value: T, _ target: inout TargetStream
) {
  if let debugPrintableObject = value as? CustomDebugStringConvertible {
    debugPrintableObject.debugDescription.write(to: &target)
    return
  }

  if let printableObject = value as? CustomStringConvertible {
    printableObject.description.write(to: &target)
    return
  }

  if let streamableObject = value as? TextOutputStreamable {
    streamableObject.write(to: &target)
    return
  }

#if SWIFT_ENABLE_REFLECTION
  let mirror = Mirror(reflecting: value)
  _adHocPrint_unlocked(value, mirror, &target, isDebugPrint: true)
#else
  target.write("(value cannot be printed without reflection)")
#endif
}

#if SWIFT_ENABLE_REFLECTION
@_semantics("optimize.sil.specialize.generic.never")
internal func _dumpPrint_unlocked<T, TargetStream: TextOutputStream>(
    _ value: T, _ mirror: Mirror, _ target: inout TargetStream
) {
  if let displayStyle = mirror.displayStyle {
    // Containers and tuples are always displayed in terms of their element
    // count
    switch displayStyle {
    case .tuple:
      let count = mirror.children.count
      target.write(count == 1 ? "(1 element)" : "(\(count) elements)")
      return
    case .collection:
      let count = mirror.children.count
      target.write(count == 1 ? "1 element" : "\(count) elements")
      return
    case .dictionary:
      let count = mirror.children.count
      target.write(count == 1 ? "1 key/value pair" : "\(count) key/value pairs")
      return
    case .`set`:
      let count = mirror.children.count
      target.write(count == 1 ? "1 member" : "\(count) members")
      return
    default:
      break
    }
  }

  if let debugPrintableObject = value as? CustomDebugStringConvertible {
    debugPrintableObject.debugDescription.write(to: &target)
    return
  }

  if let printableObject = value as? CustomStringConvertible {
    printableObject.description.write(to: &target)
    return
  }

  if let streamableObject = value as? TextOutputStreamable {
    streamableObject.write(to: &target)
    return
  }

  if let displayStyle = mirror.displayStyle {
    switch displayStyle {
    case .`class`, .`struct`:
      // Classes and structs without custom representations are displayed as
      // their fully qualified type name
      target.write(_typeName(mirror.subjectType, qualified: true))
      return
    case .`enum`:
      target.write(_typeName(mirror.subjectType, qualified: true))
      if let cString = _getEnumCaseName(value),
          let caseName = String(validatingCString: cString) {
        target.write(".")
        target.write(caseName)
      }
      return
    default:
      break
    }
  }

  _adHocPrint_unlocked(value, mirror, &target, isDebugPrint: true)
}
#endif

//===----------------------------------------------------------------------===//
// OutputStreams
//===----------------------------------------------------------------------===//

internal struct _Stdout: TextOutputStream {
  internal init() {}

  internal mutating func _lock() {
    _swift_stdlib_flockfile_stdout()
  }

  internal mutating func _unlock() {
    _swift_stdlib_funlockfile_stdout()
  }

  internal mutating func write(_ string: String) {
    if string.isEmpty { return }

    var string = string
    _ = string.withUTF8 { utf8 in
      _swift_stdlib_fwrite_stdout(utf8.baseAddress!, 1, utf8.count)
    }
  }
}

extension String: TextOutputStream {
  /// Appends the given string to this string.
  ///
  /// - Parameter other: A string to append.
  public mutating func write(_ other: String) {
    self += other
  }

  public mutating func _writeASCII(_ buffer: UnsafeBufferPointer<UInt8>) {
    self._guts.append(_StringGuts(buffer, isASCII: true))
  }
}

//===----------------------------------------------------------------------===//
// Streamables
//===----------------------------------------------------------------------===//

extension String: TextOutputStreamable {
  /// Writes the string into the given output stream.
  ///
  /// - Parameter target: An output stream.
  @inlinable
  public func write<Target: TextOutputStream>(to target: inout Target) {
    target.write(self)
  }
}

extension Character: TextOutputStreamable {
  /// Writes the character into the given output stream.
  ///
  /// - Parameter target: An output stream.
  public func write<Target: TextOutputStream>(to target: inout Target) {
    target.write(String(self))
  }
}

extension Unicode.Scalar: TextOutputStreamable {
  /// Writes the textual representation of the Unicode scalar into the given
  /// output stream.
  ///
  /// - Parameter target: An output stream.
  public func write<Target: TextOutputStream>(to target: inout Target) {
    target.write(String(Character(self)))
  }
}

/// A hook for playgrounds to print through.
public var _playgroundPrintHook: ((String) -> Void)? = nil

internal struct _TeeStream<L: TextOutputStream, R: TextOutputStream>
  : TextOutputStream
{
  internal var left: L
  internal var right: R

  internal init(left: L, right: R) {
    self.left = left
    self.right = right
  }
  /// Append the given `string` to this stream.
  internal mutating func write(_ string: String) {
    left.write(string); right.write(string)
  }

  internal mutating func _lock() { left._lock(); right._lock() }
  internal mutating func _unlock() { right._unlock(); left._unlock() }
}
