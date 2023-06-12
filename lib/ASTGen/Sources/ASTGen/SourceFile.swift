import SwiftDiagnostics
import SwiftParser
import SwiftSyntax
import SwiftParserDiagnostics

/// Describes a source file that has been "exported" to the C++ part of the
/// compiler, with enough information to interface with the C++ layer.
struct ExportedSourceFile {
  /// The underlying buffer within the C++ SourceManager, which is used
  /// for computations of source locations.
  let buffer: UnsafeBufferPointer<UInt8>

  /// The name of the enclosing module.
  let moduleName: String

  /// The name of the source file being parsed.
  let fileName: String

  /// The syntax tree for the complete source file.
  let syntax: SourceFileSyntax
}

/// Parses the given source file and produces a pointer to a new
/// ExportedSourceFile instance.
@_cdecl("swift_ASTGen_parseSourceFile")
public func parseSourceFile(
  buffer: UnsafePointer<UInt8>, bufferLength: Int,
  moduleName: UnsafePointer<UInt8>, filename: UnsafePointer<UInt8>
) -> UnsafeRawPointer {
  let buffer = UnsafeBufferPointer(start: buffer, count: bufferLength)
  let sourceFile = Parser.parse(source: buffer)

  let exportedPtr = UnsafeMutablePointer<ExportedSourceFile>.allocate(capacity: 1)
  exportedPtr.initialize(
    to: .init(
      buffer: buffer, moduleName: String(cString: moduleName),
      fileName: String(cString: filename), syntax: sourceFile)
  )

  return UnsafeRawPointer(exportedPtr)
}

/// Deallocate a parsed source file.
@_cdecl("swift_ASTGen_destroySourceFile")
public func destroySourceFile(
  sourceFilePtr: UnsafeMutablePointer<UInt8>
) {
  sourceFilePtr.withMemoryRebound(to: ExportedSourceFile.self, capacity: 1) { sourceFile in
    sourceFile.deinitialize(count: 1)
    sourceFile.deallocate()
  }
}

/// Check for whether the given source file round-trips
@_cdecl("swift_ASTGen_roundTripCheck")
public func roundTripCheck(
  sourceFilePtr: UnsafeMutablePointer<UInt8>
) -> CInt {
  sourceFilePtr.withMemoryRebound(to: ExportedSourceFile.self, capacity: 1) { sourceFile in
    let sf = sourceFile.pointee
    return sf.syntax.syntaxTextBytes.elementsEqual(sf.buffer) ? 0 : 1
  }
}

extension Syntax {
  /// Whether this syntax node is or is enclosed within a #if.
  fileprivate var isInIfConfig: Bool {
    if self.is(IfConfigDeclSyntax.self) {
      return true
    }

    return parent?.isInIfConfig ?? false
  }
}

/// Emit diagnostics within the given source file.
@_cdecl("swift_ASTGen_emitParserDiagnostics")
public func emitParserDiagnostics(
  diagEnginePtr: UnsafeMutablePointer<UInt8>,
  sourceFilePtr: UnsafeMutablePointer<UInt8>,
  emitOnlyErrors: CInt,
  downgradePlaceholderErrorsToWarnings: CInt
) -> CInt {
  return sourceFilePtr.withMemoryRebound(
    to: ExportedSourceFile.self, capacity: 1
  ) { sourceFile in
    var anyDiags = false

    let diags = ParseDiagnosticsGenerator.diagnostics(
      for: sourceFile.pointee.syntax
    )
    for diag in diags {
      // Skip over diagnostics within #if, because we don't know whether
      // we are in an active region or not.
      // FIXME: This heuristic could be improved.
      if diag.node.isInIfConfig {
        continue
      }

      let diagnosticSeverity: DiagnosticSeverity
      if downgradePlaceholderErrorsToWarnings == 1 && diag.diagMessage.diagnosticID == StaticTokenError.editorPlaceholder.diagnosticID {
        diagnosticSeverity = .warning
      } else {
        diagnosticSeverity = diag.diagMessage.severity
      }

      if emitOnlyErrors != 0, diagnosticSeverity != .error {
        continue
      }

      emitDiagnostic(
        diagEnginePtr: diagEnginePtr,
        sourceFileBuffer: sourceFile.pointee.buffer,
        diagnostic: diag,
        diagnosticSeverity: diagnosticSeverity
      )
      anyDiags = true
    }

    return anyDiags ? 1 : 0
  }
}
