//===--- IDETypeChecking.h - Type-check entry points ------------*- C++ -*-===//
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
/// \file
/// Provides extra type-checking entry points for use during code
/// completion, which happens *without* type-checking an entire file at once.
//
//===----------------------------------------------------------------------===//

#ifndef SWIFT_SEMA_IDETYPECHECKING_H
#define SWIFT_SEMA_IDETYPECHECKING_H

#include "swift/AST/ASTNode.h"
#include "swift/AST/Identifier.h"
#include "swift/AST/TypeCheckRequests.h"
#include "swift/Basic/SourceLoc.h"
#include <memory>
#include <tuple>

namespace swift {
  class AbstractFunctionDecl;
  class ASTContext;
  class CaptureListExpr;
  class ConcreteDeclRef;
  class Decl;
  class DeclContext;
  class DeclName;
  enum class DeclRefKind;
  class Expr;
  class ExtensionDecl;
  class FreestandingMacroExpansion;
  class FunctionType;
  class LabeledConditionalStmt;
  class LookupResult;
  class NominalTypeDecl;
  class PatternBindingDecl;
  class ProtocolDecl;
  class SourceFile;
  class SubscriptDecl;
  class TopLevelCodeDecl;
  class Type;
  class ValueDecl;
  struct PrintOptions;

  namespace constraints {
  class ConstraintSystem;
  class Solution;
  class SyntacticElementTarget;
  }

  /// Typecheck binding initializer at \p bindingIndex.
  void typeCheckPatternBinding(PatternBindingDecl *PBD, unsigned bindingIndex,
                               bool leaveClosureBodiesUnchecked);

  /// Check if T1 is convertible to T2.
  ///
  /// \returns true on convertible, false on not.
  bool isConvertibleTo(Type T1, Type T2, bool openArchetypes, DeclContext &DC);

  void collectDefaultImplementationForProtocolMembers(ProtocolDecl *PD,
                        llvm::SmallDenseMap<ValueDecl*, ValueDecl*> &DefaultMap);

  enum InterestedMemberKind : uint8_t {
    Viable,
    Unviable,
    All,
  };

  struct ResolvedMemberResult {
    struct Implementation;
    Implementation *Impl;

    ResolvedMemberResult();
    ~ResolvedMemberResult();
    ResolvedMemberResult(const ResolvedMemberResult &) = delete;
    ResolvedMemberResult & operator=(ResolvedMemberResult &) = delete;
    ResolvedMemberResult(ResolvedMemberResult &&other) {
      Impl = other.Impl;
      other.Impl = nullptr;
    }
    operator bool() const;
    bool hasBestOverload() const;
    ValueDecl* getBestOverload() const;
    ArrayRef<ValueDecl*> getMemberDecls(InterestedMemberKind Kind);
  };

  /// Look up a member with the given name in the given type.
  ///
  /// Unlike other member lookup functions, \c swift::resolveValueMember()
  /// should be used when you want to look up declarations with the same name as
  /// one you already have.
  ResolvedMemberResult resolveValueMember(DeclContext &DC, Type BaseTy,
                                          DeclName Name);

  /// Given a type and an extension to the original type decl of that type,
  /// decide if the extension has been applied, i.e. if the requirements of the
  /// extension have been fulfilled.
  /// \returns True on applied, false on not applied.
  bool isExtensionApplied(const DeclContext *DC, Type Ty,
                          const ExtensionDecl *ED);

  /// Given a type and an member value decl , decide if the decl is applied,
  /// i.e. if the \c where requirements of the decl have been fulfilled.
  /// \returns True on applied, false on not applied.
  bool isMemberDeclApplied(const DeclContext *DC, Type Ty, const ValueDecl *VD);

  /// The kind of type checking to perform for code completion.
  enum class CompletionTypeCheckKind {
    /// Type check the expression as normal.
    Normal,

    /// Type check the argument to an Objective-C #keyPath.
    KeyPath,
  };

  /// Return the type of an expression parsed during code completion, or
  /// None on error.
  Optional<Type> getTypeOfCompletionContextExpr(
                   ASTContext &Ctx,
                   DeclContext *DC,
                   CompletionTypeCheckKind kind,
                   Expr *&parsedExpr,
                   ConcreteDeclRef &referencedDecl);

  /// Resolve type of operator function with \c opName appending it to \c LHS.
  ///
  /// For \p refKind, use \c DeclRefKind::PostfixOperator for postfix operator,
  /// or \c DeclRefKind::BinaryOperator for infix operator.
  /// On success, returns resolved function type of the operator. The LHS should
  /// already be type-checked. This function guarantees LHS not to be modified.
  FunctionType *getTypeOfCompletionOperator(DeclContext *DC, Expr *LHS,
                                            Identifier opName,
                                            DeclRefKind refKind,
                                            ConcreteDeclRef &referencedDecl);

  /// Typecheck the given expression.
  bool typeCheckExpression(DeclContext *DC, Expr *&parsedExpr);

  /// Type check a function body element which is at \p TagetLoc.
  bool typeCheckASTNodeAtLoc(TypeCheckASTNodeAtLocContext TypeCheckCtx,
                             SourceLoc TargetLoc);

  /// Thunk around \c TypeChecker::typeCheckForCodeCompletion to make it
  /// available to \c swift::ide.
  /// Type check the given expression and provide results back to code
  /// completion via specified callback.
  ///
  /// This method is designed to be used for code completion which means that
  /// it doesn't mutate given expression, even if there is a single valid
  /// solution, and constraint solver is allowed to produce partially correct
  /// solutions. Such solutions can have any number of holes in them.
  ///
  /// \returns `true` if target was applicable and it was possible to infer
  /// types for code completion, `false` otherwise.
  bool typeCheckForCodeCompletion(
      constraints::SyntacticElementTarget &target, bool needsPrecheck,
      llvm::function_ref<void(const constraints::Solution &)> callback);

  /// Thunk around \c TypeChecker::resolveDeclRefExpr to make it available to
  /// \c swift::ide
  Expr *resolveDeclRefExpr(UnresolvedDeclRefExpr *UDRE, DeclContext *Context,
                         bool replaceInvalidRefsWithErrors);

  LookupResult
  lookupSemanticMember(DeclContext *DC, Type ty, DeclName name);

  /// Get all of the top-level declarations that should be printed as part of
  /// this module. This may force synthesis of top-level declarations that
  /// \c ModuleDecl::getDisplayDecls() would only return if previous
  /// work happened to have synthesized them.
  void
  getTopLevelDeclsForDisplay(ModuleDecl *M, SmallVectorImpl<Decl*> &Results, bool Recursive = false);

  struct ExtensionInfo {
    // The extension with the declarations to apply.
    ExtensionDecl *Ext;
    // The extension that enables the former to apply, if any (i.e. a
    // conditional
    // conformance to Foo enables 'extension Foo').
    ExtensionDecl *EnablingExt;
    bool IsSynthesized;
  };

  using ExtensionGroupOperation =
      llvm::function_ref<void(ArrayRef<ExtensionInfo>)>;

  class SynthesizedExtensionAnalyzer {
    struct Implementation;
    Implementation &Impl;
  public:
    SynthesizedExtensionAnalyzer(NominalTypeDecl *Target,
                                 PrintOptions Options,
                                 bool IncludeUnconditional = true);
    ~SynthesizedExtensionAnalyzer();

    enum class MergeGroupKind : char {
      All,
      MergeableWithTypeDef,
      UnmergeableWithTypeDef,
    };

    void forEachExtensionMergeGroup(MergeGroupKind Kind,
                                    ExtensionGroupOperation Fn);
    bool isInSynthesizedExtension(const ValueDecl *VD);
    bool shouldPrintRequirement(ExtensionDecl *ED, StringRef Req);
    bool hasMergeGroup(MergeGroupKind Kind);
  };

  /// Reported type for an expression. This expression is represented by offset
  /// length in the source buffer;
  struct ExpressionTypeInfo {

    /// The start of the expression;
    uint32_t offset;

    /// The length of the expression;
    uint32_t length;

    /// The start of the printed type in a separately given string buffer.
    uint32_t typeOffset;

    /// The length of the printed type
    uint32_t typeLength;

    /// The offsets and lengths of all protocols the type conforms to
    std::vector<std::pair<uint32_t, uint32_t>> protocols;
  };

  /// Collect type information for every expression in \c SF; all types will
  /// be printed to \c OS.
  ArrayRef<ExpressionTypeInfo> collectExpressionType(
      SourceFile &SF, ArrayRef<const char *> ExpectedProtocols,
      std::vector<ExpressionTypeInfo> &scratch, bool FullyQualified,
      bool CanonicalType, llvm::raw_ostream &OS);

  /// Resolve a list of mangled names to accessible protocol decls from
  /// the decl context.
  ProtocolDecl *resolveProtocolName(DeclContext *dc, StringRef Name);

  /// Reported type of a variable declaration.
  struct VariableTypeInfo {
    /// The start of the variable identifier.
    uint32_t Offset;

    /// The length of the variable identifier.
    uint32_t Length;

    /// Whether the variable has an explicit type annotation.
    bool HasExplicitType;

    /// The start of the printed type in a separate string buffer.
    uint32_t TypeOffset;

    VariableTypeInfo(uint32_t Offset, uint32_t Length, bool HasExplicitType,
                     uint32_t TypeOffset);
  };

  /// Collect type information for every variable declaration in \c SF
  /// within the given range into \c VariableTypeInfos.
  /// All types will be printed to \c OS and the type offsets of the
  /// \c VariableTypeInfos will index into the string that backs this
  /// stream.
  void collectVariableType(SourceFile &SF, SourceRange Range,
                           bool FullyQualified,
                           std::vector<VariableTypeInfo> &VariableTypeInfos,
                           llvm::raw_ostream &OS);

  /// Returns the root type and result type of the keypath type in a keypath
  /// dynamic member lookup subscript, or \c None if it cannot be determined.
  ///
  /// \param subscript The potential keypath dynamic member lookup subscript.
  Type getRootTypeOfKeypathDynamicMember(SubscriptDecl *subscript);

  Type getResultTypeOfKeypathDynamicMember(SubscriptDecl *subscript);

  /// Collect all the protocol requirements that a given declaration can
  ///   provide default implementations for. VD is a declaration in extension
  ///   declaration. Scratch is the buffer to collect those protocol
  ///   requirements.
  ///
  /// \returns the slice of Scratch
  ArrayRef<ValueDecl*>
  canDeclProvideDefaultImplementationFor(ValueDecl* VD);

  /// Get decls that the given decl overrides, protocol requirements that
  ///   it serves as a default implementation of, and optionally protocol
  ///   requirements it satisfies in a conforming class
  ArrayRef<ValueDecl*>
  collectAllOverriddenDecls(ValueDecl *VD,
                            bool IncludeProtocolRequirements = true,
                            bool Transitive = false);

  /// Enumerates the various kinds of "build" functions within a result
  /// builder.
  enum class ResultBuilderBuildFunction {
    BuildBlock,
    BuildExpression,
    BuildOptional,
    BuildEitherFirst,
    BuildEitherSecond,
    BuildArray,
    BuildLimitedAvailability,
    BuildFinalResult,
    BuildPartialBlockFirst,
    BuildPartialBlockAccumulated,
  };

  /// Try to infer the component type of a result builder from the type
  /// of buildBlock or buildExpression, if it was there.
  Type inferResultBuilderComponentType(NominalTypeDecl *builder);

  /// Print the declaration for a result builder "build" function, for use
  /// in Fix-Its, code completion, and so on.
  void printResultBuilderBuildFunction(
      NominalTypeDecl *builder, Type componentType,
      ResultBuilderBuildFunction function,
      Optional<std::string> stubIndent, llvm::raw_ostream &out);

  /// Compute the insertion location, indentation string, and component type
  /// for a Fix-It that adds a new build* function to a result builder.
  std::tuple<SourceLoc, std::string, Type>
  determineResultBuilderBuildFixItInfo(NominalTypeDecl *builder);

  /// Just a proxy to swift::contextUsesConcurrencyFeatures() from lib/IDE code.
  bool completionContextUsesConcurrencyFeatures(const DeclContext *dc);

  /// Determine the isolation of a particular closure.
  ClosureActorIsolation determineClosureActorIsolation(
      AbstractClosureExpr *closure, llvm::function_ref<Type(Expr *)> getType,
      llvm::function_ref<ClosureActorIsolation(AbstractClosureExpr *)>
          getClosureActorIsolation);

  /// If the capture list shadows any declarations using shorthand syntax, i.e.
  /// syntax that names both the newly declared variable and the referenced
  /// variable by the same identifier in the source text, i.e. `[foo]`, return
  /// these shorthand shadows.
  /// The first element in the pair is the implicitly declared variable and the
  /// second variable is the shadowed one.
  /// If a \c DeclContext is passed, it is used to resolve any
  /// \c UnresolvedDeclRef that a shorthand shadow may refer to.
  SmallVector<std::pair<ValueDecl *, ValueDecl *>, 1>
  getShorthandShadows(CaptureListExpr *CaptureList, DeclContext *DC = nullptr);

  /// Same as above but for shorthand `if let foo {` syntax.
  /// If a \c DeclContext is passed, it is used to resolve any
  /// \c UnresolvedDeclRef that a shorthand shadow may refer to.
  SmallVector<std::pair<ValueDecl *, ValueDecl *>, 1>
  getShorthandShadows(LabeledConditionalStmt *CondStmt,
                      DeclContext *DC = nullptr);

  SourceFile *evaluateFreestandingMacro(FreestandingMacroExpansion *expansion,
                                        StringRef discriminator);

  SourceFile *evaluateAttachedMacro(MacroDecl *macro, Decl *attachedTo,
                                    CustomAttr *attr, bool passParentContext,
                                    MacroRole role, StringRef discriminator);
}

#endif
