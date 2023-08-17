//===--- Utils.swift - Some bridging utilities ----------------------------===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2022 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//

@_exported import BasicBridging
import CxxStdlib

/// The assert function to be used in the compiler.
///
/// This overrides the standard Swift assert for two reasons:
/// * We also like to check for assert failures in release builds. Although this could be
///   achieved with `precondition`, it's easy to forget about it and use `assert` instead.
/// * We need to see the error message in crashlogs of release builds. This is even not the
///   case for `precondition`.
@_transparent
public func assert(_ condition: Bool, _ message: @autoclosure () -> String,
                   file: StaticString = #fileID, line: UInt = #line) {
  if !condition {
    fatalError(message(), file: file, line: line)
  }
}

/// The assert function (without a message) to be used in the compiler.
///
/// Unforuntately it's not possible to just add a default argument to `message` in the
/// other `assert` function. We need to defined this overload.
@_transparent
public func assert(_ condition: Bool, file: StaticString = #fileID, line: UInt = #line) {
  if !condition {
    fatalError("", file: file, line: line)
  }
}

//===----------------------------------------------------------------------===//
//                            Debugging Utilities
//===----------------------------------------------------------------------===//

/// Let's lldb's `po` command not print any "internal" properties of the conforming type.
///
/// This is useful if the `description` already contains all the information of a type instance.
public protocol NoReflectionChildren : CustomReflectable { }

public extension NoReflectionChildren {
  var customMirror: Mirror { Mirror(self, children: []) }
}


//===----------------------------------------------------------------------===//
//                              StringRef
//===----------------------------------------------------------------------===//

public struct StringRef : CustomStringConvertible, NoReflectionChildren {
  let _bridged: llvm.StringRef

  public init(bridged: llvm.StringRef) { self._bridged = bridged }

  public var string: String { _bridged.string }
  public var description: String { string }

  public var count: Int {
#if $NewCxxMethodSafetyHeuristics
    Int(_bridged.bytes_end() - _bridged.bytes_begin())
#else
    Int(_bridged.__bytes_endUnsafe() - _bridged.__bytes_beginUnsafe())
#endif
  }

  public subscript(index: Int) -> UInt8 {
#if $NewCxxMethodSafetyHeuristics
    let buffer = UnsafeBufferPointer<UInt8>(start: _bridged.bytes_begin(),
                                            count: count)
#else
    let buffer = UnsafeBufferPointer<UInt8>(start: _bridged.__bytes_beginUnsafe(),
                                            count: count)
#endif
    return buffer[index]
  }

  public static func ==(lhs: StringRef, rhs: StaticString) -> Bool {
#if $NewCxxMethodSafetyHeuristics
    let lhsBuffer = UnsafeBufferPointer<UInt8>(
      start: lhs._bridged.bytes_begin(),
      count: lhs.count)
#else
    let lhsBuffer = UnsafeBufferPointer<UInt8>(
      start: lhs._bridged.__bytes_beginUnsafe(),
      count: lhs.count)
#endif
    return rhs.withUTF8Buffer { (rhsBuffer: UnsafeBufferPointer<UInt8>) in
      if lhsBuffer.count != rhsBuffer.count { return false }
      return lhsBuffer.elementsEqual(rhsBuffer, by: ==)
    }
  }
  
  public static func !=(lhs: StringRef, rhs: StaticString) -> Bool { !(lhs == rhs) }

  public static func ~=(pattern: StaticString, value: StringRef) -> Bool { value == pattern }
}

//===----------------------------------------------------------------------===//
//                            Bridging Utilities
//===----------------------------------------------------------------------===//

extension llvm.StringRef {
  public var string: String {
    String(_cxxString: self.str())
  }
}

extension String {
  /// Underscored to avoid name collision with Swift LLVM Bindings.
  /// To be replaced with a bindings call once bindings are a dependency.
  public func _withStringRef<T>(_ c: (llvm.StringRef) -> T) -> T {
    var str = self
    return str.withUTF8 { buffer in
      return c(llvm.StringRef(buffer.baseAddress, buffer.count))
    }
  }

  /// Underscored to avoid name collision with the std overlay.
  /// To be replaced with an overlay call once the CI uses SDKs built with Swift 5.8.
  public init(_cxxString s: std.string) {
    self.init(cString: s.__c_strUnsafe())
    withExtendedLifetime(s) {}
  }
}

extension Array {
  public func withBridgedArrayRef<T>(_ c: (BridgedArrayRef) -> T) -> T {
    return withUnsafeBytes { buf in
      return c(BridgedArrayRef(data: buf.baseAddress!, numElements: count))
    }
  }
}

public typealias SwiftObject = UnsafeMutablePointer<BridgedSwiftObject>

extension UnsafeMutablePointer where Pointee == BridgedSwiftObject {
  public init<T: AnyObject>(_ object: T) {
    let ptr = unsafeBitCast(object, to: UnsafeMutableRawPointer.self)
    self = ptr.bindMemory(to: BridgedSwiftObject.self, capacity: 1)
  }

  public func getAs<T: AnyObject>(_ objectType: T.Type) -> T {
    return unsafeBitCast(self, to: T.self)
  }
}

extension Optional where Wrapped == UnsafeMutablePointer<BridgedSwiftObject> {
  public func getAs<T: AnyObject>(_ objectType: T.Type) -> T? {
    if let pointer = self {
      return pointer.getAs(objectType)
    }
    return nil
  }
}

extension BridgedArrayRef {
  public func withElements<T, R>(ofType ty: T.Type, _ c: (UnsafeBufferPointer<T>) -> R) -> R {
    let start = data?.bindMemory(to: ty, capacity: numElements);
    let buffer = UnsafeBufferPointer(start: start, count: numElements);
    return c(buffer)
  }
}
