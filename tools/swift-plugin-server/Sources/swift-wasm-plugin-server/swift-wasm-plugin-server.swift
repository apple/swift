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
@_spi(PluginMessage) import SwiftPluginServerSupport
import Foundation
import swiftLLVMJSON

@main
final class SwiftPluginServer {
  let connection: PluginHostConnection
  var loadedWasmPlugins: [String: WasmPlugin] = [:]
  var hostCapability: PluginMessage.HostCapability?

  init(connection: PluginHostConnection) {
    self.connection = connection
  }

  private func sendMessage(_ message: PluginToHostMessage) throws {
    try connection.sendMessage(message)
  }

  private func loadPluginLibrary(path: String, moduleName: String) async throws -> WasmPlugin {
    guard path.hasSuffix(".wasm") else { throw PluginServerError(message: "swift-wasm-plugin-server can only load wasm") }
    let wasm = try Data(contentsOf: URL(fileURLWithPath: path))
    let plugin = try await defaultWasmPlugin.init(wasm: wasm)
    let abiVersion = try plugin.abiVersion()
    guard abiVersion == 1 else {
      throw PluginServerError(message: "Wasm plugin has unsupported ABI version: \(abiVersion)")
    }
    return plugin
  }

  private func expandMacro(
    moduleName module: String,
    message: HostToPluginMessage
  ) async throws {
    let response: PluginToHostMessage
    do {
      guard let plugin = loadedWasmPlugins[module] else { throw PluginServerError(message: "Could not find module \(module)") }
      let request = try LLVMJSON.encoding(message) { Data(UnsafeRawBufferPointer($0)) }
      let responseRaw = try await plugin.handleMessage(request)
      response = try responseRaw.withUnsafeBytes {
        try $0.withMemoryRebound(to: Int8.self) {
          try LLVMJSON.decode(PluginToHostMessage.self, from: $0)
        }
      }
    } catch {
      try sendMessage(.expandMacroResult(expandedSource: nil, diagnostics: []))
      return
    }
    try sendMessage(response)
  }

  func run() async throws {
    while let message = try connection.waitForNextMessage(HostToPluginMessage.self) {
      switch message {
      case .getCapability(let hostCapability):
        // Remember the peer capability if provided.
        if let hostCapability = hostCapability {
          self.hostCapability = .init(hostCapability)
        }

        // Return the plugin capability.
        let capability = PluginMessage.PluginCapability(
          protocolVersion: PluginMessage.PROTOCOL_VERSION_NUMBER,
          features: [PluginFeature.loadPluginLibrary.rawValue]
        )
        try sendMessage(.getCapabilityResult(capability: capability))
      case .loadPluginLibrary(let libraryPath, let moduleName):
        do {
          loadedWasmPlugins[moduleName] = try await loadPluginLibrary(path: libraryPath, moduleName: moduleName)
        } catch {
          try? FileHandle.standardError.write(contentsOf: Data("Error: \(error)\n".utf8))
          try sendMessage(.loadPluginLibraryResult(loaded: false, diagnostics: []))
          continue
        }
        try sendMessage(.loadPluginLibraryResult(loaded: true, diagnostics: []))
      case .expandAttachedMacro(let macro, _, _, _, _, _, _, _, _),
           .expandFreestandingMacro(let macro, _, _, _, _):
        try await expandMacro(moduleName: macro.moduleName, message: message)
      #if !SWIFT_PACKAGE
      @unknown default:
        break
      #endif
      }
    }
  }

  /// @main entry point.
  static func main() async throws {
    let connection = try PluginHostConnection()
    try await Self(connection: connection).run()
  }
}

protocol WasmPlugin {
  init(wasm: Data) async throws

  func abiVersion() throws -> UInt32
  func handleMessage(_ json: Data) async throws -> Data
}

private var defaultWasmPlugin: (some WasmPlugin).Type { DefaultWasmPlugin.self }
