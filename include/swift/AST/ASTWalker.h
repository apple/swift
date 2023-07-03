//===--- ASTWalker.h - Class for walking the AST ----------------*- C++ -*-===//
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

#ifndef SWIFT_AST_ASTWALKER_H
#define SWIFT_AST_ASTWALKER_H

#include "swift/Basic/LLVM.h"
#include "llvm/ADT/Optional.h"
#include "llvm/ADT/PointerUnion.h"
#include <utility>

namespace swift {

class ArgumentList;
class Decl;
class Expr;
class ClosureExpr;
class CustomAttr;
class ModuleDecl;
class PackageUnit;
class Stmt;
class Pattern;
class TypeRepr;
class ParameterList;
enum class AccessKind: unsigned char;

enum class SemaReferenceKind : uint8_t {
  ModuleRef = 0,
  DeclRef,
  DeclMemberRef,
  DeclConstructorRef,
  TypeRef,
  EnumElementRef,
  SubscriptRef,
  DynamicMemberRef,
};

struct ReferenceMetaData {
  SemaReferenceKind Kind;
  llvm::Optional<AccessKind> AccKind;
  bool isImplicit = false;
  bool isImplicitCtorType = false;

  /// When non-none, this is a custom attribute reference.
  llvm::Optional<std::pair<const CustomAttr *, Decl *>> CustomAttrRef;

  ReferenceMetaData(SemaReferenceKind Kind, llvm::Optional<AccessKind> AccKind,
                    bool isImplicit = false,
                    llvm::Optional<std::pair<const CustomAttr *, Decl *>>
                        customAttrRef = llvm::None)
      : Kind(Kind), AccKind(AccKind), isImplicit(isImplicit),
        CustomAttrRef(customAttrRef) {}
};

/// Specifies how the initialization expression of a \c lazy variable should be
/// walked by the ASTWalker.
enum class LazyInitializerWalking {
  /// No lazy initialization expressions will be walked.
  None,

  /// The lazy initialization expression will only be walked as a part of
  /// the variable's pattern binding decl. This is the default behavior, and is
  /// consistent with the initializer being syntactically part of the pattern
  /// binding.
  InPatternBinding,

  /// The lazy initialization expression will only be walked as part of the
  /// body of the synthesized accessor for the lazy variable. In such an
  /// accessor, the expression is denoted by LazyInitializerExpr. This is mainly
  /// useful for code emission.
  InAccessor
};

/// Specifies the behavior for walking a macro expansion, whether we want to
/// see the macro arguments, the expansion, or both.
enum class MacroWalking {
  /// Walk into the expansion of the macro, to see the semantic effect of
  /// the macro expansion.
  Expansion,

  /// Walk into the arguments of the macro as written in the source code.
  ///
  /// The actual arguments walked may not make it into the program itself,
  /// because they can be translated by the macro in arbitrary ways.
  Arguments,

  /// Walk into both the arguments of the macro as written in the source code
  /// and also the macro expansion.
  ArgumentsAndExpansion,

  /// Don't walk into macros.
  None
};

/// An abstract class used to traverse an AST.
class ASTWalker {
public:
  enum class ParentKind {
    Package, Module, Decl, Stmt, Expr, Pattern, TypeRepr
  };

  class ParentTy {
    ParentKind Kind;
    void *Ptr = nullptr;

  public:
    ParentTy(PackageUnit *Pkg) : Kind(ParentKind::Package), Ptr(Pkg) {}
    ParentTy(ModuleDecl *Mod) : Kind(ParentKind::Module), Ptr(Mod) {}
    ParentTy(Decl *D) : Kind(ParentKind::Decl), Ptr(D) {}
    ParentTy(Stmt *S) : Kind(ParentKind::Stmt), Ptr(S) {}
    ParentTy(Expr *E) : Kind(ParentKind::Expr), Ptr(E) {}
    ParentTy(Pattern *P) : Kind(ParentKind::Pattern), Ptr(P) {}
    ParentTy(TypeRepr *T) : Kind(ParentKind::TypeRepr), Ptr(T) {}
    ParentTy() : Kind(ParentKind::Module), Ptr(nullptr) { }

    bool isNull() const { return Ptr == nullptr; }
    ParentKind getKind() const {
      assert(!isNull());
      return Kind;
    }

    PackageUnit *getAsPackage() const {
      return Kind == ParentKind::Package ? static_cast<PackageUnit*>(Ptr)
                                        : nullptr;
    }
    ModuleDecl *getAsModule() const {
      return Kind == ParentKind::Module ? static_cast<ModuleDecl*>(Ptr)
                                        : nullptr;
    }
    Decl *getAsDecl() const {
      return Kind == ParentKind::Decl ? static_cast<Decl*>(Ptr) : nullptr;
    }
    Stmt *getAsStmt() const {
      return Kind == ParentKind::Stmt ? static_cast<Stmt*>(Ptr) : nullptr;
    }
    Expr *getAsExpr() const {
      return Kind == ParentKind::Expr ? static_cast<Expr*>(Ptr) : nullptr;
    }
    Pattern *getAsPattern() const {
      return Kind == ParentKind::Pattern ? static_cast<Pattern*>(Ptr) : nullptr;
    }
    TypeRepr *getAsTypeRepr() const {
      return Kind==ParentKind::TypeRepr ? static_cast<TypeRepr*>(Ptr) : nullptr;
    }
  };

  /// The parent of the node we are visiting.
  ParentTy Parent;

  struct _Detail {
    _Detail() = delete;

    // The 'Action' set of types, which do not take a payload.
    struct ContinueWalkAction {};
    struct SkipChildrenIfWalkAction { bool Cond; };
    struct StopIfWalkAction { bool Cond; };
    struct StopWalkAction {};

    // The 'Result' set of types, which do take a payload.
    template <typename T>
    struct ContinueWalkResult {
      T Value;
    };
    template <typename T>
    struct SkipChildrenIfWalkResult {
      bool Cond;
      T Value;
    };
    template <typename T>
    struct StopIfWalkResult {
      bool Cond;
      T Value;
    };
  };

  /// A namespace for ASTWalker actions that may be returned from pre-walk and
  /// post-walk functions.
  ///
  /// Only certain AST nodes support being replaced during a walk. The
  /// visitation methods for such nodes use the PreWalkResult/PostWalkResult
  /// types, which store both the action to take, along with the AST node to
  /// splice into the tree in place of the old node. The node must be provided
  /// to \c Action::<action> function, e.g \c Action::Continue(E) or
  /// \c Action::SkipChildren(E). The only exception is \c Action::Stop(),
  /// which never accepts a node.
  ///
  /// AST nodes that do not support being replaced during a walk use visitation
  /// methods that return PreWalkAction/PostWalkAction. These just store the
  /// walking action to perform, and you return e.g \c Action::Continue() or
  /// \c Action::SkipChildren().
  ///
  /// Each function here returns a separate underscored type, which is then
  /// consumed by PreWalkAction/PreWalkResult/PostWalkAction/PostWalkAction. It
  /// is designed this way to achieve a pseudo form of return type overloading,
  /// where e.g \c Action::Continue() can become either a pre-walk action or a
  /// post-walk action, but \c Action::SkipChildren() can only become a pre-walk
  /// action.
  struct Action {
    Action() = delete;

    /// Continue the current walk, replacing the current node with \p node.
    template <typename T>
    static _Detail::ContinueWalkResult<T> Continue(T node) {
      return {std::move(node)};
    }

    /// Continue the current walk, replacing the current node with \p node.
    /// However, skip visiting the children of \p node, and instead resume the
    /// walk of the parent node.
    template <typename T>
    static _Detail::SkipChildrenIfWalkResult<T> SkipChildren(T node) {
      return SkipChildrenIf(true, std::move(node));
    }

    /// If \p cond is true, this is equivalent to \c Action::SkipChildren(node).
    /// Otherwise, it is equivalent to \c Action::Continue(node).
    template <typename T>
    static _Detail::SkipChildrenIfWalkResult<T>
    SkipChildrenIf(bool cond, T node) {
      return {cond, std::move(node)};
    }

    /// If \p cond is true, this is equivalent to \c Action::Continue(node).
    /// Otherwise, it is equivalent to \c Action::SkipChildren(node).
    template <typename T>
    static _Detail::SkipChildrenIfWalkResult<T>
    VisitChildrenIf(bool cond, T node) {
      return SkipChildrenIf(!cond, std::move(node));
    }

    /// If \p cond is true, this is equivalent to \c Action::Stop().
    /// Otherwise, it is equivalent to \c Action::Continue(node).
    template <typename T>
    static _Detail::StopIfWalkResult<T> StopIf(bool cond, T node) {
      return {cond, std::move(node)};
    }

    /// Continue the current walk.
    static _Detail::ContinueWalkAction Continue() { return {}; }

    /// Continue the current walk, but do not visit the children of the current
    /// node. Instead, resume at the parent's post-walk.
    static _Detail::SkipChildrenIfWalkAction SkipChildren() {
      return SkipChildrenIf(true);
    }

    /// If \p cond is true, this is equivalent to \c Action::SkipChildren().
    /// Otherwise, it is equivalent to \c Action::Continue().
    static _Detail::SkipChildrenIfWalkAction SkipChildrenIf(bool cond) {
      return {cond};
    }

    /// If \p cond is true, this is equivalent to \c Action::Continue().
    /// Otherwise, it is equivalent to \c Action::SkipChildren().
    static _Detail::SkipChildrenIfWalkAction VisitChildrenIf(bool cond) {
      return SkipChildrenIf(!cond);
    }

    /// Terminate the walk, returning without visiting any other nodes.
    static _Detail::StopWalkAction Stop() { return {}; }

    /// If \p cond is true, this is equivalent to \c Action::Stop().
    /// Otherwise, it is equivalent to \c Action::Continue().
    static _Detail::StopIfWalkAction StopIf(bool cond) { return {cond}; }
  };

  /// Do not construct directly, use \c Action::<action> instead.
  ///
  /// A pre-visitation action for AST nodes that do not support being replaced
  /// while walking.
  struct PreWalkAction {
    enum Kind { Stop, SkipChildren, Continue };
    Kind Action;

    PreWalkAction(Kind action) : Action(action) {}

    PreWalkAction(_Detail::ContinueWalkAction) : Action(Continue) {}
    PreWalkAction(_Detail::StopWalkAction) : Action(Stop) {}

    PreWalkAction(_Detail::SkipChildrenIfWalkAction action)
        : Action(action.Cond ? SkipChildren : Continue) {}

    PreWalkAction(_Detail::StopIfWalkAction action)
        : Action(action.Cond ? Stop : Continue) {}
  };

  /// Do not construct directly, use \c Action::<action> instead.
  ///
  /// A post-visitation action for AST nodes that do not support being replaced
  /// while walking.
  struct PostWalkAction {
    enum Kind { Stop, Continue };
    Kind Action;

    PostWalkAction(Kind action) : Action(action) {}

    PostWalkAction(_Detail::ContinueWalkAction) : Action(Continue) {}
    PostWalkAction(_Detail::StopWalkAction) : Action(Stop) {}

    PostWalkAction(_Detail::StopIfWalkAction action)
        : Action(action.Cond ? Stop : Continue) {}
  };

  /// Do not construct directly, use \c Action::<action> instead.
  ///
  /// A pre-visitation result for AST nodes that support being replaced while
  /// walking. Stores both the walking action to take, along with the node to
  /// splice into the AST in place of the old node.
  template <typename T>
  struct PreWalkResult {
    PreWalkAction Action;
    llvm::Optional<T> Value;

    template <typename U,
              typename std::enable_if<std::is_convertible<U, T>::value>::type
                  * = nullptr>
    PreWalkResult(const PreWalkResult<U> &Other) : Action(Other.Action) {
      if (Other.Value)
        Value = *Other.Value;
    }

    template <typename U,
              typename std::enable_if<std::is_convertible<U, T>::value>::type
                  * = nullptr>
    PreWalkResult(PreWalkResult<U> &&Other) : Action(Other.Action) {
      if (Other.Value)
        Value = std::move(*Other.Value);
    }

    template <typename U>
    PreWalkResult(_Detail::ContinueWalkResult<U> Result)
        : Action(PreWalkAction::Continue), Value(std::move(Result.Value)) {}

    template <typename U>
    PreWalkResult(_Detail::SkipChildrenIfWalkResult<U> Result)
        : Action(Result.Cond ? PreWalkAction::SkipChildren
                             : PreWalkAction::Continue),
          Value(std::move(Result.Value)) {}

    template <typename U>
    PreWalkResult(_Detail::StopIfWalkResult<U> Result)
        : Action(Result.Cond ? PreWalkAction::Stop : PreWalkAction::Continue),
          Value(std::move(Result.Value)) {}

    PreWalkResult(_Detail::StopWalkAction)
        : Action(PreWalkAction::Stop), Value(llvm::None) {}
  };

  /// Do not construct directly, use \c Action::<action> instead.
  ///
  /// A post-visitation result for AST nodes that support being replaced while
  /// walking. Stores both the walking action to take, along with the node to
  /// splice into the AST in place of the old node.
  template <typename T>
  struct PostWalkResult {
    PostWalkAction Action;
    llvm::Optional<T> Value;

    template <typename U,
              typename std::enable_if<std::is_convertible<U, T>::value>::type
                  * = nullptr>
    PostWalkResult(const PostWalkResult<U> &Other) : Action(Other.Action) {
      if (Other.Value)
        Value = *Other.Value;
    }

    template <typename U,
              typename std::enable_if<std::is_convertible<U, T>::value>::type
                  * = nullptr>
    PostWalkResult(PostWalkResult<U> &&Other) : Action(Other.Action) {
      if (Other.Value)
        Value = std::move(*Other.Value);
    }

    template <typename U>
    PostWalkResult(_Detail::ContinueWalkResult<U> Result)
        : Action(PostWalkAction::Continue), Value(std::move(Result.Value)) {}

    template <typename U>
    PostWalkResult(_Detail::StopIfWalkResult<U> Result)
        : Action(Result.Cond ? PostWalkAction::Stop : PostWalkAction::Continue),
          Value(std::move(Result.Value)) {}

    PostWalkResult(_Detail::StopWalkAction)
        : Action(PostWalkAction::Stop), Value(llvm::None) {}
  };

  /// This method is called when first visiting an expression
  /// before walking into its children.
  ///
  /// \param E The expression to check.
  ///
  /// \returns A result that contains a potentially re-written expression,
  /// along with the walk action to perform. The default implementation
  /// returns \c Action::Continue(E).
  /// 
  virtual PreWalkResult<Expr *> walkToExprPre(Expr *E) {
    return Action::Continue(E);
  }

  /// This method is called after visiting an expression's children. If a new
  /// expression is returned, it is spliced in where the old expression
  /// previously appeared.
  ///
  /// \param E The expression that was walked.
  ///
  /// \returns A result that contains a potentially re-written expression,
  /// along with the walk action to perform. The default implementation
  /// returns \c Action::Continue(E).
  ///
  virtual PostWalkResult<Expr *> walkToExprPost(Expr *E) {
    return Action::Continue(E);
  }

  /// This method is called when first visiting a statement before
  /// walking into its children.
  ///
  /// \param S The statement to check.
  ///
  /// \returns A result that contains a potentially re-written statement,
  /// along with the walk action to perform. The default implementation
  /// returns \c Action::Continue(S).
  ///
  virtual PreWalkResult<Stmt *> walkToStmtPre(Stmt *S) {
    return Action::Continue(S);
  }

  /// This method is called after visiting an statements's children. If a new
  /// statement is returned, it is spliced in where the old statement
  /// previously appeared.
  ///
  /// \param S The statement that was walked.
  ///
  /// \returns A result that contains a potentially re-written statement,
  /// along with the walk action to perform. The default implementation
  /// returns \c Action::Continue(S).
  ///
  virtual PostWalkResult<Stmt *> walkToStmtPost(Stmt *S) {
    return Action::Continue(S);
  }

  /// This method is called when first visiting a pattern before walking into
  /// its children.
  ///
  /// \param P The statement to check.
  ///
  /// \returns A result that contains a potentially re-written pattern,
  /// along with the walk action to perform. The default implementation
  /// returns \c Action::Continue(P).
  ///
  virtual PreWalkResult<Pattern *> walkToPatternPre(Pattern *P) {
    return Action::Continue(P);
  }

  /// This method is called after visiting an pattern's children. If a new
  /// pattern is returned, it is spliced in where the old pattern
  /// previously appeared.
  ///
  /// \param P The pattern that was walked.
  ///
  /// \returns A result that contains a potentially re-written pattern,
  /// along with the walk action to perform. The default implementation
  /// returns \c Action::Continue(P).
  ///
  virtual PostWalkResult<Pattern *> walkToPatternPost(Pattern *P) {
    return Action::Continue(P);
  }

  /// walkToDeclPre - This method is called when first visiting a decl, before
  /// walking into its children.
  ///
  /// \param D The declaration to check. The callee may update this declaration
  /// in-place.
  ///
  /// \returns The walking action to perform. By default, this
  /// is \c Action::Continue().
  ///
  virtual PreWalkAction walkToDeclPre(Decl *D) { return Action::Continue(); }

  /// walkToDeclPost - This method is called after visiting the children of a
  /// decl.
  ///
  /// \param D The declaration that was walked.
  ///
  /// \returns The walking action to perform. By default, this
  /// is \c Action::Continue().
  ///
  virtual PostWalkAction walkToDeclPost(Decl *D) { return Action::Continue(); }

  /// This method is called when first visiting a TypeRepr, before
  /// walking into its children.
  ///
  /// \param T The TypeRepr to check.
  ///
  /// \returns The walking action to perform. By default, this
  /// is \c Action::Continue().
  ///
  virtual PreWalkAction walkToTypeReprPre(TypeRepr *T) {
    return Action::Continue();
  }

  /// This method is called after visiting the children of a TypeRepr.
  ///
  /// \param T The type that was walked.
  ///
  /// \returns The walking action to perform. By default, this
  /// is \c Action::Continue().
  ///
  virtual PostWalkAction walkToTypeReprPost(TypeRepr *T) {
    return Action::Continue();
  }

  /// This method configures whether the walker should explore into the generic
  /// params in AbstractFunctionDecl and NominalTypeDecl.
  virtual bool shouldWalkIntoGenericParams() { return false; }

  /// This method configures how the walker should walk the initializers of
  /// lazy variables. These initializers are semantically different from other
  /// initializers in their context and so sometimes should be visited as part
  /// of the synthesized getter, or should not be visited at all.
  virtual LazyInitializerWalking getLazyInitializerWalkingBehavior() {
    return LazyInitializerWalking::InPatternBinding;
  }

  /// This method configures how the walker should walk into uses of macros.
  virtual MacroWalking getMacroWalkingBehavior() const {
    return MacroWalking::ArgumentsAndExpansion;
  }

  /// Determine whether we should walk macro arguments (as they appear in
  /// source) and the expansion (which is semantically part of the program).
  std::pair<bool, bool> shouldWalkMacroArgumentsAndExpansion() const {
    switch (getMacroWalkingBehavior()) {
    case MacroWalking::Expansion:
      return std::make_pair(false, true);

    case MacroWalking::Arguments:
      return std::make_pair(true, false);

    case MacroWalking::ArgumentsAndExpansion:
      return std::make_pair(true, true);

    case MacroWalking::None:
      return std::make_pair(false, false);
    }
  }

  /// This method configures whether the walker should visit the body of a
  /// closure that was checked separately from its enclosing expression.
  ///
  /// For work that is performed for every top-level expression, this should
  /// be overridden to return false, to avoid duplicating work or visiting
  /// bodies of closures that have not yet been type checked.
  virtual bool shouldWalkIntoSeparatelyCheckedClosure(ClosureExpr *) {
    return true;
  }

  /// This method configures whether the walker should visit the body of a
  /// TapExpr.
  virtual bool shouldWalkIntoTapExpression() { return true; }

  /// This method configures whether the walker should visit the underlying
  /// value of a property wrapper placeholder.
  virtual bool shouldWalkIntoPropertyWrapperPlaceholderValue() { return true; }

  /// This method configures whether the walker should visit the capture
  /// initializer expressions within a capture list directly, rather than
  /// walking the declarations.
  virtual bool shouldWalkCaptureInitializerExpressions() { return false; }

  /// This method configures whether the walker should exhibit the legacy
  /// behavior where accessors appear as peers of their storage, rather
  /// than children nested inside of it.
  ///
  /// Please don't write new ASTWalker implementations that override this
  /// method to return true; instead, refactor existing code as needed
  /// until eventually we can remove this altogether.
  virtual bool shouldWalkAccessorsTheOldWay() { return false; }

  /// Whether to walk internal top level decls in serialized modules.
  ///
  /// TODO: Consider changing this to false by default.
  virtual bool shouldWalkSerializedTopLevelInternalDecls() { return true; }

  /// Whether to walk into the definition of a \c MacroDecl if it hasn't been
  /// type-checked yet.
  virtual bool shouldWalkIntoUncheckedMacroDefinitions() { return false; }

  /// walkToParameterListPre - This method is called when first visiting a
  /// ParameterList, before walking into its parameters.
  ///
  /// \param PL The parameter list to walk.
  ///
  /// \returns The walking action to perform. By default, this
  /// is \c Action::Continue().
  ///
  virtual PreWalkAction walkToParameterListPre(ParameterList *PL) {
    return Action::Continue();
  }

  /// walkToParameterListPost - This method is called after visiting the
  /// children of a parameter list.
  ///
  /// \param PL The parameter list that was walked.
  ///
  /// \returns The walking action to perform. By default, this
  /// is \c Action::Continue().
  ///
  virtual PostWalkAction walkToParameterListPost(ParameterList *PL) {
    return Action::Continue();
  }

  /// This method is called when first visiting an argument list before walking
  /// into its arguments.
  ///
  /// \param ArgList The argument list to walk.
  ///
  /// \returns A result that contains a potentially re-written argument list,
  /// along with the walk action to perform.
  ///
  /// The default implementation returns \c Action::Continue(ArgList).
  virtual PreWalkResult<ArgumentList *>
  walkToArgumentListPre(ArgumentList *ArgList) {
    return Action::Continue(ArgList);
  }

  /// This method is called after visiting the arguments in an argument list.
  /// If a new argument list is returned, it is spliced in where the old
  /// argument list previously appeared.
  ///
  /// The default implementation returns \c Action::Continue(ArgList).
  virtual PostWalkResult<ArgumentList *>
  walkToArgumentListPost(ArgumentList *ArgList) {
    return Action::Continue(ArgList);
  }

protected:
  ASTWalker() = default;
  ASTWalker(const ASTWalker &) = default;
  virtual ~ASTWalker() = default;

  virtual void anchor();
};

} // end namespace swift

#endif
