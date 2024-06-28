//===----------------------------------------------------------------------===//
//
// This source file is part of the Swift open source project
//
// Copyright (c) 2022-2023 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See http://swift.org/LICENSE.txt for license information
// See http://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//

import ASTBridging
import BasicBridging
@_spi(PluginMessage) @_spi(ExperimentalLanguageFeature) import SwiftCompilerPluginMessageHandling
import SwiftDiagnostics
import SwiftOperators
import SwiftParser
import SwiftSyntax
import SwiftSyntaxBuilder
@_spi(ExperimentalLanguageFeature) @_spi(Compiler) import SwiftSyntaxMacroExpansion

struct ExportedExternalMacro {
  var moduleName: String
  var typeName: String
  var plugin: CompilerPlugin
}

extension MacroRole {
  init(rawMacroRole: UInt8) {
    switch rawMacroRole {
    case 0: self = .expression
    case 1: self = .declaration
    case 2: self = .accessor
    case 3: self = .memberAttribute
    case 4: self = .member
    case 5: self = .peer
    case 6: self = .conformance
    case 7: self = .codeItem
    case 8: self = .`extension`
    case 9: self = .preamble
    case 10: self = .body

    default: fatalError("unknown macro role")
    }
  }
}

@_cdecl("swift_ASTGen_resolveExternalMacro")
public func resolveExternalMacro(
  moduleName: UnsafePointer<CChar>,
  typeName: UnsafePointer<CChar>,
  pluginOpaqueHandle: UnsafeMutableRawPointer
) -> UnsafeRawPointer {
  // NOTE: This doesn't actually resolve anything.
  // Executable plugins is "trusted" to have the macro implementation. If not,
  // the actual expansion fails.
  let exportedPtr = UnsafeMutablePointer<ExportedExternalMacro>.allocate(capacity: 1)
  exportedPtr.initialize(
    to: .init(
      moduleName: String(cString: moduleName),
      typeName: String(cString: typeName),
      plugin: CompilerPlugin(opaqueHandle: pluginOpaqueHandle)
    )
  )
  return UnsafeRawPointer(exportedPtr)
}

@_cdecl("swift_ASTGen_destroyExternalMacro")
public func destroyExternalMacro(
  macroPtr: UnsafeMutableRawPointer
) {
  let macroPtr = macroPtr.assumingMemoryBound(to: ExportedExternalMacro.self)
  macroPtr.deinitialize(count: 1)
  macroPtr.deallocate()
}

/// Diagnostics produced here.
enum ASTGenMacroDiagnostic: DiagnosticMessage, FixItMessage {
  case thrownError(Error)
  case oldStyleExternalMacro
  case useExternalMacro
  case unknownBuiltin(String)
  case notStringLiteralArgument(String)

  var message: String {
    switch self {
    case .thrownError(let error):
      return String(describing: error)

    case .oldStyleExternalMacro:
      return "external macro definitions are now written using #externalMacro"

    case .useExternalMacro:
      return "use '#externalMacro'"

    case .unknownBuiltin(let type):
      return "ignoring definition of unknown builtin macro \(type)"

    case .notStringLiteralArgument(let kind):
      return "argument to `#externalMacro` must be a string literal naming the external macro's \(kind)"
    }
  }

  var severity: DiagnosticSeverity {
    switch self {
    case .thrownError, .notStringLiteralArgument:
      return .error

    case .oldStyleExternalMacro, .unknownBuiltin:
      return .warning

    case .useExternalMacro:
      return .note
    }
  }

  var diagnosticID: MessageID {
    .init(domain: "Swift", id: "\(self)")
  }

  var fixItID: MessageID { diagnosticID }
}

/// Treat the given expression as a string literal, which should contain a
/// single identifier.
fileprivate func identifierFromStringLiteral(_ node: ExprSyntax) -> String? {
  guard let stringLiteral = node.as(StringLiteralExprSyntax.self),
    stringLiteral.segments.count == 1,
    let segment = stringLiteral.segments.first,
    case .stringSegment(let stringSegment) = segment
  else {
    return nil
  }

  return stringSegment.content.text
}

/// Checks if the macro expression used as an default argument has any issues.
///
/// - Returns: `true` if all restrictions are satisfied, `false` if diagnostics
/// are emitted.
@_cdecl("swift_ASTGen_checkDefaultArgumentMacroExpression")
func checkDefaultArgumentMacroExpression(
  diagEnginePtr: UnsafeMutableRawPointer,
  sourceFilePtr: UnsafeRawPointer,
  macroLocationPtr: UnsafePointer<UInt8>
) -> Bool {
  let sourceFilePtr = sourceFilePtr.bindMemory(to: ExportedSourceFile.self, capacity: 1)

  // Find the macro expression.
  guard
    let macroExpr = findSyntaxNodeInSourceFile(
      sourceFilePtr: sourceFilePtr,
      sourceLocationPtr: macroLocationPtr,
      type: MacroExpansionExprSyntax.self
    )
  else {
    // FIXME: Produce an error
    return false
  }

  do {
    try macroExpr.checkDefaultArgumentMacroExpression()
    return true
  } catch let errDiags as DiagnosticsError {
    let srcMgr = SourceManager(cxxDiagnosticEngine: diagEnginePtr)
    srcMgr.insert(sourceFilePtr)
    for diag in errDiags.diagnostics {
      srcMgr.diagnose(diagnostic: diag)
    }
    return false
  } catch let error {
    let srcMgr = SourceManager(cxxDiagnosticEngine: diagEnginePtr)
    srcMgr.insert(sourceFilePtr)
    srcMgr.diagnose(
      diagnostic: .init(
        node: macroExpr,
        message: ASTGenMacroDiagnostic.thrownError(error)
      )
    )
    return false
  }
}

/// Check a macro definition, producing a description of that macro definition
/// for use in macro expansion.
///
/// When the resulting macro requires expansion, the result will come in
/// two parts:
///
/// - Returns: -1 on failure, BridgedMacroDefinitionKind on success. When the
/// successful result is "expanded macro", `replacementsPtr` will point to a
/// number of "replacements" to perform when expanding that macro. Each
/// replacement is a textual replacement of use of a macro parameter with the
/// source text of the corresponding argument, and is represented as a triple
/// (start offset, end offset, parameter index): the [start offset, end offset)
/// range in the macro expansion expression should be replaced with the
/// argument matching the corresponding parameter.
@_cdecl("swift_ASTGen_checkMacroDefinition")
func checkMacroDefinition(
  diagEnginePtr: UnsafeMutableRawPointer,
  sourceFileBuffer: BridgedStringRef,
  macroDeclText: BridgedStringRef,
  externalMacroOutPtr: UnsafeMutablePointer<BridgedStringRef>,
  replacementsPtr: UnsafeMutablePointer<UnsafeMutablePointer<Int>?>,
  numReplacementsPtr: UnsafeMutablePointer<Int>,
  genericReplacementsPtr: UnsafeMutablePointer<UnsafeMutablePointer<Int>?>,
  numGenericReplacementsPtr: UnsafeMutablePointer<Int>
) -> Int {
  // Assert "out" parameters are initialized.
  assert(externalMacroOutPtr.pointee.isEmptyInitialized)
  assert(replacementsPtr.pointee == nil && numReplacementsPtr.pointee == 0)

  // Parse 'macro' decl.
  // FIXME: Use 'ExportedSourceFile' when C++ parser is replaced.
  let textBuffer = UnsafeBufferPointer<UInt8>(start: macroDeclText.data, count: macroDeclText.count)
  var parser = Parser(textBuffer)
  guard let macroDecl = DeclSyntax.parse(from: &parser).as(MacroDeclSyntax.self) else {
    // FIXME: Produce an error
    return -1
  }

  func diagnose(diagnostic: Diagnostic) {
    emitDiagnostic(
      diagnosticEngine: BridgedDiagnosticEngine(raw: diagEnginePtr),
      sourceFileBuffer: UnsafeBufferPointer(start: sourceFileBuffer.data, count: sourceFileBuffer.count),
      sourceFileBufferOffset: macroDeclText.data! - sourceFileBuffer.data!,
      diagnostic: diagnostic,
      diagnosticSeverity: diagnostic.diagMessage.severity
    )
  }

  // Check the definition
  do {
    let definition = try macroDecl.checkDefinition()
    switch definition {
    case let .deprecatedExternal(node: node, module: module, type: type):
      // Check for known builtins.
      if module == "Builtin" {
        switch type {
        case "ExternalMacro":
          return Int(BridgedMacroDefinitionKind.builtinExternalMacro.rawValue)

        case "IsolationMacro":
          return Int(BridgedMacroDefinitionKind.builtinIsolationMacro.rawValue)

        // These builtins don't exist, but are put into the standard library at
        // least for documentation purposes right now. Don't emit a warning for
        // them, but do fail operation.
        case "FileIDMacro",
          "FilePathMacro",
          "FileMacro",
          "FunctionMacro",
          "LineMacro",
          "ColumnMacro",
          "DSOHandleMacro",
          "WarningMacro",
          "ErrorMacro":
          return -1

        default:
          // Warn about the unknown builtin.
          diagnose(
            diagnostic: .init(
              node: node,
              message: ASTGenMacroDiagnostic.unknownBuiltin(type)
            )
          )

          return -1
        }
      }

      // Form the "ModuleName.TypeName" result string.
      externalMacroOutPtr.pointee =
        allocateBridgedString("\(module).\(type)")

      // Translate this into a use of #externalMacro.
      let expansionSourceSyntax: ExprSyntax =
        "#externalMacro(module: \(literal: module), type: \(literal: type))"

      // Warn about the use of old-style external macro syntax here.
      diagnose(
        diagnostic: .init(
          node: node,
          message: ASTGenMacroDiagnostic.oldStyleExternalMacro,
          fixIts: [
            FixIt(
              message: ASTGenMacroDiagnostic.useExternalMacro,
              changes: [
                FixIt.Change.replace(
                  oldNode: node,
                  newNode: Syntax(expansionSourceSyntax)
                )
              ]
            )
          ]
        )
      )
      return Int(BridgedMacroDefinitionKind.externalMacro.rawValue)

    case let .expansion(expansionSyntax, replacements: _, genericReplacements: _)
    where expansionSyntax.macroName.text == "externalMacro":
      // Extract the identifier from the "module" argument.
      guard let firstArg = expansionSyntax.arguments.first,
        let firstArgLabel = firstArg.label?.text,
        firstArgLabel == "module",
        let module = identifierFromStringLiteral(firstArg.expression)
      else {
        diagnose(
          diagnostic: .init(
            node: Syntax(expansionSyntax),
            message: ASTGenMacroDiagnostic.notStringLiteralArgument("module")
          )
        )
        return -1
      }

      // Extract the identifier from the "type" argument.
      guard let secondArg = expansionSyntax.arguments.dropFirst().first,
        let secondArgLabel = secondArg.label?.text,
        secondArgLabel == "type",
        let type = identifierFromStringLiteral(secondArg.expression)
      else {
        diagnose(
          diagnostic: .init(
            node: Syntax(expansionSyntax),
            message: ASTGenMacroDiagnostic.notStringLiteralArgument("type")
          )
        )
        return -1
      }

      // Form the "ModuleName.TypeName" result string.
      externalMacroOutPtr.pointee =
        allocateBridgedString("\(module).\(type)")
      return Int(BridgedMacroDefinitionKind.externalMacro.rawValue)

    case let .expansion(expansionSyntax,
      replacements: replacements, genericReplacements: genericReplacements):
      // Provide the expansion syntax.
      externalMacroOutPtr.pointee =
        allocateBridgedString(expansionSyntax.trimmedDescription)

      // If there are no replacements, we're done.
      let totalReplacementsCount = replacements.count + genericReplacements.count
      guard totalReplacementsCount > 0 else {
        return Int(BridgedMacroDefinitionKind.expandedMacro.rawValue)
      }

      // The replacements are triples: (startOffset, endOffset, parameter index).
      let replacementBuffer = UnsafeMutableBufferPointer<Int>.allocate(capacity: 3 * replacements.count)
      for (index, replacement) in replacements.enumerated() {
        let expansionStart = expansionSyntax.positionAfterSkippingLeadingTrivia.utf8Offset

        replacementBuffer[index * 3] =
          replacement.reference.positionAfterSkippingLeadingTrivia.utf8Offset - expansionStart
        replacementBuffer[index * 3 + 1] =
          replacement.reference.endPositionBeforeTrailingTrivia.utf8Offset - expansionStart
        replacementBuffer[index * 3 + 2] = replacement.parameterIndex
      }
      replacementsPtr.pointee = replacementBuffer.baseAddress
      numReplacementsPtr.pointee = replacements.count

      // The replacements are triples: (startOffset, endOffset, parameter index).
      let genericReplacementBuffer = UnsafeMutableBufferPointer<Int>.allocate(capacity: 3 * genericReplacements.count)
      for (index, genericReplacement) in genericReplacements.enumerated() {
        let expansionStart = expansionSyntax.positionAfterSkippingLeadingTrivia.utf8Offset

        genericReplacementBuffer[index * 3] =
          genericReplacement.reference.positionAfterSkippingLeadingTrivia.utf8Offset - expansionStart
        genericReplacementBuffer[index * 3 + 1] =
          genericReplacement.reference.endPositionBeforeTrailingTrivia.utf8Offset - expansionStart
        genericReplacementBuffer[index * 3 + 2] =
          genericReplacement.parameterIndex
      }
      genericReplacementsPtr.pointee = genericReplacementBuffer.baseAddress
      numGenericReplacementsPtr.pointee = genericReplacements.count

      return Int(BridgedMacroDefinitionKind.expandedMacro.rawValue)
#if RESILIENT_SWIFT_SYNTAX
    @unknown default:
      fatalError()
#endif
    }
  } catch let errDiags as DiagnosticsError {
    for diag in errDiags.diagnostics {
      diagnose(diagnostic: diag)
    }
    return -1
  } catch let error {
    diagnose(
      diagnostic: .init(
        node: Syntax(macroDecl),
        message: ASTGenMacroDiagnostic.thrownError(error)
      )
    )
    return -1
  }
}

@_cdecl("swift_ASTGen_freeExpansionReplacements")
public func freeExpansionReplacements(
  pointer: UnsafeMutablePointer<Int>?,
  numReplacements: Int
) {
  UnsafeMutableBufferPointer(start: pointer, count: numReplacements).deallocate()
}

// Make an expansion result for '@_cdecl' function caller.
func makeExpansionOutputResult(
  expandedSource: String?,
  outputPointer: UnsafeMutablePointer<BridgedStringRef>
) -> Int {
  guard let expandedSource = expandedSource else {
    outputPointer.pointee = BridgedStringRef()
    return -1
  }
  outputPointer.pointee = allocateBridgedString(expandedSource)
  return 0
}

@_cdecl("swift_ASTGen_expandFreestandingMacro")
@usableFromInline
func expandFreestandingMacro(
  diagEnginePtr: UnsafeMutableRawPointer,
  macroPtr: UnsafeRawPointer,
  discriminatorText: UnsafePointer<CChar>,
  rawMacroRole: UInt8,
  sourceFilePtr: UnsafeRawPointer,
  sourceLocationPtr: UnsafePointer<UInt8>?,
  expandedSourceOutPtr: UnsafeMutablePointer<BridgedStringRef>
) -> Int {
  // We didn't expand anything so far.
  assert(expandedSourceOutPtr.pointee.isEmptyInitialized)

  guard let sourceLocationPtr = sourceLocationPtr else {
    print("NULL source location")
    return -1
  }

  let sourceFilePtr = sourceFilePtr.bindMemory(to: ExportedSourceFile.self, capacity: 1)

  guard
    let macroSyntax = findSyntaxNodeInSourceFile(
      sourceFilePtr: sourceFilePtr,
      sourceLocationPtr: sourceLocationPtr,
      type: Syntax.self
    )
  else {
    return 1
  }
  guard
    let expansion = macroSyntax.asProtocol(
      FreestandingMacroExpansionSyntax.self
    )
  else {
    print("not on a macro expansion node: \(macroSyntax.debugDescription)")
    return 1
  }

  let discriminator = String(cString: discriminatorText)

  let macroRole = MacroRole(rawMacroRole: rawMacroRole)
  let expandedSource: String? = expandFreestandingMacroImpl(
    macroPtr: macroPtr,
    macroRole: macroRole,
    diagEnginePtr: diagEnginePtr,
    expansionSyntax: expansion,
    sourceFilePtr: sourceFilePtr,
    discriminator: discriminator
  )

  return makeExpansionOutputResult(
    expandedSource: expandedSource,
    outputPointer: expandedSourceOutPtr
  )
}

func expandFreestandingMacroImpl(
  macroPtr: UnsafeRawPointer,
  macroRole: MacroRole,
  diagEnginePtr: UnsafeMutableRawPointer,
  expansionSyntax: FreestandingMacroExpansionSyntax,
  sourceFilePtr: UnsafePointer<ExportedSourceFile>,
  discriminator: String
) -> String? {

  let macroName: String
  if let exprSyntax = expansionSyntax.as(MacroExpansionExprSyntax.self) {
    macroName = exprSyntax.macroName.text
  } else if let declSyntax = expansionSyntax.as(MacroExpansionDeclSyntax.self) {
    macroName = declSyntax.macroName.text
  } else {
    fatalError("unknown syntax")
  }

  let macro = macroPtr.assumingMemoryBound(to: ExportedExternalMacro.self).pointee

  // Map the macro role.
  let pluginMacroRole: PluginMessage.MacroRole
  switch macroRole {
  case .accessor, .member, .memberAttribute, .peer, .conformance, .extension, .preamble, .body:
    preconditionFailure("unhandled macro role for freestanding macro")

  case .expression: pluginMacroRole = .expression
  case .declaration: pluginMacroRole = .declaration
  case .codeItem: pluginMacroRole = .codeItem
#if RESILIENT_SWIFT_SYNTAX
  @unknown default: fatalError()
#endif
  }

  // Send the message.
  let message = HostToPluginMessage.expandFreestandingMacro(
    macro: .init(moduleName: macro.moduleName, typeName: macro.typeName, name: macroName),
    macroRole: pluginMacroRole,
    discriminator: discriminator,
    syntax: PluginMessage.Syntax(syntax: Syntax(expansionSyntax), in: sourceFilePtr)!,
    lexicalContext: pluginLexicalContext(of: expansionSyntax)
  )
  do {
    let result = try macro.plugin.sendMessageAndWait(message)
    let expandedSource: String?
    let diagnostics: [PluginMessage.Diagnostic]
    switch result {
    case .expandMacroResult(let _expandedSource, let _diagnostics),
      .expandFreestandingMacroResult(let _expandedSource, let _diagnostics):
      expandedSource = _expandedSource
      diagnostics = _diagnostics
    default:
      throw PluginError.invalidReponseKind
    }

    // Process the result.
    if !diagnostics.isEmpty {
      let diagEngine = PluginDiagnosticsEngine(cxxDiagnosticEngine: diagEnginePtr)
      diagEngine.add(exportedSourceFile: sourceFilePtr)
      diagEngine.emit(diagnostics, messageSuffix: " (from macro '\(macroName)')")
    }
    return expandedSource

  } catch let error {
    let srcMgr = SourceManager(cxxDiagnosticEngine: diagEnginePtr)
    srcMgr.insert(sourceFilePtr)
    srcMgr.diagnose(
      diagnostic: .init(
        node: Syntax(expansionSyntax),
        // FIXME: This is probably a plugin communication error.
        // The error might not be relevant as the diagnostic message.
        message: ASTGenMacroDiagnostic.thrownError(error)
      ),
      messageSuffix: " (from macro '\(macroName)')"
    )
    return nil
  }
}

/// Retrieve a syntax node in the given source file, with the given type.
func findSyntaxNodeInSourceFile<Node: SyntaxProtocol>(
  sourceFilePtr: UnsafeRawPointer,
  sourceLocationPtr: UnsafePointer<UInt8>?,
  type: Node.Type,
  wantOutermost: Bool = false
) -> Node? {
  guard let sourceLocationPtr = sourceLocationPtr else {
    return nil
  }

  let sourceFilePtr = sourceFilePtr.assumingMemoryBound(to: ExportedSourceFile.self)

  // Find the offset.
  let buffer = sourceFilePtr.pointee.buffer
  let offset = sourceLocationPtr - buffer.baseAddress!
  if offset < 0 || offset >= buffer.count {
    print("source location isn't inside this buffer")
    return nil
  }

  // Find the token at that offset.
  let sf = sourceFilePtr.pointee.syntax
  guard let token = sf.token(at: AbsolutePosition(utf8Offset: offset)) else {
    print("couldn't find token at offset \(offset)")
    return nil
  }

  var currentSyntax = Syntax(token)
  var resultSyntax: Node? = nil
  while let parentSyntax = currentSyntax.parent {
    currentSyntax = parentSyntax
    if let typedParent = currentSyntax.as(type) {
      resultSyntax = typedParent
      break
    }
  }

  // If we didn't find anything, complain and fail.
  guard var resultSyntax else {
    print("unable to find node: \(token.debugDescription)")
    return nil
  }

  // If we want the outermost node, keep looking.
  // E.g. for 'foo.bar' we want the member ref expression instead of the
  // identifier expression.
  if wantOutermost {
    while let parentSyntax = currentSyntax.parent,
      parentSyntax.position == resultSyntax.position
    {
      currentSyntax = parentSyntax
      if let typedParent = currentSyntax.as(type) {
        resultSyntax = typedParent
      }
    }
  }

  return resultSyntax
}

@_cdecl("swift_ASTGen_expandAttachedMacro")
@usableFromInline
func expandAttachedMacro(
  diagEnginePtr: UnsafeMutableRawPointer,
  macroPtr: UnsafeRawPointer,
  discriminatorText: UnsafePointer<CChar>,
  qualifiedTypeText: UnsafePointer<CChar>,
  conformanceListText: UnsafePointer<CChar>,
  rawMacroRole: UInt8,
  customAttrSourceFilePtr: UnsafeRawPointer,
  customAttrSourceLocPointer: UnsafePointer<UInt8>?,
  declarationSourceFilePtr: UnsafeRawPointer,
  attachedTo declarationSourceLocPointer: UnsafePointer<UInt8>?,
  parentDeclSourceFilePtr: UnsafeRawPointer?,
  parentDeclSourceLocPointer: UnsafePointer<UInt8>?,
  expandedSourceOutPtr: UnsafeMutablePointer<BridgedStringRef>
) -> Int {
  // We didn't expand anything so far.
  assert(expandedSourceOutPtr.pointee.isEmptyInitialized)

  // Dig out the custom attribute for the attached macro declarations.
  guard
    let customAttrNode = findSyntaxNodeInSourceFile(
      sourceFilePtr: customAttrSourceFilePtr,
      sourceLocationPtr: customAttrSourceLocPointer,
      type: AttributeSyntax.self
    )
  else {
    return 1
  }

  // Dig out the node for the declaration to which the custom attribute is
  // attached.
  guard
    let declarationNode = findSyntaxNodeInSourceFile(
      sourceFilePtr: declarationSourceFilePtr,
      sourceLocationPtr: declarationSourceLocPointer,
      type: DeclSyntax.self
    )
  else {
    return 1
  }

  var parentDeclNode: DeclSyntax?
  if let parentDeclSourceFilePtr = parentDeclSourceFilePtr {
    parentDeclNode = findSyntaxNodeInSourceFile(
      sourceFilePtr: parentDeclSourceFilePtr,
      sourceLocationPtr: parentDeclSourceLocPointer,
      type: DeclSyntax.self
    )
  }

  let customAttrSourceFilePtr = customAttrSourceFilePtr.bindMemory(to: ExportedSourceFile.self, capacity: 1)
  let declarationSourceFilePtr = declarationSourceFilePtr.bindMemory(to: ExportedSourceFile.self, capacity: 1)
  let parentDeclSourceFilePtr = parentDeclSourceFilePtr?.bindMemory(to: ExportedSourceFile.self, capacity: 1)

  let discriminator = String(cString: discriminatorText)
  let qualifiedType = String(cString: qualifiedTypeText)
  let conformanceList = String(cString: conformanceListText)

  let expandedSource: String? = expandAttachedMacroImpl(
    diagEnginePtr: diagEnginePtr,
    macroPtr: macroPtr,
    rawMacroRole: rawMacroRole,
    discriminator: discriminator,
    qualifiedType: qualifiedType,
    conformanceList: conformanceList,
    customAttrSourceFilePtr: customAttrSourceFilePtr,
    customAttrNode: customAttrNode,
    declarationSourceFilePtr: declarationSourceFilePtr,
    attachedTo: declarationNode,
    parentDeclSourceFilePtr: parentDeclSourceFilePtr,
    parentDeclNode: parentDeclNode
  )

  return makeExpansionOutputResult(
    expandedSource: expandedSource,
    outputPointer: expandedSourceOutPtr
  )
}

/// Produce the full lexical context of the given node to pass along to
/// macro expansion.
private func lexicalContext(of node: some SyntaxProtocol) -> [Syntax] {
  // FIXME: Should we query the source manager to get the macro expansion
  // information?
  node.allMacroLexicalContexts()
}

/// Produce the full lexical context of the given node to pass along to
/// macro expansion.
private func pluginLexicalContext(of node: some SyntaxProtocol) -> [PluginMessage.Syntax] {
  lexicalContext(of: node).compactMap { .init(syntax: $0) }
}

func expandAttachedMacroImpl(
  diagEnginePtr: UnsafeMutableRawPointer,
  macroPtr: UnsafeRawPointer,
  rawMacroRole: UInt8,
  discriminator: String,
  qualifiedType: String,
  conformanceList: String,
  customAttrSourceFilePtr: UnsafePointer<ExportedSourceFile>,
  customAttrNode: AttributeSyntax,
  declarationSourceFilePtr: UnsafePointer<ExportedSourceFile>,
  attachedTo declarationNode: DeclSyntax,
  parentDeclSourceFilePtr: UnsafePointer<ExportedSourceFile>?,
  parentDeclNode: DeclSyntax?
) -> String? {
  let macroName: String = customAttrNode.attributeName.description
  let macro = macroPtr.assumingMemoryBound(to: ExportedExternalMacro.self).pointee

  // Map the macro role.
  let macroRole: PluginMessage.MacroRole
  switch MacroRole(rawMacroRole: rawMacroRole) {
  case .accessor: macroRole = .accessor
  case .member: macroRole = .member
  case .memberAttribute: macroRole = .memberAttribute
  case .peer: macroRole = .peer
  case .conformance: macroRole = .conformance
  case .extension: macroRole = .`extension`
  case .preamble: macroRole = .preamble
  case .body: macroRole = .body

  case .expression,
    .declaration,
    .codeItem:
    preconditionFailure("unhandled macro role for attached macro")

#if RESILIENT_SWIFT_SYNTAX
  @unknown default:
    fatalError()
#endif
  }

  // Prepare syntax nodes to transfer.
  let customAttributeSyntax = PluginMessage.Syntax(
    syntax: Syntax(customAttrNode),
    in: customAttrSourceFilePtr
  )!

  let declSyntax = PluginMessage.Syntax(
    syntax: Syntax(declarationNode),
    in: declarationSourceFilePtr
  )!

  let parentDeclSyntax: PluginMessage.Syntax?
  if parentDeclNode != nil {
    parentDeclSyntax = .init(syntax: Syntax(parentDeclNode!), in: parentDeclSourceFilePtr!)!
  } else {
    parentDeclSyntax = nil
  }

  let extendedTypeSyntax: PluginMessage.Syntax?
  if (!qualifiedType.isEmpty) {
    let typeSyntax: TypeSyntax = "\(raw: qualifiedType)"
    extendedTypeSyntax = .init(syntax: Syntax(typeSyntax))!
  } else {
    extendedTypeSyntax = nil
  }

  let conformanceListSyntax: PluginMessage.Syntax?
  if (conformanceList.isEmpty) {
    conformanceListSyntax = nil
  } else {
    let placeholderDecl: DeclSyntax =
      """
      struct Placeholder: \(raw: conformanceList) {}
      """
    conformanceListSyntax = .init(syntax: Syntax(placeholderDecl))!
  }


  // Send the message.
  let message = HostToPluginMessage.expandAttachedMacro(
    macro: .init(moduleName: macro.moduleName, typeName: macro.typeName, name: macroName),
    macroRole: macroRole,
    discriminator: discriminator,
    attributeSyntax: customAttributeSyntax,
    declSyntax: declSyntax,
    parentDeclSyntax: parentDeclSyntax,
    extendedTypeSyntax: extendedTypeSyntax,
    conformanceListSyntax: conformanceListSyntax,
    lexicalContext: pluginLexicalContext(of: declarationNode)
  )
  do {
    let expandedSource: String?
    let diagnostics: [PluginMessage.Diagnostic]
    switch try macro.plugin.sendMessageAndWait(message) {
    case .expandMacroResult(let _expandedSource, let _diagnostics):
      expandedSource = _expandedSource
      diagnostics = _diagnostics

    // Handle legacy result message.
    case .expandAttachedMacroResult(let _expandedSources, let _diagnostics):
      if let _expandedSources = _expandedSources {
        expandedSource = SwiftSyntaxMacroExpansion.collapse(
          expansions: _expandedSources,
          for: MacroRole(rawMacroRole: rawMacroRole),
          attachedTo: declarationNode
        )
      } else {
        expandedSource = nil
      }
      diagnostics = _diagnostics
      break
    default:
      throw PluginError.invalidReponseKind
    }

    // Process the result.
    if !diagnostics.isEmpty {
      let diagEngine = PluginDiagnosticsEngine(cxxDiagnosticEngine: diagEnginePtr)
      diagEngine.add(exportedSourceFile: customAttrSourceFilePtr)
      diagEngine.add(exportedSourceFile: declarationSourceFilePtr)
      if let parentDeclSourceFilePtr = parentDeclSourceFilePtr {
        diagEngine.add(exportedSourceFile: parentDeclSourceFilePtr)
      }
      diagEngine.emit(diagnostics, messageSuffix: " (from macro '\(macroName)')")
    }
    return expandedSource

  } catch let error {
    let srcMgr = SourceManager(cxxDiagnosticEngine: diagEnginePtr)
    srcMgr.insert(customAttrSourceFilePtr)
    srcMgr.insert(declarationSourceFilePtr)
    if let parentDeclSourceFilePtr = parentDeclSourceFilePtr {
      srcMgr.insert(parentDeclSourceFilePtr)
    }
    // FIXME: Need to decide where to diagnose the error:
    srcMgr.diagnose(
      diagnostic: .init(
        node: Syntax(declarationNode),
        // FIXME: This is probably a plugin communication error.
        // The error might not be relevant as the diagnostic message.
        message: ASTGenMacroDiagnostic.thrownError(error)
      ),
      messageSuffix: " (from macro '\(macroName)')"
    )
    return nil
  }
}

