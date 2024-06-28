// RUN: %sourcekitd-test -req=cursor -pos=12:11 %s -- %s

private class MacroApplication<Context: MacroExpansionContext>: SyntaxRewriter {

  override func visitAny(_ node: Syntax) -> Syntax? {
    if var declSyntax = node.as(DeclSyntax.self),
      let attributedNode = node.asProtocol(WithAttributesSyntax.self),
      !attributedNode.attributes.isEmpty
    {
      for (attribute, spec) in attributesToRemove {
        if let index = self.expandedAttributes.firstIndex(where: { expandedAttribute in
          expandedAttribute.position == attribute.position
        }) {
        } else {
        }
      }
    }
  }
}
