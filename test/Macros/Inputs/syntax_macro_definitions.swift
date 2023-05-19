import SwiftDiagnostics
import SwiftOperators
import SwiftSyntax
import SwiftSyntaxBuilder
import SwiftSyntaxMacros

/// Replace the label of the first element in the tuple with the given
/// new label.
private func replaceFirstLabel(
  of tuple: TupleExprElementListSyntax, with newLabel: String
) -> TupleExprElementListSyntax{
  guard let firstElement = tuple.first else {
    return tuple
  }

  return tuple.replacing(
    childAt: 0, with: firstElement.with(\.label, .identifier(newLabel)))
}

public struct ColorLiteralMacro: ExpressionMacro {
  public static func expansion(
    of macro: some FreestandingMacroExpansionSyntax,
    in context: some MacroExpansionContext
  ) -> ExprSyntax {
    let argList = replaceFirstLabel(
      of: macro.argumentList, with: "_colorLiteralRed"
    )
    let initSyntax: ExprSyntax = ".init(\(argList))"
    return initSyntax
  }
}

public struct FileIDMacro: ExpressionMacro {
  public static func expansion<
    Node: FreestandingMacroExpansionSyntax,
    Context: MacroExpansionContext
  >(
    of macro: Node,
    in context: Context
  ) throws -> ExprSyntax {
    guard let sourceLoc = context.location(of: macro) else {
      throw CustomError.message("can't find location for macro")
    }

    let fileLiteral: ExprSyntax = "\(sourceLoc.file)"
    return fileLiteral
  }
}

public struct AssertMacro: ExpressionMacro {
  public static func expansion(
    of macro: some FreestandingMacroExpansionSyntax,
    in context: some MacroExpansionContext
  ) -> ExprSyntax {
    guard let argument = macro.argumentList.first?.expression else {
      fatalError("boom")
    }

    return "assert(\(argument))"
  }
}

public struct StringifyMacro: ExpressionMacro {
  public static func expansion(
    of macro: some FreestandingMacroExpansionSyntax,
    in context: some MacroExpansionContext
  ) -> ExprSyntax {
    guard let argument = macro.argumentList.first?.expression else {
      fatalError("boom")
    }

    return "(\(argument), \(StringLiteralExprSyntax(content: argument.description)))"
  }
}

public struct StringifyAndTryMacro: ExpressionMacro {
  public static func expansion(
    of macro: some FreestandingMacroExpansionSyntax,
    in context: some MacroExpansionContext
  ) -> ExprSyntax {
    guard let argument = macro.argumentList.first?.expression else {
      fatalError("boom")
    }

    return "(try \(argument), \(StringLiteralExprSyntax(content: argument.description)))"
  }
}

struct SimpleDiagnosticMessage: DiagnosticMessage {
  let message: String
  let diagnosticID: MessageID
  let severity: DiagnosticSeverity
}

extension SimpleDiagnosticMessage: FixItMessage {
  var fixItID: MessageID { diagnosticID }
}

public enum AddBlocker: ExpressionMacro {
  class AddVisitor: SyntaxRewriter {
    var diagnostics: [Diagnostic] = []

    override func visit(
      _ node: InfixOperatorExprSyntax
    ) -> ExprSyntax {
      if let binOp = node.operatorOperand.as(BinaryOperatorExprSyntax.self) {
        if binOp.operatorToken.text == "+" {
          let messageID = MessageID(domain: "silly", id: "addblock")
          diagnostics.append(
            Diagnostic(
              node: Syntax(node.operatorOperand),
              message: SimpleDiagnosticMessage(
                message: "blocked an add; did you mean to subtract?",
                diagnosticID: messageID,
                severity: .error
              ),
              highlights: [
                Syntax(node.leftOperand),
                Syntax(node.rightOperand)
              ],
              fixIts: [
                FixIt(
                  message: SimpleDiagnosticMessage(
                    message: "use '-'",
                    diagnosticID: messageID,
                    severity: .error
                  ),
                  changes: [
                    FixIt.Change.replace(
                      oldNode: Syntax(binOp.operatorToken),
                      newNode: Syntax(
                        TokenSyntax(
                          .binaryOperator("-"),
                          presence: .present
                        )
                      )
                    )
                  ]
                ),
              ]
            )
          )

          return ExprSyntax(
            node.with(
              \.operatorOperand,
              ExprSyntax(
                binOp.with(
                  \.operatorToken,
                  binOp.operatorToken.with(\.tokenKind, .binaryOperator("-"))
                )
              )
            )
          )
        }
      }

      return ExprSyntax(node)
    }
  }

  public static func expansion(
    of node: some FreestandingMacroExpansionSyntax,
    in context: some MacroExpansionContext
  ) -> ExprSyntax {
    let visitor = AddVisitor()
    let result = visitor.visit(Syntax(node))

    for diag in visitor.diagnostics {
      context.diagnose(diag)
    }

    return result.asProtocol(FreestandingMacroExpansionSyntax.self)!.argumentList.first!.expression
  }
}

public class RecursiveMacro: ExpressionMacro {
  public static func expansion(
    of macro: some FreestandingMacroExpansionSyntax,
    in context: some MacroExpansionContext
  ) -> ExprSyntax {
    guard let argument = macro.argumentList.first?.expression,
          argument.description == "false" else {
      return "\(macro)"
    }

    return "()"
  }
}

public class NestedDeclInExprMacro: ExpressionMacro {
  public static func expansion(
    of macro: some FreestandingMacroExpansionSyntax,
    in context: some MacroExpansionContext
  ) -> ExprSyntax {
    return """
    { () -> Void in
      struct Foo { }
      return ()
    }
    """
  }
}

enum CustomError: Error, CustomStringConvertible {
  case message(String)

  var description: String {
    switch self {
    case .message(let text):
      return text
    }
  }
}

public struct EmptyDeclarationMacro: DeclarationMacro {
  public static func expansion(
    of node: some FreestandingMacroExpansionSyntax,
    in context: some MacroExpansionContext
  ) throws -> [DeclSyntax] {
    return []
  }
}

public struct DefineBitwidthNumberedStructsMacro: DeclarationMacro {
  public static func expansion(
    of node: some FreestandingMacroExpansionSyntax,
    in context: some MacroExpansionContext
  ) throws -> [DeclSyntax] {
    guard let firstElement = node.argumentList.first,
          let stringLiteral = firstElement.expression.as(StringLiteralExprSyntax.self),
          stringLiteral.segments.count == 1,
          case let .stringSegment(prefix) = stringLiteral.segments.first else {
      throw CustomError.message("#bitwidthNumberedStructs macro requires a string literal")
    }

    if prefix.content.text == "BUG" {
      return [
        """

        struct \(raw: prefix) {
          func \(context.makeUniqueName("method"))() { return 0 }
          func \(context.makeUniqueName("method"))() { return 1 }
        }
        """
      ]
    }

    return [8, 16, 32, 64].map { bitwidth in
      """

      struct \(raw: prefix)\(raw: String(bitwidth)) {
        func \(context.makeUniqueName("method"))() { }
        func \(context.makeUniqueName("method"))() { }
      }
      """
    }
  }
}

public struct DefineDeclsWithKnownNamesMacro: DeclarationMacro {
  public static func expansion(
    of node: some FreestandingMacroExpansionSyntax,
    in context: some MacroExpansionContext
  ) throws -> [DeclSyntax] {
    return [
      """

      struct A {
        func \(context.makeUniqueName("method"))() { }
        func \(context.makeUniqueName("method"))() { }
      }
      """,
      """

      struct B {
        func \(context.makeUniqueName("method"))() { }
        func \(context.makeUniqueName("method"))() { }
      }
      """,
      """

      var foo: Int {
          1
      }
      """,
      """

      var addOne: (Int) -> Int { { $0 + 1 } }
      """

      // FIXME:
      // 1. Stored properties are not visited in IRGen
      //    let addTwo: (Int) -> Int = { $0 + 2 }
      // 2. Curry thunk at call sites
      //    func foo()
      //    Foo2.foo // AutoClosureExpr with invalid discriminator
    ]
  }
}

public struct WarningMacro: ExpressionMacro {
   public static func expansion(
     of macro: some FreestandingMacroExpansionSyntax,
     in context: some MacroExpansionContext
   ) throws -> ExprSyntax {
     guard let firstElement = macro.argumentList.first,
       let stringLiteral = firstElement.expression
         .as(StringLiteralExprSyntax.self),
       stringLiteral.segments.count == 1,
       case let .stringSegment(messageString)? = stringLiteral.segments.first
     else {
       throw CustomError.message("#myWarning macro requires a string literal")
     }

     context.diagnose(
       Diagnostic(
         node: Syntax(macro),
         message: SimpleDiagnosticMessage(
           message: messageString.content.description,
           diagnosticID: MessageID(domain: "test", id: "error"),
           severity: .warning
         )
       )
     )

     return "()"
  }
}

public struct ErrorMacro: ExpressionMacro {
  public static func expansion(
    of macro: some FreestandingMacroExpansionSyntax,
    in context: some MacroExpansionContext
  ) throws -> ExprSyntax {
    guard let firstElement = macro.argumentList.first,
          let stringLiteral = firstElement.expression.as(StringLiteralExprSyntax.self),
          stringLiteral.segments.count == 1,
          case let .stringSegment(messageString)? = stringLiteral.segments.first
      else {
        let errorNode: Syntax
          if let firstElement = macro.argumentList.first {
          errorNode = Syntax(firstElement)
        } else {
          errorNode = Syntax(macro)
        }

        let messageID = MessageID(domain: "silly", id: "error")
        let diag = Diagnostic(
          node: errorNode,
          message: SimpleDiagnosticMessage(
            message: "#myError macro requires a string literal",
            diagnosticID: messageID,
            severity: .error
          )
        )

       throw DiagnosticsError(diagnostics: [diag])
     }

     context.diagnose(
       Diagnostic(
         node: Syntax(macro),
         message: SimpleDiagnosticMessage(
           message: messageString.content.description,
           diagnosticID: MessageID(domain: "test", id: "error"),
           severity: .error
         )
       )
     )

     return "()"
  }
}

public struct PropertyWrapperMacro {}

extension PropertyWrapperMacro: AccessorMacro, Macro {
  public static func expansion(
    of node: AttributeSyntax,
    providingAccessorsOf declaration: some DeclSyntaxProtocol,
    in context: some MacroExpansionContext
  ) throws -> [AccessorDeclSyntax] {
    guard let varDecl = declaration.as(VariableDeclSyntax.self),
      let binding = varDecl.bindings.first,
      let identifier = binding.pattern.as(IdentifierPatternSyntax.self)?.identifier,
      binding.accessor == nil
    else {
      return []
    }

    return [
      """

        get {
          _\(identifier).wrappedValue
        }
      """,
      """

        set {
          _\(identifier).wrappedValue = newValue
        }
      """,
    ]
  }
}

extension PropertyWrapperMacro: PeerMacro {
  public static func expansion(
    of node: AttributeSyntax,
    providingPeersOf declaration: some DeclSyntaxProtocol,
    in context: some MacroExpansionContext
  ) throws -> [DeclSyntax] {
    guard let varDecl = declaration.as(VariableDeclSyntax.self),
      let binding = varDecl.bindings.first,
      let identifier = binding.pattern.as(IdentifierPatternSyntax.self)?.identifier,
      binding.accessor == nil,
      let type = binding.typeAnnotation?.type
    else {
      return []
    }

    return [
      """
      var _\(raw: identifier.trimmedDescription): MyWrapperThingy<\(type)>
      """
    ]
  }
}

public struct WrapAllProperties: MemberAttributeMacro {
  public static func expansion(
    of node: AttributeSyntax,
    attachedTo parent: some DeclGroupSyntax,
    providingAttributesFor member: some DeclSyntaxProtocol,
    in context: some MacroExpansionContext
  ) throws -> [AttributeSyntax] {
    guard member.is(VariableDeclSyntax.self) else {
      return []
    }

    let wrapperTypeName: String
    if parent.is(ClassDeclSyntax.self) {
      wrapperTypeName = "EnclosingSelfWrapper"
    } else {
      wrapperTypeName = "Wrapper"
    }

    let propertyWrapperAttr = AttributeSyntax(
      attributeName: SimpleTypeIdentifierSyntax(
        name: .identifier(wrapperTypeName)
      )
    )

    return [propertyWrapperAttr]
  }
}

public struct TypeWrapperMacro {}

extension TypeWrapperMacro: MemberAttributeMacro {
  public static func expansion(
    of node: AttributeSyntax,
    attachedTo decl: some DeclGroupSyntax,
    providingAttributesFor member: some DeclSyntaxProtocol,
    in context: some MacroExpansionContext
  ) throws -> [AttributeSyntax] {
    guard let varDecl = member.as(VariableDeclSyntax.self),
      let binding = varDecl.bindings.first,
      let identifier = binding.pattern.as(IdentifierPatternSyntax.self)?.identifier,
      binding.accessor == nil
    else {
      return []
    }

    if identifier.text == "_storage" {
      return []
    }

    let customAttr = AttributeSyntax(
      attributeName: SimpleTypeIdentifierSyntax(
        name: .identifier("accessViaStorage")
      )
    )

    return [customAttr]
  }
}

extension TypeWrapperMacro: MemberMacro {
  public static func expansion(
    of node: AttributeSyntax,
    providingMembersOf decl: some DeclGroupSyntax,
    in context: some MacroExpansionContext
  ) throws -> [DeclSyntax] {
    let storageVariable: DeclSyntax =
      """
      private var _storage = _Storage()
      """

    return [
      storageVariable,
    ]
  }
}

public struct AccessViaStorageMacro: AccessorMacro {
  public static func expansion(
    of node: AttributeSyntax,
    providingAccessorsOf declaration: some DeclSyntaxProtocol,
    in context: some MacroExpansionContext
  ) throws -> [AccessorDeclSyntax] {
    guard let varDecl = declaration.as(VariableDeclSyntax.self),
      let binding = varDecl.bindings.first,
      let identifier = binding.pattern.as(IdentifierPatternSyntax.self)?.identifier,
      binding.accessor == nil
    else {
      return []
    }

    if identifier.text == "_storage" {
      return []
    }

    return [
      "get { _storage.\(identifier) }",
      "set { _storage.\(identifier) = newValue }",
    ]
  }
}

public struct AddMembers: MemberMacro {
  public static func expansion(
    of node: AttributeSyntax,
    providingMembersOf decl: some DeclGroupSyntax,
    in context: some MacroExpansionContext
  ) throws -> [DeclSyntax] {
    let uniqueClassName = context.makeUniqueName("uniqueClass")

    let storageStruct: DeclSyntax =
      """
      struct Storage {}
      """

    let storageVariable: DeclSyntax =
      """
      private var storage = Storage()
      """

    let instanceMethod: DeclSyntax =
      """
      func getStorage() -> Storage {
        print("synthesized method")
        return storage
      }
      """

    let staticMethod: DeclSyntax =
      """
      static func method() {}
      """

    let initDecl: DeclSyntax =
      """
      init() { _ = \(uniqueClassName)() }
      """

    let classDecl: DeclSyntax =
      """
      class \(uniqueClassName) { }
      """

    return [
      storageStruct,
      storageVariable,
      instanceMethod,
      staticMethod,
      initDecl,
      classDecl,
    ]
  }
}

public struct AddExtMembers: MemberMacro {
  public static func expansion(
    of node: AttributeSyntax,
    providingMembersOf decl: some DeclGroupSyntax,
    in context: some MacroExpansionContext
  ) throws -> [DeclSyntax] {
    let uniqueClassName = context.makeUniqueName("uniqueClass")

    let instanceMethod: DeclSyntax =
      """
      func extInstanceMethod() {}
      """

    let staticMethod: DeclSyntax =
      """
      static func extStaticMethod() {}
      """

    let classDecl: DeclSyntax =
      """
      class \(uniqueClassName) { }
      """

    return [
      instanceMethod,
      staticMethod,
      classDecl,
    ]
  }
}

public struct AddArbitraryMembers: MemberMacro {
  public static func expansion(
    of node: AttributeSyntax,
    providingMembersOf decl: some DeclGroupSyntax,
    in context: some MacroExpansionContext
  ) throws -> [DeclSyntax] {
    guard let identified = decl.asProtocol(IdentifiedDeclSyntax.self) else {
      return []
    }

    let parentName = identified.identifier.trimmed
    return [
      "struct \(parentName)1 {}",
      "struct \(parentName)2 {}",
      "struct \(parentName)3 {}",
    ]
  }
}

/// Implementation of the `wrapStoredProperties` macro, which can be
/// used to apply an attribute to all of the stored properties of a type.
///
/// This macro demonstrates member-attribute macros.
public struct WrapStoredPropertiesMacro: MemberAttributeMacro {
  public static func expansion<
    Declaration: DeclGroupSyntax,
    Context: MacroExpansionContext
  >(
    of node: AttributeSyntax,
    attachedTo decl: Declaration,
    providingAttributesFor member: some DeclSyntaxProtocol,
    in context: Context
  ) throws -> [AttributeSyntax] {
    guard let property = member.as(VariableDeclSyntax.self),
          property.isStoredProperty
    else {
      return []
    }

    guard case let .argumentList(arguments) = node.argument,
        let firstElement = arguments.first,
        let stringLiteral = firstElement.expression
      .as(StringLiteralExprSyntax.self),
          stringLiteral.segments.count == 1,
          case let .stringSegment(wrapperName)? = stringLiteral.segments.first else {
      throw CustomError.message("macro requires a string literal containing the name of an attribute")
    }

    return [
      AttributeSyntax(
        attributeName: SimpleTypeIdentifierSyntax(
          name: .identifier(wrapperName.content.text)
        )
      )
    ]
  }
}

extension VariableDeclSyntax {
  /// Determine whether this variable has the syntax of a stored property.
  ///
  /// This syntactic check cannot account for semantic adjustments due to,
  /// e.g., accessor macros or property wrappers.
  var isStoredProperty: Bool {
    if bindings.count != 1 {
      return false
    }

    let binding = bindings.first!
    switch binding.accessor {
    case .none:
      return true

    case .accessors(let node):
      for accessor in node.accessors {
        switch accessor.accessorKind.tokenKind {
        case .keyword(.willSet), .keyword(.didSet):
          // Observers can occur on a stored property.
          break

        default:
          // Other accessors make it a computed property.
          return false
        }
      }

      return true

    case .getter:
      return false

    @unknown default:
      return false
    }
  }
}

extension DeclGroupSyntax {
  /// Enumerate the stored properties that syntactically occur in this
  /// declaration.
  func storedProperties() -> [VariableDeclSyntax] {
    return memberBlock.members.compactMap { member in
      guard let variable = member.decl.as(VariableDeclSyntax.self),
            variable.isStoredProperty else {
        return nil
      }

      return variable
    }
  }
}

public enum LeftHandOperandFinderMacro: ExpressionMacro {
  class Visitor<Context: MacroExpansionContext>: SyntaxVisitor {
    let context: Context

    init(context: Context) {
      self.context = context
      super.init(viewMode: .sourceAccurate)
    }

    override func visit(
      _ node: InfixOperatorExprSyntax
    ) -> SyntaxVisitorContinueKind {
      guard let lhsStartLoc = context.location(of: node.leftOperand),
            let lhsEndLoc = context.location(
              of: node.leftOperand, at: .beforeTrailingTrivia, filePathMode: .fileID
            ) else {
        fatalError("missing source location information")
      }

      print("Source range for LHS is \(lhsStartLoc.file): \(lhsStartLoc.line):\(lhsStartLoc.column)-\(lhsEndLoc.line):\(lhsEndLoc.column)")

      return .visitChildren
    }
  }

  public static func expansion(
    of node: some FreestandingMacroExpansionSyntax,
    in context: some MacroExpansionContext
  ) -> ExprSyntax {
    let visitor = Visitor(context: context)
    visitor.walk(node)

    return node.argumentList.first!.expression
  }
}

public struct AddCompletionHandler: PeerMacro {
  public static func expansion(
    of node: AttributeSyntax,
    providingPeersOf declaration: some DeclSyntaxProtocol,
    in context: some MacroExpansionContext
  ) throws -> [DeclSyntax] {
    // Only on functions at the moment. We could handle initializers as well
    // with a bit of work.
    guard let funcDecl = declaration.as(FunctionDeclSyntax.self) else {
      throw CustomError.message("@addCompletionHandler only works on functions")
    }

    // This only makes sense for async functions.
    if funcDecl.signature.effectSpecifiers?.asyncSpecifier == nil {
      throw CustomError.message(
        "@addCompletionHandler requires an async function"
      )
    }

    // Form the completion handler parameter.
    let resultType: TypeSyntax? = funcDecl.signature.output?.returnType.with(\.leadingTrivia, []).with(\.trailingTrivia, [])

    let completionHandlerParam =
      FunctionParameterSyntax(
        firstName: .identifier("completionHandler"),
        colon: .colonToken(),
        type: "@escaping (\(resultType ?? "")) -> Void" as TypeSyntax
      )

    // Add the completion handler parameter to the parameter list.
    let parameterList = funcDecl.signature.input.parameterList
    let newParameterList: FunctionParameterListSyntax
    if let lastParam = parameterList.last {
      // We need to add a trailing comma to the preceding list.
      newParameterList = parameterList.removingLast()
        .appending(
          lastParam.with(
            \.trailingComma,
            .commaToken()
          )
        )
        .appending(completionHandlerParam)
    } else {
      newParameterList = parameterList.appending(completionHandlerParam)
    }

    let callArguments: [String] = parameterList.map { param in
      let argName = param.secondName ?? param.firstName

      if param.firstName.text != "_" {
        return "\(param.firstName.text): \(argName.text)"
      }

      return "\(argName.text)"
    }

    let call: ExprSyntax =
      "\(funcDecl.identifier)(\(raw: callArguments.joined(separator: ", ")))"

    // FIXME: We should make CodeBlockSyntax ExpressibleByStringInterpolation,
    // so that the full body could go here.
    let newBody: ExprSyntax =
      """
        Task {
          completionHandler(await \(call))
        }
      """

    // Drop the @addCompletionHandler attribute from the new declaration.
    let newAttributeList = AttributeListSyntax(
      funcDecl.attributes?.filter {
        guard case let .attribute(attribute) = $0,
              let attributeType = attribute.attributeName.as(SimpleTypeIdentifierSyntax.self),
              let nodeType = node.attributeName.as(SimpleTypeIdentifierSyntax.self)
        else {
          return true
        }

        return attributeType.name.text != nodeType.name.text
      } ?? []
    )

    let newFunc =
      funcDecl
      .with(
        \.signature,
        funcDecl.signature
          .with(
            \.effectSpecifiers,
            funcDecl.signature.effectSpecifiers?.with(\.asyncSpecifier, nil)  // drop async
          )
          .with(\.output, nil)  // drop result type
          .with(
            \.input,  // add completion handler parameter
            funcDecl.signature.input.with(\.parameterList, newParameterList)
              .with(\.trailingTrivia, [])
          )
      )
      .with(
        \.body,
        CodeBlockSyntax(
          leftBrace: .leftBraceToken(),
          statements: CodeBlockItemListSyntax(
            [CodeBlockItemSyntax(item: .expr(newBody))]
          ),
          rightBrace: .rightBraceToken()
        )
      )
      .with(\.attributes, newAttributeList)

    return [DeclSyntax(newFunc)]
  }
}

public struct InvalidMacro: PeerMacro, DeclarationMacro {
  public static func expansion(
    of node: AttributeSyntax,
    providingPeersOf declaration: some DeclSyntaxProtocol,
    in context: some MacroExpansionContext
  ) throws -> [DeclSyntax] {
    return [
      "import Swift",
      "precedencegroup MyPrecedence {}",
      "@attached(member) macro myMacro()",
      "extension Int {}",
      """
      @main
      struct MyMain {
        static func main() {}
      }
      """,
      "typealias Array = Void",
      "typealias Dictionary = Void",
      "typealias BooleanLiteralType = Void",
      "typealias ExtendedGraphemeClusterType = Void",
      "typealias FloatLiteralType = Void",
      "typealias IntegerLiteralType = Void",
      "typealias StringLiteralType = Void",
      "typealias UnicodeScalarType = Void",
      "typealias _ColorLiteralType = Void",
      "typealias _ImageLiteralType = Void",
      "typealias _FileReferenceLiteralType = Void",
    ]
  }

  public static func expansion(
    of node: some FreestandingMacroExpansionSyntax,
    in context: some MacroExpansionContext
  ) throws -> [DeclSyntax] {
    return [
      "var value: Int"
    ]
  }
}

public struct CoerceToIntMacro: ExpressionMacro {
  public static func expansion(
    of node: some FreestandingMacroExpansionSyntax,
    in context: some MacroExpansionContext
  ) -> ExprSyntax {
    "\(node.argumentList.first!.expression) as Int"
  }
}

public struct WrapInType: PeerMacro {
  public static func expansion(
    of node: AttributeSyntax,
    providingPeersOf declaration: some DeclSyntaxProtocol,
    in context: some MacroExpansionContext
  ) throws -> [DeclSyntax] {
    guard let funcDecl = declaration.as(FunctionDeclSyntax.self) else {
      throw CustomError.message("@wrapInType only applies to functions")
    }

    // Build a new function with the same signature that forwards arguments
    // to the the original function.
    let parameterList = funcDecl.signature.input.parameterList
    let callArguments: [String] = parameterList.map { param in
      let argName = param.secondName ?? param.firstName

      if param.firstName.text != "_" {
        return "\(param.firstName.text): \(argName.text)"
      }

      return "\(argName.text)"
    }

    let call: ExprSyntax =
      """
      \(funcDecl.identifier)(\(raw: callArguments.joined(separator: ", ")))
      """

    // Drop the peer macro attribute from the new declaration.
    let newAttributeList = AttributeListSyntax(
      funcDecl.attributes?.filter {
        guard case let .attribute(attribute) = $0,
              let attributeType = attribute.attributeName.as(SimpleTypeIdentifierSyntax.self),
              let nodeType = node.attributeName.as(SimpleTypeIdentifierSyntax.self)
        else {
          return true
        }

        return attributeType.name.text != nodeType.name.text
      } ?? []
    )

    let method =
      funcDecl
      .with(
        \.identifier,
         "\(context.makeUniqueName(funcDecl.identifier.text))"
      )
      .with(
        \.signature,
        funcDecl.signature
      )
      .with(
        \.body,
        CodeBlockSyntax(
          leftBrace: .leftBraceToken(),
          statements: CodeBlockItemListSyntax(
            [CodeBlockItemSyntax(item: .expr(call))]
          ),
          rightBrace: .rightBraceToken()
        )
      )
      .with(\.attributes, newAttributeList)

    let structType: DeclSyntax =
      """
      struct \(context.makeUniqueName(funcDecl.identifier.text)) {
        \(method)
      }
      """

    return [structType]
  }
}

private extension DeclSyntaxProtocol {
  var isObservableStoredProperty: Bool {
    if let property = self.as(VariableDeclSyntax.self),
          let binding = property.bindings.first,
          let identifier = binding.pattern.as(IdentifierPatternSyntax.self)?.identifier,
          identifier.text != "_registrar", identifier.text != "_storage",
          binding.accessor == nil {
      return true
    }

    return false
  }
}

public struct ObservableMacro: MemberMacro, MemberAttributeMacro {

  // MARK: - MemberMacro

  public static func expansion(
    of node: AttributeSyntax,
    providingMembersOf declaration: some DeclGroupSyntax,
    in context: some MacroExpansionContext
  ) throws -> [DeclSyntax] {
    guard let identified = declaration.asProtocol(IdentifiedDeclSyntax.self) else {
      return []
    }

    let parentName = identified.identifier

    let registrar: DeclSyntax =
      """
      let _registrar = ObservationRegistrar<\(parentName)>()
      """

    let addObserver: DeclSyntax =
      """
      public nonisolated func addObserver(_ observer: some Observer<\(parentName)>) {
        _registrar.addObserver(observer)
      }
      """

    let removeObserver: DeclSyntax =
      """
      public nonisolated func removeObserver(_ observer: some Observer<\(parentName)>) {
        _registrar.removeObserver(observer)
      }
      """

    let withTransaction: DeclSyntax =
      """
      private func withTransaction<T>(_ apply: () throws -> T) rethrows -> T {
        _registrar.beginAccess()
        defer { _registrar.endAccess() }
        return try apply()
      }
      """

    let memberList = MemberDeclListSyntax(
      declaration.memberBlock.members.filter {
        $0.decl.isObservableStoredProperty
      }
    )

    let storageStruct: DeclSyntax =
      """
      private struct Storage {
      \(memberList)
      }
      """

    let storage: DeclSyntax =
      """
      private var _storage = Storage()
      """

    return [
      registrar,
      addObserver,
      removeObserver,
      withTransaction,
      storageStruct,
      storage,
    ]
  }

  // MARK: - MemberAttributeMacro

  public static func expansion(
    of node: AttributeSyntax,
    attachedTo declaration: some DeclGroupSyntax,
    providingAttributesFor member: some DeclSyntaxProtocol,
    in context: some MacroExpansionContext
  ) throws -> [SwiftSyntax.AttributeSyntax] {
    guard member.isObservableStoredProperty else {
      return []
    }

    return [
      AttributeSyntax(
        attributeName: SimpleTypeIdentifierSyntax(
          name: .identifier("ObservableProperty")
        )
      )
    ]
  }

}

public struct ObservablePropertyMacro: AccessorMacro {
  public static func expansion(
    of node: AttributeSyntax,
    providingAccessorsOf declaration: some DeclSyntaxProtocol,
    in context: some MacroExpansionContext
  ) throws -> [AccessorDeclSyntax] {
    guard let property = declaration.as(VariableDeclSyntax.self),
      let binding = property.bindings.first,
      let identifier = binding.pattern.as(IdentifierPatternSyntax.self)?.identifier,
      binding.accessor == nil
    else {
      return []
    }

    let getAccessor: AccessorDeclSyntax =
      """
      get {
        _registrar.beginAccess(\\.\(identifier))
        defer { _registrar.endAccess() }
        return _storage.\(identifier)
      }
      """

    let setAccessor: AccessorDeclSyntax =
      """
      set {
        _registrar.beginAccess(\\.\(identifier))
        _registrar.register(observable: self, willSet: \\.\(identifier), to: newValue)
        defer {
          _registrar.register(observable: self, didSet: \\.\(identifier))
          _registrar.endAccess()
        }
        _storage.\(identifier) = newValue
      }
      """

    return [getAccessor, setAccessor]
  }
}

extension DeclModifierSyntax {
  fileprivate var isNeededAccessLevelModifier: Bool {
    switch self.name.tokenKind {
    case .keyword(.public): return true
    default: return false
    }
  }
}

extension SyntaxStringInterpolation {
  fileprivate mutating func appendInterpolation<Node: SyntaxProtocol>(_ node: Node?) {
    if let node {
      appendInterpolation(node)
    }
  }
}

public struct NewTypeMacro: MemberMacro {
  public static func expansion(
    of node: AttributeSyntax,
    providingMembersOf declaration: some DeclGroupSyntax,
    in context: some MacroExpansionContext
  ) throws -> [DeclSyntax] {
    guard let type = node.attributeName.as(SimpleTypeIdentifierSyntax.self),
          let genericArguments = type.genericArgumentClause?.arguments,
          genericArguments.count == 1,
          let rawType = genericArguments.first
    else {
      throw CustomError.message(#"@NewType requires the raw type as an argument, in the form "<RawType>"."#)
    }

    guard let declaration = declaration.as(StructDeclSyntax.self) else {
      throw CustomError.message("@NewType can only be applied to a struct declarations.")
    }

    let access = declaration.modifiers?.first(where: \.isNeededAccessLevelModifier)

    return [
      "\(access)typealias RawValue = \(rawType)",
      "\(access)var rawValue: RawValue",
      "\(access)init(_ rawValue: RawValue) { self.rawValue = rawValue }",
    ]
  }
}

public struct EmptyMacro: MemberMacro {
  public static func expansion(
    of node: AttributeSyntax,
    providingMembersOf decl: some DeclGroupSyntax,
    in context: some MacroExpansionContext
  ) throws -> [DeclSyntax] {
    return []
  }
}

public struct EquatableMacro: ConformanceMacro {
  public static func expansion(
    of node: AttributeSyntax,
    providingConformancesOf decl: some DeclGroupSyntax,
    in context: some MacroExpansionContext
  ) throws -> [(TypeSyntax, GenericWhereClauseSyntax?)] {
    let protocolName: TypeSyntax = "Equatable"
    return [(protocolName, nil)]
  }
}

public struct HashableMacro: ConformanceMacro {
  public static func expansion(
    of node: AttributeSyntax,
    providingConformancesOf decl: some DeclGroupSyntax,
    in context: some MacroExpansionContext
  ) throws -> [(TypeSyntax, GenericWhereClauseSyntax?)] {
    let protocolName: TypeSyntax = "Hashable"
    return [(protocolName, nil)]
  }
}

public struct DelegatedConformanceMacro: ConformanceMacro, MemberMacro {
  public static func expansion(
    of node: AttributeSyntax,
    providingConformancesOf decl: some DeclGroupSyntax,
    in context: some MacroExpansionContext
  ) throws -> [(TypeSyntax, GenericWhereClauseSyntax?)] {
    let protocolName: TypeSyntax = "P"
    let conformance: DeclSyntax =
      """
      extension Placeholder where Element: P {}
      """

    guard let extensionDecl = conformance.as(ExtensionDeclSyntax.self) else {
      return []
    }

    return [(protocolName, extensionDecl.genericWhereClause)]
  }

  public static func expansion(
    of node: AttributeSyntax,
    providingMembersOf decl: some DeclGroupSyntax,
    in context: some MacroExpansionContext
  ) throws -> [DeclSyntax] {
    let requirement: DeclSyntax =
      """
      static func requirement() where Element : P {
        Element.requirement()
      }
      """

    return [requirement]
  }
}

public struct ExtendableEnum: MemberMacro {
  public static func expansion(
    of node: AttributeSyntax,
    providingMembersOf declaration: some DeclGroupSyntax,
    in context: some MacroExpansionContext
  ) throws -> [DeclSyntax] {
    let unknownDecl: DeclSyntax =
    """
    func unknown() -> Int { 34 } // or something like: `case unknown`
    """
    return [unknownDecl]
  }
}

public struct DefineStructWithUnqualifiedLookupMacro: DeclarationMacro {
  public static func expansion(
    of node: some FreestandingMacroExpansionSyntax,
    in context: some MacroExpansionContext
  ) throws -> [DeclSyntax] {
    return ["""
    struct StructWithUnqualifiedLookup {
      let hello = 1

      func foo() -> Int {
        hello + world // looks up "world" in the parent scope
      }
    }
    """]
  }
}

extension TupleExprElementListSyntax {
  /// Retrieve the first element with the given label.
  func first(labeled name: String) -> Element? {
    return first { element in
      if let label = element.label, label.text == name {
        return true
      }

      return false
    }
  }
}


public struct DefineAnonymousTypesMacro: DeclarationMacro {
  public static func expansion(
    of node: some FreestandingMacroExpansionSyntax,
    in context: some MacroExpansionContext
  ) throws -> [DeclSyntax] {
    guard let body = node.trailingClosure else {
      throw CustomError.message("#anonymousTypes macro requires a trailing closure")
    }

    let accessSpecifier: String
    if let _ = node.argumentList.first(labeled: "public") {
      accessSpecifier = "public "
    } else {
      accessSpecifier = ""
    }
    return [
      """

      \(raw:accessSpecifier)class \(context.makeUniqueName("name")) {
        \(raw:accessSpecifier)func hello() -> String {
          \(body.statements)
        }

        \(raw:accessSpecifier)func getSelf() -> Any.Type { return Self.self }
      }
      """,
      """

      enum \(context.makeUniqueName("name")) {
        case apple
        case banana

        func hello() -> String {
          \(body.statements)
        }
      }
      """
    ]
  }
}

public struct AddClassReferencingSelfMacro: PeerMacro {
  public static func expansion(
    of node: AttributeSyntax,
    providingPeersOf declaration: some DeclSyntaxProtocol,
    in context: some MacroExpansionContext
  ) throws -> [DeclSyntax] {
    guard let protocolDecl = declaration.as(ProtocolDeclSyntax.self) else {
      throw CustomError.message("Macro can only be applied to a protocol declarations.")
    }

    let className = "\(protocolDecl.identifier.text)Builder"
    return [
      """
      struct \(raw: className) {
       init(_ build: (_ builder: Self) -> Self) {
         _ = build(self)
       }
      }
      """
    ]
  }
}

public struct SimpleCodeItemMacro: CodeItemMacro {
  public static func expansion(
    of node: some FreestandingMacroExpansionSyntax,
    in context: some MacroExpansionContext
  ) throws -> [CodeBlockItemSyntax] {
    [
      .init(item: .decl("""

      struct \(context.makeUniqueName("foo")) {
        var x: Int
      }
      """)),
      .init(item: .stmt("""

      if true {
        print("from stmt")
        usedInExpandedStmt()
      }
      if false {
        print("impossible")
      }
      """)),
      .init(item: .expr("""

      print("from expr")
      """)),
    ]
  }
}

public struct MultiStatementClosure: ExpressionMacro {
  public static func expansion(
    of node: some FreestandingMacroExpansionSyntax,
    in context: some MacroExpansionContext
  ) throws -> ExprSyntax {
    return """
      {
        let temp = 10
        let result = temp
        return result
      }()
      """
  }
}

public struct VarValueMacro: DeclarationMacro, PeerMacro {
  public static func expansion(
    of macro: some FreestandingMacroExpansionSyntax,
    in context: some MacroExpansionContext
  ) -> [DeclSyntax] {
    return [
      "var value: Int { 1 }"
    ]
  }

  public static func expansion(
    of node: AttributeSyntax,
    providingPeersOf declaration: some DeclSyntaxProtocol,
    in context: some MacroExpansionContext
  ) throws -> [DeclSyntax] {
    return [
      "var value: Int { 1 }"
    ]
  }
}
