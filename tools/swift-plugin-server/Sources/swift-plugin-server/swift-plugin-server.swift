//===----------------------------------------------------------------------===//
//
// This source file is part of the Swift open source project
//
// Copyright (c) 2023 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See http://swift.org/LICENSE.txt for license information
// See http://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//

@_spi(PluginMessage) import SwiftCompilerPluginMessageHandling
import SwiftSyntaxMacros
import swiftLLVMJSON
import CSwiftPluginServer

@main
final class SwiftPluginServer {
  struct MacroRef: Hashable {
    var moduleName: String
    var typeName: String
    init(_ moduleName: String, _ typeName: String) {
      self.moduleName = moduleName
      self.typeName = typeName
    }
  }

  struct LoadedLibraryPlugin {
    var libraryPath: String
    var handle: UnsafeMutableRawPointer
  }

  /// Loaded dylib handles associated with the module name.
  var loadedLibraryPlugins: [String: LoadedLibraryPlugin] = [:]

  /// Resolved cached macros.
  var resolvedMacros: [MacroRef: Macro.Type] = [:]

  /// @main entry point.
  static func main() throws {
    let connection = try PluginHostConnection()
    let messageHandler = CompilerPluginMessageHandler(
      connection: connection,
      provider: self.init()
    )
    try messageHandler.main()
  }
}

extension SwiftPluginServer: PluginProvider {
  /// Load a macro implementation from the dynamic link library.
  func loadPluginLibrary(libraryPath: String, moduleName: String) throws {
    var errorMessage: UnsafePointer<CChar>?
    guard let dlHandle = PluginServer_load(libraryPath, &errorMessage) else {
      throw PluginServerError(message: "loader error: " + String(cString: errorMessage!))
    }
    loadedLibraryPlugins[moduleName] = LoadedLibraryPlugin(
      libraryPath: libraryPath,
      handle: dlHandle
    )
  }

  /// Lookup a loaded macro by a pair of module name and type name.
  func resolveMacro(moduleName: String, typeName: String) throws -> Macro.Type {
    if let resolved = resolvedMacros[.init(moduleName, typeName)] {
      return resolved
    }

    // Find 'dlopen'ed library for the module name.
    guard let plugin = loadedLibraryPlugins[moduleName] else {
      // NOTE: This should be unreachable. Compiler should not use this server
      // unless the plugin loading succeeded.
      throw PluginServerError(message: "(plugin-server) plugin not loaded for module '\(moduleName)'")
    }

    // Lookup the type metadata.
    var errorMessage: UnsafePointer<CChar>?
    guard let macroTypePtr = PluginServer_lookupMacroTypeMetadataByExternalName(
      moduleName, typeName, plugin.handle, &errorMessage
    ) else {
      throw PluginServerError(message: "macro implementation type '\(moduleName).\(typeName)' could not be found in library plugin '\(plugin.libraryPath)'")
    }

    // THe type must be a 'Macro' type.
    let macroType = unsafeBitCast(macroTypePtr, to: Any.Type.self)
    guard let macro = macroType as? Macro.Type else {
      throw PluginServerError(message: "type '\(moduleName).\(typeName)' is not a valid macro implementation type in library plugin '\(plugin.libraryPath)'")
    }

    // Cache the resolved type.
    resolvedMacros[.init(moduleName, typeName)] = macro
    return macro
  }

  /// This 'PluginProvider' implements 'loadLibraryMacro()'.
  var features: [SwiftCompilerPluginMessageHandling.PluginFeature] {
    [.loadPluginLibrary]
  }
}

final class PluginHostConnection: MessageConnection {
  let handle: UnsafeRawPointer
  init() throws {
    var errorMessage: UnsafePointer<CChar>? = nil
    guard let handle = PluginServer_createConnection(&errorMessage) else {
      throw PluginServerError(message: String(cString: errorMessage!))
    }
    self.handle = handle
  }

  deinit {
    PluginServer_destroyConnection(self.handle)
  }

  func sendMessage<TX: Encodable>(_ message: TX) throws {
    try LLVMJSON.encoding(message) { buffer in
      try self.sendMessageData(buffer)
    }
  }

  func waitForNextMessage<RX: Decodable>(_ type: RX.Type) throws -> RX? {
    return try self.withReadingMessageData { jsonData in
      try LLVMJSON.decode(RX.self, from: jsonData)
    }
  }

  /// Send a serialized message to the message channel.
  private func sendMessageData(_ data: UnsafeBufferPointer<Int8>) throws {
    // Write the header (a 64-bit length field in little endian byte order).
    var header: UInt64 = UInt64(data.count).littleEndian
    let writtenSize = try Swift.withUnsafeBytes(of: &header) { buffer in
      try self.write(buffer: UnsafeRawBufferPointer(buffer))
    }
    guard writtenSize == MemoryLayout.size(ofValue: header) else {
      throw PluginServerError(message: "failed to write message header")
    }

    // Write the body.
    guard try self.write(buffer: UnsafeRawBufferPointer(data)) == data.count else {
      throw PluginServerError(message: "failed to write message body")
    }
  }

  /// Read a serialized message from the message channel and call the 'body'
  /// with the data.
  private func withReadingMessageData<R>(_ body: (UnsafeBufferPointer<Int8>) throws -> R) throws -> R? {
    // Read the header (a 64-bit length field in little endian byte order).
    var header: UInt64 = 0
    let readSize = try Swift.withUnsafeMutableBytes(of: &header) { buffer in
      try self.read(into: UnsafeMutableRawBufferPointer(buffer))
    }
    guard readSize == MemoryLayout.size(ofValue: header) else {
      if readSize == 0 {
        // The host closed the pipe.
        return nil
      }
      // Otherwise, some error happened.
      throw PluginServerError(message: "failed to read message header")
    }

    // Read the body.
    let count = Int(UInt64(littleEndian: header))
    let data = UnsafeMutableBufferPointer<Int8>.allocate(capacity: count)
    defer { data.deallocate() }
    guard try self.read(into: UnsafeMutableRawBufferPointer(data)) == count else {
      throw PluginServerError(message: "failed to read message body")
    }

    // Invoke the handler.
    return try body(UnsafeBufferPointer(data))
  }

  /// Write the 'buffer' to the message channel.
  /// Returns the number of bytes succeeded to write.
  private func write(buffer: UnsafeRawBufferPointer) throws -> Int {
    var bytesToWrite = buffer.count
    guard bytesToWrite > 0 else {
      return 0
    }
    var ptr = buffer.baseAddress!

    while (bytesToWrite > 0) {
      let writtenSize = PluginServer_write(handle, ptr, bytesToWrite)
      if (writtenSize <= 0) {
        // error e.g. broken pipe.
        break
      }
      ptr = ptr.advanced(by: writtenSize)
      bytesToWrite -= Int(writtenSize)
    }
    return buffer.count - bytesToWrite
  }

  /// Read data from the message channel into the 'buffer' up to 'buffer.count' bytes.
  /// Returns the number of bytes succeeded to read.
  private func read(into buffer: UnsafeMutableRawBufferPointer) throws -> Int {
    var bytesToRead = buffer.count
    guard bytesToRead > 0 else {
      return 0
    }
    var ptr = buffer.baseAddress!

    while bytesToRead > 0 {
      let readSize = PluginServer_read(handle, ptr, bytesToRead)
      if (readSize <= 0) {
        // 0: EOF (the host closed), -1: Broken pipe (the host crashed?)
        break;
      }
      ptr = ptr.advanced(by: readSize)
      bytesToRead -= readSize
    }
    return buffer.count - bytesToRead
  }
}

struct PluginServerError: Error, CustomStringConvertible {
  var description: String
  init(message: String) {
    self.description = message
  }
}
