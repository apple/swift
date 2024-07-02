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

import ASTBridging
import BasicBridging
@_spi(PluginMessage) import SwiftCompilerPluginMessageHandling
import SwiftSyntax

enum PluginError: String, Error, CustomStringConvertible {
  case stalePlugin = "plugin is stale"
  case failedToSendMessage = "failed to send request to plugin"
  case failedToReceiveMessage = "failed to receive result from plugin"
  case invalidReponseKind = "plugin returned invalid result"

  var description: String { rawValue }
}

@_cdecl("swift_ASTGen_initializePlugin")
public func _initializePlugin(
  opaqueHandle: UnsafeMutableRawPointer,
  cxxDiagnosticEngine: UnsafeMutableRawPointer?
) -> Bool {
  let plugin = CompilerPlugin(opaqueHandle: opaqueHandle)

  do {
    try plugin.initialize()
    return true
  } catch {
    // Don't care the actual error. Probably the plugin is completely broken.
    // The failure is diagnosed in the caller.
    return false
  }
}

@_cdecl("swift_ASTGen_deinitializePlugin")
public func _deinitializePlugin(
  opaqueHandle: UnsafeMutableRawPointer
) {
  let plugin = CompilerPlugin(opaqueHandle: opaqueHandle)
  plugin.deinitialize()
}

/// Load the library plugin in the plugin server.
/// This should be called inside lock.
@_cdecl("swift_ASTGen_pluginServerLoadLibraryPlugin")
func swift_ASTGen_pluginServerLoadLibraryPlugin(
  opaqueHandle: UnsafeMutableRawPointer,
  libraryPath: UnsafePointer<CChar>,
  moduleName: UnsafePointer<CChar>,
  errorOut: UnsafeMutablePointer<BridgedStringRef>?
) -> Bool {
  let plugin = CompilerPlugin(opaqueHandle: opaqueHandle)

  if plugin.capability?.features.contains(.loadPluginLibrary) != true {
    errorOut?.pointee = allocateBridgedString("compiler plugin not loaded: '\(libraryPath); invalid plugin server")
    return false
  }
  assert(plugin.capability?.features.contains(.loadPluginLibrary) == true)
  let libraryPath = String(cString: libraryPath)
  let moduleName = String(cString: moduleName)

  do {
    let result = try plugin.sendMessageAndWaitWithoutLock(
      .loadPluginLibrary(libraryPath: libraryPath, moduleName: moduleName)
    )
    guard case .loadPluginLibraryResult(let loaded, let diagnostics) = result else {
      throw PluginError.invalidReponseKind
    }
    if loaded {
      assert(diagnostics.isEmpty)
      return true
    }
    let errorMsgs = diagnostics.map({ $0.message }).joined(separator: ", ");
    errorOut?.pointee = allocateBridgedString(errorMsgs);
    return false
  } catch {
    errorOut?.pointee = allocateBridgedString("\(error)")
    return false
  }
}

struct CompilerPlugin {
  struct Capability {
    enum Feature: String {
      case loadPluginLibrary = "load-plugin-library"
    }

    var protocolVersion: Int
    var features: Set<Feature>

    init(_ message: PluginMessage.PluginCapability) {
      self.protocolVersion = message.protocolVersion
      if let features = message.features {
        self.features = Set(features.compactMap(Feature.init(rawValue:)))
      } else {
        self.features = []
      }
    }
  }

  let opaqueHandle: UnsafeMutableRawPointer

  private func withLock<R>(_ body: () throws -> R) rethrows -> R {
    Plugin_lock(opaqueHandle)
    defer { Plugin_unlock(opaqueHandle) }
    return try body()
  }

  private func sendMessage(_ message: HostToPluginMessage) throws {
    let hadError = try JSON.encode(message).withUnsafeBufferPointer { (data) -> Bool in
      return Plugin_sendMessage(opaqueHandle, BridgedData(baseAddress: data.baseAddress, count: data.count))
    }
    if hadError {
      throw PluginError.failedToSendMessage
    }
  }

  private func waitForNextMessage() throws -> PluginToHostMessage {
    var result = BridgedData()
    let hadError = Plugin_waitForNextMessage(opaqueHandle, &result)
    defer { result.free() }
    guard !hadError else {
      throw PluginError.failedToReceiveMessage
    }
    let data = UnsafeBufferPointer(start: result.baseAddress, count: result.count)
    return try data.withMemoryRebound(to: UInt8.self) { buffer in
      try JSON.decode(PluginToHostMessage.self, from: buffer)
    }
  }

  func sendMessageAndWaitWithoutLock(_ message: HostToPluginMessage) throws -> PluginToHostMessage {
    guard !Plugin_spawnIfNeeded(opaqueHandle) else {
      throw PluginError.stalePlugin
    }
    try sendMessage(message)
    return try waitForNextMessage()
  }

  func sendMessageAndWait(_ message: HostToPluginMessage) throws -> PluginToHostMessage {
    try self.withLock {
      return try sendMessageAndWaitWithoutLock(message);
    }
  }

  /// Initialize the plugin. This should be called inside lock.
  func initialize() throws {
    // Send host capability and get plugin capability.
    let hostCapability = PluginMessage.HostCapability(
      protocolVersion: PluginMessage.PROTOCOL_VERSION_NUMBER
    )
    let request = HostToPluginMessage.getCapability(capability: hostCapability)
    let response = try self.sendMessageAndWaitWithoutLock(request)
    guard case .getCapabilityResult(let capability) = response else {
      throw PluginError.invalidReponseKind
    }

    deinitializePluginCapabilityIfExist()

    let ptr = UnsafeMutablePointer<Capability>.allocate(capacity: 1)
    ptr.initialize(to: .init(capability))
    Plugin_setCapability(opaqueHandle, UnsafeRawPointer(ptr))
  }

  /// Deinitialize and unset the plugin capability stored in C++
  /// 'LoadedExecutablePlugin'. This should be called inside lock.
  func deinitializePluginCapabilityIfExist() {
    if let ptr = Plugin_getCapability(opaqueHandle) {
      let capabilityPtr = UnsafeMutableRawPointer(mutating: ptr)
        .assumingMemoryBound(to: PluginMessage.PluginCapability.self)
      capabilityPtr.deinitialize(count: 1)
      capabilityPtr.deallocate()
      Plugin_setCapability(opaqueHandle, nil)
    }
  }

  func deinitialize() {
    self.withLock {
      deinitializePluginCapabilityIfExist()
    }
  }

  var capability: Capability? {
    if let ptr = Plugin_getCapability(opaqueHandle) {
      return ptr.assumingMemoryBound(to: Capability.self).pointee
    }
    return nil
  }
}

class PluginDiagnosticsEngine {
  private let bridgedDiagEngine: BridgedDiagnosticEngine
  private var exportedSourceFileByName: [String: UnsafePointer<ExportedSourceFile>] = [:]

  init(cxxDiagnosticEngine: UnsafeMutableRawPointer) {
    self.bridgedDiagEngine = BridgedDiagnosticEngine(raw: cxxDiagnosticEngine)
  }

  /// Failable convenience initializer for optional cxx engine pointer.
  convenience init?(cxxDiagnosticEngine: UnsafeMutableRawPointer?) {
    guard let cxxDiagnosticEngine = cxxDiagnosticEngine else {
      return nil
    }
    self.init(cxxDiagnosticEngine: cxxDiagnosticEngine)
  }

  /// Register an 'ExportedSourceFile' to the engine. So the engine can get
  /// C++ SourceLoc from a pair of filename and offset.
  func add(exportedSourceFile: UnsafePointer<ExportedSourceFile>) {
    exportedSourceFileByName[exportedSourceFile.pointee.fileName] = exportedSourceFile
  }

  /// Emit a diagnostic to C++ diagnostic engine. Note that one plugin
  /// diagnostic can emits multiple C++ diagnostics.
  func emit(
    _ diagnostic: PluginMessage.Diagnostic,
    messageSuffix: String? = nil
  ) {

    // Emit the main diagnostic.
    emitSingle(
      message: diagnostic.message + (messageSuffix ?? ""),
      severity: diagnostic.severity,
      position: diagnostic.position,
      highlights: diagnostic.highlights
    )

    // Emit Fix-Its.
    for fixIt in diagnostic.fixIts {
      emitSingle(
        message: fixIt.message,
        severity: .note,
        position: diagnostic.position,
        fixItChanges: fixIt.changes
      )
    }

    // Emit any notes as follow-ons.
    for note in diagnostic.notes {
      emitSingle(
        message: note.message,
        severity: .note,
        position: note.position
      )
    }
  }
  /// Emit single C++ diagnostic.
  private func emitSingle(
    message: String,
    severity: PluginMessage.Diagnostic.Severity,
    position: PluginMessage.Diagnostic.Position,
    highlights: [PluginMessage.Diagnostic.PositionRange] = [],
    fixItChanges: [PluginMessage.Diagnostic.FixIt.Change] = []
  ) {
    // Map severity
    let bridgedSeverity: BridgedDiagnosticSeverity
    switch severity {
    case .error: bridgedSeverity = .error
    case .note: bridgedSeverity = .note
    case .warning: bridgedSeverity = .warning
    case .remark: bridgedSeverity = .remark
#if RESILIENT_SWIFT_SYNTAX
    @unknown default: bridgedSeverity = .error
#endif
    }

    // Emit the diagnostic
    var mutableMessage = message
    let diag = mutableMessage.withBridgedString { bridgedMessage in
      BridgedDiagnostic(
        at: bridgedSourceLoc(at: position),
        message: bridgedMessage,
        severity: bridgedSeverity,
        engine: bridgedDiagEngine
      )
    }

    // Emit highlights
    for highlight in highlights {
      guard let (startLoc, endLoc) = bridgedSourceRange(for: highlight) else {
        continue
      }
      diag.highlight(start: startLoc, end: endLoc)
    }

    // Emit changes for a Fix-It.
    for change in fixItChanges {
      guard let (startLoc, endLoc) = bridgedSourceRange(for: change.range) else {
        continue
      }
      var newText = change.newText
      newText.withBridgedString { bridgedFixItText in
        diag.fixItReplace(
          start: startLoc,
          end: endLoc,
          replacement: bridgedFixItText
        )
      }
    }

    diag.finish()
  }

  /// Emit diagnostics.
  func emit(
    _ diagnostics: [PluginMessage.Diagnostic],
    messageSuffix: String? = nil
  ) {
    for diagnostic in diagnostics {
      self.emit(diagnostic, messageSuffix: messageSuffix)
    }
  }

  func diagnose(error: Error) {
    self.emitSingle(
      message: String(describing: error),
      severity: .error,
      position: .invalid
    )
  }

  func diagnose(message: String, severity: PluginMessage.Diagnostic.Severity) {
    self.emitSingle(message: message, severity: severity, position: .invalid)
  }

  /// Produce the C++ source location for a given position based on a
  /// syntax node.
  private func bridgedSourceLoc(
    at offset: Int,
    in fileName: String
  ) -> BridgedSourceLoc {
    // Find the corresponding exported source file.
    guard let exportedSourceFile = exportedSourceFileByName[fileName] else {
      return nil
    }

    // Compute the resulting address.
    guard let bufferBaseAddress = exportedSourceFile.pointee.buffer.baseAddress else {
      return nil
    }
    // Ensure 'offset' is within the buffer.
    guard offset <= exportedSourceFile.pointee.buffer.count else {
      return nil
    }
    return BridgedSourceLoc(raw: bufferBaseAddress).advanced(by: offset)
  }

  /// C++ source location from a position value from a plugin.
  private func bridgedSourceLoc(
    at position: PluginMessage.Diagnostic.Position
  ) -> BridgedSourceLoc {
    return bridgedSourceLoc(at: position.offset, in: position.fileName)
  }

  /// C++ source range from a range value from a plugin.
  private func bridgedSourceRange(
    for range: PluginMessage.Diagnostic.PositionRange
  ) -> (start: BridgedSourceLoc, end: BridgedSourceLoc)? {
    let start = bridgedSourceLoc(at: range.startOffset, in: range.fileName)
    let end = bridgedSourceLoc(at: range.endOffset, in: range.fileName)

    if !start.isValid || !end.isValid {
      return nil
    }
    return (start: start, end: end)
  }
}

extension String {
  /// Retrieve the base name of a string that represents a path, removing the
  /// directory.
  var basename: String {
    guard
      let lastSlash = lastIndex(where: {
        #if os(iOS) || os(macOS) || os(tvOS) || os(watchOS) || os(visionOS) || os(Android) || os(Linux)
        ["/"].contains($0)
        #else
        ["/", "\\"].contains($0)
        #endif
      })
    else {
      return self
    }

    return String(self[index(after: lastSlash)...])
  }
}

extension PluginMessage.Syntax {
  init?(syntax: Syntax, in sourceFilePtr: UnsafePointer<ExportedSourceFile>) {
    let kind: PluginMessage.Syntax.Kind
    switch true {
    case syntax.is(DeclSyntax.self): kind = .declaration
    case syntax.is(ExprSyntax.self): kind = .expression
    case syntax.is(StmtSyntax.self): kind = .statement
    case syntax.is(TypeSyntax.self): kind = .type
    case syntax.is(PatternSyntax.self): kind = .pattern
    case syntax.is(AttributeSyntax.self): kind = .attribute
    default: return nil
    }

    let source = syntax.description
    let fileName = sourceFilePtr.pointee.fileName
    let fileID = "\(sourceFilePtr.pointee.moduleName)/\(sourceFilePtr.pointee.fileName.basename)"
    let loc = sourceFilePtr.pointee.sourceLocationConverter.location(for: syntax.position)

    self.init(
      kind: kind,
      source: source,
      location: .init(
        fileID: fileID,
        fileName: fileName,
        offset: loc.offset,
        line: loc.line,
        column: loc.column
      )
    )
  }

  init?(syntax: Syntax) {
    let kind: PluginMessage.Syntax.Kind
    switch true {
    case syntax.is(DeclSyntax.self): kind = .declaration
    case syntax.is(ExprSyntax.self): kind = .expression
    case syntax.is(StmtSyntax.self): kind = .statement
    case syntax.is(TypeSyntax.self): kind = .type
    case syntax.is(PatternSyntax.self): kind = .pattern
    case syntax.is(AttributeSyntax.self): kind = .attribute
    default: return nil
    }

    let source = syntax.description

    self.init(
      kind: kind,
      source: source,
      location: .init(
        fileID: "",
        fileName: "",
        offset: 0,
        line: 0,
        column: 0
      )
    )
  }
}
