//===--- ASTGenBridgingWrappers.h - Bridging helpers for ASTGen -----------===//
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

#ifndef SWIFT_AST_ASTGENBRIDGINGWRAPPERS_H
#define SWIFT_AST_ASTGENBRIDGINGWRAPPERS_H

#include "swift/Basic/LLVM.h"
#include "swift/Basic/Nullability.h"
#include "llvm/ADT/ArrayRef.h"

#include <swift/bridging>

SWIFT_BEGIN_NULLABILITY_ANNOTATIONS

namespace swift {
class ASTContext;
class Decl;
class DeclContext;
class DeclName;
class EnumCaseDecl;
class EnumElementDecl;
class FuncDecl;
class GenericParamList;
class GenericTypeParamDecl;
class Identifier;
class ParamDecl;
class ParameterList;
class RequirementRepr;
class SourceLoc;
enum class StaticSpellingKind : uint8_t;
class TrailingWhereClause;
class TypeRepr;

/// Bridgable wrapper for 'llvm::ArrayRef<Element>'.
template <typename Element>
class BridgableArrayRef {
  using ArrRefTy = llvm::ArrayRef<Element>;
  ArrRefTy ArrRef;

public:
  BridgableArrayRef(llvm::ArrayRef<Element> arrRef) : ArrRef(arrRef) {}

  BridgableArrayRef(const Element *_Nullable data, size_t length)
      : ArrRef(data, length) {}

  using iterator = typename ArrRefTy::iterator;
  iterator begin() const { return ArrRef.begin(); }
  iterator end() const { return ArrRef.end(); }

  bool empty() const { return ArrRef.empty(); }

  llvm::ArrayRef<Element> get() const { return ArrRef; }

  operator llvm::ArrayRef<Element>() { return ArrRef; }
}
#ifdef IMPORTING_INTO_ASTGEN
SWIFT_CONFORMS_TO_PROTOCOL(swiftASTGen.BridgableArrayRefProtocol)
#endif
    ;

/// Bridgable wrapper for 'ASTContext &'.
class BridgableASTContext final {
public:
  ASTContext *_Nonnull Ctx;

  BridgableASTContext(ASTContext &ctx) : Ctx(&ctx) {}
  BridgableASTContext(ASTContext *_Nonnull ctx) : Ctx(ctx) { assert(ctx); }

  Identifier getIdentifier(StringRef str) const;

  operator ASTContext &() { return *Ctx; }
};

// GenericTypeParamDecl has trailing objects and therefore can't be bridged
// using C++ interop currently.
class BridgableGenericTypeParamDecl final {
public:
  GenericTypeParamDecl *_Nonnull Ptr;

  BridgableGenericTypeParamDecl(GenericTypeParamDecl *_Nullable ptr)
      : Ptr(ptr) {}

  SWIFT_NAME(createParsed(declContext:name:nameLoc:eachLoc:index:inherited:))
  static BridgableGenericTypeParamDecl
  createParsed(DeclContext *_Nonnull dc, Identifier name, SourceLoc nameLoc,
               SourceLoc eachLoc, unsigned index,
               TypeRepr *_Nullable inherited);
  Decl *_Nonnull asDecl();

  operator GenericTypeParamDecl * _Nonnull() { return Ptr; }
};

// GenericParamList has trailing objects and therefore can't be bridged
// using C++ interop currently.
class BridgableGenericParamList final {
public:
  GenericParamList *_Nullable Ptr;

  BridgableGenericParamList(GenericParamList *_Nullable ptr) : Ptr(ptr) {}

  SWIFT_NAME(createParsed(_:lAngleLoc:params:whereLoc:requirements:rAngleLoc:))
  static BridgableGenericParamList
  createParsed(BridgableASTContext Context, SourceLoc LAngleLoc,
               BridgableArrayRef<BridgableGenericTypeParamDecl> Params,
               SourceLoc WhereLoc,
               BridgableArrayRef<RequirementRepr> Requirements,
               SourceLoc RAngleLoc);

  operator GenericParamList * _Nullable() { return Ptr; }
};

// TrailingWhereClause has trailing objects and therefore can't be bridged
// using C++ interop currently.
class BridgableTrailingWhereClause final {
public:
  TrailingWhereClause *_Nullable Ptr;

  BridgableTrailingWhereClause(TrailingWhereClause *_Nullable ptr) : Ptr(ptr) {}

  SWIFT_NAME(create(_:whereLoc:requirements:))
  static BridgableTrailingWhereClause
  create(BridgableASTContext ctx, SourceLoc whereLoc,
         BridgableArrayRef<RequirementRepr> requirements);

  operator TrailingWhereClause * _Nullable() { return Ptr; }
};

// ParameterList has trailing objects and therefore can't be bridged
// using C++ interop currently.
class BridgableParameterList final {
public:
  ParameterList *_Nullable Ptr;

  BridgableParameterList(ParameterList *_Nullable ptr) : Ptr(ptr) {}

  SWIFT_NAME(createParsed(_:lParenLoc:params:rParenLoc:))
  static BridgableParameterList
  createParsed(BridgableASTContext ctx, SourceLoc lParenLoc,
               BridgableArrayRef<ParamDecl *_Nonnull> params,
               SourceLoc rParenLoc);

  bool isNull() const { return !Ptr; }

  operator ParameterList * _Nullable() { return Ptr; }
};

// EnumCaseDecl has trailing objects and therefore can't be bridged
// using C++ interop currently.
class BridgableEnumCaseDecl final {
public:
  EnumCaseDecl *_Nonnull Ptr;

  BridgableEnumCaseDecl(EnumCaseDecl *_Nonnull ptr) : Ptr(ptr) {}

  SWIFT_NAME(createParsed(keywordLoc:elements:declContext:))
  static BridgableEnumCaseDecl
  createParsed(SourceLoc CaseLoc,
               BridgableArrayRef<EnumElementDecl *_Nonnull> Elements,
               DeclContext *_Nonnull DC);

  Decl *_Nonnull asDecl() const;

  operator EnumCaseDecl * _Nonnull() { return Ptr; }
};

// FuncDecl ends up in a fun request cycle if createParsed is defined on it
// with the bridging wrappes, so define it here until that bug is fixed
// (rdar://116426238).
class BridgableFuncDecl final {
public:
  FuncDecl *_Nonnull Ptr;

  BridgableFuncDecl(FuncDecl *_Nonnull ptr) : Ptr(ptr) {}

  SWIFT_NAME(createParsed(_:staticLoc:staticSpelling:funcKeywordLoc:name:nameLoc:genericParams:params:asyncLoc:throwsLoc:throwsType:resultType:whereClause:declContext:))
  static BridgableFuncDecl createParsed(
      BridgableASTContext Context, SourceLoc StaticLoc,
      StaticSpellingKind StaticSpelling, SourceLoc FuncLoc, DeclName Name,
      SourceLoc NameLoc, BridgableGenericParamList GenericParams,
      BridgableParameterList BodyParams, SourceLoc AsyncLoc,
      SourceLoc ThrowsLoc, TypeRepr *_Nullable ThrownTyR,
      TypeRepr *_Nullable ResultTyR, BridgableTrailingWhereClause WhereClause,
      DeclContext *_Nonnull Parent);

  Decl *_Nonnull asDecl() const;

  operator FuncDecl * _Nonnull() { return Ptr; }
};

} // namespace swift

SWIFT_END_NULLABILITY_ANNOTATIONS

#endif // SWIFT_AST_ASTGENBRIDGINGWRAPPERS_H
