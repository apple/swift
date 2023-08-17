//===--- Verifier.cpp - AST Invariant Verification ------------------------===//
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
//  This file implements a verifier of AST invariants.
//
//===----------------------------------------------------------------------===//

#include "swift/AST/ASTContext.h"
#include "swift/AST/ASTWalker.h"
#include "swift/AST/AccessScope.h"
#include "swift/AST/Decl.h"
#include "swift/AST/Effects.h"
#include "swift/AST/ExistentialLayout.h"
#include "swift/AST/Expr.h"
#include "swift/AST/ForeignAsyncConvention.h"
#include "swift/AST/ForeignErrorConvention.h"
#include "swift/AST/GenericEnvironment.h"
#include "swift/AST/GenericSignature.h"
#include "swift/AST/Initializer.h"
#include "swift/AST/MacroDiscriminatorContext.h"
#include "swift/AST/Module.h"
#include "swift/AST/ParameterList.h"
#include "swift/AST/Pattern.h"
#include "swift/AST/PrettyStackTrace.h"
#include "swift/AST/ProtocolConformance.h"
#include "swift/AST/SourceFile.h"
#include "swift/AST/Stmt.h"
#include "swift/AST/TypeCheckRequests.h"
#include "swift/AST/TypeRepr.h"
#include "swift/Basic/SourceManager.h"
#include "swift/Subsystems.h"
#include "llvm/ADT/SmallBitVector.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/raw_ostream.h"
#include <functional>
#include <type_traits>
using namespace swift;

namespace {

template<typename T>
struct ASTNodeBase {};

#define EXPR(ID, PARENT) \
    template<> \
    struct ASTNodeBase<ID ## Expr *> { \
      typedef PARENT BaseTy; \
    };
#define ABSTRACT_EXPR(ID, PARENT) EXPR(ID, PARENT)
#include "swift/AST/ExprNodes.def"

#define STMT(ID, PARENT) \
    template<> \
    struct ASTNodeBase<ID ## Stmt *> { \
      typedef PARENT BaseTy; \
    };
#include "swift/AST/StmtNodes.def"

#define DECL(ID, PARENT) \
    template<> \
    struct ASTNodeBase<ID ## Decl *> { \
      typedef PARENT BaseTy; \
    };
#define ABSTRACT_DECL(ID, PARENT) DECL(ID, PARENT)
#include "swift/AST/DeclNodes.def"

#define PATTERN(ID, PARENT) \
    template<> \
    struct ASTNodeBase<ID ## Pattern *> { \
      typedef PARENT BaseTy; \
    };
#include "swift/AST/PatternNodes.def"

template <typename Ty>
struct is_apply_expr
    : public std::integral_constant<
          bool,
          std::is_same<Ty, CallExpr>::value ||
              std::is_same<Ty, PrefixUnaryExpr>::value ||
              std::is_same<Ty, PostfixUnaryExpr>::value ||
              std::is_same<Ty, BinaryExpr>::value ||
              std::is_same<Ty, DotSyntaxCallExpr>::value ||
              std::is_same<Ty, ConstructorRefCallExpr>::value> {};

template <typename Ty>
struct is_subscript_expr
    : public std::integral_constant<
          bool, std::is_same<Ty, SubscriptExpr>::value ||
                    std::is_same<Ty, DynamicSubscriptExpr>::value> {};

template <typename Ty>
struct is_autoclosure_expr
    : public std::integral_constant<bool,
                                    std::is_same<Ty, AutoClosureExpr>::value> {
};

template <typename Ty>
struct is_apply_subscript_or_autoclosure_expr
    : public std::integral_constant<bool, is_apply_expr<Ty>::value ||
                                              is_subscript_expr<Ty>::value ||
                                              is_autoclosure_expr<Ty>::value> {
};

template <typename Verifier, typename Kind>
ASTWalker::PreWalkResult<Expr *> dispatchVisitPreExprHelper(
    Verifier &V,
    typename std::enable_if<
        is_apply_expr<typename std::remove_pointer<Kind>::type>::value,
        Kind>::type node) {
  if (V.shouldVerify(node)) {
    // Record any inout_to_pointer or array_to_pointer that we see in
    // the proper position.
    V.maybeRecordValidPointerConversion(node->getArgs());
    return ASTWalker::Action::Continue(node);
  }
  V.cleanup(node);
  return ASTWalker::Action::SkipChildren(node);
}

template <typename Verifier, typename Kind>
ASTWalker::PreWalkResult<Expr *> dispatchVisitPreExprHelper(
    Verifier &V,
    typename std::enable_if<
        is_subscript_expr<typename std::remove_pointer<Kind>::type>::value,
        Kind>::type node) {
  if (V.shouldVerify(node)) {
    // Record any inout_to_pointer or array_to_pointer that we see in
    // the proper position.
    V.maybeRecordValidPointerConversion(node->getArgs());
    return ASTWalker::Action::Continue(node);
  }
  V.cleanup(node);
  return ASTWalker::Action::SkipChildren(node);
}

template <typename Verifier, typename Kind>
ASTWalker::PreWalkResult<Expr *> dispatchVisitPreExprHelper(
    Verifier &V,
    typename std::enable_if<
        is_autoclosure_expr<typename std::remove_pointer<Kind>::type>::value,
        Kind>::type node) {
  if (V.shouldVerify(node)) {
    // Record any inout_to_pointer or array_to_pointer that we see in
    // the proper position.
    V.maybeRecordValidPointerConversionForArg(node->getSingleExpressionBody());
    return ASTWalker::Action::Continue(node);
  }
  V.cleanup(node);
  return ASTWalker::Action::SkipChildren(node);
}

template <typename Verifier, typename Kind>
ASTWalker::PreWalkResult<Expr *> dispatchVisitPreExprHelper(
    Verifier &V, typename std::enable_if<
                     !is_apply_subscript_or_autoclosure_expr<
                         typename std::remove_pointer<Kind>::type>::value,
                     Kind>::type node) {
  if (V.shouldVerify(node)) {
    return ASTWalker::Action::Continue(node);
  }
  V.cleanup(node);
  return ASTWalker::Action::SkipChildren(node);
}

namespace {
// Retrieve the "overridden" declaration of this declaration, but only if
// it's already been computed.
template <typename T> T *getOverriddenDeclIfAvailable(T *decl) {
  if (!decl->overriddenDeclsComputed())
    return nullptr;

  return cast_or_null<T>(decl->getOverriddenDecl());
}
} // namespace

class Verifier : public ASTWalker {
  PointerUnion<ModuleDecl *, SourceFile *> M;
  ASTContext &Ctx;
  llvm::raw_ostream &Out;
  const bool HadError;
  SmallVector<bool, 8> InImplicitBraceStmt;

  /// The stack of functions we're visiting.
  SmallVector<DeclContext *, 4> Functions;

  /// The stack of scopes we're visiting.
  using ScopeLike = llvm::PointerUnion<DeclContext *, BraceStmt *>;
  SmallVector<ScopeLike, 4> Scopes;

  /// The stack of generic contexts.
  using GenericLike = llvm::PointerUnion<DeclContext *, GenericEnvironment *>;
  SmallVector<GenericLike, 2> Generics;

  /// The stack of optional evaluations active at this point.
  SmallVector<OptionalEvaluationExpr *, 4> OptionalEvaluations;

  /// The set of opaque value expressions active at this point.
  llvm::DenseMap<OpaqueValueExpr *, unsigned> OpaqueValues;

  /// The set of opened existential archetypes that are currently
  /// active.
  llvm::DenseSet<OpenedArchetypeType *> OpenedExistentialArchetypes;

  /// The set of inout to pointer expr that match the following pattern:
  ///
  /// (call-expr
  ///    (brace-stmt
  ///      ... maybe other arguments ...
  ///      (inject_into_optional
  ///        (inout_to_pointer ...))
  ///      ... maybe other arguments ...))
  ///
  /// Any other inout to pointer expr that we see is invalid and the verifier
  /// will assert.
  llvm::DenseSet<InOutToPointerExpr *> ValidInOutToPointerExprs;
  llvm::DenseSet<ArrayToPointerExpr *> ValidArrayToPointerExprs;

  /// A key into ClosureDiscriminators is a combination of a
  /// ("canonicalized") local DeclContext* and a flag for whether to
  /// use the explicit closure sequence (false) or the implicit
  /// closure sequence (true).
  typedef llvm::PointerIntPair<DeclContext *, 1, bool> ClosureDiscriminatorKey;
  llvm::DenseMap<ClosureDiscriminatorKey, SmallBitVector>
      ClosureDiscriminators;
  DeclContext *CanonicalTopLevelSubcontext = nullptr;

  typedef std::pair</*MacroDiscriminatorContext*/const void *, Identifier>
      MacroExpansionDiscriminatorKey;
  llvm::DenseMap<MacroExpansionDiscriminatorKey, SmallBitVector>
      MacroExpansionDiscriminators;

  Verifier(PointerUnion<ModuleDecl *, SourceFile *> M, DeclContext *DC)
      : M(M),
        Ctx(M.is<ModuleDecl *>() ? M.get<ModuleDecl *>()->getASTContext()
                                 : M.get<SourceFile *>()->getASTContext()),
        Out(llvm::errs()), HadError(Ctx.hadError()) {
    pushScope(DC);
  }

  /// Emit an error message and abort, optionally dumping the expression.
  /// \param E if non-null, the expression to dump() followed by a new-line.
  void error(llvm::StringRef msg, Expr *E = nullptr) {
    Out << msg << "\n";
    if (E) {
      E->dump(Out);
      Out << "\n";
    }
    abort();
  }

public:
  Verifier(ModuleDecl *M, DeclContext *DC)
      : Verifier(PointerUnion<ModuleDecl *, SourceFile *>(M), DC) {}
  Verifier(SourceFile &SF, DeclContext *DC) : Verifier(&SF, DC) {}

  static Verifier forDecl(const Decl *D) {
    DeclContext *DC = D->getDeclContext();
    DeclContext *topDC = DC->getModuleScopeContext();
    if (auto SF = dyn_cast<SourceFile>(topDC))
      return Verifier(*SF, DC);
    return Verifier(topDC->getParentModule(), DC);
  }

  MacroWalking getMacroWalkingBehavior() const override {
    return MacroWalking::None;
  }

  PreWalkResult<Expr *> walkToExprPre(Expr *E) override {
    switch (E->getKind()) {
#define DISPATCH(ID) return dispatchVisitPreExpr(static_cast<ID##Expr*>(E))
#define EXPR(ID, PARENT) \
      case ExprKind::ID: \
        DISPATCH(ID);
#define UNCHECKED_EXPR(ID, PARENT) \
      case ExprKind::ID: \
        assert((HadError || !M.is<SourceFile*>() || \
                M.get<SourceFile*>()->ASTStage < SourceFile::TypeChecked) && \
               #ID "in wrong phase");\
        DISPATCH(ID);
#include "swift/AST/ExprNodes.def"
#undef DISPATCH
      }
      llvm_unreachable("not all cases handled!");
    }

    PostWalkResult<Expr *> walkToExprPost(Expr *E) override {
      switch (E->getKind()) {
#define DISPATCH(ID) return dispatchVisitPost(static_cast<ID##Expr*>(E))
#define EXPR(ID, PARENT) \
      case ExprKind::ID: \
        DISPATCH(ID);
#define UNCHECKED_EXPR(ID, PARENT) \
      case ExprKind::ID: \
        assert((HadError || !M.is<SourceFile*>() || \
                M.get<SourceFile*>()->ASTStage < SourceFile::TypeChecked) && \
               #ID "in wrong phase");\
        DISPATCH(ID);
#include "swift/AST/ExprNodes.def"
#undef DISPATCH
      }
      llvm_unreachable("not all cases handled!");
    }

    PreWalkResult<Stmt *> walkToStmtPre(Stmt *S) override {
      switch (S->getKind()) {
#define DISPATCH(ID) return dispatchVisitPreStmt(static_cast<ID##Stmt*>(S))
#define STMT(ID, PARENT) \
      case StmtKind::ID: \
        DISPATCH(ID);
#include "swift/AST/StmtNodes.def"
#undef DISPATCH
      }
      llvm_unreachable("not all cases handled!");
    }

  PostWalkResult<Stmt *> walkToStmtPost(Stmt *S) override {
      switch (S->getKind()) {
#define DISPATCH(ID) return dispatchVisitPost(static_cast<ID##Stmt*>(S))
#define STMT(ID, PARENT) \
      case StmtKind::ID: \
        DISPATCH(ID);
#include "swift/AST/StmtNodes.def"
#undef DISPATCH
      }
      llvm_unreachable("not all cases handled!");
    }

    PreWalkResult<Pattern *> walkToPatternPre(Pattern *P) override {
      switch (P->getKind()) {
#define DISPATCH(ID) \
        return dispatchVisitPrePattern(static_cast<ID##Pattern*>(P))
#define PATTERN(ID, PARENT) \
      case PatternKind::ID: \
        DISPATCH(ID);
#include "swift/AST/PatternNodes.def"
#undef DISPATCH
      }
      llvm_unreachable("not all cases handled!");
    }

  PostWalkResult<Pattern *> walkToPatternPost(Pattern *P) override {
      switch (P->getKind()) {
#define DISPATCH(ID) \
        return dispatchVisitPost(static_cast<ID##Pattern*>(P))
#define PATTERN(ID, PARENT) \
      case PatternKind::ID: \
        DISPATCH(ID);
#include "swift/AST/PatternNodes.def"
#undef DISPATCH
      }
      llvm_unreachable("not all cases handled!");
    }

    PreWalkAction walkToDeclPre(Decl *D) override {
      switch (D->getKind()) {
#define DISPATCH(ID) return dispatchVisitPre(static_cast<ID##Decl*>(D))
#define DECL(ID, PARENT) \
      case DeclKind::ID: \
        DISPATCH(ID);
#include "swift/AST/DeclNodes.def"
#undef DISPATCH
      }
      llvm_unreachable("not all cases handled!");
    }

    PostWalkAction walkToDeclPost(Decl *D) override {
      switch (D->getKind()) {
#define DISPATCH(ID) return dispatchVisitPost(static_cast<ID##Decl*>(D)).Action
#define DECL(ID, PARENT) \
      case DeclKind::ID: \
        DISPATCH(ID);
#include "swift/AST/DeclNodes.def"
#undef DISPATCH
      }

      llvm_unreachable("Unhandled declaration kind");
    }

    /// Helper template for dispatching pre-visitation.
    /// If we're visiting in pre-order, don't validate the node yet;
    /// just check whether we should stop further descent.
    template <class T> PreWalkAction dispatchVisitPre(T node) {
      if (shouldVerify(node))
        return Action::Continue();
      cleanup(node);
      return Action::SkipChildren();
    }

    /// Helper template for dispatching pre-visitation.
    ///
    /// If we're visiting in pre-order, don't validate the node yet;
    /// just check whether we should stop further descent.
    template <class T> PreWalkResult<Expr *> dispatchVisitPreExpr(T node) {
      return dispatchVisitPreExprHelper<Verifier, T>(*this, node);
    }

    /// Helper template for dispatching pre-visitation.
    /// If we're visiting in pre-order, don't validate the node yet;
    /// just check whether we should stop further descent.
    template <class T> PreWalkResult<Stmt *> dispatchVisitPreStmt(T node) {
      if (shouldVerify(node))
        return Action::Continue(node);
      cleanup(node);
      return Action::SkipChildren(node);
    }

    /// Helper template for dispatching pre-visitation.
    /// If we're visiting in pre-order, don't validate the node yet;
    /// just check whether we should stop further descent.
    template <class T>
  PreWalkResult<Pattern *> dispatchVisitPrePattern(T node) {
      if (shouldVerify(node))
        return Action::Continue(node);
      cleanup(node);
      return Action::SkipChildren(node);
    }

    /// Helper template for dispatching post-visitation.
    template <class T> PostWalkResult<T> dispatchVisitPost(T node) {
      // Verify source ranges if the AST node was parsed from source.
      auto *SF = M.dyn_cast<SourceFile *>();
      if (SF) {
        // If we are inside an implicit BraceStmt, don't verify source
        // locations.  LLDB creates implicit BraceStmts which contain a mix of
        // generated/user-written code.
        if (InImplicitBraceStmt.empty() || !InImplicitBraceStmt.back())
          checkSourceRanges(node);
      }

      // Check that nodes marked invalid have the correct type.
      checkErrors(node);

      // Always verify the node as a parsed node.
      verifyParsed(node);

      // If we've checked types already, do some extra verification.
      if (!SF || SF->ASTStage >= SourceFile::TypeChecked) {
        verifyCheckedAlways(node);
        if (!HadError && shouldVerifyChecked(node))
          verifyChecked(node);
      }

      // Clean up anything that we've placed into a stack to check.
      cleanup(node);

      // Always continue.
      return Action::Continue(node);
    }

    // Default cases for whether we should verify within the given subtree.
    bool shouldVerify(Expr *E) { return true; }
    bool shouldVerify(Stmt *S) { return true; }
    bool shouldVerify(Pattern *S) { return true; }
    bool shouldVerify(Decl *S) { return true; }

    bool shouldVerify(TypeAliasDecl *typealias) {
      // Don't verify type aliases formed by the debugger; they violate some
      // AST invariants involving archetypes.
      if (typealias->isDebuggerAlias()) return false;

      return true;
    }

    // Default cases for whether we should verify a checked subtree.
    bool shouldVerifyChecked(Expr *E) {
      if (!E->getType()) {
        // For @objc enums, we serialize the pre-type-checked integer
        // literal raw values, and thus when they are deserialized
        // they do not have a type on them.
        if (!isa<IntegerLiteralExpr>(E) && !isa<MacroExpansionExpr>(E)) {
          Out << "expression has no type\n";
          E->dump(Out);
          abort();
        }
      }
      return true;
    }
    bool shouldVerifyChecked(Stmt *S) { return true; }
    bool shouldVerifyChecked(Pattern *S) { return true; }
    bool shouldVerifyChecked(Decl *S) { return true; }

    // Only verify functions if they have bodies we can safely walk.
    // FIXME: This is a bit of a hack; we should be able to check the
    // invariants of a parsed body as well.
    bool shouldVerify(AbstractFunctionDecl *afd) {
      switch (afd->getBodyKind()) {
      case AbstractFunctionDecl::BodyKind::None:
      case AbstractFunctionDecl::BodyKind::TypeChecked:
      case AbstractFunctionDecl::BodyKind::Skipped:
      case AbstractFunctionDecl::BodyKind::SILSynthesize:
      case AbstractFunctionDecl::BodyKind::Deserialized:
        return true;

      case AbstractFunctionDecl::BodyKind::Unparsed:
      case AbstractFunctionDecl::BodyKind::Parsed:
      case AbstractFunctionDecl::BodyKind::Synthesize:
        if (auto SF = dyn_cast<SourceFile>(afd->getModuleScopeContext())) {
          return SF->ASTStage < SourceFile::TypeChecked;
        }

        return false;
      }
      llvm_unreachable("unhandled kind");
    }

    // Default cases for cleaning up as we exit a node.
    void cleanup(Expr *E) { }
    void cleanup(Stmt *S) { }
    void cleanup(Pattern *P) { }
    void cleanup(Decl *D) { }
    
    // Base cases for the various stages of verification.
    void verifyParsed(Expr *E) {}
    void verifyParsed(Stmt *S) {}
    void verifyParsed(Pattern *P) {}
    void verifyParsed(Decl *D) {
      PrettyStackTraceDecl debugStack("verifying ", D);
      if (!D->getDeclContext()) {
        Out << "every Decl should have a DeclContext\n";
        abort();
      }
      if (auto *DC = dyn_cast<DeclContext>(D)) {
        if (D->getDeclContext() != DC->getParent()) {
          Out << "Decl's DeclContext not in sync with DeclContext's parent\n";
          D->getDeclContext()->printContext(Out);
          DC->getParent()->printContext(Out);
          abort();
        }
      }
    }
    template<typename T>
    void verifyParsedBase(T ASTNode) {
      verifyParsed(cast<typename ASTNodeBase<T>::BaseTy>(ASTNode));
    }

    /// @{
    /// These verification functions are always run on type checked ASTs
    /// (even if there were errors).
    void verifyCheckedAlways(Expr *E) {
      if (E->getType())
        verifyChecked(E->getType());
    }
    void verifyCheckedAlways(Stmt *S) {}
    void verifyCheckedAlways(Pattern *P) {
      if (P->hasType() && !P->getDelayedInterfaceType())
        verifyChecked(P->getType());
    }
    void verifyCheckedAlways(Decl *D) {
    }

    template<typename T>
    void verifyCheckedAlwaysBase(T ASTNode) {
      verifyCheckedAlways(cast<typename ASTNodeBase<T>::BaseTy>(ASTNode));
    }
    /// @}

    /// @{
    /// These verification functions are run on type checked ASTs if there were
    /// no errors.
    void verifyChecked(Expr *E) {
      // Some imported expressions don't have types, even in checked mode.
      // TODO: eliminate all these
      if (!E->getType()) {
        // For @objc enums, we serialize the pre-type-checked integer
        // literal raw values, and thus when they are deserialized
        // they do not have a type on them.
        if (!isa<IntegerLiteralExpr>(E)) {
          Out << "expression has no type\n";
          E->dump(Out);
          abort();
        }
        return;
      }
    }
    void verifyChecked(Pattern *P) {
      if (!P->hasType()) {
        Out << "pattern has no type\n";
        P->dump(Out);
        abort();
      }
    }
    void verifyChecked(Stmt *S) {}
    void verifyChecked(Decl *D) {}

    void verifyChecked(Type type) {
      llvm::SmallPtrSet<ArchetypeType *, 4> visitedArchetypes;
      verifyChecked(type, visitedArchetypes);
    }

    void
    verifyChecked(Type type,
                  llvm::SmallPtrSetImpl<ArchetypeType *> &visitedArchetypes) {
      if (!type)
        return;

      // Check for type variables that escaped the type checker.
      if (type->hasTypeVariable()) {
        Out << "a type variable escaped the type checker\n";
        abort();
      }

      // Check for invalid pack expansion shape types.
      if (auto *expansion = type->getAs<PackExpansionType>()) {
        auto countType = expansion->getCountType();
        if (!(countType->is<PackType>() ||
              countType->is<PackArchetypeType>() ||
              countType->isRootParameterPack())) {
          Out << "non-pack shape type" << countType->getString() << "\n";
          abort();
        }
      }

      if (!type->hasArchetype())
        return;

      bool foundError = type->getCanonicalType().findIf([&](Type type) -> bool {
        if (auto archetype = type->getAs<ArchetypeType>()) {
          auto root = archetype->getRoot();
          
          // Opaque archetypes are globally available. We don't need to check
          // them here.
          if (isa<OpaqueTypeArchetypeType>(root))
            return false;
          
          // Only visit each archetype once.
          if (!visitedArchetypes.insert(archetype).second)
            return false;
          
          // We should know about archetypes corresponding to opened
          // existential archetypes.
          if (auto opened = dyn_cast<OpenedArchetypeType>(root)) {
            if (OpenedExistentialArchetypes.count(opened) == 0) {
              Out << "Found opened existential archetype "
                  << root->getString()
                  << " outside enclosing OpenExistentialExpr\n";
              return true;
            }

            return false;
          }

          // Otherwise, the archetype needs to be from this scope.
          if (Generics.empty() || !Generics.back()) {
            Out << "AST verification error: archetype outside of generic "
                   "context: " << root->getString() << "\n";
            return true;
          }

          // Get the archetype's generic signature.
          GenericEnvironment *archetypeEnv = root->getGenericEnvironment();
          auto archetypeSig = archetypeEnv->getGenericSignature();

          auto genericCtx = Generics.back();
          GenericSignature genericSig;
          if (auto *genericDC = genericCtx.dyn_cast<DeclContext *>()) {
            genericSig = genericDC->getGenericSignatureOfContext();
          } else {
            auto *genericEnv = genericCtx.get<GenericEnvironment *>();
            genericSig = genericEnv->getGenericSignature();

            // Check whether this archetype is a substitution from the
            // outer generic context of an opened element environment.
            if (genericEnv->getKind() == GenericEnvironment::Kind::OpenedElement) {
              auto contextSubs = genericEnv->getPackElementContextSubstitutions();
              QuerySubstitutionMap isInContext{contextSubs};
              if (isInContext(root->getInterfaceType()->castTo<GenericTypeParamType>()))
                return false;
            }
          }

          if (genericSig.getPointer() != archetypeSig.getPointer()) {
            Out << "Archetype " << root->getString() << " not allowed "
                << "in this context\n";
            Out << "Archetype generic signature: "
                << archetypeSig->getAsString() << "\n";
            Out << "Context generic signature: "
                << genericSig->getAsString() << "\n";

            return true;
          }

          // Mapping the archetype out and back in should produce the
          // same archetype.
          auto interfaceType = archetype->getInterfaceType();
          auto contextType = archetypeEnv->mapTypeIntoContext(interfaceType);

          if (contextType.getPointer() != archetype) {
            Out << "Archetype " << archetype->getString() << "does not appear"
                << " inside its own generic environment\n";
            Out << "Interface type: " << interfaceType.getString() << "\n";
            Out << "Contextual type: " << contextType.getString() << "\n";

            return true;
          }
        }

        return false;
      });
      
      if (foundError)
        abort();
    }

    template<typename T>
    void verifyCheckedBase(T ASTNode) {
      verifyChecked(cast<typename ASTNodeBase<T>::BaseTy>(ASTNode));
    }
    /// @}

    // Specialized verifiers.

    void pushScope(DeclContext *scope) {
      Scopes.push_back(scope);
      Generics.push_back(scope);
    }
    void pushScope(BraceStmt *scope) {
      Scopes.push_back(scope);
    }
    void popScope(DeclContext *scope) {
      assert(Scopes.back().get<DeclContext*>() == scope);
      assert(Generics.back().get<DeclContext*>() == scope);
      Scopes.pop_back();
      Generics.pop_back();
    }
    void popScope(BraceStmt *scope) {
      assert(Scopes.back().get<BraceStmt*>() == scope);
      Scopes.pop_back();
    }

    void pushFunction(DeclContext *functionScope) {
      pushScope(functionScope);
      Functions.push_back(functionScope);
    }
    void popFunction(DeclContext *functionScope) {
      assert(Functions.back() == functionScope);
      Functions.pop_back();
      popScope(functionScope);
    }

#define FUNCTION_LIKE(NODE)                                     \
    bool shouldVerify(NODE *fn) {                               \
      pushFunction(fn);                                         \
      return shouldVerify(cast<ASTNodeBase<NODE*>::BaseTy>(fn));\
    }                                                           \
    void cleanup(NODE *fn) {                                    \
      popFunction(fn);                                          \
    }
#define TYPE_LIKE(NODE)                                         \
    bool shouldVerify(NODE *dc) {                               \
      pushScope(dc);                                            \
      if (dc->hasLazyMembers())                                 \
        return false;                                           \
      if (dc->hasUnparsedMembers())                             \
        return false;                                           \
      return shouldVerify(cast<ASTNodeBase<NODE*>::BaseTy>(dc));\
    }                                                           \
    void cleanup(NODE *dc) {                                    \
      popScope(dc);                                             \
    }

    FUNCTION_LIKE(AbstractClosureExpr)
    FUNCTION_LIKE(ConstructorDecl)
    FUNCTION_LIKE(DestructorDecl)
    FUNCTION_LIKE(FuncDecl)
    FUNCTION_LIKE(EnumElementDecl)
    FUNCTION_LIKE(SubscriptDecl)
    FUNCTION_LIKE(MacroDecl)
    TYPE_LIKE(NominalTypeDecl)
    TYPE_LIKE(ExtensionDecl)

#undef TYPE_LIKE
#undef FUNCTION_LIKE

    bool shouldVerify(BraceStmt *BS) {
      pushScope(BS);
      InImplicitBraceStmt.push_back(BS->isImplicit());
      return shouldVerify(cast<Stmt>(BS));
    }

    void cleanup(BraceStmt *BS) {
      InImplicitBraceStmt.pop_back();
      popScope(BS);
    }

    bool shouldVerify(ForEachStmt *S) {
      if (!shouldVerify(cast<Stmt>(S)))
        return false;

      if (!S->getElementExpr())
        return true;

      assert(!OpaqueValues.count(S->getElementExpr()));
      OpaqueValues[S->getElementExpr()] = 0;
      return true;
    }

    void cleanup(ForEachStmt *S) {
      if (!S->getElementExpr())
        return;

      assert(OpaqueValues.count(S->getElementExpr()));
      OpaqueValues.erase(S->getElementExpr());
    }

    bool shouldVerify(InterpolatedStringLiteralExpr *expr) {
      if (!shouldVerify(cast<Expr>(expr)))
        return false;

      if (!expr->getInterpolationExpr())
        return true;

      assert(!OpaqueValues.count(expr->getInterpolationExpr()));
      OpaqueValues[expr->getInterpolationExpr()] = 0;
      return true;
    }

    void cleanup(InterpolatedStringLiteralExpr *expr) {
      if (!expr->getInterpolationExpr())
        return;

      assert(OpaqueValues.count(expr->getInterpolationExpr()));
      OpaqueValues.erase(expr->getInterpolationExpr());
    }

    bool shouldVerify(OpenExistentialExpr *expr) {
      if (!shouldVerify(cast<Expr>(expr)))
        return false;

      // In rare instances we clear the opaque value because we no
      // longer have a subexpression that references it.
      if (!expr->getOpaqueValue())
        return true;

      assert(!OpaqueValues.count(expr->getOpaqueValue()));
      OpaqueValues[expr->getOpaqueValue()] = 0;
      assert(OpenedExistentialArchetypes.count(expr->getOpenedArchetype())==0);
      OpenedExistentialArchetypes.insert(expr->getOpenedArchetype());
      return true;
    }

    void cleanup(OpenExistentialExpr *expr) {
      // In rare instances we clear the opaque value because we no
      // longer have a subexpression that references it.
      if (!expr->getOpaqueValue())
        return;

      assert(OpaqueValues.count(expr->getOpaqueValue()));
      OpaqueValues.erase(expr->getOpaqueValue());
      assert(OpenedExistentialArchetypes.count(expr->getOpenedArchetype())==1);
      OpenedExistentialArchetypes.erase(expr->getOpenedArchetype());
    }

    bool shouldVerify(PackExpansionExpr *expr) {
      if (!shouldVerify(cast<Expr>(expr)))
        return false;

      Generics.push_back(expr->getGenericEnvironment());
      return true;
    }

    void cleanup(PackExpansionExpr *E) {
      assert(Generics.back().get<GenericEnvironment *>() ==
             E->getGenericEnvironment());
      Generics.pop_back();
    }

    bool shouldVerify(MakeTemporarilyEscapableExpr *expr) {
      if (!shouldVerify(cast<Expr>(expr)))
        return false;
      
      assert(!OpaqueValues.count(expr->getOpaqueValue()));
      OpaqueValues[expr->getOpaqueValue()] = 0;
      return true;
    }
    
    void cleanup(MakeTemporarilyEscapableExpr *expr) {
      assert(OpaqueValues.count(expr->getOpaqueValue()));
      OpaqueValues.erase(expr->getOpaqueValue());
    }

    // Register the OVEs in a DestructureTupleExpr.
    bool shouldVerify(DestructureTupleExpr *expr) {
      if (!shouldVerify(cast<Expr>(expr)))
        return false;

      for (auto *opaqueElt : expr->getDestructuredElements()) {
        assert(!OpaqueValues.count(opaqueElt));
        OpaqueValues[opaqueElt] = 0;
      }

      return true;
    }

    void cleanup(DestructureTupleExpr *expr) {
      for (auto *opaqueElt : expr->getDestructuredElements()) {
        assert(OpaqueValues.count(opaqueElt));
        OpaqueValues.erase(opaqueElt);
      }
    }

    // Keep a stack of the currently-live optional evaluations.
    bool shouldVerify(OptionalEvaluationExpr *expr) {
      if (!shouldVerify(cast<Expr>(expr)))
        return false;

      OptionalEvaluations.push_back(expr);
      return true;
    }
    void cleanup(OptionalEvaluationExpr *expr) {
      assert(OptionalEvaluations.back() == expr);
      OptionalEvaluations.pop_back();
    }

    // Register the OVEs in a collection upcast.
    bool shouldVerify(CollectionUpcastConversionExpr *expr) {
      if (!shouldVerify(cast<Expr>(expr)))
        return false;

      if (auto keyConversion = expr->getKeyConversion())
        OpaqueValues[keyConversion.OrigValue] = 0;
      if (auto valueConversion = expr->getValueConversion())
        OpaqueValues[valueConversion.OrigValue] = 0;
      return true;
    }
    void cleanup(CollectionUpcastConversionExpr *expr) {
      if (auto keyConversion = expr->getKeyConversion())
        OpaqueValues.erase(keyConversion.OrigValue);
      if (auto valueConversion = expr->getValueConversion())
        OpaqueValues.erase(valueConversion.OrigValue);
    }

    /// Canonicalize the given DeclContext pointer, in terms of
    /// producing something that can be looked up in
    /// ClosureDiscriminators.
    DeclContext *getCanonicalDeclContext(DeclContext *DC) {
      // All we really need to do is use a single TopLevelCodeDecl.
      if (auto topLevel = dyn_cast<TopLevelCodeDecl>(DC)) {
        if (!CanonicalTopLevelSubcontext)
          CanonicalTopLevelSubcontext = topLevel;
        return CanonicalTopLevelSubcontext;
      }

      // TODO: check for uniqueness of initializer contexts?

      return DC;
    }

    /// Return the appropriate discriminator set for a closure expression.
    SmallBitVector &getClosureDiscriminators(AbstractClosureExpr *closure) {
      auto dc = getCanonicalDeclContext(closure->getParent());
      bool isAutoClosure = isa<AutoClosureExpr>(closure);
      return ClosureDiscriminators[ClosureDiscriminatorKey(dc, isAutoClosure)];
    }

    void verifyCheckedAlways(ValueDecl *D) {
      if (D->hasInterfaceType())
        verifyChecked(D->getInterfaceType());

      if (D->hasAccess()) {
        PrettyStackTraceDecl debugStack("verifying access", D);
        if (!D->getASTContext().isAccessControlDisabled()) {
          if (D->getFormalAccessScope().isPublic() &&
              D->getFormalAccess() < AccessLevel::Public) {
            Out << "non-public decl has no formal access scope\n";
            D->dump(Out);
            abort();
          }
        }
        if (D->getEffectiveAccess() == AccessLevel::Private) {
          Out << "effective access should use 'fileprivate' for 'private'\n";
          D->dump(Out);
          abort();
        }
      }

      if (auto Overridden = getOverriddenDeclIfAvailable(D)) {
        if (D->getDeclContext() == Overridden->getDeclContext()) {
          PrettyStackTraceDecl debugStack("verifying overridden", D);
          Out << "cannot override a decl in the same DeclContext";
          D->dump(Out);
          Overridden->dump(Out);
          abort();
        }

        if (!isa<ClassDecl>(D->getDeclContext()) &&
            !isa<ProtocolDecl>(D->getDeclContext()) &&
            !isa<ExtensionDecl>(D->getDeclContext())) {
          PrettyStackTraceDecl debugStack("verifying override", D);
          Out << "'override' attribute outside of a class or protocol\n";
          D->dump(Out);
          abort();
        }
      }

      verifyCheckedAlwaysBase(D);
    }

    void verifyCheckedAlways(NominalTypeDecl *D) {
      verifyCheckedAlwaysBase(D);
    }

    bool shouldVerifyChecked(ThrowStmt *S) {
      return shouldVerifyChecked(S->getSubExpr());
    }

    void verifyChecked(ThrowStmt *S) {
      checkSameType(S->getSubExpr()->getType(),
                    checkExceptionTypeExists("throw expression"),
                    "throw operand");
      verifyCheckedBase(S);
    }

    bool shouldVerifyChecked(ReturnStmt *S) {
      return !S->hasResult() || shouldVerifyChecked(S->getResult());
    }

    void verifyChecked(ReturnStmt *S) {
      auto func = Functions.back();
      Type resultType;
      if (auto *FD = dyn_cast<FuncDecl>(func)) {
        resultType = FD->getResultInterfaceType();
        resultType = FD->mapTypeIntoContext(resultType);
      } else if (auto closure = dyn_cast<AbstractClosureExpr>(func)) {
        resultType = closure->getResultType();
      } else {
        resultType = TupleType::getEmpty(Ctx);
      }
      
      if (S->hasResult()) {
        if (isa<ConstructorDecl>(func)) {
          Out << "Expected ReturnStmt not to have a result. A constructor "
                 "should not return a result. Returned expression: ";
          S->getResult()->dump(Out);
          Out << "\n";
          abort();
        }

        auto result = S->getResult();
        auto returnType = result->getType();
        // Make sure that the return has the same type as the function.
        checkSameType(resultType, returnType, "return type");
      } else {
        // Make sure that the function has a Void result type.
        checkSameType(resultType, TupleType::getEmpty(Ctx), "return type");
      }

      verifyCheckedBase(S);
    }
    void verifyChecked(DeferStmt *S) {
      auto FT = S->getTempDecl()->getInterfaceType()->castTo<AnyFunctionType>();
      assert(FT->isNoEscape() && "Defer statements must not escape");
      (void)FT;
      verifyCheckedBase(S);
    }

    void verifyChecked(FailStmt *S) {
      // Dig out the initializer we're in (if we are).
      ConstructorDecl *ctor = nullptr;
      if (!Functions.empty()) {
        ctor = dyn_cast<ConstructorDecl>(Functions.back());
      }

      // Fail statements are only permitted in initializers.
      if (!ctor) {
        Out << "'fail' statement outside of initializer\n";
        abort();
      }

      if (!ctor->isFailable() && !ctor->isInvalid()) {
        Out << "non-failable initializer contains a 'fail' statement\n";
        ctor->dump(Out);
        abort();
      }
    }

    void checkConditionElement(const StmtConditionElement &elt) {
      switch (elt.getKind()) {
      case StmtConditionElement::CK_Availability:
      case StmtConditionElement::CK_HasSymbol:
        break;
      case StmtConditionElement::CK_Boolean: {
        auto *E = elt.getBoolean();
        if (shouldVerifyChecked(E))
          checkSameType(E->getType(), Ctx.getBoolType(), "condition type");
        break;
      }

      case StmtConditionElement::CK_PatternBinding:
        if (shouldVerifyChecked(elt.getPattern()) &&
            shouldVerifyChecked(elt.getInitializer())) {
          checkSameType(elt.getPattern()->getType(),
                    elt.getInitializer()->getType(),
                    "conditional binding type");
        }
        break;
      }
    }
    
    void checkCondition(StmtCondition C) {
      for (auto elt : C)
        checkConditionElement(elt);
    }
    
    void verifyChecked(IfStmt *S) {
      checkCondition(S->getCond());
      verifyCheckedBase(S);
    }

    void verifyChecked(GuardStmt *S) {
      checkCondition(S->getCond());
      verifyCheckedBase(S);
    }

    void verifyChecked(WhileStmt *S) {
      checkCondition(S->getCond());
      verifyCheckedBase(S);
    }

    Type checkAssignDest(Expr *Dest) {
      if (auto *TE = dyn_cast<TupleExpr>(Dest)) {
        SmallVector<TupleTypeElt, 4> lhsTupleTypes;
        for (unsigned i = 0; i != TE->getNumElements(); ++i) {
          Type SubType = checkAssignDest(TE->getElement(i));
          lhsTupleTypes.push_back(TupleTypeElt(SubType, TE->getElementName(i)));
        }
        return TupleType::get(lhsTupleTypes, Ctx);
      }
      return checkLValue(Dest->getType(), "LHS of assignment");
    }

    void verifyChecked(DeclRefExpr *E) {
      if (E->getType()->is<InOutType>()) {
        PrettyStackTraceExpr debugStack(Ctx, "verifying decl reference", E);
        Out << "reference with inout type "
          << E->getType().getString() << "\n";
        E->dump(Out);
        Out << "\n";
        abort();
      }
      if (E->getType()->is<GenericFunctionType>()) {
        PrettyStackTraceExpr debugStack(Ctx, "verifying decl reference", E);
        Out << "unspecialized reference with polymorphic type "
          << E->getType().getString() << "\n";
        E->dump(Out);
        Out << "\n";
        abort();
      }
      verifyCheckedBase(E);
    }

    void verifyChecked(AssignExpr *S) {
      Type lhsTy = checkAssignDest(S->getDest());
      checkSameType(lhsTy, S->getSrc()->getType(), "assignment operands");
      verifyCheckedBase(S);
    }
    
    void verifyChecked(EnumIsCaseExpr *E) {
      auto nom = E->getSubExpr()->getType()->getAnyNominal();
      if (!nom || !isa<EnumDecl>(nom)) {
        Out << "enum_is_decl operand is not an enum: ";
        E->getSubExpr()->getType().print(Out);
        Out << '\n';
        abort();
      }
      
      if (nom != E->getEnumElement()->getParentEnum()) {
        Out << "enum_is_decl case is not member of enum:\n";
        Out << "  case: ";
        E->getEnumElement()->print(Out);
        Out << "\n  type: ";
        E->getSubExpr()->getType().print(Out);
        Out << '\n';
        abort();
      }
    }

    void verifyChecked(TupleExpr *E) {
      PrettyStackTraceExpr debugStack(Ctx, "verifying TupleExpr", E);
      const TupleType *exprTy = E->getType()->castTo<TupleType>();
      for_each(exprTy->getElements().begin(), exprTy->getElements().end(),
               E->getElements().begin(),
               [this](const TupleTypeElt &field, const Expr *elt) {
        if (!field.getType()->isEqual(elt->getType())) {
          Out << "tuple_expr element type mismatch:\n";
          Out << "  field: ";
          Out << field.getType() << "\n";
          Out << "  element: ";
          Out << elt->getType() << "\n";
          abort();
        }
      });
      verifyCheckedBase(E);
    }

    void verifyChecked(InOutExpr *E) {
      Type srcObj = checkLValue(E->getSubExpr()->getType(),
                                "result of InOutExpr");
      auto DestTy = E->getType()->castTo<InOutType>()->getObjectType();
      
      checkSameType(DestTy, srcObj, "object types for InOutExpr");
      verifyCheckedBase(E);
    }

    void verifyChecked(SingleValueStmtExpr *E) {
      using Kind = IsSingleValueStmtResult::Kind;
      switch (E->getStmt()->mayProduceSingleValue(Ctx).getKind()) {
      case Kind::NoExpressionBranches:
        // These are allowed as long as the type is Void.
        checkSameType(
            E->getType(), Ctx.getVoidType(),
            "SingleValueStmtExpr with no expression branches must be Void");
        break;
      case Kind::UnterminatedBranches:
      case Kind::NonExhaustiveIf:
      case Kind::UnhandledStmt:
      case Kind::CircularReference:
      case Kind::HasLabel:
      case Kind::InvalidJumps:
        // These should have been diagnosed.
        Out << "invalid SingleValueStmtExpr should be been diagnosed:\n";
        E->dump(Out);
        Out << "\n";
        abort();
      case Kind::Valid:
        break;
      }
    }

    void verifyParsed(AbstractClosureExpr *E) {
      Type Ty = E->getType();
      if (!Ty)
        return;
      if (Ty->hasError())
        return;
      if (!Ty->is<FunctionType>()) {
        PrettyStackTraceExpr debugStack(Ctx, "verifying closure", E);
        Out << "a closure should have a function type";
        E->dump(Out);
        Out << "\n";
        abort();
      }
      verifyParsedBase(E);
    }

    void verifyChecked(AbstractClosureExpr *E) {
      PrettyStackTraceExpr debugStack(Ctx, "verifying closure", E);

      assert(Scopes.back().get<DeclContext*>() == E);
      assert(E->getParent()->isLocalContext() &&
             "closure expression was not in local context!");

      // Check that the discriminator is unique in its context.
      auto &discriminatorSet = getClosureDiscriminators(E);
      unsigned discriminator = E->getDiscriminator();
      if (discriminator >= discriminatorSet.size()) {
        discriminatorSet.resize(discriminator+1);
        discriminatorSet.set(discriminator);
      } else if (discriminatorSet.test(discriminator)) {
        Out << "a closure must have a unique discriminator in its context\n";
        E->dump(Out);
        Out << "\n";
        abort();
      } else {
        discriminatorSet.set(discriminator);
      }

      // If the enclosing scope is a DC directly, rather than a local scope,
      // then the closure should be parented by an Initializer.  Otherwise,
      // it should be parented by the innermost function.
      auto enclosingScope = Scopes[Scopes.size() - 2];
      auto enclosingDC = enclosingScope.dyn_cast<DeclContext*>();
      if (enclosingDC && !isa<AbstractClosureExpr>(enclosingDC)){
        auto parentDC = E->getParent();
        if (!isa<Initializer>(parentDC)) {
          Out << "a closure in non-local context should be parented "
                 "by an initializer or REPL context";
          E->dump(Out);
          Out << "\n";
          abort();
        } else if (parentDC->getParent() != enclosingDC) {
          Out << "closure in non-local context not grandparented by its "
                 "enclosing function";
          E->dump(Out);
          Out << "\n";
          abort();
        }
      } else if (Functions.size() >= 2 &&
                 Functions[Functions.size() - 2] != E->getParent()) {
        Out << "closure in local context not parented by its "
               "enclosing function";
        E->dump(Out);
        Out << "\n";
        abort();
      }

      if (E->getDiscriminator() == AbstractClosureExpr::InvalidDiscriminator) {
        Out << "a closure expression should have a valid discriminator\n";
        E->dump(Out);
        Out << "\n";
        abort();
      }
    }

    void verifyChecked(MetatypeConversionExpr *E) {
      PrettyStackTraceExpr debugStack(Ctx, "verifying MetatypeConversion", E);

      auto destTy = checkMetatypeType(E->getType(),
                                      "result of MetatypeConversionExpr");
      auto srcTy = checkMetatypeType(E->getSubExpr()->getType(),
                                     "source of MetatypeConversionExpr");

      if (destTy->isEqual(srcTy)) {
        Out << "trivial MetatypeConversionExpr:\n";
        E->dump(Out);
        Out << "\n";
        abort();
      }

      checkTrivialSubtype(srcTy, destTy, "MetatypeConversionExpr");
      verifyCheckedBase(E);
    }
    
    void verifyChecked(ClassMetatypeToObjectExpr *E) {
      PrettyStackTraceExpr debugStack(Ctx, "verifying ClassMetatypeToObject", E);
      
      auto srcTy = checkMetatypeType(E->getSubExpr()->getType(),
                                     "source of ClassMetatypeToObject");
      
      if (!srcTy->mayHaveSuperclass()) {
        Out << "ClassMetatypeToObject with non-class metatype:\n";
        E->dump(Out);
        Out << "\n";
        abort();
      }
      
      if (!E->getType()->isEqual(Ctx.getAnyObjectType())) {
        Out << "ClassMetatypeToObject does not produce AnyObject:\n";
        E->dump(Out);
        Out << "\n";
        abort();
      }
    }
    
    void verifyChecked(ExistentialMetatypeToObjectExpr *E) {
      PrettyStackTraceExpr debugStack(Ctx,
                                    "verifying ExistentialMetatypeToObject", E);
      
      auto srcTy = checkMetatypeType(E->getSubExpr()->getType(),
                                     "source of ExistentialMetatypeToObject");
      
      if (!E->getSubExpr()->getType()->is<ExistentialMetatypeType>()) {
        Out << "ExistentialMetatypeToObject with non-existential "
               "metatype:\n";
        E->dump(Out);
        Out << "\n";
        abort();
      }
      if (!srcTy->isClassExistentialType()) {
        Out << "ExistentialMetatypeToObject with non-class existential "
               "metatype:\n";
        E->dump(Out);
        Out << "\n";
        abort();
      }
      
      if (!E->getType()->isEqual(Ctx.getAnyObjectType())) {
        Out << "ExistentialMetatypeToObject does not produce AnyObject:\n";
        E->dump(Out);
        Out << "\n";
        abort();
      }
    }
    
    void verifyChecked(ProtocolMetatypeToObjectExpr *E) {
      PrettyStackTraceExpr debugStack(Ctx,
                                    "verifying ProtocolMetatypeToObject", E);
      
      auto srcTy = checkMetatypeType(E->getSubExpr()->getType(),
                                     "source of ProtocolMetatypeToObject");
      if (E->getSubExpr()->getType()->is<ExistentialMetatypeType>()) {
        Out << "ProtocolMetatypeToObject with existential "
               "metatype:\n";
        E->dump(Out);
        Out << "\n";
        abort();
      }

      if (!srcTy->isExistentialType()) {
        Out << "ProtocolMetatypeToObject with non-existential metatype:\n";
        E->dump(Out);
        Out << "\n";
        abort();
      }

      auto layout = srcTy->getExistentialLayout();
      if (layout.explicitSuperclass ||
          !layout.isObjC() ||
          layout.getProtocols().size() != 1) {
        Out << "ProtocolMetatypeToObject with non-ObjC-protocol metatype:\n";
        E->dump(Out);
        Out << "\n";
        abort();
      }

      if (!E->getType()->getClassOrBoundGenericClass()) {
        Out << "ProtocolMetatypeToObject does not produce class:\n";
        E->dump(Out);
        Out << "\n";
        abort();
      }
    }
    
    void verifyChecked(PointerToPointerExpr *E) {
      PrettyStackTraceExpr debugStack(Ctx,
                                      "verifying PointerToPointer", E);

      auto fromElement = E->getSubExpr()->getType()->getAnyPointerElementType();
      auto toElement = E->getType()->getAnyPointerElementType();
      
      if (!fromElement || !toElement) {
        Out << "PointerToPointer does not convert between pointer types:\n";
        E->dump(Out);
        Out << "\n";
        abort();
      }
    }
    
    void verifyChecked(InOutToPointerExpr *E) {
      PrettyStackTraceExpr debugStack(Ctx,
                                      "verifying InOutToPointer", E);

      if (!ValidInOutToPointerExprs.count(E)) {
        Out << "InOutToPointerExpr in unexpected position!\n";
        E->dump(Out);
        Out << "\n";
        abort();
      }

      auto fromElement = E->getSubExpr()->getType()->getInOutObjectType();
      auto toElement = E->getType()->getAnyPointerElementType();
      
      if (!E->getSubExpr()->getType()->is<InOutType>() && !toElement) {
        Out << "InOutToPointer does not convert from inout to pointer:\n";
        E->dump(Out);
        Out << "\n";
        abort();
      }
      
      // Ensure we don't convert an array to a void pointer this way.
      
      if (fromElement->isArray() && toElement->isVoid()) {
        Out << "InOutToPointer is converting an array to a void pointer; "
               "ArrayToPointer should be used instead:\n";
        E->dump(Out);
        Out << "\n";
        abort();
      }
    }
    
    void verifyChecked(ArrayToPointerExpr *E) {
      PrettyStackTraceExpr debugStack(Ctx,
                                      "verifying ArrayToPointer", E);

      if (!ValidArrayToPointerExprs.count(E)) {
        Out << "ArrayToPointer in invalid position?!\n";
        E->dump(Out);
        Out << "\n";
        abort();
      }

      // The source may be optionally inout.
      auto fromArray = E->getSubExpr()->getType()->getInOutObjectType();
      
      if (!fromArray->isArray()) {
        Out << "ArrayToPointer does not convert from array:\n";
        E->dump(Out);
        Out << "\n";
        abort();
      }
      
      auto toElement = E->getType()->getAnyPointerElementType();

      if (!toElement) {
        Out << "ArrayToPointer does not convert to pointer:\n";
        E->dump(Out);
        Out << "\n";
        abort();
      }
    }
    
    void verifyChecked(StringToPointerExpr *E) {
      PrettyStackTraceExpr debugStack(Ctx,
                                      "verifying StringToPointer", E);
      
      if (!E->getSubExpr()->getType()->isString()) {
        Out << "StringToPointer does not convert from string:\n";
        E->dump(Out);
        Out << "\n";
        abort();
      }
      
      PointerTypeKind PTK;
      auto toElement = E->getType()->getAnyPointerElementType(PTK);
      if (!toElement) {
        Out << "StringToPointer does not convert to pointer:\n";
        E->dump(Out);
        Out << "\n";
        abort();
      }
      if (PTK != PTK_UnsafePointer && PTK != PTK_UnsafeRawPointer) {
        Out << "StringToPointer converts to non-const pointer:\n";
        E->dump(Out);
        Out << "\n";
        abort();
      }
    }
    
    void verifyChecked(CollectionUpcastConversionExpr *E) {
      verifyChecked(E->getSubExpr());
      verifyCheckedBase(E);
    }
        
    void verifyChecked(DerivedToBaseExpr *E) {
      PrettyStackTraceExpr debugStack(Ctx, "verifying DerivedToBaseExpr", E);

      auto destTy = E->getType();
      auto srcTy = E->getSubExpr()->getType();
      if (destTy->isEqual(srcTy)) {
        Out << "trivial DerivedToBaseExpr:\n";
        E->dump(Out);
        Out << "\n";
        abort();
      }

      if (!destTy->getClassOrBoundGenericClass() ||
          !(srcTy->getClassOrBoundGenericClass() ||
            srcTy->is<DynamicSelfType>())) {
        Out << "DerivedToBaseExpr does not involve class types:\n";
        E->dump(Out);
        Out << "\n";
        abort();
      }

      checkTrivialSubtype(srcTy, destTy, "DerivedToBaseExpr");
      verifyCheckedBase(E);
    }

    bool shouldVerify(ErasureExpr *expr) {
      if (!shouldVerify(cast<Expr>(expr)))
        return false;

      for (auto &elt : expr->getArgumentConversions()) {
        assert(!OpaqueValues.count(elt.OrigValue));
        OpaqueValues[elt.OrigValue] = 0;
      }

      return true;
    }

    void cleanup(ErasureExpr *expr) {
      for (auto &elt : expr->getArgumentConversions()) {
        assert(OpaqueValues.count(elt.OrigValue));
        OpaqueValues.erase(elt.OrigValue);
      }
    }

    void verifyChecked(ErasureExpr *E) {
      PrettyStackTraceExpr debugStack(Ctx, "verifying ErasureExpr", E);

      if (!E->getType()->isAnyExistentialType()) {
        Out << "ErasureExpr result is not an existential: ";
        E->getType()->print(Out);
        Out << "\n";
        abort();
      }

      auto erasedTy = E->getType();
      auto concreteTy = E->getSubExpr()->getType();
      
      // Erasure can be from concrete to existential or from existential to more
      // general existential. If we look through metatypes then we're forced
      // into one or the other by context; otherwise, it doesn't matter.
      enum {
        AnyErasure,
        ConcreteErasureOnly,
        ExistentialErasureOnly,
      } knownConcreteErasure = AnyErasure;
      
      // Existential metatypes should be erased from (existential or concrete)
      // metatypes.
      while (auto meta = erasedTy->getAs<ExistentialMetatypeType>()) {
        erasedTy = meta->getInstanceType();
        if (auto concreteMeta = concreteTy->getAs<MetatypeType>()) {
          // If this is already forced to be an existential erasure, we
          // shouldn't be here, since (P & Q).Protocol.Type doesn't exist as
          // a type.
          assert(knownConcreteErasure != ExistentialErasureOnly);
          knownConcreteErasure = ConcreteErasureOnly;
          concreteTy = concreteMeta->getInstanceType();
        } else if (concreteTy->is<ExistentialMetatypeType>()) {
          // If this is already forced to be a concrete erasure (say we're going
          // from (P & Q).Type.Protocol to P.Type.Type), then this is invalid,
          // because it would require the existential metatype to be
          // "self-conforming" to the protocol's static requirements (in other
          // words, (P & Q).Type.self would have to be able to witness all of P's
          // static requirements), which is currently never the case.
          if (knownConcreteErasure == true) {
            Out << "ErasureExpr concrete metatype is not a subtype of the "
                   "existential metatype\n"
                   "Destination type: ";
            E->getType()->print(Out);
            Out << "\nSource type: ";
            E->getSubExpr()->getType()->print(Out);
            Out << "\n";
            abort();
          }
          knownConcreteErasure = ExistentialErasureOnly;
          concreteTy = concreteMeta->getInstanceType();
        } else {
          // Anything else wouldn't be a valid erasure to an existential
          // metatype.
          Out << "ErasureExpr from non-metatype to existential metatype\n"
                 "Destination type: ";
          E->getType()->print(Out);
          Out << "\nSource type: ";
          E->getSubExpr()->getType()->print(Out);
          Out << "\n";
          abort();
        }
      }

      auto erasedLayout = erasedTy->getCanonicalType()->getExistentialLayout();

      // An existential-to-existential erasure ought to reduce the set of
      // constraints.
      if (knownConcreteErasure != ConcreteErasureOnly
          && concreteTy->isExistentialType()) {
        // TODO
      } else {
        // Check class constraints.
        if (erasedLayout.requiresClass()) {
          // A class constraint can be satisfied by a class, class-constrained
          // archetype, or a class-constrained existential with no witness table
          // requirements.
          bool canBeClass;
          if (concreteTy->mayHaveSuperclass()) {
            canBeClass = true;
          } else if (concreteTy->isExistentialType()) {
            auto concreteLayout = concreteTy->getCanonicalType()
                                            ->getExistentialLayout();
            canBeClass = concreteLayout.getKind() == ExistentialLayout::Kind::Class
              && !concreteLayout.containsNonObjCProtocol;
          } else {
            canBeClass = false;
          }
          
          if (!canBeClass) {
            Out << "ErasureExpr from non-class to existential that requires a "
                   "class\n"
                   "Destination type: ";
            E->getType()->print(Out);
            Out << "\nSource type: ";
            E->getSubExpr()->getType()->print(Out);
            Out << "\n";
            abort();
          }
        }
        
        auto superclass = erasedLayout.getSuperclass();
        if (superclass
            && !superclass->isExactSuperclassOf(concreteTy)) {
          Out << "ErasureExpr from class to existential with a superclass "
                 "constraint that does not match the class\n"
                 "Destination type: ";
          E->getType()->print(Out);
          Out << "\nSource type: ";
          E->getSubExpr()->getType()->print(Out);
          Out << "\n";
          abort();
        }
        
        // A concrete-to-existential erasure should have conformances on hand
        // for all of the existential's requirements.
        auto conformances = E->getConformances();
        for (auto proto : erasedLayout.getProtocols()) {
          if (std::find_if(conformances.begin(), conformances.end(),
                           [&](ProtocolConformanceRef ref) -> bool {
                             return ref.getRequirement() == proto->getDecl();
                           })
              == conformances.end()) {
            Out << "ErasureExpr is missing conformance for required protocol\n";
            E->getType()->print(Out);
            Out << "\nSource type: ";
            E->getSubExpr()->getType()->print(Out);
            Out << "\n";
            abort();
          }
          // TODO: Verify that the conformance applies to the type?
        }
        
        // TODO: Check layout constraints?
      }
    }

    void verifyChecked(AnyHashableErasureExpr *E) {
      auto anyHashableDecl = Ctx.getAnyHashableDecl();
      if (!anyHashableDecl) {
        Out << "AnyHashable declaration could not be found\n";
        abort();
      }

      auto hashableDecl = Ctx.getProtocol(KnownProtocolKind::Hashable);
      if (!hashableDecl) {
        Out << "Hashable declaration could not be found\n";
        abort();
      }

      checkSameType(E->getType(),
                    anyHashableDecl->getDeclaredInterfaceType(),
                    "AnyHashableErasureExpr and the standard AnyHashable type");

      if (E->getConformance().getRequirement() != hashableDecl) {
        Out << "conformance on AnyHashableErasureExpr was not for Hashable\n";
        E->getConformance().dump(Out);
        abort();
      }

      verifyConformance(E->getSubExpr()->getType(), E->getConformance());

      verifyCheckedBase(E);
    }

    void verifyChecked(TupleElementExpr *E) {
      PrettyStackTraceExpr debugStack(Ctx, "verifying TupleElementExpr", E);

      Type resultType = E->getType();
      Type baseType = E->getBase()->getType();
      checkSameLValueness(baseType, resultType,
                          "base and result of TupleElementExpr");

      TupleType *tupleType = baseType->getAs<TupleType>();
      if (!tupleType) {
        Out << "base of TupleElementExpr does not have tuple type: ";
        E->getBase()->getType().print(Out);
        Out << "\n";
        abort();
      }

      if (E->getFieldNumber() >= tupleType->getNumElements()) {
        Out << "field index " << E->getFieldNumber()
            << " for TupleElementExpr is out of range [0,"
            << tupleType->getNumElements() << ")\n";
        abort();
      }

      checkSameType(resultType, tupleType->getElementType(E->getFieldNumber()),
                    "TupleElementExpr and the corresponding tuple element");
      verifyCheckedBase(E);
    }

    void maybeRecordValidPointerConversionForArg(Expr *argExpr) {
      auto *subExpr = argExpr;
      unsigned optionalDepth = 0;

      // FIXME: This doesn't seem like a particularly robust
      //        approach to tracking whether pointer conversions
      //        always appear as call arguments.
      while (true) {
        // Look through optional evaluations.
        if (auto *optionalEval = dyn_cast<OptionalEvaluationExpr>(subExpr)) {
          subExpr = optionalEval->getSubExpr();
          ++optionalDepth;
          continue;
        }

        // Look through injections into Optional<Pointer>.
        if (auto *injectIntoOpt = dyn_cast<InjectIntoOptionalExpr>(subExpr)) {
          subExpr = injectIntoOpt->getSubExpr();
          continue;
        }

        // FIXME: This is only handling the value conversion, not
        //        the key conversion. What this verifier check
        //        should probably do is just track whether we're
        //        currently visiting arguments of an apply when we
        //        find these conversions.
        if (auto *upcast = dyn_cast<CollectionUpcastConversionExpr>(subExpr)) {
          subExpr = upcast->getValueConversion().Conversion;
          continue;
        }

        break;
      }

      auto checkIsBindOptional = [&](Expr *expr) {
        for (unsigned depth = optionalDepth; depth; --depth) {
          if (auto bind = dyn_cast<BindOptionalExpr>(expr)) {
            expr = bind->getSubExpr();
          } else {
            Out << "malformed optional pointer conversion\n";
            argExpr->dump(Out);
            Out << '\n';
            abort();
          }
        }
      };

      // Record inout-to-pointer conversions.
      if (auto *inOutToPtr = dyn_cast<InOutToPointerExpr>(subExpr)) {
        ValidInOutToPointerExprs.insert(inOutToPtr);
        checkIsBindOptional(inOutToPtr->getSubExpr());
        return;
      }

      // Record array-to-pointer conversions.
      if (auto *arrayToPtr = dyn_cast<ArrayToPointerExpr>(subExpr)) {
        ValidArrayToPointerExprs.insert(arrayToPtr);
        checkIsBindOptional(arrayToPtr->getSubExpr());
        return;
      }
    }

    void maybeRecordValidPointerConversion(ArgumentList *argList) {
      for (auto arg : *argList)
        maybeRecordValidPointerConversionForArg(arg.getExpr());
    }

    void verifyChecked(ApplyExpr *E) {
      PrettyStackTraceExpr debugStack(Ctx, "verifying ApplyExpr", E);

      FunctionType *FT = E->getFn()->getType()->getAs<FunctionType>();
      if (!FT) {
        Out << "callee of apply expression does not have function type:";
        E->getFn()->getType().print(Out);
        Out << "\n";
        abort();
      }
      Type ResultExprTy = E->getType();
      if (!ResultExprTy->isEqual(FT->getResult())) {
        Out << "result of ApplyExpr does not match result type of callee:";
        E->getType().print(Out);
        Out << " vs. ";
        FT->getResult()->print(Out);
        Out << "\n";
        abort();
      }

      if (!E->getArgs()->matches(FT->getParams())) {
        Out << "Argument list does not match parameters in ApplyExpr:"
               "\nArgument list: ";
        E->getArgs()->dump(Out);
        Out << "\nParameter types: ";
        AnyFunctionType::printParams(FT->getParams(), Out);
        Out << "\n";
        E->dump(Out);
        Out << "\n";
        abort();
      }

      if (!E->isThrowsSet()) {
        Out << "apply expression is not marked as throwing or non-throwing\n";
        E->dump(Out);
        Out << "\n";
        abort();
      } else if (E->throws() && !FT->isThrowing() && !E->implicitlyThrows()) {
        PolymorphicEffectKind rethrowingKind = PolymorphicEffectKind::Invalid;
        if (auto DRE = dyn_cast<DeclRefExpr>(E->getFn())) {
          if (auto fnDecl = dyn_cast<AbstractFunctionDecl>(DRE->getDecl())) {
            rethrowingKind = fnDecl->getPolymorphicEffectKind(EffectKind::Throws);
          }
        } else if (auto OCDRE = dyn_cast<OtherConstructorDeclRefExpr>(E->getFn())) {
          if (auto fnDecl = dyn_cast<AbstractFunctionDecl>(OCDRE->getDecl())) {
            rethrowingKind = fnDecl->getPolymorphicEffectKind(EffectKind::Throws);
          }
        }

        if (rethrowingKind != PolymorphicEffectKind::ByConformance &&
            rethrowingKind != PolymorphicEffectKind::Always) {
          Out << "apply expression is marked as throwing, but function operand"
                 "does not have a throwing function type\n";
          E->dump(Out);
          Out << "\n";
          abort();
        }
      }

      verifyCheckedBase(E);
    }

    void verifyChecked(MemberRefExpr *E) {
      PrettyStackTraceExpr debugStack(Ctx, "verifying MemberRefExpr", E);

      if (!E->getMember()) {
        Out << "Member reference is missing declaration\n";
        E->dump(Out);
        Out << "\n";
        abort();
      }

      if (!isa<VarDecl>(E->getMember().getDecl())) {
        Out << "Member reference to a non-VarDecl\n";
        E->dump(Out);
        Out << "\n";
        abort();
      }

      auto baseType = E->getBase()->getType();
      if (baseType->is<InOutType>()) {
        Out << "Member reference to an inout type\n";
        E->dump(Out);
        Out << "\n";
        abort();
      }

      // The base of a member reference cannot be an existential type.
      if (baseType->getWithoutSpecifierType()->isExistentialType()) {
        Out << "Member reference into an unopened existential type\n";
        E->dump(Out);
        Out << "\n";
        abort();
      }

      // FIXME: Check container/member types through substitutions.

      verifyCheckedBase(E);
    }

    void verifyChecked(DynamicMemberRefExpr *E) {
      PrettyStackTraceExpr debugStack(Ctx, "verifying DynamicMemberRefExpr", E);

      // The base of a dynamic member reference cannot be an
      // existential type.
      if (E->getBase()->getType()->getWithoutSpecifierType()
            ->isAnyExistentialType()) {
        Out << "Member reference into an unopened existential type\n";
        E->dump(Out);
        Out << "\n";
        abort();
      }

      verifyCheckedBase(E);
    }

    void verifyChecked(SubscriptExpr *E) {
      PrettyStackTraceExpr debugStack(Ctx, "verifying SubscriptExpr", E);

      if (!E->hasDecl()) {
        Out << "Subscript expression is missing subscript declaration";
        abort();
      }

      // The base of a subscript cannot be an existential type.
      if (E->getBase()->getType()->getWithoutSpecifierType()
            ->isAnyExistentialType()) {
        Out << "Member reference into an unopened existential type\n";
        E->dump(Out);
        Out << "\n";
        abort();
      }

      // FIXME: Check base/member types through substitutions.

      verifyCheckedBase(E);
    }

    void verifyChecked(DynamicSubscriptExpr *E) {
      PrettyStackTraceExpr debugStack(Ctx, "verifying DynamicSubscriptExpr", E);

      // The base of a subscript cannot be an existential type.
      if (E->getBase()->getType()->getWithoutSpecifierType()
            ->isAnyExistentialType()) {
        Out << "Member reference into an unopened existential type\n";
        E->dump(Out);
        Out << "\n";
        abort();
      }

      // FIXME: Check base/member types through substitutions.

      verifyCheckedBase(E);
    }
    
    void checkOptionalObjectType(Type optionalType,
                                 Type objectType,
                                 Expr *E) {
      auto optionalRVType = optionalType->getRValueType();
      auto objectRVType = objectType->getRValueType();

      checkSameType(objectRVType, optionalRVType->getOptionalObjectType(),
                    "optional object type");

      if (objectType->is<LValueType>() != optionalType->is<LValueType>()) {
        Out << "optional operation must preserve lvalue-ness of base\n";
        E->dump(Out);
        abort();
      }
    }

    void verifyChecked(OptionalEvaluationExpr *E) {
      if (E->getType()->hasLValueType()) {
        Out << "Optional evaluation should not produce an lvalue";
        E->dump(Out);
        abort();
      }
      checkSameType(E->getType(), E->getSubExpr()->getType(),
                    "OptionalEvaluation cannot change type");
    }
    
    void verifyChecked(BindOptionalExpr *E) {
      PrettyStackTraceExpr debugStack(Ctx, "verifying BindOptionalExpr", E);

      if (E->getDepth() >= OptionalEvaluations.size()) {
        Out << "BindOptional expression is out of its depth\n";
        E->dump(Out);
        abort();
      }
      
      checkOptionalObjectType(E->getSubExpr()->getType(),
                              E->getType(), E);
      
      verifyCheckedBase(E);
    }

    void verifyChecked(CheckedCastExpr *E) {
      PrettyStackTraceExpr debugStack(Ctx, "verifying CheckCastExpr", E);

      if (!E->isResolved()) {
        Out << "CheckedCast kind not resolved\n";
        abort();
      }

      verifyCheckedBase(E);
    }

    void verifyChecked(CoerceExpr *E) {
      PrettyStackTraceExpr debugStack(Ctx, "verifying CoerceExpr", E);

      checkSameType(E->getType(), E->getSubExpr()->getType(),
                    "coercion type and subexpression type");

      verifyCheckedBase(E);
    }

    void verifyChecked(IdentityExpr *E) {
      PrettyStackTraceExpr debugStack(Ctx, "verifying IdentityExpr", E);
      if (!E->getType()->isEqual(E->getSubExpr()->getType())) {
        Out << "Unexpected types in IdentityExpr\n";
        abort();
      }

      verifyCheckedBase(E);
    }

    void verifyChecked(ParenExpr *E) {
      PrettyStackTraceExpr debugStack(Ctx, "verifying ParenExpr", E);
      auto ty = dyn_cast<ParenType>(E->getType().getPointer());
      if (!ty) {
        Out << "ParenExpr not of ParenType\n";
        abort();
      }
      verifyCheckedBase(E);
    }

    void verifyChecked(AnyTryExpr *E) {
      PrettyStackTraceExpr debugStack(Ctx, "verifying AnyTryExpr", E);

      if (!isa<OptionalTryExpr>(E)) {
        checkSameType(E->getType(), E->getSubExpr()->getType(),
                      "AnyTryExpr and sub-expression");
      }

      verifyCheckedBase(E);
    }

    void verifyChecked(OptionalTryExpr *E) {
      PrettyStackTraceExpr debugStack(Ctx, "verifying OptionalTryExpr", E);

      if (Ctx.LangOpts.isSwiftVersionAtLeast(5)) {
        checkSameType(E->getType(), E->getSubExpr()->getType(),
                      "OptionalTryExpr and sub-expression");
      }
      else {
        Type unwrappedType = E->getType()->getOptionalObjectType();
        if (!unwrappedType) {
          Out << "OptionalTryExpr result type is not optional\n";
          abort();
        }
        
        checkSameType(unwrappedType, E->getSubExpr()->getType(),
                      "OptionalTryExpr and sub-expression");
      }
      
      verifyCheckedBase(E);
    }

    void verifyChecked(DestructureTupleExpr *E) {
      PrettyStackTraceExpr debugStack(Ctx, "verifying DestructureTupleExpr", E);

      auto getInputElementType = [&](unsigned i) {
        return (E->getSubExpr()->getType()->castTo<TupleType>()
                 ->getElementType(i));
      };

      auto getOpaqueElementType = [&](unsigned i) -> Type {
        return E->getDestructuredElements()[i]->getType();
      };

      for (unsigned i = 0, e = E->getDestructuredElements().size(); i != e; ++i) {
        Type inputType = getInputElementType(i);
        Type opaqueType = getOpaqueElementType(i);
        if (!inputType->isEqual(opaqueType)) {
          Out << "Input type mismatch in DestructureTupleExpr\n";
          inputType->dump(Out);
          opaqueType->dump(Out);
          abort();
        }
      }

      if (!E->getResultExpr()->getType()->isEqual(E->getType())) {
        Out << "Result type mismatch in DestructureTupleExpr\n";
        E->getResultExpr()->getType()->dump(Out);
        E->getType()->dump(Out);
      }

      verifyCheckedBase(E);
    }

    void verifyChecked(DynamicTypeExpr *E) {
      PrettyStackTraceExpr debugStack(Ctx, "verifying DynamicTypeExpr", E);

      auto metatype = E->getType()->getAs<AnyMetatypeType>();
      if (!metatype) {
        Out << "DynamicTypeExpr must have metatype type\n";
        abort();
      }

      auto instance = metatype->getInstanceType();
      if (auto existential = metatype->getAs<ExistentialMetatypeType>())
        instance = existential->getExistentialInstanceType();
      checkSameType(E->getBase()->getType(), instance,
                    "base type of .Type expression");
      verifyCheckedBase(E);
    }

    void verifyChecked(InjectIntoOptionalExpr *E) {
      PrettyStackTraceExpr debugStack(Ctx, "verifying InjectIntoOptionalExpr",
                                      E);

      auto valueType = E->getType()->getOptionalObjectType();
      if (!valueType) {
        Out << "InjectIntoOptionalExpr is not of Optional type";
        abort();
      }

      if (!E->getSubExpr()->getType()->isEqual(valueType)) {
        Out << "InjectIntoOptionalExpr operand is not of the value type";
        abort();
      }
      verifyCheckedBase(E);
    }

    void verifyChecked(TernaryExpr *E) {
      PrettyStackTraceExpr debugStack(Ctx, "verifying TernaryExpr", E);

      auto condTy = E->getCondExpr()->getType();
      if (!condTy->isBool()) {
        Out << "TernaryExpr condition is not Bool\n";
        abort();
      }

      checkSameType(E->getThenExpr()->getType(),
                    E->getElseExpr()->getType(),
                    "then and else branches of a TernaryExpr");
      verifyCheckedBase(E);
    }
    
    void verifyChecked(SuperRefExpr *expr) {
      verifyCheckedBase(expr);
    }

    void verifyChecked(TypeExpr *expr) {
      if (!expr->getType()->is<AnyMetatypeType>()) {
        Out << "TypeExpr must have metatype type\n";
        abort();
      }

      verifyCheckedBase(expr);
    }

    void verifyChecked(ForceValueExpr *E) {
      checkOptionalObjectType(E->getSubExpr()->getType(),
                              E->getType(), E);
      
      verifyCheckedBase(E);
    }

    void verifyChecked(OpaqueValueExpr *E) {
      PrettyStackTraceExpr debugStack(Ctx, "verifying OpaqueValueExpr", E);

      if (!OpaqueValues.count(E)) {
        Out << "OpaqueValueExpr not introduced at this point in AST\n";
        abort();
      }

      ++OpaqueValues[E];

      // Make sure opaque values are uniquely-referenced.
      if (OpaqueValues[E] > 1) {
        Out << "Multiple references to unique OpaqueValueExpr\n";
        abort();
      }
      verifyCheckedBase(E);
    }
    
    void verifyChecked(MakeTemporarilyEscapableExpr *E) {
      PrettyStackTraceExpr debugStack(
        Ctx, "verifying MakeTemporarilyEscapableExpr", E);
      
      // Expression type should match subexpression.
      if (!E->getType()->isEqual(E->getSubExpr()->getType())) {
        Out << "MakeTemporarilyEscapableExpr type does not match subexpression";
        abort();
      }

      auto call = dyn_cast<CallExpr>(E->getSubExpr());
      if (!call) {
        Out << "MakeTemporarilyEscapableExpr subexpression is not a call\n";
        abort();
      }

      auto callFnTy = call->getFn()->getType()->getAs<FunctionType>();
      if (!callFnTy) {
        Out << "MakeTemporarilyEscapableExpr call does not call function\n";
        abort();
      }
      if (!callFnTy->getExtInfo().isNoEscape()) {
        Out << "MakeTemporarilyEscapableExpr called function is not noescape\n";
        abort();
      }

      auto *unaryArg = call->getArgs()->getUnaryExpr();
      if (!unaryArg) {
        Out << "MakeTemporarilyEscapableExpr doesn't have a unary argument\n";
        abort();
      }

      if (!unaryArg->getType()->is<FunctionType>()) {
        Out << "MakeTemporarilyEscapableExpr call argument is not a function\n";
        abort();
      }

      // Closure and opaque value should both be functions, with the closure
      // noescape and the opaque value escapable but otherwise matching.
      auto closureFnTy =
          E->getNonescapingClosureValue()->getType()->getAs<FunctionType>();
      if (!closureFnTy) {
        Out << "MakeTemporarilyEscapableExpr closure type is not a closure\n";
        abort();
      }
      auto opaqueValueFnTy =
          E->getOpaqueValue()->getType()->getAs<FunctionType>();
      if (!opaqueValueFnTy) {
        Out << "MakeTemporarilyEscapableExpr opaque value type is not a "
               "closure\n";
        abort();
      }
      auto closureFnNoEscape =
          closureFnTy->withExtInfo(closureFnTy->getExtInfo().withNoEscape());
      auto opaqueValueNoEscape = opaqueValueFnTy->withExtInfo(
          opaqueValueFnTy->getExtInfo().withNoEscape());
      if (!closureFnNoEscape->isEqual(opaqueValueNoEscape)) {
        Out << "MakeTemporarilyEscapableExpr closure and opaque value type "
               "don't match\n";
        abort();
      }
    }
  
    void verifyChecked(KeyPathApplicationExpr *E) {
      PrettyStackTraceExpr debugStack(
        Ctx, "verifying KeyPathApplicationExpr", E);
      
      auto baseTy = E->getBase()->getType();
      auto keyPathTy = E->getKeyPath()->getType();
      auto resultTy = E->getType();
      
      if (keyPathTy->isAnyKeyPath()) {
        // AnyKeyPath application is <T> rvalue T -> rvalue Any?
        if (baseTy->is<LValueType>()) {
          Out << "AnyKeyPath application base is not an rvalue\n";
          abort();
        }
        auto resultObjTy = resultTy->getOptionalObjectType();
        if (!resultObjTy || !resultObjTy->isAny()) {
          Out << "AnyKeyPath application result must be Any?\n";
          abort();
        }
        return;
      } else if (auto bgt = keyPathTy->getAs<BoundGenericType>()) {
        if (keyPathTy->isPartialKeyPath()) {
          // PartialKeyPath<T> application is rvalue T -> rvalue Any
          if (!baseTy->isEqual(bgt->getGenericArgs()[0])) {
            Out << "PartialKeyPath application base doesn't match type\n";
            abort();
          }
          if (!resultTy->isAny()) {
            Out << "PartialKeyPath application result must be Any?\n";
            abort();
          }
          return;
        } else if (keyPathTy->isKeyPath()) {
          // KeyPath<T, U> application is rvalue T -> rvalue U
          if (!baseTy->isEqual(bgt->getGenericArgs()[0])) {
            Out << "KeyPath application base doesn't match type\n";
            abort();
          }
          if (!resultTy->isEqual(bgt->getGenericArgs()[1])) {
            Out << "KeyPath application result doesn't match type\n";
            abort();
          }
          return;
        } else if (keyPathTy->isWritableKeyPath()) {
          // WritableKeyPath<T, U> application is
          //    lvalue T -> lvalue U
          // or rvalue T -> rvalue U
          if (baseTy->is<LValueType>()) {
            if (!resultTy->is<LValueType>()) {
              Out << "WritableKeyPath base and result don't match lvalue-ness\n";
              abort();
            }
            baseTy = baseTy->getRValueType();
            resultTy = resultTy->getRValueType();
          }
          
          if (!baseTy->isEqual(bgt->getGenericArgs()[0])) {
            Out << "WritableKeyPath application base doesn't match type\n";
            abort();
          }
          if (!resultTy->isEqual(bgt->getGenericArgs()[1])) {
            Out << "WritableKeyPath application result doesn't match type\n";
            abort();
          }
          return;
        } else if (keyPathTy->isReferenceWritableKeyPath()) {
          // ReferenceWritableKeyPath<T, U> application is
          //    rvalue T -> lvalue U
          // or lvalue T -> lvalue U
          // or rvalue T -> rvalue U
          if (baseTy->is<LValueType>()) {
            if (!resultTy->is<LValueType>()) {
              Out << "ReferenceWritableKeyPath base and result don't "
                     "match lvalue-ness\n";
              abort();
            }
            baseTy = baseTy->getRValueType();
            resultTy = resultTy->getRValueType();
          } else {
            resultTy = resultTy->getRValueType();
          }

          if (!baseTy->isEqual(bgt->getGenericArgs()[0])) {
            Out << "ReferenceWritableKeyPath application base doesn't "
                   "match type\n";
            abort();
          }
          if (!resultTy->isEqual(bgt->getGenericArgs()[1])) {
            Out << "ReferenceWritableKeyPath application result doesn't "
                   "match type\n";
            abort();
          }
          return;
        }
      }
      
      Out << "invalid key path type\n";
      abort();
    }

    void verifyChecked(LoadExpr *E) {
      PrettyStackTraceExpr debugStack(Ctx, "verifying LoadExpr", E);

      auto *subExpr = E->getSubExpr();
      if (isa<ParenExpr>(subExpr) || isa<ForceValueExpr>(subExpr)) {
        Out << "Immediate ParenExpr/ForceValueExpr should precede a LoadExpr\n";
        E->dump(Out);
        Out << "\n";
        abort();
      }

      verifyCheckedBase(E);
    }

    void verifyChecked(ABISafeConversionExpr *E) {
      PrettyStackTraceExpr debugStack(Ctx, "verify ABISafeConversionExpr", E);

      auto toType = E->getType();
      auto fromType = E->getSubExpr()->getType();

      if (!fromType->hasLValueType())
        error("conversion source must be an l-value", E);

      if (!toType->hasLValueType())
        error("conversion result must be an l-value", E);

      {
        // At the moment, "ABI Safe" means concurrency features can be stripped.
        // Since we don't know how deeply the stripping is happening, to verify
        // in a fuzzy way, strip everything to see if they're the same type.
        auto strippedFrom = fromType->getRValueType()
                                    ->stripConcurrency(/*recurse*/true,
                                                       /*dropGlobalActor*/true);
        auto strippedTo = toType->getRValueType()
                                ->stripConcurrency(/*recurse*/true,
                                                   /*dropGlobalActor*/true);

        if (!strippedFrom->isEqual(strippedTo))
          error("possibly non-ABI safe conversion", E);
      }
    }

    void verifyChecked(MacroExpansionExpr *expansion) {
      MacroExpansionDiscriminatorKey key{
        MacroDiscriminatorContext::getParentOf(expansion).getOpaqueValue(),
        expansion->getMacroName().getBaseName().getIdentifier()
      };
      auto &discriminatorSet = MacroExpansionDiscriminators[key];
      unsigned discriminator = expansion->getDiscriminator();

      if (discriminator >= discriminatorSet.size()) {
        discriminatorSet.resize(discriminator+1);
        discriminatorSet.set(discriminator);
      } else if (discriminatorSet.test(discriminator)) {
        Out << "a macro expansion must have a unique discriminator "
            << "in its context\n";
        expansion->dump(Out);
        Out << "\n";
        abort();
      } else {
        discriminatorSet.set(discriminator);
      }
    }

    void verifyChecked(MacroExpansionDecl *expansion) {
      MacroExpansionDiscriminatorKey key{
        MacroDiscriminatorContext::getParentOf(expansion).getOpaqueValue(),
        expansion->getMacroName().getBaseName().getIdentifier()
      };
      auto &discriminatorSet = MacroExpansionDiscriminators[key];
      unsigned discriminator = expansion->getDiscriminator();

      if (discriminator >= discriminatorSet.size()) {
        discriminatorSet.resize(discriminator+1);
        discriminatorSet.set(discriminator);
      } else if (discriminatorSet.test(discriminator)) {
        Out << "a macro expansion must have a unique discriminator "
            << "in its context\n";
        expansion->dump(Out);
        Out << "\n";
        abort();
      } else {
        discriminatorSet.set(discriminator);
      }
    }

    void verifyChecked(ValueDecl *VD) {
      if (VD->getInterfaceType()->hasError()) {
        Out << "checked decl cannot have error type\n";
        VD->dump(Out);
        abort();
      }

      // Make sure that there are no archetypes in the interface type.
      if (!isa<VarDecl>(VD) && VD->getInterfaceType()->hasArchetype()) {
        Out << "Interface type contains archetypes\n";
        VD->dump(Out);
        abort();
      }

      if (VD->hasAccess()) {
        if (VD->getFormalAccess() == AccessLevel::Open) {
          if (!isa<ClassDecl>(VD) && !VD->isSyntacticallyOverridable()) {
            Out << "decl cannot be 'open'\n";
            VD->dump(Out);
            abort();
          }
          if (VD->isFinal()) {
            Out << "decl cannot be both 'open' and 'final'\n";
            VD->dump(Out);
            abort();
          }
        }
      }

      verifyCheckedBase(VD);
    }

    LazyInitializerWalking getLazyInitializerWalkingBehavior() override {
      // We don't want to walk into lazy initializers because they should
      // have been reparented to their synthesized getter, which will
      // invalidate various invariants.
      return LazyInitializerWalking::None;
    }

    void verifyChecked(PatternBindingDecl *binding) {
      // Look at all of the VarDecls being bound.
      for (auto idx : range(binding->getNumPatternEntries()))
        if (auto *P = binding->getPattern(idx))
          P->forEachVariable([&](VarDecl *VD) {
            // ParamDecls never get PBD's.
            assert(!isa<ParamDecl>(VD) && "ParamDecl has a PatternBindingDecl?");
          });
    }

    void verifyChecked(AbstractStorageDecl *ASD) {
      if (ASD->hasAccess() && ASD->isSettable(nullptr)) {
        auto setterAccess = ASD->getSetterFormalAccess();
        auto *setter = ASD->getAccessor(AccessorKind::Set);
        if (setter && setter->getFormalAccess() != setterAccess) {
          Out << "AbstractStorageDecl's setter access is out of sync"
                 " with the access actually on the setter\n";
          abort();
        }
      }

      if (auto getter = ASD->getAccessor(AccessorKind::Get)) {
        if (getter->isMutating() != ASD->isGetterMutating()) {
          Out << "AbstractStorageDecl::isGetterMutating is out of sync"
                 " with whether the getter is actually mutating\n";
          abort();
        }
      }
      if (auto setter = ASD->getAccessor(AccessorKind::Set)) {
        if (setter->isMutating() != ASD->isSetterMutating()) {
          Out << "AbstractStorageDecl::isSetterMutating is out of sync"
                 " with whether the setter is actually mutating\n";
          abort();
        }
      }
      if (auto addressor = ASD->getAccessor(AccessorKind::Address)) {
        if (addressor->isMutating() != ASD->isGetterMutating()) {
          Out << "AbstractStorageDecl::isGetterMutating is out of sync"
                 " with whether immutable addressor is mutating";
          abort();
        }
      }
      if (auto reader = ASD->getAccessor(AccessorKind::Read)) {
        if (reader->isMutating() != ASD->isGetterMutating()) {
          Out << "AbstractStorageDecl::isGetterMutating is out of sync"
                 " with whether read accessor is mutating";
          abort();
        }
      }
      if (auto addressor = ASD->getAccessor(AccessorKind::MutableAddress)) {
        if (addressor->isMutating() != ASD->isSetterMutating()) {
          Out << "AbstractStorageDecl::isSetterMutating is out of sync"
                 " with whether mutable addressor is mutating";
          abort();
        }
      }
      if (auto modifier = ASD->getAccessor(AccessorKind::Modify)) {
        if (modifier->isMutating() !=
            (ASD->isSetterMutating() || ASD->isGetterMutating())) {
          Out << "AbstractStorageDecl::isSetterMutating is out of sync"
                 " with whether modify addressor is mutating";
          abort();
        }
      }
      //      if (auto *VD = dyn_cast<VarDecl>(ASD->getStorage())) {
      //        const bool foundIt =
      //            llvm::any_of(vd->getAllAccessors(),
      //                         [&](AccessorDecl *VDA) { return VDA == ASD; });
      //        Out << "Accessor for a VarDecl must be listed in its accessors";
      //        abort();
      //      }

      verifyCheckedBase(ASD);
    }

    void verifyChecked(VarDecl *var) {
      if (!var->hasInterfaceType())
        return;

      PrettyStackTraceDecl debugStack("verifying VarDecl", var);

      // Variables must have materializable type.
      if (!var->getInterfaceType()->isMaterializable()) {
        Out << "VarDecl has non-materializable type: ";
        var->getInterfaceType().print(Out);
        Out << "\n";
        abort();
      }

      // The fact that this is *directly* be a reference storage type
      // cuts the code down quite a bit in getTypeOfReference.
      if (var->getAttrs().hasAttribute<ReferenceOwnershipAttr>() !=
          isa<ReferenceStorageType>(var->getInterfaceType().getPointer())) {
        if (var->getAttrs().hasAttribute<ReferenceOwnershipAttr>()) {
          Out << "VarDecl has an ownership attribute, but its type"
                 " is not a ReferenceStorageType: ";
        } else {
          Out << "VarDecl has no ownership attribute, but its type"
                 " is a ReferenceStorageType: ";
        }
        var->getInterfaceType().print(Out);
        abort();
      }

      Type typeForAccessors = var->getValueInterfaceType();
      if (const FuncDecl *getter = var->getAccessor(AccessorKind::Get)) {
        if (getter->getParameters()->size() != 0) {
          Out << "property getter has parameters\n";
          abort();
        }
        if (getter->hasInterfaceType()) {
          Type getterResultType = getter->getResultInterfaceType();
          if (!getterResultType->isEqual(typeForAccessors)) {
            Out << "property and getter have mismatched types: '";
            typeForAccessors.print(Out);
            Out << "' vs. '";
            getterResultType.print(Out);
            Out << "'\n";
            abort();
          }
        }
      }

      if (const FuncDecl *setter = var->getAccessor(AccessorKind::Set)) {
        if (setter->hasInterfaceType()) {
          if (!setter->getResultInterfaceType()->isVoid()) {
            Out << "property setter has non-Void result type\n";
            abort();
          }
          if (setter->getParameters()->size() == 0) {
            Out << "property setter has no parameters\n";
            abort();
          }
          if (setter->getParameters()->size() != 1) {
            Out << "property setter has 2+ parameters\n";
            abort();
          }
          const ParamDecl *param = setter->getParameters()->get(0);
          Type paramType = param->getInterfaceType();
          if (!paramType->isEqual(typeForAccessors)) {
            Out << "property and setter param have mismatched types:\n";
            typeForAccessors.dump(Out, 2);
            Out << "vs.\n";
            paramType.dump(Out, 2);
            abort();
          }
        }
      }

      if (var->isImplicitlyUnwrappedOptional()) {
        auto varTy = var->getValueInterfaceType();

        // FIXME: Update to look for plain Optional once
        // ImplicitlyUnwrappedOptional is removed
        if (!varTy->getOptionalObjectType()) {
          Out << "implicitly unwrapped optional attribute should only be set on VarDecl "
                 "with optional type\n";
          abort();
        }
      }

      if (auto *caseStmt =
	    dyn_cast_or_null<CaseStmt>(var->getRecursiveParentPatternStmt())) {
        // In a type checked AST, a case stmt that is a recursive parent pattern
        // stmt of a var decl, must have bound decls. This is because we
        // guarantee that all case label items bind corresponding patterns and
        // the case body var decls of a case stmt are created from the var decls
        // of the first case label items.
        if (!caseStmt->hasBoundDecls()) {
          Out << "parent CaseStmt of VarDecl does not have any case body "
                 "decls?!\n";
          abort();
        }
      }

      verifyCheckedBase(var);
    }

    // Dump a reference to the given declaration.
    void dumpRef(Decl *decl) {
      if (auto value = dyn_cast<ValueDecl>(decl))
        value->dumpRef(Out);
      else if (auto ext = dyn_cast<ExtensionDecl>(decl)) {
        Out << "extension of ";
        if (ext->getExtendedType())
          ext->getExtendedType().print(Out);
      }
    }

    /// Verify that the given conformance makes sense for the given
    /// type.
    void verifyConformance(Type type, ProtocolConformanceRef conformance) {
      if (conformance.isAbstract()) {
        if (!type->is<ArchetypeType>() && !type->isAnyExistentialType()) {
          Out << "type " << type
              << " should not have an abstract conformance to "
              << conformance.getRequirement()->getName();
          abort();
        }

        return;
      }

      if (!type->isEqual(conformance.getConcrete()->getType())) {
        Out << "conforming type does not match conformance\n";
        Out << "conforming type:\n";
        type.dump(Out, 2);
        Out << "\nconformance:\n";
        conformance.getConcrete()->dump(Out, 2);
        Out << "\n";
        abort();
      }
    }

    void verifyChecked(SubstitutionMap substitutions){
      // FIXME: Check replacement types without forcing anything.
    }

    /// Check the given explicit protocol conformance.
    void verifyConformance(Decl *decl, ProtocolConformance *conformance) {
      PrettyStackTraceDecl debugStack("verifying protocol conformance", decl);

      if (!conformance) {
        // FIXME: Eventually, this should itself be a verification
        // failure.
        return;
      }

      switch (conformance->getState()) {
      case ProtocolConformanceState::Complete:
        // More checking below.
        break;
        
      case ProtocolConformanceState::Incomplete:
        // Ignore incomplete conformances; we didn't need them.
        return;

      case ProtocolConformanceState::CheckingTypeWitnesses:
      case ProtocolConformanceState::Checking:
        dumpRef(decl);
        Out << " has a protocol conformance that is still being checked "
            << conformance->getProtocol()->getName().str() << "\n";
        abort();
      }

      auto normal = dyn_cast<NormalProtocolConformance>(conformance);
      if (!normal)
        return;

      // If the conformance is lazily resolved, don't check it; that can cause
      // massive deserialization at a point where the compiler cannot handle it.
      if (normal->isLazilyLoaded()) return;

      // Translate the owning declaration into a DeclContext.
      auto *nominal = dyn_cast<NominalTypeDecl>(decl);
      DeclContext *conformingDC;
      if (nominal) {
        conformingDC = nominal;
      } else {
        auto ext = cast<ExtensionDecl>(decl);
        conformingDC = ext;
        nominal = ext->getExtendedNominal();
      }

      auto proto = conformance->getProtocol();
      if (normal->getDeclContext() != conformingDC) {
        Out << "AST verification error: conformance of "
            << nominal->getName().str() << " to protocol "
            << proto->getName().str() << " is in the wrong context.\n"
            << "Owning context:\n";
        conformingDC->printContext(Out);
        Out << "Conformance context:\n";
        normal->getDeclContext()->printContext(Out);
        abort();
      }

      // Tracking for those Objective-C requirements that have witnesses.
      llvm::SmallDenseSet<std::pair<ObjCSelector, char>> hasObjCWitnessMap;
      bool populatedObjCWitnesses = false;
      auto populateObjCWitnesses = [&] {
        if (populatedObjCWitnesses)
          return;

        populatedObjCWitnesses = true;
        for (auto req : proto->getMembers()) {
          if (auto reqFunc = dyn_cast<AbstractFunctionDecl>(req)) {
            if (normal->hasWitness(reqFunc)) {
              hasObjCWitnessMap.insert(
                  {reqFunc->getObjCSelector(), reqFunc->isInstanceMember()});
            }
          }
        }
      };

      // Check whether there is a witness with the same selector and kind as
      // this requirement.
      auto hasObjCWitness = [&](ValueDecl *req) {
        if (!proto->isObjC())
          return false;

        auto func = dyn_cast<AbstractFunctionDecl>(req);
        if (!func)
          return false;

        populateObjCWitnesses();

        std::pair<ObjCSelector, char> key(
            func->getObjCSelector(), func->isInstanceMember());
        return hasObjCWitnessMap.count(key) > 0;
      };

      // Check that a normal protocol conformance is complete.
      for (auto member : proto->getMembers()) {
        if (auto assocType = dyn_cast<AssociatedTypeDecl>(member)) {
          if (!normal->hasTypeWitness(assocType)) {
            dumpRef(decl);
            Out << " is missing type witness for "
                << conformance->getProtocol()->getName().str() 
                << "." << assocType->getName().str()
                << "\n";
            abort();
          }

          // Make sure that the replacement type only uses archetypes allowed
          // in the context where the normal conformance exists.
          auto replacementType =
              normal->getTypeWitnessUncached(assocType).getWitnessType();
          Verifier(M, normal->getDeclContext())
            .verifyChecked(replacementType);
          continue;
        }
          
        // No witness necessary for type aliases
        if (isa<TypeAliasDecl>(member))
          continue;
        
        // If this is an accessor for something, ignore it.
        if (isa<AccessorDecl>(member))
          continue;

        if (auto req = dyn_cast<ValueDecl>(member)) {
          if (!normal->hasWitness(req)) {
            if ((req->getAttrs().isUnavailable(Ctx) ||
                 req->getAttrs().hasAttribute<OptionalAttr>()) &&
                proto->isObjC()) {
              continue;
            }

            // Check if *any* witness matches the Objective-C selector.
            if (hasObjCWitness(req))
              continue;

            dumpRef(decl);
            Out << " is missing witness for "
                << conformance->getProtocol()->getName().str() 
                << "." << req->getBaseName()
                << "\n";
            abort();
          }

          continue;
        }
      }

      // Make sure we have the right signature conformances.
      if (!normal->isInvalid()){
        auto conformances = normal->getSignatureConformances();
        unsigned idx = 0;
        auto reqs = proto->getRequirementSignature().getRequirements();
        for (const auto &req : reqs) {
          if (req.getKind() != RequirementKind::Conformance)
            continue;

          if (idx >= conformances.size()) {
            Out << "error: not enough conformances for requirement signature\n";
            normal->dump(Out);
            abort();
          }

          auto reqProto = req.getProtocolDecl();
          if (reqProto != conformances[idx].getRequirement()) {
            Out << "error: wrong protocol in signature conformances: have "
              << conformances[idx].getRequirement()->getName().str()
              << ", expected " << reqProto->getName().str()<< "\n";
            normal->dump(Out);
            abort();
          }

          ++idx;
        }

        if (idx != conformances.size()) {
          Out << "error: too many conformances for requirement signature\n";
          normal->dump(Out);
          abort();
        }
      }
    }

    void verifyChecked(GenericTypeDecl *generic) {
      verifyCheckedBase(generic);
    }

    void verifyChecked(NominalTypeDecl *nominal) {
      // Make sure that the protocol conformances are complete.
      // Only do so within the source file of the nominal type,
      // because anywhere else this can trigger new type-check requests.
      if (auto sf = M.dyn_cast<SourceFile *>()) {
        if (nominal->getParentSourceFile() == sf) {
          for (auto conformance : nominal->getLocalConformances()) {
            verifyConformance(nominal, conformance);
          }
        }
      }

      verifyCheckedBase(nominal);
    }

    void verifyCheckedAlways(GenericTypeParamDecl *GTPD) {
      PrettyStackTraceDecl debugStack("verifying GenericTypeParamDecl", GTPD);

      const DeclContext *DC = GTPD->getDeclContext();

      // Skip verification of deserialized generic param decls that have the
      // file set as their parent. This happens when they have not yet had their
      // correct parent set.
      // FIXME: This is a hack to workaround the fact that we don't necessarily
      // parent a GenericTypeParamDecl if we just deserialize its type.
      if (auto *fileDC = dyn_cast<FileUnit>(DC)) {
        if (fileDC->getKind() == FileUnitKind::SerializedAST)
          return;
      }

      if (!DC->isInnermostContextGeneric()) {
        Out << "DeclContext of GenericTypeParamDecl does not have "
               "generic params\n";
        abort();
      }

      GenericParamList *paramList =
          static_cast<const GenericContext *>(DC)->getGenericParams();
      if (!paramList) {
        Out << "DeclContext of GenericTypeParamDecl does not have "
               "generic params\n";
        abort();
      }

      if (paramList->getOuterParameters() &&
          !isa<ExtensionDecl>(DC)) {
        Out << "GenericParamList can only have outer parameters in an "
               "extension\n";
        abort();
      }

      unsigned currentDepth = DC->getGenericContextDepth();
      if (currentDepth < GTPD->getDepth()) {
        // If this is actually an opaque type's generic parameter, we're okay.
        if (auto value = dyn_cast_or_null<ValueDecl>(DC->getAsDecl())) {
          auto opaqueDecl = dyn_cast<OpaqueTypeDecl>(value);
          if (!opaqueDecl)
            opaqueDecl = value->getOpaqueResultTypeDecl();
          if (opaqueDecl) {
            if (GTPD->getDepth() ==
                    opaqueDecl->getOpaqueGenericParams().front()->getDepth()) {
              return;
            }
          }
        }

        Out << "GenericTypeParamDecl has incorrect depth\n";
        abort();
      }
      while (currentDepth > GTPD->getDepth()) {
        paramList = paramList->getOuterParameters();
        --currentDepth;
      }
      assert(paramList && "this is guaranteed by the parameter list's depth");

      if (paramList->size() <= GTPD->getIndex() ||
          paramList->getParams()[GTPD->getIndex()] != GTPD) {
        if (llvm::is_contained(paramList->getParams(), GTPD))
          Out << "GenericTypeParamDecl has incorrect index\n";
        else
          Out << "GenericTypeParamDecl not found in GenericParamList; "
                 "incorrect depth or wrong DeclContext\n";
        abort();
      }

      verifyCheckedBase(GTPD);
    }

    void verifyChecked(ExtensionDecl *ext) {
      // Make sure that the protocol conformances are complete.
      for (auto conformance : ext->getLocalConformances()) {
        verifyConformance(ext, conformance);
      }

      // Make sure extension binding succeeded.
      if (!ext->hasBeenBound()) {
        Out << "ExtensionDecl was not bound\n";
        abort();
      }
      verifyCheckedBase(ext);
    }

    void verifyParsed(EnumElementDecl *UED) {
      PrettyStackTraceDecl debugStack("verifying EnumElementDecl", UED);

      if (!isa<EnumDecl>(UED->getDeclContext())) {
        Out << "EnumElementDecl has wrong DeclContext";
        abort();
      }

      verifyParsedBase(UED);
    }

    void verifyParsed(EnumCaseDecl *D) {
      PrettyStackTraceDecl debugStack("verifying EnumCaseDecl", D);
      if (!D->getAttrs().isEmpty()) {
        Out << "EnumCaseDecl should not have attributes";
        abort();
      }

      verifyParsedBase(D);
    }

    void verifyParsed(AbstractFunctionDecl *AFD) {
      PrettyStackTraceDecl debugStack("verifying AbstractFunctionDecl", AFD);

      // All of the parameter names should match.
      if (!isa<DestructorDecl>(AFD)) { // Destructor has no non-self params.
        auto paramNames = AFD->getName().getArgumentNames();
        bool checkParamNames = (bool)AFD->getName();
        auto *firstParams = AFD->getParameters();

        if (checkParamNames &&
            paramNames.size() != firstParams->size()) {
          Out << "Function name does not match its argument pattern ("
              << paramNames.size() << " elements instead of "
              << firstParams->size() << ")\n";
          AFD->dump(Out);
          abort();
        }

        // This doesn't use for_each because paramNames shouldn't be checked
        // when the function is anonymous.
        for (size_t i = 0, e = firstParams->size(); i < e; ++i) {
          auto &param = firstParams->get(i);

          if (checkParamNames &&
              param->getArgumentName() != paramNames[i]) {
            Out << "Function full name doesn't match parameter's arg name\n";
            AFD->dump(Out);
            abort();
          }
        }
      }

      verifyParsedBase(AFD);
    }

    void verifyParsed(ConstructorDecl *CD) {
      PrettyStackTraceDecl debugStack("verifying ConstructorDecl", CD);
      verifyParsedBase(CD);
    }

    void verifyChecked(ProtocolDecl *PD) {
      PrettyStackTraceDecl debugStack("verifying ProtocolDecl", PD);

      if (PD->isObjC() && !PD->requiresClass()) {
        Out << "@objc protocols should be class protocols as well";
        abort();
      }
      verifyCheckedBase(PD);
    }

    void verifyChecked(ConstructorDecl *CD) {
      PrettyStackTraceDecl debugStack("verifying ConstructorDecl", CD);

      auto *ND = CD->getDeclContext()->getSelfNominalTypeDecl();
      if (!isa<ClassDecl>(ND) && !isa<StructDecl>(ND) && !isa<EnumDecl>(ND) &&
          !isa<ProtocolDecl>(ND) && !CD->isInvalid()) {
        Out << "ConstructorDecls outside structs, classes or enums "
               "should be marked invalid";
        abort();
      }

      // Verify that the optionality of the result type of the
      // initializer matches the failability of the initializer.
      if (!CD->isInvalid() &&
          !CD->getDeclContext()->getDeclaredInterfaceType()->isOptional()) {
        bool resultIsOptional = (bool) CD->getResultInterfaceType()
            ->getOptionalObjectType();
        auto declIsOptional = CD->isFailable();

        if (resultIsOptional != declIsOptional) {
          Out << "Initializer has result optionality/failability mismatch\n";
          CD->dump(llvm::errs());
          abort();
        }

        // Also check the interface type.
        if (auto genericFn 
              = CD->getInterfaceType()->getAs<GenericFunctionType>()) {
          resultIsOptional = (bool) genericFn->getResult()
              ->castTo<AnyFunctionType>()
              ->getResult()
              ->getOptionalObjectType();
          if (resultIsOptional != declIsOptional) {
            Out << "Initializer has result optionality/failability mismatch\n";
            CD->dump(llvm::errs());
            abort();
          }
        }
      }

      if (CD->isFailable()) {
        auto resultTy = CD->getResultInterfaceType();
        if (!resultTy->getOptionalObjectType()) {
          Out << "implicitly unwrapped optional attribute should only be set "
                 "on constructors with optional return types\n";
          CD->dump(llvm::errs());
          abort();
        }
      } else {
        if (CD->isImplicitlyUnwrappedOptional()) {
          Out << "Expected failable constructor if result is IUO\n";
          CD->dump(llvm::errs());
          abort();
        }
      }

      verifyCheckedBase(CD);
    }

    void verifyParsed(DestructorDecl *DD) {
      PrettyStackTraceDecl debugStack("verifying DestructorDecl", DD);

      if (DD->isGeneric()) {
        Out << "DestructorDecl cannot be generic";
        abort();
      }
      verifyParsedBase(DD);
    }

    void verifyChecked(AbstractFunctionDecl *AFD) {
      PrettyStackTraceDecl debugStack("verifying AbstractFunctionDecl", AFD);

      if (!AFD->hasInterfaceType())
        return;

      // If this function is generic or is within a generic context, it should
      // have an interface type.
      if (AFD->isGenericContext() !=
          AFD->getInterfaceType()->is<GenericFunctionType>()) {
        Out << "Functions in generic context must have an interface type\n";
        AFD->dump(Out);
        abort();
      }

      // If the function has a generic interface type, it should also have a
      // generic signature.
      if (AFD->isGenericContext() !=
          (!AFD->getGenericSignature().isNull())) {
        Out << "Functions in generic context must have a generic signature\n";
        AFD->dump(Out);
        abort();
      }

      // If there is an interface type, it shouldn't have any unresolved
      // dependent member types.
      // FIXME: This is a general property of the type system.
      auto interfaceTy = AFD->getInterfaceType();
      if (auto unresolvedDependentTy
            = interfaceTy->findUnresolvedDependentMemberType()) {
        Out << "Unresolved dependent member type ";
        unresolvedDependentTy->print(Out);
        abort();
      }

      // Check that type members have an interface type of the form
      // (Self) -> (Args...) -> Result.
      if (AFD->hasImplicitSelfDecl()) {
        if (!interfaceTy->castTo<AnyFunctionType>()
              ->getResult()->is<FunctionType>()) {
          Out << "Interface type of method must return a function";
          interfaceTy->dump(Out);
          abort();
        }
      }

      // Asynchronous @objc methods must have a foreign async convention.
      if (AFD->isObjC() &&
          static_cast<bool>(AFD->getForeignAsyncConvention())
            != AFD->hasAsync()) {
        if (AFD->hasAsync())
          Out << "@objc method async but does not have a foreign async "
              << "convention";
        else
          Out << "@objc method has a foreign async convention but is not "
              << "async";
        abort();
      }

      // Synchronous throwing @objc methods must have a foreign error
      // convention.
      if (AFD->isObjC() &&
          !AFD->hasAsync() &&
          static_cast<bool>(AFD->getForeignErrorConvention())
            != AFD->hasThrows()) {
        if (AFD->hasThrows())
          Out << "@objc method throws but does not have a foreign error "
              << "convention";
        else
          Out << "@objc method has a foreign error convention but does not "
              << "throw";
        abort();
      }

      // If a decl has the Throws bit set, the ThrowsLoc should be valid,
      // and vice versa, unless the decl was imported, de-serialized, or
      // implicit.
      if (!AFD->isImplicit() &&
          isa<SourceFile>(AFD->getModuleScopeContext()) &&
          (AFD->getThrowsLoc().isValid() != AFD->hasThrows())) {
        Out << "function 'throws' location does not match 'throws' flag\n";
        AFD->dump(Out);
        abort();
      }

      // If a decl has the Throws bit set, the function type should throw,
      // and vice versa.
      auto fnTy = AFD->getInterfaceType()->castTo<AnyFunctionType>();
      if (AFD->hasImplicitSelfDecl())
        fnTy = fnTy->getResult()->castTo<FunctionType>();

      if (AFD->hasThrows() != fnTy->getExtInfo().isThrowing()) {
        Out << "function 'throws' flag does not match function type\n";
        AFD->dump(Out);
        abort();
      }

      if (AFD->getForeignErrorConvention()
          && !AFD->isObjC() && !AFD->getAttrs().hasAttribute<CDeclAttr>()) {
        Out << "foreign error convention on non-@objc, non-@_cdecl function\n";
        AFD->dump(Out);
        abort();
      }

      verifyCheckedBase(AFD);
    }

    void verifyChecked(DestructorDecl *DD) {
      PrettyStackTraceDecl debugStack("verifying DestructorDecl", DD);

      auto *ND = DD->getDeclContext()->getSelfNominalTypeDecl();
      if (!isa<ClassDecl>(ND) && !ND->isMoveOnly() && !DD->isInvalid()) {
        Out << "DestructorDecls outside classes/move only types should be "
               "marked invalid\n";
        abort();
      }
      verifyCheckedBase(DD);
    }

    void verifyChecked(FuncDecl *FD) {
      PrettyStackTraceDecl debugStack("verifying FuncDecl", FD);

      // Note that there's nothing inherently wrong with wanting to use this on
      // non-accessors in the future, but you should visit all call sites and
      // make sure we do the right thing, because this flag conflates several
      // different behaviors.
      if (FD->hasForcedStaticDispatch()) {
        auto *AD = dyn_cast<AccessorDecl>(FD);
        if (AD == nullptr) {
          Out << "hasForcedStaticDispatch() set on non-accessor\n";
          abort();
        }

        if (AD->getStorage()->requiresOpaqueAccessor(AD->getAccessorKind())) {
          Out << "hasForcedStaticDispatch() set on accessor that's opaque "
                 "for its storage\n";
          abort();
        }
      }

      if (FD->isMutating()) {
        if (!FD->isInstanceMember()) {
          Out << "mutating function is not an instance member\n";
          abort();
        }
        if (FD->getDeclContext()->getSelfClassDecl()) {
          Out << "mutating function in a class\n";
          abort();
        }
        const ParamDecl *selfParam = FD->getImplicitSelfDecl(
            /*createIfNeeded=*/false);
        if (selfParam && !selfParam->isInOut()) {
          Out << "mutating function does not have inout 'self'\n";
          abort();
        }
      } else {
        const ParamDecl *selfParam = FD->getImplicitSelfDecl(
            /*createIfNeeded=*/false);
        if (selfParam && selfParam->isInOut()) {
          Out << "non-mutating function has inout 'self'\n";
          abort();
        }
      }

      if (FD->isImplicitlyUnwrappedOptional()) {
        auto resultTy = FD->getResultInterfaceType();
        if (!resultTy->getOptionalObjectType()) {
          Out << "implicitly unwrapped optional attribute should only be set "
                 "on functions with optional return types\n";
          abort();
        }
      }

      verifyCheckedBase(FD);
    }

    void verifyChecked(AccessorDecl *FD) {
      PrettyStackTraceDecl debugStack("verifying AccessorDecl", FD);

      auto *storageDecl = FD->getStorage();
      if (!storageDecl) {
        Out << "Missing storage decl\n";
        abort();
      }

      if (FD->isGetterOrSetter()) {
        if (FD->isFinal() != storageDecl->isFinal()) {
          Out << "Property and accessor do not match for 'final'\n";
          abort();
        }
        if (FD->isDynamic() != storageDecl->isDynamic() &&
            // We allow a non dynamic setter if there is a dynamic modify,
            // observer, or mutable addressor.
            !(FD->isSetter() &&
              (storageDecl->getWriteImpl() == WriteImplKind::Modify ||
               storageDecl->getWriteImpl() ==
                   WriteImplKind::StoredWithObservers ||
               storageDecl->getWriteImpl() == WriteImplKind::MutableAddress) &&
              storageDecl->shouldUseNativeDynamicDispatch()) &&
            // We allow a non dynamic getter if there is a dynamic read.
            !(FD->isGetter() &&
              (storageDecl->getReadImpl() == ReadImplKind::Read ||
               storageDecl->getReadImpl() == ReadImplKind::Address) &&
              storageDecl->shouldUseNativeDynamicDispatch())) {
          Out << "Property and accessor do not match for 'dynamic'\n";
          abort();
        }
        if (FD->isDynamic()) {
          if (FD->isObjC() != storageDecl->isObjC()) {
            Out << "Property and accessor do not match for '@objc'\n";
            abort();
          }
        }
      }

      auto storedAccessor = storageDecl->getAccessor(FD->getAccessorKind());
      if (storedAccessor != FD) {
        Out << "storage declaration has different accessor for this kind\n";
        abort();
      }

      verifyCheckedBase(FD);
    }

    void verifyParsed(FuncDecl *FD) {
      PrettyStackTraceDecl debugStack("verifying FuncDecl", FD);

      verifyParsedBase(FD);
    }

    void verifyParsed(AccessorDecl *FD) {
      PrettyStackTraceDecl debugStack("verifying AccessorDecl", FD);

      verifyParsedBase(FD);
    }

    void verifyChecked(ClassDecl *CD) {
      PrettyStackTraceDecl debugStack("verifying ClassDecl", CD);
      
      if (!CD->hasLazyMembers()) {
        unsigned NumDestructors = 0;
        for (auto Member : CD->getMembers()) {
          if (isa<DestructorDecl>(Member)) {
            ++NumDestructors;
          }
        }
        if (NumDestructors > 1) {
          Out << "every class should have at most one destructor, "
                 "explicitly provided or created by the type checker\n";
          abort();
        }
      }

      verifyCheckedBase(CD);
    }

    void verifyParsed(AssociatedTypeDecl *ATD) {
      PrettyStackTraceDecl debugStack("verifying AssociatedTypeDecl", ATD);

      auto *DC = ATD->getDeclContext();
      if (!isa<NominalTypeDecl>(DC) ||
          !isa<ProtocolDecl>(cast<NominalTypeDecl>(DC))) {
        Out << "AssociatedTypeDecl should only occur inside a protocol\n";
        abort();
      }
      verifyParsedBase(ATD);
    }

    void verifyParsed(TuplePattern *TP) {
      PrettyStackTracePattern debugStack(Ctx, "verifying TuplePattern", TP);
      verifyParsedBase(TP);
    }

    void verifyChecked(TuplePattern *TP) {
      PrettyStackTracePattern debugStack(Ctx, "verifying TuplePattern", TP);
      verifyCheckedBase(TP);
    }

    /// Look through a possible l-value type, returning true if it was
    /// an l-value.
    bool lookThroughLValue(Type &type, bool &isInOut) {
      if (LValueType *lv = type->getAs<LValueType>()) {
        Type objectType = lv->getObjectType();
        if (objectType->is<LValueType>()) {
          Out << "type is an lvalue of lvalue type: ";
          type.print(Out);
          Out << "\n";
        }
        isInOut = false;
        type = objectType;
        return true;
      }
      if (InOutType *io = type->getAs<InOutType>()) {
        Type objectType = io->getObjectType();
        if (objectType->is<InOutType>()) {
          Out << "type is an inout of inout type: ";
          type.print(Out);
          Out << "\n";
        }
        isInOut = true;
        type = objectType;
        return true;
      }
      return false;
    }

    /// The two types are required to either both be l-values or
    /// both not be l-values.  They are adjusted to not be l-values.
    /// Returns true if they are both l-values.
    bool checkSameLValueness(Type &T0, Type &T1,
                             const char *what) {
      bool Q0, Q1;
      bool isLValue0 = lookThroughLValue(T0, Q0);
      bool isLValue1 = lookThroughLValue(T1, Q1);
      
      if (isLValue0 != isLValue1) {
        Out << "lvalue-ness of " << what << " do not match: "
            << isLValue0 << ", " << isLValue1 << "\n";
        abort();
      }

      if (isLValue0 && Q0 != Q1) {
        Out << "qualification of " << what << " do not match\n";
        abort();
      }

      return isLValue0;
    }

    Type checkLValue(Type T, const char *what) {
      LValueType *LV = T->getAs<LValueType>();
      if (LV)
        return LV->getObjectType();

      Out << "type is not an l-value in " << what << ": ";
      T.print(Out);
      Out << "\n";
      abort();
    }

    // Verification utilities.
    Type checkMetatypeType(Type type, const char *what) {
      auto metatype = type->getAs<AnyMetatypeType>();
      if (metatype) return metatype->getInstanceType();

      Out << what << " is not a metatype: ";
      type.print(Out);
      Out << "\n";
      abort();
    }

    void checkSameType(Type T0, Type T1, const char *what) {
      if (T0->isEqual(T1))
        return;

      Out << "different types for " << what << ": ";
      T0.print(Out);
      Out << " vs. ";
      T1.print(Out);
      Out << "\n";
      abort();
    }

    void checkTrivialSubtype(Type srcTy, Type destTy, const char *what) {
      if (srcTy->isEqual(destTy)) return;

      if (auto srcMetatype = srcTy->getAs<AnyMetatypeType>()) {
        if (auto destMetatype = destTy->getAs<AnyMetatypeType>()) {
          return checkTrivialSubtype(srcMetatype->getInstanceType(),
                                     destMetatype->getInstanceType(),
                                     what);
        }
        goto fail;
      }

      // If the destination is a class, walk the supertypes of the source.
      if (destTy->getClassOrBoundGenericClass()) {
        if (!destTy->isBindableToSuperclassOf(srcTy)) {
          srcTy.print(Out);
          Out << " is not a superclass of ";
          destTy.print(Out);
          Out << " for " << what << "\n";
          abort();
        }

        return;
      }

      // FIXME: Tighten up checking for conversions to protocol types.
      if (destTy->isExistentialType())
        return;

    fail:
      Out << "subtype conversion in " << what << " is invalid: ";
      srcTy.print(Out);
      Out << " to ";
      destTy.print(Out);
      Out << "\n";
      abort();
    }

    void checkSameOrSubType(Type T0, Type T1, const char *what) {
      if (T0->isEqual(T1))
        return;

      // Protocol subtyping.
      if (auto Proto0 = T0->getAs<ProtocolType>())
        if (auto Proto1 = T1->getAs<ProtocolType>())
          if (Proto0->getDecl()->inheritsFrom(Proto1->getDecl()))
            return;
      
      // FIXME: Actually check this?
      if (T0->isExistentialType() || T1->isExistentialType())
        return;
      
      Out << "incompatible types for " << what << ": ";
      T0.print(Out);
      Out << " vs. ";
      T1.print(Out);
      Out << "\n";
      abort();
    }

    Type checkExceptionTypeExists(const char *where) {
      if (!Ctx.getErrorDecl()) {
        Out << "exception type does not exist in " << where << "\n";
        abort();
      }

      return Ctx.getErrorExistentialType();
    }

    bool isGoodSourceRange(SourceRange SR) {
      if (SR.isInvalid())
        return false;
      (void) Ctx.SourceMgr.findBufferContainingLoc(SR.Start);
      (void) Ctx.SourceMgr.findBufferContainingLoc(SR.End);
      return true;
    }
    
    template<typename T>
    void checkSourceRangesBase(T ASTNode) {
      checkSourceRanges(cast<typename ASTNodeBase<T>::BaseTy>(ASTNode));
    }
    
    void checkSourceRanges(Expr *E) {
      PrettyStackTraceExpr debugStack(Ctx, "verifying ranges", E);

      if (!E->getSourceRange().isValid()) {
        // We don't care about source ranges on implicitly-generated
        // expressions.
        if (E->isImplicit())
          return;

        Out << "invalid source range for expression: ";
        E->dump(Out);
        Out << "\n";
        abort();
      }
      if (!isGoodSourceRange(E->getSourceRange())) {
        Out << "bad source range for expression: ";
        E->dump(Out);
        Out << "\n";
        abort();
      }
      // FIXME: Re-visit this to always do the check.
      if (!E->isImplicit())
        checkSourceRanges(E->getSourceRange(), Parent,
                          [&]{ E->dump(Out); } );
    }

    void checkSourceRanges(Stmt *S) {
      PrettyStackTraceStmt debugStack(Ctx, "verifying ranges", S);

      if (!S->getSourceRange().isValid()) {
        // We don't care about source ranges on implicitly-generated
        // statements.
        if (S->isImplicit())
          return;

        Out << "invalid source range for statement: ";
        S->dump(Out);
        Out << "\n";
        abort();
      }
      if (!isGoodSourceRange(S->getSourceRange())) {
        Out << "bad source range for statement: ";
        S->dump(Out);
        Out << "\n";
        abort();
      }
      checkSourceRanges(S->getSourceRange(), Parent,
                        [&]{ S->dump(Out); });
    }

    void checkSourceRanges(IfConfigDecl *ICD) {
      checkSourceRangesBase(ICD);

      SourceLoc Location = ICD->getStartLoc();
      for (auto &Clause : ICD->getClauses()) {
        // Clause start, note that the first clause start location is the
        // same as that of the whole statement
        if (Location == ICD->getStartLoc()) {
          if (Location != Clause.Loc) {
            Out << "bad start location of IfConfigDecl first clause\n";
            ICD->print(Out);
            abort();
          }
        } else {
          if (!Ctx.SourceMgr.isBeforeInBuffer(Location, Clause.Loc)) {
            Out << "bad start location of IfConfigDecl clause\n";
            ICD->print(Out);
            abort();
          }
        }
        Location = Clause.Loc;

        // Condition if present
        Expr *Cond = Clause.Cond;
        if (Cond) {
          if (!Ctx.SourceMgr.isBeforeInBuffer(Location, Cond->getStartLoc())) {
            Out << "invalid IfConfigDecl clause condition start location\n";
            ICD->print(Out);
            abort();
          }
          Location = Cond->getEndLoc();
        }
        
        // Body elements
        auto StoredLoc = Location;
        for (auto &Element : Clause.Elements) {
          auto StartLocation = Element.getStartLoc();
          if (StartLocation.isInvalid()) {
            continue;
          }
          
          if (!Ctx.SourceMgr.isBeforeInBuffer(StoredLoc, StartLocation)) {
            Out << "invalid IfConfigDecl clause element start location\n";
            ICD->print(Out);
            abort();
          }
          
          auto EndLocation = Element.getEndLoc();
          if (EndLocation.isValid() &&
              Ctx.SourceMgr.isBeforeInBuffer(Location, EndLocation)) {
            Location = EndLocation;
          }
        }
      }

      if (Ctx.SourceMgr.isBeforeInBuffer(ICD->getEndLoc(), Location)) {
        Out << "invalid IfConfigDecl end location\n";
        ICD->print(Out);
        abort();
      }
    }
    
    void checkSourceRanges(Pattern *P) {
      PrettyStackTracePattern debugStack(Ctx, "verifying ranges", P);

      // We don't care about source ranges on implicitly-generated
      // patterns.
      if (P->isImplicit())
        return;

      if (!P->getSourceRange().isValid()) {
        Out << "invalid source range for pattern: ";
        P->print(Out);
        Out << "\n";
        abort();
      }
      if (!isGoodSourceRange(P->getSourceRange())) {
        Out << "bad source range for pattern: ";
        P->print(Out);
        Out << "\n";
        abort();
      }
      checkSourceRanges(P->getSourceRange(), Parent,
                        [&]{ P->print(Out); });
    }

    void assertValidRegion(Decl *D) {
      auto R = D->getSourceRange();
      if (R.isValid() && Ctx.SourceMgr.isBeforeInBuffer(R.End, R.Start)) {
        Out << "invalid type source range for decl: ";
        D->print(Out);
        Out << "\n";
        abort();
      }
    }

    void checkSourceRanges(ParamDecl *PD) {
      assertValidRegion(PD);
    }

    void checkSourceRanges(Decl *D) {
      PrettyStackTraceDecl debugStack("verifying ranges", D);
      const auto SR = D->getSourceRange();

      // We don't care about source ranges on implicitly-generated
      // decls.
      if (D->isImplicit())
        return;

      if (!SR.isValid()) {
        Out << "invalid source range for decl: ";
        D->print(Out);
        Out << "\n";
        abort();
      }
      // ASTScope lookup depends on Decls having correct ordering.
      // Move the order check to isGoodSourceRange after extending
      // the invariant to Exprs, etc., per rdar://53637494
      if (Ctx.SourceMgr.isBeforeInBuffer(SR.End, SR.Start)) {
        Out << "backwards source range for decl: ";
        D->print(Out);
        Out << "\n";
        abort();
      }
      checkSourceRanges(SR, Parent, [&] { D->print(Out); });
    }

    /// Verify that the given source ranges is contained within the
    /// parent's source range.
    void checkSourceRanges(SourceRange Current, ASTWalker::ParentTy Parent,
                           llvm::function_ref<void()> printEntity) {
      SourceRange Enclosing;
      if (Parent.isNull())
          return;

      // An alternative enclosing scope, used for macro expansions.
      SourceRange AltEnclosing;
      if (Parent.getAsModule()) {
        return;
      } else if (Decl *D = Parent.getAsDecl()) {
        Enclosing = D->getSourceRange();

        // If the current source range is in a macro expansion buffer, its enclosing
        // context can be in the source file where the macro expansion originated. In
        // this case, grab the source range of the original ASTNode that was expanded.
        if (!Ctx.SourceMgr.rangeContains(Enclosing, Current)) {
          auto *expansionBuffer =
              D->getModuleContext()->getSourceFileContainingLocation(Current.Start);
          if (auto expansion = expansionBuffer->getMacroExpansion()) {
            Current = expansion.getSourceRange();
          }
        }

        if (D->isImplicit())
          return;
        // FIXME: This is not working well for decl parents.
        // But it must work when using lazy ASTScopes for IterableDeclContexts
        if (!isa<IterableDeclContext>(D))
          return;
      } else if (Stmt *S = Parent.getAsStmt()) {
        Enclosing = S->getSourceRange();
        if (S->isImplicit())
          return;
      } else if (Pattern *P = Parent.getAsPattern()) {
        Enclosing = P->getSourceRange();
        if (P->isImplicit())
          return;
      } else if (Expr *E = Parent.getAsExpr()) {
        // FIXME: This hack is required because the inclusion check below
        // doesn't compares the *start* of the ranges, not the end of the
        // ranges.  In the case of an interpolated string literal expr, the
        // subexpressions are contained within the string token.  This means
        // that comparing the start of the string token to the end of an
        // embedded expression will fail.
        if (isa<InterpolatedStringLiteralExpr>(E))
          return;

        if (E->isImplicit())
          return;

        if (auto expansion = dyn_cast<MacroExpansionExpr>(E)) {
          if (auto rewritten = expansion->getRewritten())
            AltEnclosing = rewritten->getSourceRange();
        }

        Enclosing = E->getSourceRange();
      } else if (TypeRepr *TyR = Parent.getAsTypeRepr()) {
        Enclosing = TyR->getSourceRange();
      } else {
        llvm_unreachable("impossible parent node");
      }
      
      if (!Ctx.SourceMgr.rangeContains(Enclosing, Current) &&
          !(AltEnclosing.isValid() &&
            Ctx.SourceMgr.rangeContains(AltEnclosing, Current))) {
        Out << "child source range not contained within its parent: ";
        printEntity();
        Out << "\n  parent range: ";
        Enclosing.print(Out, Ctx.SourceMgr);
        Out << "\n  child range: ";
        Current.print(Out, Ctx.SourceMgr);
        Out << "\n";
        abort();
      }
    }

    void checkErrors(Expr *E) {}
    void checkErrors(Stmt *S) {}
    void checkErrors(Pattern *P) {}
    void checkErrors(Decl *D) {}
    void checkErrors(ValueDecl *D) {}
  };
} // end anonymous namespace

static bool shouldVerifyGivenContext(const ASTContext &ctx) {
  using ASTVerifierOverrideKind = LangOptions::ASTVerifierOverrideKind;
  switch (ctx.LangOpts.ASTVerifierOverride) {
  case ASTVerifierOverrideKind::EnableVerifier:
    return true;
  case ASTVerifierOverrideKind::DisableVerifier:
    return false;
  case ASTVerifierOverrideKind::NoOverride:
#ifndef NDEBUG
    // asserts. Default behavior is to run.
    return true;
#else
    // no-asserts. Default behavior is not to run the verifier.
    return false;
#endif
  }
  llvm_unreachable("Covered switch isn't covered?!");
}

void swift::verify(SourceFile &SF) {
  if (!shouldVerifyGivenContext(SF.getASTContext()))
    return;
  Verifier verifier(SF, &SF);
  SF.walk(verifier);
}

bool swift::shouldVerify(const Decl *D, const ASTContext &Context) {
  if (!shouldVerifyGivenContext(Context))
    return false;

  if (const auto *ED = dyn_cast<ExtensionDecl>(D)) {
    return shouldVerify(ED->getExtendedNominal(), Context);
  }

  const auto *VD = dyn_cast<ValueDecl>(D);
  if (!VD) {
    // Verify declarations without names everywhere.
    return true;
  }

  return true;
}

void swift::verify(Decl *D) {
  if (!shouldVerifyGivenContext(D->getASTContext()))
    return;

  Verifier V = Verifier::forDecl(D);
  D->walk(V);
}

