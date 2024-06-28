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

/// The assert function to be used in the compiler.
///
/// This overrides the standard Swift assert for two reasons:
/// * We also like to check for assert failures in release builds. Although this could be
///   achieved with `precondition`, it's easy to forget about it and use `assert` instead.
/// * We need to see the error message in crashlogs of release builds. This is even not the
///   case for `precondition`.
@_transparent
public func assert(_ condition: Bool, _ message: @autoclosure () -> String,
                   file: StaticString = #fileID, line: UInt = #line, function: StaticString = #function) {
  precondition(condition, message(), file: file, line: line, function: function)
}

/// The assert function (without a message) to be used in the compiler.
///
/// Unforuntately it's not possible to just add a default argument to `message` in the
/// other `assert` function. We need to defined this overload.
/// TODO: For some reason the compiler is not happy when adding a `function` argument.
@_transparent
public func assert(_ condition: Bool, file: StaticString = #fileID, line: UInt = #line) {
  precondition(condition, "", file: file, line: line, function: "")
}

/// The assert function to be used in the compiler.
///
/// This overrides the standard Swift precondition and forwards an assertion failure
/// to the assertion-handling in the C++ code base.
@_transparent
public func precondition(_ condition: Bool, _ message: @autoclosure () -> String,
                         file: StaticString = #fileID, line: UInt = #line, function: StaticString = #function) {
  if !_fastPath(condition) {
    let msg = message()
    msg.withCString { msgStr in
      file.withUTF8Buffer { fileBytes in
        function.withUTF8Buffer { functionBytes in
          assertFail(msgStr, fileBytes.baseAddress!, line, functionBytes.baseAddress!)
        }
      }
    }
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

#if !os(Windows)
// TODO: https://github.com/apple/swift/issues/73252

public var standardError = CFileStream(fp: stderr)

#if os(Android) || canImport(Musl)
  public typealias FILEPointer = OpaquePointer
#else
  public typealias FILEPointer = UnsafeMutablePointer<FILE>
#endif

public struct CFileStream: TextOutputStream {
  var fp: FILEPointer

  public func write(_ string: String) {
    fputs(string, fp)
  }

  public func flush() {
    fflush(fp)
  }
}

#endif

//===----------------------------------------------------------------------===//
//                              StringRef
//===----------------------------------------------------------------------===//

public struct StringRef : CustomStringConvertible, NoReflectionChildren {
  let _bridged: BridgedStringRef

  public init(bridged: BridgedStringRef) { self._bridged = bridged }

  public var string: String { String(_bridged)  }
  public var description: String { string }

  public var count: Int {
    _bridged.count
  }

  public subscript(index: Int) -> UInt8 {
    let buffer = UnsafeBufferPointer<UInt8>(start: _bridged.data, count: count)
    return buffer[index]
  }

  public static func ==(lhs: StringRef, rhs: StringRef) -> Bool {
    let lhsBuffer = UnsafeBufferPointer<UInt8>(start: lhs._bridged.data, count: lhs.count)
    let rhsBuffer = UnsafeBufferPointer<UInt8>(start: rhs._bridged.data, count: rhs.count)
    if lhsBuffer.count != rhsBuffer.count { return false }
    return lhsBuffer.elementsEqual(rhsBuffer, by: ==)
  }

  public static func ==(lhs: StringRef, rhs: StaticString) -> Bool {
    let lhsBuffer = UnsafeBufferPointer<UInt8>(start: lhs._bridged.data, count: lhs.count)
    return rhs.withUTF8Buffer { (rhsBuffer: UnsafeBufferPointer<UInt8>) in
      if lhsBuffer.count != rhsBuffer.count { return false }
      return lhsBuffer.elementsEqual(rhsBuffer, by: ==)
    }
  }
  
  public static func !=(lhs: StringRef, rhs: StaticString) -> Bool { !(lhs == rhs) }
  public static func !=(lhs: StringRef, rhs: StringRef) -> Bool { !(lhs == rhs) }

  public static func ~=(pattern: StaticString, value: StringRef) -> Bool { value == pattern }
}

//===----------------------------------------------------------------------===//
//                            Bridging Utilities
//===----------------------------------------------------------------------===//

extension String {
  public func _withBridgedStringRef<T>(_ c: (BridgedStringRef) -> T) -> T {
    var str = self
    return str.withUTF8 { buffer in
      return c(BridgedStringRef(data: buffer.baseAddress, count: buffer.count))
    }
  }

  public init(_ s: BridgedStringRef) {
    let buffer = UnsafeBufferPointer<UInt8>(start: s.data, count: s.count)
    self.init(decoding: buffer, as: UTF8.self)
  }

  public init(taking s: BridgedOwnedString) {
    let buffer = UnsafeBufferPointer<UInt8>(start: s.data, count: s.count)
    self.init(decoding: buffer, as: UTF8.self)
    s.destroy()
  }
}

extension Array {
  public func withBridgedArrayRef<T>(_ c: (BridgedArrayRef) -> T) -> T {
    return withUnsafeBytes { buf in
      return c(BridgedArrayRef(data: buf.baseAddress!, count: count))
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
    let start = data?.bindMemory(to: ty, capacity: count)
    let buffer = UnsafeBufferPointer(start: start, count: count)
    return c(buffer)
  }
}
