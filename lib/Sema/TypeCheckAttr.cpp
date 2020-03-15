//===--- TypeCheckAttr.cpp - Type Checking for Attributes -----------------===//
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
// This file implements semantic analysis for attributes.
//
//===----------------------------------------------------------------------===//

#include "MiscDiagnostics.h"
#include "TypeCheckObjC.h"
#include "TypeCheckType.h"
#include "TypeChecker.h"
#include "swift/AST/ASTVisitor.h"
#include "swift/AST/ClangModuleLoader.h"
#include "swift/AST/DiagnosticsParse.h"
#include "swift/AST/GenericEnvironment.h"
#include "swift/AST/GenericSignatureBuilder.h"
#include "swift/AST/ModuleNameLookup.h"
#include "swift/AST/NameLookup.h"
#include "swift/AST/NameLookupRequests.h"
#include "swift/AST/ParameterList.h"
#include "swift/AST/PropertyWrappers.h"
#include "swift/AST/SourceFile.h"
#include "swift/AST/TypeCheckRequests.h"
#include "swift/AST/Types.h"
#include "swift/Parse/Lexer.h"
#include "swift/Sema/IDETypeChecking.h"
#include "clang/Basic/CharInfo.h"
#include "llvm/Support/Debug.h"

using namespace swift;

namespace {
  /// This emits a diagnostic with a fixit to remove the attribute.
  template<typename ...ArgTypes>
  void diagnoseAndRemoveAttr(DiagnosticEngine &Diags, Decl *D,
                             DeclAttribute *attr, ArgTypes &&...Args) {
    assert(!D->hasClangNode() && "Clang importer propagated a bogus attribute");
    if (!D->hasClangNode()) {
      SourceLoc loc = attr->getLocation();
      assert(loc.isValid() && "Diagnosing attribute with invalid location");
      if (loc.isInvalid()) {
        loc = D->getLoc();
      }
      if (loc.isValid()) {
        Diags.diagnose(loc, std::forward<ArgTypes>(Args)...)
          .fixItRemove(attr->getRangeWithAt());
      }
    }

    attr->setInvalid();
  }

/// This visits each attribute on a decl.  The visitor should return true if
/// the attribute is invalid and should be marked as such.
class AttributeChecker : public AttributeVisitor<AttributeChecker> {
  ASTContext &Ctx;
  Decl *D;

public:
  AttributeChecker(Decl *D) : Ctx(D->getASTContext()), D(D) {}

  /// This emits a diagnostic with a fixit to remove the attribute.
  template<typename ...ArgTypes>
  void diagnoseAndRemoveAttr(DeclAttribute *attr, ArgTypes &&...Args) {
    ::diagnoseAndRemoveAttr(Ctx.Diags, D, attr,
                            std::forward<ArgTypes>(Args)...);
  }

  template <typename... ArgTypes>
  InFlightDiagnostic diagnose(ArgTypes &&... Args) const {
    return Ctx.Diags.diagnose(std::forward<ArgTypes>(Args)...);
  }

  /// Deleting this ensures that all attributes are covered by the visitor
  /// below.
  bool visitDeclAttribute(DeclAttribute *A) = delete;

#define IGNORED_ATTR(X) void visit##X##Attr(X##Attr *) {}
  IGNORED_ATTR(AlwaysEmitIntoClient)
  IGNORED_ATTR(HasInitialValue)
  IGNORED_ATTR(ClangImporterSynthesizedType)
  IGNORED_ATTR(Convenience)
  IGNORED_ATTR(Effects)
  IGNORED_ATTR(Exported)
  IGNORED_ATTR(ForbidSerializingReference)
  IGNORED_ATTR(HasStorage)
  IGNORED_ATTR(HasMissingDesignatedInitializers)
  IGNORED_ATTR(InheritsConvenienceInitializers)
  IGNORED_ATTR(Inline)
  IGNORED_ATTR(ObjCBridged)
  IGNORED_ATTR(ObjCNonLazyRealization)
  IGNORED_ATTR(ObjCRuntimeName)
  IGNORED_ATTR(RawDocComment)
  IGNORED_ATTR(RequiresStoredPropertyInits)
  IGNORED_ATTR(RestatedObjCConformance)
  IGNORED_ATTR(Semantics)
  IGNORED_ATTR(ShowInInterface)
  IGNORED_ATTR(SILGenName)
  IGNORED_ATTR(StaticInitializeObjCMetadata)
  IGNORED_ATTR(SynthesizedProtocol)
  IGNORED_ATTR(Testable)
  IGNORED_ATTR(WeakLinked)
  IGNORED_ATTR(PrivateImport)
  IGNORED_ATTR(DisfavoredOverload)
  IGNORED_ATTR(ProjectedValueProperty)
  IGNORED_ATTR(ReferenceOwnership)
  IGNORED_ATTR(OriginallyDefinedIn)
  // TODO(TF-830): Upstream `@transpose` attribute type-checking from tensorflow
  // branch.
  IGNORED_ATTR(Transpose)
#undef IGNORED_ATTR

  void visitAlignmentAttr(AlignmentAttr *attr) {
    // Alignment must be a power of two.
    auto value = attr->getValue();
    if (value == 0 || (value & (value - 1)) != 0)
      diagnose(attr->getLocation(), diag::alignment_not_power_of_two);
  }

  void visitBorrowedAttr(BorrowedAttr *attr) {
    // These criteria are the same preconditions laid out by
    // AbstractStorageDecl::requiresOpaqueModifyCoroutine().

    assert(!D->hasClangNode() && "@_borrowed on imported declaration?");

    if (D->getAttrs().hasAttribute<DynamicAttr>()) {
      diagnose(attr->getLocation(), diag::borrowed_with_objc_dynamic,
               D->getDescriptiveKind())
        .fixItRemove(attr->getRange());
      D->getAttrs().removeAttribute(attr);
      return;
    }

    auto dc = D->getDeclContext();
    auto protoDecl = dyn_cast<ProtocolDecl>(dc);
    if (protoDecl && protoDecl->isObjC()) {
      diagnose(attr->getLocation(), diag::borrowed_on_objc_protocol_requirement,
               D->getDescriptiveKind())
        .fixItRemove(attr->getRange());
      D->getAttrs().removeAttribute(attr);
      return;
    }
  }

  void visitTransparentAttr(TransparentAttr *attr);
  void visitMutationAttr(DeclAttribute *attr);
  void visitMutatingAttr(MutatingAttr *attr) { visitMutationAttr(attr); }
  void visitNonMutatingAttr(NonMutatingAttr *attr) { visitMutationAttr(attr); }
  void visitConsumingAttr(ConsumingAttr *attr) { visitMutationAttr(attr); }
  void visitDynamicAttr(DynamicAttr *attr);

  void visitIndirectAttr(IndirectAttr *attr) {
    if (auto caseDecl = dyn_cast<EnumElementDecl>(D)) {
      // An indirect case should have a payload.
      if (!caseDecl->hasAssociatedValues())
        diagnose(attr->getLocation(), diag::indirect_case_without_payload,
                 caseDecl->getName());
      // If the enum is already indirect, its cases don't need to be.
      else if (caseDecl->getParentEnum()->getAttrs()
                 .hasAttribute<IndirectAttr>())
        diagnose(attr->getLocation(), diag::indirect_case_in_indirect_enum);
    }
  }

  void visitWarnUnqualifiedAccessAttr(WarnUnqualifiedAccessAttr *attr) {
    if (!D->getDeclContext()->isTypeContext()) {
      diagnoseAndRemoveAttr(attr, diag::attr_methods_only, attr);
    }
  }

  void visitFinalAttr(FinalAttr *attr);
  void visitIBActionAttr(IBActionAttr *attr);
  void visitIBSegueActionAttr(IBSegueActionAttr *attr);
  void visitLazyAttr(LazyAttr *attr);
  void visitIBDesignableAttr(IBDesignableAttr *attr);
  void visitIBInspectableAttr(IBInspectableAttr *attr);
  void visitGKInspectableAttr(GKInspectableAttr *attr);
  void visitIBOutletAttr(IBOutletAttr *attr);
  void visitLLDBDebuggerFunctionAttr(LLDBDebuggerFunctionAttr *attr);
  void visitNSManagedAttr(NSManagedAttr *attr);
  void visitOverrideAttr(OverrideAttr *attr);
  void visitNonOverrideAttr(NonOverrideAttr *attr);
  void visitAccessControlAttr(AccessControlAttr *attr);
  void visitSetterAccessAttr(SetterAccessAttr *attr);
  void visitSPIAccessControlAttr(SPIAccessControlAttr *attr);
  bool visitAbstractAccessControlAttr(AbstractAccessControlAttr *attr);

  void visitObjCAttr(ObjCAttr *attr);
  void visitNonObjCAttr(NonObjCAttr *attr);
  void visitObjCMembersAttr(ObjCMembersAttr *attr);

  void visitOptionalAttr(OptionalAttr *attr);

  void visitAvailableAttr(AvailableAttr *attr);

  void visitCDeclAttr(CDeclAttr *attr);

  void visitDynamicCallableAttr(DynamicCallableAttr *attr);

  void visitDynamicMemberLookupAttr(DynamicMemberLookupAttr *attr);

  void visitNSCopyingAttr(NSCopyingAttr *attr);
  void visitRequiredAttr(RequiredAttr *attr);
  void visitRethrowsAttr(RethrowsAttr *attr);

  void checkApplicationMainAttribute(DeclAttribute *attr,
                                     Identifier Id_ApplicationDelegate,
                                     Identifier Id_Kit,
                                     Identifier Id_ApplicationMain);

  void visitNSApplicationMainAttr(NSApplicationMainAttr *attr);
  void visitUIApplicationMainAttr(UIApplicationMainAttr *attr);

  void visitUnsafeNoObjCTaggedPointerAttr(UnsafeNoObjCTaggedPointerAttr *attr);
  void visitSwiftNativeObjCRuntimeBaseAttr(
                                         SwiftNativeObjCRuntimeBaseAttr *attr);

  void checkOperatorAttribute(DeclAttribute *attr);

  void visitInfixAttr(InfixAttr *attr) { checkOperatorAttribute(attr); }
  void visitPostfixAttr(PostfixAttr *attr) { checkOperatorAttribute(attr); }
  void visitPrefixAttr(PrefixAttr *attr) { checkOperatorAttribute(attr); }

  void visitSpecializeAttr(SpecializeAttr *attr);

  void visitFixedLayoutAttr(FixedLayoutAttr *attr);
  void visitUsableFromInlineAttr(UsableFromInlineAttr *attr);
  void visitInlinableAttr(InlinableAttr *attr);
  void visitOptimizeAttr(OptimizeAttr *attr);

  void visitDiscardableResultAttr(DiscardableResultAttr *attr);
  void visitDynamicReplacementAttr(DynamicReplacementAttr *attr);
  void visitTypeEraserAttr(TypeEraserAttr *attr);
  void visitImplementsAttr(ImplementsAttr *attr);

  void visitFrozenAttr(FrozenAttr *attr);

  void visitCustomAttr(CustomAttr *attr);
  void visitPropertyWrapperAttr(PropertyWrapperAttr *attr);
  void visitFunctionBuilderAttr(FunctionBuilderAttr *attr);

  void visitImplementationOnlyAttr(ImplementationOnlyAttr *attr);
  void visitNonEphemeralAttr(NonEphemeralAttr *attr);
  void checkOriginalDefinedInAttrs(Decl *D, ArrayRef<OriginallyDefinedInAttr*> Attrs);

  void visitDifferentiableAttr(DifferentiableAttr *attr);
  void visitDerivativeAttr(DerivativeAttr *attr);
  
  void visitRequiresConcreteImplementationAttr(
                                     RequiresConcreteImplementationAttr *attr);
};
} // end anonymous namespace

void AttributeChecker::visitTransparentAttr(TransparentAttr *attr) {
  DeclContext *dc = D->getDeclContext();
  // Protocol declarations cannot be transparent.
  if (isa<ProtocolDecl>(dc))
    diagnoseAndRemoveAttr(attr, diag::transparent_in_protocols_not_supported);
  // Class declarations cannot be transparent.
  if (isa<ClassDecl>(dc)) {
    
    // @transparent is always ok on implicitly generated accessors: they can
    // be dispatched (even in classes) when the references are within the
    // class themself.
    if (!(isa<AccessorDecl>(D) && D->isImplicit()))
      diagnoseAndRemoveAttr(attr, diag::transparent_in_classes_not_supported);
  }

  if (auto *VD = dyn_cast<VarDecl>(D)) {
    // Stored properties and variables can't be transparent.
    if (VD->hasStorage())
      diagnoseAndRemoveAttr(attr, diag::attribute_invalid_on_stored_property,
                            attr);
  }
}

void AttributeChecker::visitMutationAttr(DeclAttribute *attr) {
  FuncDecl *FD = cast<FuncDecl>(D);

  SelfAccessKind attrModifier;
  switch (attr->getKind()) {
  case DeclAttrKind::DAK_Consuming:
    attrModifier = SelfAccessKind::Consuming;
    break;
  case DeclAttrKind::DAK_Mutating:
    attrModifier = SelfAccessKind::Mutating;
    break;
  case DeclAttrKind::DAK_NonMutating:
    attrModifier = SelfAccessKind::NonMutating;
    break;
  default:
    llvm_unreachable("unhandled attribute kind");
  }

  // mutation attributes may only appear in type context.
  if (auto contextTy = FD->getDeclContext()->getDeclaredInterfaceType()) {
    // 'mutating' and 'nonmutating' are not valid on types
    // with reference semantics.
    if (contextTy->hasReferenceSemantics()) {
      if (attrModifier != SelfAccessKind::Consuming)
        diagnoseAndRemoveAttr(attr, diag::mutating_invalid_classes,
                              attrModifier);
    }
  } else {
    diagnoseAndRemoveAttr(attr, diag::mutating_invalid_global_scope,
                          attrModifier);
  }

  // Verify we don't have more than one of mutating, nonmutating,
  // and __consuming.
  if ((FD->getAttrs().hasAttribute<MutatingAttr>() +
          FD->getAttrs().hasAttribute<NonMutatingAttr>() +
          FD->getAttrs().hasAttribute<ConsumingAttr>()) > 1) {
    if (auto *NMA = FD->getAttrs().getAttribute<NonMutatingAttr>()) {
      if (attrModifier != SelfAccessKind::NonMutating) {
        diagnoseAndRemoveAttr(NMA, diag::functions_mutating_and_not,
                              SelfAccessKind::NonMutating, attrModifier);
      }
    }

    if (auto *MUA = FD->getAttrs().getAttribute<MutatingAttr>()) {
      if (attrModifier != SelfAccessKind::Mutating) {
        diagnoseAndRemoveAttr(MUA, diag::functions_mutating_and_not,
                                SelfAccessKind::Mutating, attrModifier);
      }
    }

    if (auto *CSA = FD->getAttrs().getAttribute<ConsumingAttr>()) {
      if (attrModifier != SelfAccessKind::Consuming) {
        diagnoseAndRemoveAttr(CSA, diag::functions_mutating_and_not,
                              SelfAccessKind::Consuming, attrModifier);
      }
    }
  }

  // Verify that we don't have a static function.
  if (FD->isStatic())
    diagnoseAndRemoveAttr(attr, diag::static_functions_not_mutating);
}

void AttributeChecker::visitDynamicAttr(DynamicAttr *attr) {
  // Members cannot be both dynamic and @_transparent.
  if (D->getAttrs().hasAttribute<TransparentAttr>())
    diagnoseAndRemoveAttr(attr, diag::dynamic_with_transparent);
  if (!D->getAttrs().hasAttribute<ObjCAttr>() &&
      D->getModuleContext()->isResilient())
    diagnoseAndRemoveAttr(attr,
                          diag::dynamic_and_library_evolution_not_supported);
}

static bool
validateIBActionSignature(ASTContext &ctx, DeclAttribute *attr,
                          const FuncDecl *FD, unsigned minParameters,
                          unsigned maxParameters, bool hasVoidResult = true) {
  bool valid = true;

  auto arity = FD->getParameters()->size();
  auto resultType = FD->getResultInterfaceType();

  if (arity < minParameters || arity > maxParameters) {
    auto diagID = diag::invalid_ibaction_argument_count;
    if (minParameters == maxParameters)
      diagID = diag::invalid_ibaction_argument_count_exact;
    else if (minParameters == 0)
      diagID = diag::invalid_ibaction_argument_count_max;
    ctx.Diags.diagnose(FD, diagID, attr->getAttrName(), minParameters,
                       maxParameters);
    valid = false;
  }

  if (resultType->isVoid() != hasVoidResult) {
    ctx.Diags.diagnose(FD, diag::invalid_ibaction_result, attr->getAttrName(),
                       hasVoidResult);
    valid = false;
  }

  // We don't need to check here that parameter or return types are
  // ObjC-representable; IsObjCRequest will validate that.

  if (!valid)
    attr->setInvalid();
  return valid;
}

static bool isiOS(ASTContext &ctx) {
  return ctx.LangOpts.Target.isiOS();
}

static bool iswatchOS(ASTContext &ctx) {
  return ctx.LangOpts.Target.isWatchOS();
}

static bool isRelaxedIBAction(ASTContext &ctx) {
  return isiOS(ctx) || iswatchOS(ctx);
}

void AttributeChecker::visitIBActionAttr(IBActionAttr *attr) {
  // Only instance methods can be IBActions.
  const FuncDecl *FD = cast<FuncDecl>(D);
  if (!FD->isPotentialIBActionTarget()) {
    diagnoseAndRemoveAttr(attr, diag::invalid_ibaction_decl,
                          attr->getAttrName());
    return;
  }

  if (isRelaxedIBAction(Ctx))
    // iOS, tvOS, and watchOS allow 0-2 parameters to an @IBAction method.
    validateIBActionSignature(Ctx, attr, FD, /*minParams=*/0, /*maxParams=*/2);
  else
    // macOS allows 1 parameter to an @IBAction method.
    validateIBActionSignature(Ctx, attr, FD, /*minParams=*/1, /*maxParams=*/1);
}

void AttributeChecker::visitIBSegueActionAttr(IBSegueActionAttr *attr) {
  // Only instance methods can be IBActions.
  const FuncDecl *FD = cast<FuncDecl>(D);
  if (!FD->isPotentialIBActionTarget())
    diagnoseAndRemoveAttr(attr, diag::invalid_ibaction_decl,
                          attr->getAttrName());

  if (!validateIBActionSignature(Ctx, attr, FD,
                                 /*minParams=*/1, /*maxParams=*/3,
                                 /*hasVoidResult=*/false))
    return;

  // If the IBSegueAction method's selector belongs to one of the ObjC method
  // families (like -newDocumentSegue: or -copyScreen), it would return the
  // object at +1, but the caller would expect it to be +0 and would therefore
  // leak it.
  //
  // To prevent that, diagnose if the selector belongs to one of the method
  // families and suggest that the user change the Swift name or Obj-C selector.
  auto currentSelector = FD->getObjCSelector();

  SmallString<32> prefix("make");

  switch (currentSelector.getSelectorFamily()) {
  case ObjCSelectorFamily::None:
    // No error--exit early.
    return;

  case ObjCSelectorFamily::Alloc:
  case ObjCSelectorFamily::Init:
  case ObjCSelectorFamily::New:
    // Fix-it will replace the "alloc"/"init"/"new" in the selector with "make".
    break;

  case ObjCSelectorFamily::Copy:
    // Fix-it will replace the "copy" in the selector with "makeCopy".
    prefix += "Copy";
    break;

  case ObjCSelectorFamily::MutableCopy:
    // Fix-it will replace the "mutable" in the selector with "makeMutable".
    prefix += "Mutable";
    break;
  }

  // Emit the actual error.
  diagnose(FD, diag::ibsegueaction_objc_method_family, attr->getAttrName(),
           currentSelector);

  // The rest of this is just fix-it generation.

  /// Replaces the first word of \c oldName with the prefix, where "word" is a
  /// sequence of lowercase characters.
  auto replacingPrefix = [&](Identifier oldName) -> Identifier {
    SmallString<32> scratch = prefix;
    scratch += oldName.str().drop_while(clang::isLowercase);
    return Ctx.getIdentifier(scratch);
  };

  // Suggest changing the Swift name of the method, unless there is already an
  // explicit selector.
  if (!FD->getAttrs().hasAttribute<ObjCAttr>() ||
      !FD->getAttrs().getAttribute<ObjCAttr>()->hasName()) {
    auto newSwiftBaseName = replacingPrefix(FD->getBaseName().getIdentifier());
    auto argumentNames = FD->getFullName().getArgumentNames();
    DeclName newSwiftName(Ctx, newSwiftBaseName, argumentNames);

    auto diag = diagnose(FD, diag::fixit_rename_in_swift, newSwiftName);
    fixDeclarationName(diag, FD, newSwiftName);
  }

  // Suggest changing just the selector to one with a different first piece.
  auto oldPieces = currentSelector.getSelectorPieces();
  SmallVector<Identifier, 4> newPieces(oldPieces.begin(), oldPieces.end());
  newPieces[0] = replacingPrefix(newPieces[0]);
  ObjCSelector newSelector(Ctx, currentSelector.getNumArgs(), newPieces);

  auto diag = diagnose(FD, diag::fixit_rename_in_objc, newSelector);
  fixDeclarationObjCName(diag, FD, currentSelector, newSelector);
}

void AttributeChecker::visitIBDesignableAttr(IBDesignableAttr *attr) {
  if (auto *ED = dyn_cast<ExtensionDecl>(D)) {
    if (auto nominalDecl = ED->getExtendedNominal()) {
      if (!isa<ClassDecl>(nominalDecl))
        diagnoseAndRemoveAttr(attr, diag::invalid_ibdesignable_extension);
    }
  }
}

void AttributeChecker::visitIBInspectableAttr(IBInspectableAttr *attr) {
  // Only instance properties can be 'IBInspectable'.
  auto *VD = cast<VarDecl>(D);
  if (!VD->getDeclContext()->getSelfClassDecl() || VD->isStatic())
    diagnoseAndRemoveAttr(attr, diag::invalid_ibinspectable,
                                 attr->getAttrName());
}

void AttributeChecker::visitGKInspectableAttr(GKInspectableAttr *attr) {
  // Only instance properties can be 'GKInspectable'.
  auto *VD = cast<VarDecl>(D);
  if (!VD->getDeclContext()->getSelfClassDecl() || VD->isStatic())
    diagnoseAndRemoveAttr(attr, diag::invalid_ibinspectable,
                                 attr->getAttrName());
}

static Optional<Diag<bool,Type>>
isAcceptableOutletType(Type type, bool &isArray, ASTContext &ctx) {
  if (type->isObjCExistentialType() || type->isAny())
    return None; // @objc existential types are okay

  auto nominal = type->getAnyNominal();

  if (auto classDecl = dyn_cast_or_null<ClassDecl>(nominal)) {
    if (classDecl->isObjC())
      return None; // @objc class types are okay.
    return diag::iboutlet_nonobjc_class;
  }

  if (nominal == ctx.getStringDecl()) {
    // String is okay because it is bridged to NSString.
    // FIXME: BridgesTypes.def is almost sufficient for this.
    return None;
  }

  if (nominal == ctx.getArrayDecl()) {
    // Arrays of arrays are not allowed.
    if (isArray)
      return diag::iboutlet_nonobject_type;

    isArray = true;

    // Handle Array<T>. T must be an Objective-C class or protocol.
    auto boundTy = type->castTo<BoundGenericStructType>();
    auto boundArgs = boundTy->getGenericArgs();
    assert(boundArgs.size() == 1 && "invalid Array declaration");
    Type elementTy = boundArgs.front();
    return isAcceptableOutletType(elementTy, isArray, ctx);
  }

  if (type->isExistentialType())
    return diag::iboutlet_nonobjc_protocol;

  // No other types are permitted.
  return diag::iboutlet_nonobject_type;
}


void AttributeChecker::visitIBOutletAttr(IBOutletAttr *attr) {
  // Only instance properties can be 'IBOutlet'.
  auto *VD = cast<VarDecl>(D);
  if (!VD->getDeclContext()->getSelfClassDecl() || VD->isStatic())
    diagnoseAndRemoveAttr(attr, diag::invalid_iboutlet);

  if (!VD->isSettable(nullptr)) {
    // Allow non-mutable IBOutlet properties in module interfaces,
    // as they may have been private(set)
    SourceFile *Parent = VD->getDeclContext()->getParentSourceFile();
    if (!Parent || Parent->Kind != SourceFileKind::Interface)
      diagnoseAndRemoveAttr(attr, diag::iboutlet_only_mutable);
  }

  // Verify that the field type is valid as an outlet.
  auto type = VD->getType();

  if (VD->isInvalid())
    return;

  // Look through ownership types, and optionals.
  type = type->getReferenceStorageReferent();
  bool wasOptional = false;
  if (Type underlying = type->getOptionalObjectType()) {
    type = underlying;
    wasOptional = true;
  }

  bool isArray = false;
  if (auto isError = isAcceptableOutletType(type, isArray, Ctx))
    diagnoseAndRemoveAttr(attr, isError.getValue(),
                                 /*array=*/isArray, type);

  // If the type wasn't optional, an array, or unowned, complain.
  if (!wasOptional && !isArray) {
    diagnose(attr->getLocation(), diag::iboutlet_non_optional, type);
    auto typeRange = VD->getTypeSourceRangeForDiagnostics();
    { // Only one diagnostic can be active at a time.
      auto diag = diagnose(typeRange.Start, diag::note_make_optional,
                           OptionalType::get(type));
      if (type->hasSimpleTypeRepr()) {
        diag.fixItInsertAfter(typeRange.End, "?");
      } else {
        diag.fixItInsert(typeRange.Start, "(")
          .fixItInsertAfter(typeRange.End, ")?");
      }
    }
    { // Only one diagnostic can be active at a time.
      auto diag = diagnose(typeRange.Start,
                           diag::note_make_implicitly_unwrapped_optional);
      if (type->hasSimpleTypeRepr()) {
        diag.fixItInsertAfter(typeRange.End, "!");
      } else {
        diag.fixItInsert(typeRange.Start, "(")
          .fixItInsertAfter(typeRange.End, ")!");
      }
    }
  }
}

void AttributeChecker::visitNSManagedAttr(NSManagedAttr *attr) {
  // @NSManaged only applies to instance methods and properties within a class.
  if (cast<ValueDecl>(D)->isStatic() ||
      !D->getDeclContext()->getSelfClassDecl()) {
    diagnoseAndRemoveAttr(attr, diag::attr_NSManaged_not_instance_member);
  }

  if (auto *method = dyn_cast<FuncDecl>(D)) {
    // Separate out the checks for methods.
    if (method->hasBody())
      diagnoseAndRemoveAttr(attr, diag::attr_NSManaged_method_body);

    return;
  }

  // Everything below deals with restrictions on @NSManaged properties.
  auto *VD = cast<VarDecl>(D);

  // @NSManaged properties cannot be @NSCopying
  if (auto *NSCopy = VD->getAttrs().getAttribute<NSCopyingAttr>())
    diagnoseAndRemoveAttr(NSCopy, diag::attr_NSManaged_NSCopying);

}

void AttributeChecker::
visitLLDBDebuggerFunctionAttr(LLDBDebuggerFunctionAttr *attr) {
  // This is only legal when debugger support is on.
  if (!D->getASTContext().LangOpts.DebuggerSupport)
    diagnoseAndRemoveAttr(attr, diag::attr_for_debugger_support_only);
}

void AttributeChecker::visitOverrideAttr(OverrideAttr *attr) {
  if (!isa<ClassDecl>(D->getDeclContext()) &&
      !isa<ProtocolDecl>(D->getDeclContext()) &&
      !isa<ExtensionDecl>(D->getDeclContext()))
    diagnoseAndRemoveAttr(attr, diag::override_nonclass_decl);
}

void AttributeChecker::visitNonOverrideAttr(NonOverrideAttr *attr) {
  if (auto overrideAttr = D->getAttrs().getAttribute<OverrideAttr>())
    diagnoseAndRemoveAttr(overrideAttr, diag::nonoverride_and_override_attr);

  if (!isa<ClassDecl>(D->getDeclContext()) &&
      !isa<ProtocolDecl>(D->getDeclContext()) &&
      !isa<ExtensionDecl>(D->getDeclContext())) {
    diagnoseAndRemoveAttr(attr, diag::nonoverride_wrong_decl_context);
  }
}

void AttributeChecker::visitLazyAttr(LazyAttr *attr) {
  // lazy may only be used on properties.
  auto *VD = cast<VarDecl>(D);

  auto attrs = VD->getAttrs();
  // 'lazy' is not allowed to have reference attributes
  if (auto *refAttr = attrs.getAttribute<ReferenceOwnershipAttr>())
    diagnoseAndRemoveAttr(attr, diag::lazy_not_strong, refAttr->get());

  auto varDC = VD->getDeclContext();

  // 'lazy' is not allowed on a global variable or on a static property (which
  // are already lazily initialized).
  // TODO: we can't currently support lazy properties on non-type-contexts.
  if (VD->isStatic() ||
      (varDC->isModuleScopeContext() &&
       !varDC->getParentSourceFile()->isScriptMode())) {
    diagnoseAndRemoveAttr(attr, diag::lazy_on_already_lazy_global);
  } else if (!VD->getDeclContext()->isTypeContext()) {
    diagnoseAndRemoveAttr(attr, diag::lazy_must_be_property);
  }
}

bool AttributeChecker::visitAbstractAccessControlAttr(
    AbstractAccessControlAttr *attr) {
  // Access control attr may only be used on value decls and extensions.
  if (!isa<ValueDecl>(D) && !isa<ExtensionDecl>(D)) {
    diagnoseAndRemoveAttr(attr, diag::invalid_decl_modifier, attr);
    return true;
  }

  if (auto extension = dyn_cast<ExtensionDecl>(D)) {
    if (!extension->getInherited().empty()) {
      diagnoseAndRemoveAttr(attr, diag::extension_access_with_conformances,
                            attr);
      return true;
    }
  }

  // And not on certain value decls.
  if (isa<DestructorDecl>(D) || isa<EnumElementDecl>(D)) {
    diagnoseAndRemoveAttr(attr, diag::invalid_decl_modifier, attr);
    return true;
  }

  // Or within protocols.
  if (isa<ProtocolDecl>(D->getDeclContext())) {
    diagnoseAndRemoveAttr(attr, diag::access_control_in_protocol, attr);
    diagnose(attr->getLocation(), diag::access_control_in_protocol_detail);
    return true;
  }

  return false;
}

void AttributeChecker::visitAccessControlAttr(AccessControlAttr *attr) {
  visitAbstractAccessControlAttr(attr);

  if (auto extension = dyn_cast<ExtensionDecl>(D)) {
    if (attr->getAccess() == AccessLevel::Open) {
      diagnose(attr->getLocation(), diag::access_control_extension_open)
        .fixItReplace(attr->getRange(), "public");
      attr->setInvalid();
      return;
    }

    NominalTypeDecl *nominal = extension->getExtendedNominal();

    // Extension is ill-formed; suppress the attribute.
    if (!nominal) {
      attr->setInvalid();
      return;
    }

    AccessLevel typeAccess = nominal->getFormalAccess();
    if (attr->getAccess() > typeAccess) {
      diagnose(attr->getLocation(), diag::access_control_extension_more,
               typeAccess, nominal->getDescriptiveKind(), attr->getAccess())
        .fixItRemove(attr->getRange());
      attr->setInvalid();
      return;
    }

  } else if (auto extension = dyn_cast<ExtensionDecl>(D->getDeclContext())) {
    AccessLevel maxAccess = extension->getMaxAccessLevel();
    if (std::min(attr->getAccess(), AccessLevel::Public) > maxAccess) {
      // FIXME: It would be nice to say what part of the requirements actually
      // end up being problematic.
      auto diag = diagnose(attr->getLocation(),
                           diag::access_control_ext_requirement_member_more,
                           attr->getAccess(),
                           D->getDescriptiveKind(),
                           maxAccess);
      swift::fixItAccess(diag, cast<ValueDecl>(D), maxAccess);
      return;
    }

    if (auto extAttr =
        extension->getAttrs().getAttribute<AccessControlAttr>()) {
      AccessLevel defaultAccess = extension->getDefaultAccessLevel();
      if (attr->getAccess() > defaultAccess) {
        auto diag = diagnose(attr->getLocation(),
                             diag::access_control_ext_member_more,
                             attr->getAccess(),
                             extAttr->getAccess());
        // Don't try to fix this one; it's just a warning, and fixing it can
        // lead to diagnostic fights between this and "declaration must be at
        // least this accessible" checking for overrides and protocol
        // requirements.
      } else if (attr->getAccess() == defaultAccess) {
        diagnose(attr->getLocation(),
                 diag::access_control_ext_member_redundant,
                 attr->getAccess(),
                 D->getDescriptiveKind(),
                 extAttr->getAccess())
          .fixItRemove(attr->getRange());
      }
    }
  }

  if (attr->getAccess() == AccessLevel::Open) {
    if (!isa<ClassDecl>(D) && !D->isPotentiallyOverridable() &&
        !attr->isInvalid()) {
      diagnose(attr->getLocation(), diag::access_control_open_bad_decl)
        .fixItReplace(attr->getRange(), "public");
      attr->setInvalid();
    }
  }
}

void AttributeChecker::visitSetterAccessAttr(
    SetterAccessAttr *attr) {
  auto storage = dyn_cast<AbstractStorageDecl>(D);
  if (!storage)
    diagnoseAndRemoveAttr(attr, diag::access_control_setter, attr->getAccess());

  if (visitAbstractAccessControlAttr(attr))
    return;

  if (!storage->isSettable(storage->getDeclContext())) {
    // This must stay in sync with diag::access_control_setter_read_only.
    enum {
      SK_Constant = 0,
      SK_Variable,
      SK_Property,
      SK_Subscript
    } storageKind;
    if (isa<SubscriptDecl>(storage))
      storageKind = SK_Subscript;
    else if (storage->getDeclContext()->isTypeContext())
      storageKind = SK_Property;
    else if (cast<VarDecl>(storage)->isLet())
      storageKind = SK_Constant;
    else
      storageKind = SK_Variable;
    diagnoseAndRemoveAttr(attr, diag::access_control_setter_read_only,
                          attr->getAccess(), storageKind);
  }

  auto getterAccess = cast<ValueDecl>(D)->getFormalAccess();
  if (attr->getAccess() > getterAccess) {
    // This must stay in sync with diag::access_control_setter_more.
    enum {
      SK_Variable = 0,
      SK_Property,
      SK_Subscript
    } storageKind;
    if (isa<SubscriptDecl>(D))
      storageKind = SK_Subscript;
    else if (D->getDeclContext()->isTypeContext())
      storageKind = SK_Property;
    else
      storageKind = SK_Variable;
    diagnose(attr->getLocation(), diag::access_control_setter_more,
             getterAccess, storageKind, attr->getAccess());
    attr->setInvalid();
    return;

  } else if (attr->getAccess() == getterAccess) {
    diagnose(attr->getLocation(),
             diag::access_control_setter_redundant,
             attr->getAccess(),
             D->getDescriptiveKind(),
             getterAccess)
      .fixItRemove(attr->getRange());
    return;
  }
}

void AttributeChecker::visitSPIAccessControlAttr(SPIAccessControlAttr *attr) {
  if (auto VD = dyn_cast<ValueDecl>(D)) {
    auto declAccess = VD->getFormalAccess();
    if (declAccess < AccessLevel::Public) {
      diagnoseAndRemoveAttr(attr,
                            diag::spi_attribute_on_non_public,
                            declAccess,
                            D->getDescriptiveKind());
    }
  }
}

static bool checkObjCDeclContext(Decl *D) {
  DeclContext *DC = D->getDeclContext();
  if (DC->getSelfClassDecl())
    return true;
  if (auto *PD = dyn_cast<ProtocolDecl>(DC))
    if (PD->isObjC())
      return true;
  return false;
}

void AttributeChecker::visitObjCAttr(ObjCAttr *attr) {
  // Only certain decls can be ObjC.
  Optional<Diag<>> error;
  if (isa<ClassDecl>(D) ||
      isa<ProtocolDecl>(D)) {
    /* ok */
  } else if (auto Ext = dyn_cast<ExtensionDecl>(D)) {
    if (!Ext->getSelfClassDecl())
      error = diag::objc_extension_not_class;
  } else if (auto ED = dyn_cast<EnumDecl>(D)) {
    if (ED->isGenericContext())
      error = diag::objc_enum_generic;
  } else if (auto EED = dyn_cast<EnumElementDecl>(D)) {
    auto ED = EED->getParentEnum();
    if (!ED->getAttrs().hasAttribute<ObjCAttr>())
      error = diag::objc_enum_case_req_objc_enum;
    else if (attr->hasName() && EED->getParentCase()->getElements().size() > 1)
      error = diag::objc_enum_case_multi;
  } else if (auto *func = dyn_cast<FuncDecl>(D)) {
    if (!checkObjCDeclContext(D))
      error = diag::invalid_objc_decl_context;
    else if (auto accessor = dyn_cast<AccessorDecl>(func))
      if (!accessor->isGetterOrSetter())
        error = diag::objc_observing_accessor;
  } else if (isa<ConstructorDecl>(D) ||
             isa<DestructorDecl>(D) ||
             isa<SubscriptDecl>(D) ||
             isa<VarDecl>(D)) {
    if (!checkObjCDeclContext(D))
      error = diag::invalid_objc_decl_context;
    /* ok */
  } else {
    error = diag::invalid_objc_decl;
  }

  if (error) {
    diagnoseAndRemoveAttr(attr, *error);
    return;
  }

  // If there is a name, check whether the kind of name is
  // appropriate.
  if (auto objcName = attr->getName()) {
    if (isa<ClassDecl>(D) || isa<ProtocolDecl>(D) || isa<VarDecl>(D)
        || isa<EnumDecl>(D) || isa<EnumElementDecl>(D)
        || isa<ExtensionDecl>(D)) {
      // Types and properties can only have nullary
      // names. Complain and recover by chopping off everything
      // after the first name.
      if (objcName->getNumArgs() > 0) {
        SourceLoc firstNameLoc = attr->getNameLocs().front();
        SourceLoc afterFirstNameLoc =
          Lexer::getLocForEndOfToken(Ctx.SourceMgr, firstNameLoc);
        diagnose(firstNameLoc, diag::objc_name_req_nullary,
                 D->getDescriptiveKind())
          .fixItRemoveChars(afterFirstNameLoc, attr->getRParenLoc());
        const_cast<ObjCAttr *>(attr)->setName(
          ObjCSelector(Ctx, 0, objcName->getSelectorPieces()[0]),
          /*implicit=*/false);
      }
    } else if (isa<SubscriptDecl>(D) || isa<DestructorDecl>(D)) {
      diagnose(attr->getLParenLoc(),
               isa<SubscriptDecl>(D)
                 ? diag::objc_name_subscript
                 : diag::objc_name_deinit);
      const_cast<ObjCAttr *>(attr)->clearName();
    } else {
      auto func = cast<AbstractFunctionDecl>(D);

      // Trigger lazy loading of any imported members with the same selector.
      // This ensures we correctly diagnose selector conflicts.
      if (auto *CD = D->getDeclContext()->getSelfClassDecl()) {
        (void) CD->lookupDirect(*objcName, !func->isStatic());
      }

      // We have a function. Make sure that the number of parameters
      // matches the "number of colons" in the name.
      auto params = func->getParameters();
      unsigned numParameters = params->size();
      if (auto CD = dyn_cast<ConstructorDecl>(func))
        if (CD->isObjCZeroParameterWithLongSelector())
          numParameters = 0;  // Something like "init(foo: ())"

      // A throwing method has an error parameter.
      if (func->hasThrows())
        ++numParameters;

      unsigned numArgumentNames = objcName->getNumArgs();
      if (numArgumentNames != numParameters) {
        diagnose(attr->getNameLocs().front(),
                 diag::objc_name_func_mismatch,
                 isa<FuncDecl>(func),
                 numArgumentNames,
                 numArgumentNames != 1,
                 numParameters,
                 numParameters != 1,
                 func->hasThrows());
        D->getAttrs().add(
          ObjCAttr::createUnnamed(Ctx, attr->AtLoc,  attr->Range.Start));
        D->getAttrs().removeAttribute(attr);
      }
    }
  } else if (isa<EnumElementDecl>(D)) {
    // Enum elements require names.
    diagnoseAndRemoveAttr(attr, diag::objc_enum_case_req_name);
  }
}

void AttributeChecker::visitNonObjCAttr(NonObjCAttr *attr) {
  // Only extensions of classes; methods, properties, subscripts
  // and constructors can be NonObjC.
  // The last three are handled automatically by generic attribute
  // validation -- for the first one, we have to check FuncDecls
  // ourselves.
  auto func = dyn_cast<FuncDecl>(D);
  if (func &&
      (isa<DestructorDecl>(func) ||
       !checkObjCDeclContext(func) ||
       (isa<AccessorDecl>(func) &&
        !cast<AccessorDecl>(func)->isGetterOrSetter()))) {
    diagnoseAndRemoveAttr(attr, diag::invalid_nonobjc_decl);
  }

  if (auto ext = dyn_cast<ExtensionDecl>(D)) {
    if (!ext->getSelfClassDecl())
      diagnoseAndRemoveAttr(attr, diag::invalid_nonobjc_extension);
  }
}

void AttributeChecker::visitObjCMembersAttr(ObjCMembersAttr *attr) {
  if (!isa<ClassDecl>(D))
    diagnoseAndRemoveAttr(attr, diag::objcmembers_attribute_nonclass);
}

void AttributeChecker::visitOptionalAttr(OptionalAttr *attr) {
  if (!isa<ProtocolDecl>(D->getDeclContext())) {
    diagnoseAndRemoveAttr(attr, diag::optional_attribute_non_protocol);
  } else if (!cast<ProtocolDecl>(D->getDeclContext())->isObjC()) {
    diagnoseAndRemoveAttr(attr, diag::optional_attribute_non_objc_protocol);
  } else if (isa<ConstructorDecl>(D)) {
    diagnoseAndRemoveAttr(attr, diag::optional_attribute_initializer);
  } else {
    auto objcAttr = D->getAttrs().getAttribute<ObjCAttr>();
    if (!objcAttr || objcAttr->isImplicit()) {
      auto diag = diagnose(attr->getLocation(),
                           diag::optional_attribute_missing_explicit_objc);
      if (auto VD = dyn_cast<ValueDecl>(D))
        diag.fixItInsert(VD->getAttributeInsertionLoc(false), "@objc ");
    }
  }
}

void TypeChecker::checkDeclAttributes(Decl *D) {
  AttributeChecker Checker(D);
  // We need to check all OriginallyDefinedInAttr relative to each other, so
  // collect them and check in batch later.
  llvm::SmallVector<OriginallyDefinedInAttr*, 4> ODIAttrs;
  for (auto attr : D->getAttrs()) {
    if (!attr->isValid()) continue;

    // If Attr.def says that the attribute cannot appear on this kind of
    // declaration, diagnose it and disable it.
    if (attr->canAppearOnDecl(D)) {
      if (auto *ODI = dyn_cast<OriginallyDefinedInAttr>(attr)) {
        ODIAttrs.push_back(ODI);
      } else {
        // Otherwise, check it.
        Checker.visit(attr);
      }
      continue;
    }

    // Otherwise, this attribute cannot be applied to this declaration.  If the
    // attribute is only valid on one kind of declaration (which is pretty
    // common) give a specific helpful error.
    auto PossibleDeclKinds = attr->getOptions() & DeclAttribute::OnAnyDecl;
    StringRef OnlyKind;
    switch (PossibleDeclKinds) {
    case DeclAttribute::OnAccessor:    OnlyKind = "accessor"; break;
    case DeclAttribute::OnClass:       OnlyKind = "class"; break;
    case DeclAttribute::OnConstructor: OnlyKind = "init"; break;
    case DeclAttribute::OnDestructor:  OnlyKind = "deinit"; break;
    case DeclAttribute::OnEnum:        OnlyKind = "enum"; break;
    case DeclAttribute::OnEnumCase:    OnlyKind = "case"; break;
    case DeclAttribute::OnFunc | DeclAttribute::OnAccessor: // FIXME
    case DeclAttribute::OnFunc:        OnlyKind = "func"; break;
    case DeclAttribute::OnImport:      OnlyKind = "import"; break;
    case DeclAttribute::OnModule:      OnlyKind = "module"; break;
    case DeclAttribute::OnParam:       OnlyKind = "parameter"; break;
    case DeclAttribute::OnProtocol:    OnlyKind = "protocol"; break;
    case DeclAttribute::OnStruct:      OnlyKind = "struct"; break;
    case DeclAttribute::OnSubscript:   OnlyKind = "subscript"; break;
    case DeclAttribute::OnTypeAlias:   OnlyKind = "typealias"; break;
    case DeclAttribute::OnVar:         OnlyKind = "var"; break;
    default: break;
    }

    if (!OnlyKind.empty())
      Checker.diagnoseAndRemoveAttr(attr, diag::attr_only_one_decl_kind,
                                    attr, OnlyKind);
    else if (attr->isDeclModifier())
      Checker.diagnoseAndRemoveAttr(attr, diag::invalid_decl_modifier, attr);
    else
      Checker.diagnoseAndRemoveAttr(attr, diag::invalid_decl_attribute, attr);
  }
  Checker.checkOriginalDefinedInAttrs(D, ODIAttrs);
}

/// Returns true if the given method is an valid implementation of a
/// @dynamicCallable attribute requirement. The method is given to be defined
/// as one of the following: `dynamicallyCall(withArguments:)` or
/// `dynamicallyCall(withKeywordArguments:)`.
bool swift::isValidDynamicCallableMethod(FuncDecl *decl, DeclContext *DC,
                                         bool hasKeywordArguments) {
  auto &ctx = decl->getASTContext();
  // There are two cases to check.
  // 1. `dynamicallyCall(withArguments:)`.
  //    In this case, the method is valid if the argument has type `A` where
  //    `A` conforms to `ExpressibleByArrayLiteral`.
  //    `A.ArrayLiteralElement` and the return type can be arbitrary.
  // 2. `dynamicallyCall(withKeywordArguments:)`
  //    In this case, the method is valid if the argument has type `D` where
  //    `D` conforms to `ExpressibleByDictionaryLiteral` and `D.Key` conforms to
  //    `ExpressibleByStringLiteral`.
  //    `D.Value` and the return type can be arbitrary.

  auto paramList = decl->getParameters();
  if (paramList->size() != 1 || paramList->get(0)->isVariadic()) return false;
  auto argType = paramList->get(0)->getType();

  // If non-keyword (positional) arguments, check that argument type conforms to
  // `ExpressibleByArrayLiteral`.
  if (!hasKeywordArguments) {
    auto arrayLitProto =
      ctx.getProtocol(KnownProtocolKind::ExpressibleByArrayLiteral);
    return (bool)TypeChecker::conformsToProtocol(argType, arrayLitProto, DC,
                                                 ConformanceCheckOptions());
  }
  // If keyword arguments, check that argument type conforms to
  // `ExpressibleByDictionaryLiteral` and that the `Key` associated type
  // conforms to `ExpressibleByStringLiteral`.
  auto stringLitProtocol =
    ctx.getProtocol(KnownProtocolKind::ExpressibleByStringLiteral);
  auto dictLitProto =
    ctx.getProtocol(KnownProtocolKind::ExpressibleByDictionaryLiteral);
  auto dictConf = TypeChecker::conformsToProtocol(argType, dictLitProto, DC,
                                                  ConformanceCheckOptions());
  if (dictConf.isInvalid())
    return false;
  auto keyType = dictConf.getTypeWitnessByName(argType, ctx.Id_Key);
  return (bool)TypeChecker::conformsToProtocol(keyType, stringLitProtocol, DC,
                                               ConformanceCheckOptions());
}

/// Returns true if the given nominal type has a valid implementation of a
/// @dynamicCallable attribute requirement with the given argument name.
static bool hasValidDynamicCallableMethod(NominalTypeDecl *decl,
                                          Identifier argumentName,
                                          bool hasKeywordArgs) {
  auto &ctx = decl->getASTContext();
  auto declType = decl->getDeclaredType();
  DeclNameRef methodName({ ctx, ctx.Id_dynamicallyCall, { argumentName } });
  auto candidates = TypeChecker::lookupMember(decl, declType, methodName);
  if (candidates.empty()) return false;

  // Filter valid candidates.
  candidates.filter([&](LookupResultEntry entry, bool isOuter) {
    auto candidate = cast<FuncDecl>(entry.getValueDecl());
    return isValidDynamicCallableMethod(candidate, decl, hasKeywordArgs);
  });

  // If there are no valid candidates, return false.
  if (candidates.size() == 0) return false;
  return true;
}

void AttributeChecker::
visitDynamicCallableAttr(DynamicCallableAttr *attr) {
  // This attribute is only allowed on nominal types.
  auto decl = cast<NominalTypeDecl>(D);
  auto type = decl->getDeclaredType();

  bool hasValidMethod = false;
  hasValidMethod |=
    hasValidDynamicCallableMethod(decl, Ctx.Id_withArguments,
                                  /*hasKeywordArgs*/ false);
  hasValidMethod |=
    hasValidDynamicCallableMethod(decl, Ctx.Id_withKeywordArguments,
                                  /*hasKeywordArgs*/ true);
  if (!hasValidMethod) {
    diagnose(attr->getLocation(), diag::invalid_dynamic_callable_type, type);
    attr->setInvalid();
  }
}

static bool hasSingleNonVariadicParam(SubscriptDecl *decl,
                                      Identifier expectedLabel,
                                      bool ignoreLabel = false) {
  auto *indices = decl->getIndices();
  if (decl->isInvalid() || indices->size() != 1)
    return false;

  auto *index = indices->get(0);
  if (index->isVariadic() || !index->hasInterfaceType())
    return false;

  if (ignoreLabel) {
    return true;
  }

  return index->getArgumentName() == expectedLabel;
}

/// Returns true if the given subscript method is an valid implementation of
/// the `subscript(dynamicMember:)` requirement for @dynamicMemberLookup.
/// The method is given to be defined as `subscript(dynamicMember:)`.
bool swift::isValidDynamicMemberLookupSubscript(SubscriptDecl *decl,
                                                DeclContext *DC,
                                                bool ignoreLabel) {
  // It could be
  // - `subscript(dynamicMember: {Writable}KeyPath<...>)`; or
  // - `subscript(dynamicMember: String*)`
  return isValidKeyPathDynamicMemberLookup(decl, ignoreLabel) ||
         isValidStringDynamicMemberLookup(decl, DC, ignoreLabel);
}

bool swift::isValidStringDynamicMemberLookup(SubscriptDecl *decl,
                                             DeclContext *DC,
                                             bool ignoreLabel) {
  auto &ctx = decl->getASTContext();
  // There are two requirements:
  // - The subscript method has exactly one, non-variadic parameter.
  // - The parameter type conforms to `ExpressibleByStringLiteral`.
  if (!hasSingleNonVariadicParam(decl, ctx.Id_dynamicMember,
                                 ignoreLabel))
    return false;

  const auto *param = decl->getIndices()->get(0);
  auto paramType = param->getType();

  auto stringLitProto =
    ctx.getProtocol(KnownProtocolKind::ExpressibleByStringLiteral);

  // If this is `subscript(dynamicMember: String*)`
  return (bool)TypeChecker::conformsToProtocol(paramType, stringLitProto, DC,
                                               ConformanceCheckOptions());
}

bool swift::isValidKeyPathDynamicMemberLookup(SubscriptDecl *decl,
                                              bool ignoreLabel) {
  auto &ctx = decl->getASTContext();
  if (!hasSingleNonVariadicParam(decl, ctx.Id_dynamicMember,
                                 ignoreLabel))
    return false;

  const auto *param = decl->getIndices()->get(0);
  if (auto NTD = param->getInterfaceType()->getAnyNominal()) {
    return NTD == ctx.getKeyPathDecl() ||
           NTD == ctx.getWritableKeyPathDecl() ||
           NTD == ctx.getReferenceWritableKeyPathDecl();
  }
  return false;
}

/// The @dynamicMemberLookup attribute is only allowed on types that have at
/// least one subscript member declared like this:
///
/// subscript<KeywordType: ExpressibleByStringLiteral, LookupValue>
///   (dynamicMember name: KeywordType) -> LookupValue { get }
///
/// ... but doesn't care about the mutating'ness of the getter/setter.
/// We just manually check the requirements here.
void AttributeChecker::
visitDynamicMemberLookupAttr(DynamicMemberLookupAttr *attr) {
  // This attribute is only allowed on nominal types.
  auto decl = cast<NominalTypeDecl>(D);
  auto type = decl->getDeclaredType();
  auto &ctx = decl->getASTContext();

  auto emitInvalidTypeDiagnostic = [&](const SourceLoc loc) {
    diagnose(loc, diag::invalid_dynamic_member_lookup_type, type);
    attr->setInvalid();
  };

  // Look up `subscript(dynamicMember:)` candidates.
  DeclNameRef subscriptName(
      { ctx, DeclBaseName::createSubscript(), { ctx.Id_dynamicMember } });
  auto candidates = TypeChecker::lookupMember(decl, type, subscriptName);

  if (!candidates.empty()) {
    // If no candidates are valid, then reject one.
    auto oneCandidate = candidates.front().getValueDecl();
    candidates.filter([&](LookupResultEntry entry, bool isOuter) -> bool {
      auto cand = cast<SubscriptDecl>(entry.getValueDecl());
      return isValidDynamicMemberLookupSubscript(cand, decl);
    });

    if (candidates.empty()) {
      emitInvalidTypeDiagnostic(oneCandidate->getLoc());
    }

    return;
  }

  // If we couldn't find any candidates, it's likely because:
  //
  // 1. We don't have a subscript with `dynamicMember` label.
  // 2. We have a subscript with `dynamicMember` label, but no argument label.
  //
  // Let's do another lookup using just the base name.
  auto newCandidates =
      TypeChecker::lookupMember(decl, type, DeclNameRef::createSubscript());

  // Validate the candidates while ignoring the label.
  newCandidates.filter([&](const LookupResultEntry entry, bool isOuter) {
    auto cand = cast<SubscriptDecl>(entry.getValueDecl());
    return isValidDynamicMemberLookupSubscript(cand, decl,
                                               /*ignoreLabel*/ true);
  });

  // If there were no potentially valid candidates, then throw an error.
  if (newCandidates.empty()) {
    emitInvalidTypeDiagnostic(attr->getLocation());
    return;
  }

  // For each candidate, emit a diagnostic. If we don't have an explicit
  // argument label, then emit a fix-it to suggest the user to add one.
  for (auto cand : newCandidates) {
    auto SD = cast<SubscriptDecl>(cand.getValueDecl());
    auto index = SD->getIndices()->get(0);
    diagnose(SD, diag::invalid_dynamic_member_lookup_type, type);

    // If we have something like `subscript(foo:)` then we want to insert
    // `dynamicMember` before `foo`.
    if (index->getParameterNameLoc().isValid() &&
        index->getArgumentNameLoc().isInvalid()) {
      diagnose(SD, diag::invalid_dynamic_member_subscript)
          .highlight(index->getSourceRange())
          .fixItInsert(index->getParameterNameLoc(), "dynamicMember ");
    }
  }

  attr->setInvalid();
  return;
}

/// Get the innermost enclosing declaration for a declaration.
static Decl *getEnclosingDeclForDecl(Decl *D) {
  // If the declaration is an accessor, treat its storage declaration
  // as the enclosing declaration.
  if (auto *accessor = dyn_cast<AccessorDecl>(D)) {
    return accessor->getStorage();
  }

  return D->getDeclContext()->getInnermostDeclarationDeclContext();
}

void AttributeChecker::visitAvailableAttr(AvailableAttr *attr) {
  if (Ctx.LangOpts.DisableAvailabilityChecking)
    return;

  if (auto *PD = dyn_cast<ProtocolDecl>(D->getDeclContext())) {
    if (auto *VD = dyn_cast<ValueDecl>(D)) {
      if (VD->isProtocolRequirement()) {
        if (attr->isActivePlatform(Ctx) ||
            attr->isLanguageVersionSpecific() ||
            attr->isPackageDescriptionVersionSpecific()) {
          auto versionAvailability = attr->getVersionAvailability(Ctx);
          if (attr->isUnconditionallyUnavailable() ||
              versionAvailability == AvailableVersionComparison::Obsoleted ||
              versionAvailability == AvailableVersionComparison::Unavailable) {
              if (!PD->isObjC()) {
                diagnoseAndRemoveAttr(attr, diag::unavailable_method_non_objc_protocol);
                return;
              }
            }
          }
        }
    }
  }

  if (!attr->hasPlatform() || !attr->isActivePlatform(Ctx) ||
      !attr->Introduced.hasValue()) {
    return;
  }

  // Make sure there isn't a more specific attribute we should be using instead.
  // findMostSpecificActivePlatform() is O(N), so only do this if we're checking
  // an iOS attribute while building for macCatalyst.
  if (attr->Platform == PlatformKind::iOS &&
      isPlatformActive(PlatformKind::macCatalyst, Ctx.LangOpts)) {
    if (attr != D->getAttrs().findMostSpecificActivePlatform(Ctx)) {
      return;
    }
  }

  SourceLoc attrLoc = attr->getLocation();

  Optional<Diag<>> MaybeNotAllowed =
      TypeChecker::diagnosticIfDeclCannotBePotentiallyUnavailable(D);
  if (MaybeNotAllowed.hasValue()) {
    diagnose(attrLoc, MaybeNotAllowed.getValue());
  }

  // Find the innermost enclosing declaration with an availability
  // range annotation and ensure that this attribute's available version range
  // is fully contained within that declaration's range. If there is no such
  // enclosing declaration, then there is nothing to check.
  Optional<AvailabilityContext> EnclosingAnnotatedRange;
  Decl *EnclosingDecl = getEnclosingDeclForDecl(D);

  while (EnclosingDecl) {
    EnclosingAnnotatedRange =
        AvailabilityInference::annotatedAvailableRange(EnclosingDecl, Ctx);

    if (EnclosingAnnotatedRange.hasValue())
      break;

    EnclosingDecl = getEnclosingDeclForDecl(EnclosingDecl);
  }

  if (!EnclosingDecl)
    return;

  AvailabilityContext AttrRange{
      VersionRange::allGTE(attr->Introduced.getValue())};

  if (!AttrRange.isContainedIn(EnclosingAnnotatedRange.getValue())) {
    diagnose(attr->getLocation(), diag::availability_decl_more_than_enclosing);
    diagnose(EnclosingDecl->getLoc(),
             diag::availability_decl_more_than_enclosing_enclosing_here);
  }
}

void AttributeChecker::visitCDeclAttr(CDeclAttr *attr) {
  // Only top-level func decls are currently supported.
  if (D->getDeclContext()->isTypeContext())
    diagnose(attr->getLocation(), diag::cdecl_not_at_top_level);

  // The name must not be empty.
  if (attr->Name.empty())
    diagnose(attr->getLocation(), diag::cdecl_empty_name);
}

void AttributeChecker::visitUnsafeNoObjCTaggedPointerAttr(
                                          UnsafeNoObjCTaggedPointerAttr *attr) {
  // Only class protocols can have the attribute.
  auto proto = dyn_cast<ProtocolDecl>(D);
  if (!proto) {
    diagnose(attr->getLocation(),
             diag::no_objc_tagged_pointer_not_class_protocol);
    attr->setInvalid();
  }
  
  if (!proto->requiresClass()
      && !proto->getAttrs().hasAttribute<ObjCAttr>()) {
    diagnose(attr->getLocation(),
             diag::no_objc_tagged_pointer_not_class_protocol);
    attr->setInvalid();    
  }
}

void AttributeChecker::visitSwiftNativeObjCRuntimeBaseAttr(
                                         SwiftNativeObjCRuntimeBaseAttr *attr) {
  // Only root classes can have the attribute.
  auto theClass = dyn_cast<ClassDecl>(D);
  if (!theClass) {
    diagnose(attr->getLocation(),
             diag::swift_native_objc_runtime_base_not_on_root_class);
    attr->setInvalid();
    return;
  }

  if (theClass->hasSuperclass()) {
    diagnose(attr->getLocation(),
             diag::swift_native_objc_runtime_base_not_on_root_class);
    attr->setInvalid();
    return;
  }
}

void AttributeChecker::visitFinalAttr(FinalAttr *attr) {
  // Reject combining 'final' with 'open'.
  if (auto accessAttr = D->getAttrs().getAttribute<AccessControlAttr>()) {
    if (accessAttr->getAccess() == AccessLevel::Open) {
      diagnose(attr->getLocation(), diag::open_decl_cannot_be_final,
               D->getDescriptiveKind());
      return;
    }
  }

  if (isa<ClassDecl>(D))
    return;

  // 'final' only makes sense in the context of a class declaration.
  // Reject it on global functions, protocols, structs, enums, etc.
  if (!D->getDeclContext()->getSelfClassDecl()) {
    diagnose(attr->getLocation(), diag::member_cannot_be_final)
      .fixItRemove(attr->getRange());

    // Remove the attribute so child declarations are not flagged as final
    // and duplicate the error message.
    D->getAttrs().removeAttribute(attr);
    return;
  }

  // We currently only support final on var/let, func and subscript
  // declarations.
  if (!isa<VarDecl>(D) && !isa<FuncDecl>(D) && !isa<SubscriptDecl>(D)) {
    diagnose(attr->getLocation(), diag::final_not_allowed_here)
      .fixItRemove(attr->getRange());
    return;
  }

  if (auto *accessor = dyn_cast<AccessorDecl>(D)) {
    if (!attr->isImplicit()) {
      unsigned Kind = 2;
      if (auto *VD = dyn_cast<VarDecl>(accessor->getStorage()))
        Kind = VD->isLet() ? 1 : 0;
      diagnose(attr->getLocation(), diag::final_not_on_accessors, Kind)
        .fixItRemove(attr->getRange());
      return;
    }
  }
}

/// Return true if this is a builtin operator that cannot be defined in user
/// code.
static bool isBuiltinOperator(StringRef name, DeclAttribute *attr) {
  return ((isa<PrefixAttr>(attr)  && name == "&") ||   // lvalue to inout
          (isa<PostfixAttr>(attr) && name == "!") ||   // optional unwrapping
          // FIXME: Not actually a builtin operator, but should probably
          // be allowed and accounted for in Sema?
          (isa<PrefixAttr>(attr)  && name == "?") ||
          (isa<PostfixAttr>(attr) && name == "?") ||   // optional chaining
          (isa<InfixAttr>(attr)   && name == "?") ||   // ternary operator
          (isa<PostfixAttr>(attr) && name == ">") ||   // generic argument list
          (isa<PrefixAttr>(attr)  && name == "<") ||   // generic argument list
                                     name == "="  ||   // Assignment
          // FIXME: Should probably be allowed in expression position?
                                     name == "->");
}

void AttributeChecker::checkOperatorAttribute(DeclAttribute *attr) {
  // Check out the operator attributes.  They may be attached to an operator
  // declaration or a function.
  if (auto *OD = dyn_cast<OperatorDecl>(D)) {
    // Reject attempts to define builtin operators.
    if (isBuiltinOperator(OD->getName().str(), attr)) {
      diagnose(D->getStartLoc(), diag::redefining_builtin_operator,
               attr->getAttrName(), OD->getName().str());
      attr->setInvalid();
      return;
    }

    // Otherwise, the attribute is always ok on an operator.
    return;
  }

  // Operators implementations may only be defined as functions.
  auto *FD = dyn_cast<FuncDecl>(D);
  if (!FD) {
    diagnose(D->getLoc(), diag::operator_not_func);
    attr->setInvalid();
    return;
  }

  // Only functions with an operator identifier can be declared with as an
  // operator.
  if (!FD->isOperator()) {
    diagnose(D->getStartLoc(), diag::attribute_requires_operator_identifier,
             attr->getAttrName());
    attr->setInvalid();
    return;
  }

  // Reject attempts to define builtin operators.
  if (isBuiltinOperator(FD->getName().str(), attr)) {
    diagnose(D->getStartLoc(), diag::redefining_builtin_operator,
             attr->getAttrName(), FD->getName().str());
    attr->setInvalid();
    return;
  }

  // Otherwise, must be unary.
  if (!FD->isUnaryOperator()) {
    diagnose(attr->getLocation(), diag::attribute_requires_single_argument,
             attr->getAttrName());
    attr->setInvalid();
    return;
  }
}

void AttributeChecker::visitNSCopyingAttr(NSCopyingAttr *attr) {
  // The @NSCopying attribute is only allowed on stored properties.
  auto *VD = cast<VarDecl>(D);

  // It may only be used on class members.
  auto classDecl = D->getDeclContext()->getSelfClassDecl();
  if (!classDecl) {
    diagnose(attr->getLocation(), diag::nscopying_only_on_class_properties);
    attr->setInvalid();
    return;
  }

  if (!VD->isSettable(VD->getDeclContext())) {
    diagnose(attr->getLocation(), diag::nscopying_only_mutable);
    attr->setInvalid();
    return;
  }

  if (!VD->hasStorage()) {
    diagnose(attr->getLocation(), diag::nscopying_only_stored_property);
    attr->setInvalid();
    return;
  }

  if (VD->hasInterfaceType()) {
    if (TypeChecker::checkConformanceToNSCopying(VD).isInvalid()) {
      attr->setInvalid();
      return;
    }
  }

  assert(VD->getOverriddenDecl() == nullptr &&
         "Can't have value with storage that is an override");

  // Check the type.  It must be an [unchecked]optional, weak, a normal
  // class, AnyObject, or classbound protocol.
  // It must conform to the NSCopying protocol.

}

void AttributeChecker::checkApplicationMainAttribute(DeclAttribute *attr,
                                             Identifier Id_ApplicationDelegate,
                                             Identifier Id_Kit,
                                             Identifier Id_ApplicationMain) {
  // %select indexes for ApplicationMain diagnostics.
  enum : unsigned {
    UIApplicationMainClass,
    NSApplicationMainClass,
  };

  unsigned applicationMainKind;
  if (isa<UIApplicationMainAttr>(attr))
    applicationMainKind = UIApplicationMainClass;
  else if (isa<NSApplicationMainAttr>(attr))
    applicationMainKind = NSApplicationMainClass;
  else
    llvm_unreachable("not an ApplicationMain attr");

  auto *CD = dyn_cast<ClassDecl>(D);

  // The applicant not being a class should have been diagnosed by the early
  // checker.
  if (!CD) return;

  // The class cannot be generic.
  if (CD->isGenericContext()) {
    diagnose(attr->getLocation(),
             diag::attr_generic_ApplicationMain_not_supported,
             applicationMainKind);
    attr->setInvalid();
    return;
  }

  // @XXApplicationMain classes must conform to the XXApplicationDelegate
  // protocol.
  auto *SF = cast<SourceFile>(CD->getModuleScopeContext());
  auto &C = SF->getASTContext();

  auto KitModule = C.getLoadedModule(Id_Kit);
  ProtocolDecl *ApplicationDelegateProto = nullptr;
  if (KitModule) {
    SmallVector<ValueDecl *, 1> decls;
    namelookup::lookupInModule(KitModule, Id_ApplicationDelegate,
                               decls, NLKind::QualifiedLookup,
                               namelookup::ResolutionKind::TypesOnly,
                               SF);
    if (decls.size() == 1)
      ApplicationDelegateProto = dyn_cast<ProtocolDecl>(decls[0]);
  }

  if (!ApplicationDelegateProto ||
      !TypeChecker::conformsToProtocol(CD->getDeclaredType(),
                                       ApplicationDelegateProto, CD, None)) {
    diagnose(attr->getLocation(),
             diag::attr_ApplicationMain_not_ApplicationDelegate,
             applicationMainKind);
    attr->setInvalid();
  }

  if (attr->isInvalid())
    return;

  // Register the class as the main class in the module. If there are multiples
  // they will be diagnosed.
  if (SF->registerMainClass(CD, attr->getLocation()))
    attr->setInvalid();
}

void AttributeChecker::visitNSApplicationMainAttr(NSApplicationMainAttr *attr) {
  auto &C = D->getASTContext();
  checkApplicationMainAttribute(attr,
                                C.getIdentifier("NSApplicationDelegate"),
                                C.getIdentifier("AppKit"),
                                C.getIdentifier("NSApplicationMain"));
}
void AttributeChecker::visitUIApplicationMainAttr(UIApplicationMainAttr *attr) {
  auto &C = D->getASTContext();
  checkApplicationMainAttribute(attr,
                                C.getIdentifier("UIApplicationDelegate"),
                                C.getIdentifier("UIKit"),
                                C.getIdentifier("UIApplicationMain"));
}

/// Determine whether the given context is an extension to an Objective-C class
/// where the class is defined in the Objective-C module and the extension is
/// defined within its module.
static bool isObjCClassExtensionInOverlay(DeclContext *dc) {
  // Check whether we have an extension.
  auto ext = dyn_cast<ExtensionDecl>(dc);
  if (!ext)
    return false;

  // Find the extended class.
  auto classDecl = ext->getSelfClassDecl();
  if (!classDecl)
    return false;

  auto clangLoader = dc->getASTContext().getClangModuleLoader();
  if (!clangLoader) return false;
  return clangLoader->isInOverlayModuleForImportedModule(ext, classDecl);
}

void AttributeChecker::visitRequiredAttr(RequiredAttr *attr) {
  // The required attribute only applies to constructors.
  auto ctor = cast<ConstructorDecl>(D);
  auto parentTy = ctor->getDeclContext()->getDeclaredInterfaceType();
  if (!parentTy) {
    // Constructor outside of nominal type context; we've already complained
    // elsewhere.
    attr->setInvalid();
    return;
  }
  // Only classes can have required constructors.
  if (parentTy->getClassOrBoundGenericClass()) {
    // The constructor must be declared within the class itself.
    // FIXME: Allow an SDK overlay to add a required initializer to a class
    // defined in Objective-C
    if (!isa<ClassDecl>(ctor->getDeclContext()) &&
        !isObjCClassExtensionInOverlay(ctor->getDeclContext())) {
      diagnose(ctor, diag::required_initializer_in_extension, parentTy)
        .highlight(attr->getLocation());
      attr->setInvalid();
      return;
    }
  } else {
    if (!parentTy->hasError()) {
      diagnose(ctor, diag::required_initializer_nonclass, parentTy)
        .highlight(attr->getLocation());
    }
    attr->setInvalid();
    return;
  }
}

static bool hasThrowingFunctionParameter(CanType type) {
  // Only consider throwing function types.
  if (auto fnType = dyn_cast<AnyFunctionType>(type)) {
    return fnType->getExtInfo().throws();
  }

  // Look through tuples.
  if (auto tuple = dyn_cast<TupleType>(type)) {
    for (auto eltType : tuple.getElementTypes()) {
      if (hasThrowingFunctionParameter(eltType))
        return true;
    }
    return false;
  }

  // Suppress diagnostics in the presence of errors.
  if (type->hasError()) {
    return true;
  }

  return false;
}

void AttributeChecker::visitRethrowsAttr(RethrowsAttr *attr) {
  // 'rethrows' only applies to functions that take throwing functions
  // as parameters.
  auto fn = cast<AbstractFunctionDecl>(D);
  for (auto param : *fn->getParameters()) {
    if (hasThrowingFunctionParameter(param->getType()
            ->lookThroughAllOptionalTypes()
            ->getCanonicalType()))
      return;
  }

  diagnose(attr->getLocation(), diag::rethrows_without_throwing_parameter);
  attr->setInvalid();
}

/// Collect all used generic parameter types from a given type.
static void collectUsedGenericParameters(
    Type Ty, SmallPtrSetImpl<TypeBase *> &ConstrainedGenericParams) {
  if (!Ty)
    return;

  if (!Ty->hasTypeParameter())
    return;

  // Add used generic parameters/archetypes.
  Ty.visit([&](Type Ty) {
    if (auto GP = dyn_cast<GenericTypeParamType>(Ty->getCanonicalType())) {
      ConstrainedGenericParams.insert(GP);
    }
  });
}

/// Perform some sanity checks for the requirements provided by
/// the @_specialize attribute.
static void checkSpecializeAttrRequirements(
    SpecializeAttr *attr,
    AbstractFunctionDecl *FD,
    const SmallPtrSet<TypeBase *, 4> &constrainedGenericParams,
    ASTContext &ctx) {
  auto genericSig = FD->getGenericSignature();

  if (!attr->isFullSpecialization())
    return;

  if (constrainedGenericParams.size() == genericSig->getGenericParams().size())
    return;

  ctx.Diags.diagnose(
      attr->getLocation(), diag::specialize_attr_type_parameter_count_mismatch,
      genericSig->getGenericParams().size(), constrainedGenericParams.size(),
      constrainedGenericParams.size() < genericSig->getGenericParams().size());

  if (constrainedGenericParams.size() < genericSig->getGenericParams().size()) {
    // Figure out which archetypes are not constrained.
    for (auto gp : genericSig->getGenericParams()) {
      if (constrainedGenericParams.count(gp->getCanonicalType().getPointer()))
        continue;
      auto gpDecl = gp->getDecl();
      if (gpDecl) {
        ctx.Diags.diagnose(attr->getLocation(),
                           diag::specialize_attr_missing_constraint,
                           gpDecl->getFullName());
      }
    }
  }
}

/// Require that the given type either not involve type parameters or be
/// a type parameter.
static bool diagnoseIndirectGenericTypeParam(SourceLoc loc, Type type,
                                             TypeRepr *typeRepr) {
  if (type->hasTypeParameter() && !type->is<GenericTypeParamType>()) {
    type->getASTContext().Diags.diagnose(
        loc,
        diag::specialize_attr_only_generic_param_req)
      .highlight(typeRepr->getSourceRange());
    return true;
  }

  return false;
}

/// Type check that a set of requirements provided by @_specialize.
/// Store the set of requirements in the attribute.
void AttributeChecker::visitSpecializeAttr(SpecializeAttr *attr) {
  DeclContext *DC = D->getDeclContext();
  auto *FD = cast<AbstractFunctionDecl>(D);
  auto genericSig = FD->getGenericSignature();
  auto *trailingWhereClause = attr->getTrailingWhereClause();

  if (!trailingWhereClause) {
    // Report a missing "where" clause.
    diagnose(attr->getLocation(), diag::specialize_missing_where_clause);
    return;
  }

  if (trailingWhereClause->getRequirements().empty()) {
    // Report an empty "where" clause.
    diagnose(attr->getLocation(), diag::specialize_empty_where_clause);
    return;
  }

  if (!genericSig) {
    // Only generic functions are permitted to have trailing where clauses.
    diagnose(attr->getLocation(),
             diag::specialize_attr_nongeneric_trailing_where,
             FD->getFullName())
        .highlight(trailingWhereClause->getSourceRange());
    return;
  }

  // Form a new generic signature based on the old one.
  GenericSignatureBuilder Builder(D->getASTContext());

  // First, add the old generic signature.
  Builder.addGenericSignature(genericSig);

  // Set of generic parameters being constrained. It is used to
  // determine if a full specialization misses requirements for
  // some of the generic parameters.
  SmallPtrSet<TypeBase *, 4> constrainedGenericParams;

  // Go over the set of requirements, adding them to the builder.
  WhereClauseOwner(FD, attr).visitRequirements(TypeResolutionStage::Interface,
      [&](const Requirement &req, RequirementRepr *reqRepr) {
        // Collect all of the generic parameters used by these types.
        switch (req.getKind()) {
        case RequirementKind::Conformance:
        case RequirementKind::SameType:
        case RequirementKind::Superclass:
          collectUsedGenericParameters(req.getSecondType(),
                                       constrainedGenericParams);
          LLVM_FALLTHROUGH;

        case RequirementKind::Layout:
          collectUsedGenericParameters(req.getFirstType(),
                                       constrainedGenericParams);
          break;
        }

        // Check additional constraints.
        // FIXME: These likely aren't fundamental limitations.
        switch (req.getKind()) {
        case RequirementKind::SameType: {
          bool firstHasTypeParameter = req.getFirstType()->hasTypeParameter();
          bool secondHasTypeParameter = req.getSecondType()->hasTypeParameter();

          // Exactly one type can have a type parameter.
          if (firstHasTypeParameter == secondHasTypeParameter) {
            diagnose(attr->getLocation(),
                     firstHasTypeParameter
                       ? diag::specialize_attr_non_concrete_same_type_req
                       : diag::specialize_attr_only_one_concrete_same_type_req)
              .highlight(reqRepr->getSourceRange());
            return false;
          }

          // We either need a fully-concrete type or a generic type parameter.
          if (diagnoseIndirectGenericTypeParam(attr->getLocation(),
                                               req.getFirstType(),
                                               reqRepr->getFirstTypeRepr()) ||
              diagnoseIndirectGenericTypeParam(attr->getLocation(),
                                               req.getSecondType(),
                                               reqRepr->getSecondTypeRepr())) {
            return false;
          }
          break;
        }

        case RequirementKind::Superclass:
          diagnose(attr->getLocation(),
                   diag::specialize_attr_non_protocol_type_constraint_req)
            .highlight(reqRepr->getSourceRange());
          return false;

        case RequirementKind::Conformance:
          if (diagnoseIndirectGenericTypeParam(attr->getLocation(),
                                               req.getFirstType(),
                                               reqRepr->getSubjectRepr())) {
            return false;
          }

          if (!req.getSecondType()->is<ProtocolType>()) {
            diagnose(attr->getLocation(),
                     diag::specialize_attr_non_protocol_type_constraint_req)
              .highlight(reqRepr->getSourceRange());
            return false;
          }

          diagnose(attr->getLocation(),
                   diag::specialize_attr_unsupported_kind_of_req)
            .highlight(reqRepr->getSourceRange());

          return false;

        case RequirementKind::Layout:
          if (diagnoseIndirectGenericTypeParam(attr->getLocation(),
                                               req.getFirstType(),
                                               reqRepr->getSubjectRepr())) {
            return false;
          }
          break;
        }

        // Add the requirement to the generic signature builder.
        using FloatingRequirementSource =
          GenericSignatureBuilder::FloatingRequirementSource;
        Builder.addRequirement(req, reqRepr,
                               FloatingRequirementSource::forExplicit(reqRepr),
                               nullptr, DC->getParentModule());
        return false;
      });

  // Check the validity of provided requirements.
  checkSpecializeAttrRequirements(attr, FD, constrainedGenericParams, Ctx);

  // Check the result.
  auto specializedSig = std::move(Builder).computeGenericSignature(
      attr->getLocation(),
      /*allowConcreteGenericParams=*/true);
  attr->setSpecializedSignature(specializedSig);
}

void AttributeChecker::visitFixedLayoutAttr(FixedLayoutAttr *attr) {
  if (isa<StructDecl>(D)) {
    diagnose(attr->getLocation(), diag::fixed_layout_struct)
      .fixItReplace(attr->getRange(), "@frozen");
  }

  auto *VD = cast<ValueDecl>(D);

  if (VD->getFormalAccess() < AccessLevel::Public &&
      !VD->getAttrs().hasAttribute<UsableFromInlineAttr>()) {
    diagnoseAndRemoveAttr(attr, diag::fixed_layout_attr_on_internal_type,
                          VD->getFullName(), VD->getFormalAccess());
  }
}

void AttributeChecker::visitUsableFromInlineAttr(UsableFromInlineAttr *attr) {
  auto *VD = cast<ValueDecl>(D);

  // FIXME: Once protocols can contain nominal types, do we want to allow
  // these nominal types to have access control (and also @usableFromInline)?
  if (isa<ProtocolDecl>(VD->getDeclContext())) {
    diagnoseAndRemoveAttr(attr, diag::usable_from_inline_attr_in_protocol);
    return;
  }

  // @usableFromInline can only be applied to internal declarations.
  if (VD->getFormalAccess() != AccessLevel::Internal) {
    diagnoseAndRemoveAttr(attr,
                          diag::usable_from_inline_attr_with_explicit_access,
                          VD->getFullName(),
                          VD->getFormalAccess());
    return;
  }

  // On internal declarations, @inlinable implies @usableFromInline.
  if (VD->getAttrs().hasAttribute<InlinableAttr>()) {
    if (Ctx.isSwiftVersionAtLeast(4,2))
      diagnoseAndRemoveAttr(attr, diag::inlinable_implies_usable_from_inline);
    return;
  }
}

void AttributeChecker::visitInlinableAttr(InlinableAttr *attr) {
  // @inlinable cannot be applied to stored properties.
  //
  // If the type is fixed-layout, the accessors are inlinable anyway;
  // if the type is resilient, the accessors cannot be inlinable
  // because clients cannot directly access storage.
  if (auto *VD = dyn_cast<VarDecl>(D)) {
    if (VD->hasStorage() || VD->getAttrs().hasAttribute<LazyAttr>()) {
      diagnoseAndRemoveAttr(attr,
                            diag::attribute_invalid_on_stored_property,
                            attr);
      return;
    }
  }

  auto *VD = cast<ValueDecl>(D);

  // Calls to dynamically-dispatched declarations are never devirtualized,
  // so marking them as @inlinable does not make sense.
  if (VD->isDynamic()) {
    diagnoseAndRemoveAttr(attr, diag::inlinable_dynamic_not_supported);
    return;
  }

  // @inlinable can only be applied to public or internal declarations.
  auto access = VD->getFormalAccess();
  if (access < AccessLevel::Internal) {
    diagnoseAndRemoveAttr(attr, diag::inlinable_decl_not_public,
                          VD->getBaseName(),
                          access);
    return;
  }

  // @inlinable cannot be applied to deinitializers in resilient classes.
  if (auto *DD = dyn_cast<DestructorDecl>(D)) {
    if (auto *CD = dyn_cast<ClassDecl>(DD->getDeclContext())) {
      if (CD->isResilient()) {
        diagnoseAndRemoveAttr(attr, diag::inlinable_resilient_deinit);
        return;
      }
    }
  }
}

void AttributeChecker::visitOptimizeAttr(OptimizeAttr *attr) {
  if (auto *VD = dyn_cast<VarDecl>(D)) {
    if (VD->hasStorage()) {
      diagnoseAndRemoveAttr(attr,
                            diag::attribute_invalid_on_stored_property,
                            attr);
      return;
    }
  }
}

void AttributeChecker::visitDiscardableResultAttr(DiscardableResultAttr *attr) {
  if (auto *FD = dyn_cast<FuncDecl>(D)) {
    if (auto result = FD->getResultInterfaceType()) {
      auto resultIsVoid = result->isVoid();
      if (resultIsVoid || result->isUninhabited()) {
        diagnoseAndRemoveAttr(attr,
                              diag::discardable_result_on_void_never_function,
                              resultIsVoid);
      }
    }
  }
}

/// Lookup the replaced decl in the replacments scope.
static void lookupReplacedDecl(DeclNameRef replacedDeclName,
                               const DynamicReplacementAttr *attr,
                               const ValueDecl *replacement,
                               SmallVectorImpl<ValueDecl *> &results) {
  auto *declCtxt = replacement->getDeclContext();

  // Look at the accessors' storage's context.
  if (auto *accessor = dyn_cast<AccessorDecl>(replacement)) {
    auto *storage = accessor->getStorage();
    declCtxt = storage->getDeclContext();
  }

  auto *moduleScopeCtxt = declCtxt->getModuleScopeContext();
  if (isa<FileUnit>(declCtxt)) {
    auto &ctx = declCtxt->getASTContext();
    auto descriptor = UnqualifiedLookupDescriptor(
        replacedDeclName, moduleScopeCtxt, attr->getLocation());
    auto lookup = evaluateOrDefault(ctx.evaluator,
                                    UnqualifiedLookupRequest{descriptor}, {});
    for (auto entry : lookup) {
      results.push_back(entry.getValueDecl());
    }
    return;
  }

  assert(declCtxt->isTypeContext());
  auto typeCtx = dyn_cast<NominalTypeDecl>(declCtxt->getAsDecl());
  if (!typeCtx)
    typeCtx = cast<ExtensionDecl>(declCtxt->getAsDecl())->getExtendedNominal();

  if (typeCtx)
    moduleScopeCtxt->lookupQualified({typeCtx}, replacedDeclName,
                                     NL_QualifiedDefault, results);
}

/// Remove any argument labels from the interface type of the given value that
/// are extraneous from the type system's point of view, producing the
/// type to compare against for the purposes of dynamic replacement.
static Type getDynamicComparisonType(ValueDecl *value) {
  unsigned numArgumentLabels = 0;

  if (isa<AbstractFunctionDecl>(value)) {
    ++numArgumentLabels;

    if (value->getDeclContext()->isTypeContext())
      ++numArgumentLabels;
  } else if (isa<SubscriptDecl>(value)) {
    ++numArgumentLabels;
  }

  auto interfaceType = value->getInterfaceType();
  return interfaceType->removeArgumentLabels(numArgumentLabels);
}

static FuncDecl *findReplacedAccessor(DeclNameRef replacedVarName,
                                      AccessorDecl *replacement,
                                      DynamicReplacementAttr *attr,
                                      ASTContext &ctx) {

  // Retrieve the replaced abstract storage decl.
  SmallVector<ValueDecl *, 4> results;
  lookupReplacedDecl(replacedVarName, attr, replacement, results);

  // Filter out any accessors that won't work.
  if (!results.empty()) {
    auto replacementStorage = replacement->getStorage();
    Type replacementStorageType = getDynamicComparisonType(replacementStorage);
    results.erase(std::remove_if(results.begin(), results.end(),
        [&](ValueDecl *result) {
          // Protocol requirements are not replaceable.
          if (isa<ProtocolDecl>(result->getDeclContext()))
            return true;
          // Check for static/instance mismatch.
          if (result->isStatic() != replacementStorage->isStatic())
            return true;

          // Check for type mismatch.
          auto resultType = getDynamicComparisonType(result);
          if (!resultType->isEqual(replacementStorageType) &&
              !resultType->matches(
                  replacementStorageType,
                  TypeMatchFlags::AllowCompatibleOpaqueTypeArchetypes)) {
            return true;
          }

          return false;
        }),
        results.end());
  }

  auto &Diags = ctx.Diags;
  if (results.empty()) {
    Diags.diagnose(attr->getLocation(),
                   diag::dynamic_replacement_accessor_not_found,
                   replacedVarName);
    attr->setInvalid();
    return nullptr;
  }

  if (results.size() > 1) {
    Diags.diagnose(attr->getLocation(),
                   diag::dynamic_replacement_accessor_ambiguous,
                   replacedVarName);
    for (auto result : results) {
      Diags.diagnose(result,
                     diag::dynamic_replacement_accessor_ambiguous_candidate,
                     result->getModuleContext()->getFullName());
    }
    attr->setInvalid();
    return nullptr;
  }

  assert(!isa<FuncDecl>(results[0]));
  
  auto *origStorage = cast<AbstractStorageDecl>(results[0]);
  if (!origStorage->isDynamic()) {
    Diags.diagnose(attr->getLocation(),
                   diag::dynamic_replacement_accessor_not_dynamic,
                   origStorage->getFullName());
    attr->setInvalid();
    return nullptr;
  }

  // Find the accessor in the replaced storage decl.
  auto *origAccessor = origStorage->getOpaqueAccessor(
      replacement->getAccessorKind());
  if (!origAccessor)
    return nullptr;

  if (origAccessor->isImplicit() &&
      !(origStorage->getReadImpl() == ReadImplKind::Stored &&
        origStorage->getWriteImpl() == WriteImplKind::Stored)) {
    Diags.diagnose(attr->getLocation(),
                   diag::dynamic_replacement_accessor_not_explicit,
                   (unsigned)origAccessor->getAccessorKind(),
                   origStorage->getFullName());
    attr->setInvalid();
    return nullptr;
  }

  return origAccessor;
}

static AbstractFunctionDecl *
findReplacedFunction(DeclNameRef replacedFunctionName,
                     const AbstractFunctionDecl *replacement,
                     DynamicReplacementAttr *attr, DiagnosticEngine *Diags) {

  // Note: we might pass a constant attribute when typechecker is nullptr.
  // Any modification to attr must be guarded by a null check on TC.
  //
  SmallVector<ValueDecl *, 4> results;
  lookupReplacedDecl(replacedFunctionName, attr, replacement, results);

  for (auto *result : results) {
    // Protocol requirements are not replaceable.
    if (isa<ProtocolDecl>(result->getDeclContext()))
      continue;
    // Check for static/instance mismatch.
    if (result->isStatic() != replacement->isStatic())
      continue;

    auto resultTy = result->getInterfaceType();
    auto replaceTy = replacement->getInterfaceType();
    TypeMatchOptions matchMode = TypeMatchFlags::AllowABICompatible;
    matchMode |= TypeMatchFlags::AllowCompatibleOpaqueTypeArchetypes;
    if (resultTy->matches(replaceTy, matchMode)) {
      if (!result->isDynamic()) {
        if (Diags) {
          Diags->diagnose(attr->getLocation(),
                          diag::dynamic_replacement_function_not_dynamic,
                          result->getFullName());
          attr->setInvalid();
        }
        return nullptr;
      }
      return cast<AbstractFunctionDecl>(result);
    }
  }

  if (!Diags)
    return nullptr;

  if (results.empty()) {
    Diags->diagnose(attr->getLocation(),
                    diag::dynamic_replacement_function_not_found,
                    replacedFunctionName);
  } else {
    Diags->diagnose(attr->getLocation(),
                    diag::dynamic_replacement_function_of_type_not_found,
                    replacedFunctionName,
                    replacement->getInterfaceType()->getCanonicalType());

    for (auto *result : results) {
      Diags->diagnose(SourceLoc(),
                      diag::dynamic_replacement_found_function_of_type,
                      result->getFullName(),
                      result->getInterfaceType()->getCanonicalType());
    }
  }
  attr->setInvalid();
  return nullptr;
}

static AbstractStorageDecl *
findReplacedStorageDecl(DeclNameRef replacedFunctionName,
                        const AbstractStorageDecl *replacement,
                        const DynamicReplacementAttr *attr) {

  SmallVector<ValueDecl *, 4> results;
  lookupReplacedDecl(replacedFunctionName, attr, replacement, results);

  for (auto *result : results) {
    // Check for static/instance mismatch.
    if (result->isStatic() != replacement->isStatic())
      continue;
    auto resultTy = result->getInterfaceType();
    auto replaceTy = replacement->getInterfaceType();
    TypeMatchOptions matchMode = TypeMatchFlags::AllowABICompatible;
    matchMode |= TypeMatchFlags::AllowCompatibleOpaqueTypeArchetypes;
    if (resultTy->matches(replaceTy, matchMode)) {
      if (!result->isDynamic()) {
        return nullptr;
      }
      return cast<AbstractStorageDecl>(result);
    }
  }
  return nullptr;
}

void AttributeChecker::visitDynamicReplacementAttr(DynamicReplacementAttr *attr) {
  assert(isa<AbstractFunctionDecl>(D) || isa<AbstractStorageDecl>(D));
  auto *replacement = cast<ValueDecl>(D);

  if (!isa<ExtensionDecl>(replacement->getDeclContext()) &&
      !replacement->getDeclContext()->isModuleScopeContext()) {
    diagnose(attr->getLocation(), diag::dynamic_replacement_not_in_extension,
             replacement->getBaseName());
    attr->setInvalid();
    return;
  }

  if (replacement->isNativeDynamic()) {
    diagnose(attr->getLocation(), diag::dynamic_replacement_must_not_be_dynamic,
             replacement->getBaseName());
    attr->setInvalid();
    return;
  }

  auto *original = replacement->getDynamicallyReplacedDecl();
  if (!original) {
    attr->setInvalid();
    return;
  }

  if (original->isObjC() && !replacement->isObjC()) {
    diagnose(attr->getLocation(),
             diag::dynamic_replacement_replacement_not_objc_dynamic,
             replacement->getFullName());
    attr->setInvalid();
  }
  if (!original->isObjC() && replacement->isObjC()) {
    diagnose(attr->getLocation(),
             diag::dynamic_replacement_replaced_not_objc_dynamic,
             original->getFullName());
    attr->setInvalid();
  }

  if (auto *CD = dyn_cast<ConstructorDecl>(replacement)) {
    auto *attr = CD->getAttrs().getAttribute<DynamicReplacementAttr>();
    auto replacedIsConvenienceInit =
        cast<ConstructorDecl>(original)->isConvenienceInit();
    if (replacedIsConvenienceInit &&!CD->isConvenienceInit()) {
      diagnose(attr->getLocation(),
               diag::dynamic_replacement_replaced_constructor_is_convenience,
               attr->getReplacedFunctionName());
    } else if (!replacedIsConvenienceInit && CD->isConvenienceInit()) {
      diagnose(
          attr->getLocation(),
          diag::dynamic_replacement_replaced_constructor_is_not_convenience,
          attr->getReplacedFunctionName());
    }
  }
}

llvm::Expected<bool>
TypeEraserHasViableInitRequest::evaluate(Evaluator &evaluator,
                                         TypeEraserAttr *attr,
                                         ProtocolDecl *protocol) const {
  auto &ctx = protocol->getASTContext();
  auto &diags = ctx.Diags;
  TypeLoc &typeEraserLoc = attr->getTypeEraserLoc();
  TypeRepr *typeEraserRepr = typeEraserLoc.getTypeRepr();
  DeclContext *dc = protocol->getDeclContext();
  Type protocolType = protocol->getDeclaredType();

  // Get the NominalTypeDecl for the type eraser.
  Type typeEraser = typeEraserLoc.getType();
  if (!typeEraser && typeEraserRepr) {
    auto resolution = TypeResolution::forContextual(protocol);
    typeEraser = resolution.resolveType(typeEraserRepr, /*options=*/None);
    typeEraserLoc.setType(typeEraser);
  }

  if (typeEraser->hasError())
    return false;

  // The type eraser must be a concrete nominal type
  auto nominalTypeDecl = typeEraser->getAnyNominal();
  if (auto typeAliasDecl = dyn_cast_or_null<TypeAliasDecl>(nominalTypeDecl))
    nominalTypeDecl = typeAliasDecl->getUnderlyingType()->getAnyNominal();

  if (!nominalTypeDecl || isa<ProtocolDecl>(nominalTypeDecl)) {
    diags.diagnose(typeEraserLoc.getLoc(), diag::non_nominal_type_eraser);
    return false;
  }

  // The nominal type must be accessible wherever the protocol is accessible
  if (nominalTypeDecl->getFormalAccess() < protocol->getFormalAccess()) {
    diags.diagnose(typeEraserLoc.getLoc(), diag::type_eraser_not_accessible,
                   nominalTypeDecl->getFormalAccess(), nominalTypeDecl->getName(),
                   protocolType, protocol->getFormalAccess());
    diags.diagnose(nominalTypeDecl->getLoc(), diag::type_eraser_declared_here);
    return false;
  }

  // The type eraser must conform to the annotated protocol
  if (!TypeChecker::conformsToProtocol(typeEraser, protocol, dc, None)) {
    diags.diagnose(typeEraserLoc.getLoc(), diag::type_eraser_does_not_conform,
                   typeEraser, protocolType);
    diags.diagnose(nominalTypeDecl->getLoc(), diag::type_eraser_declared_here);
    return false;
  }

  // The type eraser must have an init of the form init<T: Protocol>(erasing: T)
  auto lookupResult = TypeChecker::lookupMember(dc, typeEraser,
                                                DeclNameRef::createConstructor());

  // Keep track of unviable init candidates for diagnostics
  enum class UnviableReason {
    Failable,
    UnsatisfiedRequirements,
    Inaccessible,
  };
  SmallVector<std::tuple<ConstructorDecl *, UnviableReason, Type>, 2> unviable;

  bool foundMatch = llvm::any_of(lookupResult, [&](const LookupResultEntry &entry) {
    auto *init = cast<ConstructorDecl>(entry.getValueDecl());
    if (!init->isGeneric() || init->getGenericParams()->size() != 1)
      return false;

    auto genericSignature = init->getGenericSignature();
    auto genericParamType = genericSignature->getInnermostGenericParams().front();

    // Fow now, only allow one parameter.
    auto params = init->getParameters();
    if (params->size() != 1)
      return false;

    // The parameter must have the form `erasing: T` where T conforms to the protocol.
    ParamDecl *param = *init->getParameters()->begin();
    if (param->getArgumentName() != ctx.Id_erasing ||
        !param->getInterfaceType()->isEqual(genericParamType) ||
        !genericSignature->conformsToProtocol(genericParamType, protocol))
      return false;

    // Allow other constraints as long as the init can be called with any
    // type conforming to the annotated protocol. We will check this by
    // substituting the protocol's Self type for the generic arg and check that
    // the requirements in the generic signature are satisfied.
    auto baseMap =
        typeEraser->getContextSubstitutionMap(nominalTypeDecl->getParentModule(),
                                              nominalTypeDecl);
    QuerySubstitutionMap getSubstitution{baseMap};
    auto subMap = SubstitutionMap::get(
        genericSignature,
        [&](SubstitutableType *type) -> Type {
          if (type->isEqual(genericParamType))
            return protocol->getSelfTypeInContext();

          return getSubstitution(type);
        },
        TypeChecker::LookUpConformance(dc));

    // Use invalid 'SourceLoc's to suppress diagnostics.
    auto result = TypeChecker::checkGenericArguments(
          protocol, SourceLoc(), SourceLoc(), typeEraser,
          genericSignature->getGenericParams(),
          genericSignature->getRequirements(),
          QuerySubstitutionMap{subMap},
          TypeChecker::LookUpConformance(dc),
          None);

    if (result != RequirementCheckResult::Success) {
      unviable.push_back(
          std::make_tuple(init, UnviableReason::UnsatisfiedRequirements,
                          genericParamType));
      return false;
    }

    if (init->isFailable()) {
      unviable.push_back(
          std::make_tuple(init, UnviableReason::Failable, genericParamType));
      return false;
    }

    if (init->getFormalAccess() < protocol->getFormalAccess()) {
      unviable.push_back(
          std::make_tuple(init, UnviableReason::Inaccessible, genericParamType));
      return false;
    }

    return true;
  });

  if (!foundMatch) {
    if (unviable.empty()) {
      diags.diagnose(attr->getLocation(), diag::type_eraser_missing_init,
                     typeEraser, protocol->getName().str());
      diags.diagnose(nominalTypeDecl->getLoc(), diag::type_eraser_declared_here);
      return false;
    }

    diags.diagnose(attr->getLocation(), diag::type_eraser_unviable_init,
                   typeEraser, protocol->getName().str());
    for (auto &candidate: unviable) {
      auto init = std::get<0>(candidate);
      auto reason = std::get<1>(candidate);
      auto genericParamType = std::get<2>(candidate);

      switch (reason) {
      case UnviableReason::Failable:
        diags.diagnose(init->getLoc(), diag::type_eraser_failable_init);
        break;
      case UnviableReason::UnsatisfiedRequirements:
        diags.diagnose(init->getLoc(),
                       diag::type_eraser_init_unsatisfied_requirements,
                       genericParamType, protocol->getName().str());
        break;
      case UnviableReason::Inaccessible:
        diags.diagnose(init->getLoc(), diag::type_eraser_init_not_accessible,
                       init->getFormalAccess(), protocolType,
                       protocol->getFormalAccess());
        break;
      }
    }
    return false;
  }

  return true;
}

void AttributeChecker::visitTypeEraserAttr(TypeEraserAttr *attr) {
  assert(isa<ProtocolDecl>(D));
  // Invoke the request.
  (void)attr->hasViableTypeEraserInit(cast<ProtocolDecl>(D));
}

void AttributeChecker::visitImplementsAttr(ImplementsAttr *attr) {
  TypeLoc &ProtoTypeLoc = attr->getProtocolType();

  DeclContext *DC = D->getDeclContext();

  Type T = ProtoTypeLoc.getType();
  if (!T && ProtoTypeLoc.getTypeRepr()) {
    TypeResolutionOptions options = None;
    options |= TypeResolutionFlags::AllowUnboundGenerics;

    auto resolution = TypeResolution::forContextual(DC);
    T = resolution.resolveType(ProtoTypeLoc.getTypeRepr(), options);
    ProtoTypeLoc.setType(T);
  }

  // Definite error-types were already diagnosed in resolveType.
  if (T->hasError())
    return;

  // Check that we got a ProtocolType.
  if (auto PT = T->getAs<ProtocolType>()) {
    ProtocolDecl *PD = PT->getDecl();

    // Check that the ProtocolType has the specified member.
    LookupResult R =
        TypeChecker::lookupMember(PD->getDeclContext(), PT,
                                  DeclNameRef(attr->getMemberName()));
    if (!R) {
      diagnose(attr->getLocation(),
               diag::implements_attr_protocol_lacks_member,
               PD->getBaseName(), attr->getMemberName())
        .highlight(attr->getMemberNameLoc().getSourceRange());
    }

    // Check that the decl we're decorating is a member of a type that actually
    // conforms to the specified protocol.
    NominalTypeDecl *NTD = DC->getSelfNominalTypeDecl();
    SmallVector<ProtocolConformance *, 2> conformances;
    if (!NTD->lookupConformance(DC->getParentModule(), PD, conformances)) {
      diagnose(attr->getLocation(),
               diag::implements_attr_protocol_not_conformed_to,
               NTD->getFullName(), PD->getFullName())
        .highlight(ProtoTypeLoc.getTypeRepr()->getSourceRange());
    }

  } else {
    diagnose(attr->getLocation(), diag::implements_attr_non_protocol_type)
      .highlight(ProtoTypeLoc.getTypeRepr()->getSourceRange());
  }
}

void AttributeChecker::visitFrozenAttr(FrozenAttr *attr) {
  if (auto *ED = dyn_cast<EnumDecl>(D)) {
    if (!ED->getModuleContext()->isResilient()) {
      attr->setInvalid();
      return;
    }

    if (ED->getFormalAccess() < AccessLevel::Public &&
        !ED->getAttrs().hasAttribute<UsableFromInlineAttr>()) {
      diagnoseAndRemoveAttr(attr, diag::enum_frozen_nonpublic, attr);
      return;
    }
  }

  auto *VD = cast<ValueDecl>(D);

  if (VD->getFormalAccess() < AccessLevel::Public &&
      !VD->getAttrs().hasAttribute<UsableFromInlineAttr>()) {
    diagnoseAndRemoveAttr(attr, diag::frozen_attr_on_internal_type,
                          VD->getFullName(), VD->getFormalAccess());
  }
}

void AttributeChecker::visitCustomAttr(CustomAttr *attr) {
  auto dc = D->getInnermostDeclContext();

  // Figure out which nominal declaration this custom attribute refers to.
  auto nominal = evaluateOrDefault(
    Ctx.evaluator, CustomAttrNominalRequest{attr, dc}, nullptr);

  // If there is no nominal type with this name, complain about this being
  // an unknown attribute.
  if (!nominal) {
    std::string typeName;
    if (auto typeRepr = attr->getTypeLoc().getTypeRepr()) {
      llvm::raw_string_ostream out(typeName);
      typeRepr->print(out);
    } else {
      typeName = attr->getTypeLoc().getType().getString();
    }

    diagnose(attr->getLocation(), diag::unknown_attribute, typeName);
    attr->setInvalid();
    return;
  }

  // If the nominal type is a property wrapper type, we can be delegating
  // through a property.
  if (nominal->getPropertyWrapperTypeInfo()) {
    // property wrappers can only be applied to variables
    if (!isa<VarDecl>(D) || isa<ParamDecl>(D)) {
      diagnose(attr->getLocation(),
               diag::property_wrapper_attribute_not_on_property,
               nominal->getFullName());
      attr->setInvalid();
      return;
    }

    return;
  }

  // If the nominal type is a function builder type, verify that D is a
  // function, storage with an explicit getter, or parameter of function type.
  if (nominal->getAttrs().hasAttribute<FunctionBuilderAttr>()) {
    ValueDecl *decl;
    if (auto param = dyn_cast<ParamDecl>(D)) {
      decl = param;
    } else if (auto func = dyn_cast<FuncDecl>(D)) {
      decl = func;
    } else if (auto storage = dyn_cast<AbstractStorageDecl>(D)) {
      decl = storage;

      // Check whether this is a property without an explicit getter.
      auto shouldDiagnose = [&]() -> bool {
        auto getter = storage->getParsedAccessor(AccessorKind::Get);
        if (!getter)
          return true;

        // Module interfaces don't print bodies for all getters, so allow getters
        // that don't have a body if we're compiling a module interface.
        SourceFile *parent = storage->getDeclContext()->getParentSourceFile();
        bool isInInterface = parent && parent->Kind == SourceFileKind::Interface;
        if (!isInInterface && !getter->hasBody())
          return true;

        return false;
      };

      if (shouldDiagnose()) {
        diagnose(attr->getLocation(),
                 diag::function_builder_attribute_on_storage_without_getter,
                 nominal->getFullName(),
                 isa<SubscriptDecl>(storage) ? 0
                   : storage->getDeclContext()->isTypeContext() ? 1
                   : cast<VarDecl>(storage)->isLet() ? 2 : 3);
        attr->setInvalid();
        return;
      }
    } else {
      diagnose(attr->getLocation(),
               diag::function_builder_attribute_not_allowed_here,
               nominal->getFullName());
      attr->setInvalid();
      return;
    }

    // Diagnose and ignore arguments.
    if (attr->getArg()) {
      diagnose(attr->getLocation(), diag::function_builder_arguments)
        .highlight(attr->getArg()->getSourceRange());
    }

    // Complain if this isn't the primary function-builder attribute.
    auto attached = decl->getAttachedFunctionBuilder();
    if (attached != attr) {
      diagnose(attr->getLocation(), diag::function_builder_multiple,
               isa<ParamDecl>(decl));
      diagnose(attached->getLocation(), diag::previous_function_builder_here);
      attr->setInvalid();
      return;
    } else {
      // Force any diagnostics associated with computing the function-builder
      // type.
      (void) decl->getFunctionBuilderType();
    }

    return;
  }

  diagnose(attr->getLocation(), diag::nominal_type_not_attribute,
           nominal->getDescriptiveKind(), nominal->getFullName());
  nominal->diagnose(diag::decl_declared_here, nominal->getFullName());
  attr->setInvalid();
}


void AttributeChecker::visitPropertyWrapperAttr(PropertyWrapperAttr *attr) {
  auto nominal = dyn_cast<NominalTypeDecl>(D);
  if (!nominal)
    return;

  // Force checking of the property wrapper type.
  (void)nominal->getPropertyWrapperTypeInfo();
}

void AttributeChecker::visitFunctionBuilderAttr(FunctionBuilderAttr *attr) {
  // TODO: check that the type at least provides a `sequence` factory?
  // Any other validation?
}

void
AttributeChecker::visitImplementationOnlyAttr(ImplementationOnlyAttr *attr) {
  if (isa<ImportDecl>(D)) {
    // These are handled elsewhere.
    return;
  }

  auto *VD = cast<ValueDecl>(D);
  auto *overridden = VD->getOverriddenDecl();
  if (!overridden) {
    diagnoseAndRemoveAttr(attr, diag::implementation_only_decl_non_override);
    return;
  }

  // Check if VD has the exact same type as what it overrides.
  // Note: This is specifically not using `swift::getMemberTypeForComparison`
  // because that erases more information than we want, like `throws`-ness.
  auto baseInterfaceTy = overridden->getInterfaceType();
  auto derivedInterfaceTy = VD->getInterfaceType();

  auto selfInterfaceTy = VD->getDeclContext()->getDeclaredInterfaceType();

  auto overrideInterfaceTy =
      selfInterfaceTy->adjustSuperclassMemberDeclType(overridden, VD,
                                                      baseInterfaceTy);

  if (isa<AbstractFunctionDecl>(VD)) {
    // Drop the 'Self' parameter.
    // FIXME: The real effect here, though, is dropping the generic signature.
    // This should be okay because it should already be checked as part of
    // making an override, but that isn't actually the case as of this writing,
    // and it's kind of suspect anyway.
    derivedInterfaceTy =
        derivedInterfaceTy->castTo<AnyFunctionType>()->getResult();
    overrideInterfaceTy =
        overrideInterfaceTy->castTo<AnyFunctionType>()->getResult();
  } else if (isa<SubscriptDecl>(VD)) {
    // For subscripts, we don't have a 'Self' type, but turn it
    // into a monomorphic function type.
    // FIXME: does this actually make sense, though?
    auto derivedInterfaceFuncTy = derivedInterfaceTy->castTo<AnyFunctionType>();
    derivedInterfaceTy =
        FunctionType::get(derivedInterfaceFuncTy->getParams(),
                          derivedInterfaceFuncTy->getResult());
    auto overrideInterfaceFuncTy =
        overrideInterfaceTy->castTo<AnyFunctionType>();
    overrideInterfaceTy =
        FunctionType::get(overrideInterfaceFuncTy->getParams(),
                          overrideInterfaceFuncTy->getResult());
  }

  if (!derivedInterfaceTy->isEqual(overrideInterfaceTy)) {
    diagnose(VD, diag::implementation_only_override_changed_type,
             overrideInterfaceTy);
    diagnose(overridden, diag::overridden_here);
    return;
  }

  // FIXME: When compiling without library evolution enabled, this should also
  // check whether VD or any of its accessors need a new vtable entry, even if
  // it won't necessarily be able to say why.
}

void AttributeChecker::visitNonEphemeralAttr(NonEphemeralAttr *attr) {
  auto *param = cast<ParamDecl>(D);
  auto type = param->getInterfaceType()->lookThroughSingleOptionalType();

  // Can only be applied to Unsafe[...]Pointer types
  if (type->getAnyPointerElementType())
    return;

  // ... or the protocol Self type.
  auto *outerDC = param->getDeclContext()->getParent();
  if (outerDC->getSelfProtocolDecl() &&
      type->isEqual(outerDC->getProtocolSelfType())) {
    return;
  }

  diagnose(attr->getLocation(), diag::non_ephemeral_non_pointer_type);
  attr->setInvalid();
}

void TypeChecker::checkParameterAttributes(ParameterList *params) {
  for (auto param: *params) {
    checkDeclAttributes(param);
  }
}

void AttributeChecker::checkOriginalDefinedInAttrs(Decl *D,
    ArrayRef<OriginallyDefinedInAttr*> Attrs) {
  if (Attrs.empty())
    return;
  auto &Ctx = D->getASTContext();
  std::map<PlatformKind, SourceLoc> seenPlatforms;

  // Attrs are in the reverse order of the source order. We need to visit them
  // in source order to diagnose the later attribute.
  for (auto *Attr: Attrs) {
    if (!Attr->isActivePlatform(Ctx))
      continue;
    auto AtLoc = Attr->AtLoc;
    auto Platform = Attr->Platform;
    if (!seenPlatforms.insert({Platform, AtLoc}).second) {
      // We've seen the platform before, emit error to the previous one which
      // comes later in the source order.
      diagnose(seenPlatforms[Platform],
               diag::originally_defined_in_dupe_platform,
               platformString(Platform));
      return;
    }
    static StringRef AttrName = "_originallyDefinedIn";
    if (!D->getDeclContext()->isModuleScopeContext()) {
      diagnose(AtLoc, diag::originally_definedin_topleve_decl, AttrName);
      return;
    }
    auto IntroVer = D->getIntroducedOSVersion(Platform);
    if (!IntroVer.hasValue()) {
      diagnose(AtLoc, diag::originally_definedin_need_available,
               AttrName);
      return;
    }
    if (IntroVer.getValue() >= Attr->MovedVersion) {
      diagnose(AtLoc,
               diag::originally_definedin_must_after_available_version,
               AttrName);
      return;
    }
  }
}

Type TypeChecker::checkReferenceOwnershipAttr(VarDecl *var, Type type,
                                              ReferenceOwnershipAttr *attr) {
  auto &Diags = var->getASTContext().Diags;
  auto *dc = var->getDeclContext();

  // Don't check ownership attribute if the type is invalid.
  if (attr->isInvalid() || type->is<ErrorType>())
    return type;

  auto ownershipKind = attr->get();

  // A weak variable must have type R? or R! for some ownership-capable type R.
  auto underlyingType = type->getOptionalObjectType();
  auto isOptional = bool(underlyingType);

  switch (optionalityOf(ownershipKind)) {
  case ReferenceOwnershipOptionality::Disallowed:
    if (isOptional) {
      var->diagnose(diag::invalid_ownership_with_optional, ownershipKind)
          .fixItReplace(attr->getRange(), "weak");
      attr->setInvalid();
    }
    break;
  case ReferenceOwnershipOptionality::Allowed:
    break;
  case ReferenceOwnershipOptionality::Required:
    if (var->isLet()) {
      var->diagnose(diag::invalid_ownership_is_let, ownershipKind);
      attr->setInvalid();
    }

    if (!isOptional) {
      attr->setInvalid();

      // @IBOutlet has its own diagnostic when the property type is
      // non-optional.
      if (var->getAttrs().hasAttribute<IBOutletAttr>())
        break;

      auto diag = var->diagnose(diag::invalid_ownership_not_optional,
                                ownershipKind, OptionalType::get(type));
      auto typeRange = var->getTypeSourceRangeForDiagnostics();
      if (type->hasSimpleTypeRepr()) {
        diag.fixItInsertAfter(typeRange.End, "?");
      } else {
        diag.fixItInsert(typeRange.Start, "(")
          .fixItInsertAfter(typeRange.End, ")?");
      }
    }
    break;
  }

  if (!underlyingType)
    underlyingType = type;

  auto sig = var->getDeclContext()->getGenericSignatureOfContext();
  if (!underlyingType->allowsOwnership(sig.getPointer())) {
    auto D = diag::invalid_ownership_type;

    if (underlyingType->isExistentialType() ||
        underlyingType->isTypeParameter()) {
      // Suggest the possibility of adding a class bound.
      D = diag::invalid_ownership_protocol_type;
    }

    var->diagnose(D, ownershipKind, underlyingType);
    attr->setInvalid();
  }

  ClassDecl *underlyingClass = underlyingType->getClassOrBoundGenericClass();
  if (underlyingClass && underlyingClass->isIncompatibleWithWeakReferences()) {
    Diags
        .diagnose(attr->getLocation(),
                  diag::invalid_ownership_incompatible_class, underlyingType,
                  ownershipKind)
        .fixItRemove(attr->getRange());
    attr->setInvalid();
  }

  auto PDC = dyn_cast<ProtocolDecl>(dc);
  if (PDC && !PDC->isObjC()) {
    // Ownership does not make sense in protocols, except for "weak" on
    // properties of Objective-C protocols.
    auto D = var->getASTContext().isSwiftVersionAtLeast(5)
                 ? diag::ownership_invalid_in_protocols
                 : diag::ownership_invalid_in_protocols_compat_warning;
    Diags.diagnose(attr->getLocation(), D, ownershipKind)
        .fixItRemove(attr->getRange());
    attr->setInvalid();
  }

  if (attr->isInvalid())
    return type;

  // Change the type to the appropriate reference storage type.
  return ReferenceStorageType::get(type, ownershipKind, var->getASTContext());
}

Optional<Diag<>>
TypeChecker::diagnosticIfDeclCannotBePotentiallyUnavailable(const Decl *D) {
  DeclContext *DC = D->getDeclContext();
  // Do not permit potential availability of script-mode global variables;
  // their initializer expression is not lazily evaluated, so this would
  // not be safe.
  if (isa<VarDecl>(D) && DC->isModuleScopeContext() &&
      DC->getParentSourceFile()->isScriptMode()) {
    return diag::availability_global_script_no_potential;
  }

  // For now, we don't allow stored properties to be potentially unavailable.
  // We will want to support these eventually, but we haven't figured out how
  // this will interact with Definite Initialization, deinitializers and
  // resilience yet.
  if (auto *VD = dyn_cast<VarDecl>(D)) {
    // Globals and statics are lazily initialized, so they are safe
    // for potential unavailability. Note that if D is a global in script
    // mode (which are not lazy) then we will already have returned
    // a diagnosis above.
    bool lazilyInitializedStored = VD->isStatic() ||
                                   VD->getAttrs().hasAttribute<LazyAttr>() ||
                                   DC->isModuleScopeContext();

    if (VD->hasStorage() && !lazilyInitializedStored) {
      return diag::availability_stored_property_no_potential;
    }
  }

  return None;
}

static bool shouldBlockImplicitDynamic(Decl *D) {
  if (D->getAttrs().hasAttribute<NonObjCAttr>() ||
      D->getAttrs().hasAttribute<SILGenNameAttr>() ||
      D->getAttrs().hasAttribute<TransparentAttr>() ||
      D->getAttrs().hasAttribute<InlinableAttr>())
    return true;
  return false;
}
void TypeChecker::addImplicitDynamicAttribute(Decl *D) {
  if (!D->getModuleContext()->isImplicitDynamicEnabled())
    return;

  // Add the attribute if the decl kind allows it and it is not an accessor
  // decl. Accessor decls should always infer the var/subscript's attribute.
  if (!DeclAttribute::canAttributeAppearOnDecl(DAK_Dynamic, D) ||
      isa<AccessorDecl>(D))
    return;

  // Don't add dynamic if decl is inlinable or tranparent.
  if (shouldBlockImplicitDynamic(D))
   return;

  if (auto *FD = dyn_cast<FuncDecl>(D)) {
    // Don't add dynamic to defer bodies.
    if (FD->isDeferBody())
      return;
    // Don't add dynamic to functions with a cdecl.
    if (FD->getAttrs().hasAttribute<CDeclAttr>())
      return;
    // Don't add dynamic to local function definitions.
    if (!FD->getDeclContext()->isTypeContext() &&
        FD->getDeclContext()->isLocalContext())
      return;
  }

  // Don't add dynamic if accessor is inlinable or transparent.
  if (auto *asd = dyn_cast<AbstractStorageDecl>(D)) {
    bool blocked = false;
    asd->visitParsedAccessors([&](AccessorDecl *accessor) {
      blocked |= shouldBlockImplicitDynamic(accessor);
    });
    if (blocked)
      return;
  }

  if (auto *VD = dyn_cast<VarDecl>(D)) {
    // Don't turn stored into computed properties. This could conflict with
    // exclusivity checking.
    // If there is a didSet or willSet function we allow dynamic replacement.
    if (VD->hasStorage() &&
        !VD->getParsedAccessor(AccessorKind::DidSet) &&
        !VD->getParsedAccessor(AccessorKind::WillSet))
      return;
    // Don't add dynamic to local variables.
    if (VD->getDeclContext()->isLocalContext())
      return;
    // Don't add to implicit variables.
    if (VD->isImplicit())
      return;
  }

  if (!D->getAttrs().hasAttribute<DynamicAttr>() &&
      !D->getAttrs().hasAttribute<DynamicReplacementAttr>()) {
    auto attr = new (D->getASTContext()) DynamicAttr(/*implicit=*/true);
    D->getAttrs().add(attr);
  }
}

llvm::Expected<ValueDecl *>
DynamicallyReplacedDeclRequest::evaluate(Evaluator &evaluator,
                                         ValueDecl *VD) const {
  // Dynamic replacements must be explicit.
  if (VD->isImplicit())
    return nullptr;

  auto *attr = VD->getAttrs().getAttribute<DynamicReplacementAttr>();
  if (!attr) {
    // It's likely that the accessor isn't annotated but its storage is.
    if (auto *AD = dyn_cast<AccessorDecl>(VD)) {
      // Try to grab the attribute from the storage.
      attr = AD->getStorage()->getAttrs().getAttribute<DynamicReplacementAttr>();
    }

    if (!attr) {
      // Otherwise, it's not dynamically replacing anything.
      return nullptr;
    }
  }

  // If the attribute is invalid, bail.
  if (attr->isInvalid())
    return nullptr;

  // If we can lazily resolve the function, do so now.
  if (auto *LazyResolver = attr->Resolver) {
    auto decl = LazyResolver->loadDynamicallyReplacedFunctionDecl(
        attr, attr->ResolverContextData);
    attr->Resolver = nullptr;
    return decl;
  }

  auto &Ctx = VD->getASTContext();
  if (auto *AD = dyn_cast<AccessorDecl>(VD)) {
    return findReplacedAccessor(attr->getReplacedFunctionName(), AD, attr, Ctx);
  }

  if (auto *AFD = dyn_cast<AbstractFunctionDecl>(VD)) {
    return findReplacedFunction(attr->getReplacedFunctionName(), AFD,
                                attr, &Ctx.Diags);
  }

  if (auto *SD = dyn_cast<AbstractStorageDecl>(VD)) {
    return findReplacedStorageDecl(attr->getReplacedFunctionName(), SD, attr);
  }

  return nullptr;
}

/// Returns true if the given type conforms to `Differentiable` in the given
/// module.
static bool conformsToDifferentiable(Type type, DeclContext *DC) {
  auto &ctx = type->getASTContext();
  auto *differentiableProto =
      ctx.getProtocol(KnownProtocolKind::Differentiable);
  auto conf = TypeChecker::conformsToProtocol(
      type, differentiableProto, DC, ConformanceCheckFlags::InExpression);
  if (!conf)
    return false;
  // Try to get the `TangentVector` type witness, in case the conformance has
  // not been fully checked and the type witness cannot be resolved.
  Type tanType = conf.getTypeWitnessByName(type, ctx.Id_TangentVector);
  return !tanType.isNull() && !tanType->hasError();
};

IndexSubset *TypeChecker::inferDifferentiabilityParameters(
    AbstractFunctionDecl *AFD, GenericEnvironment *derivativeGenEnv) {
  auto &ctx = AFD->getASTContext();
  auto *functionType = AFD->getInterfaceType()->castTo<AnyFunctionType>();
  auto numUncurriedParams = functionType->getNumParams();
  if (auto *resultFnType =
          functionType->getResult()->getAs<AnyFunctionType>()) {
    numUncurriedParams += resultFnType->getNumParams();
  }
  llvm::SmallBitVector parameterBits(numUncurriedParams);
  SmallVector<Type, 4> allParamTypes;

  // Returns true if the i-th parameter type is differentiable.
  auto isDifferentiableParam = [&](unsigned i) -> bool {
    if (i >= allParamTypes.size())
      return false;
    auto paramType = allParamTypes[i];
    if (derivativeGenEnv)
      paramType = derivativeGenEnv->mapTypeIntoContext(paramType);
    else
      paramType = AFD->mapTypeIntoContext(paramType);
    // Return false for existential types.
    if (paramType->isExistentialType())
      return false;
    // Return true if the type conforms to `Differentiable`.
    return conformsToDifferentiable(paramType, AFD);
  };

  // Get all parameter types.
  // NOTE: To be robust, result function type parameters should be added only if
  // `functionType` comes from a static/instance method, and not a free function
  // returning a function type. In practice, this code path should not be
  // reachable for free functions returning a function type.
  if (auto resultFnType = functionType->getResult()->getAs<AnyFunctionType>())
    for (auto &param : resultFnType->getParams())
      allParamTypes.push_back(param.getPlainType());
  for (auto &param : functionType->getParams())
    allParamTypes.push_back(param.getPlainType());

  // Set differentiability parameters.
  for (unsigned i : range(parameterBits.size()))
    if (isDifferentiableParam(i))
      parameterBits.set(i);

  return IndexSubset::get(ctx, parameterBits);
}

// Computes the differentiability parameter indices from the given parsed
// differentiability parameters for the given original or derivative
// `AbstractFunctionDecl` and derivative generic environment. On error, emits
// diagnostics and returns `nullptr`.
// - If parsed parameters are empty, infer parameter indices.
// - Otherwise, build parameter indices from parsed parameters.
// The attribute name/location are used in diagnostics.
static IndexSubset *computeDifferentiabilityParameters(
    ArrayRef<ParsedAutoDiffParameter> parsedDiffParams,
    AbstractFunctionDecl *function, GenericEnvironment *derivativeGenEnv,
    StringRef attrName, SourceLoc attrLoc) {
  auto &ctx = function->getASTContext();
  auto &diags = ctx.Diags;

  // Get function type and parameters.
  auto *functionType = function->getInterfaceType()->castTo<AnyFunctionType>();
  auto &params = *function->getParameters();
  auto numParams = function->getParameters()->size();
  auto isInstanceMethod = function->isInstanceMember();

  // Diagnose if function has no parameters.
  if (params.size() == 0) {
    // If function is not an instance method, diagnose immediately.
    if (!isInstanceMethod) {
      diags
          .diagnose(attrLoc, diag::diff_function_no_parameters,
                    function->getFullName())
          .highlight(function->getSignatureSourceRange());
      return nullptr;
    }
    // If function is an instance method, diagnose only if `self` does not
    // conform to `Differentiable`.
    else {
      auto selfType = function->getImplicitSelfDecl()->getInterfaceType();
      if (derivativeGenEnv)
        selfType = derivativeGenEnv->mapTypeIntoContext(selfType);
      else
        selfType = function->mapTypeIntoContext(selfType);
      if (!conformsToDifferentiable(selfType, function)) {
        diags
            .diagnose(attrLoc, diag::diff_function_no_parameters,
                      function->getFullName())
            .highlight(function->getSignatureSourceRange());
        return nullptr;
      }
    }
  }

  // If parsed differentiability parameters are empty, infer parameter indices
  // from the function type.
  if (parsedDiffParams.empty())
    return TypeChecker::inferDifferentiabilityParameters(function,
                                                         derivativeGenEnv);

  // Otherwise, build parameter indices from parsed differentiability
  // parameters.
  auto numUncurriedParams = functionType->getNumParams();
  if (auto *resultFnType =
          functionType->getResult()->getAs<AnyFunctionType>()) {
    numUncurriedParams += resultFnType->getNumParams();
  }
  llvm::SmallBitVector parameterBits(numUncurriedParams);
  int lastIndex = -1;
  for (unsigned i : indices(parsedDiffParams)) {
    auto paramLoc = parsedDiffParams[i].getLoc();
    switch (parsedDiffParams[i].getKind()) {
    case ParsedAutoDiffParameter::Kind::Named: {
      auto nameIter = llvm::find_if(params.getArray(), [&](ParamDecl *param) {
        return param->getName() == parsedDiffParams[i].getName();
      });
      // Parameter name must exist.
      if (nameIter == params.end()) {
        diags.diagnose(paramLoc, diag::diff_params_clause_param_name_unknown,
                       parsedDiffParams[i].getName());
        return nullptr;
      }
      // Parameter names must be specified in the original order.
      unsigned index = std::distance(params.begin(), nameIter);
      if ((int)index <= lastIndex) {
        diags.diagnose(paramLoc,
                       diag::diff_params_clause_params_not_original_order);
        return nullptr;
      }
      parameterBits.set(index);
      lastIndex = index;
      break;
    }
    case ParsedAutoDiffParameter::Kind::Self: {
      // 'self' is only applicable to instance methods.
      if (!isInstanceMethod) {
        diags.diagnose(paramLoc,
                       diag::diff_params_clause_self_instance_method_only);
        return nullptr;
      }
      // 'self' can only be the first in the list.
      if (i > 0) {
        diags.diagnose(paramLoc, diag::diff_params_clause_self_must_be_first);
        return nullptr;
      }
      parameterBits.set(parameterBits.size() - 1);
      break;
    }
    case ParsedAutoDiffParameter::Kind::Ordered: {
      auto index = parsedDiffParams[i].getIndex();
      if (index >= numParams) {
        diags.diagnose(paramLoc,
                       diag::diff_params_clause_param_index_out_of_range);
        return nullptr;
      }
      // Parameter names must be specified in the original order.
      if ((int)index <= lastIndex) {
        diags.diagnose(paramLoc,
                       diag::diff_params_clause_params_not_original_order);
        return nullptr;
      }
      parameterBits.set(index);
      lastIndex = index;
      break;
    }
    }
  }
  return IndexSubset::get(ctx, parameterBits);
}

// Checks if the given differentiability parameter indices are valid for the
// given original or derivative `AbstractFunctionDecl` and original function
// type in the given derivative generic environment and module context. Returns
// true on error.
//
// The parsed differentiability parameters and attribute location are used in
// diagnostics.
static bool checkDifferentiabilityParameters(
    AbstractFunctionDecl *AFD, IndexSubset *diffParamIndices,
    AnyFunctionType *functionType, GenericEnvironment *derivativeGenEnv,
    ModuleDecl *module, ArrayRef<ParsedAutoDiffParameter> parsedDiffParams,
    SourceLoc attrLoc) {
  auto &ctx = AFD->getASTContext();
  auto &diags = ctx.Diags;

  // Diagnose empty differentiability indices. No differentiability parameters
  // were resolved or inferred.
  if (diffParamIndices->isEmpty()) {
    diags.diagnose(attrLoc, diag::diff_params_clause_no_inferred_parameters);
    return true;
  }

  // Check that differentiability parameters have allowed types.
  SmallVector<AnyFunctionType::Param, 4> diffParams;
  functionType->getSubsetParameters(diffParamIndices, diffParams);
  for (unsigned i : range(diffParams.size())) {
    SourceLoc loc =
        parsedDiffParams.empty() ? attrLoc : parsedDiffParams[i].getLoc();
    auto diffParamType = diffParams[i].getPlainType();
    if (!diffParamType->hasTypeParameter())
      diffParamType = diffParamType->mapTypeOutOfContext();
    if (derivativeGenEnv)
      diffParamType = derivativeGenEnv->mapTypeIntoContext(diffParamType);
    else
      diffParamType = AFD->mapTypeIntoContext(diffParamType);
    // Parameter must conform to `Differentiable`.
    if (!conformsToDifferentiable(diffParamType, AFD)) {
      diags.diagnose(loc, diag::diff_params_clause_param_not_differentiable,
                     diffParamType);
      return true;
    }
  }
  return false;
}

// Returns the function declaration corresponding to the given function name and
// lookup context. If the base type of the function is specified, member lookup
// is performed. Otherwise, unqualified lookup is performed.
// If the function declaration cannot be resolved, emits a diagnostic and
// returns nullptr.
static AbstractFunctionDecl *findAbstractFunctionDecl(
    DeclNameRef funcName, SourceLoc funcNameLoc, Type baseType,
    DeclContext *lookupContext,
    const std::function<bool(AbstractFunctionDecl *)> &isValidCandidate,
    const std::function<void()> &noneValidDiagnostic,
    const std::function<void()> &ambiguousDiagnostic,
    const std::function<void()> &notFunctionDiagnostic,
    NameLookupOptions lookupOptions,
    const Optional<std::function<bool(AbstractFunctionDecl *)>>
        &hasValidTypeCtx,
    const Optional<std::function<void()>> &invalidTypeCtxDiagnostic) {
  auto &ctx = lookupContext->getASTContext();
  AbstractFunctionDecl *resolvedCandidate = nullptr;

  // Perform lookup.
  LookupResult results;
  // If `baseType` is not null but `lookupContext` is a type context, set
  // `baseType` to the `self` type of `lookupContext` to perform member lookup.
  if (!baseType && lookupContext->isTypeContext())
    baseType = lookupContext->getSelfTypeInContext();
  if (baseType) {
    results = TypeChecker::lookupMember(lookupContext, baseType, funcName);
  } else {
    results = TypeChecker::lookupUnqualified(lookupContext, funcName,
                                             funcNameLoc, lookupOptions);
  }

  // Initialize error flags.
  bool notFunction = false;
  bool wrongTypeContext = false;
  bool ambiguousFuncDecl = false;
  bool foundInvalid = false;

  // Filter lookup results.
  for (auto choice : results) {
    auto decl = choice.getValueDecl();
    if (!decl)
      continue;
    // Cast the candidate to an `AbstractFunctionDecl`.
    auto *candidate = dyn_cast<AbstractFunctionDecl>(decl);
    // If the candidate is an `AbstractStorageDecl`, use its getter as the
    // candidate.
    if (auto *asd = dyn_cast<AbstractStorageDecl>(decl))
      candidate = asd->getAccessor(AccessorKind::Get);
    if (!candidate) {
      notFunction = true;
      continue;
    }
    if (hasValidTypeCtx && !(*hasValidTypeCtx)(candidate)) {
      wrongTypeContext = true;
      continue;
    }
    if (!isValidCandidate(candidate)) {
      foundInvalid = true;
      continue;
    }
    if (resolvedCandidate) {
      ambiguousFuncDecl = true;
      resolvedCandidate = nullptr;
      break;
    }
    resolvedCandidate = candidate;
  }
  // If function declaration was resolved, return it.
  if (resolvedCandidate)
    return resolvedCandidate;

  // Otherwise, emit the appropriate diagnostic and return nullptr.
  if (results.empty()) {
    ctx.Diags.diagnose(funcNameLoc, diag::use_unresolved_identifier, funcName,
                       funcName.isOperator());
    return nullptr;
  }
  if (ambiguousFuncDecl) {
    ambiguousDiagnostic();
    return nullptr;
  }
  if (wrongTypeContext) {
    assert(invalidTypeCtxDiagnostic &&
           "Type context diagnostic should've been specified");
    (*invalidTypeCtxDiagnostic)();
    return nullptr;
  }
  if (foundInvalid) {
    noneValidDiagnostic();
    return nullptr;
  }
  assert(notFunction && "Expected 'not a function' error");
  notFunctionDiagnostic();
  return nullptr;
}

// Checks that the `candidate` function type equals the `required` function
// type, disregarding parameter labels and tuple result labels.
// `checkGenericSignature` is used to check generic signatures, if specified.
// Otherwise, generic signatures are checked for equality.
static bool checkFunctionSignature(
    CanAnyFunctionType required, CanType candidate,
    Optional<std::function<bool(GenericSignature, GenericSignature)>>
        checkGenericSignature = None) {
  // Check that candidate is actually a function.
  auto candidateFnTy = dyn_cast<AnyFunctionType>(candidate);
  if (!candidateFnTy)
    return false;

  // Erase dynamic self types.
  required = dyn_cast<AnyFunctionType>(required->getCanonicalType());
  candidateFnTy = dyn_cast<AnyFunctionType>(candidateFnTy->getCanonicalType());

  // Check that generic signatures match.
  auto requiredGenSig = required.getOptGenericSignature();
  auto candidateGenSig = candidateFnTy.getOptGenericSignature();
  // Call generic signature check function, if specified.
  // Otherwise, check that generic signatures are equal.
  if (!checkGenericSignature) {
    if (candidateGenSig != requiredGenSig)
      return false;
  } else if (!(*checkGenericSignature)(requiredGenSig, candidateGenSig)) {
    return false;
  }

  // Map type into the required function type's generic signature, if it exists.
  // This is significant when the required generic signature has same-type
  // requirements while the candidate generic signature does not.
  auto mapType = [&](Type type) {
    if (!requiredGenSig)
      return type->getCanonicalType();
    return requiredGenSig->getCanonicalTypeInContext(type);
  };

  // Check that parameter types match, disregarding labels.
  if (required->getNumParams() != candidateFnTy->getNumParams())
    return false;
  if (!std::equal(required->getParams().begin(), required->getParams().end(),
                  candidateFnTy->getParams().begin(),
                  [&](AnyFunctionType::Param x, AnyFunctionType::Param y) {
                    return x.getOldType()->isEqual(mapType(y.getOldType()));
                  }))
    return false;

  // If required result type is not a function type, check that result types
  // match exactly.
  auto requiredResultFnTy = dyn_cast<AnyFunctionType>(required.getResult());
  auto candidateResultTy = mapType(candidateFnTy.getResult());
  if (!requiredResultFnTy) {
    auto requiredResultTupleTy = dyn_cast<TupleType>(required.getResult());
    auto candidateResultTupleTy = dyn_cast<TupleType>(candidateResultTy);
    if (!requiredResultTupleTy || !candidateResultTupleTy)
      return required.getResult()->isEqual(candidateResultTy);
    // If result types are tuple types, check that element types match,
    // ignoring labels.
    if (requiredResultTupleTy->getNumElements() !=
        candidateResultTupleTy->getNumElements())
      return false;
    return std::equal(requiredResultTupleTy.getElementTypes().begin(),
                      requiredResultTupleTy.getElementTypes().end(),
                      candidateResultTupleTy.getElementTypes().begin(),
                      [](CanType x, CanType y) { return x->isEqual(y); });
  }

  // Required result type is a function. Recurse.
  return checkFunctionSignature(requiredResultFnTy, candidateResultTy);
};

// Returns an `AnyFunctionType` from the given parameters, result type, and
// generic signature.
static AnyFunctionType *
makeFunctionType(ArrayRef<AnyFunctionType::Param> parameters, Type resultType,
                 GenericSignature genericSignature) {
  if (genericSignature)
    return GenericFunctionType::get(genericSignature, parameters, resultType);
  return FunctionType::get(parameters, resultType);
}

// Computes the original function type corresponding to the given derivative
// function type. Used for `@derivative` attribute type-checking.
static AnyFunctionType *
getDerivativeOriginalFunctionType(AnyFunctionType *derivativeFnTy) {
  // Unwrap curry levels. At most, two parameter lists are necessary, for
  // curried method types with a `(Self)` parameter list.
  SmallVector<AnyFunctionType *, 2> curryLevels;
  auto *currentLevel = derivativeFnTy;
  for (unsigned i : range(2)) {
    (void)i;
    if (currentLevel == nullptr)
      break;
    curryLevels.push_back(currentLevel);
    currentLevel = currentLevel->getResult()->getAs<AnyFunctionType>();
  }

  auto derivativeResult = curryLevels.back()->getResult()->getAs<TupleType>();
  assert(derivativeResult && derivativeResult->getNumElements() == 2 &&
         "Expected derivative result to be a two-element tuple");
  auto originalResult = derivativeResult->getElement(0).getType();
  auto *originalType = makeFunctionType(
      curryLevels.back()->getParams(), originalResult,
      curryLevels.size() == 1 ? derivativeFnTy->getOptGenericSignature()
                              : nullptr);

  // Wrap the derivative function type in additional curry levels.
  auto curryLevelsWithoutLast =
      ArrayRef<AnyFunctionType *>(curryLevels).drop_back(1);
  for (auto pair : enumerate(llvm::reverse(curryLevelsWithoutLast))) {
    unsigned i = pair.index();
    AnyFunctionType *curryLevel = pair.value();
    originalType =
        makeFunctionType(curryLevel->getParams(), originalType,
                         i == curryLevelsWithoutLast.size() - 1
                             ? derivativeFnTy->getOptGenericSignature()
                             : nullptr);
  }
  return originalType;
}

// Finds a derivative function declaration using the given function specifier,
// original function declaration, expected type, and "is valid" predicate. If no
// valid derivative function is found, emits diagnostics and returns false.
static FuncDecl *findAutoDiffDerivativeFunction(
    DeclNameRefWithLoc specifier, AbstractFunctionDecl *original,
    Type expectedTy, std::function<bool(AbstractFunctionDecl *)> isValid) {
  auto &ctx = original->getASTContext();
  auto &diags = ctx.Diags;
  auto noneValidDiagnostic = [&]() {
    diags.diagnose(specifier.Loc, diag::differentiable_attr_overload_not_found,
                   specifier.Name, expectedTy);
  };
  auto ambiguousDiagnostic = [&]() {
    diags.diagnose(specifier.Loc, diag::attr_ambiguous_reference_to_decl,
                   specifier.Name, "differentiable");
  };
  auto notFunctionDiagnostic = [&]() {
    diags.diagnose(specifier.Loc,
                   diag::differentiable_attr_derivative_not_function,
                   specifier.Name);
  };
  std::function<void()> invalidTypeContextDiagnostic = [&]() {
    diags.diagnose(specifier.Loc,
                   diag::differentiable_attr_function_not_same_type_context,
                   specifier.Name);
  };

  // Returns true if the original function and derivative function candidate are
  // defined in compatible type contexts. If the original function and the
  // derivative function have different parents, or if they both have no type
  // context and are in different modules, return false.
  std::function<bool(AbstractFunctionDecl *)> hasValidTypeContext =
      [&](AbstractFunctionDecl *func) {
        // Check if both functions are top-level.
        if (!original->getInnermostTypeContext() &&
            !func->getInnermostTypeContext() &&
            original->getParentModule() == func->getParentModule())
          return true;
        // Check if both functions are defined in the same type context.
        if (auto typeCtx1 = original->getInnermostTypeContext())
          if (auto typeCtx2 = func->getInnermostTypeContext())
            return typeCtx1->getSelfNominalTypeDecl() ==
                   typeCtx2->getSelfNominalTypeDecl();
        return original->getParent() == func->getParent();
      };

  auto isABIPublic = [&](AbstractFunctionDecl *func) {
    return func->getFormalAccess() >= AccessLevel::Public ||
           func->getAttrs().hasAttribute<InlinableAttr>() ||
           func->getAttrs().hasAttribute<UsableFromInlineAttr>();
  };

  // If the original function is exported (i.e. it is public or
  // `@usableFromInline`), then the derivative functions must also be exported.
  // Returns true on error.
  auto checkAccessControl = [&](AbstractFunctionDecl *func) {
    if (!isABIPublic(original))
      return false;
    if (isABIPublic(func))
      return false;
    diags.diagnose(specifier.Loc, diag::differentiable_attr_invalid_access,
                   specifier.Name, original->getFullName());
    return true;
  };

  auto originalTypeCtx = original->getInnermostTypeContext();
  if (!originalTypeCtx)
    originalTypeCtx = original->getParent();
  assert(originalTypeCtx);

  // Set lookup options.
  auto lookupOptions =
      defaultMemberLookupOptions | NameLookupFlags::IgnoreAccessControl;

  auto *candidate = findAbstractFunctionDecl(
      specifier.Name, specifier.Loc.getBaseNameLoc(), /*baseType*/ Type(),
      originalTypeCtx, isValid, noneValidDiagnostic, ambiguousDiagnostic,
      notFunctionDiagnostic, lookupOptions, hasValidTypeContext,
      invalidTypeContextDiagnostic);
  if (!candidate)
    return nullptr;
  // Reject non-`func` registered derivatives. JVPs and VJPs must be `func`
  // declarations.
  if (isa<AccessorDecl>(candidate)) {
    diags.diagnose(specifier.Loc,
                   diag::differentiable_attr_derivative_not_function,
                   specifier.Name);
    return nullptr;
  }
  if (checkAccessControl(candidate))
    return nullptr;
  // Derivatives of class members must be final.
  if (original->getDeclContext()->getSelfClassDecl() && !candidate->isFinal()) {
    diags.diagnose(specifier.Loc,
                   diag::differentiable_attr_class_derivative_not_final);
    return nullptr;
  }
  assert(isa<FuncDecl>(candidate));
  auto *funcDecl = cast<FuncDecl>(candidate);
  return funcDecl;
}

/// Given a `@differentiable` attribute, attempts to resolve the original
/// `AbstractFunctionDecl` for which it is registered, using the declaration
/// on which it is actually declared. On error, emits diagnostic and returns
/// `nullptr`.
AbstractFunctionDecl *
resolveDifferentiableAttrOriginalFunction(DifferentiableAttr *attr) {
  auto *D = attr->getOriginalDeclaration();
  assert(D &&
         "Original declaration should be resolved by parsing/deserialization");
  auto &ctx = D->getASTContext();
  auto &diags = ctx.Diags;
  auto *original = dyn_cast<AbstractFunctionDecl>(D);
  if (auto *asd = dyn_cast<AbstractStorageDecl>(D)) {
    // Derivative registration is unsupported for stored properties.
    if (asd->getImplInfo().isSimpleStored() &&
        (attr->getJVP() || attr->getVJP())) {
      diagnoseAndRemoveAttr(
          diags, D, attr,
          diag::differentiable_attr_stored_property_variable_unsupported);
      attr->setInvalid();
      return nullptr;
    }
    // If `@differentiable` attribute is declared directly on a
    // `AbstractStorageDecl` (a stored/computed property or subscript),
    // forward the attribute to the storage's getter.
    // TODO(TF-129): Forward `@differentiable` attributes to setters after
    // differentiation supports inout parameters.
    // TODO(TF-1080): Forward `@differentiable` attributes to `read` and
    // `modify` accessors after differentiation supports `inout` parameters.
    if (!asd->getDeclContext()->isModuleScopeContext()) {
      original = asd->getSynthesizedAccessor(AccessorKind::Get);
    } else {
      original = nullptr;
    }
  }
  // Non-`get` accessors are not yet supported: `set`, `read`, and `modify`.
  // TODO(TF-129): Enable `set` when differentiation supports inout parameters.
  // TODO(TF-1080): Enable `read` and `modify` when differentiation supports
  // coroutines.
  if (auto *accessor = dyn_cast_or_null<AccessorDecl>(original))
    if (!accessor->isGetter())
      original = nullptr;
  // Diagnose if original `AbstractFunctionDecl` could not be resolved.
  if (!original) {
    diagnoseAndRemoveAttr(diags, D, attr, diag::invalid_decl_attribute, attr);
    attr->setInvalid();
    return nullptr;
  }
  // If the original function has an error interface type, return.
  // A diagnostic should have already been emitted.
  if (original->getInterfaceType()->hasError())
    return nullptr;
  return original;
}

/// Given a `@differentiable` attribute, attempts to resolve the derivative
/// generic signature. The derivative generic signature is returned as
/// `derivativeGenSig`. On error, emits diagnostic, assigns `nullptr` to
/// `derivativeGenSig`, and returns true.
bool resolveDifferentiableAttrDerivativeGenericSignature(
    DifferentiableAttr *attr, AbstractFunctionDecl *original,
    GenericSignature &derivativeGenSig) {
  derivativeGenSig = nullptr;

  auto &ctx = original->getASTContext();
  auto &diags = ctx.Diags;

  bool isOriginalProtocolRequirement =
      isa<ProtocolDecl>(original->getDeclContext()) &&
      original->isProtocolRequirement();

  // Compute the derivative generic signature for the `@differentiable`
  // attribute:
  // - If the `@differentiable` attribute has a `where` clause, use it to
  //   compute the derivative generic signature.
  // - Otherwise, use the original function's generic signature by default.
  derivativeGenSig = original->getGenericSignature();

  // Handle the `where` clause, if it exists.
  // - Resolve attribute where clause requirements and store in the attribute
  //   for serialization.
  // - Compute generic signature for autodiff derivative functions based on
  //   the original function's generate signature and the attribute's where
  //   clause requirements.
  if (auto *whereClause = attr->getWhereClause()) {
    // `@differentiable` attributes on protocol requirements do not support
    // `where` clauses.
    if (isOriginalProtocolRequirement) {
      diags.diagnose(attr->getLocation(),
                     diag::differentiable_attr_protocol_req_where_clause);
      attr->setInvalid();
      return true;
    }
    if (whereClause->getRequirements().empty()) {
      // `where` clause must not be empty.
      diags.diagnose(attr->getLocation(),
                     diag::differentiable_attr_empty_where_clause);
      attr->setInvalid();
      return true;
    }

    auto originalGenSig = original->getGenericSignature();
    if (!originalGenSig) {
      // `where` clauses are valid only when the original function is generic.
      diags
          .diagnose(
              attr->getLocation(),
              diag::differentiable_attr_where_clause_for_nongeneric_original,
              original->getFullName())
          .highlight(whereClause->getSourceRange());
      attr->setInvalid();
      return true;
    }

    // Build a new generic signature for autodiff derivative functions.
    GenericSignatureBuilder builder(ctx);
    // Add the original function's generic signature.
    builder.addGenericSignature(originalGenSig);

    using FloatingRequirementSource =
        GenericSignatureBuilder::FloatingRequirementSource;

    bool errorOccurred = false;
    WhereClauseOwner(original, attr)
        .visitRequirements(
            TypeResolutionStage::Structural,
            [&](const Requirement &req, RequirementRepr *reqRepr) {
              switch (req.getKind()) {
              case RequirementKind::SameType:
              case RequirementKind::Superclass:
              case RequirementKind::Conformance:
                break;

              // Layout requirements are not supported.
              case RequirementKind::Layout:
                diags
                    .diagnose(attr->getLocation(),
                              diag::differentiable_attr_layout_req_unsupported)
                    .highlight(reqRepr->getSourceRange());
                errorOccurred = true;
                return false;
              }

              // Add requirement to generic signature builder.
              builder.addRequirement(
                  req, reqRepr, FloatingRequirementSource::forExplicit(reqRepr),
                  nullptr, original->getModuleContext());
              return false;
            });

    if (errorOccurred) {
      attr->setInvalid();
      return true;
    }

    // Compute generic signature for derivative functions.
    derivativeGenSig = std::move(builder).computeGenericSignature(
        attr->getLocation(), /*allowConcreteGenericParams=*/true);
  }

  // Set the resolved derivative generic signature in the attribute.
  // Do not set the derivative generic signature if the original function's
  // generic signature is equal to `derivativeGenSig` and all generic parameters
  // are concrete. In that case, the original function and derivative functions
  // are all lowered as SIL functions with no generic signature (specialized
  // with concrete types from same-type requirements), so the derivative generic
  // signature should not be set.
  auto skipDerivativeGenericSignature = [&] {
    auto origCanGenSig =
        original->getGenericSignature().getCanonicalSignature();
    auto derivativeCanGenSig = derivativeGenSig.getCanonicalSignature();
    if (!derivativeCanGenSig)
      return false;
    return origCanGenSig == derivativeCanGenSig &&
           derivativeCanGenSig->areAllParamsConcrete();
  };
  if (skipDerivativeGenericSignature())
    derivativeGenSig = GenericSignature();
  attr->setDerivativeGenericSignature(derivativeGenSig);
  return false;
}

/// Given a `@differentiable` attribute, attempts to resolve and validate the
/// differentiability parameter indices. The parameter indices are returned as
/// `diffParamIndices`. On error, emits diagnostic, assigns `nullptr` to
/// `diffParamIndices`, and returns true.
bool resolveDifferentiableAttrDifferentiabilityParameters(
    DifferentiableAttr *attr, AbstractFunctionDecl *original,
    AnyFunctionType *derivativeFnTy, GenericEnvironment *derivativeGenEnv,
    IndexSubset *&diffParamIndices) {
  diffParamIndices = nullptr;

  // Get the parsed differentiability parameter indices, which have not yet been
  // resolved. Parsed differentiability parameter indices are defined only for
  // parsed attributes.
  auto parsedDiffParams = attr->getParsedParameters();

  diffParamIndices = computeDifferentiabilityParameters(
      parsedDiffParams, original, derivativeGenEnv, attr->getAttrName(),
      attr->getLocation());
  if (!diffParamIndices) {
    attr->setInvalid();
    return true;
  }

  // Check if differentiability parameter indices are valid.
  if (checkDifferentiabilityParameters(original, diffParamIndices,
                                       derivativeFnTy, derivativeGenEnv,
                                       original->getModuleContext(),
                                       parsedDiffParams, attr->getLocation())) {
    attr->setInvalid();
    return true;
  }

  return false;
}

/// Given a `@differentiable` attribute, attempts to resolve the JVP and VJP
/// derivative function declarations, if specified. The JVP and VJP functions
/// are returned as `jvp` and `vjp`, respectively. On error, emits diagnostic,
/// assigns `nullptr` to `jvp` and `vjp`, and returns true.
bool resolveDifferentiableAttrDerivativeFunctions(
    DifferentiableAttr *attr, AbstractFunctionDecl *original,
    IndexSubset *resolvedDiffParamIndices, GenericSignature derivativeGenSig,
    FuncDecl *&jvp, FuncDecl *&vjp) {
  jvp = nullptr;
  vjp = nullptr;

  auto &ctx = original->getASTContext();
  auto &diags = ctx.Diags;

  // `@differentiable` attributes on protocol requirements do not support
  // JVP/VJP.
  bool isOriginalProtocolRequirement =
      isa<ProtocolDecl>(original->getDeclContext()) &&
      original->isProtocolRequirement();
  if (isOriginalProtocolRequirement && (attr->getJVP() || attr->getVJP())) {
    diags.diagnose(attr->getLocation(),
                   diag::differentiable_attr_protocol_req_assoc_func);
    attr->setInvalid();
    return false;
  }

  auto *originalFnTy = original->getInterfaceType()->castTo<AnyFunctionType>();
  auto lookupConformance =
      LookUpConformanceInModule(original->getDeclContext()->getParentModule());

  // Resolve the JVP function, if it is specified and exists.
  if (attr->getJVP()) {
    auto *expectedJVPFnTy = originalFnTy->getAutoDiffDerivativeFunctionType(
        resolvedDiffParamIndices, AutoDiffDerivativeFunctionKind::JVP,
        lookupConformance, derivativeGenSig, /*makeSelfParamFirst*/ true);
    auto isValidJVP = [&](AbstractFunctionDecl *jvpCandidate) -> bool {
      return checkFunctionSignature(
          cast<AnyFunctionType>(expectedJVPFnTy->getCanonicalType()),
          jvpCandidate->getInterfaceType()->getCanonicalType());
    };
    auto *jvp = findAutoDiffDerivativeFunction(
        attr->getJVP().getValue(), original, expectedJVPFnTy, isValidJVP);
    if (!jvp) {
      attr->setInvalid();
      return true;
    }
    // Set the JVP function in the attribute.
    attr->setJVPFunction(jvp);
  }

  // Resolve the VJP function, if it is specified and exists.
  if (attr->getVJP()) {
    auto *expectedVJPFnTy = originalFnTy->getAutoDiffDerivativeFunctionType(
        resolvedDiffParamIndices, AutoDiffDerivativeFunctionKind::VJP,
        lookupConformance, derivativeGenSig, /*makeSelfParamFirst*/ true);
    auto isValidVJP = [&](AbstractFunctionDecl *vjpCandidate) -> bool {
      return checkFunctionSignature(
          cast<AnyFunctionType>(expectedVJPFnTy->getCanonicalType()),
          vjpCandidate->getInterfaceType()->getCanonicalType());
    };
    auto *vjp = findAutoDiffDerivativeFunction(
        attr->getVJP().getValue(), original, expectedVJPFnTy, isValidVJP);
    if (!vjp) {
      attr->setInvalid();
      return true;
    }
    // Set the VJP function in the attribute.
    attr->setVJPFunction(vjp);
  }

  return false;
}

/// Checks whether differentiable programming is enabled for the given
/// differentiation-related attribute. Returns true on error.
bool checkIfDifferentiableProgrammingEnabled(
    ASTContext &ctx, DeclAttribute *attr) {
  auto &diags = ctx.Diags;
  // The experimental differentiable programming flag must be enabled.
  if (!ctx.LangOpts.EnableExperimentalDifferentiableProgramming) {
    diags
        .diagnose(attr->getLocation(),
                  diag::experimental_differentiable_programming_disabled)
        .highlight(attr->getRangeWithAt());
    return true;
  }
  // The `Differentiable` protocol must be available.
  // If unavailable, the `_Differentiation` module should be imported.
  if (!ctx.getProtocol(KnownProtocolKind::Differentiable)) {
    diags
        .diagnose(attr->getLocation(), diag::attr_used_without_required_module,
                  attr, ctx.Id_Differentiation)
        .highlight(attr->getRangeWithAt());
    return true;
  }
  return false;
}

llvm::Expected<IndexSubset *> DifferentiableAttributeTypeCheckRequest::evaluate(
    Evaluator &evaluator, DifferentiableAttr *attr) const {
  // Skip type-checking for implicit `@differentiable` attributes. We currently
  // assume that all implicit `@differentiable` attributes are valid.
  //
  // Motivation: some implicit attributes do not have a `where` clause, and this
  // function assumes that the `where` clauses exist. Propagating `where`
  // clauses and requirements consistently is a larger problem, to be revisited.
  if (attr->isImplicit())
    return nullptr;

  auto *D = attr->getOriginalDeclaration();
  auto &ctx = D->getASTContext();
  auto &diags = ctx.Diags;
  // `@differentiable` attribute requires experimental differentiable
  // programming to be enabled.
  if (checkIfDifferentiableProgrammingEnabled(ctx, attr))
    return nullptr;

  // Derivative registration is disabled for `@differentiable(linear)`
  // attributes. Instead, use `@transpose` attribute to register transpose
  // functions.
  if (attr->isLinear() && (attr->getVJP() || attr->getJVP())) {
    diagnoseAndRemoveAttr(diags, D, attr,
                          diag::differentiable_attr_no_vjp_or_jvp_when_linear);
    attr->setInvalid();
    return nullptr;
  }

  // Resolve the original `AbstractFunctionDecl`.
  auto *original = resolveDifferentiableAttrOriginalFunction(attr);
  if (!original)
    return nullptr;

  auto *originalFnTy = original->getInterfaceType()->castTo<AnyFunctionType>();

  // Diagnose if original function is an invalid class member.
  bool isOriginalClassMember = original->getDeclContext() &&
                               original->getDeclContext()->getSelfClassDecl();
  if (isOriginalClassMember) {
    auto *classDecl = original->getDeclContext()->getSelfClassDecl();
    assert(classDecl);
    // Class members returning dynamic `Self` are not supported.
    // Dynamic `Self` is supported only as a single top-level result for class
    // members. JVP/VJP functions returning `(Self, ...)` tuples would not
    // type-check.
    bool diagnoseDynamicSelfResult = original->hasDynamicSelfResult();
    if (diagnoseDynamicSelfResult) {
      // Diagnose class initializers in non-final classes.
      if (isa<ConstructorDecl>(original)) {
        if (!classDecl->isFinal()) {
          diags.diagnose(
              attr->getLocation(),
              diag::differentiable_attr_nonfinal_class_init_unsupported,
              classDecl->getDeclaredInterfaceType());
          attr->setInvalid();
          return nullptr;
        }
      }
      // Diagnose all other declarations returning dynamic `Self`.
      else {
        diags.diagnose(
            attr->getLocation(),
            diag::
                differentiable_attr_class_member_dynamic_self_result_unsupported);
        attr->setInvalid();
        return nullptr;
      }
    }
  }

  // Resolve the derivative generic signature.
  GenericSignature derivativeGenSig = nullptr;
  if (resolveDifferentiableAttrDerivativeGenericSignature(attr, original,
                                                          derivativeGenSig))
    return nullptr;
  GenericEnvironment *derivativeGenEnv = nullptr;
  if (derivativeGenSig)
    derivativeGenEnv = derivativeGenSig->getGenericEnvironment();

  // Compute the derivative function type.
  auto derivativeFnTy = originalFnTy;
  if (derivativeGenEnv)
    derivativeFnTy = derivativeGenEnv->mapTypeIntoContext(derivativeFnTy)
                         ->castTo<AnyFunctionType>();

  // Resolve and validate the differentiability parameters.
  IndexSubset *resolvedDiffParamIndices = nullptr;
  if (resolveDifferentiableAttrDifferentiabilityParameters(
          attr, original, derivativeFnTy, derivativeGenEnv,
          resolvedDiffParamIndices))
    return nullptr;

  // Get the original semantic result type.
  llvm::SmallVector<AutoDiffSemanticFunctionResultType, 1> originalResults;
  autodiff::getFunctionSemanticResultTypes(originalFnTy, originalResults,
                                           derivativeGenEnv);
  // Check that original function has at least one semantic result, i.e.
  // that the original semantic result type is not `Void`.
  if (originalResults.empty()) {
    diags
        .diagnose(attr->getLocation(), diag::autodiff_attr_original_void_result,
                  original->getFullName())
        .highlight(original->getSourceRange());
    attr->setInvalid();
    return nullptr;
  }
  // Check that original function does not have multiple semantic results.
  if (originalResults.size() > 1) {
    diags
        .diagnose(attr->getLocation(),
                  diag::autodiff_attr_original_multiple_semantic_results)
        .highlight(original->getSourceRange());
    attr->setInvalid();
    return nullptr;
  }
  auto originalResult = originalResults.front();
  auto originalResultTy = originalResult.type;
  // Check that the original semantic result conforms to `Differentiable`.
  if (!conformsToDifferentiable(originalResultTy, original)) {
    diags.diagnose(attr->getLocation(),
                   diag::differentiable_attr_result_not_differentiable,
                   originalResultTy);
    attr->setInvalid();
    return nullptr;
  }

  // Resolve JVP and VJP derivative functions, if specified.
  FuncDecl *jvp = nullptr;
  FuncDecl *vjp = nullptr;
  if (resolveDifferentiableAttrDerivativeFunctions(
          attr, original, resolvedDiffParamIndices, derivativeGenSig, jvp, vjp))
    return nullptr;

  if (auto *asd = dyn_cast<AbstractStorageDecl>(D)) {
    // Remove `@differentiable` attribute from storage declaration to prevent
    // duplicate attribute registration during SILGen.
    D->getAttrs().removeAttribute(attr);
    // Transfer `@differentiable` attribute from storage declaration to
    // getter accessor.
    auto *getterDecl = asd->getAccessor(AccessorKind::Get);
    auto *newAttr = DifferentiableAttr::create(
        getterDecl, /*implicit*/ true, attr->AtLoc, attr->getRange(),
        attr->isLinear(), resolvedDiffParamIndices, attr->getJVP(),
        attr->getVJP(), attr->getDerivativeGenericSignature());
    newAttr->setJVPFunction(attr->getJVPFunction());
    newAttr->setVJPFunction(attr->getVJPFunction());
    auto insertion = ctx.DifferentiableAttrs.try_emplace(
        {getterDecl, resolvedDiffParamIndices}, newAttr);
    // Reject duplicate `@differentiable` attributes.
    if (!insertion.second) {
      diagnoseAndRemoveAttr(diags, D, attr,
                            diag::differentiable_attr_duplicate);
      diags.diagnose(insertion.first->getSecond()->getLocation(),
                     diag::differentiable_attr_duplicate_note);
      return nullptr;
    }
    getterDecl->getAttrs().add(newAttr);
    return resolvedDiffParamIndices;
  }
  // Reject duplicate `@differentiable` attributes.
  auto insertion =
      ctx.DifferentiableAttrs.try_emplace({D, resolvedDiffParamIndices}, attr);
  if (!insertion.second && insertion.first->getSecond() != attr) {
    diagnoseAndRemoveAttr(diags, D, attr, diag::differentiable_attr_duplicate);
    diags.diagnose(insertion.first->getSecond()->getLocation(),
                   diag::differentiable_attr_duplicate_note);
    return nullptr;
  }
  // Register derivative function configuration.
  auto *resultIndices = IndexSubset::get(ctx, 1, {0});
  original->addDerivativeFunctionConfiguration(
      {resolvedDiffParamIndices, resultIndices, derivativeGenSig});
  return resolvedDiffParamIndices;
}

void AttributeChecker::visitDifferentiableAttr(DifferentiableAttr *attr) {
  // Call `getParameterIndices` to trigger
  // `DifferentiableAttributeTypeCheckRequest`.
  (void)attr->getParameterIndices();
}

/// Type-checks the given `@derivative` attribute `attr` on declaration `D`.
///
/// Effects are:
/// - Sets the original function and parameter indices on `attr`.
/// - Diagnoses errors.
/// - Stores the attribute in `ASTContext::DerivativeAttrs`.
///
/// \returns true on error, false on success.
static bool typeCheckDerivativeAttr(ASTContext &Ctx, Decl *D,
                                    DerivativeAttr *attr) {
  // Note: Implementation must be idempotent because it may be called multiple
  // times for the same attribute.
  auto &diags = Ctx.Diags;
  // `@derivative` attribute requires experimental differentiable programming
  // to be enabled.
  if (checkIfDifferentiableProgrammingEnabled(Ctx, attr))
    return true;
  auto *derivative = cast<FuncDecl>(D);
  auto lookupConformance =
      LookUpConformanceInModule(D->getDeclContext()->getParentModule());
  auto originalName = attr->getOriginalFunctionName();

  auto *derivativeInterfaceType =
      derivative->getInterfaceType()->castTo<AnyFunctionType>();

  // Perform preliminary `@derivative` declaration checks.
  // The result type should be a two-element tuple.
  // Either a value and pullback:
  //     (value: R, pullback: (R.TangentVector) -> (T.TangentVector...)
  // Or a value and differential:
  //     (value: R, differential: (T.TangentVector...) -> (R.TangentVector)
  auto derivativeResultType = derivative->getResultInterfaceType();
  auto derivativeResultTupleType = derivativeResultType->getAs<TupleType>();
  if (!derivativeResultTupleType ||
      derivativeResultTupleType->getNumElements() != 2) {
    diags.diagnose(attr->getLocation(),
                   diag::derivative_attr_expected_result_tuple);
    return true;
  }
  auto valueResultElt = derivativeResultTupleType->getElement(0);
  auto funcResultElt = derivativeResultTupleType->getElement(1);
  // Get derivative kind and derivative function identifier.
  AutoDiffDerivativeFunctionKind kind;
  if (valueResultElt.getName().str() != "value") {
    diags.diagnose(attr->getLocation(),
                   diag::derivative_attr_invalid_result_tuple_value_label);
    return true;
  }
  if (funcResultElt.getName().str() == "differential") {
    kind = AutoDiffDerivativeFunctionKind::JVP;
  } else if (funcResultElt.getName().str() == "pullback") {
    kind = AutoDiffDerivativeFunctionKind::VJP;
  } else {
    diags.diagnose(attr->getLocation(),
                   diag::derivative_attr_invalid_result_tuple_func_label);
    return true;
  }
  attr->setDerivativeKind(kind);

  // Compute expected original function type and look up original function.
  auto *originalFnType =
      getDerivativeOriginalFunctionType(derivativeInterfaceType);

  // Returns true if the generic parameters in `source` satisfy the generic
  // requirements in `target`.
  std::function<bool(GenericSignature, GenericSignature)>
      checkGenericSignatureSatisfied = [&](GenericSignature source,
                                           GenericSignature target) {
        // If target is null, then its requirements are satisfied.
        if (!target)
          return true;
        // If source is null but target is not null, then target's
        // requirements are not satisfied.
        if (!source)
          return false;
        // Check if target's requirements are satisfied by source.
        // Cancel diagnostics using `DiagnosticTransaction`.
        // Diagnostics should not be emitted because this function is used to
        // check candidates; if no candidates match, a separate diagnostic will
        // be produced.
        DiagnosticTransaction transaction(Ctx.Diags);
        SWIFT_DEFER { transaction.abort(); };
        return TypeChecker::checkGenericArguments(
                   derivative, originalName.Loc.getBaseNameLoc(),
                   originalName.Loc.getBaseNameLoc(), Type(),
                   source->getGenericParams(), target->getRequirements(),
                   [](SubstitutableType *dependentType) {
                     return Type(dependentType);
                   },
                   lookupConformance, None) == RequirementCheckResult::Success;
      };

  auto isValidOriginal = [&](AbstractFunctionDecl *originalCandidate) {
    // TODO(TF-982): Allow derivatives on protocol requirements.
    if (isa<ProtocolDecl>(originalCandidate->getDeclContext()))
      return false;
    return checkFunctionSignature(
        cast<AnyFunctionType>(originalFnType->getCanonicalType()),
        originalCandidate->getInterfaceType()->getCanonicalType(),
        checkGenericSignatureSatisfied);
  };

  auto noneValidDiagnostic = [&]() {
    diags.diagnose(originalName.Loc,
                   diag::autodiff_attr_original_decl_none_valid_found,
                   originalName.Name, originalFnType);
  };
  auto ambiguousDiagnostic = [&]() {
    diags.diagnose(originalName.Loc, diag::attr_ambiguous_reference_to_decl,
                   originalName.Name, attr->getAttrName());
  };
  auto notFunctionDiagnostic = [&]() {
    diags.diagnose(originalName.Loc,
                   diag::autodiff_attr_original_decl_invalid_kind,
                   originalName.Name);
  };
  std::function<void()> invalidTypeContextDiagnostic = [&]() {
    diags.diagnose(originalName.Loc,
                   diag::autodiff_attr_original_decl_not_same_type_context,
                   originalName.Name);
  };

  // Returns true if the derivative function and original function candidate are
  // defined in compatible type contexts. If the derivative function and the
  // original function candidate have different parents, return false.
  std::function<bool(AbstractFunctionDecl *)> hasValidTypeContext =
      [&](AbstractFunctionDecl *func) {
        // Check if both functions are top-level.
        if (!derivative->getInnermostTypeContext() &&
            !func->getInnermostTypeContext())
          return true;
        // Check if both functions are defined in the same type context.
        if (auto typeCtx1 = derivative->getInnermostTypeContext())
          if (auto typeCtx2 = func->getInnermostTypeContext()) {
            return typeCtx1->getSelfNominalTypeDecl() ==
                   typeCtx2->getSelfNominalTypeDecl();
          }
        return derivative->getParent() == func->getParent();
      };

  auto resolution = TypeResolution::forContextual(derivative->getDeclContext());
  Type baseType;
  if (auto *baseTypeRepr = attr->getBaseTypeRepr()) {
    TypeResolutionOptions options = None;
    options |= TypeResolutionFlags::AllowModule;
    baseType = resolution.resolveType(baseTypeRepr, options);
  }
  if (baseType && baseType->hasError())
    return true;
  auto lookupOptions = attr->getBaseTypeRepr()
                           ? defaultMemberLookupOptions
                           : defaultUnqualifiedLookupOptions;
  auto derivativeTypeCtx = derivative->getInnermostTypeContext();
  if (!derivativeTypeCtx)
    derivativeTypeCtx = derivative->getParent();
  assert(derivativeTypeCtx);

  // Look up original function.
  auto *originalAFD = findAbstractFunctionDecl(
      originalName.Name, originalName.Loc.getBaseNameLoc(), baseType,
      derivativeTypeCtx, isValidOriginal, noneValidDiagnostic,
      ambiguousDiagnostic, notFunctionDiagnostic, lookupOptions,
      hasValidTypeContext, invalidTypeContextDiagnostic);
  if (!originalAFD)
    return true;
  // Diagnose original stored properties. Stored properties cannot have custom
  // registered derivatives.
  if (auto *accessorDecl = dyn_cast<AccessorDecl>(originalAFD)) {
    auto *asd = accessorDecl->getStorage();
    if (asd->hasStorage()) {
      diags.diagnose(originalName.Loc,
                     diag::derivative_attr_original_stored_property_unsupported,
                     originalName.Name);
      diags.diagnose(originalAFD->getLoc(), diag::decl_declared_here,
                     asd->getFullName());
      return true;
    }
  }
  // Diagnose if original function is an invalid class member.
  bool isOriginalClassMember =
      originalAFD->getDeclContext() &&
      originalAFD->getDeclContext()->getSelfClassDecl();
  if (isOriginalClassMember) {
    auto *classDecl = originalAFD->getDeclContext()->getSelfClassDecl();
    assert(classDecl);
    // Class members returning dynamic `Self` are not supported.
    // Dynamic `Self` is supported only as a single top-level result for class
    // members. JVP/VJP functions returning `(Self, ...)` tuples would not
    // type-check.
    bool diagnoseDynamicSelfResult = originalAFD->hasDynamicSelfResult();
    if (diagnoseDynamicSelfResult) {
      // Diagnose class initializers in non-final classes.
      if (isa<ConstructorDecl>(originalAFD)) {
        if (!classDecl->isFinal()) {
          diags.diagnose(attr->getLocation(),
                         diag::derivative_attr_nonfinal_class_init_unsupported,
                         classDecl->getDeclaredInterfaceType());
          return true;
        }
      }
      // Diagnose all other declarations returning dynamic `Self`.
      else {
        diags.diagnose(
            attr->getLocation(),
            diag::derivative_attr_class_member_dynamic_self_result_unsupported,
            DeclNameRef(originalAFD->getFullName()));
        return true;
      }
    }
  }
  attr->setOriginalFunction(originalAFD);

  // Get the resolved differentiability parameter indices.
  auto *resolvedDiffParamIndices = attr->getParameterIndices();

  // Get the parsed differentiability parameter indices, which have not yet been
  // resolved. Parsed differentiability parameter indices are defined only for
  // parsed attributes.
  auto parsedDiffParams = attr->getParsedParameters();

  // If differentiability parameter indices are not resolved, compute them.
  if (!resolvedDiffParamIndices)
    resolvedDiffParamIndices = computeDifferentiabilityParameters(
        parsedDiffParams, derivative, derivative->getGenericEnvironment(),
        attr->getAttrName(), attr->getLocation());
  if (!resolvedDiffParamIndices)
    return true;

  // Check if the differentiability parameter indices are valid.
  if (checkDifferentiabilityParameters(
          originalAFD, resolvedDiffParamIndices, originalFnType,
          derivative->getGenericEnvironment(), derivative->getModuleContext(),
          parsedDiffParams, attr->getLocation()))
    return true;

  // Set the resolved differentiability parameter indices in the attribute.
  attr->setParameterIndices(resolvedDiffParamIndices);

  // Get the original semantic result.
  llvm::SmallVector<AutoDiffSemanticFunctionResultType, 1> originalResults;
  autodiff::getFunctionSemanticResultTypes(
      originalFnType, originalResults,
      derivative->getGenericEnvironmentOfContext());
  // Check that original function has at least one semantic result, i.e.
  // that the original semantic result type is not `Void`.
  if (originalResults.empty()) {
    diags
        .diagnose(attr->getLocation(), diag::autodiff_attr_original_void_result,
                  derivative->getFullName())
        .highlight(attr->getOriginalFunctionName().Loc.getSourceRange());
    attr->setInvalid();
    return true;
  }
  // Check that original function does not have multiple semantic results.
  if (originalResults.size() > 1) {
    diags
        .diagnose(attr->getLocation(),
                  diag::autodiff_attr_original_multiple_semantic_results)
        .highlight(attr->getOriginalFunctionName().Loc.getSourceRange());
    attr->setInvalid();
    return true;
  }
  auto originalResult = originalResults.front();
  auto originalResultType = originalResult.type;
  // Check that the original semantic result conforms to `Differentiable`.
  auto *diffableProto = Ctx.getProtocol(KnownProtocolKind::Differentiable);
  auto valueResultConf = TypeChecker::conformsToProtocol(
      originalResultType, diffableProto, derivative->getDeclContext(), None);
  if (!valueResultConf) {
    diags.diagnose(attr->getLocation(),
                   diag::derivative_attr_result_value_not_differentiable,
                   valueResultElt.getType());
    return true;
  }

  // Compute the actual differential/pullback type that we use for comparison
  // with the expected type. We must canonicalize the derivative interface type
  // before extracting the differential/pullback type from it, so that the
  // derivative interface type generic signature is available for simplifying
  // types.
  CanType canActualResultType = derivativeInterfaceType->getCanonicalType();
  while (isa<AnyFunctionType>(canActualResultType)) {
    canActualResultType =
        cast<AnyFunctionType>(canActualResultType).getResult();
  }
  CanType actualFuncEltType =
      cast<TupleType>(canActualResultType).getElementType(1);

  // Compute expected differential/pullback type.
  Type expectedFuncEltType =
      originalFnType->getAutoDiffDerivativeFunctionLinearMapType(
          resolvedDiffParamIndices, kind.getLinearMapKind(), lookupConformance,
          /*makeSelfParamFirst*/ true);
  if (expectedFuncEltType->hasTypeParameter())
    expectedFuncEltType = derivative->mapTypeIntoContext(expectedFuncEltType);
  if (expectedFuncEltType->hasArchetype())
    expectedFuncEltType = expectedFuncEltType->mapTypeOutOfContext();

  // Check if differential/pullback type matches expected type.
  if (!actualFuncEltType->isEqual(expectedFuncEltType)) {
    // Emit differential/pullback type mismatch error on attribute.
    diags.diagnose(attr->getLocation(),
                   diag::derivative_attr_result_func_type_mismatch,
                   funcResultElt.getName(), originalAFD->getFullName());
    // Emit note with expected differential/pullback type on actual type
    // location.
    auto *tupleReturnTypeRepr =
        cast<TupleTypeRepr>(derivative->getBodyResultTypeLoc().getTypeRepr());
    auto *funcEltTypeRepr = tupleReturnTypeRepr->getElementType(1);
    diags
        .diagnose(funcEltTypeRepr->getStartLoc(),
                  diag::derivative_attr_result_func_type_mismatch_note,
                  funcResultElt.getName(), expectedFuncEltType)
        .highlight(funcEltTypeRepr->getSourceRange());
    // Emit note showing original function location, if possible.
    if (originalAFD->getLoc().isValid())
      diags.diagnose(originalAFD->getLoc(),
                     diag::derivative_attr_result_func_original_note,
                     originalAFD->getFullName());
    return true;
  }

  // Reject different-file derivative registration.
  // TODO(TF-1021): Lift same-file derivative registration restriction.
  if (originalAFD->getParentSourceFile() != derivative->getParentSourceFile()) {
    diags.diagnose(attr->getLocation(),
                   diag::derivative_attr_not_in_same_file_as_original);
    return true;
  }

  // Reject duplicate `@derivative` attributes.
  auto &derivativeAttrs = Ctx.DerivativeAttrs[std::make_tuple(
      originalAFD, resolvedDiffParamIndices, kind)];
  derivativeAttrs.insert(attr);
  if (derivativeAttrs.size() > 1) {
    diags.diagnose(attr->getLocation(),
                   diag::derivative_attr_original_already_has_derivative,
                   originalAFD->getFullName());
    for (auto *duplicateAttr : derivativeAttrs) {
      if (duplicateAttr == attr)
        continue;
      diags.diagnose(duplicateAttr->getLocation(),
                     diag::derivative_attr_duplicate_note);
    }
    return true;
  }

  return false;
}

void AttributeChecker::visitDerivativeAttr(DerivativeAttr *attr) {
  if (typeCheckDerivativeAttr(Ctx, D, attr))
    attr->setInvalid();
}

void AttributeChecker::visitRequiresConcreteImplementationAttr(
                                     RequiresConcreteImplementationAttr *attr) {
  if (!isa<ProtocolDecl>(D->getDeclContext()))
    diagnoseAndRemoveAttr(attr,
                diag::requires_concrete_implementation_attr_wrong_decl_context);
}
