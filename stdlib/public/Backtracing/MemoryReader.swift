//===--- MemoryReader.swift -----------------------------------*- swift -*-===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2023 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//
//
//  Provides the ability to read memory, both in the current process and
//  remotely.
//
//===----------------------------------------------------------------------===//

import Swift

@_implementationOnly import OS.Libc
#if os(macOS)
@_implementationOnly import OS.Darwin
#elseif os(Linux)
@_implementationOnly import OS.Linux
#endif

@_implementationOnly import Runtime

@_spi(MemoryReaders) public protocol MemoryReader {
  associatedtype Address: FixedWidthInteger
  associatedtype Size: FixedWidthInteger

  /// Fill the specified buffer with data from the specified location in
  /// the source.
  func fetch<T>(from address: Address,
                into buffer: UnsafeMutableBufferPointer<T>) throws

  /// Write data from the specified location in the source through a pointer
  func fetch<T>(from addr: Address,
                into pointer: UnsafeMutablePointer<T>) throws

  /// Fetch an array of Ts from the specified location in the source
  func fetch<T>(from addr: Address, count: Int, as: T.Type) throws -> [T]

  /// Fetch a T from the specified location in the source
  func fetch<T>(from addr: Address, as: T.Type) throws -> T

  /// Fetch a NUL terminated string from the specified location in the source
  func fetchString(from addr: Address) throws -> String?
}

extension MemoryReader {

  public func fetch<T>(from addr: Address,
                       into pointer: UnsafeMutablePointer<T>) throws {
    try fetch(from: addr,
              into: UnsafeMutableBufferPointer(start: pointer, count: 1))
  }

  public func fetch<T>(from addr: Address, count: Int, as: T.Type) throws -> [T] {
    let array = try Array<T>(unsafeUninitializedCapacity: count){
      buffer, initializedCount in

      try fetch(from: addr, into: buffer)

      initializedCount = count
    }

    return array
  }

  public func fetch<T>(from addr: Address, as: T.Type) throws -> T {
    return try withUnsafeTemporaryAllocation(of: T.self, capacity: 1) { buf in
      try fetch(from: addr, into: buf)
      return buf[0]
    }
  }

  public func fetchString(from addr: Address) throws -> String? {
    var bytes: [UInt8] = []
    var ptr = addr
    while true {
      let ch = try fetch(from: ptr, as: UInt8.self)
      if ch == 0 {
        break
      }
      bytes.append(ch)
      ptr += 1
    }

    return String(decoding: bytes, as: UTF8.self)
  }

}

@_spi(MemoryReaders) public struct UnsafeLocalMemoryReader: MemoryReader {
  public typealias Address = UInt
  public typealias Size = UInt

  public init() {}

  public func fetch<T>(from address: Address,
                       into buffer: UnsafeMutableBufferPointer<T>) throws {
    buffer.baseAddress!.update(from: UnsafePointer<T>(bitPattern: address)!,
                               count: buffer.count)
  }
}

#if os(macOS)
@_spi(MemoryReaders) public struct MachError: Error {
  var result: kern_return_t
}

@_spi(MemoryReaders) public struct RemoteMemoryReader: MemoryReader {
  public typealias Address = UInt64
  public typealias Size = UInt64

  private var task: task_t

  // Sadly we can't expose the type of this argument
  public init(task: Any) {
    self.task = task as! task_t
  }

  public func fetch<T>(from address: Address,
                       into buffer: UnsafeMutableBufferPointer<T>) throws {
    let size = UInt64(MemoryLayout<T>.stride * buffer.count)
    var sizeOut = UInt64(0)
    let result = mach_vm_read_overwrite(task,
                                        UInt64(address),
                                        UInt64(size),
                                        buffer.baseAddress,
                                        &sizeOut)

    if result != KERN_SUCCESS {
      throw MachError(result: result)
    }
  }
}

@_spi(MemoryReaders) public struct LocalMemoryReader: MemoryReader {
  public typealias Address = UInt64
  public typealias Size = UInt64

  public func fetch<T>(from address: Address,
                       into buffer: UnsafeMutableBufferPointer<T>) throws {
    let reader = RemoteMemoryReader(task: mach_task_self())
    return try reader.fetch(from: address, into: buffer)
  }
}
#endif

#if os(Linux)
@_spi(MemoryReaders) public struct POSIXError: Error {
  var errno: CInt
}

@_spi(MemoryReaders) public struct MemserverError: Error {
  var message: String
}

@_spi(MemoryReaders) public struct MemserverMemoryReader: MemoryReader {
  public typealias Address = UInt64
  public typealias Size = UInt64

  private var fd: CInt

  public init(fd: CInt) {
    self.fd = fd
  }

  private func sendRequest(for bytes: Size, from addr: Address) throws {
    var request = memserver_req(addr: addr, len: bytes)
    try withUnsafeBytes(of: &request){ ptr in
      let ret = write(fd, ptr.baseAddress, ptr.count)
      if ret < 0 || ret != ptr.count {
        throw POSIXError(errno: _swift_get_errno())
      }
    }
  }

  private func receiveReply() throws -> memserver_resp {
    var response = memserver_resp(addr: 0, len: 0)
    try withUnsafeMutableBytes(of: &response){ ptr in
      let ret = read(fd, ptr.baseAddress, ptr.count)
      if ret < 0 || ret != ptr.count {
        throw POSIXError(errno: _swift_get_errno())
      }
    }
    return response
  }

  public func fetch<T>(from addr: Address,
                       into buffer: UnsafeMutableBufferPointer<T>) throws {
    try buffer.withMemoryRebound(to: UInt8.self) { bytes in
      try sendRequest(for: Size(bytes.count), from: addr)

      var done = 0
      while done < bytes.count {
        let reply = try receiveReply()

        if reply.len < 0 {
          throw MemserverError(message: "Unreadable")
        }

        if done + Int(reply.len) > bytes.count {
          throw MemserverError(message: "Overrun")
        }

        let ret = read(fd,
                       bytes.baseAddress!.advanced(by: done),
                       Int(reply.len))

        if ret < 0 || ret != reply.len {
          throw POSIXError(errno: _swift_get_errno())
        }

        done += Int(reply.len)
      }
    }
  }
}

@_spi(MemoryReaders) public struct RemoteMemoryReader: MemoryReader {
  public typealias Address = UInt64
  public typealias Size = UInt64

  private var pid: pid_t

  public init(pid: Any) {
    self.pid = pid as! pid_t
  }

  public func fetch<T>(from address: Address,
                       into buffer: UnsafeMutableBufferPointer<T>) throws {
    let size = size_t(MemoryLayout<T>.stride * buffer.count)
    var fromIOVec = iovec(iov_base: UnsafeMutableRawPointer(
                            bitPattern: UInt(address)),
                          iov_len: size)
    var toIOVec = iovec(iov_base: buffer.baseAddress, iov_len: size)
    let result = process_vm_readv(pid, &toIOVec, 1, &fromIOVec, 1, 0)
    if result != size {
      throw POSIXError(errno: _swift_get_errno())
    }
  }
}

@_spi(MemoryReaders) public struct LocalMemoryReader: MemoryReader {
  public typealias Address = UInt64
  public typealias Size = UInt64

  private var reader: RemoteMemoryReader

  init() {
    reader = RemoteMemoryReader(pid: getpid())
  }

  public func fetch<T>(from address: Address,
                       into buffer: UnsafeMutableBufferPointer<T>) throws {
    return try reader.fetch(from: address, into: buffer)
  }
}
#endif
