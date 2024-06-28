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

import Swift
import SwiftShims

#if os(Windows)
import CRT
import WinSDK
#else
#if canImport(Darwin)
import Darwin
#elseif canImport(Glibc)
import Glibc
#elseif canImport(Musl)
import Musl
#elseif canImport(Android)
import Android
#elseif canImport(WASILibc)
import WASILibc
#endif
let (platform_read, platform_write, platform_close) = (read, write, close)
#endif 

#if os(Windows)
public struct _FDInputStream {
  public var handle: HANDLE = INVALID_HANDLE_VALUE
  public var isEOF: Bool = false
  public var isClosed: Bool { return handle == INVALID_HANDLE_VALUE }

  internal var _buffer: ContiguousArray<UInt8> =
      ContiguousArray<UInt8>(repeating: 0, count: 256)
  internal var _offset: Int = 0

  public init(handle: HANDLE) {
    self.handle = handle
  }

  public mutating func getline() -> String? {
    // FIXME(compnerd) Windows uses \r\n for the line delimiter, we should split
    // on that and remove the workaround in the test harness
    if let index =
        _buffer[0..<_offset].firstIndex(of: UInt8(Unicode.Scalar("\n").value)) {
      let result = String(decoding: _buffer[0..<index], as: UTF8.self)
      _buffer.removeSubrange(0...index)
      _offset -= index + 1
      return result
    }
    if isEOF && _offset > 0 {
      let result = String(decoding: _buffer[0..<_offset], as: UTF8.self)
      _buffer.removeAll()
      _offset = 0
      return result
    }
    return nil
  }

  public mutating func read() {
    var space = _buffer.count - _offset
    if space < 128 {
      let capacity = _buffer.count + (128 - space)
      _buffer.reserveCapacity(capacity)
      for _ in _buffer.count..<capacity {
        _buffer.append(0)
      }
      space = 128
    }
    let read: Int = _buffer.withUnsafeMutableBufferPointer { buffer in
      var read: DWORD = 0
      ReadFile(handle, buffer.baseAddress! + _offset, DWORD(space), &read, nil)
      return Int(read)
    }
    if read == 0 {
      isEOF = true
    } else {
      _offset += read
    }
  }

  public mutating func close() {
    if isClosed { return }
    CloseHandle(handle)
    handle = INVALID_HANDLE_VALUE
  }
}
#else
public struct _FDInputStream {
  public let fd: CInt
  public var isClosed: Bool = false
  public var isEOF: Bool = false
  internal var _buffer = [UInt8](repeating: 0, count: 256)
  internal var _bufferUsed: Int = 0

  public init(fd: CInt) {
    self.fd = fd
  }

  public mutating func getline() -> String? {
    if let newlineIndex =
      _buffer[0..<_bufferUsed].firstIndex(of: UInt8(Unicode.Scalar("\n").value)) {
      let result = String(decoding: _buffer[0..<newlineIndex], as: UTF8.self)
      _buffer.removeSubrange(0...newlineIndex)
      _bufferUsed -= newlineIndex + 1
      return result
    }
    if isEOF && _bufferUsed > 0 {
      let result = String(decoding: _buffer[0..<_bufferUsed], as: UTF8.self)
      _buffer.removeAll()
      _bufferUsed = 0
      return result
    }
    return nil
  }

  public mutating func read() {
    let minFree = 128
    var bufferFree = _buffer.count - _bufferUsed
    if bufferFree < minFree {
      _buffer.reserveCapacity(minFree - bufferFree)
      while bufferFree < minFree {
        _buffer.append(0)
        bufferFree += 1
      }
    }
    let fd = self.fd
    let readResult: Int = _buffer.withUnsafeMutableBufferPointer {
      (_buffer) in
      let addr = _buffer.baseAddress! + self._bufferUsed
      let size = bufferFree
      return platform_read(fd, addr, size)
    }
    if readResult == 0 {
      isEOF = true
      return
    }
    if readResult < 0 {
      fatalError("read() returned error")
    }
    _bufferUsed += readResult
  }

  public mutating func close() {
    if isClosed {
      return
    }
    let result = platform_close(fd)
    if result < 0 {
      fatalError("close() returned an error")
    }
    isClosed = true
  }
}
#endif

public struct _Stderr : TextOutputStream {
  public init() {}

  public mutating func write(_ string: String) {
    for c in string.utf8 {
      _swift_stdlib_putc_stderr(CInt(c))
    }
  }
}

#if os(Windows)
public struct _FDOutputStream : TextOutputStream {
  public var handle: HANDLE
  public var isClosed: Bool {
    return handle == INVALID_HANDLE_VALUE
  }

  public init(handle: HANDLE) {
    self.handle = handle
  }

  public mutating func write(_ string: String) {
    string.utf8CString.withUnsafeBufferPointer { buffer in
      let dwLength: DWORD = DWORD(buffer.count - 1)
      var dwOffset: DWORD = 0
      while dwOffset < dwLength {
        var dwBytesWritten: DWORD = 0
        if !WriteFile(handle,
                      UnsafeRawPointer(buffer.baseAddress! + Int(dwOffset)),
                      dwLength - dwOffset, &dwBytesWritten, nil) {
          fatalError("WriteFile() failed")
        }
        dwOffset += dwBytesWritten
      }
    }
  }

  public mutating func close() {
    if handle == INVALID_HANDLE_VALUE { return }
    CloseHandle(handle)
    handle = INVALID_HANDLE_VALUE
  }
}
#else
public struct _FDOutputStream : TextOutputStream {
  public let fd: CInt
  public var isClosed: Bool = false

  public init(fd: CInt) {
    self.fd = fd
  }

  public mutating func write(_ string: String) {
    let utf8CStr = string.utf8CString
    utf8CStr.withUnsafeBufferPointer {
      (utf8CStr) -> Void in
      var writtenBytes = 0
      let bufferSize = utf8CStr.count - 1
      while writtenBytes != bufferSize {
        let result = platform_write(
          self.fd, UnsafeRawPointer(utf8CStr.baseAddress! + Int(writtenBytes)),
          bufferSize - writtenBytes)
        if result < 0 {
          fatalError("write() returned an error")
        }
        writtenBytes += result
      }
    }
  }

  public mutating func close() {
    if isClosed {
      return
    }
    let result = platform_close(fd)
    if result < 0 {
      fatalError("close() returned an error")
    }
    isClosed = true
  }
}
#endif
