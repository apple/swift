//===--- ASTScopeImpl.h - Swift AST Object-Oriented Scope --------*- C++-*-===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2017 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//
///
/// This file defines the ASTScopeImpl class ontology, which
/// describes the scopes that exist within a Swift AST.
///
/// Each scope has four basic functions: printing for debugging, creation of
/// itself and its children, obtaining its SourceRange (for lookup), and looking
/// up names accessible from that scope.
///
/// Invariants:
///   a child's source range is a subset (proper or improper) of its parent's,
///   children are ordered by source range, and do not overlap,
///   all the names visible within a parent are visible within the child, unless
///   the nesting is illegal. For instance, a protocol nested inside of a class
///   does not get to see the symbols in the class or its ancestors.
///
//===----------------------------------------------------------------------===//
#ifndef SWIFT_AST_AST_SCOPE_H
#define SWIFT_AST_AST_SCOPE_H

#include "swift/AST/ASTNode.h"
#include "swift/AST/NameLookup.h"
#include "swift/AST/SimpleRequest.h"
#include "swift/Basic/Compiler.h"
#include "swift/Basic/Debug.h"
#include "swift/Basic/LLVM.h"
#include "swift/Basic/NullablePtr.h"
#include "swift/Basic/SourceManager.h"
#include "llvm/ADT/PointerIntPair.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SmallVector.h"

/// In case there's a bug in the ASTScope lookup system, suggest that the user
/// try disabling it.
/// \p message must be a string literal
#define ASTScopeAssert(predicate, message)                                     \
  assert((predicate) && message)

#define ASTScope_unreachable(message)                                          \
  llvm_unreachable(message)

namespace swift {

#pragma mark Forward-references

#define DECL(Id, Parent) class Id##Decl;
#define ABSTRACT_DECL(Id, Parent) class Id##Decl;
#include "swift/AST/DeclNodes.def"
#undef DECL
#undef ABSTRACT_DECL

#define EXPR(Id, Parent) class Id##Expr;
#include "swift/AST/ExprNodes.def"
#undef EXPR

#define STMT(Id, Parent) class Id##Stmt;
#define ABSTRACT_STMT(Id, Parent) class Id##Stmt;
#include "swift/AST/StmtNodes.def"
#undef STMT
#undef ABSTRACT_STMT

class GenericParamList;
class TrailingWhereClause;
class ParameterList;
class PatternBindingEntry;
class SpecializeAttr;
class GenericContext;
class DeclName;
class StmtConditionElement;

namespace Lowering {
class SILGenFunction;
}

namespace ast_scope {
class ASTScopeImpl;
class GenericTypeOrExtensionScope;
class IterableTypeScope;
class TypeAliasScope;
class ScopeCreator;

struct AnnotatedInsertionPoint {
  ASTScopeImpl *insertionPoint;
  const char *explanation;
};
} // namespace ast_scope

namespace ast_scope {

void simple_display(llvm::raw_ostream &out, const ASTScopeImpl *);
void simple_display(llvm::raw_ostream &out, const ScopeCreator *);

SourceLoc extractNearestSourceLoc(std::tuple<ASTScopeImpl *, ScopeCreator *>);

#pragma mark the root ASTScopeImpl class

/// Describes a lexical scope within a source file.
///
/// Each \c ASTScopeImpl is a node within a tree that describes all of the
/// lexical scopes within a particular source range. The root of this scope tree
/// is always a \c SourceFile node, and the tree covers the entire source file.
/// The children of a particular node are the lexical scopes immediately
/// nested within that node, and have source ranges that are enclosed within
/// the source range of their parent node. At the leaves are lexical scopes
/// that cannot be subdivided further.
///
/// The tree provides source-location-based query operations, allowing one to
/// find the innermost scope that contains a given source location. Navigation
/// to parent nodes from that scope allows one to walk the lexically enclosing
/// scopes outward to the source file. Given a scope, one can also query the
/// associated \c DeclContext for additional contextual information.
///
/// \code
/// -dump-scope-maps expanded
/// \endcode
class ASTScopeImpl : public ASTAllocated<ASTScopeImpl> {
  friend class NodeAdder;
  friend class Portion;
  friend class GenericTypeOrExtensionWholePortion;
  friend class NomExtDeclPortion;
  friend class GenericTypeOrExtensionWherePortion;
  friend class GenericTypeOrExtensionWherePortion;
  friend class IterableTypeBodyPortion;
  friend class ScopeCreator;
  friend class ASTSourceFileScope;
  friend class Lowering::SILGenFunction;

#pragma mark - tree state
protected:
  using Children = SmallVector<ASTScopeImpl *, 4>;
  /// Whether the given parent is the accessor node for an abstract
  /// storage declaration or is directly descended from it.

private:
  /// The pointer:
  /// - Always set by the constructor, so that when creating a child
  ///   the parent chain is available. Null at the root.
  /// The int:
  /// - A flag indicating if the scope has been expanded yet or not.
  llvm::PointerIntPair<ASTScopeImpl *, 1> parentAndWasExpanded;

  /// Child scopes, sorted by source range.
  Children storedChildren;

  mutable Optional<CharSourceRange> cachedCharSourceRange;

#pragma mark - constructor / destructor
public:
  ASTScopeImpl(){};
  // TOD: clean up all destructors and deleters
  virtual ~ASTScopeImpl() {}

  ASTScopeImpl(ASTScopeImpl &&) = delete;
  ASTScopeImpl &operator=(ASTScopeImpl &&) = delete;
  ASTScopeImpl(const ASTScopeImpl &) = delete;
  ASTScopeImpl &operator=(const ASTScopeImpl &) = delete;

  // Need this because have virtual destructors
  void operator delete(void *data) {}

#pragma mark - tree declarations
protected:
  NullablePtr<ASTScopeImpl> getParent() {
    return parentAndWasExpanded.getPointer();
  }
  NullablePtr<const ASTScopeImpl> getParent() const {
    return parentAndWasExpanded.getPointer();
  }

  const Children &getChildren() const { return storedChildren; }

public:
  void addChild(ASTScopeImpl *child, ASTContext &);

public:
  void preOrderDo(function_ref<void(ASTScopeImpl *)>);
  /// Like preorderDo but without myself.
  void preOrderChildrenDo(function_ref<void(ASTScopeImpl *)>);
  void postOrderDo(function_ref<void(ASTScopeImpl *)>);

#pragma mark - source ranges

public:
  CharSourceRange getCharSourceRangeOfScope(SourceManager &SM,
                                            bool omitAssertions = false) const;
  bool isCharSourceRangeCached() const;

  /// Returns source range of this node alone, without factoring in any
  /// children.
  virtual SourceRange
  getSourceRangeOfThisASTNode(bool omitAssertions = false) const = 0;

protected:
  SourceManager &getSourceManager() const;

private:
  void checkSourceRangeBeforeAddingChild(ASTScopeImpl *child,
                                         const ASTContext &ctx) const;

#pragma mark common queries
public:
  virtual NullablePtr<AbstractClosureExpr> getClosureIfClosureScope() const;
  virtual ASTContext &getASTContext() const;
  virtual NullablePtr<Decl> getDeclIfAny() const { return nullptr; };
  virtual NullablePtr<Stmt> getStmtIfAny() const { return nullptr; };
  virtual NullablePtr<Expr> getExprIfAny() const { return nullptr; };
  virtual NullablePtr<DeclAttribute> getDeclAttributeIfAny() const {
    return nullptr;
  }

  virtual NullablePtr<MacroExpansionDecl> getFreestandingMacro() const {
    return nullptr;
  }

#pragma mark - debugging and printing

public:
  virtual const SourceFile *getSourceFile() const;
  virtual std::string getClassName() const = 0;

  /// Print out this scope for debugging/reporting purposes.
  void print(llvm::raw_ostream &out, unsigned level = 0, bool lastChild = false,
             bool printChildren = true) const;

  void printRange(llvm::raw_ostream &out) const;

protected:
  virtual void printSpecifics(llvm::raw_ostream &out) const {}
  virtual NullablePtr<const void> addressForPrinting() const;

public:
  SWIFT_DEBUG_DUMP;

  void dumpOneScopeMapLocation(std::pair<unsigned, unsigned> lineColumn);

private:
  llvm::raw_ostream &verificationError() const;

#pragma mark - Scope tree creation
public:
  /// Expand the scope if unexpanded.
  ASTScopeImpl *expandAndBeCurrent(ScopeCreator &);

  bool getWasExpanded() const { return parentAndWasExpanded.getInt(); }

protected:
  void setWasExpanded() { parentAndWasExpanded.setInt(1); }
  virtual ASTScopeImpl *expandSpecifically(ScopeCreator &) = 0;

public:
  /// Some scopes can be expanded lazily.
  /// Such scopes must return an insertion point outside themselves when
  /// expanded.
  virtual NullablePtr<ASTScopeImpl> insertionPointForDeferredExpansion();

private:
  virtual ScopeCreator &getScopeCreator();

#pragma mark - lookup

public:
  using DeclConsumer = namelookup::AbstractASTScopeDeclConsumer &;

  /// Entry point into ASTScopeImpl-land for lookups
  static void
  unqualifiedLookup(SourceFile *, SourceLoc, DeclConsumer);

  /// Entry point into ASTScopeImpl-land for labeled statement lookups.
  static llvm::SmallVector<LabeledStmt *, 4>
  lookupLabeledStmts(SourceFile *sourceFile, SourceLoc loc);

  static std::pair<CaseStmt *, CaseStmt *>
  lookupFallthroughSourceAndDest(SourceFile *sourceFile, SourceLoc loc);

  static void lookupEnclosingMacroScope(
      SourceFile *sourceFile, SourceLoc loc,
      llvm::function_ref<bool(ASTScope::PotentialMacro)> consume);

  /// Scopes that cannot bind variables may set this to true to create more
  /// compact scope tree in the debug info.
  virtual bool ignoreInDebugInfo() const { return false; }

#pragma mark - - lookup- starting point
private:
  static const ASTScopeImpl *findStartingScopeForLookup(SourceFile *,
                                                        const SourceLoc where);

protected:
  /// Not const because may reexpand some scopes.
  ASTScopeImpl *findInnermostEnclosingScope(SourceLoc,
                                            NullablePtr<raw_ostream>);
  ASTScopeImpl *findInnermostEnclosingScopeImpl(SourceLoc,
                                                NullablePtr<raw_ostream>,
                                                SourceManager &,
                                                ScopeCreator &);

private:
  NullablePtr<ASTScopeImpl> findChildContaining(SourceLoc loc,
                                                SourceManager &sourceMgr) const;

#pragma mark - - lookup- per scope
protected:
  /// The main (recursive) lookup function:
  /// Tell DeclConsumer about all names found in this scope and if not done,
  /// recurse for enclosing scopes. Stop lookup if about to look in limit.
  ///
  /// If the lookup depends on implicit self, selfDC is its context.
  /// (Names in extensions never depend on self.)
  ///
  /// In a Nominal, Extension, or TypeAliasScope, the lookup can start at either
  /// the body portion (for the first two), the where portion, or a
  /// GenericParamScope. In every case, the generics on the type decl must be
  /// searched, but only once. And they must be searched *before* the generic
  /// parameters. For instance, the following is correct: \code class
  /// ShadowingGenericParameter<T> { \code   typealias T = Int;  func foo (t :
  /// T) {} \code } \code ShadowingGenericParameter<String>().foo(t: "hi")
  ///
  /// So keep track of the last generic param list searched to avoid
  /// duplicating work.
  ///
  /// Look in this scope.
  /// \param limit A scope into which lookup should not go. See \c
  /// getLookupLimit. \param lastListSearched Last list searched.
  /// \param consumer is the object to which found decls are reported.
  void lookup(NullablePtr<const ASTScopeImpl> limit,
              NullablePtr<const GenericParamList> lastListSearched,
              DeclConsumer consumer) const;

protected:
  /// Find either locals or members (no scope has both)
  /// \return True if lookup is done
  virtual bool lookupLocalsOrMembers(DeclConsumer consumer) const;

  /// Returns isDone and the list searched, if any
  std::pair<bool, NullablePtr<const GenericParamList>>
  lookInMyGenericParameters(
      NullablePtr<const GenericParamList> priorListSearched,
      DeclConsumer consumer) const;

  virtual NullablePtr<const GenericParamList> genericParams() const;

  // Consume the generic parameters in the context and its outer contexts
  static bool lookInGenericParametersOf(NullablePtr<const GenericParamList>,
                                        DeclConsumer);

  NullablePtr<const ASTScopeImpl> parentIfNotChildOfTopScope() const {
    const auto *p = getParent().get();
    return p->getParent().isNonNull() ? p : nullptr;
  }

public:
  /// The tree is organized by source location and for most nodes this is also
  /// what obtains for scoping. However, guards are different. The scope after
  /// the guard else must hop into the innermoset scope of the guard condition.
  virtual NullablePtr<const ASTScopeImpl> getLookupParent() const {
    return getParent();
  }

#pragma mark - - lookup- local bindings
protected:
  // A local binding is a basically a local variable defined in that very scope
  // It is not an instance variable or inherited type.

  static bool lookupLocalBindingsInPattern(const Pattern *p,
                                           DeclConsumer consumer);

  /// When lookup must stop before the outermost scope, return the scope to stop
  /// at. Example, if a protocol is nested in a struct, we must stop before
  /// looking into the struct.
  ///
  /// Ultimately, the task of rejecting results found in inapplicable outer
  /// scopes is best moved to the clients of the ASTScope lookup subsystem. It
  /// seems out of place here.
  virtual NullablePtr<const ASTScopeImpl> getLookupLimit() const;

  NullablePtr<const ASTScopeImpl>
  ancestorWithDeclSatisfying(function_ref<bool(const Decl *)> predicate) const;

  /// Whether this scope terminates lookup of labeled statements in the
  /// children below it, because one cannot perform a "break" or a "continue"
  /// in a child that goes outside of this scope.
  virtual bool isLabeledStmtLookupTerminator() const;

}; // end of ASTScopeImpl

#pragma mark - specific scope classes

/// The root of the scope tree.
class ASTSourceFileScope final : public ASTScopeImpl {
public:
  SourceFile *const SF;
  ScopeCreator *const scopeCreator;

  ASTSourceFileScope(SourceFile *SF, ScopeCreator *scopeCreator);

  std::string getClassName() const override;
  SourceRange
  getSourceRangeOfThisASTNode(bool omitAssertions = false) const override;

protected:
  void printSpecifics(llvm::raw_ostream &out) const override;

public:
  void buildFullyExpandedTree();
  void
  buildEnoughOfTreeForTopLevelExpressionsButDontRequestGenericsOrExtendedNominals();

  void expandFunctionBody(AbstractFunctionDecl *AFD);

  const SourceFile *getSourceFile() const override;
  NullablePtr<const void> addressForPrinting() const override { return SF; }

  ASTContext &getASTContext() const override;
  bool ignoreInDebugInfo() const override { return true; }

protected:
  ASTScopeImpl *expandSpecifically(ScopeCreator &scopeCreator) override;

  ScopeCreator &getScopeCreator() override;

private:
  AnnotatedInsertionPoint
  expandAScopeThatCreatesANewInsertionPoint(ScopeCreator &);
};

class Portion : public ASTAllocated<ASTScopeImpl> {
public:
  const char *portionName;
  Portion(const char *n) : portionName(n) {}
  virtual ~Portion() {}

  // Need this because have virtual destructors
  void operator delete(void *data) {}

  /// Return the new insertion point
  virtual ASTScopeImpl *expandScope(GenericTypeOrExtensionScope *,
                                    ScopeCreator &) const = 0;

  /// \Returns \c true if this lookup is done looking for results, else \c false.
  virtual SourceRange
  getChildlessSourceRangeOf(const GenericTypeOrExtensionScope *scope,
                            bool omitAssertions) const = 0;

  virtual bool lookupMembersOf(const GenericTypeOrExtensionScope *scope,
                               ASTScopeImpl::DeclConsumer consumer) const;

  virtual NullablePtr<const ASTScopeImpl>
  getLookupLimitFor(const GenericTypeOrExtensionScope *) const;

  virtual NullablePtr<ASTScopeImpl>
  insertionPointForDeferredExpansion(IterableTypeScope *) const = 0;
  };

  // For the whole Decl scope of a GenericType or an Extension
  class GenericTypeOrExtensionWholePortion final : public Portion {
  public:
    GenericTypeOrExtensionWholePortion() : Portion("Decl") {}
    virtual ~GenericTypeOrExtensionWholePortion() {}

    // Just for TypeAlias
    ASTScopeImpl *expandScope(GenericTypeOrExtensionScope *,
                              ScopeCreator &) const override;

    SourceRange getChildlessSourceRangeOf(const GenericTypeOrExtensionScope *,
                                          bool omitAssertions) const override;

    NullablePtr<const ASTScopeImpl>
    getLookupLimitFor(const GenericTypeOrExtensionScope *) const override;

    NullablePtr<ASTScopeImpl>
    insertionPointForDeferredExpansion(IterableTypeScope *) const override;
  };

  /// GenericTypeOrExtension = GenericType or Extension
  class GenericTypeOrExtensionWhereOrBodyPortion : public Portion {
  public:
    GenericTypeOrExtensionWhereOrBodyPortion(const char *n) : Portion(n) {}
    virtual ~GenericTypeOrExtensionWhereOrBodyPortion() {}

    bool lookupMembersOf(const GenericTypeOrExtensionScope *scope,
                         ASTScopeImpl::DeclConsumer consumer) const override;
};

/// Behavior specific to representing the trailing where clause of a
/// GenericTypeDecl or ExtensionDecl scope.
class GenericTypeOrExtensionWherePortion final
    : public GenericTypeOrExtensionWhereOrBodyPortion {
public:
  GenericTypeOrExtensionWherePortion()
      : GenericTypeOrExtensionWhereOrBodyPortion("Where") {}

  bool lookupMembersOf(const GenericTypeOrExtensionScope *scope,
                       ASTScopeImpl::DeclConsumer consumer) const override;

  ASTScopeImpl *expandScope(GenericTypeOrExtensionScope *,
                            ScopeCreator &) const override;

  SourceRange getChildlessSourceRangeOf(const GenericTypeOrExtensionScope *,
                                        bool omitAssertions) const override;

  NullablePtr<ASTScopeImpl>
  insertionPointForDeferredExpansion(IterableTypeScope *) const override;
};

/// Behavior specific to representing the Body of a NominalTypeDecl or
/// ExtensionDecl scope
class IterableTypeBodyPortion final
    : public GenericTypeOrExtensionWhereOrBodyPortion {
public:
  IterableTypeBodyPortion()
      : GenericTypeOrExtensionWhereOrBodyPortion("Body") {}

  ASTScopeImpl *expandScope(GenericTypeOrExtensionScope *,
                            ScopeCreator &) const override;
  SourceRange getChildlessSourceRangeOf(const GenericTypeOrExtensionScope *,
                                        bool omitAssertions) const override;

  NullablePtr<ASTScopeImpl>
  insertionPointForDeferredExpansion(IterableTypeScope *) const override;
};

/// GenericType or Extension scope
/// : Whole type decl, trailing where clause, or body
class GenericTypeOrExtensionScope : public ASTScopeImpl {
public:
  const Portion *const portion;

  GenericTypeOrExtensionScope(const Portion *p) : portion(p) {}
  virtual ~GenericTypeOrExtensionScope() {}

  virtual NullablePtr<IterableDeclContext> getIterableDeclContext() const {
    return nullptr;
  }
  virtual bool shouldHaveABody() const { return false; }

protected:
  ASTScopeImpl *expandSpecifically(ScopeCreator &scopeCreator) override;

public:
  virtual void expandBody(ScopeCreator &);

  virtual Decl *getDecl() const = 0;
  NullablePtr<Decl> getDeclIfAny() const override { return getDecl(); }

private:
  AnnotatedInsertionPoint
  expandAScopeThatCreatesANewInsertionPoint(ScopeCreator &);

public:
  SourceRange
  getSourceRangeOfThisASTNode(bool omitAssertions = false) const override;

  /// \c tryBindExtension needs to get the extended nominal, and the DeclContext
  /// is the parent of the \c ExtensionDecl. If the \c SourceRange of an \c
  /// ExtensionScope were to start where the \c ExtensionDecl says, the lookup
  /// source location would fall within the \c ExtensionScope. This inclusion
  /// would cause the lazy \c ExtensionScope to be expanded which would ask for
  /// its generic parameters in order to create those sub-scopes. That request
  /// would cause a cycle because it would ask for the extended nominal. So,
  /// move the source range of an \c ExtensionScope *past* the extended nominal
  /// type, which is not in-scope there anyway.
  virtual SourceRange moveStartPastExtendedNominal(SourceRange) const = 0;

  virtual GenericContext *getGenericContext() const = 0;
  std::string getClassName() const override;
  virtual std::string declKindName() const = 0;
  virtual bool doesDeclHaveABody() const;
  const char *portionName() const { return portion->portionName; }

public:
  // Only for DeclScope, not BodyScope
  // Returns the where clause scope, or the parent if none
  virtual ASTScopeImpl *createTrailingWhereClauseScope(ASTScopeImpl *parent,
                                                       ScopeCreator &);
  virtual NullablePtr<NominalTypeDecl> getCorrespondingNominalTypeDecl() const {
    return nullptr;
  }

  bool areMembersVisibleFromWhereClause() const;

  virtual void createBodyScope(ASTScopeImpl *leaf, ScopeCreator &) {}

protected:
  bool
  lookupLocalsOrMembers(ASTScopeImpl::DeclConsumer consumer) const override;
  void printSpecifics(llvm::raw_ostream &out) const override;

public:
  NullablePtr<const ASTScopeImpl> getLookupLimit() const override;
  virtual NullablePtr<const ASTScopeImpl> getLookupLimitForDecl() const;
};

class GenericTypeScope : public GenericTypeOrExtensionScope {
public:
  GenericTypeScope(const Portion *p) : GenericTypeOrExtensionScope(p) {}
  virtual ~GenericTypeScope() {}
  SourceRange moveStartPastExtendedNominal(SourceRange) const override;

protected:
  NullablePtr<const GenericParamList> genericParams() const override;
};

class IterableTypeScope : public GenericTypeScope {
public:
  IterableTypeScope(const Portion *p) : GenericTypeScope(p) {}
  virtual ~IterableTypeScope() {}

  virtual SourceRange getBraces() const = 0;
  bool shouldHaveABody() const override { return true; }
  bool doesDeclHaveABody() const override;
  void expandBody(ScopeCreator &) override;

public:
  NullablePtr<ASTScopeImpl> insertionPointForDeferredExpansion() override;
};

class NominalTypeScope final : public IterableTypeScope {
public:
  NominalTypeDecl *decl;
  NominalTypeScope(const Portion *p, NominalTypeDecl *e)
      : IterableTypeScope(p), decl(e) {}
  virtual ~NominalTypeScope() {}

  std::string declKindName() const override { return "NominalType"; }
  NullablePtr<IterableDeclContext> getIterableDeclContext() const override {
    return decl;
  }
  NullablePtr<NominalTypeDecl>
  getCorrespondingNominalTypeDecl() const override {
    return decl;
  }
  GenericContext *getGenericContext() const override { return decl; }
  Decl *getDecl() const override { return decl; }

  SourceRange getBraces() const override;
  NullablePtr<const ASTScopeImpl> getLookupLimitForDecl() const override;

  void createBodyScope(ASTScopeImpl *leaf, ScopeCreator &) override;
  ASTScopeImpl *createTrailingWhereClauseScope(ASTScopeImpl *parent,
                                               ScopeCreator &) override;
};

class ExtensionScope final : public IterableTypeScope {
public:
  ExtensionDecl *const decl;
  ExtensionScope(const Portion *p, ExtensionDecl *e)
      : IterableTypeScope(p), decl(e) {}
  virtual ~ExtensionScope() {}

  GenericContext *getGenericContext() const override { return decl; }
  NullablePtr<IterableDeclContext> getIterableDeclContext() const override {
    return decl;
  }
  NullablePtr<NominalTypeDecl> getCorrespondingNominalTypeDecl() const override;
  std::string declKindName() const override { return "Extension"; }
  SourceRange getBraces() const override;
  SourceRange moveStartPastExtendedNominal(SourceRange) const override;
  ASTScopeImpl *createTrailingWhereClauseScope(ASTScopeImpl *parent,
                                               ScopeCreator &) override;
  void createBodyScope(ASTScopeImpl *leaf, ScopeCreator &) override;
  Decl *getDecl() const override { return decl; }
  NullablePtr<const ASTScopeImpl> getLookupLimitForDecl() const override;
protected:
  NullablePtr<const GenericParamList> genericParams() const override;
};

class TypeAliasScope final : public GenericTypeScope {
public:
  TypeAliasDecl *const decl;
  TypeAliasScope(const Portion *p, TypeAliasDecl *e)
      : GenericTypeScope(p), decl(e) {}
  virtual ~TypeAliasScope() {}

  std::string declKindName() const override { return "TypeAlias"; }
  ASTScopeImpl *createTrailingWhereClauseScope(ASTScopeImpl *parent,
                                               ScopeCreator &) override;
  GenericContext *getGenericContext() const override { return decl; }
  Decl *getDecl() const override { return decl; }
};

class OpaqueTypeScope final : public GenericTypeScope {
public:
  OpaqueTypeDecl *const decl;
  OpaqueTypeScope(const Portion *p, OpaqueTypeDecl *e)
      : GenericTypeScope(p), decl(e) {}
  virtual ~OpaqueTypeScope() {}

  std::string declKindName() const override { return "OpaqueType"; }
  GenericContext *getGenericContext() const override { return decl; }
  Decl *getDecl() const override { return decl; }
};

/// Since each generic parameter can "see" the preceding ones,
/// (e.g. <A, B: A>) -- it's not legal but that's how lookup behaves --
/// Each GenericParamScope scopes just ONE parameter, and we next
/// each one within the previous one.
///
/// Here's a wrinkle: for a Subscript, the caller expects this scope (based on
/// source loc) to match requested DeclContexts for starting lookup in EITHER
/// the getter or setter AbstractFunctionDecl (context)
class GenericParamScope final : public ASTScopeImpl {
public:
  /// The declaration that has generic parameters.
  Decl *const holder;
  /// The generic parameters themselves.
  GenericParamList *const paramList;
  /// The index of the current parameter.
  const unsigned index;

  GenericParamScope(Decl *holder, GenericParamList *paramList, unsigned index)
      : holder(holder), paramList(paramList), index(index) {}
  virtual ~GenericParamScope() {}

  /// Actually holder is always a GenericContext, need to test if
  /// ProtocolDecl or SubscriptDecl but will refactor later.
  std::string getClassName() const override;
  SourceRange
  getSourceRangeOfThisASTNode(bool omitAssertions = false) const override;

protected:
  ASTScopeImpl *expandSpecifically(ScopeCreator &) override;
  void printSpecifics(llvm::raw_ostream &out) const override;

public:
  NullablePtr<const void> addressForPrinting() const override {
    return paramList;
  }

protected:
  bool lookupLocalsOrMembers(DeclConsumer) const override;
};

/// Concrete class for a function/initializer/deinitializer
class AbstractFunctionDeclScope final : public ASTScopeImpl {
public:
  AbstractFunctionDecl *const decl;
  AbstractFunctionDeclScope(AbstractFunctionDecl *e) : decl(e) {}
  virtual ~AbstractFunctionDeclScope() {}

protected:
  ASTScopeImpl *expandSpecifically(ScopeCreator &scopeCreator) override;

private:
  void expandAScopeThatDoesNotCreateANewInsertionPoint(ScopeCreator &);

public:
  std::string getClassName() const override;
  SourceRange
  getSourceRangeOfThisASTNode(bool omitAssertions = false) const override;

protected:
  void printSpecifics(llvm::raw_ostream &out) const override;

public:
  virtual NullablePtr<Decl> getDeclIfAny() const override { return decl; }
  Decl *getDecl() const { return decl; }

protected:
  NullablePtr<const GenericParamList> genericParams() const override;
};

/// The parameters for an abstract function (init/func/deinit)., subscript, and
/// enum element
class ParameterListScope final : public ASTScopeImpl {
public:
  ParameterList *const params;
  /// For get functions in subscript declarations,
  /// a lookup into the subscript parameters must count as the get func context.
  const NullablePtr<DeclContext> matchingContext;

  ParameterListScope(ParameterList *params,
                     NullablePtr<DeclContext> matchingContext)
      : params(params), matchingContext(matchingContext) {}
  virtual ~ParameterListScope() {}

protected:
  ASTScopeImpl *expandSpecifically(ScopeCreator &scopeCreator) override;

private:
  void expandAScopeThatDoesNotCreateANewInsertionPoint(ScopeCreator &);

public:
  std::string getClassName() const override;
  SourceRange
  getSourceRangeOfThisASTNode(bool omitAssertions = false) const override;

  NullablePtr<const void> addressForPrinting() const override { return params; }
  bool ignoreInDebugInfo() const override { return true; }
};

/// Body of functions, methods, constructors, destructors and accessors.
class FunctionBodyScope : public ASTScopeImpl {
public:
  AbstractFunctionDecl *const decl;

  FunctionBodyScope(AbstractFunctionDecl *e) : decl(e) {}
  virtual ~FunctionBodyScope() {}

protected:
  ASTScopeImpl *expandSpecifically(ScopeCreator &scopeCreator) override;

private:
  void expandAScopeThatDoesNotCreateANewInsertionPoint(ScopeCreator &);
  void expandBody(ScopeCreator &);

public:
  SourceRange
  getSourceRangeOfThisASTNode(bool omitAssertions = false) const override;
  virtual NullablePtr<Decl> getDeclIfAny() const override { return decl; }
  Decl *getDecl() const { return decl; }
  bool ignoreInDebugInfo() const override { return true; }

protected:
  bool lookupLocalsOrMembers(DeclConsumer) const override;

public:
  std::string getClassName() const override;
  NullablePtr<ASTScopeImpl> insertionPointForDeferredExpansion() override;
};

class DefaultArgumentInitializerScope final : public ASTScopeImpl {
public:
  ParamDecl *const decl;

  DefaultArgumentInitializerScope(ParamDecl *e) : decl(e) {}
  ~DefaultArgumentInitializerScope() {}

protected:
  ASTScopeImpl *expandSpecifically(ScopeCreator &scopeCreator) override;

public:
  void expandAScopeThatDoesNotCreateANewInsertionPoint(ScopeCreator &);
  std::string getClassName() const override;
  SourceRange
  getSourceRangeOfThisASTNode(bool omitAssertions = false) const override;
  virtual NullablePtr<Decl> getDeclIfAny() const override { return decl; }
  Decl *getDecl() const { return decl; }
  bool ignoreInDebugInfo() const override { return true; }
};

/// The scope for custom attributes and their arguments, such as for
/// attached property wrappers and for attached macros.
///
/// Source locations for the attribute name and its arguments are in the
/// custom attribute, so lookup is invoked from within the attribute
/// itself.
class CustomAttributeScope final : public ASTScopeImpl {
public:
  CustomAttr *attr;
  Decl *decl;

  CustomAttributeScope(CustomAttr *attr,Decl *decl)
      : attr(attr), decl(decl) {}
  virtual ~CustomAttributeScope() {}

protected:
  ASTScopeImpl *expandSpecifically(ScopeCreator &) override;

public:
  std::string getClassName() const override;
  SourceRange
  getSourceRangeOfThisASTNode(bool omitAssertions = false) const override;
  NullablePtr<const void> addressForPrinting() const override { return decl; }

  NullablePtr<DeclAttribute> getDeclAttributeIfAny() const override {
    return attr;
  }
  bool ignoreInDebugInfo() const override { return true; }
  
private:
  void expandAScopeThatDoesNotCreateANewInsertionPoint(ScopeCreator &);
};

/// PatternBindingDecl's (PBDs) are tricky (See the comment for \c
/// PatternBindingDecl):
///
/// A PBD contains a list of "patterns", e.g.
///   var (a, b) = foo(), (c,d) = bar() which has two patterns.
///
/// For each pattern, there will be potentially three scopes:
/// always one for the declarations, maybe one for the initializers, and maybe
/// one for users of that pattern.
///
/// If a PBD occurs in code, its initializer can access all prior declarations.
/// Thus, a new scope must be created, nested in the scope of the PBD.
/// In contrast, if a PBD occurs in a type declaration body, its initializer
/// cannot access prior declarations in that body.
///
/// As a further complication, we get VarDecls and their accessors in deferred
/// which really must go into one of the PBD scopes. So we discard them in
/// createIfNeeded, and special-case their creation in
/// addVarDeclScopesAndTheirAccessors.

class AbstractPatternEntryScope : public ASTScopeImpl {
public:
  PatternBindingDecl *const decl;
  const unsigned patternEntryIndex;

  AbstractPatternEntryScope(PatternBindingDecl *, unsigned entryIndex);
  virtual ~AbstractPatternEntryScope() {}

  const PatternBindingEntry &getPatternEntry() const;
  Pattern *getPattern() const;

protected:
  void printSpecifics(llvm::raw_ostream &out) const override;

public:
  NullablePtr<Decl> getDeclIfAny() const override { return decl; }
  Decl *getDecl() const { return decl; }
};

class PatternEntryDeclScope final : public AbstractPatternEntryScope {
  const bool isLocalBinding;
  Optional<SourceLoc> endLoc;

public:
  PatternEntryDeclScope(PatternBindingDecl *pbDecl, unsigned entryIndex,
                        bool isLocalBinding, Optional<SourceLoc> endLoc)
      : AbstractPatternEntryScope(pbDecl, entryIndex),
        isLocalBinding(isLocalBinding), endLoc(endLoc) {}
  virtual ~PatternEntryDeclScope() {}

protected:
  ASTScopeImpl *expandSpecifically(ScopeCreator &scopeCreator) override;

private:
  AnnotatedInsertionPoint
  expandAScopeThatCreatesANewInsertionPoint(ScopeCreator &);

public:
  std::string getClassName() const override;
  SourceRange
  getSourceRangeOfThisASTNode(bool omitAssertions = false) const override;

protected:
  bool lookupLocalsOrMembers(DeclConsumer) const override;
  bool isLabeledStmtLookupTerminator() const override;
};

class PatternEntryInitializerScope final : public AbstractPatternEntryScope {
  Expr *initAsWrittenWhenCreated;

public:
  PatternEntryInitializerScope(PatternBindingDecl *pbDecl, unsigned entryIndex)
      : AbstractPatternEntryScope(pbDecl, entryIndex),
        initAsWrittenWhenCreated(pbDecl->isDebuggerBinding() ?
                                 pbDecl->getInit(entryIndex) :
                                 pbDecl->getOriginalInit(entryIndex)) {}
  virtual ~PatternEntryInitializerScope() {}

protected:
  ASTScopeImpl *expandSpecifically(ScopeCreator &scopeCreator) override;
  NullablePtr<const ASTScopeImpl> getLookupParent() const override;

private:
  void expandAScopeThatDoesNotCreateANewInsertionPoint(ScopeCreator &);

public:
  std::string getClassName() const override;
  SourceRange
  getSourceRangeOfThisASTNode(bool omitAssertions = false) const override;

protected:
  bool lookupLocalsOrMembers(DeclConsumer) const override;
  bool isLabeledStmtLookupTerminator() const override;
};

/// The scope introduced by a conditional clause initializer in an
/// if/while/guard statement.
class ConditionalClauseInitializerScope final : public ASTScopeImpl {
public:
  Expr *const initializer;
  const SourceRange bodyRange;

  ConditionalClauseInitializerScope(Expr *initializer)
      : initializer(initializer) {}

  virtual ~ConditionalClauseInitializerScope() {}
  SourceRange
  getSourceRangeOfThisASTNode(bool omitAssertions = false) const override;
  std::string getClassName() const override;
  bool ignoreInDebugInfo() const override { return true; }

private:
  void expandAScopeThatDoesNotCreateANewInsertionPoint(ScopeCreator &);

protected:
  ASTScopeImpl *expandSpecifically(ScopeCreator &scopeCreator) override;
  NullablePtr<const ASTScopeImpl> getLookupParent() const override;
};

/// If, while, & guard statements all start with a conditional clause, then some
/// later part of the statement, (then, body, or after the guard) circumvents
/// the normal lookup rule to pass the lookup scope into the deepest conditional
/// clause.
class ConditionalClausePatternUseScope final : public ASTScopeImpl {
  StmtConditionElement sec;
  SourceLoc endLoc;

public:
  ConditionalClausePatternUseScope(StmtConditionElement sec, SourceLoc endLoc)
      : sec(sec), endLoc(endLoc) {}

  SourceRange
  getSourceRangeOfThisASTNode(bool omitAssertions = false) const override;
  std::string getClassName() const override;

private:
  AnnotatedInsertionPoint
  expandAScopeThatCreatesANewInsertionPoint(ScopeCreator &);

protected:
  ASTScopeImpl *expandSpecifically(ScopeCreator &) override;
  bool lookupLocalsOrMembers(DeclConsumer) const override;
  void printSpecifics(llvm::raw_ostream &out) const override;
  bool isLabeledStmtLookupTerminator() const override;
};


/// Capture lists introduce local bindings.
class CaptureListScope final : public ASTScopeImpl {
public:
  CaptureListExpr *const expr;
  CaptureListScope(CaptureListExpr *e) : expr(e) {}
  virtual ~CaptureListScope() {}

protected:
  ASTScopeImpl *expandSpecifically(ScopeCreator &scopeCreator) override;

private:
  void expandAScopeThatDoesNotCreateANewInsertionPoint(ScopeCreator &);

public:
  std::string getClassName() const override;
  SourceRange
  getSourceRangeOfThisASTNode(bool omitAssertions = false) const override;
  NullablePtr<Expr> getExprIfAny() const override { return expr; }
  Expr *getExpr() const { return expr; }
  bool lookupLocalsOrMembers(DeclConsumer) const override;
};

/// For a closure with named parameters, this scope does the local bindings.
class ClosureParametersScope final : public ASTScopeImpl {
public:
  AbstractClosureExpr *const closureExpr;

  ClosureParametersScope(AbstractClosureExpr *closureExpr)
      : closureExpr(closureExpr) {}
  virtual ~ClosureParametersScope() {}

  std::string getClassName() const override;
  SourceRange
  getSourceRangeOfThisASTNode(bool omitAssertions = false) const override;

  NullablePtr<AbstractClosureExpr> getClosureIfClosureScope() const override {
    return closureExpr;
  }
  NullablePtr<Expr> getExprIfAny() const override { return closureExpr; }
  Expr *getExpr() const { return closureExpr; }
  bool ignoreInDebugInfo() const override { return true; }

protected:
  ASTScopeImpl *expandSpecifically(ScopeCreator &scopeCreator) override;

private:
  void expandAScopeThatDoesNotCreateANewInsertionPoint(ScopeCreator &);

protected:
  bool lookupLocalsOrMembers(DeclConsumer) const override;
};

class TopLevelCodeScope final : public ASTScopeImpl {
public:
  TopLevelCodeDecl *const decl;
  SourceLoc endLoc;

  TopLevelCodeScope(TopLevelCodeDecl *e, SourceLoc endLoc)
      : decl(e), endLoc(endLoc) {}
  virtual ~TopLevelCodeScope() {}

protected:
  ASTScopeImpl *expandSpecifically(ScopeCreator &scopeCreator) override;

private:
  AnnotatedInsertionPoint
  expandAScopeThatCreatesANewInsertionPoint(ScopeCreator &);

public:
  std::string getClassName() const override;
  SourceRange
  getSourceRangeOfThisASTNode(bool omitAssertions = false) const override;
  virtual NullablePtr<Decl> getDeclIfAny() const override { return decl; }
  Decl *getDecl() const { return decl; }
};

/// The \c _@specialize attribute.
class SpecializeAttributeScope final : public ASTScopeImpl {
public:
  SpecializeAttr *const specializeAttr;
  AbstractFunctionDecl *const whatWasSpecialized;

  SpecializeAttributeScope(SpecializeAttr *specializeAttr,
                           AbstractFunctionDecl *whatWasSpecialized)
      : specializeAttr(specializeAttr), whatWasSpecialized(whatWasSpecialized) {
  }
  virtual ~SpecializeAttributeScope() {}

  std::string getClassName() const override;
  SourceRange
  getSourceRangeOfThisASTNode(bool omitAssertions = false) const override;
  NullablePtr<const void> addressForPrinting() const override {
    return specializeAttr;
  }

  NullablePtr<DeclAttribute> getDeclAttributeIfAny() const override {
    return specializeAttr;
  }

protected:
  ASTScopeImpl *expandSpecifically(ScopeCreator &) override;
  bool lookupLocalsOrMembers(DeclConsumer) const override;
};

/// A `@differentiable` attribute scope.
///
/// This exists because `@differentiable` attribute may have a `where` clause
/// referring to generic parameters from some generic context.
class DifferentiableAttributeScope final : public ASTScopeImpl {
public:
  DifferentiableAttr *const differentiableAttr;
  Decl *const attributedDeclaration;

  DifferentiableAttributeScope(DifferentiableAttr *diffAttr, Decl *decl)
      : differentiableAttr(diffAttr), attributedDeclaration(decl) {}
  virtual ~DifferentiableAttributeScope() {}

  std::string getClassName() const override;
  SourceRange
  getSourceRangeOfThisASTNode(bool omitAssertions = false) const override;
  NullablePtr<const void> addressForPrinting() const override {
    return differentiableAttr;
  }

  NullablePtr<DeclAttribute> getDeclAttributeIfAny() const override {
    return differentiableAttr;
  }

protected:
  ASTScopeImpl *expandSpecifically(ScopeCreator &) override;
  bool lookupLocalsOrMembers(DeclConsumer) const override;
};

class SubscriptDeclScope final : public ASTScopeImpl {
public:
  SubscriptDecl *const decl;

  SubscriptDeclScope(SubscriptDecl *e) : decl(e) {}
  virtual ~SubscriptDeclScope() {}

protected:
  ASTScopeImpl *expandSpecifically(ScopeCreator &scopeCreator) override;

private:
  void expandAScopeThatDoesNotCreateANewInsertionPoint(ScopeCreator &);

public:
  std::string getClassName() const override;
  SourceRange
  getSourceRangeOfThisASTNode(bool omitAssertions = false) const override;

protected:
  void printSpecifics(llvm::raw_ostream &out) const override;

public:
  virtual NullablePtr<Decl> getDeclIfAny() const override { return decl; }
  Decl *getDecl() const { return decl; }

protected:
  NullablePtr<const GenericParamList> genericParams() const override;
};

class EnumElementScope : public ASTScopeImpl {
  EnumElementDecl *const decl;

public:
  EnumElementScope(EnumElementDecl *e) : decl(e) {}

  SourceRange
  getSourceRangeOfThisASTNode(bool omitAssertions = false) const override;

  std::string getClassName() const override;
  ASTScopeImpl *expandSpecifically(ScopeCreator &) override;
  NullablePtr<Decl> getDeclIfAny() const override { return decl; }
  Decl *getDecl() const { return decl; }

private:
  void expandAScopeThatDoesNotCreateANewInsertionPoint(ScopeCreator &);
};

class MacroDeclScope final : public ASTScopeImpl {
public:
  MacroDecl *const decl;

  MacroDeclScope(MacroDecl *e) : decl(e) {}
  virtual ~MacroDeclScope() {}

protected:
  ASTScopeImpl *expandSpecifically(ScopeCreator &scopeCreator) override;

private:
  void expandAScopeThatDoesNotCreateANewInsertionPoint(ScopeCreator &);

public:
  std::string getClassName() const override;
  SourceRange
  getSourceRangeOfThisASTNode(bool omitAssertions = false) const override;

protected:
  void printSpecifics(llvm::raw_ostream &out) const override;

public:
  virtual NullablePtr<Decl> getDeclIfAny() const override { return decl; }
  Decl *getDecl() const { return decl; }

protected:
  NullablePtr<const GenericParamList> genericParams() const override;
  bool lookupLocalsOrMembers(DeclConsumer) const override;
};

/// The scope introduced for the definition of a macro, which follows the `=`.
class MacroDefinitionScope final : public ASTScopeImpl {
public:
  Expr *const definition;

  MacroDefinitionScope(Expr *definition) : definition(definition) {}

  virtual ~MacroDefinitionScope() {}
  SourceRange
  getSourceRangeOfThisASTNode(bool omitAssertions = false) const override;
  std::string getClassName() const override;

private:
  void expandAScopeThatDoesNotCreateANewInsertionPoint(ScopeCreator &);

protected:
  ASTScopeImpl *expandSpecifically(ScopeCreator &scopeCreator) override;
};

class MacroExpansionDeclScope final : public ASTScopeImpl {
public:
  MacroExpansionDecl *const decl;

  MacroExpansionDeclScope(MacroExpansionDecl *e) : decl(e) {}
  virtual ~MacroExpansionDeclScope() {}

protected:
  ASTScopeImpl *expandSpecifically(ScopeCreator &scopeCreator) override;

private:
  void expandAScopeThatDoesNotCreateANewInsertionPoint(ScopeCreator &);

public:
  std::string getClassName() const override;
  SourceRange
  getSourceRangeOfThisASTNode(bool omitAssertions = false) const override;

  NullablePtr<MacroExpansionDecl> getFreestandingMacro() const override {
    return decl;
  }

protected:
  void printSpecifics(llvm::raw_ostream &out) const override;

public:
  virtual NullablePtr<Decl> getDeclIfAny() const override { return decl; }
  Decl *getDecl() const { return decl; }
};

class AbstractStmtScope : public ASTScopeImpl {
public:
  SourceRange
  getSourceRangeOfThisASTNode(bool omitAssertions = false) const override;
  virtual Stmt *getStmt() const = 0;
  NullablePtr<Stmt> getStmtIfAny() const override { return getStmt(); }

protected:
  bool isLabeledStmtLookupTerminator() const override;
};

class LabeledConditionalStmtScope : public AbstractStmtScope {
public:
  Stmt *getStmt() const override;
  virtual LabeledConditionalStmt *getLabeledConditionalStmt() const = 0;

protected:
  /// Return the lookupParent required to search these.
  ASTScopeImpl *createNestedConditionalClauseScopes(ScopeCreator &,
                                                    SourceLoc);
};

class IfStmtScope final : public LabeledConditionalStmtScope {
public:
  IfStmt *const stmt;
  IfStmtScope(IfStmt *e) : stmt(e) {}
  virtual ~IfStmtScope() {}

protected:
  ASTScopeImpl *expandSpecifically(ScopeCreator &scopeCreator) override;

private:
  void expandAScopeThatDoesNotCreateANewInsertionPoint(ScopeCreator &);

public:
  std::string getClassName() const override;
  LabeledConditionalStmt *getLabeledConditionalStmt() const override;
};

class WhileStmtScope final : public LabeledConditionalStmtScope {
public:
  WhileStmt *const stmt;
  WhileStmtScope(WhileStmt *e) : stmt(e) {}
  virtual ~WhileStmtScope() {}

protected:
  ASTScopeImpl *expandSpecifically(ScopeCreator &scopeCreator) override;

private:
  void expandAScopeThatDoesNotCreateANewInsertionPoint(ScopeCreator &);

public:
  std::string getClassName() const override;
  LabeledConditionalStmt *getLabeledConditionalStmt() const override;
};

class GuardStmtScope final : public LabeledConditionalStmtScope {
public:
  GuardStmt *const stmt;
  SourceLoc endLoc;
  GuardStmtScope(GuardStmt *e, SourceLoc endLoc) : stmt(e), endLoc(endLoc) {}
  virtual ~GuardStmtScope() {}

protected:
  ASTScopeImpl *expandSpecifically(ScopeCreator &scopeCreator) override;

private:
  AnnotatedInsertionPoint
  expandAScopeThatCreatesANewInsertionPoint(ScopeCreator &);

public:
  std::string getClassName() const override;
  LabeledConditionalStmt *getLabeledConditionalStmt() const override;
  SourceRange
  getSourceRangeOfThisASTNode(bool omitAssertions = false) const override;
};

/// A scope for the body of a guard statement. Lookups from the body must
/// skip the parent scopes for introducing pattern bindings, since they're
/// not visible in the guard body, only after the body ends.
class GuardStmtBodyScope final : public ASTScopeImpl {
public:
  ASTScopeImpl *const lookupParent;
  BraceStmt *const body;

  GuardStmtBodyScope(ASTScopeImpl *lookupParent, BraceStmt *body)
      : lookupParent(lookupParent), body(body) {}

  SourceRange
  getSourceRangeOfThisASTNode(bool omitAssertions = false) const override;
  std::string getClassName() const override;

private:
  void expandAScopeThatDoesNotCreateANewInsertionPoint(ScopeCreator &);

protected:
  ASTScopeImpl *expandSpecifically(ScopeCreator &) override;
  NullablePtr<const ASTScopeImpl> getLookupParent() const override {
    return lookupParent;
  }
  bool isLabeledStmtLookupTerminator() const override;
};

class RepeatWhileScope final : public AbstractStmtScope {
public:
  RepeatWhileStmt *const stmt;
  RepeatWhileScope(RepeatWhileStmt *e) : stmt(e) {}
  virtual ~RepeatWhileScope() {}

protected:
  ASTScopeImpl *expandSpecifically(ScopeCreator &scopeCreator) override;

private:
  void expandAScopeThatDoesNotCreateANewInsertionPoint(ScopeCreator &);

public:
  std::string getClassName() const override;
  Stmt *getStmt() const override { return stmt; }
};

class DoStmtScope final : public AbstractStmtScope {
public:
  DoStmt *const stmt;
  DoStmtScope(DoStmt *e) : stmt(e) {}
  virtual ~DoStmtScope() {}

protected:
  ASTScopeImpl *expandSpecifically(ScopeCreator &scopeCreator) override;

private:
  void expandAScopeThatDoesNotCreateANewInsertionPoint(ScopeCreator &);

public:
  std::string getClassName() const override;
  Stmt *getStmt() const override { return stmt; }
};

class DoCatchStmtScope final : public AbstractStmtScope {
public:
  DoCatchStmt *const stmt;
  DoCatchStmtScope(DoCatchStmt *e) : stmt(e) {}
  virtual ~DoCatchStmtScope() {}

protected:
  ASTScopeImpl *expandSpecifically(ScopeCreator &scopeCreator) override;

private:
  void expandAScopeThatDoesNotCreateANewInsertionPoint(ScopeCreator &);

public:
  std::string getClassName() const override;
  Stmt *getStmt() const override { return stmt; }
};

class SwitchStmtScope final : public AbstractStmtScope {
public:
  SwitchStmt *const stmt;
  SwitchStmtScope(SwitchStmt *e) : stmt(e) {}
  virtual ~SwitchStmtScope() {}

protected:
  ASTScopeImpl *expandSpecifically(ScopeCreator &scopeCreator) override;

private:
  void expandAScopeThatDoesNotCreateANewInsertionPoint(ScopeCreator &);

public:
  std::string getClassName() const override;
  Stmt *getStmt() const override { return stmt; }
};

class ForEachStmtScope final : public AbstractStmtScope {
public:
  ForEachStmt *const stmt;
  ForEachStmtScope(ForEachStmt *e) : stmt(e) {}
  virtual ~ForEachStmtScope() {}

protected:
  ASTScopeImpl *expandSpecifically(ScopeCreator &scopeCreator) override;

private:
  void expandAScopeThatDoesNotCreateANewInsertionPoint(ScopeCreator &);

public:
  std::string getClassName() const override;
  Stmt *getStmt() const override { return stmt; }
};

class ForEachPatternScope final : public ASTScopeImpl {
public:
  ForEachStmt *const stmt;
  ForEachPatternScope(ForEachStmt *e) : stmt(e) {}
  virtual ~ForEachPatternScope() {}

protected:
  ASTScopeImpl *expandSpecifically(ScopeCreator &scopeCreator) override;

private:
  void expandAScopeThatDoesNotCreateANewInsertionPoint(ScopeCreator &);

public:
  std::string getClassName() const override;
  SourceRange
  getSourceRangeOfThisASTNode(bool omitAssertions = false) const override;

protected:
  bool lookupLocalsOrMembers(DeclConsumer) const override;
  bool isLabeledStmtLookupTerminator() const override;
};

/// The parent scope for a 'case' statement, consisting of zero or more
/// CaseLabelItemScopes, followed by a CaseStmtBodyScope.
///
/// +------------------------------------------------------------------
/// | CaseStmtScope
/// +------------------------------------------------------------------
/// |                               +--------------------------+
/// |                               | CaseLabelItemScope:      |
/// |                               +--------------------------+
/// | case .foo(let x, let y) where | condition(x, y),         |
/// |               ^------^--------------------^--^           |
/// |               this guard expression sees first 'x'/'y'   |
/// |                               +--------------------------+
/// |
/// |                               +--------------------------+
/// |                               | CaseLabelItemScope:      |
/// |                               +--------------------------+
/// |      .foo(let x, let y) where | condition(x, y),         |
/// |               ^------^--------------------^--^           |
/// |               this guard expression sees second 'x'/'y'  |
/// |                               +--------------------------+
/// |
/// |      .bar(let x, let y)
/// |               this case label item doesn't have a guard, so no
/// |               scope is created.
/// |
/// | +----------------------------------------------------------------
/// | | CaseStmtBodyScope:
/// | +----------------------------------------------------------------
/// | | {
/// | |    ... x, y  <-- body sees "joined" 'x'/'y' created by parser
/// | | }
/// | +----------------------------------------------------------------
/// |
/// +------------------------------------------------------------------

class CaseStmtScope final : public AbstractStmtScope {
public:
  CaseStmt *const stmt;
  CaseStmtScope(CaseStmt *e) : stmt(e) {}
  virtual ~CaseStmtScope() {}

protected:
  ASTScopeImpl *expandSpecifically(ScopeCreator &scopeCreator) override;

private:
  void expandAScopeThatDoesNotCreateANewInsertionPoint(ScopeCreator &);

public:
  std::string getClassName() const override;
  Stmt *getStmt() const override { return stmt; }
};

/// The scope used for the guard expression in a case statement. Any
/// variables bound by the case label item's pattern are visible in
/// this scope.
class CaseLabelItemScope final : public ASTScopeImpl {
public:
  CaseLabelItem item;
  CaseLabelItemScope(const CaseLabelItem &item) : item(item) {}
  virtual ~CaseLabelItemScope() {}

protected:
  ASTScopeImpl *expandSpecifically(ScopeCreator &scopeCreator) override;

private:
  void expandAScopeThatDoesNotCreateANewInsertionPoint(ScopeCreator &);

public:
  std::string getClassName() const override;
  SourceRange
  getSourceRangeOfThisASTNode(bool omitAssertions = false) const override;

protected:
  bool lookupLocalsOrMembers(ASTScopeImpl::DeclConsumer) const override;
};

/// The scope used for the body of a 'case' statement.
///
/// If the 'case' statement has multiple case label items, each label
/// item's pattern must bind the same variables; the parser creates
/// "fake" variables to represent the join of the variables bound by
/// each pattern.
///
/// These "fake" variables are visible in the 'case' statement body.
class CaseStmtBodyScope final : public ASTScopeImpl {
public:
  CaseStmt *const stmt;
  CaseStmtBodyScope(CaseStmt *e) : stmt(e) {}
  virtual ~CaseStmtBodyScope() {}

protected:
  ASTScopeImpl *expandSpecifically(ScopeCreator &scopeCreator) override;

private:
  void expandAScopeThatDoesNotCreateANewInsertionPoint(ScopeCreator &);

public:
  std::string getClassName() const override;
  SourceRange
  getSourceRangeOfThisASTNode(bool omitAssertions = false) const override;
protected:
  bool lookupLocalsOrMembers(ASTScopeImpl::DeclConsumer) const override;
  bool isLabeledStmtLookupTerminator() const override;
};

class BraceStmtScope final : public AbstractStmtScope {
  BraceStmt *const stmt;

  /// Declarations which are in scope from the beginning of the statement.
  ArrayRef<ValueDecl *> localFuncsAndTypes;

  /// Declarations that are normally in scope only after their
  /// definition.
  ArrayRef<VarDecl *> localVars;

  /// The end location for bindings introduced in this scope. This can
  /// extend past the actual end of the BraceStmt in top-level code,
  /// where every TopLevelCodeDecl introduces a new scope through the
  /// end of the buffer.
  SourceLoc endLoc;

public:
  BraceStmtScope(BraceStmt *e,
                 ArrayRef<ValueDecl *> localFuncsAndTypes,
                 ArrayRef<VarDecl *> localVars,
                 SourceLoc endLoc)
      : stmt(e),
        localFuncsAndTypes(localFuncsAndTypes),
        localVars(localVars),
        endLoc(endLoc) {}
  virtual ~BraceStmtScope() {}

protected:
  ASTScopeImpl *expandSpecifically(ScopeCreator &scopeCreator) override;

private:
  AnnotatedInsertionPoint
  expandAScopeThatCreatesANewInsertionPoint(ScopeCreator &);

public:
  std::string getClassName() const override;
  SourceRange
  getSourceRangeOfThisASTNode(bool omitAssertions = false) const override;

  NullablePtr<AbstractClosureExpr> parentClosureIfAny() const; // public??
  Stmt *getStmt() const override { return stmt; }

protected:
  bool lookupLocalsOrMembers(DeclConsumer) const override;
};
} // namespace ast_scope
} // namespace swift

#endif // SWIFT_AST_AST_SCOPE_H
