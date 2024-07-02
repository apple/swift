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
import WasmKit
import WasmKitWASI
import SystemPackage

typealias DefaultWasmEngine = WasmKitEngine

struct WasmKitEngine: WasmEngine {
  private let runtime: Runtime
  private let functions: [String: Function]

  init(path: FilePath, imports: WASIBridgeToHost) throws {
    runtime = Runtime(hostModules: imports.hostModules)

    let module = try parseWasm(filePath: path)
    let instance = try runtime.instantiate(module: module)
    functions = instance.exports.compactMapValues { export in
      guard case let .function(function) = export else { return nil }
      return function
    }
  }

  func function(named name: String) throws -> WasmFunction? {
    guard let function = functions[name] else { return nil }
    return { _ = try function.invoke(runtime: runtime) }
  }
}
