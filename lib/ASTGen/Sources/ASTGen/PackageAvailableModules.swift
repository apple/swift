//===----------------------------------------------------------------------===//
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

import ASTBridging
import BasicBridging
import SwiftBasicFormat
@_spi(PluginMessage) import SwiftCompilerPluginMessageHandling
import SwiftDiagnostics
@_spi(ExperimentalLanguageFeatures) import SwiftParser
import SwiftSyntax

/// Describes the set of modules that are available to targets within a
/// given package along with the target dependency that's required to import
/// that module into a given
struct AvailableModules: Codable {
    /// A description of the target dependency that would be needed to
    /// import a given module.
    enum TargetDependency: Codable {
        case target(name: String)
        case product(name: String, package: String?)
    }

    /// The set of modules that are available within the package described by
    /// the manifest, along with the target dependency required to reference
    /// the module.
    var modules: [String: TargetDependency] = [:]
}

@_cdecl("swift_ASTGen_load_available_modules")
public func loadAvailableModules(
  jsonContentsPtr: UnsafePointer<UInt8>,
  jsonContentsLength: Int
) -> UnsafeMutableRawPointer? {
  let pointer = UnsafeMutablePointer<AvailableModules>.allocate(capacity: 1)
  do {
    let data = UnsafeBufferPointer(start: jsonContentsPtr, count: jsonContentsLength)
    pointer.initialize(to: try JSON.decode(AvailableModules.self, from: data))
    return UnsafeMutableRawPointer(pointer)
  } catch {
    pointer.deallocate()
    return nil
  }
}

@_cdecl("swift_ASTGen_destroy_available_modules")
public func destroyAvailableModules(pointer: UnsafeMutableRawPointer) {
  pointer.withMemoryRebound(to: AvailableModules.self, capacity: 1) { ptr in
    ptr.deinitialize(count: 1)
    ptr.deallocate()
  }
}

/// A diagnostic message used to fix the package manifest to add a missing
/// target dependency.
fileprivate struct AddTargetDependencyMessage: DiagnosticMessage, FixItMessage {
  let dependency: AvailableModules.TargetDependency

  var message: String {
    switch dependency {
    case .target(name: let name):
      return "add dependency on target '\(name)' to the package manifest"

    case .product(name: let name, package: nil):
      return "add dependency on product '\(name)' from the current package package manifest"

    case .product(name: let name, package: let package?):
      return "add dependency on product '\(name)' from the package '\(package)' to the package manifest"
    }
  }

  var severity: DiagnosticSeverity { .note }

  var diagnosticID: MessageID {
    .init(domain: "Swift", id: "\(self)")
  }

  var fixItID: MessageID { diagnosticID }
}

@_cdecl("swift_ASTGen_package_manifest_import_fixit")
public func packageManifestImportFixIt(
    diagEnginePtr: UnsafeMutableRawPointer,
    availableModulesPtr: UnsafeMutableRawPointer,
    manifestSourceFilePtr: UnsafeMutablePointer<UInt8>,
    moduleNamePtr: UnsafePointer<UInt8>,
    moduleNameLength: Int,
    targetNamePtr: UnsafePointer<UInt8>,
    targetNameLength: Int
) {
  let availableModules = availableModulesPtr.assumingMemoryBound(to: AvailableModules.self).pointee
  // The name of the module we were trying to import.
  let moduleName = String(
    decoding: UnsafeBufferPointer(
      start: moduleNamePtr,
      count: moduleNameLength
    ),
    as: UTF8.self
  )

  // Find the target dependency in the available modules. If we don't know
  // anything about this target, there's nothing we can do.
  guard let targetDependency = availableModules.modules[moduleName] else {
    return
  }

  // Figure out what we should add to the package manifest.
  let newDependencyArgument: ExprSyntax
  switch targetDependency {
  case .target(name: let name):
    newDependencyArgument = ".target(name: \(literal: name)"
  case .product(name: let name, package: nil):
    newDependencyArgument = ".product(name: \(literal: name))"
  case .product(name: let name, package: let package):
    newDependencyArgument = ".product(name: \(literal: name), package: \(literal: package))"
  }

  // The manifest source file.
  let manifestSourceFile = manifestSourceFilePtr.withMemoryRebound(
    to: ExportedSourceFile.self,
    capacity: 1
  ) { ptr in
    ptr.pointee
  }

  guard let packageCall = manifestSourceFile.syntax.findCall(calleeName: "Package") else {
      return
  }

  // Dig out the array of targets.
  guard let targetsArgument = packageCall.findArgument(labeled: "targets"),
        let targetArray = targetsArgument.expression.findArrayArgument() else {
      return
  }

  // The target in which we are trying to perform the import.
  let targetName = String(
    decoding: UnsafeBufferPointer(
      start: targetNamePtr,
      count: targetNameLength
    ),
    as: UTF8.self
  )

  // Look for a call whose name is a string literal matching the
  // requested target name.
  func matchesTargetCall(call: FunctionCallExprSyntax) -> Bool {
      guard let nameArgument = call.findArgument(labeled: "name") else {
          return false
      }

      guard let stringLiteral = nameArgument.expression.as(StringLiteralExprSyntax.self),
          let literalValue = stringLiteral.representedLiteralValue else {
          return false
      }

      return literalValue == targetName
  }

  guard let targetCall = FunctionCallExprSyntax.findFirst(in: targetArray, matching: matchesTargetCall) else {
      return
  }

  // Form the new target call.
  guard let newTargetCall = targetCall.appendingToArrayArgument(
        label: "dependencies",
        trailingLabels: argumentLabelsAfterDependencies,
        newElement: newDependencyArgument
    ) else {
    return
  }

  let message = AddTargetDependencyMessage(dependency: targetDependency)
  let diagnostic = Diagnostic(
    node: targetCall,
    message: AddTargetDependencyMessage(dependency: targetDependency),
    fixIts: [
      FixIt(
        message: message,
        changes: [
          .replace(oldNode: Syntax(targetCall), newNode: Syntax(newTargetCall))
        ]
      )
    ]
  )

  emitDiagnostic(
    diagnosticEngine: BridgedDiagnosticEngine(raw: diagEnginePtr),
    sourceFileBuffer: manifestSourceFile.buffer,
    diagnostic: diagnostic,
    diagnosticSeverity: .note
  )
}

/// The set of argument labels that can occur after the "dependencies"
/// argument in the various target initializers.
///
/// TODO: Could we generate this from the the PackageDescription module, so
/// we don't have keep it up-to-date manually?
private let argumentLabelsAfterDependencies: Set<String> = [
  "path",
  "exclude",
  "sources",
  "resources",
  "publicHeadersPath",
  "packageAccess",
  "cSettings",
  "cxxSettings",
  "swiftSettings",
  "linkerSettings",
  "plugins",
]


// TODO: Cloned from SwiftPM, should be sunk down into swift-syntax in some form.

/// Default indent when we have to introduce indentation but have no context
/// to get it right.
let defaultIndent = TriviaPiece.spaces(4)

extension Trivia {
    /// Determine whether this trivia has newlines or not.
    var hasNewlines: Bool {
        contains(where: \.isNewline)
    }

    /// Produce trivia from the last newline to the end, dropping anything
    /// prior to that.
    func onlyLastLine() -> Trivia {
        guard let lastNewline = pieces.lastIndex(where: { $0.isNewline }) else {
            return self
        }

        return Trivia(pieces: pieces[lastNewline...])
    }
}

/// Syntax walker to find the first occurrence of a given node kind that
/// matches a specific predicate.
private class FirstNodeFinder<Node: SyntaxProtocol>: SyntaxAnyVisitor {
    var predicate: (Node) -> Bool
    var found: Node? = nil

    init(predicate: @escaping (Node) -> Bool) {
        self.predicate = predicate
        super.init(viewMode: .sourceAccurate)
    }

    override func visitAny(_ node: Syntax) -> SyntaxVisitorContinueKind {
        if found != nil {
            return .skipChildren
        }

        if let matchedNode = node.as(Node.self), predicate(matchedNode) {
            found = matchedNode
            return .skipChildren
        }

        return .visitChildren
    }
}

extension SyntaxProtocol {
    /// Find the first node of the Self type that matches the given predicate.
    static func findFirst(
        in node: some SyntaxProtocol,
        matching predicate: (Self) -> Bool
    ) -> Self? {
        withoutActuallyEscaping(predicate) { escapingPredicate in
            let visitor = FirstNodeFinder<Self>(predicate: escapingPredicate)
            visitor.walk(node)
            return visitor.found
        }
    }
}

extension FunctionCallExprSyntax {
    /// Check whether this call expression has a callee that is a reference
    /// to a declaration with the given name.
    func hasCallee(named name: String) -> Bool {
        guard let calleeDeclRef = calledExpression.as(DeclReferenceExprSyntax.self) else {
            return false
        }

        return calleeDeclRef.baseName.text == name
    }

    /// Find a call argument based on its label.
    func findArgument(labeled label: String) -> LabeledExprSyntax? {
        arguments.first { $0.label?.text == label }
    }

    /// Find a call argument index based on its label.
    func findArgumentIndex(labeled label: String) -> LabeledExprListSyntax.Index? {
        arguments.firstIndex { $0.label?.text == label }
    }
}

extension LabeledExprListSyntax {
    /// Find the index at which the one would insert a new argument given
    /// the set of argument labels that could come after the argument we
    /// want to insert.
    func findArgumentInsertionPosition(
        labelsAfter: Set<String>
    ) -> SyntaxChildrenIndex {
        firstIndex {
            guard let label = $0.label else {
                return false
            }

            return labelsAfter.contains(label.text)
        } ?? endIndex
    }

    /// Form a new argument list that inserts a new argument at the specified
    /// position in this argument list.
    ///
    /// This operation will attempt to introduce trivia to match the
    /// surrounding context where possible. The actual argument will be
    /// created by the `generator` function, which is provided with leading
    /// trivia and trailing comma it should use to match the surrounding
    /// context.
    func insertingArgument(
        at position: SyntaxChildrenIndex,
        generator: (Trivia, TokenSyntax?) -> LabeledExprSyntax
    ) -> LabeledExprListSyntax {
        // Turn the arguments into an array so we can manipulate them.
        var arguments = Array(self)

        let positionIdx = distance(from: startIndex, to: position)

        let commaToken = TokenSyntax.commaToken()

        // Figure out leading trivia and adjust the prior argument (if there is
        // one) by adding a comma, if necessary.
        let leadingTrivia: Trivia
        if position > startIndex {
            let priorArgument = arguments[positionIdx - 1]

            // Our leading trivia will be based on the prior argument's leading
            // trivia.
            leadingTrivia = priorArgument.leadingTrivia

            // If the prior argument is missing a trailing comma, add one.
            if priorArgument.trailingComma == nil {
                arguments[positionIdx - 1].trailingComma = commaToken
            }
        } else if positionIdx + 1 < count {
            leadingTrivia = arguments[positionIdx + 1].leadingTrivia
        } else {
            leadingTrivia = Trivia()
        }

        // Determine whether we need a trailing comma on this argument.
        let trailingComma: TokenSyntax?
        if position < endIndex {
            trailingComma = commaToken
        } else {
            trailingComma = nil
        }

        // Create the argument and insert it into the argument list.
        let argument = generator(leadingTrivia, trailingComma)
        arguments.insert(argument, at: positionIdx)

        return LabeledExprListSyntax(arguments)
    }
}

extension SyntaxProtocol {
    /// Look for a call expression to a callee with the given name.
    func findCall(calleeName: String) -> FunctionCallExprSyntax? {
        return FunctionCallExprSyntax.findFirst(in: self) { call in
            return call.hasCallee(named: calleeName)
        }
    }
}

extension ArrayExprSyntax {
    /// Produce a new array literal expression that appends the given
    /// element, while trying to maintain similar indentation.
    func appending(
        element: ExprSyntax,
        outerLeadingTrivia: Trivia
    ) -> ArrayExprSyntax {
        var elements = self.elements

        let commaToken = TokenSyntax.commaToken()

        // If there are already elements, tack it on.
        let leadingTrivia: Trivia
        let trailingTrivia: Trivia
        let leftSquareTrailingTrivia: Trivia
        if let last = elements.last {
            // The leading trivia of the new element should match that of the
            // last element.
            leadingTrivia = last.leadingTrivia.onlyLastLine()

            // Add a trailing comma to the last element if it isn't already
            // there.
            if last.trailingComma == nil {
                var newElements = Array(elements)
                newElements[newElements.count - 1].trailingComma = commaToken
                newElements[newElements.count - 1].expression.trailingTrivia =
                    Trivia()
                newElements[newElements.count - 1].trailingTrivia = last.trailingTrivia
                elements = ArrayElementListSyntax(newElements)
            }

            trailingTrivia = Trivia()
            leftSquareTrailingTrivia = leftSquare.trailingTrivia
        } else {
            leadingTrivia = outerLeadingTrivia.appending(defaultIndent)
            trailingTrivia = outerLeadingTrivia
            if leftSquare.trailingTrivia.hasNewlines {
                leftSquareTrailingTrivia = leftSquare.trailingTrivia
            } else {
                leftSquareTrailingTrivia = Trivia()
            }
        }

        elements.append(
            ArrayElementSyntax(
                expression: element.with(\.leadingTrivia, leadingTrivia),
                trailingComma: commaToken.with(\.trailingTrivia, trailingTrivia)
            )
        )

        let newLeftSquare = leftSquare.with(
            \.trailingTrivia,
             leftSquareTrailingTrivia
        )

        return with(\.elements, elements).with(\.leftSquare, newLeftSquare)
    }
}

extension ExprSyntax {
    /// Find an array argument either at the top level or within a sequence
    /// expression.
    func findArrayArgument() -> ArrayExprSyntax? {
        if let arrayExpr = self.as(ArrayExprSyntax.self) {
            return arrayExpr
        }

        if let sequenceExpr = self.as(SequenceExprSyntax.self) {
            return sequenceExpr.elements.lazy.compactMap {
                $0.findArrayArgument()
            }.first
        }

        return nil
    }
}

// MARK: Utilities to oeprate on arrays of array literal elements.
extension Array<ArrayElementSyntax> {
    /// Append a new argument expression.
    mutating func append(expression: ExprSyntax) {
        // Add a comma on the prior expression, if there is one.
        let leadingTrivia: Trivia?
        if count > 0 {
            self[count - 1].trailingComma = TokenSyntax.commaToken()
            leadingTrivia = .newline

            // Adjust the first element to start with a newline
            if count == 1 {
                self[0].leadingTrivia = .newline
            }
        } else {
            leadingTrivia = nil
        }

        append(
            ArrayElementSyntax(
                leadingTrivia: leadingTrivia,
                expression: expression
            )
        )
    }
}

// MARK: Utilities for adding arguments into calls.
fileprivate class ReplacingRewriter: SyntaxRewriter {
    let childNode: Syntax
    let newChildNode: Syntax

    init(childNode: Syntax, newChildNode: Syntax) {
        self.childNode = childNode
        self.newChildNode = newChildNode
        super.init()
    }

    override func visitAny(_ node: Syntax) -> Syntax? {
        if node == childNode {
            return newChildNode
        }

        return nil
    }
}

fileprivate extension SyntaxProtocol {
    /// Replace the given child with a new child node.
    func replacingChild(_ childNode: Syntax, with newChildNode: Syntax) -> Self {
        return ReplacingRewriter(
            childNode: childNode,
            newChildNode: newChildNode
        ).rewrite(self).cast(Self.self)
    }
}

extension FunctionCallExprSyntax {
    /// Produce source edits that will add the given new element to the
    /// array for an argument with the given label (if there is one), or
    /// introduce a new argument with an array literal containing only the
    /// new element.
    ///
    /// - Parameters:
    ///   - label: The argument label for the argument whose array will be
    ///     added or modified.
    ///   - trailingLabels: The argument labels that could follow the label,
    ///     which helps determine where the argument should be inserted if
    ///     it doesn't exist yet.
    ///   - newElement: The new element.
    /// - Returns: the function call after making this change.
    func appendingToArrayArgument(
        label: String,
        trailingLabels: Set<String>,
        newElement: ExprSyntax
    ) -> FunctionCallExprSyntax? {
        // If there is already an argument with this name, append to the array
        // literal in there.
        if let arg = findArgument(labeled: label) {
            guard let argArray = arg.expression.findArrayArgument() else {
                return nil
            }

            // Format the element appropriately for the context.
            let indentation = Trivia(
                pieces: arg.leadingTrivia.filter { $0.isSpaceOrTab }
            )
            let format = BasicFormat(
                indentationWidth: [ defaultIndent ],
                initialIndentation: indentation.appending(defaultIndent)
            )
            let formattedElement = newElement.formatted(using: format)
                .cast(ExprSyntax.self)

            let updatedArgArray = argArray.appending(
                element: formattedElement,
                outerLeadingTrivia: arg.leadingTrivia
            )

            return replacingChild(Syntax(argArray), with: Syntax(updatedArgArray))
        }

        // There was no argument, so we need to create one.

        // Insert the new argument at the appropriate place in the call.
        let insertionPos = arguments.findArgumentInsertionPosition(
            labelsAfter: trailingLabels
        )
        let newArguments = arguments.insertingArgument(
            at: insertionPos
        ) { (leadingTrivia, trailingComma) in
            // Format the element appropriately for the context.
            let indentation = Trivia(pieces: leadingTrivia.filter { $0.isSpaceOrTab })
            let format = BasicFormat(
                indentationWidth: [ defaultIndent ],
                initialIndentation: indentation.appending(defaultIndent)
            )
            let formattedElement = newElement.formatted(using: format)
                .cast(ExprSyntax.self)

            // Form the array.
            let newArgument = ArrayExprSyntax(
                leadingTrivia: .space,
                leftSquare: .leftSquareToken(
                    trailingTrivia: .newline
                ),
                elements: ArrayElementListSyntax(
                    [
                        ArrayElementSyntax(
                            expression: formattedElement,
                            trailingComma: .commaToken()
                        )
                    ]
                ),
                rightSquare: .rightSquareToken(
                    leadingTrivia: leadingTrivia
                )
            )

            // Create the labeled argument for the array.
            return LabeledExprSyntax(
                leadingTrivia: leadingTrivia,
                label: "\(raw: label)",
                colon: .colonToken(),
                expression: ExprSyntax(newArgument),
                trailingComma: trailingComma
            )
        }

        return with(\.arguments, newArguments)
    }
}
