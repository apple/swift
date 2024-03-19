//===--- ParseBridging.h --------------------------------------------------===//
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

#ifndef SWIFT_PARSE_PARSEBRIDGING_H
#define SWIFT_PARSE_PARSEBRIDGING_H

#include "swift/AST/ASTBridging.h"
#include "swift/Basic/BasicBridging.h"

#ifdef USED_IN_CPP_SOURC
#include "swift/Parse/Parser.h"
#else
namespace swift {
class Parser;
}
#endif

SWIFT_BEGIN_NULLABILITY_ANNOTATIONS

class BridgedLegacyParser {
  swift::Parser *_Nonnull const handle;

public:
#ifdef USED_IN_CPP_SOURCE
  BridgedLegacyParser(swift::Parser &P) : handle(&P) {}

  swift::Parser &unbridged() const { return *handle; }
#endif
};

SWIFT_NAME("BridgedLegacyParser.parseExpr(self:_:_:_:)")
BridgedExpr BridgedLegacyParser_parseExpr(BridgedLegacyParser,
                                          BridgedSourceLoc loc,
                                          BridgedDeclContext DC,
                                          bool isExprBasic);

SWIFT_NAME("BridgedLegacyParser.parseDecl(self:_:_:)")
BridgedDecl BridgedLegacyParser_parseDecl(BridgedLegacyParser,
                                          BridgedSourceLoc loc,
                                          BridgedDeclContext DC);

SWIFT_NAME("BridgedLegacyParser.parseStmt(self:_:_:)")
BridgedStmt BridgedLegacyParser_parseStmt(BridgedLegacyParser,
                                          BridgedSourceLoc loc,
                                          BridgedDeclContext DC);

SWIFT_NAME("BridgedLegacyParser.parseType(self:loc:declContext:"
           "generateChildrenWithASTGen:)")
BridgedTypeRepr BridgedLegacyParser_parseType(BridgedLegacyParser,
                                              BridgedSourceLoc loc,
                                              BridgedDeclContext DC,
                                              bool generateChildrenWithASTGen);

/// Validate the `TypeRepr` translated from a `TypeSyntax` by ASTGen against the
/// `TypeRepr` parsed at it by the legacy parser, and emit errors on unexpected
/// mismatches.
SWIFT_NAME("validateGeneratedTypeRepr(_:legacyParserResult:astgenResult:)")
void validateGeneratedTypeRepr(BridgedASTContext cContext,
                               BridgedTypeRepr legacyParserResult,
                               BridgedTypeRepr astgenResult);

SWIFT_END_NULLABILITY_ANNOTATIONS

#endif // SWIFT_PARSE_PARSEBRIDGING_H
