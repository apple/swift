//===--- NSStringConversion.swift -----------------------------------------===//
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

// <rdar://problem/19003201>
#if canImport(Darwin)

import TestsUtils
import Foundation
import Darwin
import _Concurrency

@available(macOS 9999, *)
public let AsyncSequenceFoundation = [
  BenchmarkInfo(name: "LoopbackLines",
                runFunction: run_loopbackLines,
                tags: [.concurrency]
                )
]

//Some bits copy-pasted from Swift System
@available(macOS 9999, *)
struct FileDescriptor: RawRepresentable, Hashable, Codable {
  /// The raw C file handle.
  let rawValue: CInt
  
  /// Creates a strongly-typed file handle from a raw C file handle.
  init(rawValue: CInt) { self.rawValue = rawValue }
  
  func _read(
    into buffer: UnsafeMutableRawBufferPointer,
    retryOnInterrupt: Bool = true
  ) throws -> Result<Int, Errno> {
    valueOrErrno(retryOnInterrupt: retryOnInterrupt) {
      system_read(self.rawValue, buffer.baseAddress, buffer.count)
    }
  }
  
  func read(
    into buffer: UnsafeMutableRawBufferPointer,
    retryOnInterrupt: Bool = true
  ) throws -> Int {
    try _read(into: buffer, retryOnInterrupt: retryOnInterrupt).get()
  }
  
}

@available(macOS 9999, *)
var system_errno: CInt {
  get { Darwin.errno }
  set { Darwin.errno = newValue }
}

@available(macOS 9999, *)
var _EINTR: CInt { 4 }

@available(macOS 9999, *)
struct Errno: RawRepresentable, Error, Hashable, Codable {
  /// The raw C error number.
  let rawValue: CInt
  
  /// Creates a strongly typed error number from a raw C error number.
  init(rawValue: CInt) { self.rawValue = rawValue }
  
  static var current: Errno {
    get { Errno(rawValue: system_errno) }
    set { system_errno = newValue.rawValue }
  }
  
  
  static var interrupted: Errno { Errno(rawValue: _EINTR) }
}

@available(macOS 9999, *)
func valueOrErrno<I: FixedWidthInteger>(
  _ i: I
) -> Result<I, Errno> {
  i == -1 ? .failure(Errno.current) : .success(i)
}

@available(macOS 9999, *)
func system_read(
  _ fd: Int32, _ buf: UnsafeMutableRawPointer!, _ nbyte: Int
) -> Int {
  return read(fd, buf, nbyte)
}

@available(macOS 9999, *)
func valueOrErrno<I: FixedWidthInteger>(
  retryOnInterrupt: Bool, _ f: () -> I
) -> Result<I, Errno> {
  repeat {
    switch valueOrErrno(f()) {
    case .success(let r): return .success(r)
    case .failure(let err):
      guard retryOnInterrupt && err == .interrupted else { return .failure(err) }
      break
    }
  } while true
}

//end of copy-pasted System bits

@available(macOS 9999, *)
final actor IOActor {
  func createNSError(posixErrno: Errno, fileDescriptor: FileDescriptor, isReading: Bool = true) -> Error {
    return NSError(domain: NSPOSIXErrorDomain, code: Int(posixErrno.rawValue), userInfo: [:])
  }
  
  func read(from fd: FileDescriptor, into buffer: UnsafeMutableRawBufferPointer) async throws -> Int {
    if buffer.count == 0 { return 0 }
    
    do {
      return try fd.read(into: buffer)
    } catch (let err as Errno) {
      throw createNSError(posixErrno:err, fileDescriptor: fd)
    }
  }
  
  func createFileHandle(reading url: URL) async throws -> FileHandle {
    return try FileHandle(forReadingFrom: url)
  }
  
  static let `default` = IOActor()
}

@available(macOS 9999, *)
extension FileHandle {
  
  struct FakeAsyncBytes: AsyncSequence {
    typealias Element = UInt8
    typealias AsyncIterator = FileHandle.FakeAsyncBytes.Iterator
    var handle: FileHandle
    
    internal init(file: FileHandle) {
      handle = file
    }
    
    func makeAsyncIterator() -> Iterator {
      return Iterator(file: handle)
    }
    
    final actor Iterator : AsyncSequence, AsyncIteratorProtocol {
      typealias Element = UInt8
      typealias AsyncIterator = FileHandle.FakeAsyncBytes.Iterator
      var handle: FileHandle
      var buffer = UnsafeMutableRawBufferPointer.allocate(byteCount: 16384, alignment: MemoryLayout<AnyObject>.alignment)
      var bufferRange = 0..<0
      var descriptor: FileDescriptor
      var finished = false
      
      internal init(file: FileHandle) {
        handle = file
        descriptor = FileDescriptor(rawValue: handle.fileDescriptor)
      }
      
      @inline(never)
      func reloadBufferAndNext() async throws -> UInt8? {
        try Task.checkCancellation()
        if finished {
          return nil
        }
        bufferRange = 0..<0
        do {
          let readCount = try await IOActor.default.read(from: descriptor, into: buffer)
          if readCount == 0 {
            finished = true
          }
          bufferRange = 0 ..< readCount
        } catch {
          finished = true
          throw error
        }
        return try await next()
      }
      
      @inline(__always)
      func next() async throws -> UInt8? {
        if !bufferRange.isEmpty {
          defer {
            bufferRange = (bufferRange.lowerBound + MemoryLayout<UInt8>.stride) ..< bufferRange.upperBound
          }
          return buffer.load(fromByteOffset: bufferRange.lowerBound, as: UInt8.self)
        }
        return try await reloadBufferAndNext()
      }
      
      @actorIndependent
      func makeAsyncIterator() -> Iterator {
        return self
      }
      
      deinit {
        buffer.deallocate()
      }
    }
  }
  
  var fakeBytes: FakeAsyncBytes {
    return FakeAsyncBytes(file: self)
  }
}

@available(macOS 9999, *)
struct FakeAsyncLineSequence<Base: AsyncSequence>: AsyncSequence, AsyncIteratorProtocol where Base.Element == UInt8 {
  public typealias AsyncIterator = AsyncLineSequence<Base>
  public typealias Element = String
  
  var byteSource: Base.AsyncIterator
  
  //We should see if this is faster as a local after fixing stack promotion
  var buffer: Array<UInt8> = []
  
  public mutating func next() async throws -> String? {
      /*
       0D 0A: CR-LF
       0A | 0B | 0C | 0D: LF, VT, FF, CR
       E2 80 A8:  U+2028 (LINE SEPARATOR)
       E2 80 A9:  U+2029 (PARAGRAPH SEPARATOR)
       */
      let _CR: UInt8 = 0x0D
      let _LF: UInt8 = 0x0A
      let _SEPARATOR_PREFIX: UInt8 = 0xE2
      let _SEPARATOR_CONTINUATION: UInt8 = 0x80
      let _SEPARATOR_SUFFIX_LINE: UInt8 = 0xA8
      let _SEPARATOR_SUFFIX_PARAGRAPH: UInt8 = 0xA9
    
      func yield() -> String? {
          defer {
              buffer.removeAll(keepingCapacity: true)
          }
          return String(decoding: buffer, as: UTF8.self)
      }
      
      while let first = try await byteSource.next() {
          switch first {
          case _CR:
              let result = yield()
              // Swallow up any subsequent LF
              guard let next = try await byteSource.next() else { return nil }
              if next != _LF { buffer.append(next) }
              return result
          case _LF..<_CR:
              return yield()
          case _SEPARATOR_PREFIX:
              // Try to read: 80 [A8 | A9].
              // If we can't, then we put the byte in the buffer for error correction
              guard let next = try await byteSource.next() else {
                  buffer.append(first)
                  return yield()
              }
              guard next == _SEPARATOR_CONTINUATION else {
                  buffer.append(first)
                  buffer.append(next)
                  continue
              }
              guard let fin = try await byteSource.next() else {
                  buffer.append(first)
                  buffer.append(next)
                  return yield()
                  
              }
              guard fin == _SEPARATOR_SUFFIX_LINE || fin == _SEPARATOR_SUFFIX_PARAGRAPH else {
                  buffer.append(first)
                  buffer.append(next)
                  buffer.append(fin)
                  continue
              }
              return yield()
          default:
              buffer.append(first)
          }
      }
      // Don't emit an empty newline when there is no more content (e.g. end of file)
      if !buffer.isEmpty {
          return yield()
      }
      return nil
  }
  
  public func makeAsyncIterator() -> AsyncLineSequence<Base> {
      return self
  }
  
  internal init(underlyingSequence: Base) {
      byteSource = underlyingSequence.makeAsyncIterator()
  }
}

@available(macOS 9999, *)
struct FakeAsyncUnicodeScalarSequence<Base: AsyncSequence>: AsyncSequence, AsyncIteratorProtocol where Base.Element == UInt8 {
  typealias AsyncIterator = FakeAsyncUnicodeScalarSequence<Base>
  typealias Element = UnicodeScalar
  
  var remaining: Base.AsyncIterator
  var current = [UInt8]()
  
  mutating func nextMultiByte(_ first: UInt8) async throws
  -> UnicodeScalar? {
    current.append(first)
    
    while let next = try await remaining.next() {
      if UTF8.isContinuation(next) {
        current.append(next)
      } else {
        defer {
          current.removeAll(keepingCapacity: true)
          current.append(next)
        }
        return String(decoding: current, as: UTF8.self).unicodeScalars.first
      }
    }
    return String(decoding: current, as: UTF8.self).unicodeScalars.first
  }
  
  @inline(__always)
  mutating func next() async throws -> UnicodeScalar? {
    if let byte = current.count > 0 ? current.removeFirst() : try await remaining.next() {
      if UTF8.isASCII(byte) {
        return UnicodeScalar(byte)
      }
      return try await nextMultiByte(byte)
    }
    return nil
  }
  
  func makeAsyncIterator() -> FakeAsyncUnicodeScalarSequence<Base> {
    return self
  }
  
  internal init(underlyingSequence: Base) {
    remaining = underlyingSequence.makeAsyncIterator()
  }
}

@available(macOS 9999, *)
struct FakeAsyncCharacterSequence<Base: AsyncSequence>: AsyncSequence, AsyncIteratorProtocol where Base.Element == UInt8 {
  typealias AsyncIterator = FakeAsyncCharacterSequence<Base>
  typealias Element = Character
  
  var remaining: FakeAsyncUnicodeScalarSequence<Base>.AsyncIterator
  var accumulator = ""
  
  @inline(__always)
  mutating func next() async throws -> Character? {
    while let scalar = try await remaining.next() {
      accumulator.unicodeScalars.append(scalar)
      if accumulator.count > 1 {
        return accumulator.removeFirst()
      }
    }
    return accumulator.count > 0 ? accumulator.removeFirst() : nil
  }
  
  func makeAsyncIterator() -> FakeAsyncCharacterSequence<Base> {
    return self
  }
  
  internal init(underlyingSequence: Base) {
    remaining = FakeAsyncUnicodeScalarSequence(underlyingSequence: underlyingSequence).makeAsyncIterator()
  }
}

@available(macOS 9999, *)
extension AsyncSequence where Self.Element == UInt8 {
  var fakeLines: FakeAsyncLineSequence<Self> {
    FakeAsyncLineSequence(underlyingSequence: self)
  }
  
  var fakeCharacters: FakeAsyncCharacterSequence<Self> {
    FakeAsyncCharacterSequence(underlyingSequence: self)
  }
  
  var fakeUnicodeScalars: FakeAsyncUnicodeScalarSequence<Self> {
    FakeAsyncUnicodeScalarSequence(underlyingSequence: self)
  }
}

/*
 This version doesn't use custom executors, and uses a simplistic implementation
 of Async{Line, Character, UnicodeScalar}Sequence
 */
@available(macOS 9999, *)
public func run_loopbackLines_fake(_ N: Int) async {
  let pipe = Pipe()
  let reader = pipe.fileHandleForReading
  let writer = pipe.fileHandleForWriting
  //short string because we're not using the fast line breaker, and want to
  //focus on AsyncSequence overhead
  let lines = "a\nb".data(using: .utf8)!
  
  let handle = Task.runDetached {
    for _ in 0..<N {
      writer.write(lines)
    }
    try! writer.close()
  }
  
  do {
    for try await line in reader.fakeBytes.fakeLines {
      blackHole(line)
    }
    try reader.close()
  } catch {
    
  }
  //wait for everything before moving to the next iteration
  let _ = try! await handle.get()
}

#endif
