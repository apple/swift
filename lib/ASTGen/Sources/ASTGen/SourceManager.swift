import CASTBridging
import SwiftOperators
import SwiftSyntax
import SwiftSyntaxMacros

/// A source manager that keeps track of the source files in the program.
class SourceManager {
  init(cxxDiagnosticEngine: UnsafeMutablePointer<UInt8>) {
    self.bridgedDiagEngine = BridgedDiagnosticEngine(raw: cxxDiagnosticEngine)
  }

  /// The bridged diagnostic engine (just the wrapped C++ `DiagnosticEngine`).
  let bridgedDiagEngine: BridgedDiagnosticEngine

  /// The set of source files that have been exported to the C++ code of
  /// the program.
  var exportedSourceFilesBySyntax: [
    SourceFileSyntax : UnsafePointer<ExportedSourceFile>
  ] = [:]

  /// The set of nodes that have been detached from their parent nodes.
  ///
  /// The keys are the detached nodes, while the values are (parent node,
  /// offset of detached node while it was attached).
  private var detachedNodes: [Syntax: (Syntax, Int)] = [:]
}

/// MARK: Source file management
extension SourceManager {
  /// Inserts a new source file into the source manager.
  ///
  /// - Returns: `true` if the source file was inserted, `false` if it was
  ///   already there.
  @discardableResult
  func insert(_ sourceFile: UnsafePointer<ExportedSourceFile>) -> Bool {
    let syntax = sourceFile.pointee.syntax
    if exportedSourceFilesBySyntax[syntax] != nil {
      return false
    }

    exportedSourceFilesBySyntax[syntax] = sourceFile
    return true
  }
}

/// MARK: Syntax source location mapping
extension SourceManager {
  /// Detach a given node from its parent, keeping track of where it
  /// occurred in the program.
  func detach<Node: SyntaxProtocol>(
    _ node: Node,
    foldingWith operatorTable: OperatorTable? = nil
  ) -> Node {
    // Already detached
    if node.parent == nil { return node }

    let detached: Node
    if let operatorTable = operatorTable {
      detached = operatorTable.foldAll(node) { _ in }.as(Node.self)!.detached
    } else {
      detached = node.detached
    }

    detachedNodes[Syntax(detached)] = (node.root, node.position.utf8Offset)
    return detached
  }

  /// Find the root source file and offset from within that file for the given
  /// syntax node.
  func rootSourceFile<Node: SyntaxProtocol>(
    of node: Node
  ) -> (SourceFileSyntax, AbsolutePosition)? {
    let root = node.root

    // If the root is a source file, we're done.
    if let rootSF = root.as(SourceFileSyntax.self) {
      return (rootSF, node.position)
    }

    // If the root isn't a detached node we know about, there's nothing we
    // can do.
    guard let (parent, offset) = detachedNodes[root] else {
      return nil
    }

    // Recursively find the root and its offset.
    guard let (rootSF, parentOffset) = rootSourceFile(of: parent) else {
      return nil
    }

    // The position of our node is...
    let finalPosition =
      node.position                      // Our position relative to its root
      + SourceLength(utf8Length: offset) // and that root's offset in its parent
      + SourceLength(utf8Length: parentOffset.utf8Offset)
    return (rootSF, finalPosition)
  }

  /// Produce the C++ source location for a given position based on a
  /// syntax node.
  func bridgedSourceLoc<Node: SyntaxProtocol>(
    for node: Node,
    at position: AbsolutePosition? = nil
  ) -> BridgedSourceLoc {
    // Find the source file and this node's position within it.
    guard let (sourceFile, rootPosition) = rootSourceFile(of: node) else {
      return nil
    }

    // Find the corresponding exported source file.
    guard let exportedSourceFile = exportedSourceFilesBySyntax[sourceFile]
    else {
      return nil
    }

    // Find the offset of the given position based on the root of the given
    // node.
    let position = position ?? node.position
    let nodeOffset = SourceLength(utf8Length: position.utf8Offset - node.position.utf8Offset)
    let realPosition = rootPosition + nodeOffset

    return BridgedSourceLoc(at: realPosition, in: exportedSourceFile.pointee.buffer)
  }
}
