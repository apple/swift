//===--- GenericParamList.cpp - Swift Language Decl ASTs ------------------===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2018 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//
//
//  This file implements the GenericParamList class and related classes.
//
//===----------------------------------------------------------------------===//

#include "swift/AST/GenericParamList.h"

#include "swift/AST/ASTContext.h"
#include "swift/AST/TypeRepr.h"
#include "swift/Basic/Assertions.h"

using namespace swift;
SourceRange RequirementRepr::getSourceRange() const {
  if (getKind() == RequirementReprKind::LayoutConstraint)
    return SourceRange(FirstType->getSourceRange().Start,
                       SecondLayout.getSourceRange().End);
  return SourceRange(FirstType->getSourceRange().Start,
                     SecondType->getSourceRange().End);
}

GenericParamList::GenericParamList(SourceLoc LAngleLoc,
                                   ArrayRef<GenericTypeParamDecl *> Params,
                                   SourceLoc WhereLoc,
                                   MutableArrayRef<RequirementRepr> Requirements,
                                   SourceLoc RAngleLoc)
  : Brackets(LAngleLoc, RAngleLoc), NumParams(Params.size()),
    WhereLoc(WhereLoc), Requirements(Requirements),
    OuterParameters(nullptr)
{
  std::uninitialized_copy(Params.begin(), Params.end(),
                          getTrailingObjects<GenericTypeParamDecl *>());
}

GenericParamList *
GenericParamList::create(ASTContext &Context,
                         SourceLoc LAngleLoc,
                         ArrayRef<GenericTypeParamDecl *> Params,
                         SourceLoc RAngleLoc) {
  unsigned Size = totalSizeToAlloc<GenericTypeParamDecl *>(Params.size());
  void *Mem = Context.Allocate(Size, alignof(GenericParamList));
  return new (Mem) GenericParamList(LAngleLoc, Params, SourceLoc(),
                                    MutableArrayRef<RequirementRepr>(),
                                    RAngleLoc);
}

GenericParamList *
GenericParamList::create(const ASTContext &Context,
                         SourceLoc LAngleLoc,
                         ArrayRef<GenericTypeParamDecl *> Params,
                         SourceLoc WhereLoc,
                         ArrayRef<RequirementRepr> Requirements,
                         SourceLoc RAngleLoc) {
  unsigned Size = totalSizeToAlloc<GenericTypeParamDecl *>(Params.size());
  void *Mem = Context.Allocate(Size, alignof(GenericParamList));
  return new (Mem) GenericParamList(LAngleLoc, Params,
                                    WhereLoc,
                                    Context.AllocateCopy(Requirements),
                                    RAngleLoc);
}

GenericParamList *
GenericParamList::clone(DeclContext *dc) const {
  auto &ctx = dc->getASTContext();
  SmallVector<GenericTypeParamDecl *, 2> params;
  for (auto param : getParams()) {
    auto *newParam = GenericTypeParamDecl::createImplicit(
        dc, param->getName(), GenericTypeParamDecl::InvalidDepth,
        param->getIndex(), param->isParameterPack(), param->isOpaqueType(),
        param->getOpaqueTypeRepr());
    params.push_back(newParam);
  }

  return GenericParamList::create(ctx, SourceLoc(), params, SourceLoc());
}

void GenericParamList::setDepth(unsigned depth) {
  for (auto param : *this)
    param->setDepth(depth);
}

void GenericParamList::setDeclContext(DeclContext *dc) {
  for (auto param : *this)
    param->setDeclContext(dc);
}

GenericTypeParamDecl *GenericParamList::lookUpGenericParam(
    Identifier name) const {
  for (const auto *innerParams = this;
       innerParams != nullptr;
       innerParams = innerParams->getOuterParameters()) {
    for (auto *paramDecl : *innerParams) {
      if (name == paramDecl->getName()) {
        return const_cast<GenericTypeParamDecl *>(paramDecl);
      }
    }
  }

  return nullptr;
}

TrailingWhereClause::TrailingWhereClause(
                       SourceLoc whereLoc, SourceLoc endLoc,
                       ArrayRef<RequirementRepr> requirements)
  : WhereLoc(whereLoc), EndLoc(endLoc),
    NumRequirements(requirements.size())
{
  std::uninitialized_copy(requirements.begin(), requirements.end(),
                          getTrailingObjects<RequirementRepr>());
}

TrailingWhereClause *TrailingWhereClause::create(
                       ASTContext &ctx,
                       SourceLoc whereLoc,
                       SourceLoc endLoc,
                       ArrayRef<RequirementRepr> requirements) {
  unsigned size = totalSizeToAlloc<RequirementRepr>(requirements.size());
  void *mem = ctx.Allocate(size, alignof(TrailingWhereClause));
  return new (mem) TrailingWhereClause(whereLoc, endLoc, requirements);
}
