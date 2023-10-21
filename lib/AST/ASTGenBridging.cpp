//===--- ASTGenBridging.cpp - ASTGen bridging functions -------------------===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2022 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//

#include "swift/AST/ASTGenBridging.h"
#include "swift/AST/ASTContext.h"
#include "swift/AST/ParameterList.h"

using namespace swift;

Identifier BridgableASTContext::getIdentifier(StringRef str) const {
  return Ctx->getIdentifier(str);
}

BridgableGenericTypeParamDecl BridgableGenericTypeParamDecl::createParsed(
    DeclContext *_Nonnull dc, Identifier name, SourceLoc nameLoc,
    SourceLoc eachLoc, unsigned index, TypeRepr *inherited) {
  return GenericTypeParamDecl::createParsed(dc, name, nameLoc, eachLoc, index,
                                            inherited);
}

Decl *BridgableGenericTypeParamDecl::asDecl() { return Ptr; }

BridgableGenericParamList BridgableGenericParamList::createParsed(
    BridgableASTContext Context, SourceLoc LAngleLoc,
    BridgableArrayRef<BridgableGenericTypeParamDecl> Params, SourceLoc WhereLoc,
    BridgableArrayRef<RequirementRepr> Requirements, SourceLoc RAngleLoc) {
  SmallVector<GenericTypeParamDecl *, 4> genericParams;
  for (auto gp : Params)
    genericParams.emplace_back(gp);

  return GenericParamList::create(Context, LAngleLoc, genericParams, WhereLoc,
                                  Requirements, RAngleLoc);
}

BridgableTrailingWhereClause BridgableTrailingWhereClause::create(
    BridgableASTContext ctx, SourceLoc whereLoc,
    BridgableArrayRef<RequirementRepr> requirements) {
  return TrailingWhereClause::create(ctx, whereLoc, requirements);
}

BridgableParameterList BridgableParameterList::createParsed(
    BridgableASTContext Context, SourceLoc LParenLoc,
    BridgableArrayRef<ParamDecl *> params, SourceLoc RParenLoc) {
  return ParameterList::create(Context, LParenLoc, params, RParenLoc);
}

BridgableEnumCaseDecl BridgableEnumCaseDecl::createParsed(
    SourceLoc CaseLoc, BridgableArrayRef<EnumElementDecl *> Elements,
    DeclContext *DC) {
  return EnumCaseDecl::createParsed(CaseLoc, Elements, DC);
}

Decl *BridgableEnumCaseDecl::asDecl() const { return Ptr; }

BridgableFuncDecl BridgableFuncDecl::createParsed(
    BridgableASTContext Context, SourceLoc StaticLoc,
    StaticSpellingKind StaticSpelling, SourceLoc FuncLoc, DeclName Name,
    SourceLoc NameLoc, BridgableGenericParamList GenericParams,
    BridgableParameterList BodyParams, SourceLoc AsyncLoc, SourceLoc ThrowsLoc,
    TypeRepr *_Nullable ThrownTyR, TypeRepr *_Nullable ResultTyR,
    BridgableTrailingWhereClause WhereClause, DeclContext *_Nonnull Parent) {
  return FuncDecl::createParsed(Context, StaticLoc, StaticSpelling, FuncLoc,
                                Name, NameLoc, GenericParams, BodyParams,
                                AsyncLoc, ThrowsLoc, ThrownTyR, ResultTyR,
                                WhereClause, Parent);
}

Decl *BridgableFuncDecl::asDecl() const { return Ptr; }
