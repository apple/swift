//===--- SILSymbolVisitor.cpp ---------------------------------------------===//
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
//  This file implements liker symbol enumeration for SILSymbolVisitor.
//
//===----------------------------------------------------------------------===//

#include "swift/SIL/SILSymbolVisitor.h"

#include "swift/AST/ASTContext.h"
#include "swift/AST/ASTMangler.h"
#include "swift/AST/Decl.h"
#include "swift/AST/Expr.h"
#include "swift/AST/FileUnit.h"
#include "swift/AST/ParameterList.h"
#include "swift/AST/PropertyWrappers.h"
#include "swift/AST/SynthesizedFileUnit.h"
#include "swift/Basic/Defer.h"
#include "swift/SIL/FormalLinkage.h"
#include "swift/SIL/SILLinkage.h"
#include "swift/SIL/SILModule.h"
#include "swift/SIL/SILVTableVisitor.h"
#include "swift/SIL/SILWitnessTable.h"
#include "swift/SIL/SILWitnessVisitor.h"

using namespace swift;

static bool requiresLinkerDirective(Decl *D) {
  for (auto *attr : D->getAttrs()) {
    if (auto *ODA = dyn_cast<OriginallyDefinedInAttr>(attr)) {
      auto Active = ODA->isActivePlatform(D->getASTContext());
      if (Active.has_value())
        return true;
    }
  }
  return false;
}

static bool isGlobalOrStaticVar(VarDecl *VD) {
  return VD->isStatic() || VD->getDeclContext()->isModuleScopeContext();
}

using DynamicKind = SILSymbolVisitor::DynamicKind;

static llvm::Optional<DynamicKind> getDynamicKind(ValueDecl *VD) {
  if (VD->shouldUseNativeMethodReplacement())
    return DynamicKind::Replaceable;

  if (VD->getDynamicallyReplacedDecl())
    return DynamicKind::Replacement;

  return llvm::None;
}

class SILSymbolVisitorImpl : public ASTVisitor<SILSymbolVisitorImpl> {
  SILSymbolVisitor &Visitor;
  const SILSymbolVisitorContext &Ctx;
  llvm::SmallVector<Decl *, 4> DeclStack;

  /// A set of original function and derivative configuration pairs for which
  /// derivative symbols have been emitted.
  ///
  /// Used to deduplicate derivative symbol emission for `@differentiable` and
  /// `@derivative` attributes.
  llvm::DenseSet<std::pair<AbstractFunctionDecl *, AutoDiffConfig>>
      AddedDerivatives;

  void addMainIfNecessary(FileUnit *file) {
    // Make sure to only add the main symbol for the module that we're emitting
    // TBD for, and not for any statically linked libraries.
    if (!file->hasEntryPoint() || file->getParentModule() != Ctx.getModule())
      return;

    auto entryPointSymbol =
        file->getParentModule()->getASTContext().getEntryPointFunctionName();

    if (auto *decl = file->getMainDecl()) {
      addFunction(SILDeclRef::getMainDeclEntryPoint(decl),
                  /*ignoreLinkage=*/true);
      return;
    }

    auto declRef = SILDeclRef::getMainFileEntryPoint(file);
    Visitor.addFunction(entryPointSymbol, declRef);
  }

  /// Emits the given `SILDeclRef` to the downstream visitor as long as the
  /// entity has the required linkage.
  ///
  /// FIXME: The need for an ignoreLinkage flag here possibly indicates that
  ///        there is something broken about the linkage computation below.
  void addFunction(SILDeclRef declRef, bool ignoreLinkage = false) {
    if (!ignoreLinkage) {
      auto linkage = effectiveLinkageForClassMember(
          declRef.getLinkage(ForDefinition), declRef.getSubclassScope());
      if (Ctx.getOpts().PublicSymbolsOnly && linkage != SILLinkage::Public)
        return;
    }

    Visitor.addFunction(declRef);
  }

  void addAsyncFunctionPointer(SILDeclRef declRef) {
    auto silLinkage = effectiveLinkageForClassMember(
        declRef.getLinkage(ForDefinition), declRef.getSubclassScope());
    if (Ctx.getOpts().PublicSymbolsOnly && silLinkage != SILLinkage::Public)
      return;

    Visitor.addAsyncFunctionPointer(declRef);
  }

  void addAutoDiffLinearMapFunction(AbstractFunctionDecl *original,
                                    const AutoDiffConfig &config,
                                    AutoDiffLinearMapKind kind) {
    auto &ctx = original->getASTContext();
    auto declRef =
        SILDeclRef(original).asForeign(requiresForeignEntryPoint(original));

    // Linear maps are public only when the original function is serialized. So
    // if we're only including public symbols and it's not serialized, bail.
    if (Ctx.getOpts().PublicSymbolsOnly && !declRef.isSerialized())
      return;

    // Differential functions are emitted only when forward-mode is enabled.
    if (kind == AutoDiffLinearMapKind::Differential &&
        !ctx.LangOpts.hasFeature(Feature::ForwardModeDifferentiation))
      return;

    auto *loweredParamIndices = autodiff::getLoweredParameterIndices(
        config.parameterIndices,
        original->getInterfaceType()->castTo<AnyFunctionType>());
    Mangle::ASTMangler mangler;
    AutoDiffConfig silConfig{
        loweredParamIndices, config.resultIndices,
        autodiff::getDifferentiabilityWitnessGenericSignature(
            original->getGenericSignature(),
            config.derivativeGenericSignature)};
    std::string linearMapName =
        mangler.mangleAutoDiffLinearMap(original, kind, silConfig);

    // TODO: Can we encode a linear map function in a SILDeclRef instead of
    //       doing a bespoke mangling here?
    Visitor.addFunction(linearMapName, declRef);
  }

  void
  addAutoDiffDerivativeFunction(AbstractFunctionDecl *original,
                                IndexSubset *parameterIndices,
                                GenericSignature derivativeGenericSignature,
                                AutoDiffDerivativeFunctionKind kind) {
    auto *assocFnId = AutoDiffDerivativeFunctionIdentifier::get(
        kind, parameterIndices,
        autodiff::getDifferentiabilityWitnessGenericSignature(
            original->getGenericSignature(), derivativeGenericSignature),
        original->getASTContext());
    auto declRef =
        SILDeclRef(original).asForeign(requiresForeignEntryPoint(original));
    addFunction(declRef.asAutoDiffDerivativeFunction(assocFnId));
  }

  void addDifferentiabilityWitness(
      AbstractFunctionDecl *original, DifferentiabilityKind kind,
      IndexSubset *astParameterIndices, IndexSubset *resultIndices,
      GenericSignature derivativeGenericSignature) {
    bool foreign = requiresForeignEntryPoint(original);
    auto declRef = SILDeclRef(original).asForeign(foreign);

    // Skip symbol emission for original functions that do not have public
    // linkage. Exclude original functions that require a foreign entry point
    // with `public_external` linkage.
    auto originalLinkage = declRef.getLinkage(ForDefinition);
    if (foreign)
      originalLinkage = stripExternalFromLinkage(originalLinkage);
    if (Ctx.getOpts().PublicSymbolsOnly &&
        originalLinkage != SILLinkage::Public)
      return;

    auto *silParamIndices = autodiff::getLoweredParameterIndices(
        astParameterIndices,
        original->getInterfaceType()->castTo<AnyFunctionType>());

    auto originalMangledName = declRef.mangle();
    AutoDiffConfig config{
        silParamIndices, resultIndices,
        autodiff::getDifferentiabilityWitnessGenericSignature(
            original->getGenericSignature(), derivativeGenericSignature)};

    Mangle::ASTMangler mangler;
    auto mangledName = mangler.mangleSILDifferentiabilityWitness(
        originalMangledName, kind, config);

    // TODO: Can we encode a differentiability witness function in a SILDeclRef
    //       instead of doing a bespoke mangling here?
    Visitor.addFunction(mangledName, declRef);
  }

  void addDerivativeConfiguration(DifferentiabilityKind diffKind,
                                  AbstractFunctionDecl *original,
                                  const AutoDiffConfig &config) {
    auto inserted = AddedDerivatives.insert({original, config});
    if (!inserted.second)
      return;

    addAutoDiffLinearMapFunction(original, config,
                                 AutoDiffLinearMapKind::Differential);
    addAutoDiffLinearMapFunction(original, config,
                                 AutoDiffLinearMapKind::Pullback);
    addAutoDiffDerivativeFunction(original, config.parameterIndices,
                                  config.derivativeGenericSignature,
                                  AutoDiffDerivativeFunctionKind::JVP);
    addAutoDiffDerivativeFunction(original, config.parameterIndices,
                                  config.derivativeGenericSignature,
                                  AutoDiffDerivativeFunctionKind::VJP);
    addDifferentiabilityWitness(original, diffKind, config.parameterIndices,
                                config.resultIndices,
                                config.derivativeGenericSignature);
  }

  void addOpaqueResultIfNecessary(ValueDecl *VD) {
    if (auto opaqueResult = VD->getOpaqueResultTypeDecl()) {
      Visitor.addOpaqueTypeDescriptor(opaqueResult);
      assert(opaqueResult->getNamingDecl() == VD);
      if (auto dynKind = getDynamicKind(VD)) {
        Visitor.addOpaqueTypeDescriptorAccessor(opaqueResult, *dynKind);
      }
    }
  }

  void addConformances(const IterableDeclContext *IDC) {
    for (auto conformance :
         IDC->getLocalConformances(ConformanceLookupKind::NonInherited)) {
      if (conformance->getSourceKind() == ConformanceEntryKind::PreMacroExpansion)
        continue;

      auto protocol = conformance->getProtocol();
      if (Ctx.getOpts().PublicSymbolsOnly &&
          getDeclLinkage(protocol) != FormalLinkage::PublicUnique)
        continue;

      auto needsWTable =
          Lowering::TypeConverter::protocolRequiresWitnessTable(protocol);
      if (!needsWTable)
        continue;

      // Only root conformances get symbols; the others get any public symbols
      // from their parent conformances.
      auto rootConformance = dyn_cast<RootProtocolConformance>(conformance);
      if (!rootConformance) {
        continue;
      }

      // We cannot emit the witness table symbol if the protocol is imported
      // from another module and it's resilient, because initialization of that
      // protocol is necessary in this case
      if (!rootConformance->getProtocol()->isResilient(
              IDC->getAsGenericContext()->getParentModule(),
              ResilienceExpansion::Maximal))
        Visitor.addProtocolWitnessTable(rootConformance);
      Visitor.addProtocolConformanceDescriptor(rootConformance);

      // FIXME: the logic around visibility in extensions is confusing, and
      // sometimes witness thunks need to be manually made public.

      auto conformanceIsFixed =
          SILWitnessTable::conformanceIsSerialized(rootConformance);
      auto addSymbolIfNecessary = [&](ValueDecl *requirementDecl,
                                      ValueDecl *witnessDecl) {
        auto witnessRef = SILDeclRef(witnessDecl);
        if (Ctx.getOpts().PublicSymbolsOnly) {
          if (!conformanceIsFixed)
            return;

          if (!isa<SelfProtocolConformance>(rootConformance) &&
              !fixmeWitnessHasLinkageThatNeedsToBePublic(witnessRef)) {
            return;
          }
        }

        Visitor.addProtocolWitnessThunk(rootConformance, requirementDecl);
      };

      rootConformance->forEachValueWitness([&](ValueDecl *valueReq,
                                               Witness witness) {
        auto witnessDecl = witness.getDecl();
        if (isa<AbstractFunctionDecl>(valueReq)) {
          addSymbolIfNecessary(valueReq, witnessDecl);
        } else if (auto *storage = dyn_cast<AbstractStorageDecl>(valueReq)) {
          if (auto witnessStorage =
                  dyn_cast<AbstractStorageDecl>(witnessDecl)) {
            storage->visitOpaqueAccessors([&](AccessorDecl *reqtAccessor) {
              auto witnessAccessor = witnessStorage->getSynthesizedAccessor(
                  reqtAccessor->getAccessorKind());
              addSymbolIfNecessary(reqtAccessor, witnessAccessor);
            });
          } else if (isa<EnumElementDecl>(witnessDecl)) {
            auto getter = storage->getSynthesizedAccessor(AccessorKind::Get);
            addSymbolIfNecessary(getter, witnessDecl);
          }
        }
      });
    }
  }

  bool addClassMetadata(ClassDecl *CD) {
    if (Ctx.getOpts().PublicSymbolsOnly &&
        getDeclLinkage(CD) != FormalLinkage::PublicUnique)
      return false;

    auto &ctxt = CD->getASTContext();
    auto isGeneric = CD->isGenericContext();
    auto objCCompatible = ctxt.LangOpts.EnableObjCInterop && !isGeneric;
    auto isObjC = objCCompatible && CD->isObjC();

    // Metaclasses and ObjC classes (duh) are an ObjC thing, and so are not
    // needed in build artifacts/for classes which can't touch ObjC.
    if (objCCompatible) {
      if (isObjC || CD->getMetaclassKind() == ClassDecl::MetaclassKind::ObjC)
        Visitor.addObjCInterface(CD);
      else
        Visitor.addSwiftMetaclassStub(CD);
    }

    // Some members of classes get extra handling, beyond members of
    // struct/enums, so let's walk over them manually.
    if (Ctx.getOpts().VisitMembers)
      for (auto *var : CD->getStoredProperties())
        Visitor.addFieldOffset(var);

    visitNominalTypeDecl(CD);

    bool resilientAncestry = CD->checkAncestry(AncestryFlags::ResilientOther);

    // Types with resilient superclasses have some extra symbols.
    if (resilientAncestry || CD->hasResilientMetadata())
      Visitor.addClassMetadataBaseOffset(CD);

    auto &Ctx = CD->getASTContext();
    if (Ctx.LangOpts.EnableObjCInterop && resilientAncestry)
      Visitor.addObjCResilientClassStub(CD);

    return true;
  }

  void addMethodIfNecessary(FuncDecl *FD) {
    auto CD = dyn_cast<ClassDecl>(FD->getDeclContext());
    if (!CD)
      return;

    // If we're already visiting the parent ClassDecl then this was handled by
    // its vtable visitor.
    if (llvm::find(DeclStack, CD) != DeclStack.end())
      return;

    SILDeclRef method = SILDeclRef(FD);
    if (Ctx.getOpts().VirtualFunctionElimination ||
        CD->hasResilientMetadata()) {
      Visitor.addDispatchThunk(method);
    }
    Visitor.addMethodDescriptor(method);
  }

  void addRuntimeDiscoverableAttrGenerators(ValueDecl *D) {
    for (auto *attr : D->getRuntimeDiscoverableAttrs()) {
      addFunction(SILDeclRef::getRuntimeAttributeGenerator(attr, D),
                  /*ignoreLinkage=*/true);
    }
  }

public:
  SILSymbolVisitorImpl(SILSymbolVisitor &Visitor,
                       const SILSymbolVisitorContext &Ctx)
      : Visitor{Visitor}, Ctx{Ctx} {}

  void visit(Decl *D) {
    DeclStack.push_back(D);
    SWIFT_DEFER { DeclStack.pop_back(); };

    if (!Visitor.willVisitDecl(D))
      return;
    ASTVisitor::visit(D);
    Visitor.didVisitDecl(D);
  }

  void visit(FileUnit *file) {
    auto visitFile = [this](FileUnit *file) {
      SmallVector<Decl *, 16> decls;
      file->getTopLevelDeclsWithAuxiliaryDecls(decls);

      addMainIfNecessary(file);

      for (auto D : decls) {
        if (Ctx.getOpts().LinkerDirectivesOnly && !requiresLinkerDirective(D))
          continue;

        visit(D);
      }
    };

    visitFile(file);

    // Visit synthesized file, if it exists.
    if (auto *synthesizedFile = file->getSynthesizedFile())
      visitFile(synthesizedFile);
  }

  void visitDefaultArguments(ValueDecl *VD, ParameterList *PL) {
    auto moduleDecl = VD->getModuleContext();
    auto publicDefaultArgGenerators = moduleDecl->isTestingEnabled() ||
                                      moduleDecl->arePrivateImportsEnabled();
    if (Ctx.getOpts().PublicSymbolsOnly && !publicDefaultArgGenerators)
      return;

    // In Swift 3 (or under -enable-testing), default arguments (of public
    // functions) are public symbols, as the default values are computed at the
    // call site.
    auto index = 0;
    for (auto *param : *PL) {
      if (param->isDefaultArgument())
        addFunction(SILDeclRef::getDefaultArgGenerator(VD, index));
      ++index;
    }
  }

  void visitAbstractFunctionDecl(AbstractFunctionDecl *AFD) {
    // Add exported prespecialized symbols.
    for (auto *attr : AFD->getAttrs().getAttributes<SpecializeAttr>()) {
      if (!attr->isExported())
        continue;

      auto erasedSignature = attr->getSpecializedSignature().typeErased(
          attr->getTypeErasedParams());

      if (auto *targetFun = attr->getTargetFunctionDecl(AFD)) {
        addFunction(SILDeclRef(targetFun, erasedSignature),
                    /*ignoreLinkage=*/true);
      } else {
        addFunction(SILDeclRef(AFD, erasedSignature), /*ignoreLinkage=*/true);
      }
    }

    addFunction(SILDeclRef(AFD));

    if (auto dynKind = getDynamicKind(AFD)) {
      // Add the global function pointer for a dynamically replaceable function.
      Visitor.addDynamicFunction(AFD, *dynKind);
    }

    if (AFD->getAttrs().hasAttribute<CDeclAttr>()) {
      // A @_cdecl("...") function has an extra symbol, with the name from the
      // attribute.
      addFunction(SILDeclRef(AFD).asForeign());
    }

    if (auto distributedThunk = AFD->getDistributedThunk()) {
      auto thunk = SILDeclRef(distributedThunk).asDistributed();
      addFunction(thunk);
      addAsyncFunctionPointer(thunk);
    }

    // Add derivative function symbols.
    for (const auto *differentiableAttr :
           AFD->getAttrs().getAttributes<DifferentiableAttr>()) {
      auto *resultIndices = autodiff::getFunctionSemanticResultIndices(
        AFD,
        differentiableAttr->getParameterIndices());
      addDerivativeConfiguration(
          differentiableAttr->getDifferentiabilityKind(), AFD,
          AutoDiffConfig(differentiableAttr->getParameterIndices(),
                         resultIndices,
                         differentiableAttr->getDerivativeGenericSignature()));
    }

    for (const auto *derivativeAttr :
         AFD->getAttrs().getAttributes<DerivativeAttr>()) {
      auto *resultIndices = autodiff::getFunctionSemanticResultIndices(
        derivativeAttr->getOriginalFunction(AFD->getASTContext()),
        derivativeAttr->getParameterIndices());
      addDerivativeConfiguration(
          DifferentiabilityKind::Reverse,
          derivativeAttr->getOriginalFunction(AFD->getASTContext()),
          AutoDiffConfig(derivativeAttr->getParameterIndices(),
                         resultIndices,
                         AFD->getGenericSignature()));
    }

    addRuntimeDiscoverableAttrGenerators(AFD);

    visitDefaultArguments(AFD, AFD->getParameters());

    if (AFD->hasAsync()) {
      addAsyncFunctionPointer(SILDeclRef(AFD));
    }

    // Skip non objc compatible methods or non-public methods.
    if (isa<DestructorDecl>(AFD) || !AFD->isObjC() ||
        AFD->getFormalAccess() != AccessLevel::Public)
      return;
    Visitor.addObjCMethod(AFD);
  }

  void visitFuncDecl(FuncDecl *FD) {
    // If there's an opaque return type, its descriptor is exported.
    addOpaqueResultIfNecessary(FD);
    visitAbstractFunctionDecl(FD);
    addMethodIfNecessary(FD);
  }

  void visitAccessorDecl(AccessorDecl *AD) {
    llvm_unreachable("should not see an accessor here");
  }

  void visitAbstractStorageDecl(AbstractStorageDecl *ASD) {
    // Add the property descriptor if the decl needs it.
    if (ASD->exportsPropertyDescriptor()) {
      Visitor.addPropertyDescriptor(ASD);
    }

    // ...and the opaque result decl if it has one.
    addOpaqueResultIfNecessary(ASD);

    // Explicitly look at each accessor here: see visitAccessorDecl.
    ASD->visitEmittedAccessors([&](AccessorDecl *accessor) {
      visitFuncDecl(accessor);
    });

    // Add derivative function symbols.
    for (const auto *differentiableAttr :
         ASD->getAttrs().getAttributes<DifferentiableAttr>()) {
      // FIXME: handle other accessors
      auto accessorDecl = ASD->getOpaqueAccessor(AccessorKind::Get);
      addDerivativeConfiguration(
          differentiableAttr->getDifferentiabilityKind(),
          accessorDecl,
          AutoDiffConfig(differentiableAttr->getParameterIndices(),
                         autodiff::getFunctionSemanticResultIndices(accessorDecl,
                                                                    differentiableAttr->getParameterIndices()),
                         differentiableAttr->getDerivativeGenericSignature()));
    }
  }

  void visitVarDecl(VarDecl *VD) {
    // Variables inside non-resilient modules have some additional symbols.
    if (!VD->isResilient()) {
      // Non-global variables might have an explicit initializer symbol in
      // non-resilient modules.
      if (VD->getAttrs().hasAttribute<HasInitialValueAttr>() &&
          !isGlobalOrStaticVar(VD)) {
        auto declRef =
            SILDeclRef(VD, SILDeclRef::Kind::StoredPropertyInitializer);
        // Stored property initializers for public properties are public.
        addFunction(declRef);
      }

      // Statically/globally stored variables have some special handling.
      if (VD->hasStorage() && isGlobalOrStaticVar(VD)) {
        if (!Ctx.getOpts().PublicSymbolsOnly ||
            getDeclLinkage(VD) == FormalLinkage::PublicUnique) {
          Visitor.addGlobalVar(VD);
        }

        if (VD->isLazilyInitializedGlobal())
          addFunction(SILDeclRef(VD, SILDeclRef::Kind::GlobalAccessor));
      }

      // Wrapped non-static member properties may have a backing initializer.
      auto initInfo = VD->getPropertyWrapperInitializerInfo();
      if (initInfo.hasInitFromWrappedValue() && !VD->isStatic()) {
        addFunction(SILDeclRef(
            VD, SILDeclRef::Kind::PropertyWrapperBackingInitializer));
      }
    }

    visitAbstractStorageDecl(VD);

    addRuntimeDiscoverableAttrGenerators(VD);
  }

  void visitSubscriptDecl(SubscriptDecl *SD) {
    visitDefaultArguments(SD, SD->getIndices());
    visitAbstractStorageDecl(SD);
  }

  template<typename NominalOrExtension>
  void visitMembers(NominalOrExtension *D) {
    if (!Ctx.getOpts().VisitMembers)
      return;

    for (auto member : D->getMembers()) {
      member->visitAuxiliaryDecls([&](Decl *decl) {
        visit(decl);
      });

      visit(member);
    }
  }

  void visitNominalTypeDecl(NominalTypeDecl *NTD) {
    auto declaredType = NTD->getDeclaredType()->getCanonicalType();

    if (!NTD->getObjCImplementationDecl()) {
      Visitor.addNominalTypeDescriptor(NTD);

      // Generic types do not get metadata directly, only through the function.
      if (!NTD->isGenericContext()) {
        Visitor.addTypeMetadataAddress(declaredType);
      }
    }
    Visitor.addTypeMetadataAccessFunction(declaredType);

    // There are symbols associated with any protocols this type conforms to.
    addConformances(NTD);

    addRuntimeDiscoverableAttrGenerators(NTD);

    visitMembers(NTD);
  }

  void visitClassDecl(ClassDecl *CD) {
    if (!addClassMetadata(CD))
      return;

    // Emit dispatch thunks for every new vtable entry.
    struct VTableVisitor : public SILVTableVisitor<VTableVisitor> {
      SILSymbolVisitor &Visitor;
      ClassDecl *CD;
      bool FirstTime = true;
      bool VirtualFunctionElimination;

    public:
      VTableVisitor(SILSymbolVisitorImpl &V, ClassDecl *CD)
          : Visitor{V.Visitor}, CD{CD},
            VirtualFunctionElimination{
                V.Ctx.getOpts().VirtualFunctionElimination} {}

      void addMethod(SILDeclRef method) {
        assert(method.getDecl()->getDeclContext() == CD);

        if (VirtualFunctionElimination || CD->hasResilientMetadata()) {
          if (FirstTime) {
            FirstTime = false;

            // If the class is itself resilient and has at least one vtable
            // entry, it has a method lookup function.
            Visitor.addMethodLookupFunction(CD);
          }

          Visitor.addDispatchThunk(method);
        }

        Visitor.addMethodDescriptor(method);
      }

      void addMethodOverride(SILDeclRef baseRef, SILDeclRef derivedRef) {}

      void addPlaceholder(MissingMemberDecl *) {}

      void doIt() {
        addVTableEntries(CD);
      }
    };

    if (Ctx.getOpts().VisitMembers)
      VTableVisitor(*this, CD).doIt();
  }

  void visitConstructorDecl(ConstructorDecl *CD) {
    if (CD->getParent()->getSelfClassDecl()) {
      // Class constructors come in two forms, allocating and non-allocating.
      // The default ValueDecl handling gives the allocating one, so we have to
      // manually include the non-allocating one.
      addFunction(SILDeclRef(CD, SILDeclRef::Kind::Initializer));
      if (CD->hasAsync()) {
        addAsyncFunctionPointer(SILDeclRef(CD, SILDeclRef::Kind::Initializer));
      }
    }

    visitAbstractFunctionDecl(CD);
  }

  void visitDestructorDecl(DestructorDecl *DD) {
    // Destructors come in two forms (deallocating and non-deallocating), like
    // constructors above. Classes use both but move only non-class nominal
    // types only use the deallocating one. This is the deallocating one:
    visitAbstractFunctionDecl(DD);

    if (auto parentClass = DD->getParent()->getSelfClassDecl()) {
      // But the non-deallocating one doesn't apply to some @objc classes.
      if (!Lowering::usesObjCAllocator(parentClass)) {
        addFunction(SILDeclRef(DD, SILDeclRef::Kind::Destroyer));
      }
    }
  }

  void visitExtensionDecl(ExtensionDecl *ED) {
    if (auto CD = dyn_cast_or_null<ClassDecl>(ED->getImplementedObjCDecl())) {
      // @_objcImplementation extensions generate the class metadata symbols.
      (void)addClassMetadata(CD);
    }

    if (!isa<ProtocolDecl>(ED->getExtendedNominal())) {
      addConformances(ED);
    }

    visitMembers(ED);
  }

#ifndef NDEBUG
  static bool isExpectedProtocolMember(const Decl *D) {
    switch (D->getKind()) {
    case DeclKind::TypeAlias:
    case DeclKind::AssociatedType:
    case DeclKind::Var:
    case DeclKind::Subscript:
    case DeclKind::PatternBinding:
    case DeclKind::Func:
    case DeclKind::Accessor:
    case DeclKind::Constructor:
    case DeclKind::Destructor:
    case DeclKind::IfConfig:
    case DeclKind::PoundDiagnostic:
      return true;
    case DeclKind::OpaqueType:
    case DeclKind::Enum:
    case DeclKind::Struct:
    case DeclKind::Class:
    case DeclKind::Protocol:
    case DeclKind::GenericTypeParam:
    case DeclKind::Module:
    case DeclKind::Param:
    case DeclKind::EnumElement:
    case DeclKind::Extension:
    case DeclKind::TopLevelCode:
    case DeclKind::Import:
    case DeclKind::PrecedenceGroup:
    case DeclKind::MissingMember:
    case DeclKind::EnumCase:
    case DeclKind::InfixOperator:
    case DeclKind::PrefixOperator:
    case DeclKind::PostfixOperator:
    case DeclKind::Macro:
    case DeclKind::MacroExpansion:
      return false;
    case DeclKind::Missing:
      llvm_unreachable("missing decl should not show up here");
    case DeclKind::BuiltinTuple:
      llvm_unreachable("BuiltinTupleDecl should not show up here");
    }
    llvm_unreachable("covered switch");
  }
#endif

  void visitProtocolDecl(ProtocolDecl *PD) {
    if (!PD->isObjC() && !PD->isMarkerProtocol()) {
      Visitor.addProtocolDescriptor(PD);

      struct WitnessVisitor : public SILWitnessVisitor<WitnessVisitor> {
        SILSymbolVisitor &Visitor;
        ProtocolDecl *PD;
        bool Resilient;
        bool WitnessMethodElimination;

      public:
        WitnessVisitor(SILSymbolVisitorImpl &V, ProtocolDecl *PD)
            : Visitor{V.Visitor}, PD{PD},
              Resilient{PD->getParentModule()->isResilient()},
              WitnessMethodElimination{
                  V.Ctx.getOpts().WitnessMethodElimination} {}

        void addMethod(SILDeclRef declRef) {
          if (Resilient || WitnessMethodElimination) {
            Visitor.addDispatchThunk(declRef);
            Visitor.addMethodDescriptor(declRef);
          }
        }

        void addAssociatedType(AssociatedType associatedType) {
          Visitor.addAssociatedTypeDescriptor(associatedType.getAssociation());
        }

        void addProtocolConformanceDescriptor() {
          Visitor.addProtocolRequirementsBaseDescriptor(PD);
        }

        void addOutOfLineBaseProtocol(ProtocolDecl *proto) {
          Visitor.addBaseConformanceDescriptor(BaseConformance(PD, proto));
        }

        void addAssociatedConformance(AssociatedConformance associatedConf) {
          Visitor.addAssociatedConformanceDescriptor(associatedConf);
        }

        void addPlaceholder(MissingMemberDecl *decl) {}

        void doIt() {
          visitProtocolDecl(PD);
        }
      };

      WitnessVisitor(*this, PD).doIt();

      // Include the self-conformance.
      addConformances(PD);
    }

#ifndef NDEBUG
    // There are currently no symbols associated with the members of a protocol;
    // each conforming type has to handle them individually.
    // (NB. anything within an active IfConfigDecls also appears outside). Let's
    // assert this fact:
    for (auto *member : PD->getMembers()) {
      assert(isExpectedProtocolMember(member) &&
             "unexpected member of protocol during TBD generation");
    }
#endif
  }

  void visitEnumDecl(EnumDecl *ED) {
    visitNominalTypeDecl(ED);
  }

  void visitEnumElementDecl(EnumElementDecl *EED) {
    if (EED->getParentEnum()->isResilient())
      Visitor.addEnumCase(EED);

    if (auto *PL = EED->getParameterList())
      visitDefaultArguments(EED, PL);
  }

#define UNINTERESTING_DECL(CLASS)                                              \
  void visit##CLASS##Decl(CLASS##Decl *) {}

  UNINTERESTING_DECL(EnumCase)
  UNINTERESTING_DECL(IfConfig)
  UNINTERESTING_DECL(Import)
  UNINTERESTING_DECL(MacroExpansion)
  UNINTERESTING_DECL(Missing)
  UNINTERESTING_DECL(MissingMember)
  UNINTERESTING_DECL(Operator)
  UNINTERESTING_DECL(PatternBinding)
  UNINTERESTING_DECL(PoundDiagnostic)
  UNINTERESTING_DECL(PrecedenceGroup)
  UNINTERESTING_DECL(TopLevelCode)
  UNINTERESTING_DECL(Value)

#undef UNINTERESTING_DECL
};

void SILSymbolVisitor::visitDecl(Decl *D, const SILSymbolVisitorContext &ctx) {
  SILSymbolVisitorImpl(*this, ctx).visit(D);
}

void SILSymbolVisitor::visitFile(FileUnit *file,
                                 const SILSymbolVisitorContext &ctx) {
  SILSymbolVisitorImpl(*this, ctx).visit(file);
}

void SILSymbolVisitor::visitModules(llvm::SmallVector<ModuleDecl *, 4> &modules,
                                    const SILSymbolVisitorContext &ctx) {
  auto impl = SILSymbolVisitorImpl(*this, ctx);
  for (auto *M : modules) {
    for (auto *file : M->getFiles()) {
      impl.visit(file);
    }
  }
}
