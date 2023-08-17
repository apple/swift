//===--- MiscDiagnostics.cpp - AST-Level Diagnostics ----------------------===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2019 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//
//
// This file implements AST-level diagnostics.
//
//===----------------------------------------------------------------------===//

#include "MiscDiagnostics.h"
#include "TypeCheckAvailability.h"
#include "TypeCheckConcurrency.h"
#include "TypeChecker.h"
#include "swift/AST/ASTWalker.h"
#include "swift/AST/DiagnosticsSema.h"
#include "swift/AST/ExistentialLayout.h"
#include "swift/AST/Expr.h"
#include "swift/AST/NameLookup.h"
#include "swift/AST/NameLookupRequests.h"
#include "swift/AST/Pattern.h"
#include "swift/AST/SemanticAttrs.h"
#include "swift/AST/SourceFile.h"
#include "swift/AST/Stmt.h"
#include "swift/AST/TypeCheckRequests.h"
#include "swift/AST/Types.h"
#include "swift/Basic/Defer.h"
#include "swift/Basic/SourceManager.h"
#include "swift/Basic/Statistic.h"
#include "swift/Basic/StringExtras.h"
#include "swift/Parse/Lexer.h"
#include "swift/Parse/Parser.h"
#include "swift/Sema/ConstraintSystem.h"
#include "swift/Sema/IDETypeChecking.h"
#include "clang/AST/DeclObjC.h"
#include "llvm/ADT/MapVector.h"
#include "llvm/ADT/StringSwitch.h"
#include "llvm/Support/SaveAndRestore.h"

#define DEBUG_TYPE "Sema"
using namespace swift;
using namespace constraints;

/// Return true if this expression is an implicit promotion from T to T?.
static Expr *isImplicitPromotionToOptional(Expr *E) {
  if (E->isImplicit())
    if (auto IIOE = dyn_cast<InjectIntoOptionalExpr>(
                                               E->getSemanticsProvidingExpr()))
      return IIOE->getSubExpr();
  return nullptr;
}

ASTWalker::PreWalkAction BaseDiagnosticWalker::walkToDeclPre(Decl *D) {
  return Action::VisitChildrenIf(isa<ClosureExpr>(D->getDeclContext()) &&
                                 shouldWalkIntoDeclInClosureContext(D));
}

bool BaseDiagnosticWalker::shouldWalkIntoDeclInClosureContext(Decl *D) {
  auto *closure = dyn_cast<ClosureExpr>(D->getDeclContext());
  assert(closure);

  if (closure->isSeparatelyTypeChecked())
    return false;

  // Let's not walk into declarations contained in a multi-statement
  // closure because they'd be handled via `typeCheckDecl` that runs
  // syntactic diagnostics.
  if (!closure->hasSingleExpressionBody()) {
    // Since pattern bindings get their types through solution application,
    // `typeCheckDecl` doesn't touch initializers (because they are already
    // fully type-checked), so pattern bindings have to be allowed to be
    // walked to diagnose syntactic issues.
    return isa<PatternBindingDecl>(D);
  }

  return true;
}

/// Diagnose syntactic restrictions of expressions.
///
///   - Module values may only occur as part of qualification.
///   - Metatype names cannot generally be used as values: they need a "T.self"
///     qualification unless used in narrow case (e.g. T() for construction).
///   - '_' may only exist on the LHS of an assignment expression.
///   - warn_unqualified_access values must not be accessed except via qualified
///     lookup.
///   - Partial application of some decls isn't allowed due to implementation
///     limitations.
///   - "&" (aka InOutExpressions) may only exist directly in function call
///     argument lists.
///   - 'self.init' and 'super.init' cannot be wrapped in a larger expression
///     or statement.
///   - Warn about promotions to optional in specific syntactic forms.
///   - Error about collection literals that default to Any collections in
///     invalid positions.
///   - Marker protocols cannot occur as the type of an as? or is expression.
///   - KeyPath expressions cannot refer to effectful properties / subscripts
///   - SingleValueStmtExprs may only appear in certain places and has
///     restrictions on the control flow allowed.
///   - Move expressions must have a declref expr subvalue.
///
static void diagSyntacticUseRestrictions(const Expr *E, const DeclContext *DC,
                                         bool isExprStmt) {
  class DiagnoseWalker : public BaseDiagnosticWalker {
    SmallPtrSet<Expr*, 4> AlreadyDiagnosedMetatypes;
    SmallPtrSet<DeclRefExpr*, 4> AlreadyDiagnosedBitCasts;

    bool IsExprStmt;

    ASTContext &Ctx;
    const DeclContext *DC;

  public:
    DiagnoseWalker(const DeclContext *DC, bool isExprStmt)
        : IsExprStmt(isExprStmt), Ctx(DC->getASTContext()), DC(DC) {}

    MacroWalking getMacroWalkingBehavior() const override {
      return MacroWalking::Expansion;
    }

    PreWalkResult<Pattern *> walkToPatternPre(Pattern *P) override {
      return Action::SkipChildren(P);
    }

    PreWalkAction walkToTypeReprPre(TypeRepr *T) override {
      return Action::Continue();
    }

    bool shouldWalkCaptureInitializerExpressions() override { return true; }

    PreWalkResult<Expr *> walkToExprPre(Expr *E) override {
      // See through implicit conversions of the expression.  We want to be able
      // to associate the parent of this expression with the ultimate callee.
      auto Base = E;
      while (auto Conv = dyn_cast<ImplicitConversionExpr>(Base))
        Base = Conv->getSubExpr();

      if (auto *DRE = dyn_cast<DeclRefExpr>(Base)) {
        // Verify metatype uses.
        if (isa<TypeDecl>(DRE->getDecl())) {
          if (isa<ModuleDecl>(DRE->getDecl()))
            checkUseOfModule(DRE);
          else
            checkUseOfMetaTypeName(Base);
        }

        // Verify warn_unqualified_access uses.
        checkUnqualifiedAccessUse(DRE);
        
        // Verify that special decls are eliminated.
        checkForDeclWithSpecialTypeCheckingSemantics(DRE);
        
        // Verify that `unsafeBitCast` isn't misused.
        checkForSuspiciousBitCasts(DRE, nullptr);
      }
      if (auto *MRE = dyn_cast<MemberRefExpr>(Base)) {
        if (isa<TypeDecl>(MRE->getMember().getDecl()))
          checkUseOfMetaTypeName(Base);
      }
      if (isa<TypeExpr>(Base))
        checkUseOfMetaTypeName(Base);

      if (auto *KPE = dyn_cast<KeyPathExpr>(E)) {
        // raise an error if this KeyPath contains an effectful member.
        checkForEffectfulKeyPath(KPE);
      }

      // Check function calls, looking through implicit conversions on the
      // function and inspecting the arguments directly.
      if (auto *Call = dyn_cast<ApplyExpr>(E)) {
        // Warn about surprising implicit optional promotions.
        checkOptionalPromotions(Call);
        
        // Check the callee, looking through implicit conversions.
        auto base = Call->getFn();
        unsigned uncurryLevel = 0;
        while (auto conv = dyn_cast<ImplicitConversionExpr>(base))
          base = conv->getSubExpr();

        const auto findDynamicMemberRefExpr =
            [](Expr *e) -> DynamicMemberRefExpr* {
          if (auto open = dyn_cast<OpenExistentialExpr>(e)) {
            return dyn_cast<DynamicMemberRefExpr>(open->getSubExpr());
          }
          return nullptr;
        };

        if (auto force = dyn_cast<ForceValueExpr>(base)) {
          if (auto ref = findDynamicMemberRefExpr(force->getSubExpr()))
            base = ref;
        } else if (auto bind = dyn_cast<BindOptionalExpr>(base)) {
          if (auto ref = findDynamicMemberRefExpr(bind->getSubExpr()))
            base = ref;
        }

        while (auto ignoredBase = dyn_cast<DotSyntaxBaseIgnoredExpr>(base))
          base = ignoredBase->getRHS();

        ConcreteDeclRef callee;
        if (auto *calleeDRE = dyn_cast<DeclRefExpr>(base)) {
          checkForSuspiciousBitCasts(calleeDRE, Call);
          callee = calleeDRE->getDeclRef();

        // Otherwise, try to drill down through member calls for the purposes
        // of argument-matching code below.
        } else if (auto selfApply = dyn_cast<SelfApplyExpr>(base)) {
          ++uncurryLevel;
          base = selfApply->getSemanticFn();
          if (auto calleeDRE = dyn_cast<DeclRefExpr>(base))
            callee = calleeDRE->getDeclRef();

        // Otherwise, check for a dynamic member.
        } else if (auto dynamicMRE = dyn_cast<DynamicMemberRefExpr>(base)) {
          ++uncurryLevel;
          callee = dynamicMRE->getMember();
        }

        if (callee) {
          auto *args = Call->getArgs();
          for (auto idx : indices(*args)) {
            auto *arg = args->getExpr(idx);
            checkMagicIdentifierMismatch(callee, uncurryLevel, idx, arg);

            // InOutExprs can be wrapped in some implicit casts.
            Expr *unwrapped = arg;
            if (auto *IIO = dyn_cast<InjectIntoOptionalExpr>(arg))
              unwrapped = IIO->getSubExpr();

            if (isa<InOutToPointerExpr>(unwrapped) ||
                isa<ArrayToPointerExpr>(unwrapped) ||
                isa<ErasureExpr>(unwrapped)) {
              auto operand =
                cast<ImplicitConversionExpr>(unwrapped)->getSubExpr();
              if (auto *IOE = dyn_cast<InOutExpr>(operand))
                operand = IOE->getSubExpr();

              // Also do some additional work based on how the function uses
              // the argument.
              checkConvertedPointerArgument(callee, uncurryLevel, idx,
                                            unwrapped, operand);
            }
          }
        }
      }
      
      // If we have an assignment expression, scout ahead for acceptable _'s.
      if (auto *AE = dyn_cast<AssignExpr>(E)) {
        auto destExpr = AE->getDest();
        // If the user is assigning the result of a function that returns
        // Void to _ then warn, because that is redundant.
        if (auto DAE = dyn_cast<DiscardAssignmentExpr>(destExpr)) {
          if (auto CE = dyn_cast<CallExpr>(AE->getSrc())) {
            if (isa_and_nonnull<FuncDecl>(CE->getCalledValue()) &&
                CE->getType()->isVoid()) {
              Ctx.Diags
                  .diagnose(DAE->getLoc(),
                            diag::discard_expr_void_result_redundant)
                  .fixItRemoveChars(DAE->getStartLoc(),
                                    AE->getSrc()->getStartLoc());
            }
          }
        }
      }

      // Diagnose 'self.init' or 'super.init' nested in another expression
      // or closure.
      if (auto *rebindSelfExpr = dyn_cast<RebindSelfInConstructorExpr>(E)) {
        if (!Parent.isNull() || !IsExprStmt || DC->getParent()->isLocalContext()) {
          bool isChainToSuper;
          (void)rebindSelfExpr->getCalledConstructor(isChainToSuper);
          Ctx.Diags.diagnose(E->getLoc(), diag::init_delegation_nested,
                             isChainToSuper, !IsExprStmt);
        }
      }

      // Diagnose single-element tuple expressions.
      if (auto *tupleExpr = dyn_cast<TupleExpr>(E)) {
        if (tupleExpr->getNumElements() == 1 &&
            !isa<PackExpansionExpr>(tupleExpr->getElement(0))) {
          Ctx.Diags.diagnose(tupleExpr->getElementNameLoc(0),
                             diag::tuple_single_element)
            .fixItRemoveChars(tupleExpr->getElementNameLoc(0),
                              tupleExpr->getElement(0)->getStartLoc());
        }
      }

      auto diagnoseDuplicateLabels = [&](SourceLoc loc,
                                         ArrayRef<Identifier> labels) {
        llvm::SmallDenseSet<Identifier> names;
        names.reserve(labels.size());

        for (auto name : labels) {
          if (name.empty())
            continue;

          auto inserted = names.insert(name).second;
          if (!inserted) {
            Ctx.Diags.diagnose(loc, diag::tuple_duplicate_label);
            return;
          }
        }
      };

      // FIXME: Duplicate labels on enum payloads should be diagnosed
      // when declared, not when called.
      if (auto *CE = dyn_cast_or_null<CallExpr>(E)) {
        auto calledValue = CE->getCalledValue();
        if (calledValue && isa<EnumElementDecl>(calledValue)) {
          auto *args = CE->getArgs();
          SmallVector<Identifier, 4> scratch;
          diagnoseDuplicateLabels(args->getLoc(),
                                  args->getArgumentLabels(scratch));
        }
      }

      if (auto *tupleExpr = dyn_cast<TupleExpr>(E)) {
        // Diagnose tuple expressions with duplicate element label.
        diagnoseDuplicateLabels(tupleExpr->getLoc(),
                                tupleExpr->getElementNames());
                                
        // Diagnose attempts to form a tuple with any noncopyable elements.
        if (E->getType()->isPureMoveOnly()
            && !Ctx.LangOpts.hasFeature(Feature::MoveOnlyTuples)) {
          auto noncopyableTy = E->getType();
          assert(noncopyableTy->is<TupleType>() && "will use poor wording");
          Ctx.Diags.diagnose(E->getLoc(),
                             diag::tuple_containing_move_only_not_supported,
                             noncopyableTy);
        }
      }

      // Specially diagnose some checked casts that are illegal.
      if (auto cast = dyn_cast<CheckedCastExpr>(E)) {
        checkCheckedCastExpr(cast);
      }

      // Diagnose move expression uses where the sub expression is not a declref
      // expr.
      if (auto *consumeExpr = dyn_cast<ConsumeExpr>(E)) {
        checkConsumeExpr(consumeExpr);
      }

      // Diagnose copy expression uses where the sub expression is not a declref
      // expr.
      if (auto *copyExpr = dyn_cast<CopyExpr>(E)) {
        checkCopyExpr(copyExpr);
      }

      // Diagnose move expression uses where the sub expression is not a declref expr
      if (auto *borrowExpr = dyn_cast<BorrowExpr>(E)) {
        checkBorrowExpr(borrowExpr);
      }

      return Action::Continue(E);
    }

    PostWalkResult<Expr *> walkToExprPost(Expr *E) override {
      return Action::Continue(E);
    }

    /// Visit each component of the keypath and emit a diagnostic if they
    /// refer to a member that has effects.
    void checkForEffectfulKeyPath(KeyPathExpr *keyPath) {
      for (const auto &component : keyPath->getComponents()) {
        if (component.hasDeclRef()) {
          auto decl = component.getDeclRef().getDecl();
          if (auto asd = dyn_cast<AbstractStorageDecl>(decl)) {
            if (auto getter = asd->getEffectfulGetAccessor()) {
              Ctx.Diags.diagnose(component.getLoc(),
                                 diag::effectful_keypath_component,
                                 asd->getDescriptiveKind());
              Ctx.Diags.diagnose(asd->getLoc(), diag::kind_declared_here,
                                 asd->getDescriptiveKind());
            }
          }
        }
      }
    }

    void checkCheckedCastExpr(CheckedCastExpr *cast) {
      Type castType = cast->getCastType();
      if (!castType)
        return;

      if (castType->isPureMoveOnly()) {
        // can't cast anything to move-only; there should be no valid ones.
        Ctx.Diags.diagnose(cast->getLoc(), diag::noncopyable_cast);
        return;
      }

      // no support for runtime casts from move-only types.
      // as of now there is no type it could be cast to except itself, so
      // there's no reason for it to happen at runtime.
      if (auto fromType = cast->getSubExpr()->getType()) {
        if (fromType->isPureMoveOnly()) {
          // can't cast move-only to anything.
          Ctx.Diags.diagnose(cast->getLoc(), diag::noncopyable_cast);
          return;
        }
      }

      // now, look for conditional casts to marker protocols.

      if (!isa<ConditionalCheckedCastExpr>(cast) && !isa<IsExpr>(cast))
        return;

      if(!castType->isExistentialType())
        return;

      auto layout = castType->getExistentialLayout();
      for (auto proto : layout.getProtocols()) {
        if (proto->isMarkerProtocol()) {
          // can't conditionally cast to a marker protocol
          Ctx.Diags.diagnose(cast->getLoc(), diag::marker_protocol_cast,
                             proto->getName());
        }
      }
    }

    void checkConsumeExpr(ConsumeExpr *consumeExpr) {
      auto *subExpr = consumeExpr->getSubExpr();
      if (auto *li = dyn_cast<LoadExpr>(subExpr))
        subExpr = li->getSubExpr();
      if (!isa<DeclRefExpr>(subExpr)) {
        Ctx.Diags.diagnose(consumeExpr->getLoc(),
                           diag::consume_expression_not_passed_lvalue);
      }
    }

    void checkCopyExpr(CopyExpr *copyExpr) {
      // Do not allow for copy_expr to be used with pure move only types. We
      // /do/ allow it to be used with no implicit copy types though.
      if (copyExpr->getType()->isPureMoveOnly()) {
        Ctx.Diags.diagnose(
            copyExpr->getLoc(),
            diag::copy_expression_cannot_be_used_with_noncopyable_types);
      }

      // We only allow for copy_expr to be applied directly to lvalues. We do
      // not allow currently for it to be applied to fields.
      auto *subExpr = copyExpr->getSubExpr();
      if (auto *li = dyn_cast<LoadExpr>(subExpr))
        subExpr = li->getSubExpr();
      if (!isa<DeclRefExpr>(subExpr)) {
        Ctx.Diags.diagnose(copyExpr->getLoc(),
                           diag::copy_expression_not_passed_lvalue);
      }
    }

    void checkBorrowExpr(BorrowExpr *borrowExpr) {
      // Allow for a chain of member_ref exprs that end in a decl_ref expr.
      auto *subExpr = borrowExpr->getSubExpr();
      while (auto *memberRef = dyn_cast<MemberRefExpr>(subExpr))
        subExpr = memberRef->getBase();
      if (!isa<DeclRefExpr>(subExpr)) {
        Ctx.Diags.diagnose(borrowExpr->getLoc(),
                           diag::borrow_expression_not_passed_lvalue);
      }
    }

    static Expr *lookThroughArgument(Expr *arg) {
      while (1) {
        if (auto conv = dyn_cast<ImplicitConversionExpr>(arg))
          arg = conv->getSubExpr();
        else if (auto *PE = dyn_cast<ParenExpr>(arg))
          arg = PE->getSubExpr();
        else
          break;
      }
      return arg;
    }

    void checkConvertedPointerArgument(ConcreteDeclRef callee,
                                       unsigned uncurryLevel,
                                       unsigned argIndex,
                                       Expr *pointerExpr,
                                       Expr *storage) {
      if (!isPointerIdentityArgument(callee, uncurryLevel, argIndex))
        return;

      // Flag that the argument is non-accessing.
      if (auto inout = dyn_cast<InOutToPointerExpr>(pointerExpr)) {
        inout->setNonAccessing(true);
      } else if (auto array = dyn_cast<ArrayToPointerExpr>(pointerExpr)) {
        array->setNonAccessing(true);
      }

      // TODO: warn if taking the address of 'storage' will definitely
      // yield a temporary address.
    }

    /// Is the given call argument, known to be of pointer type, just used
    /// for its pointer identity?
    bool isPointerIdentityArgument(ConcreteDeclRef ref, unsigned uncurryLevel,
                                   unsigned argIndex) {
      // FIXME: derive this from an attribute instead of hacking it based
      // on the target name!
      auto decl = ref.getDecl();

      // Assume that == and != are non-accessing uses.
      if (decl->isOperator()) {
        auto op = decl->getBaseName();
        if (op == "==" || op == "!=")
          return true;
        return false;
      }

      // NSObject.addObserver(_:forKeyPath:options:context:)
      if (uncurryLevel == 1 && argIndex == 3) {
        return decl->getName().isCompoundName("addObserver",
                                              { "", "forKeyPath",
                                                "options", "context" });
      }

      // NSObject.removeObserver(_:forKeyPath:context:)
      if (uncurryLevel == 1 && argIndex == 2) {
        return decl->getName().isCompoundName("removeObserver",
                                              { "", "forKeyPath", "context" });
      }

      return false;
    }

    /// We have a collection literal with a defaulted type, e.g. of [Any].  Emit
    /// an error if it was inferred to this type in an invalid context, which is
    /// one in which the parent expression is not itself a collection literal.
    void checkTypeDefaultedCollectionExpr(CollectionExpr *c) {
      // If the parent is a non-expression, or is not itself a literal, then
      // produce an error with a fixit to add the type as an explicit
      // annotation.
      if (c->getNumElements() == 0)
        Ctx.Diags.diagnose(c->getLoc(), diag::collection_literal_empty)
            .highlight(c->getSourceRange());
      else {
        assert(c->getType()->hasTypeRepr() &&
               "a defaulted type should always be printable");
        Ctx.Diags
            .diagnose(c->getLoc(), diag::collection_literal_heterogeneous,
                      c->getType())
            .highlight(c->getSourceRange())
            .fixItInsertAfter(c->getEndLoc(),
                              " as " + c->getType()->getString());
      }
    }

    void checkMagicIdentifierMismatch(ConcreteDeclRef callee,
                                      unsigned uncurryLevel,
                                      unsigned argIndex,
                                      Expr *arg) {
      // We only care about args in the arg list.
      if (uncurryLevel != (callee.getDecl()->hasCurriedSelf() ? 1 : 0))
        return;

      // Get underlying params for both callee and caller, if declared.
      auto *calleeParam = getParameterAt(callee, argIndex);
      auto *callerParam = dyn_cast_or_null<ParamDecl>(
          arg->getReferencedDecl(/*stopAtParenExpr=*/true).getDecl()
      );

      // (Otherwise, we don't need to do anything.)
      if (!calleeParam || !callerParam)
        return;

      auto calleeDefaultArg = getMagicIdentifierDefaultArgKind(calleeParam);
      auto callerDefaultArg = getMagicIdentifierDefaultArgKind(callerParam);

      // If one of the parameters doesn't have a default arg, or they're both
      // compatible, everything's fine.
      if (!calleeDefaultArg || !callerDefaultArg ||
          areMagicIdentifiersCompatible(*calleeDefaultArg, *callerDefaultArg))
        return;

      StringRef calleeDefaultArgString =
          MagicIdentifierLiteralExpr::getKindString(*calleeDefaultArg);
      StringRef callerDefaultArgString =
          MagicIdentifierLiteralExpr::getKindString(*callerDefaultArg);

      // Emit main warning
      Ctx.Diags.diagnose(arg->getLoc(), diag::default_magic_identifier_mismatch,
                         callerParam->getName(), callerDefaultArgString,
                         calleeParam->getName(), calleeDefaultArgString);

      // Add "change caller default arg" fixit
      SourceLoc callerDefaultArgLoc =
          callerParam->getStructuralDefaultExpr()->getLoc();
      Ctx.Diags.diagnose(callerDefaultArgLoc,
                         diag::change_caller_default_to_match_callee,
                         callerParam->getName(), calleeDefaultArgString)
        .fixItReplace(callerDefaultArgLoc, calleeDefaultArgString);

      // Add "silence with parens" fixit
      Ctx.Diags.diagnose(arg->getLoc(),
                         diag::silence_default_magic_identifier_mismatch)
        .fixItInsert(arg->getStartLoc(), "(")
        .fixItInsertAfter(arg->getEndLoc(), ")");

      // Point to callee parameter
      Ctx.Diags.diagnose(calleeParam, diag::decl_declared_here, calleeParam);
    }

    llvm::Optional<MagicIdentifierLiteralExpr::Kind>
    getMagicIdentifierDefaultArgKind(const ParamDecl *param) {
      switch (param->getDefaultArgumentKind()) {
#define MAGIC_IDENTIFIER(NAME, STRING, SYNTAX_KIND) \
      case DefaultArgumentKind::NAME: \
        return MagicIdentifierLiteralExpr::Kind::NAME;
#include "swift/AST/MagicIdentifierKinds.def"

      case DefaultArgumentKind::None:
      case DefaultArgumentKind::Normal:
      case DefaultArgumentKind::Inherited:
      case DefaultArgumentKind::NilLiteral:
      case DefaultArgumentKind::EmptyArray:
      case DefaultArgumentKind::EmptyDictionary:
      case DefaultArgumentKind::StoredProperty:
        return llvm::None;
      }

      llvm_unreachable("Unhandled DefaultArgumentKind in "
                       "getMagicIdentifierDefaultArgKind");
    }

    static bool
    areMagicIdentifiersCompatible(MagicIdentifierLiteralExpr::Kind a,
                                  MagicIdentifierLiteralExpr::Kind b) {
      if (a == b)
        return true;

      // The rest of this handles special compatibility rules between the
      // `*SpelledAsFile` cases and various other File-related cases.
      //
      // The way we're going to do this is a bit magical. We will arrange the
      // cases in MagicIdentifierLiteralExpr::Kind so that they sort in
      // this order:
      //
      //     #fileID < Swift 6 #file < #filePath < Swift 5 #file < others
      //
      // Before we continue, let's verify that this holds.

      using Kind = MagicIdentifierLiteralExpr::Kind;

      static_assert(Kind::FileID < Kind::FileIDSpelledAsFile,
                    "#fileID < Swift 6 #file");
      static_assert(Kind::FileIDSpelledAsFile < Kind::FilePath,
                    "Swift 6 #file < #filePath");
      static_assert(Kind::FilePath < Kind::FilePathSpelledAsFile,
                    "#filePath < Swift 5 #file");

      static_assert(Kind::FilePathSpelledAsFile < Kind::Line,
                    "Swift 5 #file < #line");
      static_assert(Kind::FilePathSpelledAsFile < Kind::Column,
                    "Swift 5 #file < #column");
      static_assert(Kind::FilePathSpelledAsFile < Kind::Function,
                    "Swift 5 #file < #function");
      static_assert(Kind::FilePathSpelledAsFile < Kind::DSOHandle,
                    "Swift 5 #file < #dsohandle");

      // The rules are all commutative, so we will take the greater of the two
      // kinds.
      auto maxKind = std::max(a, b);

      // Both Swift 6 #file and Swift 5 #file are greater than all of the cases
      // they're compatible with. So if `maxCase` is one of those two, the other
      // case must have been compatible with it!
      return maxKind == Kind::FileIDSpelledAsFile ||
             maxKind == Kind::FilePathSpelledAsFile;
    }

    void checkUseOfModule(DeclRefExpr *E) {
      // Allow module values as a part of:
      // - ignored base expressions;
      // - expressions that failed to type check.
      if (auto *ParentExpr = Parent.getAsExpr()) {
        if (isa<DotSyntaxBaseIgnoredExpr>(ParentExpr) ||
            isa<UnresolvedDotExpr>(ParentExpr))
          return;
      }

      Ctx.Diags.diagnose(E->getStartLoc(), diag::value_of_module_type);
    }

    // Diagnose metatype values that don't appear as part of a property,
    // method, or constructor reference.
    void checkUseOfMetaTypeName(Expr *E) {
      // If we've already checked this at a higher level, we're done.
      if (!AlreadyDiagnosedMetatypes.insert(E).second)
        return;

      DiagnosticBehavior behavior = DiagnosticBehavior::Error;

      if (auto *ParentExpr = Parent.getAsExpr()) {
        if (ParentExpr->isValidParentOfTypeExpr(E))
          return;

        // In Swift < 6 warn about
        // - plain type name passed as an argument to a subscript, dynamic
        //   subscript, or ObjC literal since it used to be accepted.
        // - member type expressions rooted on non-identifier types, e.g.
        //   '[X].Y' since they used to be accepted without the '.self'.
        if (!Ctx.LangOpts.isSwiftVersionAtLeast(6)) {
          if (isa<SubscriptExpr>(ParentExpr) ||
              isa<DynamicSubscriptExpr>(ParentExpr) ||
              isa<ObjectLiteralExpr>(ParentExpr)) {
            auto *argList = ParentExpr->getArgs();
            assert(argList);
            if (argList->isUnlabeledUnary())
              behavior = DiagnosticBehavior::Warning;
          } else if (auto *TE = dyn_cast<TypeExpr>(E)) {
            if (auto *TR =
                    dyn_cast_or_null<MemberTypeRepr>(TE->getTypeRepr())) {
              if (!isa<IdentTypeRepr>(TR->getBaseComponent())) {
                behavior = DiagnosticBehavior::Warning;
              }
            }
          }
        }
      }

      // Is this a protocol metatype?
      Ctx.Diags
          .diagnose(E->getStartLoc(), diag::value_of_metatype_type,
                    behavior == DiagnosticBehavior::Warning)
          .limitBehavior(behavior);

      // Add fix-it to insert '()', only if this is a metatype of
      // non-existential type and has any initializers.
      bool isExistential = false;
      if (auto metaTy = E->getType()->getAs<MetatypeType>()) {
        auto instanceTy = metaTy->getInstanceType();
        isExistential = instanceTy->isExistentialType();
        if (!isExistential &&
            instanceTy->mayHaveMembers() &&
            !TypeChecker::lookupMember(const_cast<DeclContext *>(DC), instanceTy,
                                       DeclNameRef::createConstructor()).empty()) {
          Ctx.Diags.diagnose(E->getEndLoc(), diag::add_parens_to_type)
            .fixItInsertAfter(E->getEndLoc(), "()");
        }
      }

      // Add fix-it to insert ".self".
      auto diag = Ctx.Diags.diagnose(E->getEndLoc(), diag::add_self_to_type);
      if (E->canAppendPostfixExpression()) {
        diag.fixItInsertAfter(E->getEndLoc(), ".self");
      } else {
        diag.fixItInsert(E->getStartLoc(), "(");
        diag.fixItInsertAfter(E->getEndLoc(), ").self");
      }
    }

    void checkUnqualifiedAccessUse(const DeclRefExpr *DRE) {
      const Decl *D = DRE->getDecl();
      if (!D->getAttrs().hasAttribute<WarnUnqualifiedAccessAttr>())
        return;

      if (auto *parentExpr = Parent.getAsExpr()) {
        if (auto *ignoredBase = dyn_cast<DotSyntaxBaseIgnoredExpr>(parentExpr)){
          if (!ignoredBase->isImplicit())
            return;
        }
        if (auto *calledBase = dyn_cast<DotSyntaxCallExpr>(parentExpr)) {
          if (!calledBase->isImplicit())
            return;
        }
      }

      const auto *VD = cast<ValueDecl>(D);
      const TypeDecl *declParent =
          VD->getDeclContext()->getSelfNominalTypeDecl();
      if (!declParent) {
        // If the declaration has been validated but not fully type-checked,
        // the attribute might be applied to something invalid.
        if (!VD->getDeclContext()->isModuleScopeContext())
          return;
        declParent = VD->getDeclContext()->getParentModule();
      }

      Ctx.Diags.diagnose(DRE->getLoc(), diag::warn_unqualified_access,
                         VD->getBaseIdentifier(),
                         VD->getDescriptiveKind(),
                         declParent);
      Ctx.Diags.diagnose(VD, diag::decl_declared_here, VD);

      if (VD->getDeclContext()->isTypeContext()) {
        Ctx.Diags.diagnose(DRE->getLoc(), diag::fix_unqualified_access_member)
          .fixItInsert(DRE->getStartLoc(), "self.");
      }

      DeclContext *topLevelSubcontext = DC->getModuleScopeContext();
      auto descriptor = UnqualifiedLookupDescriptor(
          DeclNameRef(VD->getBaseName()), topLevelSubcontext, SourceLoc());
      auto lookup = evaluateOrDefault(Ctx.evaluator,
                                      UnqualifiedLookupRequest{descriptor}, {});

      // Group results by module. Pick an arbitrary result from each module.
      llvm::SmallDenseMap<const ModuleDecl*,const ValueDecl*,4> resultsByModule;
      for (auto &result : lookup) {
        const ValueDecl *value = result.getValueDecl();
        resultsByModule.insert(std::make_pair(value->getModuleContext(),value));
      }

      // Sort by module name.
      using ModuleValuePair = std::pair<const ModuleDecl *, const ValueDecl *>;
      SmallVector<ModuleValuePair, 4> sortedResults{
        resultsByModule.begin(), resultsByModule.end()
      };
      llvm::array_pod_sort(sortedResults.begin(), sortedResults.end(),
                           [](const ModuleValuePair *lhs,
                              const ModuleValuePair *rhs) {
        return lhs->first->getName().compare(rhs->first->getName());
      });

      auto topLevelDiag = diag::fix_unqualified_access_top_level;
      if (sortedResults.size() > 1)
        topLevelDiag = diag::fix_unqualified_access_top_level_multi;

      for (const ModuleValuePair &pair : sortedResults) {
        DescriptiveDeclKind k = pair.second->getDescriptiveKind();

        SmallString<32> namePlusDot = pair.first->getName().str();
        namePlusDot.push_back('.');

        Ctx.Diags.diagnose(DRE->getLoc(), topLevelDiag,
                           namePlusDot, k, pair.first->getName())
          .fixItInsert(DRE->getStartLoc(), namePlusDot);
      }
    }
    
    void checkForDeclWithSpecialTypeCheckingSemantics(const DeclRefExpr *DRE) {
      // Referencing type(of:) and other decls with special type-checking
      // behavior as functions is not implemented. Maybe we could wrap up the
      // special-case behavior in a closure someday...
      if (TypeChecker::getDeclTypeCheckingSemantics(DRE->getDecl())
            != DeclTypeCheckingSemantics::Normal) {
        Ctx.Diags.diagnose(DRE->getLoc(), diag::unsupported_special_decl_ref,
                           DRE->getDecl()->getBaseIdentifier());
      }
    }
    
    enum BitcastableNumberKind {
      BNK_None = 0,
      BNK_Int8,
      BNK_Int16,
      BNK_Int32,
      BNK_Int64,
      BNK_Int,
      BNK_UInt8,
      BNK_UInt16,
      BNK_UInt32,
      BNK_UInt64,
      BNK_UInt,
      BNK_Float,
      BNK_Double,
    };
    BitcastableNumberKind getBitcastableNumberKind(Type t) const {
      auto decl = t->getNominalOrBoundGenericNominal();
#define MATCH_DECL(type) \
      if (decl == Ctx.get##type##Decl()) \
        return BNK_##type;
      MATCH_DECL(Int8)
      MATCH_DECL(Int16)
      MATCH_DECL(Int32)
      MATCH_DECL(Int64)
      MATCH_DECL(Int)
      MATCH_DECL(UInt8)
      MATCH_DECL(UInt16)
      MATCH_DECL(UInt32)
      MATCH_DECL(UInt64)
      MATCH_DECL(UInt)
      MATCH_DECL(Float)
      MATCH_DECL(Double)
#undef MATCH_DECL
      
      return BNK_None;
    }
    
    static constexpr unsigned BNKPair(BitcastableNumberKind a,
                                      BitcastableNumberKind b) {
      return (a << 8) | b;
    }
    
    void checkForSuspiciousBitCasts(DeclRefExpr *DRE,
                                    Expr *Parent = nullptr) {
      if (DRE->getDecl() != Ctx.getUnsafeBitCast())
        return;
      
      if (DRE->getDeclRef().getSubstitutions().empty())
        return;
      
      // Don't check the same use of unsafeBitCast twice.
      if (!AlreadyDiagnosedBitCasts.insert(DRE).second)
        return;

      auto subMap = DRE->getDeclRef().getSubstitutions();
      auto fromTy = subMap.getReplacementTypes()[0];
      auto toTy = subMap.getReplacementTypes()[1];

      // Warn about `unsafeBitCast` formulations that are undefined behavior
      // or have better-defined alternative APIs that can be used instead.
      
      // If we have a parent ApplyExpr that calls bitcast, extract the argument
      // for fixits.
      Expr *subExpr = nullptr;
      CharSourceRange removeBeforeRange, removeAfterRange;
      if (auto apply = dyn_cast_or_null<ApplyExpr>(Parent)) {
        subExpr = apply->getArgs()->getExpr(0);
        // Determine the fixit range from the start of the application to
        // the first argument, `unsafeBitCast(`
        removeBeforeRange = CharSourceRange(Ctx.SourceMgr, DRE->getLoc(),
                                            subExpr->getStartLoc());
        // Determine the fixit range from the end of the first argument to
        // the end of the application, `, to: T.self)`
        removeAfterRange = CharSourceRange(Ctx.SourceMgr,
                       Lexer::getLocForEndOfToken(Ctx.SourceMgr,
                                                  subExpr->getEndLoc()),
                       Lexer::getLocForEndOfToken(Ctx.SourceMgr,
                                                  apply->getEndLoc()));
      }
  
      // Casting to the same type or a superclass is a no-op.
      if (toTy->isEqual(fromTy) ||
          toTy->isExactSuperclassOf(fromTy)) {
        auto d = Ctx.Diags.diagnose(DRE->getLoc(), diag::bitcasting_is_no_op,
                                    fromTy, toTy);
        if (subExpr) {
          d.fixItRemoveChars(removeBeforeRange.getStart(),
                             removeBeforeRange.getEnd())
           .fixItRemoveChars(removeAfterRange.getStart(),
                             removeAfterRange.getEnd());
        }
        return;
      }
      
     if (auto fromFnTy = fromTy->getAs<FunctionType>()) {
        if (auto toFnTy = toTy->getAs<FunctionType>()) {
          // Casting a nonescaping function to escaping is UB.
          // `withoutActuallyEscaping` ought to be used instead.
          if (fromFnTy->isNoEscape() && !toFnTy->isNoEscape()) {
            Ctx.Diags.diagnose(DRE->getLoc(), diag::bitcasting_away_noescape,
                               fromTy, toTy);
          }
          // Changing function representation (say, to try to force a
          // @convention(c) function pointer to exist) is also unlikely to work.
          if (fromFnTy->getRepresentation() != toFnTy->getRepresentation()) {
            Ctx.Diags.diagnose(DRE->getLoc(),
                               diag::bitcasting_to_change_function_rep, fromTy,
                               toTy);
          }
          return;
        }
      }
      
      // Unchecked casting to a subclass is better done by unsafeDowncast.
      if (fromTy->isBindableToSuperclassOf(toTy)) {
        Ctx.Diags.diagnose(DRE->getLoc(), diag::bitcasting_to_downcast,
                           fromTy, toTy)
          .fixItReplace(DRE->getNameLoc().getBaseNameLoc(),
                        "unsafeDowncast");
        return;
      }

      // Casting among pointer types should use the Unsafe*Pointer APIs for
      // rebinding typed memory or accessing raw memory instead.
      PointerTypeKind fromPTK, toPTK;
      Type fromPointee = fromTy->getAnyPointerElementType(fromPTK);
      Type toPointee = toTy->getAnyPointerElementType(toPTK);
      if (fromPointee && toPointee) {
        // Casting to a pointer to the same type or UnsafeRawPointer can use
        // normal initializers on the destination type.
        if (toPointee->isEqual(fromPointee)
            || isRawPointerKind(toPTK)) {
          auto d = Ctx.Diags.diagnose(DRE->getLoc(),
                              diag::bitcasting_to_change_pointer_kind,
                              fromTy, toTy,
                              toTy->getStructOrBoundGenericStruct()->getName());
          if (subExpr) {
            StringRef before, after;
            switch (toPTK) {
            case PTK_UnsafePointer:
              // UnsafePointer(mutablePointer)
              before = "UnsafePointer(";
              after = ")";
              break;
            case PTK_UnsafeMutablePointer:
            case PTK_AutoreleasingUnsafeMutablePointer:
              before = "UnsafeMutablePointer(mutating: ";
              after = ")";
              break;
              
            case PTK_UnsafeRawPointer:
              // UnsafeRawPointer(pointer)
              before = "UnsafeRawPointer(";
              after = ")";
              break;
              
            case PTK_UnsafeMutableRawPointer:
              // UnsafeMutableRawPointer(mutating: rawPointer)
              before = fromPTK == PTK_UnsafeMutablePointer
                ? "UnsafeMutableRawPointer("
                : "UnsafeMutableRawPointer(mutating: ";
              after = ")";
              break;
            }
            d.fixItReplaceChars(removeBeforeRange.getStart(),
                                removeBeforeRange.getEnd(),
                                before)
             .fixItReplaceChars(removeAfterRange.getStart(),
                                removeAfterRange.getEnd(),
                                after);
          }
          return;
        }
        
        // Casting to a different typed pointer type should use
        // withMemoryRebound.
        if (!isRawPointerKind(fromPTK) && !isRawPointerKind(toPTK)) {
          Ctx.Diags.diagnose(DRE->getLoc(),
                             diag::bitcasting_to_change_pointee_type,
                             fromTy, toTy);
          return;
        }
        
        // Casting a raw pointer to a typed pointer should bind the memory
        // (or assume it's already bound).
        assert(isRawPointerKind(fromPTK) && !isRawPointerKind(toPTK)
               && "unhandled cast combo?!");
        Ctx.Diags.diagnose(DRE->getLoc(),
                           diag::bitcasting_to_give_type_to_raw_pointer,
                           fromTy, toTy);
        if (subExpr) {
          SmallString<64> fixitBuf;
          {
            llvm::raw_svector_ostream os(fixitBuf);
            os << ".assumingMemoryBound(to: ";
            toPointee->print(os);
            os << ".self)";
          }
          Ctx.Diags.diagnose(DRE->getLoc(),
                             diag::bitcast_assume_memory_rebound,
                             toPointee)
            .fixItRemoveChars(removeBeforeRange.getStart(),
                              removeBeforeRange.getEnd())
            .fixItReplaceChars(removeAfterRange.getStart(),
                               removeAfterRange.getEnd(),
                               fixitBuf);
          fixitBuf.clear();
          {
            llvm::raw_svector_ostream os(fixitBuf);
            os << ".bindMemory(to: ";
            toPointee->print(os);
            os << ".self, capacity: <""#capacity#"">)";
          }
          Ctx.Diags.diagnose(DRE->getLoc(),
                             diag::bitcast_bind_memory,
                             toPointee)
            .fixItRemoveChars(removeBeforeRange.getStart(),
                              removeBeforeRange.getEnd())
            .fixItReplaceChars(removeAfterRange.getStart(),
                               removeAfterRange.getEnd(),
                               fixitBuf);
        }
        return;
      }
      
      StringRef replaceBefore, replaceAfter;
      llvm::Optional<Diag<Type, Type>> diagID;
      SmallString<64> replaceBeforeBuf;

      // Bitcasting among numeric types should use `bitPattern:` initializers.
      auto fromBNK = getBitcastableNumberKind(fromTy);
      auto toBNK = getBitcastableNumberKind(toTy);
      if (fromBNK && toBNK) {
        switch (BNKPair(fromBNK, toBNK)) {
        // Combos that can be bitPattern-ed with a constructor
        case BNKPair(BNK_Int8, BNK_UInt8):
        case BNKPair(BNK_UInt8, BNK_Int8):
        case BNKPair(BNK_Int16, BNK_UInt16):
        case BNKPair(BNK_UInt16, BNK_Int16):
        case BNKPair(BNK_Int32, BNK_UInt32):
        case BNKPair(BNK_UInt32, BNK_Int32):
        case BNKPair(BNK_Int64, BNK_UInt64):
        case BNKPair(BNK_UInt64, BNK_Int64):
        case BNKPair(BNK_Int, BNK_UInt):
        case BNKPair(BNK_UInt, BNK_Int):
        case BNKPair(BNK_UInt32, BNK_Float):
        case BNKPair(BNK_UInt64, BNK_Double):
          diagID = diag::bitcasting_for_number_bit_pattern_init;
          {
            llvm::raw_svector_ostream os(replaceBeforeBuf);
            toTy->print(os);
            os << "(bitPattern: ";
          }
          replaceBefore = replaceBeforeBuf;
          replaceAfter = ")";
          break;
        
        // Combos that can be bitPattern-ed with a constructor and sign flip
        case BNKPair(BNK_Int32, BNK_Float):
        case BNKPair(BNK_Int64, BNK_Double):
          diagID = diag::bitcasting_for_number_bit_pattern_init;
          {
            llvm::raw_svector_ostream os(replaceBeforeBuf);
            toTy->print(os);
            os << "(bitPattern: ";
            if (fromBNK == BNK_Int32)
              os << "UInt32(bitPattern: ";
            else
              os << "UInt64(bitPattern: ";
          }
          replaceBefore = replaceBeforeBuf;
          replaceAfter = "))";
          break;
        
        // Combos that can be bitPattern-ed with a property
        case BNKPair(BNK_Float, BNK_UInt32):
        case BNKPair(BNK_Double, BNK_UInt64):
          diagID = diag::bitcasting_for_number_bit_pattern_property;
          replaceAfter = ".bitPattern";
          break;
        
        // Combos that can be bitPattern-ed with a property and sign flip
        case BNKPair(BNK_Float, BNK_Int32):
        case BNKPair(BNK_Double, BNK_Int64):
          diagID = diag::bitcasting_for_number_bit_pattern_property;
          {
            llvm::raw_svector_ostream os(replaceBeforeBuf);
            toTy->print(os);
            os << "(bitPattern: ";
          }
          replaceBefore = replaceBeforeBuf;
          replaceAfter = ")";
          break;

        // Combos that can be bitPattern-ed with a constructor once (U)Int is
        // converted to a sized type.
        case BNKPair(BNK_UInt, BNK_Float):
        case BNKPair(BNK_Int, BNK_UInt32):
        case BNKPair(BNK_UInt, BNK_Int32):
        case BNKPair(BNK_Int, BNK_UInt64):
        case BNKPair(BNK_UInt, BNK_Int64):
        case BNKPair(BNK_UInt, BNK_Double):
          diagID = diag::bitcasting_for_number_bit_pattern_init;
          {
            llvm::raw_svector_ostream os(replaceBeforeBuf);
            toTy->print(os);
            os << "(bitPattern: ";
            
            if (fromBNK == BNK_Int)
              os << "Int";
            else
              os << "UInt";
            
            if (toBNK == BNK_Float
                || toBNK == BNK_Int32
                || toBNK == BNK_UInt32)
              os << "32(";
            else
              os << "64(";
          }
          replaceBefore = replaceBeforeBuf;
          replaceAfter = "))";
          break;

        case BNKPair(BNK_Int, BNK_Float):
        case BNKPair(BNK_Int, BNK_Double):
          diagID = diag::bitcasting_for_number_bit_pattern_init;
          {
            llvm::raw_svector_ostream os(replaceBeforeBuf);
            toTy->print(os);
            os << "(bitPattern: UInt";
            
            if (toBNK == BNK_Float
                || toBNK == BNK_Int32
                || toBNK == BNK_UInt32)
              os << "32(bitPattern: Int32(";
            else
              os << "64(bitPattern: Int64(";
          }
          replaceBefore = replaceBeforeBuf;
          replaceAfter = ")))";
          break;
    
        // Combos that can be bitPattern-ed then converted from a sized type
        // to (U)Int.
        case BNKPair(BNK_Int32, BNK_UInt):
        case BNKPair(BNK_UInt32, BNK_Int):
        case BNKPair(BNK_Int64, BNK_UInt):
        case BNKPair(BNK_UInt64, BNK_Int):
          diagID = diag::bitcasting_for_number_bit_pattern_init;
          {
            llvm::raw_svector_ostream os(replaceBeforeBuf);
            toTy->print(os);
            os << "(";
            if (toBNK == BNK_UInt)
              os << "UInt";
            else
              os << "Int";
            if (fromBNK == BNK_Int32 || fromBNK == BNK_UInt32)
              os << "32(bitPattern: ";
            else
              os << "64(bitPattern: ";
          }
          replaceBefore = replaceBeforeBuf;
          replaceAfter = "))";
          break;
        
        case BNKPair(BNK_Float, BNK_UInt):
        case BNKPair(BNK_Double, BNK_UInt):
          diagID = diag::bitcasting_for_number_bit_pattern_property;
          {
            llvm::raw_svector_ostream os(replaceBeforeBuf);
            toTy->print(os);
            os << "(";
          }
          replaceBefore = replaceBeforeBuf;
          replaceAfter = ".bitPattern)";
          break;
          
        case BNKPair(BNK_Float, BNK_Int):
        case BNKPair(BNK_Double, BNK_Int):
          diagID = diag::bitcasting_for_number_bit_pattern_property;
          {
            llvm::raw_svector_ostream os(replaceBeforeBuf);
            toTy->print(os);
            os << "(bitPattern: UInt(";
          }
          replaceBefore = replaceBeforeBuf;
          replaceAfter = ".bitPattern))";
          break;
        
        // Combos that should be done with a value-preserving initializer.
        case BNKPair(BNK_Int, BNK_Int32):
        case BNKPair(BNK_Int, BNK_Int64):
        case BNKPair(BNK_UInt, BNK_UInt32):
        case BNKPair(BNK_UInt, BNK_UInt64):
        case BNKPair(BNK_Int32, BNK_Int):
        case BNKPair(BNK_Int64, BNK_Int):
        case BNKPair(BNK_UInt32, BNK_UInt):
        case BNKPair(BNK_UInt64, BNK_UInt):
          diagID = diag::bitcasting_to_change_from_unsized_to_sized_int;
          {
            llvm::raw_svector_ostream os(replaceBeforeBuf);
            toTy->print(os);
            os << '(';
          }
          replaceBefore = replaceBeforeBuf;
          replaceAfter = ")";
          break;
        
        default:
          // Leave other combos alone.
          break;
        }
      }
      
      // Casting a pointer to an int or back should also use bitPattern
      // initializers.
      if (fromPointee && toBNK) {
        switch (toBNK) {
        case BNK_UInt:
        case BNK_Int:
          diagID = diag::bitcasting_for_number_bit_pattern_init;
          {
            llvm::raw_svector_ostream os(replaceBeforeBuf);
            toTy->print(os);
            os << "(bitPattern: ";
          }
          replaceBefore = replaceBeforeBuf;
          replaceAfter = ")";
          break;
          
        case BNK_UInt64:
        case BNK_UInt32:
        case BNK_Int64:
        case BNK_Int32:
          diagID = diag::bitcasting_for_number_bit_pattern_init;
          {
            llvm::raw_svector_ostream os(replaceBeforeBuf);
            toTy->print(os);
            os << '(';
            if (toBNK == BNK_UInt32 || toBNK == BNK_UInt64)
              os << "UInt(bitPattern: ";
            else
              os << "Int(bitPattern: ";
          }
          replaceBefore = replaceBeforeBuf;
          replaceAfter = "))";
          break;
        
        default:
          break;
        }
      }
      if (fromBNK && toPointee) {
        switch (fromBNK) {
        case BNK_UInt:
        case BNK_Int:
          diagID = diag::bitcasting_for_number_bit_pattern_init;
          {
            llvm::raw_svector_ostream os(replaceBeforeBuf);
            toTy->print(os);
            os << "(bitPattern: ";
          }
          replaceBefore = replaceBeforeBuf;
          replaceAfter = ")";
          break;
          
        case BNK_UInt64:
        case BNK_UInt32:
        case BNK_Int64:
        case BNK_Int32:
          diagID = diag::bitcasting_for_number_bit_pattern_init;
          {
            llvm::raw_svector_ostream os(replaceBeforeBuf);
            toTy->print(os);
            os << "(bitPattern: ";
            if (fromBNK == BNK_Int32 || fromBNK == BNK_Int64)
              os << "Int(";
            else
              os << "UInt(";
          }
          replaceBefore = replaceBeforeBuf;
          replaceAfter = "))";
          break;

        default:
          break;
        }
      }
      
      if (diagID) {
        auto d = Ctx.Diags.diagnose(DRE->getLoc(), *diagID, fromTy, toTy);
        if (subExpr) {
          d.fixItReplaceChars(removeBeforeRange.getStart(),
                              removeBeforeRange.getEnd(),
                              replaceBefore);
          d.fixItReplaceChars(removeAfterRange.getStart(),
                              removeAfterRange.getEnd(),
                              replaceAfter);
        }
      }

    }
    
    /// Return true if this is a 'nil' literal.  This looks
    /// like this if the type is Optional<T>:
    ///
    ///   (dot_syntax_call_expr type='String?'
    ///     (declref_expr type='(Optional<String>.Type) -> Optional<String>'
    ///      decl=Swift.(file).Optional.none function_ref=unapplied)
    ///     (argument_list implicit
    ///       (argument
    ///         (type_expr implicit type='String?.Type' typerepr='String?'))))
    ///
    /// Or like this if it is any other ExpressibleByNilLiteral type:
    ///
    ///   (nil_literal_expr)
    ///
    bool isTypeCheckedOptionalNil(Expr *E) {
      if (dyn_cast<NilLiteralExpr>(E)) return true;

      if (auto *DSCE = dyn_cast_or_null<DotSyntaxCallExpr>(E->getSemanticsProvidingExpr())) {
        if (auto *DRE = dyn_cast<DeclRefExpr>(DSCE->getSemanticFn()))
          return DRE->getDecl() == Ctx.getOptionalNoneDecl();
      }

      return false;
    }


    /// Warn about surprising implicit optional promotions involving operands to
    /// calls.  Specifically, we warn about these expressions when the 'x'
    /// operand is implicitly promoted to optional:
    ///
    ///       x ?? y
    ///       x == nil    // also !=
    ///
    void checkOptionalPromotions(ApplyExpr *call) {
      // We only care about binary expressions.
      auto *BE = dyn_cast<BinaryExpr>(call);
      if (!BE) return;

      // Dig out the function we're calling.
      auto fnExpr = call->getSemanticFn();
      if (auto dotSyntax = dyn_cast<DotSyntaxCallExpr>(fnExpr))
        fnExpr = dotSyntax->getSemanticFn();

      auto DRE = dyn_cast<DeclRefExpr>(fnExpr);
      if (!DRE || !DRE->getDecl()->isOperator())
        return;
      
      auto lhs = BE->getLHS();
      auto rhs = BE->getRHS();
      auto calleeName = DRE->getDecl()->getBaseName();

      Expr *subExpr = nullptr;
      if (calleeName == "??" &&
          (subExpr = isImplicitPromotionToOptional(lhs))) {

        Ctx.Diags
            .diagnose(DRE->getLoc(), diag::use_of_qq_on_non_optional_value,
                      subExpr->getType())
            .highlight(lhs->getSourceRange())
            .fixItRemoveChars(
                Lexer::getLocForEndOfToken(Ctx.SourceMgr, lhs->getEndLoc()),
                Lexer::getLocForEndOfToken(Ctx.SourceMgr, rhs->getEndLoc()));
        return;
      }
      
      if (calleeName == "==" || calleeName == "!=" ||
          calleeName == "===" || calleeName == "!==") {
        if (((subExpr = isImplicitPromotionToOptional(lhs)) &&
             isTypeCheckedOptionalNil(rhs)) ||
            (isTypeCheckedOptionalNil(lhs) &&
             (subExpr = isImplicitPromotionToOptional(rhs)))) {
          bool isTrue = calleeName == "!=" || calleeName == "!==";
          bool isNilLiteral = isa<NilLiteralExpr>(lhs) || isa<NilLiteralExpr>(rhs);
              
          Ctx.Diags.diagnose(DRE->getLoc(), diag::nonoptional_compare_to_nil,
                             subExpr->getType(), isNilLiteral, isTrue)
            .highlight(lhs->getSourceRange())
            .highlight(rhs->getSourceRange());
          return;
        }
      }
    }
  };

  DiagnoseWalker Walker(DC, isExprStmt);
  const_cast<Expr *>(E)->walk(Walker);

  // Diagnose uses of collection literals with defaulted types at the top
  // level.
  if (auto collection =
          dyn_cast<CollectionExpr>(E->getSemanticsProvidingExpr())) {
    if (collection->isTypeDefaulted()) {
      Walker.checkTypeDefaultedCollectionExpr(
          const_cast<CollectionExpr *>(collection));
    }
  }
}


/// Diagnose recursive use of properties within their own accessors
static void diagRecursivePropertyAccess(const Expr *E, const DeclContext *DC) {
  auto fn = dyn_cast<AccessorDecl>(DC);
  if (!fn)
    return;

  auto var = dyn_cast<VarDecl>(fn->getStorage());
  if (!var)  // Ignore subscripts
    return;

  class DiagnoseWalker : public ASTWalker {
    ASTContext &Ctx;
    VarDecl *Var;
    const AccessorDecl *Accessor;

  public:
    explicit DiagnoseWalker(VarDecl *var, const AccessorDecl *Accessor)
      : Ctx(var->getASTContext()), Var(var), Accessor(Accessor) {}

    /// Return true if this is an implicit reference to self.
    static bool isImplicitSelfUse(Expr *E) {
      auto *DRE = dyn_cast<DeclRefExpr>(E);
      return DRE && DRE->isImplicit() && isa<VarDecl>(DRE->getDecl()) &&
             cast<VarDecl>(DRE->getDecl())->isSelfParameter();
    }

    bool shouldWalkIntoSeparatelyCheckedClosure(ClosureExpr *expr) override {
      return false;
    }

    bool shouldWalkCaptureInitializerExpressions() override { return true; }

    MacroWalking getMacroWalkingBehavior() const override {
      return MacroWalking::Expansion;
    }

    PreWalkResult<Expr *> walkToExprPre(Expr *E) override {
      Expr *subExpr;
      bool isStore = false;

      if (auto *AE = dyn_cast<AssignExpr>(E)) {
        subExpr = AE->getDest();
        
        // If we couldn't flatten this expression, don't explode.
        if (!subExpr)
          return Action::Continue(E);

        isStore = true;
      } else if (auto *IOE = dyn_cast<InOutExpr>(E)) {
        subExpr = IOE->getSubExpr();
        isStore = true;
      } else {
        subExpr = E;
      }

      if (auto *BOE = dyn_cast<BindOptionalExpr>(subExpr))
        subExpr = BOE;

      if (auto *DRE = dyn_cast<DeclRefExpr>(subExpr)) {
        if (DRE->getDecl() == Var) {
          // Handle local and top-level computed variables.
          if (DRE->getAccessSemantics() == AccessSemantics::Ordinary) {
            bool shouldDiagnose = false;
            // Warn about any property access in the getter.
            if (Accessor->isGetter())
              shouldDiagnose = !isStore;
            // Warn about stores in the setter, but allow loads.
            if (Accessor->isSetter())
              shouldDiagnose = isStore;

            // But silence the warning if the base was explicitly qualified.
            auto parentAsExpr = Parent.getAsExpr();
            if (isa_and_nonnull<DotSyntaxBaseIgnoredExpr>(parentAsExpr))
              shouldDiagnose = false;

            if (shouldDiagnose) {
              Ctx.Diags.diagnose(subExpr->getLoc(),
                                 diag::recursive_accessor_reference,
                                 Var->getName(), Accessor->isSetter());
            }
          }
          
          // If this is a direct store in a "willSet", we reject this because
          // it is about to get overwritten.
          if (isStore &&
              DRE->getAccessSemantics() == AccessSemantics::DirectToStorage &&
              Accessor->getAccessorKind() == AccessorKind::WillSet) {
            Ctx.Diags.diagnose(E->getLoc(), diag::store_in_willset,
                               Var->getName());
          }
        }


      } else if (auto *MRE = dyn_cast<MemberRefExpr>(subExpr)) {
        // Handle instance and type computed variables.
        // Find MemberRefExprs that have an implicit "self" base.
        if (MRE->getMember().getDecl() == Var &&
            isa<DeclRefExpr>(MRE->getBase()) &&
            isImplicitSelfUse(MRE->getBase())) {
          
          if (MRE->getAccessSemantics() == AccessSemantics::Ordinary) {
            bool shouldDiagnose = false;
            // Warn about any property access in the getter.
            if (Accessor->isGetter())
              shouldDiagnose = !isStore;
            // Warn about stores in the setter, but allow loads.
            if (Accessor->isSetter())
              shouldDiagnose = isStore;

            if (shouldDiagnose) {
              Ctx.Diags.diagnose(subExpr->getLoc(),
                                 diag::recursive_accessor_reference,
                                 Var->getName(), Accessor->isSetter());
              Ctx.Diags.diagnose(subExpr->getLoc(),
                                 diag::recursive_accessor_reference_silence)
              .fixItInsert(subExpr->getStartLoc(), "self.");
            }
          }

          // If this is a direct store in a "willSet", we reject this because
          // it is about to get overwritten.
          if (isStore &&
              MRE->getAccessSemantics() == AccessSemantics::DirectToStorage &&
              Accessor->getAccessorKind() == AccessorKind::WillSet) {
            Ctx.Diags.diagnose(subExpr->getLoc(), diag::store_in_willset,
                               Var->getName());
          }
        }

      }

      return Action::Continue(E);
    }
  };

  DiagnoseWalker walker(var, fn);
  const_cast<Expr *>(E)->walk(walker);
}

/// Whether or not this closure captures self weakly
static bool closureHasWeakSelfCapture(const AbstractClosureExpr *ACE) {
  if (auto closureExpr = dyn_cast<ClosureExpr>(ACE)) {
    if (auto selfDecl = closureExpr->getCapturedSelfDecl()) {
      return selfDecl->getInterfaceType()->is<WeakStorageType>();
    }
  }

  return false;
}

// Returns true if this is an implicit self expr
static bool isImplicitSelf(const Expr *E) {
  auto *DRE = dyn_cast<DeclRefExpr>(E);

  if (!DRE || !DRE->isImplicit())
    return false;

  ASTContext &Ctx = DRE->getDecl()->getASTContext();
  return DRE->getDecl()->getName().isSimpleName(Ctx.Id_self);
}

/// Look for any property references in closures that lack a 'self.' qualifier.
/// Within a closure, we require that the source code contain 'self.' explicitly
/// (or that the closure explicitly capture 'self' in the capture list) because
/// 'self' is captured, not the property value.  This is a common source of
/// confusion, so we force an explicit self.
static void diagnoseImplicitSelfUseInClosure(const Expr *E,
                                             const DeclContext *DC) {
  class DiagnoseWalker : public BaseDiagnosticWalker {
    ASTContext &Ctx;
    SmallVector<AbstractClosureExpr *, 4> Closures;

    /// A list of "implicit self" exprs from shorthand conditions
    /// like `if let self` or `guard let self`. These conditions
    /// have an RHS 'self' decl that is implicit, but this is not
    /// the sort of "implicit self" decl that should trigger
    /// these diagnostics.
    SmallPtrSet<Expr *, 16> UnwrapStmtImplicitSelfExprs;

  public:
    explicit DiagnoseWalker(ASTContext &Ctx, AbstractClosureExpr *ACE)
        : Ctx(Ctx), Closures() {
      if (ACE)
        Closures.push_back(ACE);
    }

    static bool isEnclosingSelfReference(VarDecl *var,
                                   const AbstractClosureExpr *inClosure) {
      if (var->isSelfParameter())
        return true;

      // Capture variables have a DC of the parent function.
      if (inClosure && var->isSelfParamCapture() &&
          var->getDeclContext() != inClosure->getParent())
        return true;

      return false;
    }

    /// Return true if this is an implicit reference to self which is required
    /// to be explicit in an escaping closure. Metatype references and value
    /// type references are excluded.
    static bool isImplicitSelfParamUseLikelyToCauseCycle(
        Expr *E, const AbstractClosureExpr *inClosure) {
      if (!isImplicitSelf(E)) {
        return false;
      }

      auto *DRE = dyn_cast<DeclRefExpr>(E);

      // If this is an explicit `weak self` capture, then implicit self is
      // allowed once the closure's self param is unwrapped. We need to validate
      // that the unwrapped `self` decl specifically refers to an unwrapped copy
      // of the closure's `self` param, and not something else like in `guard
      // let self = .someOptionalVariable else { return }` or `let self =
      // someUnrelatedVariable`. If self hasn't been unwrapped yet and is still
      // an optional, we would have already hit an error elsewhere.
      if (closureHasWeakSelfCapture(inClosure)) {
        return !implicitWeakSelfReferenceIsValid(DRE, inClosure);
      }

      // Defensive check for type. If the expression doesn't have type here, it
      // should have been diagnosed somewhere else.
      Type ty = DRE->getType();
      assert(ty && "Implicit self parameter ref without type");
      if (!ty)
        return false;

      // Metatype self captures don't extend the lifetime of an object.
      if (ty->is<MetatypeType>())
        return false;

      // If self does not have reference semantics, it is very unlikely that
      // capturing it will create a reference cycle.
      if (!ty->hasReferenceSemantics())
        return false;

      if (auto closureExpr = dyn_cast<ClosureExpr>(inClosure)) {
        if (auto selfDecl = closureExpr->getCapturedSelfDecl()) {
          // If this capture is using the name `self` actually referring
          // to some other variable (e.g. with `[self = "hello"]`)
          // then implicit self is not allowed.
          if (!selfDecl->isSelfParamCapture()) {
            return true;
          }
        }
      }

      if (auto var = dyn_cast<VarDecl>(DRE->getDecl())) {
        if (!isEnclosingSelfReference(var, inClosure)) {
          return false;
        }
      }

      return true;
    }

    static bool
    implicitWeakSelfReferenceIsValid(const DeclRefExpr *DRE,
                                     const AbstractClosureExpr *inClosure) {
      ASTContext &Ctx = DRE->getDecl()->getASTContext();

      // Check if the implicit self decl refers to a var in a conditional stmt
      LabeledConditionalStmt *conditionalStmt = nullptr;
      if (auto var = dyn_cast<VarDecl>(DRE->getDecl())) {
        if (auto parentStmt = var->getParentPatternStmt()) {
          conditionalStmt = dyn_cast<LabeledConditionalStmt>(parentStmt);
        }
      }

      if (!conditionalStmt) {
        return false;
      }

      // Require `LoadExpr`s when validating the self binding.
      // This lets us reject invalid examples like:
      //
      //   let `self` = self ?? .somethingElse
      //   guard let self = self else { return }
      //   method() // <- implicit self is not allowed
      //
      return conditionalStmt->rebindsSelf(Ctx, /*requireLoadExpr*/ true);
    }

    /// Return true if this is a closure expression that will require explicit
    /// use or capture of "self." for qualification of member references.
    static bool
    isClosureRequiringSelfQualification(const AbstractClosureExpr *CE,
                                        ASTContext &Ctx) {
      // If this closure capture self weakly, then we have to validate each
      // usage of implicit self individually, even in a nonescaping closure.
      //
      // We can only do this in Swift 6 mode, since we didn't do this in Swift 5
      // (and changing this behavior causes new errors to be emitted).
      if (Ctx.LangOpts.isSwiftVersionAtLeast(6) &&
          closureHasWeakSelfCapture(CE)) {
        return true;
      }

      // If the closure's type was inferred to be noescape, then it doesn't
      // need qualification.
      if (AnyFunctionRef(const_cast<AbstractClosureExpr *>(CE))
               .isKnownNoEscape())
        return false;

      if (auto autoclosure = dyn_cast<AutoClosureExpr>(CE)) {
        if (autoclosure->getThunkKind() == AutoClosureExpr::Kind::AsyncLet)
          return false;
      }

      // If the closure was used in a context where it's explicitly stated
      // that it does not need "self." qualification, don't require it.
      if (auto closure = dyn_cast<ClosureExpr>(CE)) {
        if (closure->allowsImplicitSelfCapture())
          return false;
      }

      return true;
    }

    bool shouldWalkCaptureInitializerExpressions() override { return true; }

    PreWalkResult<Expr *> walkToExprPre(Expr *E) override {
      if (auto *CE = dyn_cast<AbstractClosureExpr>(E)) {
        // If this is a potentially-escaping closure expression, start looking
        // for references to self if we aren't already. But if this closure
        // captures self weakly, then we have to validate each usage of implicit
        // self individually, even in a nonescaping closure
        if (isClosureRequiringSelfQualification(CE, Ctx) ||
            closureHasWeakSelfCapture(CE)) {
          Closures.push_back(CE);
        }
      }

      // If we aren't in a closure, no diagnostics will be produced.
      if (Closures.size() == 0)
        return Action::Continue(E);

      // Diagnostics should correct the innermost closure
      auto *ACE = Closures[Closures.size() - 1];
      assert(ACE);

      auto &Diags = Ctx.Diags;

      // If this is an "implicit self" expr from the RHS of a shorthand
      // condition like `guard let self` or `if let self`, then this is
      // always allowed and we shouldn't run any diagnostics.
      if (UnwrapStmtImplicitSelfExprs.count(E)) {
        return Action::Continue(E);
      }

      // Until Swift 6, only emit a warning when we get this with an
      // explicit capture, since we used to not diagnose this at all.
      auto shouldOnlyWarn = [&](Expr *selfRef) {
        // If this implicit self decl is from a closure that captured self
        // weakly, then we should always emit an error, since implicit self was
        // only allowed starting in Swift 5.8 and later.
        if (closureHasWeakSelfCapture(ACE)) {
          // Implicit self was incorrectly permitted for weak self captures
          // in non-escaping closures in Swift 5.7, so in that case we can
          // only warn until Swift 6.
          return !isClosureRequiringSelfQualification(ACE, Ctx);
        }

        // We know that isImplicitSelfParamUseLikelyToCauseCycle is true,
        // which means all these casts are valid.
        return !cast<VarDecl>(cast<DeclRefExpr>(selfRef)->getDecl())
                    ->isSelfParameter();
      };

      SourceLoc memberLoc = SourceLoc();
      if (auto *MRE = dyn_cast<MemberRefExpr>(E))
        if (isImplicitSelfParamUseLikelyToCauseCycle(MRE->getBase(), ACE)) {
          auto baseName = MRE->getMember().getDecl()->getBaseName();
          memberLoc = MRE->getLoc();
          Diags.diagnose(memberLoc,
                         diag::property_use_in_closure_without_explicit_self,
                         baseName.getIdentifier())
               .warnUntilSwiftVersionIf(shouldOnlyWarn(MRE->getBase()), 6);
        }

      // Handle method calls with a specific diagnostic + fixit.
      if (auto *DSCE = dyn_cast<DotSyntaxCallExpr>(E))
        if (isImplicitSelfParamUseLikelyToCauseCycle(DSCE->getBase(), ACE) &&
            isa<DeclRefExpr>(DSCE->getFn())) {
          auto MethodExpr = cast<DeclRefExpr>(DSCE->getFn());
          memberLoc = DSCE->getLoc();
          Diags.diagnose(DSCE->getLoc(),
                         diag::method_call_in_closure_without_explicit_self,
                         MethodExpr->getDecl()->getBaseIdentifier())
               .warnUntilSwiftVersionIf(shouldOnlyWarn(DSCE->getBase()), 6);
        }

      if (memberLoc.isValid()) {
        emitFixIts(Diags, memberLoc, ACE);
        return Action::SkipChildren(E);
      }
      
      if (isImplicitSelfParamUseLikelyToCauseCycle(E, ACE))
        Diags.diagnose(E->getLoc(), diag::implicit_use_of_self_in_closure)
             .warnUntilSwiftVersionIf(shouldOnlyWarn(E), 6);

      return Action::Continue(E);
    }

    PostWalkResult<Expr *> walkToExprPost(Expr *E) override {
      auto *ACE = dyn_cast<AbstractClosureExpr>(E);
      if (!ACE) {
        return Action::Continue(E);
      }

      if (isClosureRequiringSelfQualification(ACE, Ctx) ||
          closureHasWeakSelfCapture(ACE)) {
        assert(Closures.size() > 0);
        Closures.pop_back();
      }

      return Action::Continue(E);
    }

    PreWalkResult<Stmt *> walkToStmtPre(Stmt *S) override {
      /// Conditions like `if let self` or `guard let self`
      /// have an RHS 'self' decl that is implicit, but this is not
      /// the sort of "implicit self" decl that should trigger
      /// these diagnostics. Track these DREs in a list so we can
      /// avoid running diagnostics on them when we see them later.
      auto conditionalStmt = dyn_cast<LabeledConditionalStmt>(S);
      if (!conditionalStmt) {
        return Action::Continue(S);
      }

      for (auto cond : conditionalStmt->getCond()) {
        if (cond.getKind() != StmtConditionElement::CK_PatternBinding) {
          continue;
        }

        if (auto OSP = dyn_cast<OptionalSomePattern>(cond.getPattern())) {
          if (OSP->getSubPattern()->getBoundName() != Ctx.Id_self) {
            continue;
          }

          if (auto LE = dyn_cast<LoadExpr>(cond.getInitializer())) {
            if (auto LHS = LE->getSubExpr()) {
              if (isImplicitSelf(LHS)) {
                UnwrapStmtImplicitSelfExprs.insert(LHS);
              }
            }
          }
        }
      }

      return Action::Continue(S);
    }

    /// Emit any fix-its for this error.
    void emitFixIts(DiagnosticEngine &Diags,
                    SourceLoc memberLoc,
                    const AbstractClosureExpr *ACE) {
      // This error can be fixed by either capturing self explicitly (if in an
      // explicit closure), or referencing self explicitly.
      if (auto *CE = dyn_cast<const ClosureExpr>(ACE)) {
        if (diagnoseAlmostMatchingCaptures(Diags, memberLoc, CE)) {
          // Bail on the rest of the diagnostics. Offering the option to
          // capture 'self' explicitly will result in an error, and using
          // 'self.' explicitly will be accessing something other than the
          // self param.
          return;
        }
        emitFixItsForExplicitClosure(Diags, memberLoc, CE);
      } else {
        // If this wasn't an explicit closure, just offer the fix-it to
        // reference self explicitly.
        Diags.diagnose(memberLoc, diag::note_reference_self_explicitly)
          .fixItInsert(memberLoc, "self.");
      }
    }
    
    /// Diagnose any captures which might have been an attempt to capture
    /// \c self strongly, but do not actually enable implicit \c self. Returns
    /// whether there were any such captures to diagnose.
    bool diagnoseAlmostMatchingCaptures(DiagnosticEngine &Diags,
                                        SourceLoc memberLoc,
                                        const ClosureExpr *closureExpr) {
      // If we've already captured something with the name "self" other than
      // the actual self param, offer special diagnostics.
      if (auto *VD = closureExpr->getCapturedSelfDecl()) {
        if (!VD->getInterfaceType()->is<WeakStorageType>()) {
          Diags.diagnose(VD->getLoc(), diag::note_other_self_capture);
        }
        
        return true;
      }
      return false;
    }

    /// Emit fix-its for invalid use of implicit \c self in an explicit closure.
    /// The error can be solved by capturing self explicitly,
    /// or by using \c self. explicitly.
    void emitFixItsForExplicitClosure(DiagnosticEngine &Diags,
                                      SourceLoc memberLoc,
                                      const ClosureExpr *closureExpr) {
      Diags.diagnose(memberLoc, diag::note_reference_self_explicitly)
        .fixItInsert(memberLoc, "self.");
      auto diag = Diags.diagnose(closureExpr->getLoc(),
                                 diag::note_capture_self_explicitly);
      // There are four different potential fix-its to offer based on the
      // closure signature:
      //   1. There is an existing capture list which already has some
      //      entries. We need to insert 'self' into the capture list along
      //      with a separating comma.
      //   2. There is an existing capture list, but it is empty (just '[]').
      //      We can just insert 'self'.
      //   3. Arguments or types are already specified in the signature,
      //      but there is no existing capture list. We will need to insert
      //      the capture list, but 'in' will already be present.
      //   4. The signature empty so far. We must insert the full capture
      //      list as well as 'in'.
      const auto brackets = closureExpr->getBracketRange();
      if (brackets.isValid()) {
        emitInsertSelfIntoCaptureListFixIt(brackets, diag);
      }
      else {
        emitInsertNewCaptureListFixIt(closureExpr, diag);
      }
    }

    /// Emit a fix-it for inserting \c self into in existing capture list, along
    /// with a trailing comma if needed. The fix-it will be attached to the
    /// provided diagnostic \c diag.
    void emitInsertSelfIntoCaptureListFixIt(SourceRange brackets,
                                            InFlightDiagnostic &diag) {
      // Look for any non-comment token. If there's anything before the
      // closing bracket, we assume that it is a valid capture list entry and
      // insert 'self,'. If it wasn't a valid entry, then we will at least not
      // be introducing any new errors/warnings...
      const auto locAfterBracket = brackets.Start.getAdvancedLoc(1);
      const auto nextAfterBracket =
          Lexer::getTokenAtLocation(Ctx.SourceMgr, locAfterBracket,
                                    CommentRetentionMode::None);
      if (nextAfterBracket.getLoc() != brackets.End)
        diag.fixItInsertAfter(brackets.Start, "self, ");
      else
        diag.fixItInsertAfter(brackets.Start, "self");
    }

    /// Emit a fix-it for inserting a capture list into a closure that does not
    /// already have one, along with a trailing \c in if necessary. The fix-it
    /// will be attached to the provided diagnostic \c diag.
    void emitInsertNewCaptureListFixIt(const ClosureExpr *closureExpr,
                                       InFlightDiagnostic &diag) {
      if (closureExpr->getInLoc().isValid()) {
        diag.fixItInsertAfter(closureExpr->getLoc(), " [self]");
        return;
      }

      // If there's a (non-comment) token immediately following the
      // opening brace of the closure, we may need to pad the fix-it
      // with a space.
      const auto nextLoc = closureExpr->getLoc().getAdvancedLoc(1);
      const auto next =
      Lexer::getTokenAtLocation(Ctx.SourceMgr, nextLoc,
                                CommentRetentionMode::None);
      std::string trailing = next.getLoc() == nextLoc ? " " : "";

      diag.fixItInsertAfter(closureExpr->getLoc(), " [self] in" + trailing);
    }
  };

  auto &ctx = DC->getASTContext();
  AbstractClosureExpr *ACE = nullptr;
  if (DC->isLocalContext()) {
    while (DC->getParent()->isLocalContext() && !ACE) {
      if (auto *closure = dyn_cast<AbstractClosureExpr>(DC))
        if (DiagnoseWalker::isClosureRequiringSelfQualification(closure, ctx))
          ACE = const_cast<AbstractClosureExpr *>(closure);
      DC = DC->getParent();
    }
  }
  
  const_cast<Expr *>(E)->walk(DiagnoseWalker(ctx, ACE));
}

bool TypeChecker::getDefaultGenericArgumentsString(
    SmallVectorImpl<char> &buf,
    const swift::GenericTypeDecl *typeDecl,
    llvm::function_ref<Type(const GenericTypeParamDecl *)> getPreferredType) {
  llvm::raw_svector_ostream genericParamText{buf};
  genericParamText << "<";

  auto printGenericParamSummary =
      [&](GenericTypeParamType *genericParamTy) {
    const GenericTypeParamDecl *genericParam = genericParamTy->getDecl();
    if (Type result = getPreferredType(genericParam)) {
      result.print(genericParamText);
      return;
    }

    auto contextTy = typeDecl->mapTypeIntoContext(genericParamTy);
    if (auto archetypeTy = contextTy->getAs<ArchetypeType>()) {
      SmallVector<Type, 2> members;

      bool hasExplicitAnyObject = archetypeTy->requiresClass();
      if (auto superclass = archetypeTy->getSuperclass()) {
        hasExplicitAnyObject = false;
        members.push_back(superclass);
      }

      for (auto proto : archetypeTy->getConformsTo()) {
        members.push_back(proto->getDeclaredInterfaceType());
        if (proto->requiresClass())
          hasExplicitAnyObject = false;
      }

      if (hasExplicitAnyObject)
        members.push_back(typeDecl->getASTContext().getAnyObjectConstraint());

      auto type = ProtocolCompositionType::get(typeDecl->getASTContext(),
                                               members, hasExplicitAnyObject);

      if (type->isObjCExistentialType() || type->isAny()) {
        genericParamText << type;
        return;
      }

      genericParamText << "<#" << genericParam->getName() << ": ";
      genericParamText << type << "#>";
      return;
    }

    genericParamText << contextTy;
  };

  llvm::interleave(typeDecl->getInnermostGenericParamTypes(),
                   printGenericParamSummary,
                   [&] { genericParamText << ", "; });
  
  genericParamText << ">";
  return true;
}

/// Diagnose an argument labeling issue, returning true if we successfully
/// diagnosed the issue.
bool swift::diagnoseArgumentLabelError(ASTContext &ctx,
                                       const ArgumentList *argList,
                                       ArrayRef<Identifier> newNames,
                                       ParameterContext paramContext,
                                       InFlightDiagnostic *existingDiag) {
  llvm::Optional<InFlightDiagnostic> diagOpt;
  auto getDiag = [&]() -> InFlightDiagnostic & {
    if (existingDiag)
      return *existingDiag;
    return *diagOpt;
  };

  auto &diags = ctx.Diags;
  argList = argList->getOriginalArgs();

  // Figure out how many extraneous, missing, and wrong labels are in
  // the call.
  unsigned numExtra = 0, numMissing = 0, numWrong = 0;
  unsigned n = std::max(argList->size(), (unsigned)newNames.size());

  llvm::SmallString<16> missingBuffer;
  llvm::SmallString<16> extraBuffer;
  for (unsigned i = 0; i != n; ++i) {
    // oldName and newName are
    //  - None if i is out of bounds for the argument list
    //  - nullptr for an argument without a label
    //  - have a value if the argument has a label
    llvm::Optional<Identifier> oldName;
    if (i < argList->size())
      oldName = argList->getLabel(i);
    llvm::Optional<Identifier> newName;
    if (i < newNames.size())
      newName = newNames[i];

    assert(oldName || newName && "We can't have oldName and newName out of "
                                 "bounds, otherwise n would be smaller");

    if (oldName == newName || argList->isUnlabeledTrailingClosureIndex(i))
      continue;

    if (!oldName.has_value() && newName.has_value()) {
      ++numMissing;
      missingBuffer += newName->str();
      missingBuffer += ':';
    } else if (oldName.has_value() && !newName.has_value()) {
      ++numExtra;
      extraBuffer += oldName->str();
      extraBuffer += ':';
    } else if (oldName->empty()) {
      // In the cases from here onwards oldValue and newValue are not null
      ++numMissing;
      missingBuffer += newName->str();
      missingBuffer += ":";
    } else if (newName->empty()) {
      ++numExtra;
      extraBuffer += oldName->str();
      extraBuffer += ':';
    } else {
      ++numWrong;
    }
  }

  // Emit the diagnostic.
  assert(numMissing > 0 || numExtra > 0 || numWrong > 0);
  llvm::SmallString<16> haveBuffer; // note: diagOpt has references to this
  llvm::SmallString<16> expectedBuffer; // note: diagOpt has references to this

  // If we had any wrong labels, or we have both missing and extra labels,
  // emit the catch-all "wrong labels" diagnostic.
  if (!existingDiag) {
    bool plural = (numMissing + numExtra + numWrong) > 1;
    if (numWrong > 0 || (numMissing > 0 && numExtra > 0)) {
      for (unsigned i = 0, n = argList->size(); i != n; ++i) {
        auto haveName = argList->getLabel(i);
        if (haveName.empty())
          haveBuffer += '_';
        else
          haveBuffer += haveName.str();
        haveBuffer += ':';
      }

      for (auto expected : newNames) {
        if (expected.empty())
          expectedBuffer += '_';
        else
          expectedBuffer += expected.str();
        expectedBuffer += ':';
      }

      StringRef haveStr = haveBuffer;
      StringRef expectedStr = expectedBuffer;
      diagOpt.emplace(diags.diagnose(argList->getLoc(),
                                     diag::wrong_argument_labels,
                                     plural, haveStr, expectedStr,
                                     static_cast<unsigned>(paramContext)));
    } else if (numMissing > 0) {
      StringRef missingStr = missingBuffer;
      diagOpt.emplace(diags.diagnose(argList->getLoc(),
                                     diag::missing_argument_labels,
                                     plural, missingStr,
                                     static_cast<unsigned>(paramContext)));
    } else {
      assert(numExtra > 0);
      StringRef extraStr = extraBuffer;
      diagOpt.emplace(diags.diagnose(argList->getLoc(),
                                     diag::extra_argument_labels,
                                     plural, extraStr,
                                     static_cast<unsigned>(paramContext)));
    }
  }

  // Emit Fix-Its to correct the names.
  auto &diag = getDiag();
  for (unsigned i = 0, n = argList->size(); i != n; ++i) {
    Identifier oldName = argList->getLabel(i);
    Identifier newName;
    if (i < newNames.size())
      newName = newNames[i];

    if (oldName == newName || argList->isUnlabeledTrailingClosureIndex(i))
      continue;

    if (newName.empty()) {
      // If this is a labeled trailing closure, we need to replace with '_'.
      if (argList->isLabeledTrailingClosureIndex(i)) {
        diag.fixItReplace(argList->getLabelLoc(i), "_");
        continue;
      }

      // Otherwise, delete the old name.
      diag.fixItRemoveChars(argList->getLabelLoc(i),
                            argList->getExpr(i)->getStartLoc());
      continue;
    }

    bool newNameIsReserved = !canBeArgumentLabel(newName.str());
    llvm::SmallString<16> newStr;
    if (newNameIsReserved)
      newStr += "`";
    newStr += newName.str();
    if (newNameIsReserved)
      newStr += "`";

    // If the argument was previously unlabeled, insert the new label. Note that
    // we don't do this for labeled trailing closures as they write unlabeled
    // args as '_:', and therefore need replacement.
    if (oldName.empty() && !argList->isLabeledTrailingClosureIndex(i)) {
      // Insert the name.
      newStr += ": ";
      diag.fixItInsert(argList->getExpr(i)->getStartLoc(), newStr);
      continue;
    }

    // Change the name.
    diag.fixItReplace(argList->getLabelLoc(i), newStr);
  }

  // If the diagnostic is local, flush it before returning.
  // This makes sure it's emitted before the message text buffers are destroyed.
  diagOpt.reset();
  return true;
}

static const Expr *lookThroughExprsToImmediateDeallocation(const Expr *E) {
  // Look through various expressions that don't affect the fact that the user
  // will be assigning a class instance that will be immediately deallocated.
  while (true) {
    E = E->getValueProvidingExpr();

    // We don't currently deal with tuple destructuring.
    if (isa<DestructureTupleExpr>(E))
      return E;

    // If we have a TupleElementExpr with a child TupleExpr, dig into that
    // element.
    if (auto *TEE = dyn_cast<TupleElementExpr>(E)) {
      auto *subExpr = lookThroughExprsToImmediateDeallocation(TEE->getBase());
      if (auto *TE = dyn_cast<TupleExpr>(subExpr)) {
        auto *element = TE->getElements()[TEE->getFieldNumber()];
        return lookThroughExprsToImmediateDeallocation(element);
      }
      return subExpr;
    }

    if (auto *ICE = dyn_cast<ImplicitConversionExpr>(E)) {
      E = ICE->getSubExpr();
      continue;
    }
    if (auto *CE = dyn_cast<CoerceExpr>(E)) {
      E = CE->getSubExpr();
      continue;
    }
    if (auto *OEE = dyn_cast<OpenExistentialExpr>(E)) {
      E = OEE->getSubExpr();
      continue;
    }

    // Look through optional evaluations, we still want to diagnose on
    // things like initializers called through optional chaining and the
    // unwrapping of failable initializers.
    if (auto *OEE = dyn_cast<OptionalEvaluationExpr>(E)) {
      E = OEE->getSubExpr();
      continue;
    }
    if (auto *OBE = dyn_cast<BindOptionalExpr>(E)) {
      E = OBE->getSubExpr();
      continue;
    }
    if (auto *FOE = dyn_cast<ForceValueExpr>(E)) {
      E = FOE->getSubExpr();
      continue;
    }

    if (auto *ATE = dyn_cast<AnyTryExpr>(E)) {
      E = ATE->getSubExpr();
      continue;
    }
    if (auto *DSBIE = dyn_cast<DotSyntaxBaseIgnoredExpr>(E)) {
      E = DSBIE->getRHS();
      continue;
    }
    return E;
  }
}

static void diagnoseUnownedImmediateDeallocationImpl(ASTContext &ctx,
                                                     const VarDecl *varDecl,
                                                     const Expr *initExpr,
                                                     SourceLoc diagLoc,
                                                     SourceRange diagRange) {
  auto *ownershipAttr =
      varDecl->getAttrs().getAttribute<ReferenceOwnershipAttr>();
  if (!ownershipAttr || ownershipAttr->isInvalid())
    return;

  // Only diagnose for non-owning ownerships such as 'weak' and 'unowned'.
  // Zero is the default/strong ownership strength.
  if (ReferenceOwnership::Strong == ownershipAttr->get() ||
      isLessStrongThan(ReferenceOwnership::Strong, ownershipAttr->get()))
    return;

  // Try to find a call to a constructor.
  initExpr = lookThroughExprsToImmediateDeallocation(initExpr);
  auto *CE = dyn_cast<CallExpr>(initExpr);
  if (!CE)
    return;

  auto *CRCE = dyn_cast<ConstructorRefCallExpr>(CE->getFn());
  if (!CRCE)
    return;

  auto *DRE = dyn_cast<DeclRefExpr>(CRCE->getFn());
  if (!DRE)
    return;

  auto *constructorDecl = dyn_cast<ConstructorDecl>(DRE->getDecl());
  if (!constructorDecl)
    return;

  // Make sure the constructor constructs an instance that allows ownership.
  // This is to ensure we don't diagnose on constructors such as
  // Optional.init(nilLiteral:).
  auto selfType = constructorDecl->getDeclContext()->getSelfTypeInContext();
  if (!selfType->allowsOwnership())
    return;

  // This must stay in sync with
  // diag::unowned_assignment_immediate_deallocation.
  enum {
    SK_Variable = 0,
    SK_Property
  } storageKind = SK_Variable;

  if (varDecl->getDeclContext()->isTypeContext())
    storageKind = SK_Property;

  // TODO: The DiagnoseLifetimeIssuesPass prints a similar warning in this
  // situation. We should only print one warning.

  ctx.Diags.diagnose(diagLoc, diag::unowned_assignment_immediate_deallocation,
                     varDecl->getName(), ownershipAttr->get(),
                     unsigned(storageKind))
    .highlight(diagRange);

  ctx.Diags.diagnose(diagLoc, diag::unowned_assignment_requires_strong)
    .highlight(diagRange);

  ctx.Diags.diagnose(varDecl, diag::decl_declared_here, varDecl);
}

void swift::diagnoseUnownedImmediateDeallocation(ASTContext &ctx,
                                                 const AssignExpr *assignExpr) {
  auto *destExpr = assignExpr->getDest()->getValueProvidingExpr();
  auto *initExpr = assignExpr->getSrc();

  // Try to find a referenced VarDecl.
  const VarDecl *VD = nullptr;
  if (auto *DRE = dyn_cast<DeclRefExpr>(destExpr)) {
    VD = dyn_cast<VarDecl>(DRE->getDecl());
  } else if (auto *MRE = dyn_cast<MemberRefExpr>(destExpr)) {
    VD = dyn_cast<VarDecl>(MRE->getMember().getDecl());
  }

  if (VD)
    diagnoseUnownedImmediateDeallocationImpl(ctx, VD, initExpr,
                                             assignExpr->getLoc(),
                                             initExpr->getSourceRange());
}

void swift::diagnoseUnownedImmediateDeallocation(ASTContext &ctx,
                                                 const Pattern *pattern,
                                                 SourceLoc equalLoc,
                                                 const Expr *initExpr) {
  pattern = pattern->getSemanticsProvidingPattern();

  if (auto *TP = dyn_cast<TuplePattern>(pattern)) {
    initExpr = lookThroughExprsToImmediateDeallocation(initExpr);

    // If we've found a matching tuple initializer with the same number of
    // elements as our pattern, diagnose each element individually.
    auto TE = dyn_cast<TupleExpr>(initExpr);
    if (TE && TE->getNumElements() == TP->getNumElements()) {
      for (unsigned i = 0, e = TP->getNumElements(); i != e; ++i) {
        const TuplePatternElt &elt = TP->getElement(i);
        const Pattern *subPattern = elt.getPattern();
        Expr *subInitExpr = TE->getElement(i);

        diagnoseUnownedImmediateDeallocation(ctx, subPattern, equalLoc,
                                             subInitExpr);
      }
    }
  } else if (auto *NP = dyn_cast<NamedPattern>(pattern)) {
    diagnoseUnownedImmediateDeallocationImpl(ctx, NP->getDecl(), initExpr,
                                             equalLoc,
                                             initExpr->getSourceRange());
  }
}

namespace {
enum NoteKind_t {
  FixItReplace,
  FixItInsert,
};

static bool fixItOverrideDeclarationTypesImpl(
    ValueDecl *decl, const ValueDecl *base,
    SmallVectorImpl<std::tuple<NoteKind_t, SourceRange, std::string>> &notes) {
  // For now, just rewrite cases where the base uses a value type and the
  // override uses a reference type, and the value type is bridged to the
  // reference type. This is a way to migrate code that makes use of types
  // that previously were not bridged to value types.
  auto checkValueReferenceType =
      [&](Type overrideTy, ParamDecl::Specifier overrideSpec,
          Type baseTy, ParamDecl::Specifier baseSpec,
          SourceRange typeRange) -> bool {
    if (typeRange.isInvalid())
      return false;

    auto normalizeType = [](Type &ty, ParamDecl::Specifier spec) -> Type {
      Type normalizedTy = ty;
      if (Type unwrappedTy = normalizedTy->getOptionalObjectType())
        normalizedTy = unwrappedTy;
      if (spec == ParamDecl::Specifier::InOut)
        ty = InOutType::get(ty);
      return normalizedTy;
    };

    // Is the base type bridged?
    Type normalizedBaseTy = normalizeType(baseTy, baseSpec);
    const DeclContext *DC = decl->getDeclContext();

    ASTContext &ctx = decl->getASTContext();

    // ...and just knowing that it's bridged isn't good enough if we don't
    // know what it's bridged /to/. Also, don't do this check for trivial
    // bridging---that doesn't count.
    Type bridged;
    if (normalizedBaseTy->isAny()) {
      bridged = ctx.getAnyObjectType();
    } else {
      bridged = ctx.getBridgedToObjC(DC, normalizedBaseTy);
    }
    if (!bridged || bridged->isEqual(normalizedBaseTy))
      return false;

    // ...and is it bridged to the overridden type?
    Type normalizedOverrideTy = normalizeType(overrideTy, overrideSpec);
    if (!bridged->isEqual(normalizedOverrideTy)) {
      // If both are nominal types, check again, ignoring generic arguments.
      auto *overrideNominal = normalizedOverrideTy->getAnyNominal();
      if (!overrideNominal || bridged->getAnyNominal() != overrideNominal) {
        return false;
      }
    }

    Type newOverrideTy = baseTy;

    // Preserve optionality if we're dealing with a simple type.
    if (Type unwrappedTy = newOverrideTy->getOptionalObjectType())
      newOverrideTy = unwrappedTy;
    if (overrideTy->getOptionalObjectType())
      newOverrideTy = OptionalType::get(newOverrideTy);

    SmallString<32> baseTypeBuf;
    llvm::raw_svector_ostream baseTypeStr(baseTypeBuf);
    PrintOptions options;
    options.SynthesizeSugarOnTypes = true;

    newOverrideTy->print(baseTypeStr, options);
    notes.emplace_back(FixItReplace, typeRange, baseTypeStr.str().str());
    return true;
  };

  // Check if overriding fails because we lack @escaping attribute on the function
  // type repr.
  auto checkTypeMissingEscaping = [&](Type overrideTy, Type baseTy,
                                      SourceRange typeRange) -> bool {
    // Fix-it needs position to apply.
    if (typeRange.isInvalid())
      return false;
    auto overrideFnTy = overrideTy->getAs<AnyFunctionType>();
    auto baseFnTy = baseTy->getAs<AnyFunctionType>();

    // Both types should be function.
    if (overrideFnTy && baseFnTy &&
        // The overriding function type should be no escaping.
        overrideFnTy->getExtInfo().isNoEscape() &&
        // The overridden function type should be escaping.
        !baseFnTy->getExtInfo().isNoEscape()) {
      notes.emplace_back(FixItInsert, typeRange, "@escaping ");
      return true;
    }
    return false;
  };

  auto checkType = [&](Type overrideTy, ParamDecl::Specifier overrideSpec,
                       Type baseTy, ParamDecl::Specifier baseSpec,
                       SourceRange typeRange) -> bool {
    return checkValueReferenceType(overrideTy, overrideSpec,
                                   baseTy, baseSpec, typeRange) ||
      checkTypeMissingEscaping(overrideTy, baseTy, typeRange);
  };

  if (auto *param = dyn_cast<ParamDecl>(decl)) {
    SourceRange typeRange = param->getTypeSourceRangeForDiagnostics();
    auto *baseParam = cast<ParamDecl>(base);
    return checkType(param->getInterfaceType(), param->getSpecifier(),
                     baseParam->getInterfaceType(), baseParam->getSpecifier(),
                     typeRange);
  }

  if (auto *var = dyn_cast<VarDecl>(decl)) {
    SourceRange typeRange = var->getTypeSourceRangeForDiagnostics();
    auto *baseVar = cast<VarDecl>(base);
    return checkType(var->getInterfaceType(), ParamDecl::Specifier::Default,
                     baseVar->getInterfaceType(), ParamDecl::Specifier::Default,
                     typeRange);
  }

  if (auto *fn = dyn_cast<AbstractFunctionDecl>(decl)) {
    auto *baseFn = cast<AbstractFunctionDecl>(base);
    bool fixedAny = false;
    if (fn->getParameters()->size() ==
        baseFn->getParameters()->size()) {
      for_each(*fn->getParameters(),
               *baseFn->getParameters(),
               [&](ParamDecl *param, const ParamDecl *baseParam) {
        fixedAny |= fixItOverrideDeclarationTypesImpl(param, baseParam, notes);
      });
    }
    if (auto *method = dyn_cast<FuncDecl>(decl)) {
      auto resultType = method->mapTypeIntoContext(
          method->getResultInterfaceType());

      auto *baseMethod = cast<FuncDecl>(base);
      auto baseResultType = baseMethod->mapTypeIntoContext(
          baseMethod->getResultInterfaceType());

      fixedAny |= checkType(resultType, ParamDecl::Specifier::Default,
                            baseResultType, ParamDecl::Specifier::Default,
                            method->getResultTypeSourceRange());
    }
    return fixedAny;
  }

  if (auto *subscript = dyn_cast<SubscriptDecl>(decl)) {
    auto *baseSubscript = cast<SubscriptDecl>(base);
    bool fixedAny = false;
    if (subscript->getIndices()->size() ==
        baseSubscript->getIndices()->size()) {
      for_each(*subscript->getIndices(),
               *baseSubscript->getIndices(),
               [&](ParamDecl *param, const ParamDecl *baseParam) {
        fixedAny |= fixItOverrideDeclarationTypesImpl(param, baseParam, notes);
      });
    }

    auto resultType =
        subscript->mapTypeIntoContext(subscript->getElementInterfaceType());
    auto baseResultType = baseSubscript->mapTypeIntoContext(
        baseSubscript->getElementInterfaceType());
    fixedAny |= checkType(resultType, ParamDecl::Specifier::Default,
                          baseResultType, ParamDecl::Specifier::Default,
                          subscript->getElementTypeSourceRange());
    return fixedAny;
  }

  llvm_unreachable("unknown overridable member");
}
}

bool swift::computeFixitsForOverriddenDeclaration(
    ValueDecl *decl, const ValueDecl *base,
    llvm::function_ref<llvm::Optional<InFlightDiagnostic>(bool)> diag) {
  SmallVector<std::tuple<NoteKind_t, SourceRange, std::string>, 4> Notes;
  bool hasNotes = ::fixItOverrideDeclarationTypesImpl(decl, base, Notes);

  llvm::Optional<InFlightDiagnostic> diagnostic = diag(hasNotes);
  if (!diagnostic) return hasNotes;

  for (const auto &note : Notes) {
    if (std::get<0>(note) == FixItReplace) {
      diagnostic->fixItReplace(std::get<1>(note), std::get<2>(note));
    } else {
      diagnostic->fixItInsert(std::get<1>(note).Start, std::get<2>(note));
    }
  }
  return hasNotes;
}

//===----------------------------------------------------------------------===//
// Per func/init diagnostics
//===----------------------------------------------------------------------===//

namespace {

class VarDeclUsageChecker : public ASTWalker {
  DeclContext *DC;

  DiagnosticEngine &Diags;
  // Keep track of some information about a variable.
  enum {
    RK_Defined     = 1,      ///< Whether it was ever defined in this scope.
    RK_Read        = 2,      ///< Whether it was ever read.
    RK_Written     = 4,      ///< Whether it was ever written or passed inout.
    
    RK_CaptureList = 8       ///< Var is an entry in a capture list.
  };
  
  /// These are all of the variables that we are tracking.  VarDecls get added
  /// to this when the declaration is seen.  We use a MapVector to keep the
  /// diagnostics emission in deterministic order.
  llvm::SmallMapVector<VarDecl*, unsigned, 32> VarDecls;

  /// This is a mapping from an OpaqueValue to the expression that initialized
  /// it.
  llvm::SmallDenseMap<OpaqueValueExpr *, Expr *> OpaqueValueMap;

  /// The first reference to the given property.
  llvm::SmallDenseMap<VarDecl *, Expr *> AssociatedGetterRefExpr;

  /// This is a mapping from VarDecls to the if/while/guard statement that they
  /// occur in, when they are in a pattern in a StmtCondition.
  llvm::SmallDenseMap<VarDecl*, LabeledConditionalStmt*> StmtConditionForVD;

#ifndef NDEBUG
  llvm::SmallPtrSet<Expr*, 32> AllExprsSeen;
#endif
  
  bool sawError = false;
  
  VarDeclUsageChecker(const VarDeclUsageChecker &) = delete;
  void operator=(const VarDeclUsageChecker &) = delete;

public:
  VarDeclUsageChecker(DeclContext *DC,
                      DiagnosticEngine &Diags) : DC(DC), Diags(Diags) {}

  // After we have scanned the entire region, diagnose variables that could be
  // declared with a narrower usage kind.
  ~VarDeclUsageChecker() override;

  /// Check to see if the specified VarDecl is part of a larger
  /// PatternBindingDecl, where some other bound variable was mutated.  In this
  /// case we don't want to generate a "variable never mutated" warning, because
  /// it would require splitting up the destructuring of the tuple, which is
  ///  more code turmoil than the warning is worth.
  bool isVarDeclPartOfPBDThatHadSomeMutation(VarDecl *VD) {
    auto *PBD = VD->getParentPatternBinding();
    if (!PBD) return false;

    bool sawMutation = false;
    for (auto idx : range(PBD->getNumPatternEntries())) {
      PBD->getPattern(idx)->forEachVariable([&](VarDecl *VD) {
        auto it = VarDecls.find(VD);
        sawMutation |= it != VarDecls.end() && (it->second & RK_Written);
      });
    }
    return sawMutation;
  }

  bool shouldTrackVarDecl(VarDecl *VD) {
    // If the variable is implicit, ignore it.
    if (VD->isImplicit() || VD->getLoc().isInvalid())
      return false;
    
    // If the variable is computed, ignore it.
    if (!VD->hasStorage())
      return false;
    
    // If the variable was invalid, ignore it and notice that the code is
    // malformed.
    if (VD->isInvalid()) {
      sawError = true;
      return false;
    }
    
    // If the variable is already unnamed, ignore it.
    if (!VD->hasName() || VD->getName().str() == "_")
      return false;

    return true;
  }

  void addMark(Decl *D, unsigned Flag) {
    auto *vd = dyn_cast<VarDecl>(D);
    if (!vd) return;

    VarDecls[vd] |= Flag;
  }

  void markBaseOfStorageUse(Expr *E, ConcreteDeclRef decl, unsigned flags);
  void markBaseOfStorageUse(Expr *E, bool isMutating);
  
  void markStoredOrInOutExpr(Expr *E, unsigned Flags);

  MacroWalking getMacroWalkingBehavior() const override {
    return MacroWalking::ArgumentsAndExpansion;
  }

  // We generally walk into declarations, other than types and nested functions.
  // FIXME: peek into capture lists of nested functions.
  PreWalkAction walkToDeclPre(Decl *D) override {
    if (isa<TypeDecl>(D))
      return Action::SkipChildren();

    // The body of #if clauses are not walked into, we need custom processing
    // for them.
    if (auto *ICD = dyn_cast<IfConfigDecl>(D))
      handleIfConfig(ICD);
      
    // If this is a VarDecl, then add it to our list of things to track.
    if (auto *vd = dyn_cast<VarDecl>(D)) {
      if (shouldTrackVarDecl(vd)) {
        unsigned flags = RK_Defined;
        if (vd->isCaptureList())
          flags |= RK_CaptureList;

        if (auto childVd = vd->getCorrespondingCaseBodyVariable()) {
          // Child vars are never in capture lists.
          assert(flags == RK_Defined);
          addMark(childVd.get(), flags);
        }
        addMark(vd, flags);
      }
    }

    // Don't walk into implicit accessors, since eg. an observer's setter
    // references the variable, but we don't want to consider it as a real
    // "use".
    if (isa<AccessorDecl>(D) && D->isImplicit())
      return Action::SkipChildren();

    if (auto *afd = dyn_cast<AbstractFunctionDecl>(D)) {
      // If this AFD is a setter, track the parameter and the getter for
      // the containing property so if newValue isn't used but the getter is used
      // an error can be reported.
      if (auto FD = dyn_cast<AccessorDecl>(afd)) {
        if (FD->getAccessorKind() == AccessorKind::Set) {
          if (isa<VarDecl>(FD->getStorage())) {
            auto arguments = FD->getParameters();
            VarDecls[arguments->get(0)] = RK_Defined;
          }
        }
      }

      if (afd->isBodyTypeChecked())
        return Action::Continue();

      // Don't walk into a body that has not yet been type checked. This should
      // only occur for top-level code.
      VarDecls.clear();
      return Action::SkipChildren();
    }

    if (auto *TLCD = dyn_cast<TopLevelCodeDecl>(D)) {
      // If this is a TopLevelCodeDecl, scan for global variables
      auto *body = TLCD->getBody();
      for (auto node : body->getElements()) {
        if (node.is<Decl *>()) {
          // Flag all variables in a PatternBindingDecl
          Decl *D = node.get<Decl *>();
          auto *PBD = dyn_cast<PatternBindingDecl>(D);
          if (!PBD) continue;
          for (auto idx : range(PBD->getNumPatternEntries())) {
            PBD->getPattern(idx)->forEachVariable([&](VarDecl *VD) {
              VarDecls[VD] = RK_Read|RK_Written|RK_Defined;
            });
          }
        } else if (node.is<Stmt *>()) {
          // Flag all variables in guard statements
          Stmt *S = node.get<Stmt *>();
          auto *GS = dyn_cast<GuardStmt>(S);
          if (!GS) continue;
          for (StmtConditionElement SCE : GS->getCond()) {
            if (auto pattern = SCE.getPatternOrNull()) {
              pattern->forEachVariable([&](VarDecl *VD) {
                VarDecls[VD] = RK_Read|RK_Written|RK_Defined;
              });
            }
          }
        }
      }
    }

    // Note that we ignore the initialization behavior of PatternBindingDecls,
    // but we do want to walk into them, because we want to see any uses or
    // other things going on in the initializer expressions.
    return Action::Continue();
  }

  /// The heavy lifting happens when visiting expressions.
  PreWalkResult<Expr *> walkToExprPre(Expr *E) override;

  /// handle #if directives.
  void handleIfConfig(IfConfigDecl *ICD);

  /// Custom handling for statements.
  PreWalkResult<Stmt *> walkToStmtPre(Stmt *S) override {
    // Keep track of an association between vardecls and the StmtCondition that
    // they are bound in for IfStmt, GuardStmt, WhileStmt, etc.
    if (auto LCS = dyn_cast<LabeledConditionalStmt>(S)) {
      for (auto &cond : LCS->getCond()) {
        if (auto pat = cond.getPatternOrNull()) {
          pat->forEachVariable([&](VarDecl *VD) {
            StmtConditionForVD[VD] = LCS;
          });
        }
      }
    }
    
    // A fallthrough dest case's bound variable means the source case's
    // var of the same name is read.
    if (auto *fallthroughStmt = dyn_cast<FallthroughStmt>(S)) {
      if (auto *sourceCase = fallthroughStmt->getFallthroughSource()) {
        SmallVector<VarDecl *, 4> sourceVars;
        auto sourcePattern = sourceCase->getCaseLabelItems()[0].getPattern();
        sourcePattern->collectVariables(sourceVars);
        
        auto destCase = fallthroughStmt->getFallthroughDest();
        auto destPattern = destCase->getCaseLabelItems()[0].getPattern();
        destPattern->forEachVariable([&](VarDecl *V) {
          if (!V->hasName())
            return;
          for (auto *var : sourceVars) {
            if (var->hasName() && var->getName() == V->getName()) {
              VarDecls[var] |= RK_Read;
              break;
            }
          }
        });
      }
    }

    // Make sure that we setup our case body variables.
    if (auto *caseStmt = dyn_cast<CaseStmt>(S)) {
      for (auto *vd : caseStmt->getCaseBodyVariablesOrEmptyArray()) {
        VarDecls[vd] |= RK_Defined;
      }
    }

    return Action::Continue(S);
  }
};
  
/// An AST walker that determines the underlying type of an opaque return decl
/// from its associated function body.
class OpaqueUnderlyingTypeChecker : public ASTWalker {
  using Candidate = std::tuple<Expr *, SubstitutionMap, /*isUnique=*/bool>;
  using AvailabilityContext = IfStmt *;

  ASTContext &Ctx;
  AbstractFunctionDecl *Implementation;
  OpaqueTypeDecl *OpaqueDecl;
  BraceStmt *Body;

  /// A set of all candidates with unique signatures.
  SmallPtrSet<const void *, 4> UniqueSignatures;

  /// Represents a current availability context. `nullptr` means that
  /// there are no restrictions.
  AvailabilityContext CurrentAvailability = nullptr;

  /// All of the candidates together with their availability.
  ///
  /// If a candidate is found in non-`if #available` context or
  /// `if #available` has other dynamic conditions, it covers 'all'
  /// versions and the context is set to `nullptr`.
  SmallVector<std::pair<AvailabilityContext, Candidate>, 4> Candidates;

  bool HasInvalidReturn = false;

public:
  OpaqueUnderlyingTypeChecker(AbstractFunctionDecl *Implementation,
                              OpaqueTypeDecl *OpaqueDecl, BraceStmt *Body)
      : Ctx(Implementation->getASTContext()), Implementation(Implementation),
        OpaqueDecl(OpaqueDecl), Body(Body) {}

  void check() {
    Body->walk(*this);

    // If given function has any invalid `return`s in the body
    // let's not try to validate the types, since it wouldn't
    // be accurate.
    if (HasInvalidReturn)
      return;

    // If there are no candidates, then the body has no return statements, and
    // we have nothing to infer the underlying type from.
    if (Candidates.empty()) {
      Implementation->diagnose(diag::opaque_type_no_underlying_type_candidates);

      // We try to find if the last element of the `Body` multi element
      // `BraceStmt` is an expression that produces a value that satisfies all
      // the opaque type requirements and if that is the case, it means we can
      // suggest a fix-it note to add an explicit `return`.
      if (Body->getNumElements() > 1) {
        auto element = Body->getLastElement();
        // Let's see if the last statement would make for a valid return value.
        if (auto expr = element.dyn_cast<Expr *>()) {
          auto exprType = expr->getType();
          // Function body might not be valid and we cannot reply on
          // \c typeCheckStmt here to propagate HadError because its
          // unreliable.
          if (!exprType || exprType->hasError())
            return;

          bool conforms = llvm::all_of(
              OpaqueDecl->getOpaqueInterfaceGenericSignature()
                  .getRequirements(),
              [&exprType, this](auto requirement) {
                if (requirement.getKind() == RequirementKind::Conformance) {
                  auto conformance = TypeChecker::conformsToProtocol(
                      exprType->getRValueType(), requirement.getProtocolDecl(),
                      Implementation->getModuleContext(),
                      /*allowMissing=*/false);
                  return !conformance.isInvalid();
                }
                // If we encounter any requirements other than `Conformance`, we
                // do not attempt to type check the expression.
                return false;
              });

          // If all requirements are fulfilled, we offer to insert `return` to
          // fix the issue.
          if (conforms) {
            Ctx.Diags
                .diagnose(expr->getStartLoc(),
                          diag::opaque_type_missing_return_last_expr_note)
                .fixItInsert(expr->getStartLoc(), "return ");
          }
        }
      }

      return;
    }

    if (Candidates.size() == 1) {
      finalizeUnique(Candidates.front().second);
      return;
    }

    // Check whether all of the underlying type candidates match up.
    // TODO [OPAQUE SUPPORT]: diagnose multiple opaque types

    // There is a single unique signature, which means that all returns
    // matched.
    if (llvm::count_if(Candidates, [](const auto &entry) {
          const auto &candidate = entry.second;
          return std::get<2>(candidate); // isUnique field.
        }) == 1) {
      finalizeUnique(Candidates.front().second);
      return;
    }

    SmallVector<Candidate, 4> universallyUniqueCandidates;

    for (const auto &entry : Candidates) {
      AvailabilityContext availability = entry.first;
      const auto &candidate = entry.second;

      // Unique candidate without availability context.
      if (!availability && std::get<2>(candidate))
        universallyUniqueCandidates.push_back(candidate);
    }

    // TODO(diagnostics): Need a tailored diagnostic for this case.
    if (universallyUniqueCandidates.empty()) {
      Implementation->diagnose(diag::opaque_type_no_underlying_type_candidates);
      return;
    }

    // If there is a single universally available unique candidate
    // the underlying type would have to be determined at runtime
    // based on the results of availability checks.
    if (universallyUniqueCandidates.size() == 1) {
      finalizeOpaque(universallyUniqueCandidates.front());
      return;
    }

    // A list of all mismatches discovered across all candidates.
    // If there are any mismatches in availability contexts, they
    // are not diagnosed but propagated to the declaration.
    llvm::Optional<std::pair<unsigned, GenericTypeParamType *>> mismatch;

    auto opaqueParams = OpaqueDecl->getOpaqueGenericParams();
    SubstitutionMap underlyingSubs = std::get<1>(Candidates.front().second);

    for (auto index : indices(opaqueParams)) {
      auto *genericParam = opaqueParams[index];

      Type underlyingType = Type(genericParam).subst(underlyingSubs);
      bool found = false;
      for (const auto &candidate : universallyUniqueCandidates) {
        Type otherType = Type(genericParam).subst(std::get<1>(candidate));

        if (!underlyingType->isEqual(otherType)) {
          mismatch.emplace(index, genericParam);
          found = true;
          break;
        }
      }

      if (found)
        break;
    }

    assert(mismatch.has_value());

    if (auto genericParam =
            OpaqueDecl->getExplicitGenericParam(mismatch->first)) {
      Implementation
          ->diagnose(
              diag::opaque_type_mismatched_underlying_type_candidates_named,
              genericParam->getName())
          .highlight(genericParam->getLoc());
    } else {
      TypeRepr *opaqueRepr =
          OpaqueDecl->getOpaqueReturnTypeReprs()[mismatch->first];
      Implementation
          ->diagnose(diag::opaque_type_mismatched_underlying_type_candidates,
                     opaqueRepr)
          .highlight(opaqueRepr->getSourceRange());
    }

    for (const auto &candidate : universallyUniqueCandidates) {
      Ctx.Diags.diagnose(std::get<0>(candidate)->getLoc(),
                         diag::opaque_type_underlying_type_candidate_here,
                         Type(mismatch->second).subst(std::get<1>(candidate)));
    }
  }

  bool isSelfReferencing(const Candidate &candidate) {
    auto substitutions = std::get<1>(candidate);

    // The underlying type can't be defined recursively
    // in terms of the opaque type itself.
    auto opaqueTypeInContext = Implementation->mapTypeIntoContext(
        OpaqueDecl->getDeclaredInterfaceType());
    for (auto genericParam : OpaqueDecl->getOpaqueGenericParams()) {
      auto underlyingType = Type(genericParam).subst(substitutions);
      auto isSelfReferencing = underlyingType.findIf(
          [&](Type t) -> bool { return t->isEqual(opaqueTypeInContext); });

      if (isSelfReferencing) {
        Ctx.Diags.diagnose(std::get<0>(candidate)->getLoc(),
                           diag::opaque_type_self_referential_underlying_type,
                           underlyingType);
        return true;
      }
    }

    return false;
  }

  // A single unique underlying substitution.
  void finalizeUnique(const Candidate &candidate) {
    // If we have one successful candidate, then save it as the underlying
    // substitutions of the opaque decl.
    OpaqueDecl->setUniqueUnderlyingTypeSubstitutions(
        std::get<1>(candidate).mapReplacementTypesOutOfContext());
  }

  // There is no clear winner here since there are candidates within
  // limited availability contexts.
  void finalizeOpaque(const Candidate &universallyAvailable) {
    using AvailabilityCondition = OpaqueTypeDecl::AvailabilityCondition;

    SmallVector<OpaqueTypeDecl::ConditionallyAvailableSubstitutions *, 4>
        conditionalSubstitutions;

    for (const auto &entry : Candidates) {
      auto availabilityContext = entry.first;
      const auto &candidate = entry.second;

      if (!availabilityContext)
        continue;

      SmallVector<AvailabilityCondition, 4> conditions;

      for (const auto &elt : availabilityContext->getCond()) {
        auto condition = elt.getAvailability();

        auto availabilityRange = condition->getAvailableRange();
        // If there is no lower endpoint it means that the
        // current platform is unrelated to this condition
        // and we can ignore it.
        if (!availabilityRange.hasLowerEndpoint())
          continue;

        conditions.push_back(
            {availabilityRange, condition->isUnavailability()});
      }

      if (conditions.empty())
        continue;

      conditionalSubstitutions.push_back(
          OpaqueTypeDecl::ConditionallyAvailableSubstitutions::get(
              Ctx, conditions,
              std::get<1>(candidate).mapReplacementTypesOutOfContext()));
    }

    // Add universally available choice as the last one.
    conditionalSubstitutions.push_back(
        OpaqueTypeDecl::ConditionallyAvailableSubstitutions::get(
            Ctx, {{VersionRange::empty(), /*unavailable=*/false}},
            std::get<1>(universallyAvailable)
                .mapReplacementTypesOutOfContext()));

    OpaqueDecl->setConditionallyAvailableSubstitutions(
        conditionalSubstitutions);
  }

  MacroWalking getMacroWalkingBehavior() const override {
    return MacroWalking::Expansion;
  }

  PreWalkResult<Expr *> walkToExprPre(Expr *E) override {
    if (auto underlyingToOpaque = dyn_cast<UnderlyingToOpaqueExpr>(E)) {
      auto subMap = underlyingToOpaque->substitutions;

      auto key = subMap.getCanonical().getOpaqueValue();
      auto isUnique = UniqueSignatures.insert(key).second;

      auto candidate =
          std::make_tuple(underlyingToOpaque->getSubExpr(), subMap, isUnique);

      if (isSelfReferencing(candidate)) {
        HasInvalidReturn = true;
        return Action::Stop();
      }

      if (subMap.hasDynamicSelf()) {
        Ctx.Diags.diagnose(E->getLoc(),
                           diag::opaque_type_cannot_contain_dynamic_self);
        HasInvalidReturn = true;
        return Action::Stop();
      }

      Candidates.push_back({CurrentAvailability, candidate});
      return Action::SkipChildren(E);
    }

    return Action::Continue(E);
  }

  PreWalkResult<Stmt *> walkToStmtPre(Stmt *S) override {
    if (auto *If = dyn_cast<IfStmt>(S)) {
      if (Parent.getAsStmt() != Body) {
        // If this is not a top-level `if`, let's drop
        // contextual information that has been set previously.
        CurrentAvailability = nullptr;
        return Action::Continue(S);
      }

      // If this is `if #(un)available` statement with no other dynamic
      // conditions, let's check if it returns opaque type directly.
      if (llvm::all_of(If->getCond(), [&](const auto &condition) {
            return condition.getKind() == StmtConditionElement::CK_Availability;
          })) {
        // Check return statement directly with availability context set.
        if (auto *Then = dyn_cast<BraceStmt>(If->getThenStmt())) {
          llvm::SaveAndRestore<ParentTy> parent(Parent, Then);

          for (auto element : Then->getElements()) {
            auto *Return = getAsStmt<ReturnStmt>(element);

            // If this is not a direct return statement, walk into it
            // without setting contextual availability because we want
            // to find all `return`s.
            if (!(Return && Return->hasResult())) {
              element.walk(*this);
              continue;
            }

            // Note that we are about to walk into a return statement
            // that is located in a `if #available` without any other
            // conditions.
            llvm::SaveAndRestore<AvailabilityContext> context(
                CurrentAvailability, If);

            Return->getResult()->walk(*this);
          }
        }

        // Walk the else branch directly as well.
        if (auto *Else = If->getElseStmt()) {
          llvm::SaveAndRestore<ParentTy> parent(Parent, If);
          Else->walk(*this);
        }

        return Action::SkipChildren(S);
      }
    }

    if (auto *RS = dyn_cast<ReturnStmt>(S)) {
      if (RS->hasResult()) {
        auto resultTy = RS->getResult()->getType();
        // If expression associated with return statement doesn't have
        // a type or type has an error, checking opaque types is going
        // to produce incorrect diagnostics.
        HasInvalidReturn |= resultTy.isNull() || resultTy->hasError();
      }
    }

    return Action::Continue(S);
  }

  // Don't descend into nested decls.
  PreWalkAction walkToDeclPre(Decl *D) override {
    return Action::SkipChildren();
  }
};

class ReturnTypePlaceholderReplacer : public ASTWalker {
  FuncDecl *Implementation;
  BraceStmt *Body;
  SmallVector<Type, 4> Candidates;

  bool HasInvalidReturn = false;

public:
  ReturnTypePlaceholderReplacer(FuncDecl *Implementation, BraceStmt *Body)
      : Implementation(Implementation), Body(Body) {}

  void check() {
    auto *resultRepr = Implementation->getResultTypeRepr();
    if (!resultRepr) {
      return;
    }

    Implementation->getASTContext()
        .Diags
        .diagnose(resultRepr->getLoc(),
                  diag::placeholder_type_not_allowed_in_return_type)
        .highlight(resultRepr->getSourceRange());

    Body->walk(*this);

    // If given function has any invalid returns in the body
    // let's not try to validate the types, since it wouldn't
    // be accurate.
    if (HasInvalidReturn)
      return;

    auto writtenType = Implementation->getResultInterfaceType();
    llvm::SmallPtrSet<TypeBase *, 8> seenTypes;
    for (auto candidate : Candidates) {
      if (!seenTypes.insert(candidate.getPointer()).second) {
        continue;
      }
      TypeChecker::notePlaceholderReplacementTypes(writtenType, candidate);
    }
  }

  MacroWalking getMacroWalkingBehavior() const override {
    return MacroWalking::ArgumentsAndExpansion;
  }

  PreWalkResult<Expr *> walkToExprPre(Expr *E) override { return Action::Continue(E); }

  PreWalkResult<Stmt *> walkToStmtPre(Stmt *S) override {
    if (auto *RS = dyn_cast<ReturnStmt>(S)) {
      if (RS->hasResult()) {
        auto resultTy = RS->getResult()->getType();
        HasInvalidReturn |= resultTy.isNull() || resultTy->hasError();
        Candidates.push_back(resultTy);
      }
    }

    return Action::Continue(S);
  }

  // Don't descend into nested decls.
  PreWalkAction walkToDeclPre(Decl *D) override {
    return Action::SkipChildren();
  }
};

} // end anonymous namespace

// After we have scanned the entire region, diagnose variables that could be
// declared with a narrower usage kind.
VarDeclUsageChecker::~VarDeclUsageChecker() {
  // If we saw an ErrorExpr somewhere in the body, then we have a malformed AST
  // and we know stuff got dropped.  Instead of producing these diagnostics,
  // lets let the bigger issues get resolved first.
  if (sawError)
    return;

  for (auto p : VarDecls) {
    VarDecl *var;
    unsigned access;
    std::tie(var, access) = p;

    // If the variable was not defined in this scope, we can safely ignore it.
    if (!(access & RK_Defined))
      continue;

    if (auto *caseStmt =
            dyn_cast_or_null<CaseStmt>(var->getRecursiveParentPatternStmt())) {
      // Only diagnose VarDecls from the first CaseLabelItem in CaseStmts, as
      // the remaining items must match it anyway.
      auto caseItems = caseStmt->getCaseLabelItems();
      assert(!caseItems.empty() &&
             "If we have any case stmt var decls, we should have a case item");
      if (!caseItems.front().getPattern()->containsVarDecl(var))
        continue;

      auto *childVar = var->getCorrespondingCaseBodyVariable().get();
      access |= VarDecls[childVar];
    }

    // If the setter parameter is not used, but the property is read, report
    // a warning. Otherwise, parameters should not generate usage warnings. It
    // is common to name a parameter and not use it (e.g. because you are an
    // override or want the named keyword, etc).  Warning to rewrite it to _ is
    // more annoying than it is useful.
    if (auto param = dyn_cast<ParamDecl>(var)) {
      auto FD = dyn_cast<AccessorDecl>(param->getDeclContext());
      if (FD && FD->getAccessorKind() == AccessorKind::Set) {
        auto VD = dyn_cast<VarDecl>(FD->getStorage());
        if ((access & RK_Read) == 0) {
          auto found = AssociatedGetterRefExpr.find(VD);
          if (found != AssociatedGetterRefExpr.end()) {
            auto *DRE = found->second;
            Diags.diagnose(DRE->getLoc(), diag::unused_setter_parameter,
                           var->getName());
            Diags.diagnose(DRE->getLoc(), diag::fixit_for_unused_setter_parameter,
                           var->getName())
              .fixItReplace(DRE->getSourceRange(), var->getName().str());
          }
        }
      }
      continue;
    }

    // If this is a 'let' value, any stores to it are actually initializations,
    // not mutations.
    auto isWrittenLet = false;
    if (var->isLet()) {
      isWrittenLet = (access & RK_Written) != 0;
      access &= ~RK_Written;
    }
    
    // If this variable has WeakStorageType, then it can be mutated in ways we
    // don't know.
    if (var->getInterfaceType()->is<WeakStorageType>())
      access |= RK_Written;
    
    // Diagnose variables that were never used (other than their
    // initialization).
    //
    if ((access & (RK_Read|RK_Written)) == 0) {
      // If this is a member in a capture list, just say it is unused.  We could
      // produce a fixit hint with a parent map, but this is a lot of effort for
      // a narrow case.
      if (access & RK_CaptureList) {
        Diags.diagnose(var->getLoc(), diag::capture_never_used,
                       var->getName());
        continue;
      }
      
      // If the source of the VarDecl is a trivial PatternBinding with only a
      // single binding, rewrite the whole thing into an assignment.
      //    let x = foo()
      //  ->
      //    _ = foo()
      if (auto *pbd = var->getParentPatternBinding()) {
        if (pbd->getSingleVar() == var && pbd->getInit(0) != nullptr &&
            !isa<TypedPattern>(pbd->getPattern(0))) {
          unsigned varKind = var->isLet();
          SourceRange replaceRange(
              pbd->getStartLoc(),
              pbd->getPattern(0)->getEndLoc());
          Diags.diagnose(var->getLoc(), diag::pbd_never_used,
                         var->getName(), varKind)
            .fixItReplace(replaceRange, "_");
          continue;
        }
      }

      // If the variable is defined in a pattern in an if/while/guard statement,
      // see if we can produce a tuned fixit.  When we have something like:
      //
      //    if let x = <expr> {
      //
      // we prefer to rewrite it to:
      //
      //    if <expr> != nil {
      //
      if (auto SC = StmtConditionForVD[var]) {
        // We only handle the "if let" case right now, since it is vastly the
        // most common situation that people run into.
        if (SC->getCond().size() == 1) {
          auto pattern = SC->getCond()[0].getPattern();
          if (auto OSP = dyn_cast<OptionalSomePattern>(pattern))
            if (auto LP = dyn_cast<BindingPattern>(OSP->getSubPattern()))
              if (isa<NamedPattern>(LP->getSubPattern())) {
                auto initExpr = SC->getCond()[0].getInitializer();
                if (initExpr->getStartLoc().isValid()) {
                  unsigned noParens = initExpr->canAppendPostfixExpression();

                  // If the subexpr is an "as?" cast, we can rewrite it to
                  // be an "is" test.
                  ConditionalCheckedCastExpr *CCE = nullptr;

                  // initExpr can be wrapped inside parens or try expressions.
                  if (auto ccExpr = dyn_cast<ConditionalCheckedCastExpr>(
                          initExpr->getValueProvidingExpr())) {
                    if (!ccExpr->isImplicit()) {
                      CCE = ccExpr;
                      noParens = true;
                    }
                  }

                  // In cases where the value is optional, the cast expr is
                  // wrapped inside OptionalEvaluationExpr. Unwrap it to get
                  // ConditionalCheckedCastExpr.
                  if (auto oeExpr = dyn_cast<OptionalEvaluationExpr>(
                          initExpr->getValueProvidingExpr())) {
                    if (auto ccExpr = dyn_cast<ConditionalCheckedCastExpr>(
                            oeExpr->getSubExpr()->getValueProvidingExpr())) {
                      if (!ccExpr->isImplicit()) {
                        CCE = ccExpr;
                        noParens = true;
                      }
                    }
                  }

                  auto diagIF = Diags.diagnose(var->getLoc(),
                                               diag::pbd_never_used_stmtcond,
                                            var->getName());
                  auto introducerLoc = SC->getCond()[0].getIntroducerLoc();
                  diagIF.fixItReplaceChars(introducerLoc,
                                           initExpr->getStartLoc(),
                                           &"("[noParens]);

                  if (CCE) {
                    // If this was an "x as? T" check, rewrite it to "x is T".
                    diagIF.fixItReplace(SourceRange(CCE->getLoc(),
                                                    CCE->getQuestionLoc()),
                                        "is");
                  } else {
                    diagIF.fixItInsertAfter(initExpr->getEndLoc(),
                                            &") != nil"[noParens]);
                  }
                  continue;
                }
              }
        }
      }

      // If the variable is defined in a pattern that isn't one of the usual
      // conditional statements, try to detect and rewrite "simple" binding
      // patterns:
      //    case .pattern(let x):
      //  ->
      //    case .pattern(_):
      if (auto *pattern = var->getParentPattern()) {
        BindingPattern *foundVP = nullptr;
        pattern->forEachNode([&](Pattern *P) {
          if (auto *VP = dyn_cast<BindingPattern>(P))
            if (VP->getSingleVar() == var)
              foundVP = VP;
        });

        if (foundVP) {
          unsigned varKind = var->isLet();
          Diags
              .diagnose(var->getLoc(), diag::variable_never_used,
                        var->getName(), varKind)
              .fixItReplace(foundVP->getSourceRange(), "_");
          continue;
        }
      }
      
      // Otherwise, this is something more complex, perhaps
      //    let (a,b) = foo()
      if (isWrittenLet) {
        Diags.diagnose(var->getLoc(),
                       diag::immutable_value_never_used_but_assigned,
                       var->getName());
      } else {
        unsigned varKind = var->isLet();
        // Just rewrite the one variable with a _.
        Diags.diagnose(var->getLoc(), diag::variable_never_used,
                       var->getName(), varKind)
          .fixItReplace(var->getLoc(), "_");
      }
      continue;
    }
    
    // If this is a mutable 'var', and it was never written to, suggest
    // upgrading to 'let'.
    if (var->getIntroducer() == VarDecl::Introducer::Var
        && (access & RK_Written) == 0 &&
        // Don't warn if we have something like "let (x,y) = ..." and 'y' was
        // never mutated, but 'x' was.
        !isVarDeclPartOfPBDThatHadSomeMutation(var)) {
      SourceLoc FixItLoc;

      // Try to find the location of the 'var' so we can produce a fixit.  If
      // this is a simple PatternBinding, use its location.
      if (auto *PBD = var->getParentPatternBinding()) {
        if (PBD->getSingleVar() == var)
          FixItLoc = PBD->getLoc();
      } else if (auto *pattern = var->getParentPattern()) {
        BindingPattern *foundVP = nullptr;
        pattern->forEachNode([&](Pattern *P) {
          if (auto *VP = dyn_cast<BindingPattern>(P))
            if (VP->getSingleVar() == var)
              foundVP = VP;
        });

        if (foundVP && !foundVP->isLet())
          FixItLoc = foundVP->getLoc();
      }

      // If this is a parameter explicitly marked 'var', remove it.
      if (FixItLoc.isInvalid()) {
        Diags.diagnose(var->getLoc(), diag::variable_never_mutated,
                       var->getName(), true);
      }
      else {
        bool suggestLet = true;
        if (auto *stmt = var->getRecursiveParentPatternStmt()) {
          // Don't try to suggest 'var' -> 'let' conversion
          // in case of 'for' loop because it's an implicitly
          // immutable context.
          suggestLet = !isa<ForEachStmt>(stmt);
        }

        auto diag = Diags.diagnose(var->getLoc(), diag::variable_never_mutated,
                                   var->getName(), suggestLet);

        if (suggestLet)
          diag.fixItReplace(FixItLoc, "let");
        else
          diag.fixItRemove(FixItLoc);

        continue;
      }
    }
    
    // If this is a variable that was only written to, emit a warning.
    if ((access & RK_Read) == 0) {
      Diags.diagnose(var->getLoc(), diag::variable_never_read, var->getName());
      continue;
    }
  }
}

/// Handle a use of "x.y" or "x[0]" where 'base' is the expression for x and
/// 'decl' is the property or subscript.
///
/// TODO: Rip this out and just rely on LValueAccessKind.
void VarDeclUsageChecker::markBaseOfStorageUse(Expr *base, ConcreteDeclRef decl,
                                               unsigned flags) {
  // If the base is an rvalue, then we know that this is a non-mutating access.
  // Note that we can have mutating accesses even when the base has class or
  // metatype type due to protocols and protocol extensions.
  if (!base->getType()->hasLValueType() &&
      !base->isSemanticallyInOutExpr()) {
    base->walk(*this);
    return;
  }

  // Compute whether this access is to a mutating member.
  auto *ASD = dyn_cast_or_null<AbstractStorageDecl>(decl.getDecl());
  bool isMutating = false;
  if (!ASD) {
    // If there's no abstract storage declaration (which should hopefully
    // only happen with invalid code), treat the base access as mutating if
    // the subobject is being mutated and the base type is not a class
    // or metatype.
    if (flags & RK_Written) {
      Type type = base->getType()->getRValueType()->getInOutObjectType();
      if (!type->isAnyClassReferenceType() && !type->is<AnyMetatypeType>())
        isMutating = true;
    }
  } else {
    // Otherwise, consider whether the accessors are mutating.
    if (flags & RK_Read)
      isMutating |= ASD->isGetterMutating();
    if (flags & RK_Written)
      isMutating |= ASD->isSettable(nullptr) && ASD->isSetterMutating();
  }

  markBaseOfStorageUse(base, isMutating);
}

void VarDeclUsageChecker::markBaseOfStorageUse(Expr *base, bool isMutating) {
  // CSApply sometimes wraps the base in an InOutExpr just because the
  // base is an l-value; look through that so we can get more precise
  // checking.
  if (auto *ioe = dyn_cast<InOutExpr>(base))
    base = ioe->getSubExpr();

  if (!isMutating) {
    base->walk(*this);
    return;
  }

  // Otherwise this is a read and write of the base.
  return markStoredOrInOutExpr(base, RK_Written|RK_Read);
}


void VarDeclUsageChecker::markStoredOrInOutExpr(Expr *E, unsigned Flags) {
  // Sema leaves some subexpressions null, which seems really unfortunate.  It
  // should replace them with ErrorExpr.
  if (E == nullptr || !E->getType() || E->getType()->hasError()) {
    sawError = true;
    return;
  }
  
  // Ignore parens and other easy cases.
  E = E->getSemanticsProvidingExpr();
  
  // If we found a decl that is being assigned to, then mark it.
  if (auto *DRE = dyn_cast<DeclRefExpr>(E)) {
    addMark(DRE->getDecl(), Flags);
    return;
  }
  
  if (auto *TE = dyn_cast<TupleExpr>(E)) {
    for (auto &elt : TE->getElements())
      markStoredOrInOutExpr(elt, Flags);
    return;
  }
  
  // If this is an assignment into a mutating subscript lvalue expr, then we
  // are mutating the base expression.  We also need to visit the index
  // expressions as loads though.
  if (auto *SE = dyn_cast<SubscriptExpr>(E)) {
    // The arguments of a subscript are evaluated as rvalues.
    SE->getArgs()->walk(*this);
    markBaseOfStorageUse(SE->getBase(), SE->getDecl(), Flags);
    return;
  }
  
  // Likewise for key path applications. An application of a WritableKeyPath
  // reads and writes its base; an application of a ReferenceWritableKeyPath
  // only reads its base; the other KeyPath types cannot be written at all.
  if (auto *KPA = dyn_cast<KeyPathApplicationExpr>(E)) {
    KPA->getKeyPath()->walk(*this);

    bool isMutating =
      (Flags & RK_Written) &&
      KPA->getKeyPath()->getType()->isWritableKeyPath();
    markBaseOfStorageUse(KPA->getBase(), isMutating);
    return;
  }
  
  if (auto *ioe = dyn_cast<InOutExpr>(E))
    return markStoredOrInOutExpr(ioe->getSubExpr(), RK_Written|RK_Read);
  
  if (auto *MRE = dyn_cast<MemberRefExpr>(E)) {
    markBaseOfStorageUse(MRE->getBase(), MRE->getMember(), Flags);
    return;
  }

  if (auto *TEE = dyn_cast<TupleElementExpr>(E))
    return markStoredOrInOutExpr(TEE->getBase(), Flags);
  
  if (auto *FVE = dyn_cast<ForceValueExpr>(E))
    return markStoredOrInOutExpr(FVE->getSubExpr(), Flags);

  if (auto *BOE = dyn_cast<BindOptionalExpr>(E))
    return markStoredOrInOutExpr(BOE->getSubExpr(), Flags);

  // Bind existential expressions.
  if (auto *OEE = dyn_cast<OpenExistentialExpr>(E)) {
    OpaqueValueMap[OEE->getOpaqueValue()] = OEE->getExistentialValue();
    return markStoredOrInOutExpr(OEE->getSubExpr(), Flags);
  }

  // If this is an OpaqueValueExpr that we've seen a mapping for, jump to the
  // mapped value.
  if (auto *OVE = dyn_cast<OpaqueValueExpr>(E))
    if (auto *expr = OpaqueValueMap[OVE])
      return markStoredOrInOutExpr(expr, Flags);

  // If we don't know what kind of expression this is, assume it's a reference
  // and mark it as a read.
  E->walk(*this);
}

/// The heavy lifting happens when visiting expressions.
ASTWalker::PreWalkResult<Expr *> VarDeclUsageChecker::walkToExprPre(Expr *E) {
  STATISTIC(VarDeclUsageCheckerExprVisits,
            "# of times VarDeclUsageChecker::walkToExprPre is called");
  ++VarDeclUsageCheckerExprVisits;

  // Sema leaves some subexpressions null, which seems really unfortunate.  It
  // should replace them with ErrorExpr.
  if (E == nullptr || !E->getType() || E->getType()->hasError()) {
    sawError = true;
    return Action::SkipChildren(E);
  }

  assert(AllExprsSeen.insert(E).second && "duplicate traversal");

  // If this is a DeclRefExpr found in a random place, it is a load of the
  // vardecl.
  if (auto *DRE = dyn_cast<DeclRefExpr>(E)) {
    addMark(DRE->getDecl(), RK_Read);

    // If the Expression is a read of a getter, track for diagnostics
    if (auto VD = dyn_cast<VarDecl>(DRE->getDecl())) {
      AssociatedGetterRefExpr.insert(std::make_pair(VD, DRE));
    }
  }
  // If the Expression is a member reference, see if it is a read of the getter
  // to track for diagnostics.
  if (auto *MRE = dyn_cast<MemberRefExpr>(E)) {
    if (auto VD = dyn_cast<VarDecl>(MRE->getMember().getDecl())) {
      AssociatedGetterRefExpr.insert(std::make_pair(VD, MRE));
      markBaseOfStorageUse(MRE->getBase(), MRE->getMember(), RK_Read);
      return Action::SkipChildren(E);
    }
  }
  if (auto SE = dyn_cast<SubscriptExpr>(E)) {
    SE->getArgs()->walk(*this);
    markBaseOfStorageUse(SE->getBase(), SE->getDecl(), RK_Read);
    return Action::SkipChildren(E);
  }

  // If this is an AssignExpr, see if we're mutating something that we know
  // about.
  if (auto *assign = dyn_cast<AssignExpr>(E)) {
    markStoredOrInOutExpr(assign->getDest(), RK_Written);
    
    // Don't walk into the LHS of the assignment, only the RHS.
    assign->getSrc()->walk(*this);
    return Action::SkipChildren(E);
  }
  
  // '&x' is a read and write of 'x'.
  if (auto *io = dyn_cast<InOutExpr>(E)) {
    markStoredOrInOutExpr(io->getSubExpr(), RK_Read|RK_Written);
    // Don't bother walking into this.
    return Action::SkipChildren(E);
  }
  
  // If we see an OpenExistentialExpr, remember the mapping for its OpaqueValue
  // and only walk the subexpr.
  if (auto *oee = dyn_cast<OpenExistentialExpr>(E)) {
    OpaqueValueMap[oee->getOpaqueValue()] = oee->getExistentialValue();
    oee->getSubExpr()->walk(*this);
    return Action::SkipChildren(E);
  }

  // Visit bindings.
  if (auto ove = dyn_cast<OpaqueValueExpr>(E)) {
    if (auto mapping = OpaqueValueMap.lookup(ove))
      mapping->walk(*this);
    return Action::SkipChildren(E);
  }
  
  // If we saw an ErrorExpr, take note of this.
  if (isa<ErrorExpr>(E))
    sawError = true;

  return Action::Continue(E);
}

/// handle #if directives.  All of the active clauses are already walked by the
/// AST walker, but we also want to handle the inactive ones to avoid false
/// positives.
void VarDeclUsageChecker::handleIfConfig(IfConfigDecl *ICD) {
  struct ConservativeDeclMarker : public ASTWalker {
    VarDeclUsageChecker &VDUC;
    SourceFile *SF;

    ConservativeDeclMarker(VarDeclUsageChecker &VDUC)
      : VDUC(VDUC), SF(VDUC.DC->getParentSourceFile()) {}

    MacroWalking getMacroWalkingBehavior() const override {
      return MacroWalking::Arguments;
    }

    PostWalkResult<Expr *> walkToExprPost(Expr *E) override {
      // If we see a bound reference to a decl in an inactive #if block, then
      // conservatively mark it read and written.  This will silence "variable
      // unused" and "could be marked let" warnings for it.
      if (auto *DRE = dyn_cast<DeclRefExpr>(E))
        VDUC.addMark(DRE->getDecl(), RK_Read | RK_Written);
      else if (auto *declRef = dyn_cast<UnresolvedDeclRefExpr>(E)) {
        auto name = declRef->getName();
        auto loc = declRef->getLoc();
        if (name.isSimpleName() && loc.isValid()) {
          auto *varDecl = dyn_cast_or_null<VarDecl>(
            ASTScope::lookupSingleLocalDecl(SF, name.getFullName(), loc));
          if (varDecl)
            VDUC.addMark(varDecl, RK_Read|RK_Written);
        }
      }
      return Action::Continue(E);
    }
  };

  for (auto &clause : ICD->getClauses()) {
    // Active clauses are handled by the normal AST walk.
    if (clause.isActive) continue;

    for (auto elt : clause.Elements)
      elt.walk(ConservativeDeclMarker(*this));
  }
}

namespace {
class SingleValueStmtUsageChecker final : public ASTWalker {
  ASTContext &Ctx;
  DiagnosticEngine &Diags;
  llvm::DenseSet<SingleValueStmtExpr *> ValidSingleValueStmtExprs;

public:
  SingleValueStmtUsageChecker(ASTContext &ctx) : Ctx(ctx), Diags(ctx.Diags) {}

private:
  /// Mark a given expression as a valid position for a SingleValueStmtExpr.
  void markValidSingleValueStmt(Expr *E) {
    if (!E)
      return;

    if (auto *SVE = SingleValueStmtExpr::tryDigOutSingleValueStmtExpr(E))
      ValidSingleValueStmtExprs.insert(SVE);
  }

  MacroWalking getMacroWalkingBehavior() const override {
    return MacroWalking::Expansion;
  }

  AssignExpr *findAssignment(Expr *E) const {
    // Don't consider assignments if we have a parent expression (as otherwise
    // this would be effectively allowing it in an arbitrary expression
    // position).
    if (Parent.getAsExpr())
      return nullptr;

    // Look through optional exprs, which are present for e.g x?.y = z, as
    // we wrap the entire assign in the optional evaluation of the destination.
    if (auto *OEE = dyn_cast<OptionalEvaluationExpr>(E)) {
      E = OEE->getSubExpr();
      while (auto *IIO = dyn_cast<InjectIntoOptionalExpr>(E))
        E = IIO->getSubExpr();
    }
    return dyn_cast<AssignExpr>(E);
  }

  PreWalkResult<Expr *> walkToExprPre(Expr *E) override {
    if (auto *SVE = dyn_cast<SingleValueStmtExpr>(E)) {
      // Diagnose a SingleValueStmtExpr in a context that we do not currently
      // support.
      if (!ValidSingleValueStmtExprs.contains(SVE)) {
        Diags.diagnose(SVE->getLoc(), diag::single_value_stmt_out_of_place,
                       SVE->getStmt()->getKind());
      }

      // Nested SingleValueStmtExprs are allowed.
      SmallVector<Expr *, 4> scratch;
      for (auto *branch : SVE->getSingleExprBranches(scratch))
        markValidSingleValueStmt(branch);

      // Diagnose invalid SingleValueStmtExprs. This should only happen for
      // expressions in positions that we didn't support before
      // (e.g assignment or *explicit* return).
      auto *S = SVE->getStmt();
      auto mayProduceSingleValue = S->mayProduceSingleValue(Ctx);
      switch (mayProduceSingleValue.getKind()) {
      case IsSingleValueStmtResult::Kind::Valid:
        break;
      case IsSingleValueStmtResult::Kind::UnterminatedBranches: {
        for (auto *branch : mayProduceSingleValue.getUnterminatedBranches()) {
          if (auto *BS = dyn_cast<BraceStmt>(branch)) {
            if (BS->empty()) {
              Diags.diagnose(branch->getStartLoc(),
                             diag::single_value_stmt_branch_empty,
                             S->getKind());
              continue;
            }
          }
          Diags.diagnose(branch->getEndLoc(),
                         diag::single_value_stmt_branch_must_end_in_throw,
                         S->getKind());
        }
        break;
      }
      case IsSingleValueStmtResult::Kind::NonExhaustiveIf: {
        Diags.diagnose(S->getStartLoc(),
                       diag::if_expr_must_be_syntactically_exhaustive);
        break;
      }
      case IsSingleValueStmtResult::Kind::HasLabel: {
        // FIXME: We should offer a fix-it to remove (currently we don't track
        // the colon SourceLoc).
        auto label = cast<LabeledStmt>(S)->getLabelInfo();
        Diags.diagnose(label.Loc,
                       diag::single_value_stmt_must_be_unlabeled, S->getKind())
          .highlight(label.Loc);
        break;
      }
      case IsSingleValueStmtResult::Kind::InvalidJumps: {
        // Diagnose each invalid jump.
        for (auto *jump : mayProduceSingleValue.getInvalidJumps()) {
          Diags.diagnose(jump->getStartLoc(),
                         diag::cannot_jump_in_single_value_stmt,
                         jump->getKind(), S->getKind())
            .highlight(jump->getSourceRange());
        }
        break;
      }
      case IsSingleValueStmtResult::Kind::NoExpressionBranches:
        // This is fine, we will have typed the expression as Void (we verify
        // as such in the ASTVerifier).
        break;
      case IsSingleValueStmtResult::Kind::CircularReference:
        // Already diagnosed.
        break;
      case IsSingleValueStmtResult::Kind::UnhandledStmt:
        break;
      }
      return Action::Continue(E);
    }

    // Valid as the source of an assignment.
    if (auto *AE = findAssignment(E))
      markValidSingleValueStmt(AE->getSrc());

    // Valid as a single expression body of a closure. This is needed in
    // addition to ReturnStmt checking, as we will remove the return if the
    // expression is inferred to be Never.
    if (auto *ACE = dyn_cast<ClosureExpr>(E)) {
      if (ACE->hasSingleExpressionBody())
        markValidSingleValueStmt(ACE->getSingleExpressionBody());
    }
    return Action::Continue(E);
  }

  PreWalkResult<Stmt *> walkToStmtPre(Stmt *S) override {
    // Valid in a return/throw.
    if (auto *RS = dyn_cast<ReturnStmt>(S)) {
      if (RS->hasResult())
        markValidSingleValueStmt(RS->getResult());
    }
    if (auto *TS = dyn_cast<ThrowStmt>(S))
      markValidSingleValueStmt(TS->getSubExpr());

    return Action::Continue(S);
  }

  PreWalkAction walkToDeclPre(Decl *D) override {
    // Valid as an initializer of a pattern binding.
    if (auto *PBD = dyn_cast<PatternBindingDecl>(D)) {
      for (auto idx : range(PBD->getNumPatternEntries()))
        markValidSingleValueStmt(PBD->getInit(idx));
    }
    // Valid as a single expression body of a function. This is needed in
    // addition to ReturnStmt checking, as we will remove the return if the
    // expression is inferred to be Never.
    if (auto *AFD = dyn_cast<AbstractFunctionDecl>(D)) {
      if (AFD->hasSingleExpressionBody())
        markValidSingleValueStmt(AFD->getSingleExpressionBody());
    }
    return Action::Continue();
  }
};
} // end anonymous namespace

/// Apply the warnings managed by VarDeclUsageChecker to the top level
/// code declarations that haven't been checked yet.
void swift::
performTopLevelDeclDiagnostics(TopLevelCodeDecl *TLCD) {
  auto &ctx = TLCD->getDeclContext()->getASTContext();
  VarDeclUsageChecker checker(TLCD, ctx.Diags);
  TLCD->walk(checker);
  SingleValueStmtUsageChecker sveChecker(ctx);
  TLCD->walk(sveChecker);
}

/// Perform diagnostics for func/init/deinit declarations.
void swift::performAbstractFuncDeclDiagnostics(AbstractFunctionDecl *AFD) {
  // Don't produce these diagnostics for implicitly generated code.
  if (AFD->getLoc().isInvalid() || AFD->isImplicit() || AFD->isInvalid())
    return;

  if (!AFD->getDeclContext()->isLocalContext()) {
    // Check for unused variables, as well as variables that are could be
    // declared as constants. Skip local functions though, since they will
    // be checked as part of their parent function or TopLevelCodeDecl.
    auto &ctx = AFD->getDeclContext()->getASTContext();
    VarDeclUsageChecker checker(AFD, ctx.Diags);
    AFD->walk(checker);

    // Do a similar walk to check for out of place SingleValueStmtExprs.
    SingleValueStmtUsageChecker sveChecker(ctx);
    AFD->walk(sveChecker);
  }

  auto *body = AFD->getBody();

  // If the function has an opaque return type, check the return expressions
  // to determine the underlying type.
  if (auto opaqueResultTy = AFD->getOpaqueResultTypeDecl()) {
    OpaqueUnderlyingTypeChecker(AFD, opaqueResultTy, body).check();
  } else if (auto accessor = dyn_cast<AccessorDecl>(AFD)) {
    if (accessor->isGetter()) {
      if (auto opaqueResultTy
                          = accessor->getStorage()->getOpaqueResultTypeDecl()) {
        OpaqueUnderlyingTypeChecker(AFD, opaqueResultTy, body).check();
      }
    }
  } else if (auto *FD = dyn_cast<FuncDecl>(AFD)) {
    auto resultIFaceTy = FD->getResultInterfaceType();
    // If the result has a placeholder, we need to try to use the contextual
    // type inferred in the body to replace it.
    if (resultIFaceTy && resultIFaceTy->hasPlaceholder()) {
      ReturnTypePlaceholderReplacer(FD, body).check();
    }
  }
}

static void
diagnoseMoveOnlyPatternMatchSubject(ASTContext &C,
                                    Expr *subjectExpr) {
  // For now, move-only types must use the `consume` operator to be
  // pattern matched. Pattern matching is only implemented as a consuming
  // operation today, but we don't want to be stuck with that as the default
  // in the fullness of time when we get borrowing pattern matching later.
  
  // Don't bother if the subject wasn't given a valid type, or is a copyable
  // type.
  auto subjectType = subjectExpr->getType();
  if (!subjectType
      || subjectType->hasError()
      || !subjectType->isPureMoveOnly()) {
    return;
  }

  // A bare reference to, or load from, a move-only binding must be consumed.
  subjectExpr = subjectExpr->getSemanticsProvidingExpr();
  if (auto load = dyn_cast<LoadExpr>(subjectExpr)) {
    subjectExpr = load->getSubExpr()->getSemanticsProvidingExpr();
  }
  if (isa<DeclRefExpr>(subjectExpr)) {
    C.Diags.diagnose(subjectExpr->getLoc(),
                           diag::move_only_pattern_match_not_consumed)
      .fixItInsert(subjectExpr->getStartLoc(), "consume ");
  }
}

// Perform MiscDiagnostics on Switch Statements.
static void checkSwitch(ASTContext &ctx, const SwitchStmt *stmt,
                        DeclContext *DC) {
  diagnoseMoveOnlyPatternMatchSubject(ctx, stmt->getSubjectExpr());
                        
  // We want to warn about "case .Foo, .Bar where 1 != 100:" since the where
  // clause only applies to the second case, and this is surprising.
  for (auto cs : stmt->getCases()) {
    TypeChecker::checkExistentialTypes(ctx, cs, DC);

    // The case statement can have multiple case items, each can have a where.
    // If we find a "where", and there is a preceding item without a where, and
    // if they are on the same source line, then warn.
    auto items = cs->getCaseLabelItems();
    
    // Don't do any work for the vastly most common case.
    if (items.size() == 1) continue;
    
    // Ignore the first item, since it can't have preceding ones.
    for (unsigned i = 1, e = items.size(); i != e; ++i) {
      // Must have a where clause.
      auto where = items[i].getGuardExpr();
      if (!where)
        continue;
      
      // Preceding item must not.
      if (items[i-1].getGuardExpr())
        continue;
      
      // Must be on the same source line.
      auto prevLoc = items[i-1].getStartLoc();
      auto thisLoc = items[i].getStartLoc();
      if (prevLoc.isInvalid() || thisLoc.isInvalid())
        continue;
      
      auto &SM = ctx.SourceMgr;
      auto prevLineCol = SM.getLineAndColumnInBuffer(prevLoc);
      if (SM.getLineAndColumnInBuffer(thisLoc).first != prevLineCol.first)
        continue;

      ctx.Diags.diagnose(items[i].getWhereLoc(), diag::where_on_one_item)
        .highlight(items[i].getPattern()->getSourceRange())
        .highlight(where->getSourceRange());
      
      // Whitespace it out to the same column as the previous item.
      std::string whitespace(prevLineCol.second-1, ' ');
      ctx.Diags.diagnose(thisLoc, diag::add_where_newline)
        .fixItInsert(thisLoc, "\n"+whitespace);

      auto whereRange = SourceRange(items[i].getWhereLoc(),
                                    where->getEndLoc());
      auto charRange = Lexer::getCharSourceRangeFromSourceRange(SM, whereRange);
      auto whereText = SM.extractText(charRange);
      ctx.Diags.diagnose(prevLoc, diag::duplicate_where)
        .fixItInsertAfter(items[i-1].getEndLoc(), " " + whereText.str())
        .highlight(items[i-1].getSourceRange());
    }
  }
}

void swift::fixItEncloseTrailingClosure(ASTContext &ctx,
                                        InFlightDiagnostic &diag,
                                        const CallExpr *call,
                                        Identifier closureLabel) {
  auto *argList = call->getArgs()->getOriginalArgs();
  assert(argList->size() >= 1 && "must have at least one argument");

  SmallString<32> replacement;
  SourceLoc lastLoc;
  SourceRange closureRange;

  if (argList->isUnary()) {
    closureRange = argList->getExpr(0)->getSourceRange();
    lastLoc = argList->getLParenLoc(); // e.g funcName() { 1 }
    if (!lastLoc.isValid()) {
      // Bare trailing closure: e.g. funcName { 1 }
      replacement = "(";
      lastLoc = call->getFn()->getEndLoc();
    }
  } else {
    // Tuple + trailing closure: e.g. funcName(x: 1) { 1 }
    auto numElements = argList->size();
    closureRange = argList->getExpr(numElements - 1)->getSourceRange();
    lastLoc = argList->getExpr(numElements - 2)->getEndLoc();
    replacement = ", ";
  }

  // Add argument label of the closure.
  if (!closureLabel.empty()) {
    replacement += closureLabel.str();
    replacement += ": ";
  }

  lastLoc = Lexer::getLocForEndOfToken(ctx.SourceMgr, lastLoc);
  diag
    .fixItReplaceChars(lastLoc, closureRange.Start, replacement)
    .fixItInsertAfter(closureRange.End, ")");
}

// Perform checkStmtConditionTrailingClosure for single expression.
static void checkStmtConditionTrailingClosure(ASTContext &ctx, const Expr *E) {
  if (E == nullptr || isa<ErrorExpr>(E)) return;

  // Walk into expressions which might have invalid trailing closures
  class DiagnoseWalker : public ASTWalker {
    ASTContext &Ctx;

    void diagnoseIt(const CallExpr *E) {
      // FIXME(https://github.com/apple/swift/issues/57382): We ought to handle multiple trailing closures here.
      auto *args = E->getArgs()->getOriginalArgs();
      if (args->getNumTrailingClosures() != 1)
        return;

      auto closureArg = *args->getFirstTrailingClosure();
      auto *closureExpr = closureArg.getExpr();
      auto closureTy = closureExpr->getType();

      // Ignore invalid argument type. Some diagnostics are already emitted.
      if (!closureTy || closureTy->hasError())
        return;

      // Figure out the label of the parameter the closure is being passed to.
      // This will be present in the type-checked argument list (but not the
      // original), so search it for the relevant argument, looking into
      // variadic expansions if necessary.
      Identifier label;
      for (auto arg : *E->getArgs()) {
        if (arg.getExpr() == closureExpr) {
          label = arg.getLabel();
          break;
        }
        if (auto *varg = dyn_cast<VarargExpansionExpr>(arg.getExpr())) {
          if (auto *array = dyn_cast<ArrayExpr>(varg->getSubExpr())) {
            if (!array->getElements().empty() &&
                array->getElements()[0] == closureExpr) {
              label = arg.getLabel();
              break;
            }
          }
        }
      }

      auto diag = Ctx.Diags.diagnose(closureExpr->getStartLoc(),
                                     diag::trailing_closure_requires_parens);
      fixItEncloseTrailingClosure(Ctx, diag, E, label);
    }

  public:
    DiagnoseWalker(ASTContext &ctx) : Ctx(ctx) { }

    bool shouldWalkIntoSeparatelyCheckedClosure(ClosureExpr *expr) override {
      return false;
    }

    bool shouldWalkCaptureInitializerExpressions() override { return true; }

    MacroWalking getMacroWalkingBehavior() const override {
      return MacroWalking::Expansion;
    }

    PreWalkResult<ArgumentList *>
    walkToArgumentListPre(ArgumentList *args) override {
      // Don't walk into an explicit argument list, as trailing closures that
      // appear in child arguments are fine.
      return Action::VisitChildrenIf(args->isImplicit(), args);
    }

    PreWalkResult<Expr *> walkToExprPre(Expr *E) override {
      switch (E->getKind()) {
      case ExprKind::Paren:
      case ExprKind::Tuple:
      case ExprKind::Array:
      case ExprKind::Dictionary:
      case ExprKind::InterpolatedStringLiteral:
      case ExprKind::Closure:
        // If a trailing closure appears as a child of one of these types of
        // expression, don't diagnose it as there is no ambiguity.
        return Action::VisitChildrenIf(E->isImplicit(), E);
      case ExprKind::Call:
        diagnoseIt(cast<CallExpr>(E));
        break;
      default:
        break;
      }
      return Action::Continue(E);
    }
  };

  DiagnoseWalker Walker(ctx);
  const_cast<Expr *>(E)->walk(Walker);
}

/// Diagnose trailing closure in statement-conditions.
///
/// Conditional statements, including 'for' or `switch` doesn't allow ambiguous
/// trailing closures in these conditions part. Even if the parser can recover
/// them, we force them to disambiguate.
//
/// E.g.:
///   if let _ = arr?.map {$0+1} { ... }
///   for _ in numbers.filter {$0 > 4} { ... }
static void checkStmtConditionTrailingClosure(ASTContext &ctx, const Stmt *S) {
  if (auto LCS = dyn_cast<LabeledConditionalStmt>(S)) {
    for (auto elt : LCS->getCond()) {

      if (elt.getKind() == StmtConditionElement::CK_PatternBinding) {
        checkStmtConditionTrailingClosure(ctx, elt.getInitializer());
        if (auto *exprPattern = dyn_cast<ExprPattern>(elt.getPattern())) {
          checkStmtConditionTrailingClosure(ctx, exprPattern->getMatchExpr());
        }
      } else if (elt.getKind() == StmtConditionElement::CK_Boolean)
        checkStmtConditionTrailingClosure(ctx, elt.getBoolean());
      // No trailing closure for CK_Availability: e.g. `if #available() {}`.
    }
  } else if (auto SS = dyn_cast<SwitchStmt>(S)) {
    checkStmtConditionTrailingClosure(ctx, SS->getSubjectExpr());
  } else if (auto FES = dyn_cast<ForEachStmt>(S)) {
    checkStmtConditionTrailingClosure(ctx, FES->getParsedSequence());
    checkStmtConditionTrailingClosure(ctx, FES->getWhere());
  } else if (auto DCS = dyn_cast<DoCatchStmt>(S)) {
    for (auto CS : DCS->getCatches())
      for (auto &LabelItem : CS->getCaseLabelItems())
        checkStmtConditionTrailingClosure(ctx, LabelItem.getGuardExpr());
  }
}


namespace {

class ObjCSelectorWalker : public ASTWalker {
  ASTContext &Ctx;
  const DeclContext *DC;
  Type SelectorTy;

  /// Determine whether a reference to the given method via its
  /// enclosing class/protocol is ambiguous (and, therefore, needs to
  /// be disambiguated with a coercion).
  bool isSelectorReferenceAmbiguous(AbstractFunctionDecl *method) {
    // Determine the name we would search for. If there are no
    // argument names, our lookup will be based solely on the base
    // name.
    DeclName lookupName = method->getName();
    if (lookupName.getArgumentNames().empty())
      lookupName = lookupName.getBaseName();

    // Look for members with the given name.
    auto nominal = method->getDeclContext()->getSelfNominalTypeDecl();
    auto result = TypeChecker::lookupMember(
        const_cast<DeclContext *>(DC), nominal->getDeclaredInterfaceType(),
        DeclNameRef(lookupName), method->getLoc(),
        defaultMemberLookupOptions);

    // If we didn't find multiple methods, there is no ambiguity.
    if (result.size() < 2) return false;

    // If we found more than two methods, it's ambiguous.
    if (result.size() > 2) return true;

    // Dig out the methods.
    auto firstMethod = dyn_cast<FuncDecl>(result[0].getValueDecl());
    auto secondMethod = dyn_cast<FuncDecl>(result[1].getValueDecl());
    if (!firstMethod || !secondMethod) return true;

    // If one is a static/class method and the other is not...
    if (firstMethod->isStatic() == secondMethod->isStatic()) return true;

    // ... overload resolution will prefer the static method. Check
    // that it has the correct selector. We don't even care that it's
    // the same method we're asking for, just that it has the right
    // selector.
    FuncDecl *staticMethod =
      firstMethod->isStatic() ? firstMethod : secondMethod;
    return staticMethod->getObjCSelector() != method->getObjCSelector();
  }

public:
  ObjCSelectorWalker(const DeclContext *dc, Type selectorTy)
    : Ctx(dc->getASTContext()), DC(dc), SelectorTy(selectorTy) { }

  bool shouldWalkIntoSeparatelyCheckedClosure(ClosureExpr *expr) override {
    return false;
  }

  bool shouldWalkCaptureInitializerExpressions() override { return true; }

  MacroWalking getMacroWalkingBehavior() const override {
    return MacroWalking::Expansion;
  }

  PreWalkResult<Expr *> walkToExprPre(Expr *expr) override {
    auto *stringLiteral = dyn_cast<StringLiteralExpr>(expr);
    bool fromStringLiteral = false;
    bool hadParens = false;
    if (stringLiteral) {
      // Is this a string literal that has type 'Selector'.
      if (!stringLiteral->getType() ||
          !stringLiteral->getType()->isEqual(SelectorTy))
        return Action::Continue(expr);

      fromStringLiteral = true;

      // FIXME: hadParens
    } else {
      // Is this an initialization of 'Selector'?
      auto call = dyn_cast<CallExpr>(expr);
      if (!call) return Action::Continue(expr);

      // That produce Selectors.
      if (!call->getType() || !call->getType()->isEqual(SelectorTy))
        return Action::Continue(expr);

      // Via a constructor.
      ConstructorDecl *ctor = nullptr;
      if (auto ctorRefCall = dyn_cast<ConstructorRefCallExpr>(call->getFn())) {
        if (auto ctorRef = dyn_cast<DeclRefExpr>(ctorRefCall->getFn()))
          ctor = dyn_cast<ConstructorDecl>(ctorRef->getDecl());
        else if (auto otherCtorRef =
                   dyn_cast<OtherConstructorDeclRefExpr>(ctorRefCall->getFn()))
          ctor = otherCtorRef->getDecl();
      }

      if (!ctor) return Action::Continue(expr);

      // Make sure the constructor is within Selector.
      auto ctorContextType = ctor->getDeclContext()
          ->getSelfNominalTypeDecl()
          ->getDeclaredType();
      if (!ctorContextType || !ctorContextType->isEqual(SelectorTy))
        return Action::Continue(expr);

      auto argNames = ctor->getName().getArgumentNames();
      if (argNames.size() != 1) return Action::Continue(expr);

      // Is this the init(stringLiteral:) initializer or init(_:) initializer?
      if (argNames[0] == Ctx.Id_stringLiteral)
        fromStringLiteral = true;
      else if (!argNames[0].empty())
        return Action::Continue(expr);

      auto *arg = call->getArgs()->getUnaryExpr();
      if (!arg)
        return Action::Continue(expr);

      // Track whether we had parentheses around the string literal.
      if (auto paren = dyn_cast<ParenExpr>(arg)) {
        hadParens = true;
        arg = paren->getSubExpr();
      }

      // Check whether we have a string literal.
      stringLiteral = dyn_cast<StringLiteralExpr>(arg);
      if (!stringLiteral) return Action::Continue(expr);
    }

    /// Retrieve the parent expression that coerces to Selector, if
    /// there is one.
    auto getParentCoercion = [&]() -> CoerceExpr * {
      auto parentExpr = Parent.getAsExpr();
      if (!parentExpr) return nullptr;

      auto coerce = dyn_cast<CoerceExpr>(parentExpr);
      if (!coerce) return nullptr;

      if (coerce->getType() && coerce->getType()->isEqual(SelectorTy))
        return coerce;

      return nullptr;
    };

    // Local function that adds the constructor syntax around string
    // literals implicitly treated as a Selector.
    auto addSelectorConstruction = [&](InFlightDiagnostic &diag) {
      if (!fromStringLiteral) return;

      // Introduce the beginning part of the Selector construction.
      diag.fixItInsert(stringLiteral->getLoc(), "Selector(");

      if (auto coerce = getParentCoercion()) {
        // If the string literal was coerced to Selector, replace the
        // coercion with the ")".
        SourceLoc endLoc = Lexer::getLocForEndOfToken(Ctx.SourceMgr,
                                                      expr->getEndLoc());
        diag.fixItReplace(SourceRange(endLoc, coerce->getEndLoc()), ")");
      } else {
        // Otherwise, just insert the closing ")".
        diag.fixItInsertAfter(stringLiteral->getEndLoc(), ")");
      }
    };

    // Try to parse the string literal as an Objective-C selector, and complain
    // if it isn't one.
    auto selector = ObjCSelector::parse(Ctx, stringLiteral->getValue());
    if (!selector) {
      auto diag = Ctx.Diags.diagnose(stringLiteral->getLoc(),
                                     diag::selector_literal_invalid);
      diag.highlight(stringLiteral->getSourceRange());
      addSelectorConstruction(diag);
      return Action::Continue(expr);
    }

    // Look for methods with this selector.
    SmallVector<AbstractFunctionDecl *, 8> allMethods;
    DC->lookupAllObjCMethods(*selector, allMethods);

    // If we didn't find any methods, complain.
    if (allMethods.empty()) {
      // If this was Selector(("selector-name")), suppress, the
      // diagnostic.
      if (!fromStringLiteral && hadParens)
        return Action::Continue(expr);

      {
        auto diag = Ctx.Diags.diagnose(stringLiteral->getLoc(),
                                       diag::selector_literal_undeclared,
                                       *selector);
        addSelectorConstruction(diag);
      }

      // If the result was from a Selector("selector-name"), add a
      // separate note that suggests wrapping the selector in
      // parentheses to silence the warning.
      if (!fromStringLiteral) {
        Ctx.Diags.diagnose(stringLiteral->getLoc(),
                           diag::selector_construction_suppress_warning)
          .fixItInsert(stringLiteral->getStartLoc(), "(")
          .fixItInsertAfter(stringLiteral->getEndLoc(), ")");
      }

      return Action::Continue(expr);
    }

    // Find the "best" method that has this selector, so we can report
    // that.
    AbstractFunctionDecl *bestMethod = nullptr;
    for (auto method : allMethods) {
      // If this is the first method, use it.
      if (!bestMethod) {
        bestMethod = method;
        continue;
      }

      // If referencing the best method would produce an ambiguity and
      // referencing the new method would not, we have a new "best".
      if (isSelectorReferenceAmbiguous(bestMethod) &&
          !isSelectorReferenceAmbiguous(method)) {
        bestMethod = method;
        continue;
      }

      // If this method is within a protocol...
      if (auto proto = method->getDeclContext()->getSelfProtocolDecl()) {
        // If the best so far is not from a protocol, or is from a
        // protocol that inherits this protocol, we have a new best.
        auto bestProto = bestMethod->getDeclContext()->getSelfProtocolDecl();
        if (!bestProto || bestProto->inheritsFrom(proto))
          bestMethod = method;
        continue;
      }

      // This method is from a class.
      auto classDecl = method->getDeclContext()->getSelfClassDecl();

      // If the best method was from a protocol, keep it.
      auto bestClassDecl = bestMethod->getDeclContext()->getSelfClassDecl();
      if (!bestClassDecl) continue;

      // If the best method was from a subclass of the place where
      // this method was declared, we have a new best.
      if (classDecl->isSuperclassOf(bestClassDecl)) {
        bestMethod = method;
      }
    }

    // If we have a best method, reference it.
    if (bestMethod) {
      // Form the replacement #selector expression.
      SmallString<32> replacement;
      {
        llvm::raw_svector_ostream out(replacement);
        auto nominal = bestMethod->getDeclContext()->getSelfNominalTypeDecl();
        out << "#selector(";

        DeclName name;
        auto bestAccessor = dyn_cast<AccessorDecl>(bestMethod);
        if (bestAccessor) {
          switch (bestAccessor->getAccessorKind()) {
          case AccessorKind::Get:
            out << "getter: ";
            name = bestAccessor->getStorage()->getName();
            break;

          case AccessorKind::Set:
          case AccessorKind::WillSet:
          case AccessorKind::DidSet:
            out << "setter: ";
            name = bestAccessor->getStorage()->getName();
            break;

          case AccessorKind::Address:
          case AccessorKind::MutableAddress:
          case AccessorKind::Read:
          case AccessorKind::Modify:
          case AccessorKind::Init:
            llvm_unreachable("cannot be @objc");
          }
        } else {
          name = bestMethod->getName();
        }

        auto typeName = nominal->getName().str();
        // If we're inside a type Foo (or an extension of it) and the suggestion
        // is going to be #selector(Foo.bar) (or #selector(SuperclassOfFoo.bar),
        // then suggest the more natural #selector(self.bar) instead.
        if (auto containingTypeContext = DC->getInnermostTypeContext()) {
          auto methodNominalType = nominal->getDeclaredType();
          auto outerNomType = containingTypeContext->getSelfNominalTypeDecl()
                                                   ->getDeclaredType();
          if (methodNominalType->isEqual(outerNomType) ||
              methodNominalType->isExactSuperclassOf(outerNomType))
            typeName = "self";
        }

        out << typeName << "." << name.getBaseName();
        auto argNames = name.getArgumentNames();

        // Only print the parentheses if there are some argument
        // names, because "()" would indicate a call.
        if (!argNames.empty()) {
          out << "(";
          for (auto argName : argNames) {
            if (argName.empty()) out << "_";
            else out << argName.str();
            out << ":";
          }
          out << ")";
        }

        // If there will be an ambiguity when referring to the method,
        // introduce a coercion to resolve it to the method we found.
        if (!bestAccessor && isSelectorReferenceAmbiguous(bestMethod)) {
          if (auto fnType =
                bestMethod->getInterfaceType()->getAs<FunctionType>()) {
            // For static/class members, drop the metatype argument.
            if (bestMethod->isStatic())
              fnType = fnType->getResult()->getAs<FunctionType>();

            // Coerce to this type.
            assert(fnType->hasTypeRepr() &&
                   "Objective-C methods should always have printable types");
            out << " as ";
            fnType->print(out);
          }
        }

        out << ")";
      }

      // Emit the diagnostic.
      SourceRange replacementRange = expr->getSourceRange();
      if (auto coerce = getParentCoercion())
        replacementRange.End = coerce->getEndLoc();

      Ctx.Diags
          .diagnose(expr->getLoc(),
                    fromStringLiteral
                        ? diag::selector_literal_deprecated_suggest
                        : diag::selector_construction_suggest)
          .fixItReplace(replacementRange, replacement);
      return Action::Continue(expr);
    }

    // If we couldn't pick a method to use for #selector, just wrap
    // the string literal in Selector(...).
    if (fromStringLiteral) {
      auto diag = Ctx.Diags.diagnose(stringLiteral->getLoc(),
                                     diag::selector_literal_deprecated);
      addSelectorConstruction(diag);
      return Action::Continue(expr);
    }

    return Action::Continue(expr);
  }

};
} // end anonymous namespace

static void diagDeprecatedObjCSelectors(const DeclContext *dc,
                                        const Expr *expr) {
  auto selectorTy = dc->getASTContext().getSelectorType();
  if (!selectorTy) return;

  const_cast<Expr *>(expr)->walk(ObjCSelectorWalker(dc, selectorTy));
}

/// Skip over syntactic patterns that aren't typed patterns.
static Pattern *skipNonTypeSyntacticPatterns(Pattern *pattern) {
  if (auto *pp = dyn_cast<ParenPattern>(pattern))
    return skipNonTypeSyntacticPatterns(pp->getSubPattern());
  if (auto *vp = dyn_cast<BindingPattern>(pattern))
    return skipNonTypeSyntacticPatterns(vp->getSubPattern());
  return pattern;
}

/// Diagnose things like this, where 'i' is an Int, not an Int?
///     if let x: Int = i {
static void
checkImplicitPromotionsInCondition(const StmtConditionElement &cond,
                                   ASTContext &ctx) {
  auto *p = cond.getPatternOrNull();
  if (!p) return;
  
  if (auto *subExpr = isImplicitPromotionToOptional(cond.getInitializer())) {
    // If the subexpression was actually optional, then the pattern must be
    // checking for a type, which forced it to be promoted to a double optional
    // type.
    if (auto ooType = subExpr->getType()->getOptionalObjectType()) {
      if (auto OSP = dyn_cast<OptionalSomePattern>(p)) {
        // Check for 'if let' to produce a tuned diagnostic.
        if (auto *TP = dyn_cast<TypedPattern>(OSP->getSubPattern())) {
          ctx.Diags.diagnose(cond.getIntroducerLoc(),
                             diag::optional_check_promotion,
                             subExpr->getType())
            .highlight(subExpr->getSourceRange())
            .fixItReplace(TP->getTypeRepr()->getSourceRange(),
                          ooType->getString());
          return;
        }
      }
      ctx.Diags.diagnose(cond.getIntroducerLoc(),
                         diag::optional_pattern_match_promotion,
                         subExpr->getType(), cond.getInitializer()->getType())
        .highlight(subExpr->getSourceRange());
      return;
    }

    // Check for 'if let' to produce a tuned diagnostic.
    if (isa<OptionalSomePattern>(skipNonTypeSyntacticPatterns(p))) {
      ctx.Diags.diagnose(
          cond.getIntroducerLoc(),
          p->isImplicit()
            ? diag::condition_optional_element_pattern_not_valid_type
            : diag::optional_element_pattern_not_valid_type,
         subExpr->getType())
            .highlight(subExpr->getSourceRange());
      return;
    }

    ctx.Diags.diagnose(cond.getIntroducerLoc(),
                       diag::optional_check_nonoptional,
                       subExpr->getType())
      .highlight(subExpr->getSourceRange());
  }
}

/// Diagnoses a `if #available(...)` condition. Returns true if a diagnostic
/// was emitted.
static bool diagnoseAvailabilityCondition(PoundAvailableInfo *info,
                                          DeclContext *DC) {
  // Reject inlinable code using availability macros. In order to lift this
  // restriction, macros would need to either be expanded when printed in
  // swiftinterfaces or be parsable as macros by module clients.
  auto fragileKind = DC->getFragileFunctionKind();
  if (fragileKind.kind != FragileFunctionKind::None) {
    for (auto queries : info->getQueries()) {
      if (auto availSpec =
              dyn_cast<PlatformVersionConstraintAvailabilitySpec>(queries)) {
        if (availSpec->getMacroLoc().isValid()) {
          DC->getASTContext().Diags.diagnose(
              availSpec->getMacroLoc(),
              swift::diag::availability_macro_in_inlinable,
              fragileKind.getSelector());
          return true;
        }
      }
    }
  }

  return false;
}

/// Diagnoses whether the given clang::Decl can be referenced by a
/// `if #_hasSymbol(...)` condition. Returns true if a diagnostic was emitted.
static bool diagnoseHasSymbolConditionClangDecl(SourceLoc loc,
                                                const clang::Decl *clangDecl,
                                                ASTContext &ctx) {
  if (isa<clang::ObjCInterfaceDecl>(clangDecl) ||
      isa<clang::FunctionDecl>(clangDecl))
    return false;

  if (auto *method = dyn_cast<clang::ObjCMethodDecl>(clangDecl)) {
    // FIXME: Allow objc_direct methods when supported by IRGen.
    ctx.Diags.diagnose(loc,
                       diag::has_symbol_invalid_decl_use_responds_to_selector,
                       /*isProperty*/ false, method->getNameAsString());
    return true;
  }

  if (auto *property = dyn_cast<clang::ObjCPropertyDecl>(clangDecl)) {
    // FIXME: Allow objc_direct properties when supported by IRGen.
    ctx.Diags.diagnose(loc,
                       diag::has_symbol_invalid_decl_use_responds_to_selector,
                       /*isProperty*/ true, property->getNameAsString());
    return true;
  }

  ctx.Diags.diagnose(loc, diag::has_symbol_invalid_decl);
  return true;
}

/// Diagnoses a `if #_hasSymbol(...)` condition. Returns true if a diagnostic
/// was emitted.
static bool diagnoseHasSymbolCondition(PoundHasSymbolInfo *info,
                                       DeclContext *DC) {
  // If we have an invalid info, null expression, or expression without a type
  // then type checking failed already for this condition.
  if (info->isInvalid())
    return false;

  auto symbolExpr = info->getSymbolExpr();
  if (!symbolExpr)
    return false;

  if (!symbolExpr->getType())
    return false;

  auto &ctx = DC->getASTContext();
  auto decl = info->getReferencedDecl().getDecl();
  if (!decl) {
    // Diagnose because we weren't able to interpret the expression as one
    // that uniquely identifies a single declaration.
    ctx.Diags.diagnose(symbolExpr->getLoc(), diag::has_symbol_invalid_expr);
    return true;
  }

  if (auto *clangDecl = decl->getClangDecl()) {
    if (diagnoseHasSymbolConditionClangDecl(symbolExpr->getLoc(), clangDecl,
                                            ctx))
      return true;
  }

  if (DC->getFragileFunctionKind().kind == FragileFunctionKind::None &&
      !decl->isWeakImported(DC->getParentModule())) {
    // `if #_hasSymbol(someStronglyLinkedSymbol)` is functionally a no-op
    // and may indicate the developer has mis-identified the declaration
    // they want to check (or forgot to import the module weakly).
    ctx.Diags.diagnose(symbolExpr->getLoc(), diag::has_symbol_decl_must_be_weak,
                       decl);
    return true;
  }

  return false;
}

/// Perform MiscDiagnostics for the conditions belonging to a \c
/// LabeledConditionalStmt.
static void checkLabeledStmtConditions(ASTContext &ctx,
                                       const LabeledConditionalStmt *stmt,
                                       DeclContext *DC) {
  for (auto elt : stmt->getCond()) {
    // Check for implicit optional promotions in stmt-condition patterns.
    checkImplicitPromotionsInCondition(elt, ctx);

    switch (elt.getKind()) {
    case StmtConditionElement::CK_Boolean:
      break;
    case StmtConditionElement::CK_PatternBinding:
      diagnoseMoveOnlyPatternMatchSubject(ctx, elt.getInitializer());
      break;
    case StmtConditionElement::CK_Availability: {
      auto info = elt.getAvailability();
      (void)diagnoseAvailabilityCondition(info, DC);
      break;
    }
    case StmtConditionElement::CK_HasSymbol: {
      auto info = elt.getHasSymbolInfo();
      if (diagnoseHasSymbolCondition(info, DC))
        info->setInvalid();

      break;
    }
    }
  }
}

static void diagnoseUnintendedOptionalBehavior(const Expr *E,
                                               const DeclContext *DC) {
  if (!E || isa<ErrorExpr>(E) || !E->getType())
    return;

  class UnintendedOptionalBehaviorWalker : public ASTWalker {
    ASTContext &Ctx;
    SmallPtrSet<Expr *, 16> IgnoredExprs;

    class OptionalToAnyCoercion {
    public:
      Type DestType;
      CoerceExpr *ParentCoercion;

      bool shouldSuppressDiagnostic() {
        // If we have a parent CoerceExpr that has the same type as our
        // Optional-to-Any coercion, don't emit a diagnostic.
        return ParentCoercion && ParentCoercion->getType()->isEqual(DestType);
      }
    };

    /// Returns true iff a coercion from srcType to destType is an
    /// Optional-to-Any coercion.
    bool isOptionalToAnyCoercion(Type srcType, Type destType) {
      size_t difference = 0;
      return isOptionalToAnyCoercion(srcType, destType, difference);
    }

    /// Returns true iff a coercion from srcType to destType is an
    /// Optional-to-Any coercion. On returning true, the value of 'difference'
    /// will be the difference in the levels of optionality.
    bool isOptionalToAnyCoercion(Type srcType, Type destType,
                                 size_t &difference) {
      SmallVector<Type, 4> destOptionals;
      auto destValueType =
        destType->lookThroughAllOptionalTypes(destOptionals);

      if (!destValueType->isAny())
        return false;

      SmallVector<Type, 4> srcOptionals;
      srcType->lookThroughAllOptionalTypes(srcOptionals);

      if (srcOptionals.size() > destOptionals.size()) {
        difference = srcOptionals.size() - destOptionals.size();
        return true;
      } else {
        return false;
      }
    }

    /// Returns true iff the collection upcast coercion is an Optional-to-Any
    /// coercion.
    bool isOptionalToAnyCoercion(CollectionUpcastConversionExpr::ConversionPair
                                   conversion) {
      if (!conversion.OrigValue || !conversion.Conversion)
        return false;

      auto srcType = conversion.OrigValue->getType();
      auto destType = conversion.Conversion->getType();
      return isOptionalToAnyCoercion(srcType, destType);
    }

    /// Looks through OptionalEvaluationExprs and InjectIntoOptionalExprs to
    /// find a child ErasureExpr, returning nullptr if no such child is found.
    /// Any intermediate OptionalEvaluationExprs will be marked as ignored.
    ErasureExpr *findErasureExprThroughOptionalInjections(Expr *E) {
      while (true) {
        if (auto *next = dyn_cast<OptionalEvaluationExpr>(E)) {
          // We don't want to re-visit any intermediate optional evaluations.
          IgnoredExprs.insert(next);
          E = next->getSubExpr();
        } else if (auto *next = dyn_cast<InjectIntoOptionalExpr>(E)) {
          E = next->getSubExpr();
        } else {
          break;
        }
      }
      return dyn_cast<ErasureExpr>(E);
    }

    void emitSilenceOptionalAnyWarningWithCoercion(Expr *E, Type destType) {
      assert(destType->hasTypeRepr() &&
             "coercion to Any should always be printable");

      SmallString<16> coercionString;
      coercionString += " as ";
      coercionString += destType->getWithoutParens()->getString();

      Ctx.Diags.diagnose(E->getLoc(), diag::silence_optional_to_any,
                         destType, coercionString.substr(1))
        .highlight(E->getSourceRange())
        .fixItInsertAfter(E->getEndLoc(), coercionString);
    }

    static bool hasImplicitlyUnwrappedResult(Expr *E) {
      auto *decl = getDeclForImplicitlyUnwrappedExpr(E);

      return decl && decl->isImplicitlyUnwrappedOptional();
    }

    static ValueDecl *getDeclForImplicitlyUnwrappedExpr(Expr *E) {
      E = E->getValueProvidingExpr();

      // Look through implicit conversions like loads, derived-to-base
      // conversion, etc.
      if (auto *ICE = dyn_cast<ImplicitConversionExpr>(E)) {
        E = ICE->getSubExpr();
      }

      if (auto *subscript = dyn_cast<SubscriptExpr>(E)) {
        if (subscript->hasDecl())
          return subscript->getDecl().getDecl();
        return nullptr;
      }

      if (auto *memberRef = dyn_cast<MemberRefExpr>(E))
        return memberRef->getMember().getDecl();

      if (auto *declRef = dyn_cast<DeclRefExpr>(E))
        return declRef->getDecl();

      if (auto *apply = dyn_cast<ApplyExpr>(E)) {
        auto *decl = apply->getCalledValue();
        if (isa_and_nonnull<AbstractFunctionDecl>(decl))
          return decl;
      }
      return nullptr;
    }

    void visitErasureExpr(ErasureExpr *E, OptionalToAnyCoercion coercion) {
      if (coercion.shouldSuppressDiagnostic())
        return;

      auto subExpr = E->getSubExpr();

      // Look through any BindOptionalExprs, as the coercion may have started
      // from a higher level of optionality.
      while (auto *bindExpr = dyn_cast<BindOptionalExpr>(subExpr))
        subExpr = bindExpr->getSubExpr();

      // Do not warn on coercions from implicitly unwrapped optionals
      // for Swift versions less than 5.
      if (!Ctx.isSwiftVersionAtLeast(5) &&
          hasImplicitlyUnwrappedResult(subExpr))
        return;

      // We're taking the source type from the child of any BindOptionalExprs,
      // and the destination from the parent of any
      // (InjectIntoOptional/OptionalEvaluation)Exprs in order to take into
      // account any bindings that need to be done for nested Optional-to-Any
      // coercions, e.g Int??? to Any?.
      auto srcType = subExpr->getType();
      auto destType = coercion.DestType;

      size_t optionalityDifference = 0;
      if (!isOptionalToAnyCoercion(srcType, destType, optionalityDifference))
        return;
      
      // If we're implicitly unwrapping from IUO to Any then emit a custom
      // diagnostic
      if (hasImplicitlyUnwrappedResult(subExpr)) {
        if (auto decl = getDeclForImplicitlyUnwrappedExpr(subExpr)) {
          Ctx.Diags.diagnose(subExpr->getStartLoc(), diag::iuo_to_any_coercion,
                             /* from */ srcType, /* to */ destType)
              .highlight(subExpr->getSourceRange());

          auto noteDiag = isa<FuncDecl>(decl)
                              ? diag::iuo_to_any_coercion_note_func_result
                              : diag::iuo_to_any_coercion_note;

          Ctx.Diags.diagnose(decl->getLoc(), noteDiag, decl);
        }
      } else {
        Ctx.Diags.diagnose(subExpr->getStartLoc(),
                           diag::optional_to_any_coercion,
                           /* from */ srcType, /* to */ destType)
            .highlight(subExpr->getSourceRange());
      }
      
      if (optionalityDifference == 1) {
        Ctx.Diags.diagnose(subExpr->getLoc(), diag::default_optional_to_any)
            .highlight(subExpr->getSourceRange())
            .fixItInsertAfter(subExpr->getEndLoc(), " ?? <#default value#>");
      }

      SmallString<4> forceUnwrapString;
      for (size_t i = 0; i < optionalityDifference; ++i)
        forceUnwrapString += "!";

      Ctx.Diags.diagnose(subExpr->getLoc(), diag::force_optional_to_any)
        .highlight(subExpr->getSourceRange())
        .fixItInsertAfter(subExpr->getEndLoc(), forceUnwrapString);

      emitSilenceOptionalAnyWarningWithCoercion(subExpr, destType);
    }

    void visitCollectionUpcastExpr(CollectionUpcastConversionExpr *E,
                                   OptionalToAnyCoercion coercion) {
      // We only need to consider the valueConversion, as the Key type of a
      // Dictionary cannot be implicitly coerced to Any.
      auto valueConversion = E->getValueConversion();

      // We're handling the coercion of the entire collection, so we don't need
      // to re-visit a nested ErasureExpr for the value.
      if (auto conversionExpr = valueConversion.Conversion)
        if (auto *erasureExpr =
              findErasureExprThroughOptionalInjections(conversionExpr))
          IgnoredExprs.insert(erasureExpr);

      if (coercion.shouldSuppressDiagnostic() ||
          !isOptionalToAnyCoercion(valueConversion))
        return;

      auto subExpr = E->getSubExpr();

      Ctx.Diags.diagnose(subExpr->getStartLoc(), diag::optional_to_any_coercion,
                         /* from */ subExpr->getType(), /* to */ E->getType())
        .highlight(subExpr->getSourceRange());

      emitSilenceOptionalAnyWarningWithCoercion(subExpr, E->getType());
    }

    void visitPossibleOptionalToAnyExpr(Expr *E,
                                        OptionalToAnyCoercion coercion) {
      if (auto *upcastExpr =
          dyn_cast<CollectionUpcastConversionExpr>(E)) {
        visitCollectionUpcastExpr(upcastExpr, coercion);
      } else if (auto *erasureExpr = dyn_cast<ErasureExpr>(E)) {
        visitErasureExpr(erasureExpr, coercion);
      } else if (auto *optionalEvalExpr = dyn_cast<OptionalEvaluationExpr>(E)) {
        // The ErasureExpr could be nested within optional injections and
        // bindings, such as is the case for e.g Int??? to Any?. Try and find
        // and visit it directly, making sure we don't re-visit it later.
        auto subExpr = optionalEvalExpr->getSubExpr();
        if (auto *erasureExpr =
              findErasureExprThroughOptionalInjections(subExpr)) {
          visitErasureExpr(erasureExpr, coercion);
          IgnoredExprs.insert(erasureExpr);
        }
      }
    }

    enum class UnintendedInterpolationKind: bool {
      Optional,
      Function
    };

    void visitInterpolatedStringLiteralExpr(InterpolatedStringLiteralExpr *E) {
      E->forEachSegment(Ctx,
          [&](bool isInterpolation, CallExpr *segment) -> void {
        if (isInterpolation) {
          diagnoseIfUnintendedInterpolation(segment,
                              UnintendedInterpolationKind::Optional);
          diagnoseIfUnintendedInterpolation(segment,
                              UnintendedInterpolationKind::Function);
        }
      });
    }

    void diagnoseIfUnintendedInterpolation(CallExpr *segment,
                                           UnintendedInterpolationKind kind) {
      if (interpolationWouldBeUnintended(segment->getCalledValue(), kind))
        if (auto firstArg =
              getFirstArgIfUnintendedInterpolation(segment->getArgs(), kind))
          diagnoseUnintendedInterpolation(firstArg, kind);
    }

    bool interpolationWouldBeUnintended(ConcreteDeclRef appendMethod,
                                        UnintendedInterpolationKind kind) {
      ValueDecl * fnDecl = appendMethod.getDecl();

      // If things aren't set up right, just hope for the best.
      if (!fnDecl || fnDecl->isInvalid())
        return false;

      // If the decl expects an optional, that's fine.
      auto uncurriedType = fnDecl->getInterfaceType()->getAs<AnyFunctionType>();
      auto curriedType = uncurriedType->getResult()->getAs<AnyFunctionType>();

      // I don't know why you'd use a zero-arg interpolator, but it obviously 
      // doesn't interpolate an optional.
      if (curriedType->getNumParams() == 0)
        return false;

      // If the first parameter explicitly accepts the type, this method 
      // presumably doesn't want us to warn about optional use.
      auto firstParamType =
        curriedType->getParams().front().getPlainType()->getRValueType();
      if (kind == UnintendedInterpolationKind::Optional) {
        if (firstParamType->getOptionalObjectType())
          return false;
      } else {
        if (firstParamType->is<AnyFunctionType>())
          return false;
      }

      return true;
    }

    Expr *
    getFirstArgIfUnintendedInterpolation(ArgumentList *args,
                                         UnintendedInterpolationKind kind) {
      // Just check the first argument, which is usually the value 
      // being interpolated.
      if (args->empty())
        return nullptr;

      auto *firstArg = args->getExpr(0);

      // Allow explicit casts.
      if (isa<ExplicitCastExpr>(firstArg->getSemanticsProvidingExpr()))
        return nullptr;

      // If we don't have a type, assume the best.
      if (!firstArg->getType() || firstArg->getType()->hasError())
        return nullptr;

      // Bail out if we don't have an optional.
      if (kind == UnintendedInterpolationKind::Optional) {
        if (!firstArg->getType()->getRValueType()->getOptionalObjectType())
          return nullptr;
      }
      else if (kind == UnintendedInterpolationKind::Function) {
        if (!firstArg->getType()->getRValueType()->is<AnyFunctionType>())
          return nullptr;
      }

      return firstArg;
    }

    void diagnoseUnintendedInterpolation(Expr * arg, UnintendedInterpolationKind kind) {
      Ctx.Diags
          .diagnose(arg->getStartLoc(),
                    diag::debug_description_in_string_interpolation_segment,
                    (bool)kind)
          .highlight(arg->getSourceRange());

      // Suggest 'String(describing: <expr>)'.
      auto argStart = arg->getStartLoc();
      Ctx.Diags
          .diagnose(
              arg->getLoc(),
              diag::silence_debug_description_in_interpolation_segment_call)
          .highlight(arg->getSourceRange())
          .fixItInsert(argStart, "String(describing: ")
          .fixItInsertAfter(arg->getEndLoc(), ")");

      if (kind == UnintendedInterpolationKind::Optional) {
        // Suggest inserting a default value.
        Ctx.Diags.diagnose(arg->getLoc(), diag::default_optional_to_any)
          .highlight(arg->getSourceRange())
          .fixItInsertAfter(arg->getEndLoc(), " ?? <#default value#>");
      }
    }

    bool shouldWalkIntoSeparatelyCheckedClosure(ClosureExpr *expr) override {
      return false;
    }

    bool shouldWalkCaptureInitializerExpressions() override { return true; }

    MacroWalking getMacroWalkingBehavior() const override {
      return MacroWalking::Expansion;
    }

    PreWalkResult<Expr *> walkToExprPre(Expr *E) override {
      if (!E || isa<ErrorExpr>(E) || !E->getType())
        return Action::SkipChildren(E);

      if (IgnoredExprs.count(E))
        return Action::Continue(E);

      if (auto *literal = dyn_cast<InterpolatedStringLiteralExpr>(E)) {
        visitInterpolatedStringLiteralExpr(literal);
      } else if (auto *coercion = dyn_cast<CoerceExpr>(E)) {
        // If we come across a CoerceExpr, visit its subExpr with the coercion
        // as the parent, making sure we don't re-visit the subExpr later.
        auto subExpr = coercion->getSubExpr();
        visitPossibleOptionalToAnyExpr(subExpr,
                                       { subExpr->getType(), coercion });
        IgnoredExprs.insert(subExpr);
      } else {
        visitPossibleOptionalToAnyExpr(E, { E->getType(), nullptr });
      }
      return Action::Continue(E);
    }

  public:
    UnintendedOptionalBehaviorWalker(ASTContext &ctx) : Ctx(ctx) { }
  };

  UnintendedOptionalBehaviorWalker Walker(DC->getASTContext());
  const_cast<Expr *>(E)->walk(Walker);
}

static void diagnoseDeprecatedWritableKeyPath(const Expr *E,
                                              const DeclContext *DC) {
  if (!E || isa<ErrorExpr>(E) || !E->getType())
    return;

  class DeprecatedWritableKeyPathWalker : public ASTWalker {
    ASTContext &Ctx;
    const DeclContext *DC;

    void visitKeyPathApplicationExpr(KeyPathApplicationExpr *E) {
      bool isWrite = false;
      if (auto *P = Parent.getAsExpr())
        if (auto *AE = dyn_cast<AssignExpr>(P))
          if (AE->getDest() == E)
            isWrite = true;

      if (!isWrite)
        return;

      if (auto *keyPathExpr = dyn_cast<KeyPathExpr>(E->getKeyPath())) {
        if (!keyPathExpr->getType()->isWritableKeyPath() &&
            !keyPathExpr->getType()->isReferenceWritableKeyPath())
          return;

        assert(keyPathExpr->getComponents().size() > 0);
        auto &component = keyPathExpr->getComponents().back();
        if (component.getKind() == KeyPathExpr::Component::Kind::Property) {
          auto *storage =
            cast<AbstractStorageDecl>(component.getDeclRef().getDecl());
          if (!storage->isSettable(nullptr) ||
              !storage->isSetterAccessibleFrom(DC)) {
            Ctx.Diags.diagnose(keyPathExpr->getLoc(),
                               swift::diag::expr_deprecated_writable_keypath,
                               storage);
          }
        }
      }
    }

    bool shouldWalkIntoSeparatelyCheckedClosure(ClosureExpr *expr) override {
      return false;
    }

    bool shouldWalkCaptureInitializerExpressions() override { return true; }

    MacroWalking getMacroWalkingBehavior() const override {
      return MacroWalking::Expansion;
    }

    PreWalkResult<Expr *> walkToExprPre(Expr *E) override {
      if (!E || isa<ErrorExpr>(E) || !E->getType())
        return Action::SkipChildren(E);

      if (auto *KPAE = dyn_cast<KeyPathApplicationExpr>(E)) {
        visitKeyPathApplicationExpr(KPAE);
        return Action::Continue(E);
      }

      return Action::Continue(E);
    }

  public:
    DeprecatedWritableKeyPathWalker(const DeclContext *DC)
        : Ctx(DC->getASTContext()), DC(DC) {}
  };

  DeprecatedWritableKeyPathWalker Walker(DC);
  const_cast<Expr *>(E)->walk(Walker);
}

static void maybeDiagnoseCallToKeyValueObserveMethod(const Expr *E,
                                                     const DeclContext *DC) {
  class KVOObserveCallWalker : public ASTWalker {
    const ASTContext &C;

  public:
    KVOObserveCallWalker(ASTContext &ctx) : C(ctx) {}

    void maybeDiagnoseCallExpr(CallExpr *expr) {
      auto fn = expr->getCalledValue();
      if (!fn)
        return;
      SmallVector<KeyPathExpr *, 1> keyPathArgs;
      auto *args = expr->getArgs();
      if (fn->getModuleContext()->getName() == C.Id_Foundation &&
          fn->getName().isCompoundName("observe",
                                       {"", "options", "changeHandler"})) {
        if (auto keyPathArg = dyn_cast<KeyPathExpr>(args->getExpr(0))) {
          keyPathArgs.push_back(keyPathArg);
        }
      } else if (fn->getAttrs()
                 .hasSemanticsAttr(semantics::KEYPATH_MUST_BE_VALID_FOR_KVO)) {
        for (const auto& arg: *args) {
          if (auto keyPathArg = dyn_cast<KeyPathExpr>(arg.getExpr())) {
            keyPathArgs.push_back(keyPathArg);
          }
        }
      }
      for (auto *keyPathArg : keyPathArgs) {
        auto lastComponent = keyPathArg->getComponents().back();
        if (lastComponent.getKind() != KeyPathExpr::Component::Kind::Property)
          continue;
        auto property = lastComponent.getDeclRef().getDecl();
        if (!property)
          continue;
        auto propertyVar = cast<VarDecl>(property);
        if (propertyVar->shouldUseObjCDispatch() ||
            (propertyVar->isObjC() &&
             propertyVar->getParsedAccessor(AccessorKind::Set)))
          continue;
        C.Diags
          .diagnose(expr->getLoc(),
                    diag::observe_keypath_property_not_objc_dynamic,
                    property->getName(), fn->getName())
          .highlight(lastComponent.getLoc());
      }
    }

    MacroWalking getMacroWalkingBehavior() const override {
      return MacroWalking::Expansion;
    }

    PreWalkResult<Expr *> walkToExprPre(Expr *E) override {
      if (!E || isa<ErrorExpr>(E) || !E->getType())
        return Action::SkipChildren(E);

      if (auto *CE = dyn_cast<CallExpr>(E)) {
        maybeDiagnoseCallExpr(CE);
        return Action::SkipChildren(E);
      }

      return Action::Continue(E);
    }
  };

  KVOObserveCallWalker Walker(DC->getASTContext());
  const_cast<Expr *>(E)->walk(Walker);
}

static void diagnoseExplicitUseOfLazyVariableStorage(const Expr *E,
                                                     const DeclContext *DC) {

  class ExplicitLazyVarStorageAccessFinder : public ASTWalker {
    const ASTContext &C;

  public:
    ExplicitLazyVarStorageAccessFinder(ASTContext &ctx) : C(ctx) {}

    void tryDiagnoseExplicitLazyStorageVariableUse(MemberRefExpr *MRE) {
      if (MRE->isImplicit()) {
        return;
      }
      auto VD = dyn_cast<VarDecl>(MRE->getMember().getDecl());
      if (!VD) {
        return;
      }
      auto sourceFileKind = VD->getDeclContext()->getParentSourceFile();
      if (!sourceFileKind) {
        return;
      }
      if (sourceFileKind->Kind != SourceFileKind::Library &&
          sourceFileKind->Kind != SourceFileKind::Main) {
        return;
      }
      if (VD->isLazyStorageProperty()) {
        C.Diags.diagnose(MRE->getLoc(), diag::lazy_var_storage_access);
      }
    }

    MacroWalking getMacroWalkingBehavior() const override {
      return MacroWalking::Expansion;
    }

    PreWalkResult<Expr *> walkToExprPre(Expr *E) override {
      if (!E || isa<ErrorExpr>(E) || !E->getType())
        return Action::SkipChildren(E);

      if (auto *MRE = dyn_cast<MemberRefExpr>(E)) {
        tryDiagnoseExplicitLazyStorageVariableUse(MRE);
        return Action::SkipChildren(E);
      }

      return Action::Continue(E);
    }
  };

  ExplicitLazyVarStorageAccessFinder Walker(DC->getASTContext());
  const_cast<Expr *>(E)->walk(Walker);
}

static void diagnoseComparisonWithNaN(const Expr *E, const DeclContext *DC) {
  class ComparisonWithNaNFinder : public ASTWalker {
    const ASTContext &C;
    const DeclContext *DC;

  public:
    ComparisonWithNaNFinder(const DeclContext *dc)
        : C(dc->getASTContext()), DC(dc) {}

    void tryDiagnoseComparisonWithNaN(BinaryExpr *BE) {
      ValueDecl *comparisonDecl = nullptr;

      // Dig out the function declaration.
      if (auto Fn = BE->getFn()) {
        if (auto DSCE = dyn_cast<DotSyntaxCallExpr>(Fn)) {
          comparisonDecl = DSCE->getCalledValue();
        } else {
          comparisonDecl = BE->getCalledValue();
        }
      }

      // Bail out if it isn't a function.
      if (!comparisonDecl || !isa<FuncDecl>(comparisonDecl)) {
        return;
      }

      // We're only interested in comparison functions like == or <=.
      auto comparisonDeclName = comparisonDecl->getBaseIdentifier();
      if (!comparisonDeclName.isStandardComparisonOperator()) {
        return;
      }

      auto *firstArg = BE->getLHS();
      auto *secondArg = BE->getRHS();

      // Make sure that both arguments are valid before doing anything else,
      // this helps us to debug reports of crashes in `conformsToKnownProtocol`
      // referencing arguments (rdar://78920375).
      //
      // Since this diagnostic should only be run on type-checked AST,
      // it's unclear what caused one of the arguments to have null type.
      assert(firstArg->getType() && "Expected valid type for first argument");
      assert(secondArg->getType() && "Expected valid type for second argument");

      // Both arguments must conform to FloatingPoint protocol.
      if (!TypeChecker::conformsToKnownProtocol(firstArg->getType(),
                                                KnownProtocolKind::FloatingPoint,
                                                DC->getParentModule()) ||
          !TypeChecker::conformsToKnownProtocol(secondArg->getType(),
                                                KnownProtocolKind::FloatingPoint,
                                                DC->getParentModule())) {
        return;
      }

      // Convenience utility to extract argument decl.
      auto extractArgumentDecl = [&](Expr *arg) -> ValueDecl * {
        if (auto DRE = dyn_cast<DeclRefExpr>(arg)) {
          return DRE->getDecl();
        } else if (auto MRE = dyn_cast<MemberRefExpr>(arg)) {
          return MRE->getMember().getDecl();
        }
        return nullptr;
      };

      // Dig out the declarations for the arguments.
      auto *firstVal = extractArgumentDecl(firstArg);
      auto *secondVal = extractArgumentDecl(secondArg);

      // If we can't find declarations for both arguments, bail out,
      // because one of them has to be '.nan'.
      if (!firstArg && !secondArg) {
        return;
      }

      // Convenience utility to check if this is a 'nan' variable.
      auto isNanDecl = [&](ValueDecl *VD) {
        return VD && isa<VarDecl>(VD) && VD->getBaseIdentifier().is("nan");
      };

      // Diagnose comparison with '.nan'.
      //
      // If the comparison is done using '<=', '<', '==', '>', '>=', then
      // the result is always false. If the comparison is done using '!=',
      // then the result is always true.
      //
      // Emit a different diagnostic which doesn't mention using '.isNaN' if
      // the comparison isn't done using '==' or '!=' or if both sides are
      // '.nan'.
      if (isNanDecl(firstVal) && isNanDecl(secondVal)) {
        C.Diags.diagnose(BE->getLoc(), diag::nan_comparison_both_nan,
                         comparisonDeclName.str(), comparisonDeclName.is("!="));
      } else if (isNanDecl(firstVal) || isNanDecl(secondVal)) {
        if (comparisonDeclName.is("==") || comparisonDeclName.is("!=")) {
          auto exprStr =
              C.SourceMgr
                  .extractText(Lexer::getCharSourceRangeFromSourceRange(
                      C.SourceMgr, firstArg->getSourceRange()))
                  .str();
          auto prefix = exprStr;
          if (comparisonDeclName.is("!=")) {
            prefix = "!" + prefix;
          }
          C.Diags.diagnose(BE->getLoc(), diag::nan_comparison,
                           comparisonDeclName, comparisonDeclName.is("!="),
                           prefix, exprStr);
        } else {
          C.Diags.diagnose(BE->getLoc(), diag::nan_comparison_without_isnan,
                           comparisonDeclName, comparisonDeclName.is("!="));
        }
      }
    }

    MacroWalking getMacroWalkingBehavior() const override {
      return MacroWalking::Expansion;
    }

    PreWalkResult<Expr *> walkToExprPre(Expr *E) override {
      if (!E || isa<ErrorExpr>(E) || !E->getType())
        return Action::SkipChildren(E);

      if (auto *BE = dyn_cast<BinaryExpr>(E)) {
        tryDiagnoseComparisonWithNaN(BE);
        return Action::SkipChildren(E);
      }

      return Action::Continue(E);
    }
  };

  ComparisonWithNaNFinder Walker(DC);
  const_cast<Expr *>(E)->walk(Walker);
}

static void diagUnqualifiedAccessToMethodNamedSelf(const Expr *E,
                                                   const DeclContext *DC) {
  if (!E || isa<ErrorExpr>(E) || !E->getType())
    return;

  class DiagnoseWalker : public ASTWalker {
    ASTContext &Ctx;
    const DeclContext *DC;

  public:
    DiagnoseWalker(const DeclContext *DC) : Ctx(DC->getASTContext()), DC(DC) {}

    bool shouldWalkIntoSeparatelyCheckedClosure(ClosureExpr *expr) override {
      return false;
    }

    MacroWalking getMacroWalkingBehavior() const override {
      return MacroWalking::Expansion;
    }

    PreWalkResult<Expr *> walkToExprPre(Expr *E) override {
      if (!E || isa<ErrorExpr>(E) || !E->getType())
        return Action::SkipChildren(E);

      auto *DRE = dyn_cast<DeclRefExpr>(E);
      // If this is not an explicit 'self' reference, let's keep searching.
      if (!DRE || DRE->isImplicit())
        return Action::Continue(E);

      // If this not 'self' or it's not a function reference, it's unrelated.
      if (!(DRE->getDecl()->getBaseName() == Ctx.Id_self &&
            DRE->getType()->is<AnyFunctionType>()))
        return Action::Continue(E);

      auto typeContext = DC->getInnermostTypeContext();
      // Use of 'self' in enums is not confusable.
      if (!typeContext || typeContext->getSelfEnumDecl())
        return Action::Continue(E);

      // self(...) is not easily confusable.
      if (auto *parentExpr = Parent.getAsExpr()) {
        if (isa<CallExpr>(parentExpr))
          return Action::Continue(E);

        // Explicit call to a static method 'self' of some type is not
        // confusable.
        if (isa<DotSyntaxCallExpr>(parentExpr) && !parentExpr->isImplicit())
          return Action::Continue(E);
      }

      auto baseType = typeContext->getDeclaredInterfaceType();
      auto baseTypeString = baseType.getString();

      Ctx.Diags.diagnose(E->getLoc(), diag::self_refers_to_method,
                         baseTypeString);

      Ctx.Diags
          .diagnose(E->getLoc(), diag::fix_unqualified_access_member_named_self,
                    baseTypeString)
          .fixItInsert(E->getLoc(), diag::insert_type_qualification, baseType);

      return Action::Continue(E);
    }
  };

  DiagnoseWalker Walker(DC);
  const_cast<Expr *>(E)->walk(Walker);
}

static void
diagnoseDictionaryLiteralDuplicateKeyEntries(const Expr *E,
                                             const DeclContext *DC) {
  class DiagnoseWalker : public ASTWalker {
    ASTContext &Ctx;

  private:
    std::string getKeyStringValue(const LiteralExpr *keyExpr) {
      if (auto *MLE = dyn_cast<MagicIdentifierLiteralExpr>(keyExpr)) {
        return getMagicLiteralKeyValue(MLE);
      }
      std::string out;
      llvm::raw_string_ostream OS(out);
      keyExpr->printConstExprValue(&OS, /*additionalCheck=*/nullptr);
      return out;
    }

    std::string getMagicLiteralKeyValue(const MagicIdentifierLiteralExpr *MLE) {
      auto magicLiteralValue = MLE->getLiteralKindDescription().str();
      switch (MLE->getKind()) {
      case MagicIdentifierLiteralExpr::DSOHandle:
      case MagicIdentifierLiteralExpr::FileID:
      case MagicIdentifierLiteralExpr::FileIDSpelledAsFile:
      case MagicIdentifierLiteralExpr::FilePath:
      case MagicIdentifierLiteralExpr::FilePathSpelledAsFile:
      case MagicIdentifierLiteralExpr::Function:
        break;
      // Those are literals that can evaluate to different values in a
      // dictionary literal declaration context based on source position
      // so we need to consider that position as part of the literal value.
      case MagicIdentifierLiteralExpr::Column: {
        unsigned int column;
        std::tie(std::ignore, column) =
            Ctx.SourceMgr.getPresumedLineAndColumnForLoc(MLE->getStartLoc());
        magicLiteralValue += ":" + std::to_string(column);
        break;
      }
      case MagicIdentifierLiteralExpr::Line: {
        unsigned int line;
        std::tie(line, std::ignore) =
            Ctx.SourceMgr.getPresumedLineAndColumnForLoc(MLE->getStartLoc());
        magicLiteralValue += ":" + std::to_string(line);
        break;
      }
      }
      return magicLiteralValue;
    }

    std::string getKeyStringValueForDiagnostic(const LiteralExpr *keyExpr) {
      std::string out;
      switch (keyExpr->getKind()) {
      case ExprKind::NilLiteral:
      case ExprKind::MagicIdentifierLiteral:
        return out;
      case ExprKind::StringLiteral: {
        const auto *SL = cast<StringLiteralExpr>(keyExpr);
        out = SL->getValue().str();
        break;
      }
      default:
        llvm::raw_string_ostream OS(out);
        keyExpr->printConstExprValue(&OS, /*additionalCheck=*/nullptr);
        break;
      }
      return "'" + out + "'";
    }
    
    bool shouldDiagnoseLiteral(const LiteralExpr *LE) {
      switch (LE->getKind()) {
      case ExprKind::IntegerLiteral:
      case ExprKind::FloatLiteral:
      case ExprKind::BooleanLiteral:
      case ExprKind::StringLiteral:
      case ExprKind::MagicIdentifierLiteral:
      case ExprKind::NilLiteral:
        return true;
      // Skip interpolated literals because they
      // can contain expressions that although equal
      // maybe be evaluated to different values. e.g.
      // "\(a) \(a)" where 'a' is a computed variable.
      case ExprKind::InterpolatedStringLiteral:
      // Also skip object literals as most of them takes paramenters that can
      // contain expressions that altough equal may evaluate to different
      // values e.g. #fileLiteral(resourceName: a) where 'a' is a computed
      // property is valid.
      case ExprKind::ObjectLiteral:
      // Literal expressions produce Regex<Out> type result,
      // which cannot be keys due to not conforming to hashable.
      case ExprKind::RegexLiteral:
        return false;
      // If a new literal is added in the future, the compiler
      // will warn that a case is missing from this switch.
      #define LITERAL_EXPR(Id, Parent)
      #define EXPR(Id, Parent) case ExprKind::Id:
      #include "swift/AST/ExprNodes.def"
        llvm_unreachable("Not a literal expression");
      }
      llvm_unreachable("Unhandled literal");
    }
  public:
    DiagnoseWalker(const DeclContext *DC) : Ctx(DC->getASTContext()) {}

    bool shouldWalkIntoSeparatelyCheckedClosure(ClosureExpr *expr) override {
      return false;
    }

    MacroWalking getMacroWalkingBehavior() const override {
      return MacroWalking::Expansion;
    }

    PreWalkResult<Expr *> walkToExprPre(Expr *E) override {
      const auto *DLE = dyn_cast_or_null<DictionaryExpr>(E);
      if (!DLE)
        return Action::Continue(E);

      auto type = DLE->getType();
      // For other types conforming with `ExpressibleByDictionaryLiteral`
      // protocol, duplicated keys may be allowed.
      if (!(type && type->isDictionary())) {
        return Action::Continue(E);
      }

      using LiteralKey = std::pair<std::string, ExprKind>;
      using Element = std::pair<const TupleExpr *, size_t>;

      std::map<LiteralKey, llvm::SmallVector<Element, 4>> groupedLiteralKeys;

      for (size_t i = 0; i < DLE->getElements().size(); ++i) {
        const auto *elt = DLE->getElement(i);
        const auto *tupleElt = cast<TupleExpr>(elt);
        const auto *keyExpr =
            tupleElt->getElement(0)->getSemanticsProvidingExpr();
        auto *LE = dyn_cast<LiteralExpr>(keyExpr);
        if (!LE)
          continue;
        
        if (!shouldDiagnoseLiteral(LE))
          continue;

        auto keyStringValue = getKeyStringValue(LE);
        auto literalKey = std::make_pair(keyStringValue, keyExpr->getKind());
        groupedLiteralKeys[literalKey].push_back({tupleElt, i});
      }

      // All keys are unique.
      if (groupedLiteralKeys.size() == DLE->getNumElements()) {
        return Action::Continue(E);
      }

      auto &DE = Ctx.Diags;
      auto emitNoteWithFixit = [&](const Element &duplicated) {
        auto note = DE.diagnose(duplicated.first->getLoc(),
                                diag::duplicated_key_declared_here);
        auto duplicatedEltIdx = duplicated.second;
        const auto commanLocs = DLE->getCommaLocs();
        note.fixItRemove(duplicated.first->getSourceRange());
        if (duplicatedEltIdx < commanLocs.size()) {
          note.fixItRemove(commanLocs[duplicatedEltIdx]);
        } else {
          // For the last element remove the previous comma.
          note.fixItRemove(commanLocs[duplicatedEltIdx - 1]);
        }
      };

      for (auto &entry : groupedLiteralKeys) {
        auto &keyValuePairs = entry.second;
        if (keyValuePairs.size() == 1) {
          continue;
        }

        auto elt = keyValuePairs.front();
        const auto keyValue = entry.first.first;
        const auto keyExpr = cast<LiteralExpr>(
            elt.first->getElement(0)->getSemanticsProvidingExpr());
        const auto value = getKeyStringValueForDiagnostic(keyExpr);
        DE.diagnose(elt.first->getLoc(),
                    diag::duplicated_literal_keys_in_dictionary_literal, type,
                    keyExpr->getLiteralKindDescription(), value.empty(), value);
        for (auto &duplicated : keyValuePairs) {
          emitNoteWithFixit(duplicated);
        }
      }
      return Action::Continue(E);
    }
  };

  DiagnoseWalker Walker(DC);
  const_cast<Expr *>(E)->walk(Walker);
}

//===----------------------------------------------------------------------===//
// High-level entry points.
//===----------------------------------------------------------------------===//

/// Emit diagnostics for syntactic restrictions on a given expression.
void swift::performSyntacticExprDiagnostics(const Expr *E,
                                            const DeclContext *DC,
                                            bool isExprStmt,
                                            bool disableExprAvailabilityChecking) {
  auto &ctx = DC->getASTContext();
  TypeChecker::diagnoseSelfAssignment(E);
  diagSyntacticUseRestrictions(E, DC, isExprStmt);
  diagRecursivePropertyAccess(E, DC);
  diagnoseImplicitSelfUseInClosure(E, DC);
  diagnoseUnintendedOptionalBehavior(E, DC);
  maybeDiagnoseCallToKeyValueObserveMethod(E, DC);
  diagnoseExplicitUseOfLazyVariableStorage(E, DC);
  diagnoseComparisonWithNaN(E, DC);
  if (!ctx.isSwiftVersionAtLeast(5))
    diagnoseDeprecatedWritableKeyPath(E, DC);
  if (!ctx.LangOpts.DisableAvailabilityChecking && !disableExprAvailabilityChecking)
    diagnoseExprAvailability(E, const_cast<DeclContext*>(DC));
  if (ctx.LangOpts.EnableObjCInterop)
    diagDeprecatedObjCSelectors(DC, E);
  diagnoseConstantArgumentRequirement(E, DC);
  diagUnqualifiedAccessToMethodNamedSelf(E, DC);
  diagnoseDictionaryLiteralDuplicateKeyEntries(E, DC);
}

void swift::performStmtDiagnostics(const Stmt *S, DeclContext *DC) {
  auto &ctx = DC->getASTContext();

  TypeChecker::checkExistentialTypes(ctx, const_cast<Stmt *>(S), DC);

  if (auto switchStmt = dyn_cast<SwitchStmt>(S))
    checkSwitch(ctx, switchStmt, DC);

  checkStmtConditionTrailingClosure(ctx, S);

  if (auto *lcs = dyn_cast<LabeledConditionalStmt>(S))
    checkLabeledStmtConditions(ctx, lcs, DC);

  if (!ctx.LangOpts.DisableAvailabilityChecking)
    diagnoseStmtAvailability(S, const_cast<DeclContext*>(DC));
}

//===----------------------------------------------------------------------===//
// Utility functions
//===----------------------------------------------------------------------===//

void swift::fixItAccess(InFlightDiagnostic &diag, ValueDecl *VD,
                        AccessLevel desiredAccess, bool isForSetter,
                        bool shouldUseDefaultAccess) {
  StringRef fixItString;
  switch (desiredAccess) {
  case AccessLevel::Private:      fixItString = "private ";      break;
  case AccessLevel::FilePrivate:  fixItString = "fileprivate ";  break;
  case AccessLevel::Internal:     fixItString = "internal ";     break;
  case AccessLevel::Package:      fixItString = "package ";      break;
  case AccessLevel::Public:       fixItString = "public ";       break;
  case AccessLevel::Open:         fixItString = "open ";         break;
  }

  DeclAttributes &attrs = VD->getAttrs();
  AbstractAccessControlAttr *attr;
  if (isForSetter) {
    attr = attrs.getAttribute<SetterAccessAttr>();
    cast<AbstractStorageDecl>(VD)->overwriteSetterAccess(desiredAccess);
  } else {
    attr = attrs.getAttribute<AccessControlAttr>();
    VD->overwriteAccess(desiredAccess);

    if (auto *ASD = dyn_cast<AbstractStorageDecl>(VD)) {
      if (auto *getter = ASD->getAccessor(AccessorKind::Get))
        getter->overwriteAccess(desiredAccess);

      if (auto *setterAttr = attrs.getAttribute<SetterAccessAttr>()) {
        if (setterAttr->getAccess() > desiredAccess)
          fixItAccess(diag, VD, desiredAccess, true);
      } else {
        ASD->overwriteSetterAccess(desiredAccess);
      }
    }
  }

  if (isForSetter && VD->getFormalAccess() == desiredAccess) {
    assert(attr);
    attr->setInvalid();
    // Remove the setter attribute.
    diag.fixItRemove(attr->Range);

  } else if (attr) {
    // If the formal access already matches the desired access, the problem
    // must be in a parent scope. Don't emit a fix-it.
    // FIXME: It's also possible for access to already be /broader/ than what's
    // desired, in which case the problem is also in a parent scope. However,
    // this function is sometimes called to make access narrower, so assuming
    // that a broader scope is acceptable breaks some diagnostics.
    if (attr->getAccess() != desiredAccess) {
      if (shouldUseDefaultAccess) {
        // Remove the attribute if replacement is not preferred.
        diag.fixItRemove(attr->getRange());
      } else {
        // This uses getLocation() instead of getRange() because we don't want to
        // replace the "(set)" part of a setter attribute.
        diag.fixItReplace(attr->getLocation(), fixItString.drop_back());
      }
      attr->setInvalid();
    }

  } else if (auto *override = VD->getAttrs().getAttribute<OverrideAttr>()) {
    // Insert the access in front of 'override', if it exists, in order to
    // match the same keyword order as produced by method autocompletion.
    diag.fixItInsert(override->getLocation(), fixItString);

  } else if (auto var = dyn_cast<VarDecl>(VD)) {
    if (auto PBD = var->getParentPatternBinding())
      diag.fixItInsert(PBD->getStartLoc(), fixItString);

  } else {
    diag.fixItInsert(VD->getStartLoc(), fixItString);
  }
}

/// Retrieve the type name to be used for determining whether we can
/// omit needless words.
static OmissionTypeName getTypeNameForOmission(Type type) {
  if (!type)
    return "";

  ASTContext &ctx = type->getASTContext();
  auto objcBoolType = ctx.getObjCBoolType();

  /// Determine the options associated with the given type.
  auto getOptions = [&](Type type) {
    // Look for Boolean types.
    OmissionTypeOptions options;

    // Look for Boolean types.
    if (type->isBool()) {
      // Swift.Bool
      options |= OmissionTypeFlags::Boolean;
    } else if (objcBoolType && type->isEqual(objcBoolType)) {
      // ObjectiveC.ObjCBool
      options |= OmissionTypeFlags::Boolean;
    }

    return options;
  };

  do {
    // Look through typealiases.
    if (auto aliasTy = dyn_cast<TypeAliasType>(type.getPointer())) {
      type = aliasTy->getSinglyDesugaredType();
      continue;
    }

    // Strip off lvalue/inout types.
    Type newType = type->getWithoutSpecifierType();
    if (newType.getPointer() != type.getPointer()) {
      type = newType;
      continue;
    }

    // Look through reference-storage types.
    newType = type->getReferenceStorageReferent();
    if (newType.getPointer() != type.getPointer()) {
      type = newType;
      continue;
    }

    // Look through parentheses.
    type = type->getWithoutParens();

    // Look through optionals.
    if (auto optObjectTy = type->getOptionalObjectType()) {
      type = optObjectTy;
      continue;
    }

    break;
  } while (true);

  // Nominal types.
  if (auto nominal = type->getAnyNominal()) {
    // If we have a collection, get the element type.
    if (auto bound = type->getAs<BoundGenericType>()) {
      auto args = bound->getGenericArgs();
      if (!args.empty() && (bound->isArray() || bound->isSet())) {
        return OmissionTypeName(nominal->getName().str(),
                                getOptions(bound),
                                getTypeNameForOmission(args[0]).Name);
      }
    }

    // AnyObject -> "Object".
    if (type->isAnyObject())
      return "Object";

    return OmissionTypeName(nominal->getName().str(), getOptions(type));
  }

  // Generic type parameters.
  if (auto genericParamTy = type->getAs<GenericTypeParamType>()) {
    if (auto genericParam = genericParamTy->getDecl())
      return genericParam->getName().str();

    return "";
  }

  // Dependent members.
  if (auto dependentMemberTy = type->getAs<DependentMemberType>()) {
    return dependentMemberTy->getName().str();
  }

  // Archetypes.
  if (auto archetypeTy = type->getAs<ArchetypeType>()) {
    return archetypeTy->getName().str();
  }

  // Function types.
  if (auto funcTy = type->getAs<AnyFunctionType>()) {
    if (funcTy->getRepresentation() == AnyFunctionType::Representation::Block)
      return "Block";

    return "Function";
  }
  return "";
}

llvm::Optional<DeclName>
TypeChecker::omitNeedlessWords(AbstractFunctionDecl *afd) {
  auto &Context = afd->getASTContext();

  if (afd->isInvalid() || isa<DestructorDecl>(afd))
    return llvm::None;

  const DeclName name = afd->getName();
  if (!name)
    return llvm::None;

  // String'ify the arguments.
  StringRef baseNameStr = name.getBaseName().userFacingName();
  SmallVector<StringRef, 4> argNameStrs;
  for (auto arg : name.getArgumentNames()) {
    if (arg.empty())
      argNameStrs.push_back("");
    else
      argNameStrs.push_back(arg.str());
  }

  // String'ify the parameter types.
  SmallVector<OmissionTypeName, 4> paramTypes;

  // Always look at the parameters in the last parameter list.
  for (auto param : *afd->getParameters()) {
    paramTypes.push_back(getTypeNameForOmission(param->getInterfaceType())
                         .withDefaultArgument(param->isDefaultArgument()));
  }
  
  // Handle contextual type, result type, and returnsSelf.
  Type contextType = afd->getDeclContext()->getDeclaredInterfaceType();
  Type resultType;
  bool returnsSelf = afd->hasDynamicSelfResult();

  if (auto func = dyn_cast<FuncDecl>(afd)) {
    resultType = func->getResultInterfaceType();
    resultType = func->mapTypeIntoContext(resultType);
  } else if (isa<ConstructorDecl>(afd)) {
    resultType = contextType;
  }

  // Figure out the first parameter name.
  StringRef firstParamName;
  auto params = afd->getParameters();
  if (params->size() != 0 && !params->get(0)->getName().empty())
    firstParamName = params->get(0)->getName().str();

  StringScratchSpace scratch;
  if (!swift::omitNeedlessWords(
          baseNameStr, argNameStrs, firstParamName,
          getTypeNameForOmission(resultType),
          getTypeNameForOmission(contextType), paramTypes, returnsSelf, false,
          /*allPropertyNames=*/nullptr, llvm::None, llvm::None, scratch))
    return llvm::None;

  /// Retrieve a replacement identifier.
  auto getReplacementIdentifier = [&](StringRef name,
                                      DeclBaseName old) -> DeclBaseName{
    if (name.empty())
      return Identifier();

    if (!old.empty() && name == old.userFacingName())
      return old;

    return Context.getIdentifier(name);
  };

  auto newBaseName = getReplacementIdentifier(
      baseNameStr, name.getBaseName());
  SmallVector<Identifier, 4> newArgNames;
  auto oldArgNames = name.getArgumentNames();
  for (unsigned i = 0, n = argNameStrs.size(); i != n; ++i) {
    auto argBaseName = getReplacementIdentifier(argNameStrs[i],
                                                oldArgNames[i]);
    newArgNames.push_back(argBaseName.getIdentifier());
  }

  return DeclName(Context, newBaseName, newArgNames);
}

llvm::Optional<Identifier> TypeChecker::omitNeedlessWords(VarDecl *var) {
  auto &Context = var->getASTContext();

  if (var->isInvalid())
    return llvm::None;

  if (var->getName().empty())
    return llvm::None;

  auto name = var->getName().str();

  // Dig out the context type.
  Type contextType = var->getDeclContext()->getDeclaredInterfaceType();
  if (!contextType)
    return llvm::None;

  // Dig out the type of the variable.
  Type type = var->getValueInterfaceType();
  while (auto optObjectTy = type->getOptionalObjectType())
    type = optObjectTy;

  // Omit needless words.
  StringScratchSpace scratch;
  OmissionTypeName typeName = getTypeNameForOmission(var->getInterfaceType());
  OmissionTypeName contextTypeName = getTypeNameForOmission(contextType);
  if (::omitNeedlessWords(name, {}, "", typeName, contextTypeName, {},
                          /*returnsSelf=*/false, true,
                          /*allPropertyNames=*/nullptr, llvm::None, llvm::None,
                          scratch)) {
    return Context.getIdentifier(name);
  }

  return llvm::None;
}

bool swift::diagnoseUnhandledThrowsInAsyncContext(DeclContext *dc,
                                                  ForEachStmt *forEach) {
  if (!forEach->getAwaitLoc().isValid())
    return false;

  auto conformanceRef = forEach->getSequenceConformance();
  if (conformanceRef.hasEffect(EffectKind::Throws) &&
      forEach->getTryLoc().isInvalid()) {
    auto &ctx = dc->getASTContext();
    ctx.Diags
        .diagnose(forEach->getAwaitLoc(), diag::throwing_call_unhandled, "call")
        .fixItInsert(forEach->getAwaitLoc(), "try");
    return true;
  }

  return false;
}

//===----------------------------------------------------------------------===//
//              Copyable Type Containing Move Only Type Visitor
//===----------------------------------------------------------------------===//

void swift::diagnoseCopyableTypeContainingMoveOnlyType(
    NominalTypeDecl *copyableNominalType) {
  // If we already have a move only type, just bail, we have no further work to
  // do.
  if (copyableNominalType->isMoveOnly())
    return;

  LLVM_DEBUG(llvm::dbgs() << "DiagnoseCopyableType for: "
                          << copyableNominalType->getName() << '\n');

  auto &DE = copyableNominalType->getASTContext().Diags;
  auto emitError = [&copyableNominalType,
                    &DE](PointerUnion<EnumElementDecl *, VarDecl *>
                             topFieldToError,
                         DeclBaseName parentName, DescriptiveDeclKind fieldKind,
                         DeclBaseName fieldName) {
    assert(!topFieldToError.isNull());
    if (auto *eltDecl = topFieldToError.dyn_cast<EnumElementDecl *>()) {
      DE.diagnoseWithNotes(
          copyableNominalType->diagnose(
              diag::noncopyable_within_copyable,
              copyableNominalType),
          [&]() {
            eltDecl->diagnose(
                diag::
                    noncopyable_within_copyable_location,
                fieldKind, parentName.userFacingName(),
                fieldName.userFacingName());
          });
      return;
    }

    auto *varDecl = topFieldToError.get<VarDecl *>();
    DE.diagnoseWithNotes(
        copyableNominalType->diagnose(
            diag::noncopyable_within_copyable,
            copyableNominalType),
        [&]() {
          varDecl->diagnose(
              diag::noncopyable_within_copyable_location,
              fieldKind, parentName.userFacingName(),
              fieldName.userFacingName());
        });
  };

  // If we have a struct decl...
  if (auto *structDecl = dyn_cast<StructDecl>(copyableNominalType)) {
    // Visit each of the stored property var decls of the struct decl...
    for (auto *fieldDecl : structDecl->getStoredProperties()) {
      LLVM_DEBUG(llvm::dbgs()
                 << "Visiting struct field: " << fieldDecl->getName() << '\n');
      if (!fieldDecl->getInterfaceType()->isPureMoveOnly())
        continue;
      emitError(fieldDecl, structDecl->getBaseName(),
                fieldDecl->getDescriptiveKind(), fieldDecl->getBaseName());
    }
    // We completed our checking, just return.
    return;
  }

  if (auto *enumDecl = dyn_cast<EnumDecl>(copyableNominalType)) {
    // If we have an enum but we don't have any elements, just continue, we
    // have nothing to check.
    if (enumDecl->getAllElements().empty())
      return;

    // Otherwise for each element...
    for (auto *enumEltDecl : enumDecl->getAllElements()) {
      // If the element doesn't have any associated values, we have nothing to
      // check, so continue.
      if (!enumEltDecl->hasAssociatedValues())
        continue;

      LLVM_DEBUG(llvm::dbgs() << "Visiting enum elt decl: "
                 << enumEltDecl->getName() << '\n');

      // Otherwise, we have a case and need to check the types of the
      // parameters of the case payload.
      for (auto payloadParam : *enumEltDecl->getParameterList()) {
        LLVM_DEBUG(llvm::dbgs() << "Visiting payload param: "
                   << payloadParam->getName() << '\n');
        if (payloadParam->getInterfaceType()->isPureMoveOnly()) {
            emitError(enumEltDecl, enumDecl->getBaseName(),
                      enumEltDecl->getDescriptiveKind(),
                      enumEltDecl->getBaseName());
        }
      }
    }    
    // We have finished processing this enum... so return.
    return;
  }
}
