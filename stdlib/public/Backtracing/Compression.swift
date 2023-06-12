//===--- Compression.swift - Data compression for ELF images --------------===//
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
//
// Defines the compressed image sources, which are used to access compressed
// data from ELF images.
//
// There are three different compression formats we might have to interact
// with, namely zlib (deflate), zstd and LZMA.  We don't implement the
// decompression algorithms here, but rather we will try to use zlib,
// zstd and liblzma respectively.
//
// We support two different modes; one is where the compression libraries
// have been statically linked with us, in which case we can just call them;
// the other is where we will try to load them with `dlopen()`.  We do this
// so as to avoid a hard dependency on them in the runtime.
//
//===----------------------------------------------------------------------===//

#if os(Linux)

import Swift

@_implementationOnly import OS.Libc
@_implementationOnly import CompressionLibs
@_implementationOnly import ImageFormats.Elf

enum CompressedImageSourceError: Error {
  case unboundedImageSource
  case outOfRangeFetch(UInt64, Int)
  case badCompressedData
  case unsupportedFormat
  case libraryNotFound(String)
  case outputOverrun
}

// .. CompressedStream .........................................................

protocol CompressedStream {
  typealias InputSource = () throws -> UnsafeBufferPointer<UInt8>
  typealias OutputSink = (_ used: UInt, _ done: Bool) throws
    -> UnsafeMutableBufferPointer<UInt8>?

  func decompress(input: InputSource, output: OutputSink) throws -> UInt
}

// .. Compression library bindings .............................................

private var lzmaHandle = dlopen("liblzma.so.5", RTLD_LAZY)
private var zlibHandle = dlopen("libz.so.1", RTLD_LAZY)
private var zstdHandle = dlopen("libzstd.so.1", RTLD_LAZY)

private func symbol<T>(_ handle: UnsafeMutableRawPointer?, _ name: String) -> T? {
  guard let handle = handle, let result = dlsym(handle, name) else {
    return nil
  }
  return unsafeBitCast(result, to: T.self)
}

private enum Sym {
  static let lzma_stream_decoder: (
    @convention(c) (UnsafeMutablePointer<lzma_stream>,
                    UInt64, UInt32) -> lzma_ret)?
    = symbol(lzmaHandle, "lzma_stream_decoder")

  static let lzma_code: (@convention(c) (UnsafeMutablePointer<lzma_stream>,
                                         lzma_action) -> lzma_ret)?
    = symbol(lzmaHandle, "lzma_code")

  static let lzma_end: (@convention(c) (UnsafeMutablePointer<lzma_stream>) -> ())?
    = symbol(lzmaHandle, "lzma_end")

  static let inflateInit_: (@convention(c) (z_streamp,
                                            UnsafePointer<CChar>, CInt) -> CInt)?
    = symbol(zlibHandle, "inflateInit_")

  static func inflateInit(_ stream: z_streamp) -> CInt {
    return inflateInit_!(stream, ZLIB_VERSION, CInt(MemoryLayout<z_stream>.size))
  }

  static let inflate: (@convention(c) (z_streamp, CInt) -> CInt)?
    = symbol(zlibHandle, "inflate")

  static let inflateEnd: (@convention(c) (z_streamp) -> CInt)?
    = symbol(zlibHandle, "inflateEnd")

  static let ZSTD_createDStream: (
    @convention(c) () -> UnsafeMutableRawPointer?)?
    = symbol(zstdHandle, "ZSTD_createDStream")

  static let ZSTD_freeDStream: (
    @convention(c) (UnsafeMutableRawPointer) -> UInt)?
    = symbol(zstdHandle, "ZSTD_freeDStream")

  static let ZSTD_decompressStream: (
    @convention(c) (UnsafeMutableRawPointer,
                    UnsafeMutablePointer<ZSTD_outBuffer>,
                    UnsafeMutablePointer<ZSTD_inBuffer>) -> UInt)?
    = symbol(zstdHandle, "ZSTD_decompressStream")

  static let ZSTD_isError: (@convention(c) (UInt) -> UInt)?
    = symbol(zstdHandle, "ZSTD_isError")
}

// .. zlib (deflate) ...........................................................

enum ZLibError: Error {
  case decodeError(CInt)
}

struct ZLibStream: CompressedStream {
  init() {}

  func decompress(input: InputSource, output: OutputSink) throws -> UInt {

    if zlibHandle == nil {
      throw CompressedImageSourceError.libraryNotFound("libz")
    }

    var stream = zlib_stream_init()

    let ret = Sym.inflateInit(&stream)
    if ret != Z_OK {
      throw ZLibError.decodeError(ret)
    }
    defer {
      _ = Sym.inflateEnd!(&stream)
    }

    var outputBufferSize = UInt(0)
    while true {
      if stream.avail_in == 0 {
        let buffer = try input()

        // Not really mutable; this is just an issue with z_const
        stream.next_in = UnsafeMutablePointer(mutating: buffer.baseAddress)
        stream.avail_in = CUnsignedInt(buffer.count)
      }

      if stream.avail_out == 0 {
        guard let buffer = try output(outputBufferSize, false) else {
          throw CompressedImageSourceError.outputOverrun
        }

        stream.next_out = buffer.baseAddress
        stream.avail_out = CUnsignedInt(buffer.count)
        outputBufferSize = UInt(buffer.count)
      }

      let ret = Sym.inflate!(&stream, Z_NO_FLUSH)

      if ret == Z_STREAM_END {
        _ = try output(outputBufferSize - UInt(stream.avail_out), true)
        return stream.total_out
      }

      if ret != Z_OK {
        throw ZLibError.decodeError(ret)
      }
    }
  }
}

// .. zstd .....................................................................

enum ZStdError: Error {
  case unableToCreateStream
  case decodeError(UInt)
}

struct ZStdStream: CompressedStream {
  init() {}

  func decompress(input: InputSource, output: OutputSink) throws -> UInt {

    if zstdHandle == nil {
      throw CompressedImageSourceError.libraryNotFound("libzstd")
    }

    guard let stream = Sym.ZSTD_createDStream!() else {
      throw ZStdError.unableToCreateStream
    }
    defer {
      _ = Sym.ZSTD_freeDStream!(stream)
    }

    var inBuffer = ZSTD_inBuffer(src: nil, size: 0, pos: 0)
    var outBuffer = ZSTD_outBuffer(dst: nil, size: 0, pos: 0)
    var totalOutput = UInt(0)

    while true {
      if inBuffer.size == inBuffer.pos {
        let buffer = try input()

        inBuffer.src = UnsafeRawPointer(buffer.baseAddress)
        inBuffer.size = buffer.count
        inBuffer.pos = 0
      }

      if outBuffer.size == outBuffer.pos {
        let byteCount = UInt(outBuffer.pos)

        totalOutput += byteCount

        guard let buffer = try output(byteCount, false) else {
          throw CompressedImageSourceError.outputOverrun
        }

        outBuffer.dst = UnsafeMutableRawPointer(buffer.baseAddress)
        outBuffer.size = buffer.count
        outBuffer.pos = 0
      }

      let ret = Sym.ZSTD_decompressStream!(stream, &outBuffer, &inBuffer)

      if ret == 0 {
        _ = try output(UInt(outBuffer.pos), true)
        return totalOutput
      }

      if Sym.ZSTD_isError!(ret) != 0 {
        throw ZStdError.decodeError(ret)
      }
    }
  }
}


// .. LZMA .....................................................................

enum LZMAError: Error {
  case decodeError(lzma_ret)
}

struct LZMAStream: CompressedStream {
  private var memlimit: UInt64
  private var flags: UInt32

  init(memlimit: UInt64 = ~UInt64(0), flags: UInt32 = 0) {
    self.memlimit = memlimit
    self.flags = flags
  }

  func decompress(input: InputSource, output: OutputSink) throws -> UInt {

    if lzmaHandle == nil {
      throw CompressedImageSourceError.libraryNotFound("liblzma")
    }

    var stream = lzma_stream_init()

    let ret = Sym.lzma_stream_decoder!(&stream, memlimit, flags)
    if ret != LZMA_OK {
      throw LZMAError.decodeError(ret)
    }
    defer {
      Sym.lzma_end!(&stream)
    }

    var outputBufferSize = UInt(0)
    while true {
      if stream.avail_in == 0 {
        let buffer = try input()
        stream.next_in = buffer.baseAddress
        stream.avail_in = buffer.count
      }

      if stream.avail_out == 0 {
        guard let buffer = try output(outputBufferSize, false) else {
          throw CompressedImageSourceError.outputOverrun
        }

        stream.next_out = buffer.baseAddress
        stream.avail_out = buffer.count
        outputBufferSize = UInt(buffer.count)
      }

      let ret = Sym.lzma_code!(&stream, LZMA_RUN)

      if ret == LZMA_STREAM_END {
        _ = try output(outputBufferSize - UInt(stream.avail_out), true)
        return UInt(stream.total_out)
      }

      if ret != LZMA_OK {
        throw LZMAError.decodeError(ret)
      }
    }
  }
}

// .. Image Sources ............................................................

fileprivate func decompress<S: CompressedStream, I: ImageSource>(
  stream: S, source: I, dataBounds: I.Bounds, uncompressedSize: UInt? = nil)
  throws -> [UInt8] {

  var pos = dataBounds.base
  var remaining = dataBounds.size

  let bufSize = 65536

  if let uncompressedSize = uncompressedSize {
    // If we know the uncompressed size, we can decompress directly into the
    // array.

    let inputBuffer
      = UnsafeMutableBufferPointer<UInt8>.allocate(capacity: bufSize)
    defer {
      inputBuffer.deallocate()
    }

    return try [UInt8].init(unsafeUninitializedCapacity: Int(uncompressedSize)) {
      (outputBuffer: inout UnsafeMutableBufferPointer<UInt8>,
       count: inout Int) in

      count = Int(try stream.decompress(
        input: { () throws -> UnsafeBufferPointer<UInt8> in

          let chunkSize = min(Int(remaining), inputBuffer.count)
          let slice = inputBuffer[0..<chunkSize]
          let buffer = UnsafeMutableBufferPointer(rebasing: slice)

          try source.fetch(from: pos, into: buffer)

          pos += I.Address(chunkSize)
          remaining -= I.Size(chunkSize)

          return UnsafeBufferPointer(buffer)
        },
        output: {
          (used: UInt, done: Bool) throws -> UnsafeMutableBufferPointer<UInt8>? in

          if used == 0 {
            return outputBuffer
          } else {
            return nil
          }
        }
      ))
    }
  } else {
    // Otherwise, we decompress in chunks and append them to the array.

    let buffer
      = UnsafeMutableBufferPointer<UInt8>.allocate(capacity: 2 * bufSize)
    defer {
      buffer.deallocate()
    }

    let inputBuffer = UnsafeMutableBufferPointer(rebasing: buffer[0..<bufSize])
    let outputBuffer = UnsafeMutableBufferPointer(rebasing: buffer[bufSize...])

    var data = [UInt8]()

    _ = try stream.decompress(
      input: { () throws -> UnsafeBufferPointer<UInt8> in

        let chunkSize = min(Int(remaining), inputBuffer.count)
        let slice = inputBuffer[0..<chunkSize]
        let buffer = UnsafeMutableBufferPointer(rebasing: slice)

        try source.fetch(from: pos, into: buffer)

        pos += I.Address(chunkSize)
        remaining -= I.Size(chunkSize)

        return UnsafeBufferPointer(buffer)
      },
      output: {
        (used: UInt, done: Bool) throws -> UnsafeMutableBufferPointer<UInt8>? in

        data.append(contentsOf: outputBuffer[..<Int(used)])
        if !done {
          return outputBuffer
        } else {
          return nil
        }
      }
    )

    return data
  }
}

internal struct ElfCompressedImageSource<Traits: ElfTraits>: ImageSource {

  private var data: [UInt8]

  var isMappedImage: Bool { return false }
  var path: String? { return nil }
  var bounds: Bounds? { return Bounds(base: Address(0), size: Size(data.count)) }

  init(source: some ImageSource) throws {
    guard let bounds = source.bounds else {
      throw CompressedImageSourceError.unboundedImageSource
    }

    if bounds.size < MemoryLayout<Traits.Chdr>.size {
      throw CompressedImageSourceError.badCompressedData
    }

    let chdr = try source.fetch(from: bounds.base,
                                as: Traits.Chdr.self)
    let dataBounds = bounds.adjusted(by: MemoryLayout<Traits.Chdr>.stride)
    let uncompressedSize = UInt(chdr.ch_size)

    switch chdr.ch_type {
      case .ELFCOMPRESS_ZLIB:
        data = try decompress(stream: ZLibStream(),
                              source: source, dataBounds: dataBounds,
                              uncompressedSize: uncompressedSize)
      case .ELFCOMPRESS_ZSTD:
        data = try decompress(stream: ZStdStream(),
                              source: source, dataBounds: dataBounds,
                              uncompressedSize: uncompressedSize)
      default:
        throw CompressedImageSourceError.unsupportedFormat
    }
  }

  public func fetch<T>(from addr: Address,
                       into buffer: UnsafeMutableBufferPointer<T>) throws {
    let toFetch = buffer.count * MemoryLayout<T>.stride
    if addr < 0 || addr > data.count || data.count - Int(addr) < toFetch {
      throw CompressedImageSourceError.outOfRangeFetch(addr, toFetch)
    }

    buffer.withMemoryRebound(to: UInt8.self) { outBuf in
      for n in 0..<toFetch {
        outBuf[n] = data[Int(addr) + n]
      }
    }
  }

}

internal struct ElfGNUCompressedImageSource: ImageSource {

  private var data: [UInt8]

  var isMappedImage: Bool { return false }
  var path: String? { return nil }
  var bounds: Bounds? { return Bounds(base: Address(0), size: Size(data.count)) }

  init(source: some ImageSource) throws {
    guard let bounds = source.bounds else {
      throw CompressedImageSourceError.unboundedImageSource
    }

    if bounds.size < 12 {
      throw CompressedImageSourceError.badCompressedData
    }

    let magic = try source.fetch(from: bounds.base, as: UInt32.self)
    if magic != 0x42494c5a {
      throw CompressedImageSourceError.badCompressedData
    }

    let uncompressedSize
      = UInt(try source.fetch(from: bounds.base + 4, as: UInt64.self).byteSwapped)

    data = try decompress(stream: ZLibStream(),
                          source: source,
                          dataBounds: bounds.adjusted(by: 12),
                          uncompressedSize: uncompressedSize)
  }

  public func fetch<T>(from addr: Address,
                       into buffer: UnsafeMutableBufferPointer<T>) throws {
    let toFetch = buffer.count * MemoryLayout<T>.stride
    if addr < 0 || addr > data.count || data.count - Int(addr) < toFetch {
      throw CompressedImageSourceError.outOfRangeFetch(addr, toFetch)
    }

    buffer.withMemoryRebound(to: UInt8.self) { outBuf in
      for n in 0..<toFetch {
        outBuf[n] = data[Int(addr) + n]
      }
    }
  }

}

internal struct LZMACompressedImageSource: ImageSource {

  private var data: [UInt8]

  var isMappedImage: Bool { return false }
  var path: String? { return nil }
  var bounds: Bounds? { return Bounds(base: Address(0), size: Size(data.count)) }

  init(source: some ImageSource) throws {
    // Only supported for bounded image sources
    guard let bounds = source.bounds else {
      throw CompressedImageSourceError.unboundedImageSource
    }

    data = try decompress(stream: LZMAStream(),
                          source: source,
                          dataBounds: bounds)
  }

  public func fetch<T>(from addr: Address,
                       into buffer: UnsafeMutableBufferPointer<T>) throws {
    let toFetch = buffer.count * MemoryLayout<T>.stride
    if addr < 0 || addr > data.count || data.count - Int(addr) < toFetch {
      throw CompressedImageSourceError.outOfRangeFetch(addr, toFetch)
    }

    buffer.withMemoryRebound(to: UInt8.self) { outBuf in
      for n in 0..<toFetch {
        outBuf[n] = data[Int(addr) + n]
      }
    }
  }

}

#endif // os(Linux)
