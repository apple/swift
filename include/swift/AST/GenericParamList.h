//===--- GenericParamList.h - Generic parameter list AST --------*- C++ -*-===//
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
// This file defines the GenericParamList class, and related classes.
//
//===----------------------------------------------------------------------===//

#ifndef SWIFT_GENERIC_PARAM_LIST_H
#define SWIFT_GENERIC_PARAM_LIST_H

#include "swift/AST/Decl.h"
#include "swift/AST/LayoutConstraint.h"
#include "swift/Basic/SourceLoc.h"
#include "llvm/ADT/StringRef.h"

#include <swift/bridging>

namespace swift {

class ASTPrinter;
class TypeRepr;

enum class RequirementReprKind : unsigned {
  /// A type bound T : P, where T is a type that depends on a generic
  /// parameter and P is some type that should bound T, either as a concrete
  /// supertype or a protocol to which T must conform.
  TypeConstraint,

  /// A same-type requirement T == U, where T and U are types that shall be
  /// equivalent.
  SameType,

  /// A layout bound T : L, where T is a type that depends on a generic
  /// parameter and L is some layout specification that should bound T.
  LayoutConstraint,

  // Note: there is code that packs this enum in a 2-bit bitfield.  Audit users
  // when adding enumerators.
};

/// A single requirement in a 'where' clause, which places additional
/// restrictions on the generic parameters or associated types of a generic
/// function, type, or protocol.
///
/// This always represents a requirement spelled in the source code.  It is
/// never generated implicitly.
///
/// \c GenericParamList assumes these are POD-like.

class RequirementRepr {
  SourceLoc SeparatorLoc;
  RequirementReprKind Kind : 2;
  bool Invalid : 1;
  bool IsExpansionPattern : 1;
  TypeRepr *FirstType;

  /// The second element represents the right-hand side of the constraint.
  /// It can be e.g. a type or a layout constraint.
  union {
    TypeRepr *SecondType;
    LayoutConstraintLoc SecondLayout;
  };

  /// Set during deserialization; used to print out the requirements accurately
  /// for the generated interface.
  StringRef AsWrittenString;

  RequirementRepr(SourceLoc SeparatorLoc, RequirementReprKind Kind,
                  TypeRepr *FirstType, TypeRepr *SecondType,
                  bool IsExpansionPattern)
    : SeparatorLoc(SeparatorLoc), Kind(Kind), Invalid(false),
      IsExpansionPattern(IsExpansionPattern),
      FirstType(FirstType), SecondType(SecondType) { }

  RequirementRepr(SourceLoc SeparatorLoc, RequirementReprKind Kind,
                  TypeRepr *FirstType, LayoutConstraintLoc SecondLayout,
                  bool IsExpansionPattern)
    : SeparatorLoc(SeparatorLoc), Kind(Kind), Invalid(false),
      IsExpansionPattern(IsExpansionPattern),
      FirstType(FirstType), SecondLayout(SecondLayout) { }

public:
  /// Construct a new type-constraint requirement.
  ///
  /// \param Subject The type that must conform to the given protocol or
  /// composition, or be a subclass of the given class type.
  /// \param ColonLoc The location of the ':', or an invalid location if
  /// this requirement was implied.
  /// \param Constraint The protocol or protocol composition to which the
  /// subject must conform, or superclass from which the subject must inherit.
  SWIFT_NAME(getTypeConstraint(subject:colonLoc:constraint:isExpansionPattern:))
  static RequirementRepr getTypeConstraint(TypeRepr *Subject,
                                           SourceLoc ColonLoc,
                                           TypeRepr *Constraint,
                                           bool IsExpansionPattern) {
    return { ColonLoc, RequirementReprKind::TypeConstraint, Subject, Constraint,
             IsExpansionPattern };
  }

  /// Construct a new same-type requirement.
  ///
  /// \param FirstType The first type.
  /// \param EqualLoc The location of the '==' in the same-type constraint, or
  /// an invalid location if this requirement was implied.
  /// \param SecondType The second type.
  SWIFT_NAME(getSameType(first:equalLoc:second:isExpansionPattern:))
  static RequirementRepr getSameType(TypeRepr *FirstType,
                                     SourceLoc EqualLoc,
                                     TypeRepr *SecondType,
                                     bool IsExpansionPattern) {
    return { EqualLoc, RequirementReprKind::SameType, FirstType, SecondType,
             IsExpansionPattern };
  }

  /// Construct a new layout-constraint requirement.
  ///
  /// \param Subject The type that must conform to the given layout 
  /// requirement.
  /// \param ColonLoc The location of the ':', or an invalid location if
  /// this requirement was implied.
  /// \param Layout The layout requirement to which the
  /// subject must conform.
  static RequirementRepr getLayoutConstraint(TypeRepr *Subject,
                                             SourceLoc ColonLoc,
                                             LayoutConstraintLoc Layout,
                                             bool IsExpansionPattern) {
    return {ColonLoc, RequirementReprKind::LayoutConstraint, Subject,
            Layout, IsExpansionPattern};
  }

  /// Determine the kind of requirement
  RequirementReprKind getKind() const { return Kind; }

  /// Determine whether this requirement is invalid.
  bool isInvalid() const { return Invalid; }

  /// Whether this requirement repr is the pattern of a requirement
  /// expansion, e.g. `repeat G<each T> == (each U).A`
  bool isExpansionPattern() const {
    return IsExpansionPattern;
  }

  /// Mark this requirement invalid.
  void setInvalid() { Invalid = true; }

  /// For a type-bound requirement, return the subject of the
  /// conformance relationship.
  TypeRepr *getSubjectRepr() const {
    assert(getKind() == RequirementReprKind::TypeConstraint ||
           getKind() == RequirementReprKind::LayoutConstraint);
    return FirstType;
  }

  /// For a type-bound requirement, return the protocol or to which
  /// the subject conforms or superclass it inherits.
  TypeRepr *getConstraintRepr() const {
    assert(getKind() == RequirementReprKind::TypeConstraint);
    return SecondType;
  }

  LayoutConstraint getLayoutConstraint() const {
    assert(getKind() == RequirementReprKind::LayoutConstraint);
    return SecondLayout.getLayoutConstraint();
  }

  LayoutConstraintLoc &getLayoutConstraintLoc() {
    assert(getKind() == RequirementReprKind::LayoutConstraint);
    return SecondLayout;
  }

  const LayoutConstraintLoc &getLayoutConstraintLoc() const {
    assert(getKind() == RequirementReprKind::LayoutConstraint);
    return SecondLayout;
  }

  /// Retrieve the first type of a same-type requirement.
  TypeRepr *getFirstTypeRepr() const {
    assert(getKind() == RequirementReprKind::SameType);
    return FirstType;
  }

  /// Retrieve the second type of a same-type requirement.
  TypeRepr *getSecondTypeRepr() const {
    assert(getKind() == RequirementReprKind::SameType);
    return SecondType;
  }

  /// Retrieve the location of the ':' or '==' in an explicitly-written
  /// conformance or same-type requirement respectively.
  SourceLoc getSeparatorLoc() const {
    return SeparatorLoc;
  }

  SourceRange getSourceRange() const;

  /// Retrieve the first or subject type representation from the \c repr,
  /// or \c nullptr if \c repr is null.
  static TypeRepr *getFirstTypeRepr(const RequirementRepr *repr) {
    if (!repr) return nullptr;
    return repr->FirstType;
  }

  /// Retrieve the second or constraint type representation from the \c repr,
  /// or \c nullptr if \c repr is null.
  static TypeRepr *getSecondTypeRepr(const RequirementRepr *repr) {
    if (!repr) return nullptr;
    assert(repr->getKind() == RequirementReprKind::TypeConstraint ||
           repr->getKind() == RequirementReprKind::SameType);
    return repr->SecondType;
  }

  SWIFT_DEBUG_DUMP;
  void print(raw_ostream &OS) const;
  void print(ASTPrinter &Printer) const;
};

/// GenericParamList - A list of generic parameters that is part of a generic
/// function or type, along with extra requirements placed on those generic
/// parameters and types derived from them.
class GenericParamList final :
    private llvm::TrailingObjects<GenericParamList, GenericTypeParamDecl *> {
  friend TrailingObjects;

  SourceRange Brackets;
  unsigned NumParams;
  SourceLoc WhereLoc;
  MutableArrayRef<RequirementRepr> Requirements;

  GenericParamList *OuterParameters;

  GenericParamList(SourceLoc LAngleLoc,
                   ArrayRef<GenericTypeParamDecl *> Params,
                   SourceLoc WhereLoc,
                   MutableArrayRef<RequirementRepr> Requirements,
                   SourceLoc RAngleLoc);
  
  // Don't copy.
  GenericParamList(const GenericParamList &) = delete;
  GenericParamList &operator=(const GenericParamList &) = delete;
  
public:
  /// create - Create a new generic parameter list within the given AST context.
  ///
  /// \param Context The ASTContext in which the generic parameter list will
  /// be allocated.
  /// \param LAngleLoc The location of the opening angle bracket ('<')
  /// \param Params The list of generic parameters, which will be copied into
  /// ASTContext-allocated memory.
  /// \param RAngleLoc The location of the closing angle bracket ('>')
  static GenericParamList *create(ASTContext &Context,
                                  SourceLoc LAngleLoc,
                                  ArrayRef<GenericTypeParamDecl *> Params,
                                  SourceLoc RAngleLoc);

  /// create - Create a new generic parameter list and "where" clause within
  /// the given AST context.
  ///
  /// \param Context The ASTContext in which the generic parameter list will
  /// be allocated.
  /// \param LAngleLoc The location of the opening angle bracket ('<')
  /// \param Params The list of generic parameters, which will be copied into
  /// ASTContext-allocated memory.
  /// \param WhereLoc The location of the 'where' keyword, if any.
  /// \param Requirements The list of requirements, which will be copied into
  /// ASTContext-allocated memory.
  /// \param RAngleLoc The location of the closing angle bracket ('>')
  static GenericParamList *create(const ASTContext &Context,
                                  SourceLoc LAngleLoc,
                                  ArrayRef<GenericTypeParamDecl *> Params,
                                  SourceLoc WhereLoc,
                                  ArrayRef<RequirementRepr> Requirements,
                                  SourceLoc RAngleLoc);

  MutableArrayRef<GenericTypeParamDecl *> getParams() {
    return {getTrailingObjects<GenericTypeParamDecl *>(), NumParams};
  }

  ArrayRef<GenericTypeParamDecl *> getParams() const {
    return {getTrailingObjects<GenericTypeParamDecl *>(), NumParams};
  }

  using iterator = GenericTypeParamDecl **;
  using const_iterator = const GenericTypeParamDecl * const *;

  unsigned size() const { return NumParams; }
  iterator begin() { return getParams().begin(); }
  iterator end() { return getParams().end(); }
  const_iterator begin() const { return getParams().begin(); }
  const_iterator end() const { return getParams().end(); }

  /// Retrieve the location of the 'where' keyword, or an invalid
  /// location if 'where' was not present.
  SourceLoc getWhereLoc() const { return WhereLoc; }

  /// Retrieve the set of additional requirements placed on these
  /// generic parameters and types derived from them.
  ///
  /// This list may contain both explicitly-written requirements as well as
  /// implicitly-generated requirements, and may be non-empty even if no
  /// 'where' keyword is present.
  MutableArrayRef<RequirementRepr> getRequirements() { return Requirements; }

  /// Retrieve the set of additional requirements placed on these
  /// generic parameters and types derived from them.
  ///
  /// This list may contain both explicitly-written requirements as well as
  /// implicitly-generated requirements, and may be non-empty even if no
  /// 'where' keyword is present.
  ArrayRef<RequirementRepr> getRequirements() const { return Requirements; }
  
  /// Retrieve the outer generic parameter list.
  ///
  /// This is used for extensions of nested types, and in SIL mode, where a
  /// single lexical context can have multiple logical generic parameter
  /// lists.
  GenericParamList *getOuterParameters() const { return OuterParameters; }

  /// Set the outer generic parameter list. See \c getOuterParameters
  /// for more information.
  void setOuterParameters(GenericParamList *Outer) { OuterParameters = Outer; }

  void setDeclContext(DeclContext *dc);

  SourceLoc getLAngleLoc() const { return Brackets.Start; }
  SourceLoc getRAngleLoc() const { return Brackets.End; }

  SourceRange getSourceRange() const { return Brackets; }

  /// Retrieve the source range covering the where clause.
  SourceRange getWhereClauseSourceRange() const {
    if (WhereLoc.isInvalid())
      return SourceRange();

    if (Requirements.empty())
      return WhereLoc;

    auto endLoc = Requirements.back().getSourceRange().End;
    return SourceRange(WhereLoc, endLoc);
  }

  /// Configure the depth of the generic parameters in this list.
  void setDepth(unsigned depth);

  /// Create a copy of the generic parameter list and all of its generic
  /// parameter declarations. The copied generic parameters are re-parented
  /// to the given DeclContext.
  GenericParamList *clone(DeclContext *dc) const;

  bool walk(ASTWalker &walker);

  /// Finds a generic parameter declaration by name. This should only
  /// be used from the SIL parser.
  GenericTypeParamDecl *lookUpGenericParam(Identifier name) const;

  SWIFT_DEBUG_DUMP;
  void print(raw_ostream &OS, const PrintOptions &PO = PrintOptions()) const;
  void print(ASTPrinter &Printer, const PrintOptions &PO) const;
};
  
/// A trailing where clause.
class alignas(RequirementRepr) TrailingWhereClause final :
    private llvm::TrailingObjects<TrailingWhereClause, RequirementRepr> {
  friend TrailingObjects;

  SourceLoc WhereLoc;
  SourceLoc EndLoc;

  /// The number of requirements. The actual requirements are tail-allocated.
  unsigned NumRequirements;

  TrailingWhereClause(SourceLoc whereLoc, SourceLoc endLoc,
                      ArrayRef<RequirementRepr> requirements);

public:
  /// Create a new trailing where clause with the given set of requirements.
  static TrailingWhereClause *create(ASTContext &ctx,
                                     SourceLoc whereLoc, SourceLoc endLoc,
                                     ArrayRef<RequirementRepr> requirements);

  /// Create a new trailing where clause with the given set of requirements.
  static TrailingWhereClause *create(ASTContext &ctx, SourceLoc whereLoc,
                                     ArrayRef<RequirementRepr> requirements);

  /// Retrieve the location of the 'where' keyword.
  SourceLoc getWhereLoc() const { return WhereLoc; }

  /// Retrieve the set of requirements.
  MutableArrayRef<RequirementRepr> getRequirements() {
    return {getTrailingObjects<RequirementRepr>(), NumRequirements};
  }

  /// Retrieve the set of requirements.
  ArrayRef<RequirementRepr> getRequirements() const {
    return {getTrailingObjects<RequirementRepr>(), NumRequirements};
  }

  /// Compute the source range containing this trailing where clause.
  SourceRange getSourceRange() const {
    return SourceRange(WhereLoc, EndLoc);
  }

  void print(llvm::raw_ostream &OS, bool printWhereKeyword) const;

};

}  // namespace swift

#endif // SWIFT_GENERIC_PARAM_LIST_H
