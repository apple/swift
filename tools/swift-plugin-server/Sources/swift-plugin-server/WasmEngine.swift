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

import WASI
import WasmTypes
import SystemPackage

typealias WasmFunction = () throws -> Void

protocol WasmEngine {
  init(path: FilePath, imports: WASIBridgeToHost) throws

  func function(named name: String) throws -> WasmFunction?
}

typealias DefaultWasmPlugin = WasmEnginePlugin<DefaultWasmEngine>

// a WasmPlugin implementation that delegates to a WasmEngine
struct WasmEnginePlugin<Engine: WasmEngine>: WasmPlugin {
  private let hostToPlugin: FileDescriptor
  private let pluginToHost: FileDescriptor
  private let pumpFunction: WasmFunction
  let engine: Engine

  init(path: FilePath) throws {
    let hostToPluginPipes = try FileDescriptor.pipe()
    let pluginToHostPipes = try FileDescriptor.pipe()
    self.hostToPlugin = hostToPluginPipes.writeEnd
    self.pluginToHost = pluginToHostPipes.readEnd

    let bridge = try WASIBridgeToHost(
      stdin: hostToPluginPipes.readEnd,
      stdout: pluginToHostPipes.writeEnd,
      stderr: .standardError
    )
    engine = try Engine(path: path, imports: bridge)

    let exportName = "swift_wasm_macro_v1_pump"
    guard let pump = try engine.function(named: exportName) else {
      throw WasmEngineError(message: "Wasm plugin has an unknown ABI (could not find '\(exportName)')")
    }
    self.pumpFunction = pump

    guard let start = try engine.function(named: "_start") else {
      throw WasmEngineError(message: "Wasm plugin does not have a '_start' entrypoint")
    }
    try start()
  }

  func handleMessage(_ json: [UInt8]) throws -> [UInt8] {
    try withUnsafeBytes(of: UInt64(json.count).littleEndian) {
      _ = try hostToPlugin.writeAll($0)
    }
    try hostToPlugin.writeAll(json)

    try pumpFunction()

    let lengthRaw = try withUnsafeTemporaryAllocation(of: UInt8.self, capacity: 8) { buffer in
      let lengthCount = try pluginToHost.read(into: UnsafeMutableRawBufferPointer(buffer))
      guard lengthCount == 8 else {
        throw WasmEngineError(message: "Wasm plugin sent invalid response")
      }
      return buffer.withMemoryRebound(to: UInt64.self, \.baseAddress!.pointee)
    }
    let length = Int(UInt64(littleEndian: lengthRaw))
    return try [UInt8](unsafeUninitializedCapacity: length) { buffer, size in
      let received = try pluginToHost.read(into: UnsafeMutableRawBufferPointer(buffer))
      guard received == length else {
        throw WasmEngineError(message: "Wasm plugin sent truncated response")
      }
      size = received
    }
  }
}

struct WasmEngineError: Error, CustomStringConvertible {
  let description: String

  init(message: String) {
    self.description = message
  }
}
