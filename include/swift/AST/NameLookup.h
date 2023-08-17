//===--- NameLookup.h - Swift Name Lookup Routines --------------*- C++ -*-===//
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
//
// This file defines interfaces for performing name lookup.
//
//===----------------------------------------------------------------------===//

#ifndef SWIFT_AST_NAME_LOOKUP_H
#define SWIFT_AST_NAME_LOOKUP_H

#include "swift/AST/ASTVisitor.h"
#include "swift/AST/GenericSignature.h"
#include "swift/AST/Identifier.h"
#include "swift/AST/Module.h"
#include "swift/Basic/Compiler.h"
#include "swift/Basic/Debug.h"
#include "swift/Basic/NullablePtr.h"
#include "swift/Basic/SourceLoc.h"
#include "swift/Basic/SourceManager.h"
#include "llvm/ADT/SmallVector.h"
#include "swift/AST/TypeRepr.h"

namespace swift {
class ASTContext;
class DeclName;
class Type;
class TypeDecl;
class ValueDecl;
struct SelfBounds;
class NominalTypeDecl;
namespace ast_scope {
class ASTSourceFileScope;
class ASTScopeImpl;
} // namespace ast_scope

/// Walk the type representation recursively, collecting any
/// \c OpaqueReturnTypeRepr, \c CompositionTypeRepr  or \c DeclRefTypeRepr
/// nodes.
CollectedOpaqueReprs collectOpaqueTypeReprs(TypeRepr *, ASTContext &ctx,
                                            DeclContext *dc);

/// LookupResultEntry - One result of unqualified lookup.
struct LookupResultEntry {
private:
  /// The declaration context through which we found \c Value. For instance,
  /// \code
  /// class BaseClass {
  ///   func foo() {}
  /// }
  ///
  /// class DerivedClass : BaseClass {
  ///   func bar() {}
  /// }
  /// \endcode
  ///
  /// When finding \c foo() from the body of \c DerivedClass, \c BaseDC is \c
  /// DerivedClass.
  ///
  /// Another example:
  /// \code
  /// class BaseClass {
  ///   func bar() {}
  ///   func foo() {}
  /// }
  /// \endcode
  ///
  /// When finding \c bar() from the function body of \c foo(), \c BaseDC is
  /// the method \c foo().
  ///
  /// \c BaseDC will be the type if \c self is not needed for the lookup. If
  /// \c self is needed, \c baseDC will be either the method or a closure
  /// which explicitly captured \c self.
  /// In other words: If \c baseDC is a method or a closure, it means you
  /// found an instance member and you should add an implicit 'self.' (Each
  /// method has its own implicit self decl.) There's one other kind of
  /// non-method, non-closure context that has a 'self.' -- a lazy property
  /// initializer, which unlike a non-lazy property can reference \c self.
  /// \code
  ///  class Outer {
  ///    static func s()
  ///    func i()
  ///    class Inner {
  ///      static func ss()
  ///      func ii() {
  ///        func F() {
  ///          ii() // OK! implicitly self.ii; BaseDC is the method
  ///          s()  // OK! s() is defined in an outer type; BaseDC is the type
  ///          ss() // error: must write /Inner.ss() here since its static
  ///          i()  // error: there's no outer 'self.'
  ///        }
  ///      }
  /// \endcode
  ///
  /// To sum up:  The distinction is whether you need to know the run-time
  /// value of \c self. It might be clearer if \code baseDC was always a type,
  /// and there was an additional \c ParamDecl field in \c LookupResult which
  /// would store the implicit self, if any. \c BaseDC is always one of your
  /// outer DCs. if you're inside a type it should never be an extension of
  /// that type. And if you're inside an extension it will always be an
  /// extension (if it found something at that level).
  DeclContext *BaseDC;

  /// The declaration that defines the base of the call to `Value`.
  /// This is always available, as long as `BaseDC` is not null.
  ValueDecl *BaseDecl;

  /// The declaration corresponds to the given name; i.e. the decl we are
  /// looking up.
  ValueDecl *Value;

public:
  LookupResultEntry(ValueDecl *value)
      : BaseDC(nullptr), BaseDecl(nullptr), Value(value) {}

  LookupResultEntry(DeclContext *baseDC, ValueDecl *baseDecl, ValueDecl *value)
      : BaseDC(baseDC), BaseDecl(baseDecl), Value(value) {}

  ValueDecl *getValueDecl() const { return Value; }

  DeclContext *getDeclContext() const { return BaseDC; }

  ValueDecl *getBaseDecl() const;

  friend bool operator ==(const LookupResultEntry &lhs,
                          const LookupResultEntry &rhs) {
    return lhs.BaseDC == rhs.BaseDC && lhs.Value == rhs.Value;
  }

  void print(llvm::raw_ostream &) const;
};

/// The result of name lookup.
class LookupResult {
private:
  /// The set of results found.
  SmallVector<LookupResultEntry, 4> Results;
  size_t IndexOfFirstOuterResult = 0;

public:
  LookupResult() {}

  explicit LookupResult(const SmallVectorImpl<LookupResultEntry> &Results,
                        size_t indexOfFirstOuterResult)
      : Results(Results.begin(), Results.end()),
        IndexOfFirstOuterResult(indexOfFirstOuterResult) {}

  using iterator = SmallVectorImpl<LookupResultEntry>::iterator;
  iterator begin() { return Results.begin(); }
  iterator end() {
    return Results.begin() + IndexOfFirstOuterResult;
  }
  unsigned size() const { return innerResults().size(); }
  bool empty() const { return innerResults().empty(); }

  ArrayRef<LookupResultEntry> innerResults() const {
    return llvm::makeArrayRef(Results).take_front(IndexOfFirstOuterResult);
  }

  ArrayRef<LookupResultEntry> outerResults() const {
    return llvm::makeArrayRef(Results).drop_front(IndexOfFirstOuterResult);
  }

  /// \returns An array of both the inner and outer results.
  ArrayRef<LookupResultEntry> allResults() const {
    return llvm::makeArrayRef(Results);
  }

  const LookupResultEntry& operator[](unsigned index) const {
    return Results[index];
  }

  LookupResultEntry front() const { return innerResults().front(); }
  LookupResultEntry back() const { return innerResults().back(); }

  /// \returns The index of the first outer result within \c allResults().
  size_t getIndexOfFirstOuterResult() const { return IndexOfFirstOuterResult; }

  /// Add a result to the set of results.
  void add(LookupResultEntry result, bool isOuter) {
    Results.push_back(result);
    if (!isOuter) {
      IndexOfFirstOuterResult++;
      assert(IndexOfFirstOuterResult == Results.size() &&
             "found an outer result before an inner one");
    } else {
      assert(IndexOfFirstOuterResult > 0 &&
             "found outer results without an inner one");
    }
  }

  void clear() { Results.clear(); }

  /// Determine whether the result set is nonempty.
  explicit operator bool() const {
    return !empty();
  }

  TypeDecl *getSingleTypeResult() const {
    if (size() != 1)
      return nullptr;

    return dyn_cast<TypeDecl>(front().getValueDecl());
  }

  friend bool operator ==(const LookupResult &lhs, const LookupResult &rhs) {
    return lhs.Results == rhs.Results &&
           lhs.IndexOfFirstOuterResult == rhs.IndexOfFirstOuterResult;
  }

  /// Filter out any results that aren't accepted by the given predicate.
  void
  filter(llvm::function_ref<bool(LookupResultEntry, /*isOuter*/ bool)> pred);

  /// Shift down results by dropping inner results while keeping outer
  /// results (if any), the innermost of which are recognized as inner
  /// results afterwards.
  void shiftDownResults();
};

enum class UnqualifiedLookupFlags {
  /// This lookup should only return types.
  TypeLookup            = 1 << 0,
  /// This lookup should consider declarations within protocols to which the
  /// context type conforms.
  AllowProtocolMembers  = 1 << 2,
  /// Don't check access when doing lookup into a type.
  IgnoreAccessControl   = 1 << 3,
  /// This lookup should include results from outside the innermost scope with
  /// results.
  IncludeOuterResults   = 1 << 4,
  // This lookup should include results that are @inlinable or
  // @usableFromInline.
  IncludeUsableFromInline = 1 << 5,
  /// This lookup should exclude any names introduced by macro expansions.
  ExcludeMacroExpansions = 1 << 6,
  /// This lookup should only return macros.
  MacroLookup            = 1 << 7,
};

using UnqualifiedLookupOptions = OptionSet<UnqualifiedLookupFlags>;

void simple_display(llvm::raw_ostream &out, UnqualifiedLookupOptions options);

inline UnqualifiedLookupOptions operator|(UnqualifiedLookupFlags flag1,
                                          UnqualifiedLookupFlags flag2) {
  return UnqualifiedLookupOptions(flag1) | flag2;
}

/// Describes the reason why a certain declaration is visible.
enum class DeclVisibilityKind {
  /// Declaration is a local variable or type.
  LocalVariable,

  /// Declaration is a function parameter.
  FunctionParameter,

  /// Declaration is a generic parameter.
  GenericParameter,

  /// Declaration is a member of the immediately enclosing nominal decl.
  ///
  /// For example, 'Foo' is visible at (1) because of this.
  /// \code
  ///   struct A {
  ///     typealias Foo = Int
  ///     func f() {
  ///       // (1)
  ///     }
  ///   }
  /// \endcode
  MemberOfCurrentNominal,

  /// Declaration is a requirement – in case the nominal decl does not supply
  /// a corresponding witness – or an extension member of a protocol
  /// conformed to by the immediately enclosing nominal decl.
  ///
  /// For example, 'foo' is visible at (1) because of this.
  /// \code
  /// protocol P {
  ///   func foo()
  /// }
  /// struct A : P {
  ///   func bar() {
  ///     // (1)
  ///   }
  /// }
  /// \endcode
  MemberOfProtocolConformedToByCurrentNominal,

  /// Declaration is a derived requirement of a protocol conformed to by the
  /// immediately enclosing nominal decl (a witness for a synthesized
  /// conformance).
  MemberOfProtocolDerivedByCurrentNominal,

  /// Declaration is a member of the superclass of the immediately enclosing
  /// nominal decl.
  MemberOfSuper,

  /// Declaration is a member of the non-immediately enclosing nominal decl.
  ///
  /// For example, 'Foo' is visible at (1) because of this.
  /// \code
  ///   struct A {
  ///     typealias Foo = Int
  ///     struct B {
  ///       func foo() {
  ///         // (1)
  ///       }
  ///     }
  ///   }
  /// \endcode
  MemberOfOutsideNominal,

  /// Declaration is visible at the top level because it is declared in this
  /// module or in an imported module.
  VisibleAtTopLevel,

  /// Declaration was found via \c AnyObject or \c AnyObject.Type.
  DynamicLookup,
};

/// For Decls found with DeclVisibilityKind::DynamicLookup, contains details of
/// how they were looked up. For example, the SubscriptDecl used to find a
/// KeyPath dynamic member.
class DynamicLookupInfo {
public:
  enum Kind {
    None,
    AnyObject,
    KeyPathDynamicMember,
  };

  struct KeyPathDynamicMemberInfo {
    /// The subscript(dynamicMember:) by which we found the declaration.
    SubscriptDecl *subscript = nullptr;

    /// The type context of `subscript`, which may be different than the
    /// original base type of the lookup if this declaration was found by nested
    /// dynamic lookups.
    Type baseType = Type();

    /// Visibility of the declaration itself without dynamic lookup.
    ///
    /// For example, dynamic lookup for KeyPath<Derived, U>, might find
    /// Base::foo with originalVisibility == MemberOfSuper.
    DeclVisibilityKind originalVisibility = DeclVisibilityKind::DynamicLookup;
  };

  Kind getKind() const { return kind; }

  const KeyPathDynamicMemberInfo &getKeyPathDynamicMember() const;

  DynamicLookupInfo() : kind(None) {}
  DynamicLookupInfo(Kind kind) : kind(kind) {
    assert(kind != KeyPathDynamicMember && "use KeyPathDynamicMemberInfo ctor");
  }

  /// Construct for a KeyPath dynamic member lookup.
  DynamicLookupInfo(SubscriptDecl *subscript, Type baseType,
                    DeclVisibilityKind originalVisibility);

private:
  Kind kind;
  KeyPathDynamicMemberInfo keypath = {};
};

/// An abstract base class for a visitor that consumes declarations found within
/// a given context.
class VisibleDeclConsumer {
  virtual void anchor();
public:
  virtual ~VisibleDeclConsumer() = default;

  /// This method is called every time it look for members from a decl.
  virtual void onLookupNominalTypeMembers(NominalTypeDecl *NTD,
                                          DeclVisibilityKind Reason) {}

  /// This method is called by findVisibleDecls() every time it finds a decl.
  virtual void foundDecl(ValueDecl *VD, DeclVisibilityKind Reason,
                         DynamicLookupInfo dynamicLookupInfo = {}) = 0;
};

/// An implementation of VisibleDeclConsumer that's built from a lambda.
template <class Fn>
class LambdaDeclConsumer : public VisibleDeclConsumer {
  Fn Callback;
public:
  LambdaDeclConsumer(Fn &&callback) : Callback(std::move(callback)) {}

  void foundDecl(ValueDecl *VD, DeclVisibilityKind reason, DynamicLookupInfo) override {
    Callback(VD, reason);
  }
};
template <class Fn>
LambdaDeclConsumer<Fn> makeDeclConsumer(Fn &&callback) {
  return LambdaDeclConsumer<Fn>(std::move(callback));
}

/// A consumer that inserts found decls into an externally-owned SmallVector.
class VectorDeclConsumer : public VisibleDeclConsumer {
  virtual void anchor() override;
public:
  SmallVectorImpl<ValueDecl *> &results;
  explicit VectorDeclConsumer(SmallVectorImpl<ValueDecl *> &decls)
    : results(decls) {}

  virtual void foundDecl(ValueDecl *VD, DeclVisibilityKind Reason,
                         DynamicLookupInfo) override {
    results.push_back(VD);
  }
};

/// A consumer that filters out decls that are not accessible from a given
/// DeclContext.
class AccessFilteringDeclConsumer final : public VisibleDeclConsumer {
  const DeclContext *DC;
  VisibleDeclConsumer &ChainedConsumer;

public:
  AccessFilteringDeclConsumer(const DeclContext *DC,
                              VisibleDeclConsumer &consumer)
    : DC(DC), ChainedConsumer(consumer) {}

  void onLookupNominalTypeMembers(NominalTypeDecl *NTD,
                                  DeclVisibilityKind Reason) override {
    ChainedConsumer.onLookupNominalTypeMembers(NTD, Reason);
  }

  void foundDecl(ValueDecl *D, DeclVisibilityKind reason,
                 DynamicLookupInfo dynamicLookupInfo = {}) override;
};

/// Filters out decls that are not usable based on their source location, eg.
/// a decl inside its own initializer or a non-type decl before its definition.
class UsableFilteringDeclConsumer final : public VisibleDeclConsumer {
  const SourceManager &SM;
  const DeclContext *DC;
  const DeclContext *typeContext;
  SourceLoc Loc;
  llvm::DenseMap<DeclBaseName, std::pair<ValueDecl *, DeclVisibilityKind>>
      SeenNames;
  VisibleDeclConsumer &ChainedConsumer;

public:
  UsableFilteringDeclConsumer(const SourceManager &SM, const DeclContext *DC,
                              SourceLoc loc, VisibleDeclConsumer &consumer)
      : SM(SM), DC(DC), typeContext(DC->getInnermostTypeContext()), Loc(loc),
        ChainedConsumer(consumer) {}

  void onLookupNominalTypeMembers(NominalTypeDecl *NTD,
                                  DeclVisibilityKind Reason) override {
    ChainedConsumer.onLookupNominalTypeMembers(NTD, Reason);
  }

  void foundDecl(ValueDecl *D, DeclVisibilityKind reason,
                 DynamicLookupInfo dynamicLookupInfo) override;
};

/// Remove any declarations in the given set that were overridden by
/// other declarations in that set.
///
/// \returns true if any declarations were removed, false otherwise.
bool removeOverriddenDecls(SmallVectorImpl<ValueDecl*> &decls);

/// Remove any declarations in the given set that are shadowed by
/// other declarations in that set.
///
/// \param decls The set of declarations being considered.
/// \param dc The DeclContext from which the lookup was performed.
///
/// \returns true if any shadowed declarations were removed.
bool removeShadowedDecls(SmallVectorImpl<ValueDecl*> &decls,
                         const DeclContext *dc);

/// Remove any operators in the given set that are shadowed by
/// other operators in that set.
///
/// \param decls The set of operators being considered.
/// \param dc The DeclContext from which the lookup was performed.
///
/// \returns true if any shadowed declarations were removed.
bool removeShadowedDecls(TinyPtrVector<OperatorDecl *> &decls,
                         const DeclContext *dc);

/// Remove any precedence groups in the given set that are shadowed by
/// other precedence groups in that set.
///
/// \param decls The set of precedence groups being considered.
/// \param dc The DeclContext from which the lookup was performed.
///
/// \returns true if any shadowed declarations were removed.
bool removeShadowedDecls(TinyPtrVector<PrecedenceGroupDecl *> &decls,
                         const DeclContext *dc);

/// Finds decls visible in the given context and feeds them to the given
/// VisibleDeclConsumer.  If the current DeclContext is nested in a function,
/// the SourceLoc is used to determine which declarations in that function
/// are visible.
void lookupVisibleDecls(VisibleDeclConsumer &Consumer,
                        const DeclContext *DC,
                        bool IncludeTopLevel,
                        SourceLoc Loc = SourceLoc());

/// Finds decls visible as members of the given type and feeds them to the given
/// VisibleDeclConsumer.
///
/// \param CurrDC the DeclContext from which the lookup is done.
void lookupVisibleMemberDecls(VisibleDeclConsumer &Consumer,
                              Type BaseTy, SourceLoc loc,
                              const DeclContext *CurrDC,
                              bool includeInstanceMembers,
                              bool includeDerivedRequirements,
                              bool includeProtocolExtensionMembers,
                              GenericSignature genericSig = GenericSignature());

namespace namelookup {

/// Add semantic members to \p type before attempting a semantic lookup.
void installSemanticMembersIfNeeded(Type type, DeclNameRef name);

/// Extracts directly referenced nominal type decls from a given type, asserting
/// if the type does not contain any references to a nominal.
void extractDirectlyReferencedNominalTypes(
    Type type, SmallVectorImpl<NominalTypeDecl *> &decls);

/// Extracts directly referenced nominal type decls from a given type, or leaves
/// the vector empty if the type does not contain any references to a nominal.
void tryExtractDirectlyReferencedNominalTypes(
    Type type, SmallVectorImpl<NominalTypeDecl *> &decls);

/// Once name lookup has gathered a set of results, perform any necessary
/// steps to prune the result set before returning it to the caller.
void pruneLookupResultSet(const DeclContext *dc, NLOptions options,
                          SmallVectorImpl<ValueDecl *> &decls);

/// Do nothing if debugClient is null.
template <typename Result>
void filterForDiscriminator(SmallVectorImpl<Result> &results,
                            DebuggerClient *debugClient);

/// \returns The set of macro declarations with the given name that
/// fulfill any of the given macro roles.
SmallVector<MacroDecl *, 1>
lookupMacros(DeclContext *dc, DeclNameRef macroName, MacroRoles roles);

/// \returns Whether the given source location is inside an attached
/// or freestanding macro argument.
bool isInMacroArgument(SourceFile *sourceFile, SourceLoc loc);

/// Call the given function body with each macro declaration and its associated
/// role attribute for the given role.
///
/// This routine intentionally avoids calling `forEachAttachedMacro`, which
/// triggers request cycles, and should only be used when resolving macro
/// names for the purposes of (other) name lookup.
void forEachPotentialResolvedMacro(
    DeclContext *moduleScopeCtx, DeclNameRef macroName, MacroRole role,
    llvm::function_ref<void(MacroDecl *, const MacroRoleAttr *)> body
);

/// For each macro with the given role that might be attached to the given
/// declaration, call the body.
void forEachPotentialAttachedMacro(
    Decl *decl, MacroRole role,
    llvm::function_ref<void(MacroDecl *macro, const MacroRoleAttr *)> body
);

} // end namespace namelookup

/// Describes an inherited nominal entry.
struct InheritedNominalEntry : Located<NominalTypeDecl *> {
  /// The location of the "unchecked" attribute, if present.
  SourceLoc uncheckedLoc;

  InheritedNominalEntry() { }

  InheritedNominalEntry(
    NominalTypeDecl *item, SourceLoc loc,
    SourceLoc uncheckedLoc
  ) : Located(item, loc), uncheckedLoc(uncheckedLoc) { }
};

/// Retrieve the set of nominal type declarations that are directly
/// "inherited" by the given declaration at a particular position in the
/// list of "inherited" types.
///
/// Add anything we find to the \c result vector. If we come across the
/// AnyObject type, set \c anyObject true.
void getDirectlyInheritedNominalTypeDecls(
    llvm::PointerUnion<const TypeDecl *, const ExtensionDecl *> decl,
    unsigned i, llvm::SmallVectorImpl<InheritedNominalEntry> &result,
    bool &anyObject);

/// Retrieve the set of nominal type declarations that are directly
/// "inherited" by the given declaration, looking through typealiases
/// and splitting out the components of compositions.
///
/// If we come across the AnyObject type, set \c anyObject true.
SmallVector<InheritedNominalEntry, 4> getDirectlyInheritedNominalTypeDecls(
    llvm::PointerUnion<const TypeDecl *, const ExtensionDecl *> decl,
    bool &anyObject);

/// Retrieve the set of nominal type declarations that appear as the
/// constraint type of any "Self" constraints in the where clause of the
/// given protocol or protocol extension.
SelfBounds getSelfBoundsFromWhereClause(
    llvm::PointerUnion<const TypeDecl *, const ExtensionDecl *> decl);

/// Retrieve the set of nominal type declarations that appear as the
/// constraint type of any "Self" constraints in the generic signature of the
/// given protocol or protocol extension.
SelfBounds getSelfBoundsFromGenericSignature(const ExtensionDecl *extDecl);

/// Retrieve the TypeLoc at the given \c index from among the set of
/// type declarations that are directly "inherited" by the given declaration.
inline const TypeLoc &getInheritedTypeLocAtIndex(
    llvm::PointerUnion<const TypeDecl *, const ExtensionDecl *> decl,
    unsigned index) {
  if (auto typeDecl = decl.dyn_cast<const TypeDecl *>())
    return typeDecl->getInherited()[index];

  return decl.get<const ExtensionDecl *>()->getInherited()[index];
}

namespace namelookup {

/// Searches through statements and patterns for local variable declarations.
class FindLocalVal : public StmtVisitor<FindLocalVal> {
  friend class ASTVisitor<FindLocalVal>;

  const SourceManager &SM;
  SourceLoc Loc;
  VisibleDeclConsumer &Consumer;

public:
  FindLocalVal(const SourceManager &SM, SourceLoc Loc,
               VisibleDeclConsumer &Consumer)
      : SM(SM), Loc(Loc), Consumer(Consumer) {}

  void checkValueDecl(ValueDecl *D, DeclVisibilityKind Reason);

  void checkPattern(const Pattern *Pat, DeclVisibilityKind Reason);
  
  void checkParameterList(const ParameterList *params);

  void checkGenericParams(GenericParamList *Params);

  void checkSourceFile(const SourceFile &SF);

private:
  bool isReferencePointInRange(SourceRange R) {
    return SM.rangeContainsTokenLoc(R, Loc);
  }

  void visitBreakStmt(BreakStmt *) {}
  void visitContinueStmt(ContinueStmt *) {}
  void visitFallthroughStmt(FallthroughStmt *) {}
  void visitFailStmt(FailStmt *) {}
  void visitReturnStmt(ReturnStmt *) {}
  void visitYieldStmt(YieldStmt *) {}
  void visitThrowStmt(ThrowStmt *) {}
  void visitDiscardStmt(DiscardStmt *) {}
  void visitPoundAssertStmt(PoundAssertStmt *) {}
  void visitDeferStmt(DeferStmt *DS) {
    // Nothing in the defer is visible.
  }

  void checkStmtCondition(const StmtCondition &Cond);

  void visitIfStmt(IfStmt *S);
  void visitGuardStmt(GuardStmt *S);

  void visitWhileStmt(WhileStmt *S);
  void visitRepeatWhileStmt(RepeatWhileStmt *S);
  void visitDoStmt(DoStmt *S);

  void visitForEachStmt(ForEachStmt *S);

  void visitBraceStmt(BraceStmt *S, bool isTopLevelCode = false);
  
  void visitSwitchStmt(SwitchStmt *S);

  void visitCaseStmt(CaseStmt *S);

  void visitDoCatchStmt(DoCatchStmt *S);
  
};


/// The bridge between the legacy UnqualifiedLookupFactory and the new ASTScope
/// lookup system
class AbstractASTScopeDeclConsumer {
public:
  AbstractASTScopeDeclConsumer() {}

  virtual ~AbstractASTScopeDeclConsumer() = default;

  /// Called for every ValueDecl visible from the lookup.
  ///
  /// Takes an array in order to batch the consumption before setting
  /// IndexOfFirstOuterResult when necessary.
  ///
  /// \param baseDC either a type context or the local context of a
  /// `self` parameter declaration. See LookupResult for a discussion
  /// of type -vs- instance lookup results.
  ///
  /// \return true if the lookup should be stopped at this point.
  virtual bool consume(ArrayRef<ValueDecl *> values,
                       NullablePtr<DeclContext> baseDC = nullptr) = 0;

  /// Look for members of a nominal type or extension scope.
  ///
  /// \return true if the lookup should be stopped at this point.
  virtual bool lookInMembers(const DeclContext *scopeDC) const = 0;

  /// Called for local VarDecls that might not yet be in scope.
  ///
  /// Note that the set of VarDecls visited here are going to be a
  /// superset of those visited in consume().
  virtual bool consumePossiblyNotInScope(ArrayRef<VarDecl *> values) {
    return false;
  }

  /// Called right before looking at the parent scope of a BraceStmt.
  ///
  /// \return true if the lookup should be stopped at this point.
  virtual bool
  finishLookupInBraceStmt(BraceStmt *stmt) {
    return false;
  }

#ifndef NDEBUG
  virtual void startingNextLookupStep() = 0;
  virtual void finishingLookup(std::string) const = 0;
  virtual bool isTargetLookup() const = 0;
#endif
};
  
/// Just used to print
/// Used to gather lookup results
class ASTScopeDeclGatherer : public AbstractASTScopeDeclConsumer {
  SmallVector<ValueDecl *, 32> values;

public:
  virtual ~ASTScopeDeclGatherer() = default;

  bool consume(ArrayRef<ValueDecl *> values,
               NullablePtr<DeclContext> baseDC = nullptr) override;

  /// Eventually this functionality should move into ASTScopeLookup
  bool lookInMembers(const DeclContext *) const override {
    return false;
  }

#ifndef NDEBUG
  void startingNextLookupStep() override {}
  void finishingLookup(std::string) const override {}
  bool isTargetLookup() const override { return false; }
#endif

  ArrayRef<ValueDecl *> getDecls() { return values; }
};
} // end namespace namelookup

/// The interface into the ASTScope subsystem
class ASTScope : public ASTAllocated<ASTScope> {
  friend class ast_scope::ASTScopeImpl;
  ast_scope::ASTSourceFileScope *const impl;

public:
  ASTScope(SourceFile *);

  void
  buildEnoughOfTreeForTopLevelExpressionsButDontRequestGenericsOrExtendedNominals();

  static void expandFunctionBody(AbstractFunctionDecl *);

  /// Flesh out the tree for dumping
  void buildFullyExpandedTree();

  static void unqualifiedLookup(SourceFile *, SourceLoc,
                                namelookup::AbstractASTScopeDeclConsumer &);

  /// Lookup that only finds local declarations and does not trigger
  /// interface type computation.
  ///
  /// \param stopAfterInnermostBraceStmt If lookup should consider
  /// local declarations inside the innermost syntactic scope only.
  static void lookupLocalDecls(SourceFile *, DeclName, SourceLoc,
                               bool stopAfterInnermostBraceStmt,
                               SmallVectorImpl<ValueDecl *> &);

  /// Returns the result if there is exactly one, nullptr otherwise.
  static ValueDecl *lookupSingleLocalDecl(SourceFile *, DeclName, SourceLoc);

  /// Entry point to record the visible statement labels from the given
  /// point.
  ///
  /// This lookup only considers labels that are visible within the current
  /// function, so it will not return any labels from lexical scopes that
  /// are not reachable via labeled control flow.
  ///
  /// \returns the set of labeled statements visible from the given source
  /// location, with the innermost labeled statement first and proceeding
  /// to the outermost labeled statement.
  static llvm::SmallVector<LabeledStmt *, 4>
  lookupLabeledStmts(SourceFile *sourceFile, SourceLoc loc);

  /// Look for the directly enclosing case statement and the next case
  /// statement, which together act as the source and destination for a
  /// 'fallthrough' statement within a switch case.
  ///
  /// \returns a pair (fallthrough source, fallthrough dest). If the location
  /// is not within the body of a case statement at all, the fallthrough
  /// source will be \c nullptr. If there is a fallthrough source that case is
  /// the last one, the fallthrough destination will be \c nullptr. A
  /// well-formed 'fallthrough' statement has both a source and destination.
  static std::pair<CaseStmt *, CaseStmt *>
  lookupFallthroughSourceAndDest(SourceFile *sourceFile, SourceLoc loc);

  using PotentialMacro =
      llvm::PointerUnion<FreestandingMacroExpansion *, CustomAttr *>;

  /// Look up the scope tree for the nearest enclosing macro scope at
  /// the given source location.
  ///
  /// \param sourceFile The source file containing the given location.
  /// \param loc        The source location to start lookup from.
  /// \param consume    A function that is called when a potential macro
  ///                   scope is found. If \c consume returns \c true, lookup
  ///                   will stop. If \c consume returns \c false, lookup will
  ///                   continue up the scope tree.
  static void lookupEnclosingMacroScope(
      SourceFile *sourceFile, SourceLoc loc,
      llvm::function_ref<bool(PotentialMacro macro)> consume);

  SWIFT_DEBUG_DUMP;
  void print(llvm::raw_ostream &) const;
  void dumpOneScopeMapLocation(std::pair<unsigned, unsigned>);

private:
  static ast_scope::ASTSourceFileScope *createScopeTree(SourceFile *);

  void expandFunctionBodyImpl(AbstractFunctionDecl *);
};

} // end namespace swift

#endif
