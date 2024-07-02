//===----------------------------------------------------------------------===//
//
// This source file is part of the Swift open source project
//
// Copyright (c) 2024 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See http://swift.org/LICENSE.txt for license information
// See http://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//

@_spi(PluginMessage) import SwiftCompilerPluginMessageHandling
@_spi(PluginMessage) import SwiftLibraryPluginProvider

#if canImport(Darwin)
import Darwin
#elseif canImport(Glibc)
import Glibc
#elseif canImport(Musl)
import Musl
#elseif canImport(ucrt)
import ucrt
#else
#error("'malloc' not found")
#endif

/// Entry point.
///
/// Compiler 'dlopen' this 'SwiftInProcPluginServer' library, and 'dlsym' this
/// function. When the compiler wants to use dylib plugins, it calls this
/// function with the same message as `swift-plugin-server`.
///
/// The caller must `free` the returned buffer
@_cdecl("swift_inproc_plugins_handle_message")
@MainActor
public func handleMessage(
  _ inputData: UnsafePointer<UInt8>!,
  _ inputLength: Int,
  _ outputData: UnsafeMutablePointer<UnsafeMutablePointer<UInt8>?>!,
  _ outputLength: UnsafeMutablePointer<Int>!
) -> Bool {
  do {
    let input = UnsafeBufferPointer(start: inputData, count: inputLength)
    let output = try InProcPluginServer.shared.handleMessage(input)
    output.withUnsafeBufferPointer(fillOutput(_:))
    return false // Success.
  } catch {
    var message = "Internal Error: \(error)"
    message.withUTF8(fillOutput(_:))
    return true // Error.
  }

  func fillOutput(_ responseData: UnsafeBufferPointer<UInt8>) {
    // NOTE: Use 'malloc' instead of 'UnsafeMutablePointer.allocate()' so that
    // C/C++ clients can deallocate it without using Swift.
    let buffer = malloc(responseData.count)!
    buffer.initializeMemory(
      as: UInt8.self,
      from: responseData.baseAddress!,
      count: responseData.count
    )
    outputData.pointee = buffer.assumingMemoryBound(to: UInt8.self)
    outputLength.pointee = responseData.count
  }
}

/// Singleton "plugin server".
struct InProcPluginServer {
  private let handler: CompilerPluginMessageHandler<LibraryPluginProvider>

  @MainActor
  private init() {
    self.handler = CompilerPluginMessageHandler(
      provider: LibraryPluginProvider.shared
    )
  }

  func handleMessage(_ input: UnsafeBufferPointer<UInt8>) throws -> [UInt8] {
    let request = try JSON.decode(HostToPluginMessage.self, from: input)
    let response =  handler.handleMessage(request)
    return try JSON.encode(response)
  }

  @MainActor
  static let shared = Self()
}

