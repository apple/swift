//===--- CompletionLookup.h -----------------------------------------------===//
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

#ifndef SWIFT_IDE_COMPLETIONLOOKUP_H
#define SWIFT_IDE_COMPLETIONLOOKUP_H

#include "swift/AST/ASTContext.h"
#include "swift/AST/ASTPrinter.h"
#include "swift/AST/ASTWalker.h"
#include "swift/AST/Expr.h"
#include "swift/AST/ImportCache.h"
#include "swift/AST/Initializer.h"
#include "swift/AST/NameLookup.h"
#include "swift/AST/ProtocolConformance.h"
#include "swift/ClangImporter/ClangImporter.h"
#include "swift/IDE/CodeCompletionContext.h"
#include "swift/IDE/CodeCompletionResult.h"
#include "swift/IDE/CodeCompletionStringPrinter.h"
#include "swift/IDE/PossibleParamInfo.h"
#include "swift/Parse/IDEInspectionCallbacks.h"
#include "swift/Sema/IDETypeChecking.h"
#include "swift/Strings.h"

namespace swift {
namespace ide {

using DeclFilter =
    std::function<bool(ValueDecl *, DeclVisibilityKind, DynamicLookupInfo)>;

/// A filter that always returns \c true.
bool DefaultFilter(ValueDecl *VD, DeclVisibilityKind Kind,
                   DynamicLookupInfo dynamicLookupInfo);

bool KeyPathFilter(ValueDecl *decl, DeclVisibilityKind,
                   DynamicLookupInfo dynamicLookupInfo);

bool MacroFilter(ValueDecl *decl, DeclVisibilityKind,
                 DynamicLookupInfo dynamicLookupInfo);

/// Returns \c true only if the completion is happening for top-level
/// declrarations. i.e.:
///
///     if condition {
///       #false#
///     }
///     expr.#false#
///
///     #true#
///
///     struct S {
///       #false#
///       func foo() {
///         #false#
///       }
///     }
bool isCodeCompletionAtTopLevel(const DeclContext *DC);

/// Returns \c true if the completion is happening in local context such as
/// inside function bodies. i.e.:
///
///     if condition {
///       #true#
///     }
///     expr.#true#
///
///     #false#
///
///     struct S {
///       #false#
///       func foo() {
///         #true#
///       }
///     }
bool isCompletionDeclContextLocalContext(DeclContext *DC);

/// Returns \c true if \p DC can handles async call.
bool canDeclContextHandleAsync(const DeclContext *DC);

/// Return \c true if the completion happens at top-level of a library file.
bool isCodeCompletionAtTopLevelOfLibraryFile(const DeclContext *DC);

/// Build completions by doing visible decl lookup from a context.
class CompletionLookup final : public swift::VisibleDeclConsumer {
  CodeCompletionResultSink &Sink;
  ASTContext &Ctx;
  const DeclContext *CurrDeclContext;
  ModuleDecl *CurrModule;
  ClangImporter *Importer;
  CodeCompletionContext *CompletionContext;

  enum class LookupKind {
    ValueExpr,
    ValueInDeclContext,
    EnumElement,
    Type,
    TypeInDeclContext,
    ImportFromModule,
    GenericRequirement,
  };

  LookupKind Kind;

  /// Type of the user-provided expression for LookupKind::ValueExpr
  /// completions.
  Type ExprType;

  /// Whether the expr is of statically inferred metatype.
  bool IsStaticMetatype = false;

  /// User-provided base type for LookupKind::Type completions.
  Type BaseType;

  /// Expected types of the code completion expression.
  ExpectedTypeContext expectedTypeContext;

  /// Variables whose type was determined while type checking the code
  /// completion expression and that are thus not recorded in the AST.
  /// This in particular applies to params of closures that contain the code
  /// completion token.
  llvm::SmallDenseMap<const VarDecl *, Type> SolutionSpecificVarTypes;

  bool CanCurrDeclContextHandleAsync = false;
  /// Actor isolations that were determined during constraint solving but that
  /// haven't been saved to the AST.
  llvm::DenseMap<AbstractClosureExpr *, ClosureActorIsolation>
      ClosureActorIsolations;
  bool HaveDot = false;
  bool IsUnwrappedOptional = false;
  SourceLoc DotLoc;
  bool NeedLeadingDot = false;

  bool NeedOptionalUnwrap = false;
  unsigned NumBytesToEraseForOptionalUnwrap = 0;

  bool HaveLParen = false;
  bool IsSuperRefExpr = false;
  bool IsSelfRefExpr = false;
  bool IsKeyPathExpr = false;
  bool IsSwiftKeyPathExpr = false;
  bool IsAfterSwiftKeyPathRoot = false;
  bool IsDynamicLookup = false;
  bool IsCrossActorReference = false;
  bool PreferFunctionReferencesToCalls = false;
  bool HaveLeadingSpace = false;

  bool CheckForDuplicates = false;
  llvm::DenseSet<std::pair<const Decl *, Type>> PreviouslySeen;

  bool IncludeInstanceMembers = false;

  /// True if we are code completing inside a static method.
  bool InsideStaticMethod = false;

  /// Innermost method that the code completion point is in.
  const AbstractFunctionDecl *CurrentMethod = nullptr;

  llvm::Optional<SemanticContextKind> ForcedSemanticContext = llvm::None;
  bool IsUnresolvedMember = false;

public:
  bool FoundFunctionCalls = false;
  bool FoundFunctionsWithoutFirstKeyword = false;

private:
  bool isForCaching() const { return Kind == LookupKind::ImportFromModule; }

  void foundFunction(const AbstractFunctionDecl *AFD);

  void foundFunction(const AnyFunctionType *AFT);

  /// Returns \c true if \p TAD is usable as a first type of a requirement in
  /// \c where clause for a context.
  /// \p selfTy must be a \c Self type of the context.
  static bool canBeUsedAsRequirementFirstType(Type selfTy, TypeAliasDecl *TAD);

public:
  struct RequestedResultsTy {
    const ModuleDecl *TheModule;
    CodeCompletionFilter Filter;
    bool NeedLeadingDot;

    static RequestedResultsTy fromModule(const ModuleDecl *mod,
                                         CodeCompletionFilter filter) {
      return {mod, filter, /*NeedLeadingDot=*/false};
    }

    static RequestedResultsTy topLevelResults(CodeCompletionFilter filter) {
      return {nullptr, filter, /*NeedLeadingDot=*/false};
    }

    RequestedResultsTy needLeadingDot(bool needDot) const {
      return {TheModule, Filter, needDot};
    }

    friend bool operator==(const RequestedResultsTy &LHS,
                           const RequestedResultsTy &RHS) {
      return LHS.TheModule == RHS.TheModule &&
             LHS.Filter.containsOnly(RHS.Filter) &&
             LHS.NeedLeadingDot == RHS.NeedLeadingDot;
    }
  };

  llvm::SetVector<RequestedResultsTy> RequestedCachedResults;

public:
  CompletionLookup(CodeCompletionResultSink &Sink, ASTContext &Ctx,
                   const DeclContext *CurrDeclContext,
                   CodeCompletionContext *CompletionContext = nullptr);

  void setSolutionSpecificVarTypes(
      llvm::SmallDenseMap<const VarDecl *, Type> SolutionSpecificVarTypes) {
    this->SolutionSpecificVarTypes = SolutionSpecificVarTypes;
  }

  void setHaveDot(SourceLoc DotLoc) {
    HaveDot = true;
    this->DotLoc = DotLoc;
  }

  void setIsUnwrappedOptional(bool value) { IsUnwrappedOptional = value; }

  void setIsStaticMetatype(bool value) { IsStaticMetatype = value; }

  void setExpectedTypes(
      ArrayRef<Type> Types, bool isImplicitSingleExpressionReturn,
      bool preferNonVoid = false,
      OptionSet<CustomAttributeKind> expectedCustomAttributeKinds = {}) {
    expectedTypeContext.setIsImplicitSingleExpressionReturn(
        isImplicitSingleExpressionReturn);
    expectedTypeContext.setPreferNonVoid(preferNonVoid);
    expectedTypeContext.setPossibleTypes(Types);
    expectedTypeContext.setExpectedCustomAttributeKinds(
        expectedCustomAttributeKinds);
  }

  void setIdealExpectedType(Type Ty) { expectedTypeContext.setIdealType(Ty); }

  bool canCurrDeclContextHandleAsync() const {
    return CanCurrDeclContextHandleAsync;
  }

  void setCanCurrDeclContextHandleAsync(bool CanCurrDeclContextHandleAsync) {
    this->CanCurrDeclContextHandleAsync = CanCurrDeclContextHandleAsync;
  }

  void setClosureActorIsolations(
      llvm::DenseMap<AbstractClosureExpr *, ClosureActorIsolation>
          ClosureActorIsolations) {
    this->ClosureActorIsolations = ClosureActorIsolations;
  }

  const ExpectedTypeContext *getExpectedTypeContext() const {
    return &expectedTypeContext;
  }

  CodeCompletionContext::TypeContextKind typeContextKind() const {
    if (expectedTypeContext.empty() &&
        !expectedTypeContext.getPreferNonVoid()) {
      return CodeCompletionContext::TypeContextKind::None;
    } else if (expectedTypeContext.isImplicitSingleExpressionReturn()) {
      return CodeCompletionContext::TypeContextKind::SingleExpressionBody;
    } else {
      return CodeCompletionContext::TypeContextKind::Required;
    }
  }

  bool needDot() const { return NeedLeadingDot; }

  void setHaveLParen(bool Value) { HaveLParen = Value; }

  void setIsSuperRefExpr(bool Value = true) { IsSuperRefExpr = Value; }

  void setIsSelfRefExpr(bool value) { IsSelfRefExpr = value; }

  void setIsKeyPathExpr() { IsKeyPathExpr = true; }

  void shouldCheckForDuplicates(bool value = true) {
    CheckForDuplicates = value;
  }

  void setIsSwiftKeyPathExpr(bool onRoot) {
    IsSwiftKeyPathExpr = true;
    IsAfterSwiftKeyPathRoot = onRoot;
  }

  void setIsDynamicLookup() { IsDynamicLookup = true; }

  void setPreferFunctionReferencesToCalls() {
    PreferFunctionReferencesToCalls = true;
  }

  void setHaveLeadingSpace(bool value) { HaveLeadingSpace = value; }

  void includeInstanceMembers() { IncludeInstanceMembers = true; }

  bool isHiddenModuleName(Identifier Name) {
    return (Name.str().startswith("_") || Name == Ctx.SwiftShimsModuleName ||
            Name.str() == SWIFT_ONONE_SUPPORT);
  }

  void addSubModuleNames(
      std::vector<std::pair<std::string, bool>> &SubModuleNameVisibilityPairs);

  void collectImportedModules(llvm::StringSet<> &directImportedModules,
                              llvm::StringSet<> &allImportedModules);

  void
  addModuleName(ModuleDecl *MD,
                llvm::Optional<ContextualNotRecommendedReason> R = llvm::None);

  void addImportModuleNames();

  SemanticContextKind getSemanticContext(const Decl *D,
                                         DeclVisibilityKind Reason,
                                         DynamicLookupInfo dynamicLookupInfo);

  bool isUnresolvedMemberIdealType(Type Ty);

  /// Creates a \c CodeCompletionResultBuilder in this lookup’s sink and sets
  /// the current expected type context in it
  CodeCompletionResultBuilder
  makeResultBuilder(CodeCompletionResultKind kind,
                    SemanticContextKind semanticContext) const;

  void addValueBaseName(CodeCompletionResultBuilder &Builder,
                        DeclBaseName Name);

  void addLeadingDot(CodeCompletionResultBuilder &Builder);

  void addTypeAnnotation(CodeCompletionResultBuilder &Builder, Type T,
                         GenericSignature genericSig = GenericSignature());

  void addTypeAnnotationForImplicitlyUnwrappedOptional(
      CodeCompletionResultBuilder &Builder, Type T,
      GenericSignature genericSig = GenericSignature(),
      bool dynamicOrOptional = false);

  /// For printing in code completion results, replace archetypes with
  /// protocol compositions.
  ///
  /// FIXME: Perhaps this should be an option in PrintOptions instead.
  Type eraseArchetypes(Type type, GenericSignature genericSig);

  Type getTypeOfMember(const ValueDecl *VD,
                       DynamicLookupInfo dynamicLookupInfo);

  Type getTypeOfMember(const ValueDecl *VD, Type ExprType);

  Type getAssociatedTypeType(const AssociatedTypeDecl *ATD);

  void analyzeActorIsolation(
      const ValueDecl *VD, Type T, bool &implicitlyAsync,
      llvm::Optional<ContextualNotRecommendedReason> &NotRecommended);

  void addVarDeclRef(const VarDecl *VD, DeclVisibilityKind Reason,
                     DynamicLookupInfo dynamicLookupInfo);

  static bool hasInterestingDefaultValue(const ParamDecl *param);

  bool shouldAddItemWithoutDefaultArgs(const AbstractFunctionDecl *func);

  /// Build argument patterns for calling. Returns \c true if any content was
  /// added to \p Builder. If \p declParams is non-empty, the size must match
  /// with \p typeParams.
  bool addCallArgumentPatterns(CodeCompletionResultBuilder &Builder,
                               ArrayRef<AnyFunctionType::Param> typeParams,
                               ArrayRef<const ParamDecl *> declParams,
                               GenericSignature genericSig,
                               bool includeDefaultArgs = true);

  /// Build argument patterns for calling. Returns \c true if any content was
  /// added to \p Builder. If \p Params is non-nullptr, \F
  bool addCallArgumentPatterns(CodeCompletionResultBuilder &Builder,
                               const AnyFunctionType *AFT,
                               const ParameterList *Params,
                               GenericSignature genericSig,
                               bool includeDefaultArgs = true);

  static void addEffectsSpecifiers(CodeCompletionResultBuilder &Builder,
                                   const AnyFunctionType *AFT,
                                   const AbstractFunctionDecl *AFD,
                                   bool forceAsync = false);

  void addPoundAvailable(llvm::Optional<StmtKind> ParentKind);

  void addPoundSelector(bool needPound);

  void addPoundKeyPath(bool needPound);

  SemanticContextKind getSemanticContextKind(const ValueDecl *VD);

  void addSubscriptCallPattern(
      const AnyFunctionType *AFT, const SubscriptDecl *SD,
      const llvm::Optional<SemanticContextKind> SemanticContext = llvm::None);

  void addFunctionCallPattern(
      const AnyFunctionType *AFT, const AbstractFunctionDecl *AFD = nullptr,
      const llvm::Optional<SemanticContextKind> SemanticContext = llvm::None);

  bool isImplicitlyCurriedInstanceMethod(const AbstractFunctionDecl *FD);

  void addMethodCall(const FuncDecl *FD, DeclVisibilityKind Reason,
                     DynamicLookupInfo dynamicLookupInfo);

  void addConstructorCall(const ConstructorDecl *CD, DeclVisibilityKind Reason,
                          DynamicLookupInfo dynamicLookupInfo,
                          llvm::Optional<Type> BaseType,
                          llvm::Optional<Type> Result, bool IsOnType = true,
                          Identifier addName = Identifier());

  void addConstructorCallsForType(Type type, Identifier name,
                                  DeclVisibilityKind Reason,
                                  DynamicLookupInfo dynamicLookupInfo);

  void addSubscriptCall(const SubscriptDecl *SD, DeclVisibilityKind Reason,
                        DynamicLookupInfo dynamicLookupInfo);

  void addNominalTypeRef(const NominalTypeDecl *NTD, DeclVisibilityKind Reason,
                         DynamicLookupInfo dynamicLookupInfo);

  void addTypeAliasRef(const TypeAliasDecl *TAD, DeclVisibilityKind Reason,
                       DynamicLookupInfo dynamicLookupInfo);

  void addGenericTypeParamRef(const GenericTypeParamDecl *GP,
                              DeclVisibilityKind Reason,
                              DynamicLookupInfo dynamicLookupInfo);

  void addAssociatedTypeRef(const AssociatedTypeDecl *AT,
                            DeclVisibilityKind Reason,
                            DynamicLookupInfo dynamicLookupInfo);

  void addPrecedenceGroupRef(PrecedenceGroupDecl *PGD);

  void addEnumElementRef(const EnumElementDecl *EED, DeclVisibilityKind Reason,
                         DynamicLookupInfo dynamicLookupInfo,
                         bool HasTypeContext);
  void addMacroExpansion(const MacroDecl *MD, DeclVisibilityKind Reason);

  void addKeyword(
      StringRef Name, Type TypeAnnotation = Type(),
      SemanticContextKind SK = SemanticContextKind::None,
      CodeCompletionKeywordKind KeyKind = CodeCompletionKeywordKind::None,
      unsigned NumBytesToErase = 0);

  void addKeyword(
      StringRef Name, StringRef TypeAnnotation,
      CodeCompletionKeywordKind KeyKind = CodeCompletionKeywordKind::None,
      CodeCompletionFlair flair = {});

  void addDeclAttrParamKeyword(StringRef Name, ArrayRef<StringRef> Parameters,
                               StringRef Annotation, bool NeedSpecify);

  void addDeclAttrKeyword(StringRef Name, StringRef Annotation);

  /// Add the compound function name for the given function.
  /// Returns \c true if the compound function name is actually used.
  bool addCompoundFunctionNameIfDesiable(AbstractFunctionDecl *AFD,
                                         DeclVisibilityKind Reason,
                                         DynamicLookupInfo dynamicLookupInfo);

private:
  /// Normalize the type for 'isDupelicate' check.
  Type normalizeTypeForDuplicationCheck(Type Ty);

  /// Returns true if duplicate checking is enabled (via
  /// \c shouldCheckForDuplicates) and this decl + type combination has been
  /// checked previously. Returns false otherwise.
  bool isDuplicate(const ValueDecl *D, Type Ty) {
    if (!CheckForDuplicates)
      return false;
    return !PreviouslySeen.insert({D, normalizeTypeForDuplicationCheck(Ty)})
                .second;
  }

  /// Returns true if duplicate checking is enabled (via
  /// \c shouldCheckForDuplicates) and this decl has been checked previously
  /// with the type according to \c getTypeOfMember. Returns false otherwise.
  bool isDuplicate(const ValueDecl *D, DynamicLookupInfo dynamicLookupInfo) {
    if (!CheckForDuplicates)
      return false;
    Type Ty = getTypeOfMember(D, dynamicLookupInfo);
    return !PreviouslySeen.insert({D, normalizeTypeForDuplicationCheck(Ty)})
                .second;
  }

public:
  // Implement swift::VisibleDeclConsumer.
  void foundDecl(ValueDecl *D, DeclVisibilityKind Reason,
                 DynamicLookupInfo dynamicLookupInfo) override;

  void onLookupNominalTypeMembers(NominalTypeDecl *NTD,
                                  DeclVisibilityKind Reason) override;

  bool handleEnumElement(ValueDecl *D, DeclVisibilityKind Reason,
                         DynamicLookupInfo dynamicLookupInfo);

  bool tryTupleExprCompletions(Type ExprType);

  bool tryFunctionCallCompletions(
      Type ExprType, const ValueDecl *VD,
      llvm::Optional<SemanticContextKind> SemanticContext = llvm::None);

  bool tryModuleCompletions(Type ExprType, CodeCompletionFilter Filter);

  /// If the given ExprType is optional, this adds completions for the unwrapped
  /// type.
  ///
  /// \return true if the given type was Optional .
  bool tryUnwrappedCompletions(Type ExprType, bool isIUO);

  void getPostfixKeywordCompletions(Type ExprType, Expr *ParsedExpr);

  void getValueExprCompletions(Type ExprType, ValueDecl *VD = nullptr);

  void collectOperators(SmallVectorImpl<OperatorDecl *> &results);

  void addPostfixBang(Type resultType);

  void addPostfixOperatorCompletion(OperatorDecl *op, Type resultType);

  void tryPostfixOperator(Expr *expr, PostfixOperatorDecl *op);

  void addAssignmentOperator(Type RHSType, Type resultType);

  void addInfixOperatorCompletion(OperatorDecl *op, Type resultType,
                                  Type RHSType);

  void tryInfixOperatorCompletion(Expr *foldedExpr, InfixOperatorDecl *op);

  Expr *typeCheckLeadingSequence(Expr *LHS, ArrayRef<Expr *> leadingSequence);

  void getOperatorCompletions(Expr *LHS, ArrayRef<Expr *> leadingSequence);

  void addTypeRelationFromProtocol(CodeCompletionResultBuilder &builder,
                                   CodeCompletionLiteralKind kind);

  void addValueLiteralCompletions();

  void addObjCPoundKeywordCompletions(bool needPound);

  void getMacroCompletions(CodeCompletionMacroRoles roles);

  struct FilteredDeclConsumer : public swift::VisibleDeclConsumer {
    swift::VisibleDeclConsumer &Consumer;
    DeclFilter Filter;
    FilteredDeclConsumer(swift::VisibleDeclConsumer &Consumer,
                         DeclFilter Filter)
        : Consumer(Consumer), Filter(Filter) {}
    void foundDecl(ValueDecl *VD, DeclVisibilityKind Kind,
                   DynamicLookupInfo dynamicLookupInfo) override {
      if (Filter(VD, Kind, dynamicLookupInfo))
        Consumer.foundDecl(VD, Kind, dynamicLookupInfo);
    }
  };

  void getValueCompletionsInDeclContext(SourceLoc Loc,
                                        DeclFilter Filter = DefaultFilter,
                                        bool LiteralCompletions = true,
                                        bool ModuleQualifier = true);

  /// Returns \c true if \p VD is an initializer on the \c Optional or \c
  /// Id_OptionalNilComparisonType type from the Swift stdlib.
  static bool isInitializerOnOptional(Type T, ValueDecl *VD);

  void getUnresolvedMemberCompletions(Type T);

  /// Complete all enum members declared on \p T.
  void getEnumElementPatternCompletions(Type T);

  void getUnresolvedMemberCompletions(ArrayRef<Type> Types);

  void addCallArgumentCompletionResults(ArrayRef<PossibleParamInfo> ParamInfos,
                                        bool isLabeledTrailingClosure = false);

  void getTypeCompletions(Type BaseType);

  void getGenericRequirementCompletions(DeclContext *DC,
                                        SourceLoc CodeCompletionLoc);

  static bool canUseAttributeOnDecl(DeclAttrKind DAK, bool IsInSil,
                                    bool IsConcurrencyEnabled,
                                    llvm::Optional<DeclKind> DK,
                                    StringRef Name);

  void getAttributeDeclCompletions(bool IsInSil, llvm::Optional<DeclKind> DK);

  void getAttributeDeclParamCompletions(CustomSyntaxAttributeKind AttrKind,
                                        int ParamIndex, bool HasLabel);

  void getTypeAttributeKeywordCompletions();

  void collectPrecedenceGroups();

  void getPrecedenceGroupCompletions(
      CodeCompletionCallbacks::PrecedenceGroupCompletionKind SK);

  void getPoundAvailablePlatformCompletions();

  /// \p Loc is the location of the code completin token.
  /// \p isForDeclResult determines if were are spelling out the result type
  /// of a declaration.
  void getSelfTypeCompletionInDeclContext(SourceLoc Loc, bool isForDeclResult);

  void getTypeCompletionsInDeclContext(SourceLoc Loc,
                                       bool ModuleQualifier = true);

  void getToplevelCompletions(CodeCompletionFilter Filter);

  void lookupExternalModuleDecls(const ModuleDecl *TheModule,
                                 ArrayRef<std::string> AccessPath,
                                 bool ResultsHaveLeadingDot);

  void getStmtLabelCompletions(SourceLoc Loc, bool isContinue);

  void getOptionalBindingCompletions(SourceLoc Loc);

  void getWithoutConstraintTypes();
};

} // end namespace ide
} // end namespace swift

namespace llvm {
using RequestedResultsTy = swift::ide::CompletionLookup::RequestedResultsTy;
template <>
struct DenseMapInfo<RequestedResultsTy> {
  static inline RequestedResultsTy getEmptyKey() {
    return {DenseMapInfo<swift::ModuleDecl *>::getEmptyKey(), {}, false};
  }
  static inline RequestedResultsTy getTombstoneKey() {
    return {DenseMapInfo<swift::ModuleDecl *>::getTombstoneKey(), {}, false};
  }
  static unsigned getHashValue(const RequestedResultsTy &Val) {
    return hash_combine(
        DenseMapInfo<swift::ModuleDecl *>::getHashValue(Val.TheModule),
        Val.Filter.toRaw(), Val.NeedLeadingDot);
  }
  static bool isEqual(const RequestedResultsTy &LHS,
                      const RequestedResultsTy &RHS) {
    return LHS == RHS;
  }
};
} // namespace llvm

#endif // SWIFT_IDE_COMPLETIONLOOKUP_H
