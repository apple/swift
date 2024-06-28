import SwiftDiagnostics
import SwiftOperators
import SwiftSyntax
import SwiftSyntaxBuilder
@_spi(ExperimentalLanguageFeature) import SwiftSyntaxMacros

/// Replace the label of the first element in the tuple with the given
/// new label.
private func replaceFirstLabel(
  of tuple: LabeledExprListSyntax, with newLabel: String
) -> LabeledExprListSyntax{
  if tuple.isEmpty {
    return tuple
  }

  return tuple.with(\.[tuple.startIndex].label, .identifier(newLabel))
}

public struct ColorLiteralMacro: ExpressionMacro {
  public static func expansion(
    of macro: some FreestandingMacroExpansionSyntax,
    in context: some MacroExpansionContext
  ) -> ExprSyntax {
    let argList = replaceFirstLabel(
      of: macro.arguments, with: "_colorLiteralRed"
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
    guard let argument = macro.arguments.first?.expression else {
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
    guard let argument = macro.arguments.first?.expression else {
      fatalError("boom")
    }

    return "(\(argument), \(StringLiteralExprSyntax(content: argument.description)))"
  }
}

public struct ExprAndDeclMacro: ExpressionMacro, DeclarationMacro {
  public static func expansion(
    of macro: some FreestandingMacroExpansionSyntax,
    in context: some MacroExpansionContext
  ) -> ExprSyntax {
    guard let argument = macro.arguments.first?.expression else {
      fatalError("boom")
    }

    return "(\(argument), \(StringLiteralExprSyntax(content: argument.description)))"
  }

  public static func expansion(
    of macro: some FreestandingMacroExpansionSyntax,
    in context: some MacroExpansionContext
  ) -> [DeclSyntax] {
    return []
  }
}

public struct StringifyAndTryMacro: ExpressionMacro {
  public static func expansion(
    of macro: some FreestandingMacroExpansionSyntax,
    in context: some MacroExpansionContext
  ) -> ExprSyntax {
    guard let argument = macro.arguments.first?.expression else {
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
      if let binOp = node.operator.as(BinaryOperatorExprSyntax.self) {
        if binOp.operator.text == "+" {
          let messageID = MessageID(domain: "silly", id: "addblock")
          diagnostics.append(
            Diagnostic(
              node: Syntax(node.operator),
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
                      oldNode: Syntax(binOp.operator),
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

          let minusOperator = binOp.with(\.operator.tokenKind, .binaryOperator("-"))
          return ExprSyntax(node.with(\.operator, ExprSyntax(minusOperator)))
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
    let result = visitor.rewrite(Syntax(node))

    for diag in visitor.diagnostics {
      context.diagnose(diag)
    }

    return result.asProtocol(FreestandingMacroExpansionSyntax.self)!.arguments.first!.expression
  }
}

public class RecursiveMacro: ExpressionMacro {
  public static func expansion(
    of macro: some FreestandingMacroExpansionSyntax,
    in context: some MacroExpansionContext
  ) -> ExprSyntax {
    guard let argument = macro.arguments.first?.expression,
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

public class NullaryFunctionCallMacro: ExpressionMacro {
  public static func expansion(
    of macro: some FreestandingMacroExpansionSyntax,
    in context: some MacroExpansionContext
  ) -> ExprSyntax {
    let calls = macro.arguments.compactMap(\.expression).map { "\($0)()" }
    return "(\(raw: calls.joined(separator: ", ")))"
  }
}

public class TupleMacro: ExpressionMacro {
  public static func expansion(
    of macro: some FreestandingMacroExpansionSyntax,
    in context: some MacroExpansionContext
  ) -> ExprSyntax {
    return "(\(raw: macro.arguments.map { "\($0)" }.joined()))"
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
    guard let firstElement = node.arguments.first,
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

public struct VarDeclMacro: CodeItemMacro {
  public static func expansion(
    of node: some FreestandingMacroExpansionSyntax,
    in context: some MacroExpansionContext
  ) throws -> [CodeBlockItemSyntax] {
    let name = context.makeUniqueName("fromMacro")
    return [
      "let \(name) = 23",
      "use(\(name))",
      """
      if true {
        let \(name) = "string"
        use(\(name))
      }
      """
    ]
  }
}

public struct WarningMacro: ExpressionMacro {
   public static func expansion(
     of macro: some FreestandingMacroExpansionSyntax,
     in context: some MacroExpansionContext
   ) throws -> ExprSyntax {
     guard let firstElement = macro.arguments.first,
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
    guard let firstElement = macro.arguments.first,
          let stringLiteral = firstElement.expression.as(StringLiteralExprSyntax.self),
          stringLiteral.segments.count == 1,
          case let .stringSegment(messageString)? = stringLiteral.segments.first
      else {
        let errorNode: Syntax
          if let firstElement = macro.arguments.first {
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
      let identifier = binding.pattern.as(IdentifierPatternSyntax.self)?.identifier
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
      binding.accessorBlock == nil,
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

extension AccessorBlockSyntax {
  var hasGetter: Bool {
    switch self.accessors {
    case .accessors(let accessors):
      for accessor in accessors {
        if accessor.accessorSpecifier.text == "get" {
          return true
        }
      }

      return false
    case .getter:
      return true
    @unknown default:
      return false
    }
  }
}

public struct PropertyWrapperSkipsComputedMacro {}

extension PropertyWrapperSkipsComputedMacro: AccessorMacro, Macro {
  public static func expansion(
    of node: AttributeSyntax,
    providingAccessorsOf declaration: some DeclSyntaxProtocol,
    in context: some MacroExpansionContext
  ) throws -> [AccessorDeclSyntax] {
    guard let varDecl = declaration.as(VariableDeclSyntax.self),
      let binding = varDecl.bindings.first,
      let identifier = binding.pattern.as(IdentifierPatternSyntax.self)?.identifier, !(binding.accessorBlock?.hasGetter ?? false)
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

public struct WillSetMacro: AccessorMacro {
  public static func expansion(
    of node: AttributeSyntax,
    providingAccessorsOf declaration: some DeclSyntaxProtocol,
    in context: some MacroExpansionContext
  ) throws -> [AccessorDeclSyntax] {
    return [
      """
        willSet { }
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
      attributeName: IdentifierTypeSyntax(
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
      binding.accessorBlock == nil
    else {
      return []
    }

    if identifier.text == "_storage" {
      return []
    }

    let customAttr = AttributeSyntax(
      attributeName: IdentifierTypeSyntax(
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
      binding.accessorBlock == nil
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
    guard let identified = decl.asProtocol(NamedDeclSyntax.self) else {
      return []
    }

    let parentName = identified.name.trimmed
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

    guard case let .argumentList(arguments) = node.arguments,
        let firstElement = arguments.first,
        let stringLiteral = firstElement.expression
      .as(StringLiteralExprSyntax.self),
          stringLiteral.segments.count == 1,
          case let .stringSegment(wrapperName)? = stringLiteral.segments.first else {
      throw CustomError.message("macro requires a string literal containing the name of an attribute")
    }

    return [
      AttributeSyntax(
        attributeName: IdentifierTypeSyntax(
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
    switch binding.accessorBlock?.accessors {
    case .none:
      return true

    case .accessors(let node):
      for accessor in node {
        switch accessor.accessorSpecifier.tokenKind {
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

    return node.arguments.first!.expression
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
    let resultType: TypeSyntax? = funcDecl.signature.returnClause?.type.with(\.leadingTrivia, []).with(\.trailingTrivia, [])

    let completionHandlerParam =
      FunctionParameterSyntax(
        firstName: .identifier("completionHandler"),
        colon: .colonToken(),
        type: "@escaping (\(resultType ?? "")) -> Void" as TypeSyntax
      )

    // Add the completion handler parameter to the parameter list.
    let parameterList = funcDecl.signature.parameterClause.parameters
    var newParameterList = parameterList
    if !newParameterList.isEmpty {
      // We need to add a trailing comma to the preceding list.
      let lastIndex = newParameterList.index(before: newParameterList.endIndex)
      newParameterList[lastIndex].trailingComma = .commaToken()
    }
    newParameterList.append(completionHandlerParam)

    let callArguments: [String] = parameterList.map { param in
      let argName = param.secondName ?? param.firstName

      if param.firstName.text != "_" {
        return "\(param.firstName.text): \(argName.text)"
      }

      return "\(argName.text)"
    }

    let call: ExprSyntax =
      "\(funcDecl.name)(\(raw: callArguments.joined(separator: ", ")))"

    // FIXME: We should make CodeBlockSyntax ExpressibleByStringInterpolation,
    // so that the full body could go here.
    let newBody: ExprSyntax =
      """
        Task {
          completionHandler(await \(call))
        }
      """

    // Drop the @addCompletionHandler attribute from the new declaration.
    let newAttributeList = funcDecl.attributes.filter {
      guard case let .attribute(attribute) = $0  else {
        return true
      }

      if let attributeType = attribute.attributeName.as(IdentifierTypeSyntax.self),
         let nodeType = node.attributeName.as(IdentifierTypeSyntax.self) {
        return attributeType.name.text != nodeType.name.text
      }
      if let attributeMemberType = attribute.attributeName.as(MemberTypeSyntax.self),
         let attributeModuleName = attributeMemberType.baseType.as(IdentifierTypeSyntax.self),
         let nodeMemberType = node.attributeName.as(MemberTypeSyntax.self),
         let moduleName = nodeMemberType.baseType.as(IdentifierTypeSyntax.self) {
        return attributeModuleName.name.text != moduleName.name.text ||
               nodeMemberType.name.text != attributeMemberType.name.text
      }

      return true
    }

    var newFunc = funcDecl
    newFunc.signature.effectSpecifiers?.asyncSpecifier = nil // drop async
    newFunc.signature.returnClause = nil  // drop result type
    newFunc.signature.parameterClause.parameters = newParameterList
    newFunc.signature.parameterClause.trailingTrivia = []
    newFunc.body = CodeBlockSyntax(
      leftBrace: .leftBraceToken(),
      statements: CodeBlockItemListSyntax(
        [CodeBlockItemSyntax(item: .expr(newBody))]
      ),
      rightBrace: .rightBraceToken()
    )
    newFunc.attributes = newAttributeList

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
    "\(node.arguments.first!.expression) as Int"
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
    // to the original function.
    let parameterList = funcDecl.signature.parameterClause.parameters
    let callArguments: [String] = parameterList.map { param in
      let argName = param.secondName ?? param.firstName

      if param.firstName.text != "_" {
        return "\(param.firstName.text): \(argName.text)"
      }

      return "\(argName.text)"
    }

    let call: ExprSyntax =
      """
      \(funcDecl.name)(\(raw: callArguments.joined(separator: ", ")))
      """

    // Drop the peer macro attribute from the new declaration.
    let newAttributeList = funcDecl.attributes.filter {
      guard case let .attribute(attribute) = $0,
            let attributeType = attribute.attributeName.as(IdentifierTypeSyntax.self),
            let nodeType = node.attributeName.as(IdentifierTypeSyntax.self)
      else {
        return true
      }

      return attributeType.name.text != nodeType.name.text
    }

    var method = funcDecl
    method.name = "\(context.makeUniqueName(funcDecl.name.text))"
    method.signature = funcDecl.signature
    method.body = CodeBlockSyntax(
      leftBrace: .leftBraceToken(),
      statements: CodeBlockItemListSyntax(
        [CodeBlockItemSyntax(item: .expr(call))]
      ),
      rightBrace: .rightBraceToken()
    )
    method.attributes = newAttributeList

    let structType: DeclSyntax =
      """
      struct \(context.makeUniqueName(funcDecl.name.text)) {
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
          binding.accessorBlock == nil {
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
    guard let identified = declaration.asProtocol(NamedDeclSyntax.self) else {
      return []
    }

    let parentName = identified.name

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

    let memberList = declaration.memberBlock.members.filter {
      $0.decl.isObservableStoredProperty
    }

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
        attributeName: IdentifierTypeSyntax(
          name: .identifier("ObservableProperty")
        )
      )
    ]
  }

}

extension ObservableMacro: ExtensionMacro {
  public static func expansion(
    of node: AttributeSyntax,
    attachedTo decl: some DeclGroupSyntax,
    providingExtensionsOf type: some TypeSyntaxProtocol,
    conformingTo protocols: [TypeSyntax],
    in context: some MacroExpansionContext
  ) throws -> [ExtensionDeclSyntax] {
    if (protocols.isEmpty) {
      return []
    }

    let decl: DeclSyntax =
      """
      extension \(raw: type.trimmedDescription): Observable {
      }
      """

    return [
      decl.cast(ExtensionDeclSyntax.self)
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
      binding.accessorBlock == nil
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
    guard let type = node.attributeName.as(IdentifierTypeSyntax.self),
          let genericArguments = type.genericArgumentClause?.arguments,
          genericArguments.count == 1,
          let rawType = genericArguments.first
    else {
      throw CustomError.message(#"@NewType requires the raw type as an argument, in the form "<RawType>"."#)
    }

    guard let declaration = declaration.as(StructDeclSyntax.self) else {
      throw CustomError.message("@NewType can only be applied to a struct declarations.")
    }

    let access = declaration.modifiers.first(where: \.isNeededAccessLevelModifier)

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

public struct EmptyPeerMacro: PeerMacro {
  public static func expansion(
    of node: AttributeSyntax,
    providingPeersOf declaration: some DeclSyntaxProtocol,
    in context: some MacroExpansionContext
  ) throws -> [DeclSyntax] {
    return []
  }
}

public struct EquatableMacro: ExtensionMacro {
  public static func expansion(
    of node: AttributeSyntax,
    attachedTo: some DeclGroupSyntax,
    providingExtensionsOf type: some TypeSyntaxProtocol,
    conformingTo protocols: [TypeSyntax],
    in context: some MacroExpansionContext
  ) throws -> [ExtensionDeclSyntax] {
    let ext: DeclSyntax = "extension \(type.trimmed): Equatable {}"
    return [ext.cast(ExtensionDeclSyntax.self)]
  }
}

public struct EquatableViaMembersMacro: ExtensionMacro {
  public static func expansion(
    of node: AttributeSyntax,
    attachedTo decl: some DeclGroupSyntax,
    providingExtensionsOf type: some TypeSyntaxProtocol,
    conformingTo protocols: [TypeSyntax],
    in context: some MacroExpansionContext
  ) throws -> [ExtensionDeclSyntax] {
    let comparisons: [String] = decl.storedProperties().map { property in
      guard let binding = property.bindings.first,
            let identifier = binding.pattern.as(IdentifierPatternSyntax.self)?.identifier else {
        return "true"
      }

      return "lhs.\(identifier) == rhs.\(identifier)"
    }

    let condition = comparisons.joined(separator: " && ")
    let equalOperator: DeclSyntax = """
      static func ==(lhs: \(type.trimmed), rhs: \(type.trimmed)) -> Bool {
        return \(raw: condition)
      }
      """

    let ext: DeclSyntax = """
      extension \(type.trimmed): Equatable {
        \(equalOperator)
      }
      """
    return [ext.cast(ExtensionDeclSyntax.self)]
  }
}

public struct FooExtensionMacro: ExtensionMacro {
  public static func expansion(of node: AttributeSyntax, attachedTo declaration: some DeclGroupSyntax, providingExtensionsOf type: some TypeSyntaxProtocol, conformingTo protocols: [TypeSyntax], in context: some MacroExpansionContext) throws -> [ExtensionDeclSyntax] {
    let decl: DeclSyntax =
    """
    extension Foo {
      var foo: String { "foo" }
      func printFoo() {
        print(foo)
      }
    }
    """
    guard let extensionDecl = decl.as(ExtensionDeclSyntax.self) else {
      return []
    }

    return [extensionDecl]
  }
}

public struct ConformanceViaExtensionMacro: ExtensionMacro {
  public static func expansion(
    of node: AttributeSyntax,
    attachedTo decl: some DeclGroupSyntax,
    providingExtensionsOf type: some TypeSyntaxProtocol,
    conformingTo protocols: [TypeSyntax],
    in context: some MacroExpansionContext
  ) throws -> [ExtensionDeclSyntax] {
    if (protocols.isEmpty) {
      return []
    }

    let decl: DeclSyntax =
      """
      extension \(raw: type.trimmedDescription): MyProtocol {
      }
      """

    return [
      decl.cast(ExtensionDeclSyntax.self)
    ]
  }
}

public struct HashableMacro: ExtensionMacro {
  public static func expansion(
    of node: AttributeSyntax,
    attachedTo: some DeclGroupSyntax,
    providingExtensionsOf type: some TypeSyntaxProtocol,
    conformingTo protocols: [TypeSyntax],
    in context: some MacroExpansionContext
  ) throws -> [ExtensionDeclSyntax] {
    let ext: DeclSyntax = "extension \(type.trimmed): Hashable {}"
    return [ext.cast(ExtensionDeclSyntax.self)]
  }
}

public struct ImpliesHashableMacro: ExtensionMacro {
  public static func expansion(
    of node: AttributeSyntax,
    attachedTo: some DeclGroupSyntax,
    providingExtensionsOf type: some TypeSyntaxProtocol,
    conformingTo protocols: [TypeSyntax],
    in context: some MacroExpansionContext
  ) throws -> [ExtensionDeclSyntax] {
    let ext: DeclSyntax = "extension \(type.trimmed): ImpliesHashable {}"
    return [ext.cast(ExtensionDeclSyntax.self)]
  }
}

public struct DelegatedConformanceMacro: ExtensionMacro, MemberMacro {
  public static func expansion(
    of node: AttributeSyntax,
    attachedTo: some DeclGroupSyntax,
    providingExtensionsOf type: some TypeSyntaxProtocol,
    conformingTo protocols: [TypeSyntax],
    in context: some MacroExpansionContext
  ) throws -> [ExtensionDeclSyntax] {
    let conformance: DeclSyntax =
      """
      extension \(type.trimmed): P where Element: P {}
      """

    guard let extensionDecl = conformance.as(ExtensionDeclSyntax.self) else {
      return []
    }

    return [extensionDecl]
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

public struct DelegatedConformanceViaExtensionMacro: ExtensionMacro {
  public static func expansion(
    of node: AttributeSyntax,
    attachedTo decl: some DeclGroupSyntax,
    providingExtensionsOf type: some TypeSyntaxProtocol,
    conformingTo protocols: [TypeSyntax],
    in context: some MacroExpansionContext
  ) throws -> [ExtensionDeclSyntax] {
    if (protocols.isEmpty) {
      return []
    }

    let decl: DeclSyntax =
      """
      extension \(raw: type.trimmedDescription): P where Element: P {
        static func requirement() {
          Element.requirement()
        }
      }

      """

    guard let extensionDecl = decl.as(ExtensionDeclSyntax.self) else {
      return []
    }

    return [
      extensionDecl
    ]
  }
}

public struct AlwaysAddConformance: ExtensionMacro {
  public static func expansion(
    of node: AttributeSyntax,
    attachedTo decl: some DeclGroupSyntax,
    providingExtensionsOf type: some TypeSyntaxProtocol,
    conformingTo protocols: [TypeSyntax],
    in context: some MacroExpansionContext
  ) throws -> [ExtensionDeclSyntax] {
    let decl: DeclSyntax =
      """
      extension \(raw: type.trimmedDescription): P where Element: P {
        static func requirement() {
          Element.requirement()
        }
      }

      """

    return [
      decl.cast(ExtensionDeclSyntax.self)
    ]
  }
}

public struct ConditionallyAvailableConformance: ExtensionMacro {
  public static func expansion(
    of node: AttributeSyntax,
    attachedTo decl: some DeclGroupSyntax,
    providingExtensionsOf type: some TypeSyntaxProtocol,
    conformingTo protocols: [TypeSyntax],
    in context: some MacroExpansionContext
  ) throws -> [ExtensionDeclSyntax] {
    let decl: DeclSyntax =
      """
      @available(macOS 99, *)
      extension \(raw: type.trimmedDescription): Equatable {}
      """

    return [
      decl.cast(ExtensionDeclSyntax.self)
    ]
  }
}

public struct AddAllConformancesMacro: ExtensionMacro {
  public static func expansion(
    of node: AttributeSyntax,
    attachedTo decl: some DeclGroupSyntax,
    providingExtensionsOf type: some TypeSyntaxProtocol,
    conformingTo protocols: [TypeSyntax],
    in context: some MacroExpansionContext
  ) throws -> [ExtensionDeclSyntax] {
    protocols.map { proto in
      let decl: DeclSyntax =
        """
        extension \(type): \(proto) {}
        """
      return decl.cast(ExtensionDeclSyntax.self)
    }
  }
}

public struct AlwaysAddCodable: ExtensionMacro {
  public static func expansion(
    of node: AttributeSyntax,
    attachedTo decl: some DeclGroupSyntax,
    providingExtensionsOf type: some TypeSyntaxProtocol,
    conformingTo protocols: [TypeSyntax],
    in context: some MacroExpansionContext
  ) throws -> [ExtensionDeclSyntax] {
    let decl: DeclSyntax =
      """
      extension \(raw: type.trimmedDescription): Codable {
      }

      """

    return [
      decl.cast(ExtensionDeclSyntax.self)
    ]
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

public struct DefineComparableTypeMacro: DeclarationMacro {
  public static func expansion(
    of node: some FreestandingMacroExpansionSyntax,
    in context: some MacroExpansionContext
  ) throws -> [DeclSyntax] {
    return ["""
    struct ComparableType: Comparable {
      static func <(lhs: ComparableType, rhs: ComparableType) -> Bool {
        return false
      }

      enum Inner: String, Comparable {
        case hello = "hello"
        static func <(lhs: ComparableType.Inner, rhs: ComparableType.Inner) -> Bool {
          return lhs.rawValue < rhs.rawValue
        }
      }
    }
    """]
  }
}

public struct AddMemberWithFixIt: MemberMacro {
  public static func expansion<
    Declaration: DeclGroupSyntax, Context: MacroExpansionContext
  >(
    of node: AttributeSyntax,
    providingMembersOf declaration: Declaration,
    in context: Context
  ) throws -> [DeclSyntax] {
    [
      """
      func foo() {
        var x = 0
        _ = x
      }
      """
    ]
  }
}

extension LabeledExprListSyntax {
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
    if let _ = node.arguments.first(labeled: "public") {
      accessSpecifier = "public "
    } else {
      accessSpecifier = ""
    }
    var results: [DeclSyntax] = [
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
      """,
      """

      struct \(context.makeUniqueName("name")): Equatable {
        static func == (lhs: Self, rhs: Self) -> Bool { false }
      }
      """
    ]

    if let _ = node.arguments.first(labeled: "causeErrors") {

      results += ["""

      struct \(context.makeUniqueName("name"))<T> where T == Equatable { // expect error: need 'any'
        #introduceTypeCheckingErrors // make sure we get nested errors
      }
      """]
    }

    return results
  }
}

public struct IntroduceTypeCheckingErrorsMacro: DeclarationMacro {
  public static func expansion(
    of node: some FreestandingMacroExpansionSyntax,
    in context: some MacroExpansionContext
  ) -> [DeclSyntax] {
    return [
      """

      struct \(context.makeUniqueName("name")) {
        struct \(context.makeUniqueName("name"))<T> where T == Hashable { // expect error: need 'any'
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

    let className = "\(protocolDecl.name.text)Builder"
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
      """)),
      .init(item: .stmt("""
      if 1 == 0 {
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

public struct GenericToVoidMacro: ExpressionMacro {
  public static func expansion(
    of node: some FreestandingMacroExpansionSyntax,
    in context: some MacroExpansionContext
  ) throws -> ExprSyntax {
    return """
           ()
           """
  }
}

public struct SingleMemberMacro: MemberMacro {
  public static func expansion(
    of node: AttributeSyntax,
    providingMembersOf decl: some DeclGroupSyntax,
    in context: some MacroExpansionContext
  ) throws -> [DeclSyntax] {
    let member: DeclSyntax =
      """
      var expandedMember: Int = 10
      """

    return [
      member,
    ]
  }
}

public struct UseIdentifierMacro: DeclarationMacro {
  public static func expansion(
    of node: some FreestandingMacroExpansionSyntax,
    in context: some MacroExpansionContext
  ) throws -> [DeclSyntax] {
    guard let argument = node.arguments.first?.expression.as(StringLiteralExprSyntax.self) else {
      fatalError("boom")
    }
    return [
      """
      func \(context.makeUniqueName("name"))() {
        _ = \(raw: argument.segments.first!)
      }
      """,
      """
      struct Foo {
        var \(context.makeUniqueName("name")): Int {
          _ = \(raw: argument.segments.first!)
          return 0
        }
      }
      """
    ]
  }
}

public struct StaticFooFuncMacro: DeclarationMacro {
  public static func expansion(
    of node: some FreestandingMacroExpansionSyntax,
    in context: some MacroExpansionContext
  ) throws -> [DeclSyntax] {
    return [
      "static func foo() {}",
    ]
  }
}

public struct SelfAlwaysEqualOperator: DeclarationMacro {
  public static func expansion(
    of node: some FreestandingMacroExpansionSyntax,
    in context: some MacroExpansionContext
  ) throws -> [DeclSyntax] {
    return [
      "static func ==(lhs: Self, rhs: Bool) -> Bool { true }",
    ]
  }
}

extension SelfAlwaysEqualOperator: MemberMacro {
  public static func expansion(
    of node: AttributeSyntax,
    providingMembersOf decl: some DeclGroupSyntax,
    in context: some MacroExpansionContext
  ) throws -> [DeclSyntax] {
    return [
      "static func ==(lhs: Self, rhs: Bool) -> Bool { true }",
    ]
  }
}

public struct AddPeerStoredPropertyMacro: PeerMacro, Sendable {
  public static func expansion(
    of attribute: AttributeSyntax,
    providingPeersOf declaration: some DeclSyntaxProtocol,
    in context: some MacroExpansionContext
  ) throws -> [DeclSyntax] {
    return [
      """

      public var _foo: Int = 100
      """
    ]
  }
}

public struct InitializableMacro: ExtensionMacro {
  public static func expansion(
    of node: AttributeSyntax,
    attachedTo: some DeclGroupSyntax,
    providingExtensionsOf type: some TypeSyntaxProtocol,
    conformingTo protocols: [TypeSyntax],
    in context: some MacroExpansionContext
  ) throws -> [ExtensionDeclSyntax] {
    let ext: DeclSyntax = 
      """
      extension \(type.trimmed): Initializable {
        init(value: Int) {}
      }
      """
    return [ext.cast(ExtensionDeclSyntax.self)]
  }
}

public struct PeerValueWithSuffixNameMacro: PeerMacro {
  public static func expansion(
    of node: AttributeSyntax,
    providingPeersOf declaration: some DeclSyntaxProtocol,
    in context: some MacroExpansionContext
  ) throws -> [DeclSyntax] {
    guard let identified = declaration.asProtocol(NamedDeclSyntax.self) else {
      throw CustomError.message("Macro can only be applied to an identified declarations.")
    }
    return ["var \(raw: identified.name.text)_peer: Int { 1 }"]
  }
}

public struct MagicFileMacro: ExpressionMacro {
  public static func expansion(
    of node: some FreestandingMacroExpansionSyntax,
    in context: some MacroExpansionContext
  ) -> ExprSyntax {
    return "#file"
  }
}

public struct MagicLineMacro: ExpressionMacro {
  public static func expansion(
    of node: some FreestandingMacroExpansionSyntax,
    in context: some MacroExpansionContext
  ) -> ExprSyntax {
    return "(#line)"
  }
}

public struct NestedMagicLiteralMacro: ExpressionMacro {
  public static func expansion(
    of node: some FreestandingMacroExpansionSyntax,
    in context: some MacroExpansionContext
  ) -> ExprSyntax {
    return
      """
      {
        print(#MagicFile)
        print(#MagicLine)
      }()
      """
  }
}

public struct NativeFileIDMacro: ExpressionMacro {
  public static func expansion(
    of node: some FreestandingMacroExpansionSyntax,
    in context: some MacroExpansionContext
  ) -> ExprSyntax {
    return context.location(
        of: node, at: .afterLeadingTrivia, filePathMode: .fileID
    )!.file
  }
}

public struct NativeFilePathMacro: ExpressionMacro {
  public static func expansion(
    of node: some FreestandingMacroExpansionSyntax,
    in context: some MacroExpansionContext
  ) -> ExprSyntax {
    return context.location(
        of: node, at: .afterLeadingTrivia, filePathMode: .filePath
    )!.file
  }
}

public struct NativeLineMacro: ExpressionMacro {
  public static func expansion(
    of node: some FreestandingMacroExpansionSyntax,
    in context: some MacroExpansionContext
  ) -> ExprSyntax {
    return context.location(of: node)!.line
  }
}

public struct NativeColumnMacro: ExpressionMacro {
  public static func expansion(
    of node: some FreestandingMacroExpansionSyntax,
    in context: some MacroExpansionContext
  ) -> ExprSyntax {
    return context.location(of: node)!.column
  }
}

public struct ClosureCallerMacro: ExpressionMacro {
    public static func expansion(
      of node: some FreestandingMacroExpansionSyntax,
      in context: some MacroExpansionContext
    ) -> ExprSyntax {
        let location = context.location(of: node)!
        return #"""
        ClosureCaller({ (value, then) in
            #sourceLocation(file: \#(location.file), line: \#(location.line))
            print("\(value)@\(\#(location.file))#\(\#(location.line))")
            then()
            #sourceLocation()
        })
        """#
    }
}

public struct PrependHelloToShadowedMacro: ExpressionMacro {
    public static func expansion(
      of node: some FreestandingMacroExpansionSyntax,
      in context: some MacroExpansionContext
    ) -> ExprSyntax {
        #"""
        "hello \(shadowed)"
        """#
    }
}

public struct InvalidIfExprMacro: MemberMacro {
  public static func expansion(
    of node: AttributeSyntax,
    providingMembersOf decl: some DeclGroupSyntax,
    in context: some MacroExpansionContext
  ) throws -> [DeclSyntax] {
    return ["""
      func bar() {
        let _ = (if .random() { 0 } else { 1 })
      }
      """]
  }
}

public struct InitWithProjectedValueWrapperMacro: PeerMacro {
  public static func expansion(
    of node: AttributeSyntax,
    providingPeersOf declaration: some DeclSyntaxProtocol,
    in context: some MacroExpansionContext
  ) throws -> [DeclSyntax] {
    return [
      """
      private var _value: Wrapper
      var _$value: Wrapper {
        @storageRestrictions(initializes: _value)
        init {
          self._value = newValue
        }
        get { _value }
      }
      """
    ]
  }
}

public struct RequiredDefaultInitMacro: ExtensionMacro {
  public static func expansion(
    of node: AttributeSyntax,
    attachedTo decl: some DeclGroupSyntax,
    providingExtensionsOf type: some TypeSyntaxProtocol,
    conformingTo protocols: [TypeSyntax],
    in context: some MacroExpansionContext
  ) throws -> [ExtensionDeclSyntax] {
    if protocols.isEmpty {
      return []
    }

    let decl: DeclSyntax =
      """
      extension \(type.trimmed): DefaultInit {
      }

      """

    return [
      decl.cast(ExtensionDeclSyntax.self)
    ]
  }
}

extension RequiredDefaultInitMacro: MemberMacro {
  public static func expansion(
    of node: AttributeSyntax,
    providingMembersOf declaration: some DeclGroupSyntax,
    in context: some MacroExpansionContext
  ) throws -> [DeclSyntax] {
    fatalError("old swift-syntax")
  }

  public static func expansion(
    of node: AttributeSyntax,
    providingMembersOf declaration: some DeclGroupSyntax,
    conformingTo protocols: [TypeSyntax],
    in context: some MacroExpansionContext
  ) throws -> [DeclSyntax] {
    let initDecl: DeclSyntax
    let funcDecl: DeclSyntax
    if !declaration.is(ClassDeclSyntax.self) {
      initDecl = "init() { }"
      funcDecl = "func f() { }"
    } else if !protocols.isEmpty {
      initDecl = "required init() { }"
      funcDecl = "func f() { }"
    } else {
      initDecl = "required init() { }"
      funcDecl = "override func f() { }"
    }
    return [ initDecl, funcDecl ]
  }
}

public struct SendableMacro: ExtensionMacro {
  public static func expansion(
    of node: AttributeSyntax,
    attachedTo decl: some DeclGroupSyntax,
    providingExtensionsOf type: some TypeSyntaxProtocol,
    conformingTo protocols: [TypeSyntax],
    in context: some MacroExpansionContext
  ) throws -> [ExtensionDeclSyntax] {
    if protocols.isEmpty {
      return []
    }

    let decl: DeclSyntax =
      """
      extension \(type.trimmed): Sendable {
      }

      """

    return [
      decl.cast(ExtensionDeclSyntax.self)
    ]
  }
}

public struct GenerateStubMemberMacro: MemberMacro {
  public static func expansion(
    of node: AttributeSyntax,
    providingMembersOf declaration: some DeclGroupSyntax,
    conformingTo protocols: [TypeSyntax],
    in context: some MacroExpansionContext
  ) throws -> [DeclSyntax] {
    return ["#generateMemberStubs"]
  }
}

public struct GenerateStubsFreestandingMacro: DeclarationMacro {
  public static func expansion(
    of node: some FreestandingMacroExpansionSyntax,
    in context: some MacroExpansionContext
  ) throws -> [DeclSyntax] {
    return ["#generateMember"]
  }
}

public struct SingleMemberStubMacro: DeclarationMacro {
  public static func expansion(
    of node: some FreestandingMacroExpansionSyntax,
    in context: some MacroExpansionContext
  ) throws -> [DeclSyntax] {
    return ["static func member() {}"]
  }
}

public struct DeclMacroWithControlFlow: DeclarationMacro {
  public static func expansion(
    of node: some FreestandingMacroExpansionSyntax,
    in context: some MacroExpansionContext
  ) throws -> [DeclSyntax] {
    return ["let _ = .random() ? try throwingFn() : 0"]
  }
}

public struct GenerateStubsForProtocolRequirementsMacro: PeerMacro, ExtensionMacro {
  public static func expansion(
    of node: AttributeSyntax,
    attachedTo declaration: some DeclGroupSyntax,
    providingExtensionsOf type: some TypeSyntaxProtocol,
    conformingTo protocols: [TypeSyntax],
    in context: some MacroExpansionContext
  ) throws -> [ExtensionDeclSyntax] {
    guard let proto = declaration.as(ProtocolDeclSyntax.self) else {
      return []
    }

    let requirements =
      proto.memberBlock.members.map { member in member.trimmed }
    let requirementStubs = requirements
      .map { req in
        "\(req) { fatalError() }"
      }
      .joined(separator: "\n    ")

    let extensionDecl: DeclSyntax =
      """
      extension \(proto.name) where Self: _TestStub {
        \(raw: requirementStubs)
      }
      """
    return [extensionDecl.cast(ExtensionDeclSyntax.self)]
  }

  public static func expansion(
    of node: AttributeSyntax,
    providingPeersOf declaration: some DeclSyntaxProtocol,
    in context: some MacroExpansionContext
  ) throws -> [DeclSyntax] {
    guard let proto = declaration.as(ProtocolDeclSyntax.self) else {
      return []
    }

    return [
      """
      struct __\(proto.name): \(proto.name), _TestStub {
        init() {} 
      }
      """
    ]
  }
}

  public struct FakeCodeItemMacro: DeclarationMacro, PeerMacro {
  public static func expansion(
    of node: some FreestandingMacroExpansionSyntax,
    in context: some MacroExpansionContext
  ) throws -> [DeclSyntax] {
    return ["guard true else { return }"]
  }

  public static func expansion(
    of node: AttributeSyntax,
    providingPeersOf declaration: some DeclSyntaxProtocol,
    in context: some MacroExpansionContext
  ) throws -> [DeclSyntax] {
    return ["if true { return }"]
  }
}

public struct NotMacroStruct {
  public static func expansion(
    of macro: some FreestandingMacroExpansionSyntax,
    in context: some MacroExpansionContext
  ) -> ExprSyntax {
    fatalError()
  }
}

extension FunctionParameterSyntax {
  var parameterName: TokenSyntax? {
    // If there were two names, the second is the parameter name.
    if let secondName {
      if secondName.text == "_" {
        return nil
      }

      return secondName
    }

    if firstName.text == "_" {
      return nil
    }

    return firstName
  }
}

@_spi(ExperimentalLanguageFeature)
public struct RemoteBodyMacro: BodyMacro {
  public static func expansion(
    of node: AttributeSyntax,
    providingBodyFor declaration: some DeclSyntaxProtocol & WithOptionalCodeBlockSyntax,
    in context: some MacroExpansionContext
  ) throws -> [CodeBlockItemSyntax] {
    // FIXME: Should be able to support (de-)initializers and accessors as
    // well, but this is a lazy implementation.
    guard let funcDecl = declaration.as(FunctionDeclSyntax.self) else {
      return []
    }

    let funcBaseName = funcDecl.name.text
    let paramNames = funcDecl.signature.parameterClause.parameters.map { param in
      param.parameterName ?? TokenSyntax(.wildcard, presence: .present)
    }

    let passedArgs = DictionaryExprSyntax(
      content: .elements(
        DictionaryElementListSyntax {
          for paramName in paramNames {
            DictionaryElementSyntax(
              key: ExprSyntax("\(literal: paramName.text)"),
              value: DeclReferenceExprSyntax(baseName: paramName)
            )
          }
        }
      )
    )

    return [
      """
      return try await remoteCall(function: \(literal: funcBaseName), arguments: \(passedArgs))
      """
    ]
  }
}

@_spi(ExperimentalLanguageFeature)
public struct BodyMacroWithControlFlow: BodyMacro {
  public static func expansion(
    of node: AttributeSyntax,
    providingBodyFor declaration: some DeclSyntaxProtocol & WithOptionalCodeBlockSyntax,
    in context: some MacroExpansionContext
  ) throws -> [CodeBlockItemSyntax] {
    [
      "guard .random() else { return }",
      "_ = try throwingFn()"
    ]
  }
}

@_spi(ExperimentalLanguageFeature)
public struct TracedPreambleMacro: PreambleMacro {
  public static func expansion(
    of node: AttributeSyntax,
    providingPreambleFor declaration: some DeclSyntaxProtocol & WithOptionalCodeBlockSyntax,
    in context: some MacroExpansionContext
  ) throws -> [CodeBlockItemSyntax] {
    // FIXME: Should be able to support (de-)initializers and accessors as
    // well, but this is a lazy implementation.
    guard let funcDecl = declaration.as(FunctionDeclSyntax.self) else {
      return []
    }

    let funcBaseName = funcDecl.name
    let paramNames = funcDecl.signature.parameterClause.parameters.map { param in
      param.parameterName?.text ?? "_"
    }

    let passedArgs = paramNames.map { "\($0): \\(\($0))" }.joined(separator: ", ")

    let entry: CodeBlockItemSyntax = """
      log("Entering \(funcBaseName)(\(raw: passedArgs))")
      """

    let argLabels = paramNames.map { "\($0):" }.joined()

    let exit: CodeBlockItemSyntax = """
      log("Exiting \(funcBaseName)(\(raw: argLabels))")
      """

    return [
      entry,
      """
      defer {
        \(exit)
      }
      """,
    ]
  }
}

@_spi(ExperimentalLanguageFeature)
public struct LoggerMacro: PreambleMacro {
  public static func expansion(
    of node: AttributeSyntax,
    providingPreambleFor declaration: some DeclSyntaxProtocol & WithOptionalCodeBlockSyntax,
    in context: some MacroExpansionContext
  ) throws -> [CodeBlockItemSyntax] {
    // FIXME: Should be able to support (de-)initializers and accessors as
    // well, but this is a lazy implementation.
    guard let funcDecl = declaration.as(FunctionDeclSyntax.self) else {
      return []
    }

    let funcBaseName = funcDecl.name
    let paramNames = funcDecl.signature.parameterClause.parameters.map { param in
      param.parameterName?.text ?? "_"
    }

    let passedArgs = paramNames.map { "\($0): \\(\($0))" }.joined(separator: ", ")

    let entry: CodeBlockItemSyntax = """
      logger.log(entering: "\(funcBaseName)(\(raw: passedArgs))")
      """

    let argLabels = paramNames.map { "\($0):" }.joined()

    let exit: CodeBlockItemSyntax = """
      logger.log(exiting: "\(funcBaseName)(\(raw: argLabels))")
      """

    return [
      """
      let logger = Logger()
      """,
      entry,
      """
      defer {
        \(exit)
      }
      """,
    ]
  }
}

public struct AddMemberPeersMacro: MemberAttributeMacro {
    public static func expansion(of node: AttributeSyntax, attachedTo declaration: some DeclGroupSyntax, providingAttributesFor member: some DeclSyntaxProtocol, in context: some MacroExpansionContext) throws -> [AttributeSyntax] {
        ["@_AddPeer"]
    }
}

public struct _AddPeerMacro: PeerMacro {
    public static func expansion<Decl: DeclSyntaxProtocol>(of node: AttributeSyntax, providingPeersOf declaration: Decl, in context: some MacroExpansionContext) throws -> [DeclSyntax] {
        guard let name = declaration.as(VariableDeclSyntax.self)?.bindings.first?.pattern.as(IdentifierPatternSyntax.self)?.identifier.text else {
            return []
        }
        return ["static let \(raw: name)_special = 41"]
    }
}

public struct WrapperMacro: PeerMacro {
  public static func expansion(
      of node: SwiftSyntax.AttributeSyntax,
      providingPeersOf declaration: some SwiftSyntax.DeclSyntaxProtocol,
      in context: some SwiftSyntaxMacros.MacroExpansionContext
  ) throws -> [SwiftSyntax.DeclSyntax] {
    []
  }
}

public struct AddDeinit: MemberMacro {
  public static func expansion(
    of node: AttributeSyntax,
    providingMembersOf decl: some DeclGroupSyntax,
    in context: some MacroExpansionContext
  ) throws -> [DeclSyntax] {
    return [
      """
      deinit {
        print("deinit was called")
      }
      """
    ]
  }
}

public struct AddSubscript: MemberMacro {
  public static func expansion(
    of node: AttributeSyntax,
    providingMembersOf decl: some DeclGroupSyntax,
    in context: some MacroExpansionContext
  ) throws -> [DeclSyntax] {
    return [
      """
      subscript(unchecked index: Int) -> String {
        return "\\(index)"
      }
      """
    ]
  }
}

public struct AllLexicalContextsMacro: DeclarationMacro {
  public static func expansion(
    of node: some FreestandingMacroExpansionSyntax,
    in context: some MacroExpansionContext
  ) throws -> [DeclSyntax] {
    context.lexicalContext.compactMap { $0.as(DeclSyntax.self)?.trimmed }
  }
}

public struct AddGetterMacro: AccessorMacro {
  public static func expansion(
    of node: AttributeSyntax,
    providingAccessorsOf declaration: some DeclSyntaxProtocol,
    in context: some MacroExpansionContext
  ) throws -> [AccessorDeclSyntax] {
    return ["get { 0 }"]
  }
}
