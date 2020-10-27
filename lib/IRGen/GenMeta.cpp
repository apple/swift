//===--- GenMeta.cpp - IR generation for metadata constructs --------------===//
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
//  This file implements IR generation for type metadata constructs.
//
//===----------------------------------------------------------------------===//

#define DEBUG_TYPE "type-metadata-layout"

#include "swift/ABI/MetadataValues.h"
#include "swift/ABI/TypeIdentity.h"
#include "swift/AST/ASTContext.h"
#include "swift/AST/ASTMangler.h"
#include "swift/AST/Attr.h"
#include "swift/AST/CanTypeVisitor.h"
#include "swift/AST/Decl.h"
#include "swift/AST/GenericEnvironment.h"
#include "swift/AST/IRGenOptions.h"
#include "swift/AST/PrettyStackTrace.h"
#include "swift/AST/SubstitutionMap.h"
#include "swift/AST/Types.h"
#include "swift/ClangImporter/ClangModule.h"
#include "swift/IRGen/Linking.h"
#include "swift/SIL/FormalLinkage.h"
#include "swift/SIL/SILModule.h"
#include "swift/SIL/TypeLowering.h"
#include "swift/Strings.h"
#include "clang/AST/Decl.h"
#include "clang/AST/DeclObjC.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/GlobalVariable.h"
#include "llvm/IR/Module.h"

#include "Address.h"
#include "Callee.h"
#include "ClassLayout.h"
#include "ClassMetadataVisitor.h"
#include "ClassTypeInfo.h"
#include "ConstantBuilder.h"
#include "EnumMetadataVisitor.h"
#include "FixedTypeInfo.h"
#include "ForeignClassMetadataVisitor.h"
#include "GenArchetype.h"
#include "GenClass.h"
#include "GenDecl.h"
#include "GenPointerAuth.h"
#include "GenPoly.h"
#include "GenStruct.h"
#include "GenValueWitness.h"
#include "GenericArguments.h"
#include "HeapTypeInfo.h"
#include "IRGenDebugInfo.h"
#include "IRGenMangler.h"
#include "IRGenModule.h"
#include "MetadataLayout.h"
#include "MetadataRequest.h"
#include "ProtocolInfo.h"
#include "ScalarTypeInfo.h"
#include "StructLayout.h"
#include "StructMetadataVisitor.h"

#include "GenMeta.h"

using namespace swift;
using namespace irgen;

static Address emitAddressOfMetadataSlotAtIndex(IRGenFunction &IGF,
                                                llvm::Value *metadata,
                                                int index,
                                                llvm::Type *objectTy) {
  // Require the metadata to be some type that we recognize as a
  // metadata pointer.
  assert(metadata->getType() == IGF.IGM.TypeMetadataPtrTy);

  return IGF.emitAddressAtOffset(metadata,
                                 Offset(index * IGF.IGM.getPointerSize()),
                                 objectTy, IGF.IGM.getPointerAlignment());
}

/// Emit a load from the given metadata at a constant index.
static llvm::LoadInst *emitLoadFromMetadataAtIndex(IRGenFunction &IGF,
                                                   llvm::Value *metadata,
                                                   llvm::Value **slotPtr,
                                                   int index,
                                                   llvm::Type *objectTy,
                                             const llvm::Twine &suffix = "") {
  Address slot =
    emitAddressOfMetadataSlotAtIndex(IGF, metadata, index, objectTy);
  if (slotPtr) *slotPtr = slot.getAddress();

  // Load.
  return IGF.Builder.CreateLoad(slot, metadata->getName() + suffix);
}

static Address createPointerSizedGEP(IRGenFunction &IGF,
                                     Address base,
                                     Size offset) {
  return IGF.Builder.CreateConstArrayGEP(base,
                                         IGF.IGM.getOffsetInWords(offset),
                                         offset);
}

void IRGenModule::setTrueConstGlobal(llvm::GlobalVariable *var) {
  disableAddressSanitizer(*this, var);
  
  switch (TargetInfo.OutputObjectFormat) {
  case llvm::Triple::UnknownObjectFormat:
    llvm_unreachable("unknown object format");
  case llvm::Triple::MachO:
    var->setSection("__TEXT,__const");
    break;
  case llvm::Triple::ELF:
  case llvm::Triple::Wasm:
    var->setSection(".rodata");
    break;
  case llvm::Triple::XCOFF:
  case llvm::Triple::COFF:
    var->setSection(".rdata");
    break;
  }
}

/*****************************************************************************/
/** Metadata completion ******************************************************/
/*****************************************************************************/

/// Does the metadata for the given type, which we are currently emitting,
/// require singleton metadata initialization structures and functions?
static bool needsSingletonMetadataInitialization(IRGenModule &IGM,
                                                 NominalTypeDecl *typeDecl) {
  // Generic types never have singleton metadata initialization.
  if (typeDecl->isGenericContext())
    return false;

  // Non-generic classes use singleton initialization if they have anything
  // non-trivial about their metadata.
  if (auto *classDecl = dyn_cast<ClassDecl>(typeDecl)) {
    switch (IGM.getClassMetadataStrategy(classDecl)) {
    case ClassMetadataStrategy::Resilient:
    case ClassMetadataStrategy::Singleton:
    case ClassMetadataStrategy::Update:
    case ClassMetadataStrategy::FixedOrUpdate:
      return true;
    case ClassMetadataStrategy::Fixed:
      return false;
    }
  }

  assert(isa<StructDecl>(typeDecl) || isa<EnumDecl>(typeDecl));

  // If the type is known to be fixed-layout, we can emit its metadata such
  // that it doesn't need dynamic initialization.
  auto &ti = IGM.getTypeInfoForUnlowered(typeDecl->getDeclaredTypeInContext());
  if (ti.isFixedSize(ResilienceExpansion::Maximal))
    return false;

  return true;
}

using MetadataCompletionBodyEmitter =
  void (IRGenFunction &IGF,
        llvm::Value *metadata,
        MetadataDependencyCollector *collector);

static void emitMetadataCompletionFunction(IRGenModule &IGM,
                                           NominalTypeDecl *typeDecl,
                       llvm::function_ref<MetadataCompletionBodyEmitter> body) {
  llvm::Function *f =
    IGM.getAddrOfTypeMetadataCompletionFunction(typeDecl, ForDefinition);
  f->setAttributes(IGM.constructInitialAttributes());
  f->setDoesNotThrow();
  IGM.setHasNoFramePointer(f);

  IRGenFunction IGF(IGM, f);

  // Skip instrumentation when building for TSan to avoid false positives.
  // The synchronization for this happens in the Runtime and we do not see it.
  if (IGM.IRGen.Opts.Sanitizers & SanitizerKind::Thread)
    f->removeFnAttr(llvm::Attribute::SanitizeThread);

  if (IGM.DebugInfo)
    IGM.DebugInfo->emitArtificialFunction(IGF, f);

  Explosion params = IGF.collectParameters();
  llvm::Value *metadata = params.claimNext();
  llvm::Value *context = params.claimNext();
  llvm::Value *templatePointer = params.claimNext();

  // TODO: use these?
  (void) context;
  (void) templatePointer;

  MetadataDependencyCollector collector;

  body(IGF, metadata, &collector);

  // At the current insertion point, the metadata is now complete.

  // Merge with any metadata dependencies we may have collected.
  auto dependency = collector.finish(IGF);
  auto returnValue = dependency.combine(IGF);

  IGF.Builder.CreateRet(returnValue);
}

static bool needsForeignMetadataCompletionFunction(IRGenModule &IGM,
                                                   StructDecl *decl) {
  // Currently, foreign structs never need a completion function.
  return false;
}

static bool needsForeignMetadataCompletionFunction(IRGenModule &IGM,
                                                   EnumDecl *decl) {
  // Currently, foreign enums never need a completion function.
  return false;
}

static bool needsForeignMetadataCompletionFunction(IRGenModule &IGM,
                                                   ClassDecl *decl) {
  return IGM.getOptions().LazyInitializeClassMetadata || decl->hasSuperclass();
}

/*****************************************************************************/
/** Nominal Type Descriptor Emission *****************************************/
/*****************************************************************************/

template <class Flags>
static Flags getMethodDescriptorFlags(ValueDecl *fn) {
  if (isa<ConstructorDecl>(fn))
    return Flags(Flags::Kind::Init); // 'init' is considered static

  auto kind = [&] {
    auto accessor = dyn_cast<AccessorDecl>(fn);
    if (!accessor) return Flags::Kind::Method;
    switch (accessor->getAccessorKind()) {
    case AccessorKind::Get:
      return Flags::Kind::Getter;
    case AccessorKind::Set:
      return Flags::Kind::Setter;
    case AccessorKind::Read:
      return Flags::Kind::ReadCoroutine;
    case AccessorKind::Modify:
      return Flags::Kind::ModifyCoroutine;
#define OPAQUE_ACCESSOR(ID, KEYWORD)
#define ACCESSOR(ID) \
    case AccessorKind::ID:
#include "swift/AST/AccessorKinds.def"
      llvm_unreachable("these accessors never appear in protocols or v-tables");
    }
    llvm_unreachable("bad kind");
  }();
  return Flags(kind).withIsInstance(!fn->isStatic());
}

static void buildMethodDescriptorFields(IRGenModule &IGM,
                             const SILVTable *VTable,
                             SILDeclRef fn,
                             ConstantStructBuilder &descriptor) {
  auto *func = cast<AbstractFunctionDecl>(fn.getDecl());
  // Classify the method.
  using Flags = MethodDescriptorFlags;
  auto flags = getMethodDescriptorFlags<Flags>(func);

  // Remember if the declaration was dynamic.
  if (func->shouldUseObjCDispatch())
    flags = flags.withIsDynamic(true);

  // Include the pointer-auth discriminator.
  if (auto &schema = IGM.getOptions().PointerAuth.SwiftClassMethods) {
    auto discriminator =
      PointerAuthInfo::getOtherDiscriminator(IGM, schema, fn);
    flags = flags.withExtraDiscriminator(discriminator->getZExtValue());
  }

  // TODO: final? open?
  descriptor.addInt(IGM.Int32Ty, flags.getIntValue());

  if (auto entry = VTable->getEntry(IGM.getSILModule(), fn)) {
    assert(entry->getKind() == SILVTable::Entry::Kind::Normal);
    auto *implFn = IGM.getAddrOfSILFunction(entry->getImplementation(),
                                            NotForDefinition);
    descriptor.addRelativeAddress(implFn);
  } else {
    // The method is removed by dead method elimination.
    // It should be never called. We add a pointer to an error function.
    descriptor.addRelativeAddressOrNull(nullptr);
  }
}

void IRGenModule::emitNonoverriddenMethodDescriptor(const SILVTable *VTable,
                                                    SILDeclRef declRef) {
  auto entity = LinkEntity::forMethodDescriptor(declRef);
  auto *var = cast<llvm::GlobalVariable>(getAddrOfLLVMVariable(entity, ConstantInit(), DebugTypeInfo()));
  if (!var->isDeclaration()) {
    assert(IRGen.isLazilyReemittingNominalTypeDescriptor(VTable->getClass()));
    return;
  }

  ConstantInitBuilder ib(*this);
  ConstantStructBuilder sb(ib.beginStruct(MethodDescriptorStructTy));

  buildMethodDescriptorFields(*this, VTable, declRef, sb);
  
  auto init = sb.finishAndCreateFuture();
  
  getAddrOfLLVMVariable(entity, init, DebugTypeInfo());
}

namespace {
  template<class Impl>
  class ContextDescriptorBuilderBase {
  protected:
    Impl &asImpl() { return *static_cast<Impl*>(this); }
    IRGenModule &IGM;
  private:
    ConstantInitBuilder InitBuilder;
  protected:
    ConstantStructBuilder B;
    Optional<ConstantAggregateBuilderBase::PlaceholderPosition>
      GenericParamCount,
      GenericRequirementCount,
      GenericKeyArgumentCount,
      GenericExtraArgumentCount;
    unsigned NumGenericKeyArguments = 0;
    unsigned NumGenericExtraArguments = 0;

    ContextDescriptorBuilderBase(IRGenModule &IGM)
      : IGM(IGM), InitBuilder(IGM), B(InitBuilder.beginStruct()) {
      B.setPacked(true);
    }
  
  public:
    void layout() {
      asImpl().addFlags();
      asImpl().addParent();
    }
    
    void addFlags() {
      B.addInt32(
        ContextDescriptorFlags(asImpl().getContextKind(),
                               !asImpl().getGenericSignature().isNull(),
                               asImpl().isUniqueDescriptor(),
                               asImpl().getVersion(),
                               asImpl().getKindSpecificFlags())
          .getIntValue());
    }
    
    void addParent() {
      ConstantReference parent = asImpl().getParent();
      if (parent.getValue()) {
        B.addRelativeAddress(parent);
      } else {
        B.addInt32(0); // null offset
      }
    }
    
    void addGenericSignature() {
      if (!asImpl().getGenericSignature())
        return;
      
      asImpl().addGenericParametersHeader();
      asImpl().addGenericParameters();
      asImpl().addGenericRequirements();
      asImpl().finishGenericParameters();
    }
    
    void addGenericParametersHeader() {
      // Drop placeholders for the counts. We'll fill these in when we emit
      // the related sections.
      GenericParamCount = B.addPlaceholderWithSize(IGM.Int16Ty);
      GenericRequirementCount = B.addPlaceholderWithSize(IGM.Int16Ty);
      GenericKeyArgumentCount = B.addPlaceholderWithSize(IGM.Int16Ty);
      GenericExtraArgumentCount = B.addPlaceholderWithSize(IGM.Int16Ty);
    }
    
    void addGenericParameters() {
      GenericSignature sig = asImpl().getGenericSignature();
      assert(sig);
      auto canSig = sig.getCanonicalSignature();

      canSig->forEachParam([&](GenericTypeParamType *param, bool canonical) {
        // Currently, there are only type parameters. The parameter is a key
        // argument if it's canonical in its generic context.
        asImpl().addGenericParameter(GenericParamKind::Type,
                                     /*key argument*/ canonical,
                                     /*extra argument*/ false);
      });

      // Pad the structure up to four bytes for the following requirements.
      unsigned padding = (unsigned) -canSig->getGenericParams().size() & 3;
      for (unsigned i = 0; i < padding; ++i)
        B.addInt(IGM.Int8Ty, 0);
      
      // Fill in the parameter count.
      assert(canSig->getGenericParams().size() <= UINT16_MAX
             && "way too generic");
      B.fillPlaceholderWithInt(*GenericParamCount, IGM.Int16Ty,
                               canSig->getGenericParams().size());
    }
    
    void addGenericParameter(GenericParamKind kind,
                             bool isKeyArgument, bool isExtraArgument) {
      if (isKeyArgument)
        ++NumGenericKeyArguments;
      if (isExtraArgument)
        ++NumGenericExtraArguments;
      
      B.addInt(IGM.Int8Ty,
               GenericParamDescriptor(kind, isKeyArgument, isExtraArgument)
                 .getIntValue());
    }
    
    void addGenericRequirements() {
      auto metadata =
        irgen::addGenericRequirements(IGM, B,
                            asImpl().getGenericSignature(),
                            asImpl().getGenericSignature()->getRequirements());

      // Fill in the final requirement count.
      assert(metadata.NumRequirements <= UINT16_MAX
             && "way too generic");
      B.fillPlaceholderWithInt(*GenericRequirementCount, IGM.Int16Ty,
                               metadata.NumRequirements);
      NumGenericKeyArguments += metadata.NumGenericKeyArguments;
      NumGenericExtraArguments += metadata.NumGenericExtraArguments;
    }

    void finishGenericParameters() {
      assert(NumGenericKeyArguments <= UINT16_MAX
             && NumGenericExtraArguments <= UINT16_MAX
             && "way too generic");
      B.fillPlaceholderWithInt(*GenericKeyArgumentCount, IGM.Int16Ty,
                               NumGenericKeyArguments);
      B.fillPlaceholderWithInt(*GenericExtraArgumentCount, IGM.Int16Ty,
                               NumGenericExtraArguments);
    }

    uint8_t getVersion() {
      return 0;
    }
    
    uint16_t getKindSpecificFlags() {
      return 0;
    }
    
    // Subclasses should provide:
    //
    // bool isUniqueDescriptor();
    // llvm::Constant *getParent();
    // ContextDescriptorKind getContextKind();
    // GenericSignature getGenericSignature();
    // void emit();
  };
  
  class ModuleContextDescriptorBuilder
      : public ContextDescriptorBuilderBase<ModuleContextDescriptorBuilder> {
    using super = ContextDescriptorBuilderBase;
    
    ModuleDecl *M;
    
  public:
    ModuleContextDescriptorBuilder(IRGenModule &IGM, ModuleDecl *M)
      : super(IGM), M(M)
    {}
  
    void layout() {
      super::layout();
      addName();
    }
    
    void addName() {
      B.addRelativeAddress(IGM.getAddrOfGlobalString(M->getName().str(),
                                           /*willBeRelativelyAddressed*/ true));
    }
    
    bool isUniqueDescriptor() {
      return false;
    }
  
    ConstantReference getParent() {
      return {nullptr, ConstantReference::Direct};
    }
    
    ContextDescriptorKind getContextKind() {
      return ContextDescriptorKind::Module;
    }
    
    GenericSignature getGenericSignature() {
      return nullptr;
    }
        
    void emit() {
      asImpl().layout();
      
      auto addr = IGM.getAddrOfModuleContextDescriptor(M,
                                                     B.finishAndCreateFuture());
      auto var = cast<llvm::GlobalVariable>(addr);
      
      var->setConstant(true);
      IGM.setTrueConstGlobal(var);
    }
  };

  class ExtensionContextDescriptorBuilder
    : public ContextDescriptorBuilderBase<ExtensionContextDescriptorBuilder> {
    
    using super = ContextDescriptorBuilderBase;
    
    ExtensionDecl *E;
  
  public:
    ExtensionContextDescriptorBuilder(IRGenModule &IGM, ExtensionDecl *E)
      : super(IGM), E(E)
    {}
    
    void layout() {
      super::layout();
      addExtendedContext();
      addGenericSignature();
    }
    
    void addExtendedContext() {
      auto string = IGM.getTypeRef(E->getSelfInterfaceType(),
                                   E->getGenericSignature(),
                                   MangledTypeRefRole::Metadata).first;
      B.addRelativeAddress(string);
    }
    
    ConstantReference getParent() {
      return {IGM.getAddrOfModuleContextDescriptor(E->getParentModule()),
              ConstantReference::Direct};
    }
    
    bool isUniqueDescriptor() {
      // Extensions generated by the Clang importer will be emitted into any
      // binary that uses the Clang module. Otherwise, we can guarantee that
      // an extension (and any of its possible sub-contexts) belong to one
      // translation unit.
      return !isa<ClangModuleUnit>(E->getModuleScopeContext());
    }
    
    ContextDescriptorKind getContextKind() {
      return ContextDescriptorKind::Extension;
    }
    
    GenericSignature getGenericSignature() {
      return E->getGenericSignature();
    }
      
    void emit() {
      asImpl().layout();
      
      auto addr = IGM.getAddrOfExtensionContextDescriptor(E,
                                                     B.finishAndCreateFuture());
      auto var = cast<llvm::GlobalVariable>(addr);
      
      var->setConstant(true);
      IGM.setTrueConstGlobal(var);
    }
  };
  
  class AnonymousContextDescriptorBuilder
    : public ContextDescriptorBuilderBase<AnonymousContextDescriptorBuilder> {
    
    using super = ContextDescriptorBuilderBase;
    
    PointerUnion<DeclContext *, VarDecl *> Name;
  
    DeclContext *getInnermostDeclContext() {
      if (auto DC = Name.dyn_cast<DeclContext *>()) {
        return DC;
      }
      if (auto VD = Name.dyn_cast<VarDecl *>()) {
        return VD->getInnermostDeclContext();
      }
      llvm_unreachable("unknown name kind");
    }
      
  public:
    AnonymousContextDescriptorBuilder(IRGenModule &IGM,
                                    PointerUnion<DeclContext *, VarDecl *> Name)
      : super(IGM), Name(Name)
    {
    }
    
    void layout() {
      super::layout();
      asImpl().addGenericSignature();
      asImpl().addMangledName();
    }
  
    ConstantReference getParent() {
      return IGM.getAddrOfParentContextDescriptor(
               getInnermostDeclContext(), /*fromAnonymousContext=*/true);
    }
    
    ContextDescriptorKind getContextKind() {
      return ContextDescriptorKind::Anonymous;
    }
    
    GenericSignature getGenericSignature() {
      return getInnermostDeclContext()->getGenericSignatureOfContext();
    }
    
    bool isUniqueDescriptor() {
      return true;
    }

    uint16_t getKindSpecificFlags() {
      AnonymousContextDescriptorFlags flags{};
      flags.setHasMangledName(
        IGM.IRGen.Opts.EnableAnonymousContextMangledNames);

      return flags.getOpaqueValue();
    }

    void addMangledName() {
      if (!IGM.IRGen.Opts.EnableAnonymousContextMangledNames)
        return;

      IRGenMangler mangler;
      auto mangledName = mangler.mangleAnonymousDescriptorName(Name);
      auto mangledNameConstant =
        IGM.getAddrOfGlobalString(mangledName,
                                  /*willBeRelativelyAddressed*/ true);
      B.addRelativeAddress(mangledNameConstant);
    }

    void emit() {
      asImpl().layout();
      auto addr = IGM.getAddrOfAnonymousContextDescriptor(Name,
                                                     B.finishAndCreateFuture());
      auto var = cast<llvm::GlobalVariable>(addr);
      
      var->setConstant(true);
      IGM.setTrueConstGlobal(var);
    }
  };

  class ProtocolDescriptorBuilder
    : public ContextDescriptorBuilderBase<ProtocolDescriptorBuilder> {

    using super = ContextDescriptorBuilderBase;

    ProtocolDecl *Proto;
    SILDefaultWitnessTable *DefaultWitnesses;

    Optional<ConstantAggregateBuilderBase::PlaceholderPosition>
      NumRequirementsInSignature,
      NumRequirements;

    bool Resilient;

  public:
    ProtocolDescriptorBuilder(IRGenModule &IGM, ProtocolDecl *Proto,
                                     SILDefaultWitnessTable *defaultWitnesses)
      : super(IGM), Proto(Proto), DefaultWitnesses(defaultWitnesses),
        Resilient(IGM.getSwiftModule()->isResilient()) {}

    void layout() {
      super::layout();
    }

    ConstantReference getParent() {
      return IGM.getAddrOfParentContextDescriptor(
               Proto, /*fromAnonymousContext=*/false);
    }

    ContextDescriptorKind getContextKind() {
      return ContextDescriptorKind::Protocol;
    }

    GenericSignature getGenericSignature() {
      return nullptr;
    }

    bool isUniqueDescriptor() {
      return true;
    }

    uint16_t getKindSpecificFlags() {
      ProtocolContextDescriptorFlags flags;
      flags.setClassConstraint(Proto->requiresClass()
                                 ? ProtocolClassConstraint::Class
                                 : ProtocolClassConstraint::Any);
      flags.setSpecialProtocol(getSpecialProtocolID(Proto));
      flags.setIsResilient(DefaultWitnesses != nullptr);
      return flags.getOpaqueValue();
    }

    void emit() {
      asImpl().layout();
      asImpl().addName();
      NumRequirementsInSignature = B.addPlaceholderWithSize(IGM.Int32Ty);
      NumRequirements = B.addPlaceholderWithSize(IGM.Int32Ty);
      asImpl().addAssociatedTypeNames();
      asImpl().addRequirementSignature();
      asImpl().addRequirements();
      auto addr = IGM.getAddrOfProtocolDescriptor(Proto,
                                                  B.finishAndCreateFuture());
      auto var = cast<llvm::GlobalVariable>(addr);

      var->setConstant(true);
      IGM.setTrueConstGlobal(var);
    }

    void addName() {
      auto nameStr = IGM.getAddrOfGlobalString(Proto->getName().str(),
                                           /*willBeRelativelyAddressed*/ true);
      B.addRelativeAddress(nameStr);
    }

    void addRequirementSignature() {
      auto metadata =
        irgen::addGenericRequirements(IGM, B, Proto->getGenericSignature(),
                                      Proto->getRequirementSignature());

      B.fillPlaceholderWithInt(*NumRequirementsInSignature, IGM.Int32Ty,
                               metadata.NumRequirements);
    }

    struct RequirementInfo {
      ProtocolRequirementFlags Flags;
      llvm::Constant *DefaultImpl;
    };

    /// Build the information which will go into a ProtocolRequirement entry.
    RequirementInfo getRequirementInfo(const WitnessTableEntry &entry) {
      using Flags = ProtocolRequirementFlags;
      if (entry.isBase()) {
        assert(entry.isOutOfLineBase());
        auto flags = Flags(Flags::Kind::BaseProtocol);
        return { flags, nullptr };
      }

      if (entry.isAssociatedType()) {
        auto flags = Flags(Flags::Kind::AssociatedTypeAccessFunction);
        if (auto &schema = IGM.getOptions().PointerAuth
                              .ProtocolAssociatedTypeAccessFunctions) {
          addDiscriminator(flags, schema,
                           AssociatedType(entry.getAssociatedType()));
        }

        // Look for a default witness.
        llvm::Constant *defaultImpl =
          findDefaultTypeWitness(entry.getAssociatedType());

        return { flags, defaultImpl };
      }

      if (entry.isAssociatedConformance()) {
        auto flags = Flags(Flags::Kind::AssociatedConformanceAccessFunction);
        if (auto &schema = IGM.getOptions().PointerAuth
                           .ProtocolAssociatedTypeWitnessTableAccessFunctions) {
          addDiscriminator(flags, schema,
                           AssociatedConformance(Proto,
                                 entry.getAssociatedConformancePath(),
                                 entry.getAssociatedConformanceRequirement()));
        }

        // Look for a default witness.
        llvm::Constant *defaultImpl =
          findDefaultAssociatedConformanceWitness(
            entry.getAssociatedConformancePath(),
            entry.getAssociatedConformanceRequirement());

        return { flags, defaultImpl };
      }

      assert(entry.isFunction());
      SILDeclRef func(entry.getFunction());

      // Emit the dispatch thunk.
      if (Resilient)
        IGM.emitDispatchThunk(func);

      // Classify the function.
      auto flags = getMethodDescriptorFlags<Flags>(func.getDecl());

      if (auto &schema = IGM.getOptions().PointerAuth.ProtocolWitnesses) {
        SILDeclRef declRef(func.getDecl(),
                           isa<ConstructorDecl>(func.getDecl())
                             ? SILDeclRef::Kind::Allocator
                             : SILDeclRef::Kind::Func);
        if (entry.getFunction().isAutoDiffDerivativeFunction())
          declRef = declRef.asAutoDiffDerivativeFunction(
              entry.getFunction().getAutoDiffDerivativeFunctionIdentifier());
        addDiscriminator(flags, schema, declRef);
      }

      // Look for a default witness.
      llvm::Constant *defaultImpl = findDefaultWitness(func);

      return { flags, defaultImpl };
    }

    void addDiscriminator(ProtocolRequirementFlags &flags,
                          const PointerAuthSchema &schema,
                          const PointerAuthEntity &entity) {
      assert(schema);
      auto discriminator =
        PointerAuthInfo::getOtherDiscriminator(IGM, schema, entity);
      flags = flags.withExtraDiscriminator(discriminator->getZExtValue());
    }

    void addRequirements() {
      auto &pi = IGM.getProtocolInfo(Proto, ProtocolInfoKind::Full);

      B.fillPlaceholderWithInt(*NumRequirements, IGM.Int32Ty,
                               pi.getNumWitnesses());

      if (pi.getNumWitnesses() > 0) {
        // Define the protocol requirements "base" descriptor, which references
        // the beginning of the protocol requirements, offset so that
        // subtracting this address from the address of a given protocol
        // requirements gives the corresponding offset into the witness
        // table.
        auto address =
          B.getAddrOfCurrentPosition(IGM.ProtocolRequirementStructTy);
        int offset = WitnessTableFirstRequirementOffset;
        auto firstReqAdjustment = llvm::ConstantInt::get(IGM.Int32Ty, -offset);
        address = llvm::ConstantExpr::getGetElementPtr(nullptr, address,
                                                       firstReqAdjustment);

        IGM.defineProtocolRequirementsBaseDescriptor(Proto, address);
      }

      for (auto &entry : pi.getWitnessEntries()) {
        if (Resilient) {
          if (entry.isFunction()) {
            // Define the method descriptor.
            SILDeclRef func(entry.getFunction());
            auto *descriptor =
              B.getAddrOfCurrentPosition(
                IGM.ProtocolRequirementStructTy);
            IGM.defineMethodDescriptor(func, Proto, descriptor);
          }
        }

        if (entry.isAssociatedType()) {
          auto assocType = entry.getAssociatedType();
          // Define the associated type descriptor to point to the current
          // position in the protocol descriptor.
          IGM.defineAssociatedTypeDescriptor(
              assocType,
              B.getAddrOfCurrentPosition(IGM.ProtocolRequirementStructTy));
        }

        if (entry.isAssociatedConformance()) {
          // Define the associated conformance descriptor to point to the
          // current position in the protocol descriptor.
          AssociatedConformance conformance(
                                  Proto,
                                  entry.getAssociatedConformancePath(),
                                  entry.getAssociatedConformanceRequirement());
          IGM.defineAssociatedConformanceDescriptor(
              conformance,
              B.getAddrOfCurrentPosition(IGM.ProtocolRequirementStructTy));
        }

        if (entry.isBase()) {
          // Define a base conformance descriptor, which is just an associated
          // conformance descriptor for a base protocol.
          BaseConformance conformance(Proto, entry.getBase());
          IGM.defineBaseConformanceDescriptor(
              conformance,
              B.getAddrOfCurrentPosition(IGM.ProtocolRequirementStructTy));
        }

        auto reqt = B.beginStruct(IGM.ProtocolRequirementStructTy);

        auto info = getRequirementInfo(entry);

        // Flags.
        reqt.addInt32(info.Flags.getIntValue());

        // Default implementation.
        reqt.addRelativeAddressOrNull(info.DefaultImpl);

        reqt.finishAndAddTo(B);
      }
    }

    llvm::Constant *findDefaultWitness(SILDeclRef func) {
      if (!DefaultWitnesses) return nullptr;

      for (auto &entry : DefaultWitnesses->getEntries()) {
        if (!entry.isValid() || entry.getKind() != SILWitnessTable::Method ||
            entry.getMethodWitness().Requirement != func)
          continue;
        return IGM.getAddrOfSILFunction(entry.getMethodWitness().Witness,
                                        NotForDefinition);
      }

      return nullptr;
    }

    llvm::Constant *findDefaultTypeWitness(AssociatedTypeDecl *assocType) {
      if (!DefaultWitnesses) return nullptr;

      for (auto &entry : DefaultWitnesses->getEntries()) {
        if (!entry.isValid() ||
            entry.getKind() != SILWitnessTable::AssociatedType ||
            entry.getAssociatedTypeWitness().Requirement != assocType)
          continue;

        auto witness =
            entry.getAssociatedTypeWitness().Witness->mapTypeOutOfContext();
        return IGM.getAssociatedTypeWitness(witness,
                                            Proto->getGenericSignature(),
                                            /*inProtocolContext=*/true);
      }

      return nullptr;
    }

    llvm::Constant *findDefaultAssociatedConformanceWitness(
                                                  CanType association,
                                                  ProtocolDecl *requirement) {
      if (!DefaultWitnesses) return nullptr;

      for (auto &entry : DefaultWitnesses->getEntries()) {
        if (!entry.isValid() ||
            entry.getKind() != SILWitnessTable::AssociatedTypeProtocol ||
            entry.getAssociatedTypeProtocolWitness().Protocol != requirement ||
            entry.getAssociatedTypeProtocolWitness().Requirement != association)
          continue;

        auto witness = entry.getAssociatedTypeProtocolWitness().Witness;
        AssociatedConformance conformance(Proto, association, requirement);
        defineDefaultAssociatedConformanceAccessFunction(conformance, witness);
        return IGM.getMangledAssociatedConformance(nullptr, conformance);
      }

      return nullptr;
    }

    void defineDefaultAssociatedConformanceAccessFunction(
                      AssociatedConformance requirement,
                      ProtocolConformanceRef conformance) {
      auto accessor =
        IGM.getAddrOfDefaultAssociatedConformanceAccessor(requirement);

      IRGenFunction IGF(IGM, accessor);
      if (IGM.DebugInfo)
        IGM.DebugInfo->emitArtificialFunction(IGF, accessor);

      Explosion parameters = IGF.collectParameters();

      llvm::Value *associatedTypeMetadata = parameters.claimNext();
      llvm::Value *self = parameters.claimNext();
      llvm::Value *wtable = parameters.claimNext();

      bool hasArchetype =
        !conformance.isConcrete() ||
        conformance.getConcrete()->getType()->hasArchetype();
      if (hasArchetype) {
        // Bind local Self type data from the metadata argument.
        auto selfInContext = Proto->getSelfTypeInContext()->getCanonicalType();
        IGF.bindLocalTypeDataFromTypeMetadata(selfInContext, IsExact, self,
                                              MetadataState::Abstract);
        IGF.setUnscopedLocalTypeData(
            selfInContext,
            LocalTypeDataKind::forAbstractProtocolWitnessTable(Proto),
            wtable);

        // Bind the associated type metadata.
        IGF.bindLocalTypeDataFromTypeMetadata(requirement.getAssociation(),
                                              IsExact,
                                              associatedTypeMetadata,
                                              MetadataState::Abstract);
      }

      // For a concrete witness table, call it.
      ProtocolDecl *associatedProtocol = requirement.getAssociatedRequirement();
      if (conformance.isConcrete()) {
        auto conformanceI = &IGM.getConformanceInfo(associatedProtocol,
                                                    conformance.getConcrete());
        auto returnValue = conformanceI->getTable(IGF, &associatedTypeMetadata);
        IGF.Builder.CreateRet(returnValue);
        return;
      }

      // For an abstract table, emit a reference to the witness table.
      CanType associatedTypeInContext
        = Proto->mapTypeIntoContext(requirement.getAssociation())
            ->getCanonicalType();
      auto returnValue =
          emitArchetypeWitnessTableRef(
            IGF,
            cast<ArchetypeType>(associatedTypeInContext),
            associatedProtocol);
      IGF.Builder.CreateRet(returnValue);
      return;
    }

    void addAssociatedTypeNames() {
      std::string AssociatedTypeNames;

      auto &pi = IGM.getProtocolInfo(Proto,
                                     ProtocolInfoKind::RequirementSignature);
      for (auto &entry : pi.getWitnessEntries()) {
        // Add the associated type name to the list.
        if (entry.isAssociatedType()) {
          if (!AssociatedTypeNames.empty())
            AssociatedTypeNames += ' ';
          AssociatedTypeNames += entry.getAssociatedType()->getName().str();
        }
      }

      llvm::Constant *global = nullptr;
      if (!AssociatedTypeNames.empty()) {
        global = IGM.getAddrOfGlobalString(AssociatedTypeNames,
                                           /*willBeRelativelyAddressed=*/true);
      }
      B.addRelativeAddressOrNull(global);
    }
  };

  template<class Impl, class DeclType>
  class TypeContextDescriptorBuilderBase
    : public ContextDescriptorBuilderBase<Impl> {
  
    using super = ContextDescriptorBuilderBase<Impl>;
  
  protected:
    DeclType *Type;
    RequireMetadata_t HasMetadata;
    TypeContextDescriptorFlags::MetadataInitializationKind
      MetadataInitialization;

    StringRef UserFacingName;
    Optional<TypeImportInfo<std::string>> ImportInfo;
    
    using super::IGM;
    using super::B;
    using super::asImpl;

  public:
    using super::addGenericSignature;
  
    TypeContextDescriptorBuilderBase(IRGenModule &IGM, DeclType *Type,
                                     RequireMetadata_t requireMetadata)
      : super(IGM), Type(Type),
        HasMetadata(requireMetadata),
        MetadataInitialization(computeMetadataInitialization()) {
    }
    
    void layout() {
      asImpl().computeIdentity();

      super::layout();
      asImpl().addName();
      asImpl().addAccessFunction();
      asImpl().addReflectionFieldDescriptor();
      asImpl().addLayoutInfo();
      asImpl().addGenericSignature();
      asImpl().maybeAddResilientSuperclass();
      asImpl().maybeAddMetadataInitialization();
    }

    /// Fill out all the aspects of the type identity.
    void computeIdentity() {
      // Remember the user-facing name.
      UserFacingName = Type->getName().str();

      // For related entities, set the original type name as the ABI name
      // and remember the related entity tag.
      StringRef abiName;
      if (auto *synthesizedTypeAttr =
            Type->getAttrs()
                 .template getAttribute<ClangImporterSynthesizedTypeAttr>()) {
        abiName = synthesizedTypeAttr->originalTypeName;

        getMutableImportInfo().RelatedEntityName =
            std::string(synthesizedTypeAttr->getManglingName());

        // Otherwise, if this was imported from a Clang declaration, use that
        // declaration's name as the ABI name.
      } else if (auto clangDecl =
                            Mangle::ASTMangler::getClangDeclForMangling(Type)) {
        abiName = clangDecl->getName();

        // Typedefs and compatibility aliases that have been promoted to
        // their own nominal types need to be marked specially.
        if (isa<clang::TypedefNameDecl>(clangDecl) ||
            isa<clang::ObjCCompatibleAliasDecl>(clangDecl)) {
          getMutableImportInfo().SymbolNamespace =
            TypeImportSymbolNamespace::CTypedef;
        }
      }

      // If the ABI name differs from the user-facing name, add it as
      // an override.
      if (!abiName.empty() && abiName != UserFacingName) {
        getMutableImportInfo().ABIName = std::string(abiName);
      }
    }

    /// Get the mutable import info.  Note that calling this method itself
    /// changes the code to cause it to be used, so don't set it unless
    /// you're about to write something into it.
    TypeImportInfo<std::string> &getMutableImportInfo() {
      if (!ImportInfo)
        ImportInfo.emplace();
      return *ImportInfo;
    }

    void addName() {
      SmallString<32> name;
      name += UserFacingName;

      // Collect the import info if present.
      if (ImportInfo) {
        name += '\0';
        ImportInfo->appendTo(name);

        // getAddrOfGlobalString will add its own null terminator, so pop
        // off the second one.
        assert(name.back() == '\0');
        name.pop_back();
        assert(name.back() == '\0');
      }
      
      auto nameStr = IGM.getAddrOfGlobalString(name,
                                           /*willBeRelativelyAddressed*/ true);
      B.addRelativeAddress(nameStr);
    }
      
    void addAccessFunction() {
      llvm::Constant *accessor;

      // Don't include an access function if we're emitting the context
      // descriptor without metadata.
      if (!HasMetadata) {
        accessor = nullptr;

      // If it's a generic type, use the generic access function.
      // This has a different prototype from an ordinary function, but
      // the runtime knows to check for that.
      } else if (Type->isGenericContext()) {
        accessor = getGenericTypeMetadataAccessFunction(IGM, Type,
                                                        NotForDefinition);

      // Otherwise, use the ordinary access function, which we'll define
      // when we emit the metadata.
      } else {
        CanType type = Type->getDeclaredType()->getCanonicalType();
        accessor = getOtherwiseDefinedTypeMetadataAccessFunction(IGM, type);
      }
    
      B.addRelativeAddressOrNull(accessor);
    }
    
    ConstantReference getParent() {
      return IGM.getAddrOfParentContextDescriptor(
               Type, /*fromAnonymousContext=*/false);
    }
    
    GenericSignature getGenericSignature() {
      return Type->getGenericSignature();
    }
    
    /// Fill in the fields of a TypeGenericContextDescriptorHeader.
    void addGenericParametersHeader() {
      asImpl().addMetadataInstantiationCache();

      asImpl().addMetadataInstantiationPattern();

      super::addGenericParametersHeader();
    }

    void addMetadataInstantiationPattern() {
      if (!HasMetadata) {
        B.addInt32(0);
        return;
      }

      auto pattern = IGM.getAddrOfTypeMetadataPattern(Type);
      B.addRelativeAddress(pattern);
    }

    void addMetadataInstantiationCache() {
      if (!HasMetadata) {
        B.addInt32(0);
        return;
      }

      auto cache =
        IGM.getAddrOfTypeMetadataInstantiationCache(Type, NotForDefinition);
      B.addRelativeAddress(cache);
    }
      
    bool isUniqueDescriptor() {
      return !isa<ClangModuleUnit>(Type->getModuleScopeContext());
    }
    
    llvm::Constant *emit() {
      asImpl().layout();
      auto addr = IGM.getAddrOfTypeContextDescriptor(Type, HasMetadata,
                                                     B.finishAndCreateFuture());
      auto var = cast<llvm::GlobalVariable>(addr);
      
      var->setConstant(true);
      IGM.setTrueConstGlobal(var);
      return var;
    }

    void setCommonFlags(TypeContextDescriptorFlags &flags) {
      setClangImportedFlags(flags);
      setMetadataInitializationKind(flags);
      setHasCanonicalMetadataPrespecializations(flags);
    }
    
    void setClangImportedFlags(TypeContextDescriptorFlags &flags) {
      if (ImportInfo) {
        flags.setHasImportInfo(true);
      }
    }

    TypeContextDescriptorFlags::MetadataInitializationKind
    computeMetadataInitialization() {
      // Not if we don't have metadata.
      if (!HasMetadata)
        return TypeContextDescriptorFlags::NoMetadataInitialization;

      // Generic types use their own system.
      if (Type->isGenericContext())
        return TypeContextDescriptorFlags::NoMetadataInitialization;

      // Check for foreign metadata.
      if (requiresForeignTypeMetadata(Type))
        return TypeContextDescriptorFlags::ForeignMetadataInitialization;

      // The only other option is singleton initialization.
      if (needsSingletonMetadataInitialization(IGM, Type))
        return TypeContextDescriptorFlags::SingletonMetadataInitialization;

      return TypeContextDescriptorFlags::NoMetadataInitialization;
    }

    void setMetadataInitializationKind(TypeContextDescriptorFlags &flags) {
      flags.setMetadataInitialization(MetadataInitialization);
    }

    void setHasCanonicalMetadataPrespecializations(TypeContextDescriptorFlags &flags) {
      flags.setHasCanonicalMetadataPrespecializations(hasCanonicalMetadataPrespecializations());
    }

    bool hasCanonicalMetadataPrespecializations() {
      return IGM.shouldPrespecializeGenericMetadata() &&
             llvm::any_of(IGM.IRGen.metadataPrespecializationsForType(Type),
                          [](auto pair) {
                            return pair.second ==
                                   TypeMetadataCanonicality::Canonical;
                          });
    }

    void maybeAddMetadataInitialization() {
      switch (MetadataInitialization) {
      case TypeContextDescriptorFlags::NoMetadataInitialization:
        return;

      case TypeContextDescriptorFlags::ForeignMetadataInitialization:
        addForeignMetadataInitialization();
        return;

      case TypeContextDescriptorFlags::SingletonMetadataInitialization:
        addSingletonMetadataInitialization();
        return;
      }
      llvm_unreachable("bad kind");
    }

    /// Add a ForeignMetadataInitialization structure to the descriptor.
    void addForeignMetadataInitialization() {
      llvm::Constant *completionFunction = nullptr;
      if (asImpl().needsForeignMetadataCompletionFunction()) {
        completionFunction =
          IGM.getAddrOfTypeMetadataCompletionFunction(Type, NotForDefinition);
      }
      B.addRelativeAddressOrNull(completionFunction);
    }

    bool needsForeignMetadataCompletionFunction() {
      return ::needsForeignMetadataCompletionFunction(IGM, Type);
    }

    /// Add an SingletonMetadataInitialization structure to the descriptor.
    void addSingletonMetadataInitialization() {
      // Relative pointer to the initialization cache.
      // Note that we trigger the definition of it when emitting the
      // completion function.
      auto cache = IGM.getAddrOfTypeMetadataSingletonInitializationCache(Type,
                                                              NotForDefinition);
      B.addRelativeAddress(cache);

      asImpl().addIncompleteMetadataOrRelocationFunction();

      // Completion function.
      auto completionFunction =
        IGM.getAddrOfTypeMetadataCompletionFunction(Type, NotForDefinition);
      B.addRelativeAddress(completionFunction);
    }

    void addIncompleteMetadata() {
      // Relative pointer to the metadata.
      auto type = Type->getDeclaredTypeInContext()->getCanonicalType();
      auto metadata = IGM.getAddrOfTypeMetadata(type);
      B.addRelativeAddress(metadata);
    }

    /// Customization point for ClassContextDescriptorBuilder.
    void addIncompleteMetadataOrRelocationFunction() {
      addIncompleteMetadata();
    }

    void maybeAddCanonicalMetadataPrespecializations() {
      if (Type->isGenericContext() && hasCanonicalMetadataPrespecializations()) {
        asImpl().addCanonicalMetadataPrespecializations();
      }
    }

    void addCanonicalMetadataPrespecializations() {
      auto specializations = IGM.IRGen.metadataPrespecializationsForType(Type);
      auto count = llvm::count_if(specializations, [](auto pair) {
        return pair.second == TypeMetadataCanonicality::Canonical;
      });
      B.addInt32(count);
      for (auto pair : specializations) {
        if (pair.second != TypeMetadataCanonicality::Canonical) {
          continue;
        }
        auto specialization = pair.first;
        auto *metadata = IGM.getAddrOfTypeMetadata(specialization);
        B.addRelativeAddress(metadata);
      }
    }

    // Subclasses should provide:
    // ContextDescriptorKind getContextKind();
    // void addLayoutInfo();
    // void addReflectionFieldDescriptor();
  };

  class StructContextDescriptorBuilder
    : public TypeContextDescriptorBuilderBase<StructContextDescriptorBuilder,
                                              StructDecl>
  {
    using super = TypeContextDescriptorBuilderBase;
  
    StructDecl *getType() {
      return cast<StructDecl>(Type);
    }

    Size FieldVectorOffset;

  public:
    StructContextDescriptorBuilder(IRGenModule &IGM, StructDecl *Type,
                                   RequireMetadata_t requireMetadata)
      : super(IGM, Type, requireMetadata)
    {
      auto &layout = IGM.getMetadataLayout(getType());
      FieldVectorOffset = layout.getFieldOffsetVectorOffset().getStatic();
    }

    void layout() {
      super::layout();
      maybeAddCanonicalMetadataPrespecializations();
    }

    ContextDescriptorKind getContextKind() {
      return ContextDescriptorKind::Struct;
    }
    
    void addLayoutInfo() {
      auto properties = getType()->getStoredProperties();

      // uint32_t NumFields;
      B.addInt32(properties.size());

      // uint32_t FieldOffsetVectorOffset;
      B.addInt32(FieldVectorOffset / IGM.getPointerSize());
    }
    
    uint16_t getKindSpecificFlags() {
      TypeContextDescriptorFlags flags;

      setCommonFlags(flags);
      return flags.getOpaqueValue();
    }

    void maybeAddResilientSuperclass() { }

    void addReflectionFieldDescriptor() {
      if (IGM.IRGen.Opts.ReflectionLevel == ReflectionMetadataLevel::Disabled) {
        B.addInt32(0);
        return;
      }
    
      IGM.IRGen.noteUseOfFieldDescriptor(getType());

      B.addRelativeAddress(IGM.getAddrOfReflectionFieldDescriptor(
        getType()->getDeclaredType()->getCanonicalType()));
    }
  };
  
  class EnumContextDescriptorBuilder
    : public TypeContextDescriptorBuilderBase<EnumContextDescriptorBuilder,
                                              EnumDecl>
  {
    using super = TypeContextDescriptorBuilderBase;
  
    EnumDecl *getType() {
      return cast<EnumDecl>(Type);
    }
    
    Size PayloadSizeOffset;
    const EnumImplStrategy &Strategy;
    
  public:
    EnumContextDescriptorBuilder(IRGenModule &IGM, EnumDecl *Type,
                                 RequireMetadata_t requireMetadata)
      : super(IGM, Type, requireMetadata),
        Strategy(getEnumImplStrategy(IGM,
                     getType()->getDeclaredTypeInContext()->getCanonicalType()))
    {
      auto &layout = IGM.getMetadataLayout(getType());
      if (layout.hasPayloadSizeOffset())
        PayloadSizeOffset = layout.getPayloadSizeOffset().getStatic();
    }

    void layout() {
      super::layout();
      maybeAddCanonicalMetadataPrespecializations();
    }
    
    ContextDescriptorKind getContextKind() {
      return ContextDescriptorKind::Enum;
    }
    
    void addLayoutInfo() {
      // # payload cases in the low 24 bits, payload size offset in the high 8.
      unsigned numPayloads = Strategy.getElementsWithPayload().size();
      assert(numPayloads < (1<<24) && "too many payload elements for runtime");
      assert(PayloadSizeOffset % IGM.getPointerAlignment() == Size(0)
             && "payload size not word-aligned");
      unsigned PayloadSizeOffsetInWords
        = PayloadSizeOffset / IGM.getPointerSize();
      assert(PayloadSizeOffsetInWords < 0x100 &&
             "payload size offset too far from address point for runtime");

      // uint32_t NumPayloadCasesAndPayloadSizeOffset;
      B.addInt32(numPayloads | (PayloadSizeOffsetInWords << 24));

      // uint32_t NumEmptyCases;
      B.addInt32(Strategy.getElementsWithNoPayload().size());
    }
    
    uint16_t getKindSpecificFlags() {
      TypeContextDescriptorFlags flags;

      setCommonFlags(flags);
      return flags.getOpaqueValue();
    }

    void maybeAddResilientSuperclass() { }

    void addReflectionFieldDescriptor() {
      if (IGM.IRGen.Opts.ReflectionLevel == ReflectionMetadataLevel::Disabled) {
        B.addInt32(0);
        return;
      }

      // Force the emission of the field descriptor or fixed descriptor.
      IGM.IRGen.noteUseOfFieldDescriptor(getType());

      // Some enum layout strategies (viz. C compatible layout) aren't
      // supported by reflection.
      if (!Strategy.isReflectable()) {
        B.addInt32(0);
        return;
      }

      B.addRelativeAddress(IGM.getAddrOfReflectionFieldDescriptor(
        getType()->getDeclaredType()->getCanonicalType()));
    }
  };
  
  class ClassContextDescriptorBuilder
    : public TypeContextDescriptorBuilderBase<ClassContextDescriptorBuilder,
                                              ClassDecl>,
      public SILVTableVisitor<ClassContextDescriptorBuilder>
  {
    using super = TypeContextDescriptorBuilderBase;
  
    ClassDecl *getType() {
      return cast<ClassDecl>(Type);
    }

    // Non-null unless the type is foreign.
    ClassMetadataLayout *MetadataLayout = nullptr;

    Optional<TypeEntityReference> ResilientSuperClassRef;

    SILVTable *VTable;
    bool Resilient;
    bool HasNonoverriddenMethods = false;

    SmallVector<SILDeclRef, 8> VTableEntries;
    SmallVector<std::pair<SILDeclRef, SILDeclRef>, 8> OverrideTableEntries;

  public:
    ClassContextDescriptorBuilder(IRGenModule &IGM, ClassDecl *Type,
                                  RequireMetadata_t requireMetadata)
      : super(IGM, Type, requireMetadata),
        VTable(IGM.getSILModule().lookUpVTable(getType())),
        Resilient(IGM.hasResilientMetadata(Type, ResilienceExpansion::Minimal)) {

      if (getType()->isForeign()) return;

      MetadataLayout = &IGM.getClassMetadataLayout(Type);

      if (auto superclassDecl = getType()->getSuperclassDecl()) {
        if (MetadataLayout && MetadataLayout->hasResilientSuperclass())
          ResilientSuperClassRef = IGM.getTypeEntityReference(superclassDecl);
      }

      addVTableEntries(getType());
    }

    void addMethod(SILDeclRef fn) {
      if (!VTable || methodRequiresReifiedVTableEntry(IGM, VTable, fn)) {
        VTableEntries.push_back(fn);
      } else {
        // Emit a stub method descriptor and lookup function for nonoverridden
        // methods so that resilient code sequences can still use them.
        emitNonoverriddenMethod(fn);
      }
    }

    void addMethodOverride(SILDeclRef baseRef, SILDeclRef declRef) {
      OverrideTableEntries.emplace_back(baseRef, declRef);
    }

    void layout() {
      super::layout();
      addVTable();
      addOverrideTable();
      addObjCResilientClassStubInfo();
      maybeAddCanonicalMetadataPrespecializations();
    }

    void addIncompleteMetadataOrRelocationFunction() {
      if (MetadataLayout == nullptr ||
          !MetadataLayout->hasResilientSuperclass()) {
        addIncompleteMetadata();
        return;
      }

      auto *pattern = IGM.getAddrOfTypeMetadataPattern(Type);
      B.addRelativeAddress(pattern);
    }

    ContextDescriptorKind getContextKind() {
      return ContextDescriptorKind::Class;
    }
    
    uint16_t getKindSpecificFlags() {
      TypeContextDescriptorFlags flags;

      setCommonFlags(flags);

      if (!getType()->isForeign()) {
        if (MetadataLayout->areImmediateMembersNegative())
          flags.class_setAreImmediateMembersNegative(true);

        if (!VTableEntries.empty())
          flags.class_setHasVTable(true);

        if (!OverrideTableEntries.empty())
          flags.class_setHasOverrideTable(true);

        if (MetadataLayout->hasResilientSuperclass())
          flags.class_setHasResilientSuperclass(true);
      }

      if (ResilientSuperClassRef) {
        flags.class_setResilientSuperclassReferenceKind(
                                            ResilientSuperClassRef->getKind());
      }
      
      return flags.getOpaqueValue();
    }

    void maybeAddResilientSuperclass() {
      // RelativeDirectPointer<const void, /*nullable*/ true> SuperClass;
      if (ResilientSuperClassRef) {
        B.addRelativeAddress(ResilientSuperClassRef->getValue());
      }
    }

    void addReflectionFieldDescriptor() {
      // Classes are always reflectable, unless reflection is disabled or this
      // is a foreign class.
      if ((IGM.IRGen.Opts.ReflectionLevel == ReflectionMetadataLevel::Disabled) ||
          getType()->isForeign()) {
        B.addInt32(0);
        return;
      }
    
      B.addRelativeAddress(IGM.getAddrOfReflectionFieldDescriptor(
        getType()->getDeclaredType()->getCanonicalType()));
    }

    Size getFieldVectorOffset() {
      if (!MetadataLayout) return Size(0);
      return (MetadataLayout->hasResilientSuperclass()
                ? MetadataLayout->getRelativeFieldOffsetVectorOffset()
                : MetadataLayout->getStaticFieldOffsetVectorOffset());
    }
    
    void addVTable() {
      LLVM_DEBUG(
        llvm::dbgs() << "VTable entries for " << getType()->getName() << ":\n";
        for (auto entry : VTableEntries) {
          llvm::dbgs() << "  ";
          entry.print(llvm::dbgs());
          llvm::dbgs() << '\n';
        }
      );

      // Only emit a method lookup function if the class is resilient
      // and has a non-empty vtable, as well as no elided methods.
      if (IGM.hasResilientMetadata(getType(), ResilienceExpansion::Minimal)
          && (HasNonoverriddenMethods || !VTableEntries.empty()))
        IGM.emitMethodLookupFunction(getType());

      if (VTableEntries.empty())
        return;
      
      auto offset = MetadataLayout->hasResilientSuperclass()
                      ? MetadataLayout->getRelativeVTableOffset()
                      : MetadataLayout->getStaticVTableOffset();
      B.addInt32(offset / IGM.getPointerSize());
      B.addInt32(VTableEntries.size());
      
      for (auto fn : VTableEntries)
        emitMethodDescriptor(fn);
    }

    void emitMethodDescriptor(SILDeclRef fn) {

      // Define the method descriptor to point to the current position in the
      // nominal type descriptor, if it has a well-defined symbol name.
      IGM.defineMethodDescriptor(fn, Type,
                      B.getAddrOfCurrentPosition(IGM.MethodDescriptorStructTy));

      // Actually build the descriptor.
      auto descriptor = B.beginStruct(IGM.MethodDescriptorStructTy);
      buildMethodDescriptorFields(IGM, VTable, fn, descriptor);
      descriptor.finishAndAddTo(B);

      // Emit method dispatch thunk if the class is resilient.
      auto *func = cast<AbstractFunctionDecl>(fn.getDecl());
      if (Resilient &&
          func->getEffectiveAccess() >= AccessLevel::Public) {
        IGM.emitDispatchThunk(fn);
      }
    }
    
    void emitNonoverriddenMethod(SILDeclRef fn) {
      // TODO: Derivative functions do not distinguish themselves in the mangled
      // names of method descriptor symbols yet, causing symbol name collisions.
      if (fn.getDerivativeFunctionIdentifier())
        return;

     HasNonoverriddenMethods = true;
      // Although this method is non-overridden and therefore left out of the
      // vtable, we still need to maintain the ABI of a potentially-overridden
      // method for external clients.
      
      // Emit method dispatch thunk.
      if (hasPublicVisibility(fn.getLinkage(NotForDefinition))) {
        IGM.emitDispatchThunk(fn);
      }
      
      // Emit a freestanding method descriptor structure. This doesn't have to
      // exist in the table in the class's context descriptor since it isn't
      // in the vtable, but external clients need to be able to link against the
      // symbol.
      IGM.emitNonoverriddenMethodDescriptor(VTable, fn);
    }

    void addOverrideTable() {
      LLVM_DEBUG(
        llvm::dbgs() << "Override Table entries for " << getType()->getName() << ":\n";
        for (auto entry : OverrideTableEntries) {
          llvm::dbgs() << "  ";
          entry.first.print(llvm::dbgs());
          llvm::dbgs() << " -> ";
          entry.second.print(llvm::dbgs());
          llvm::dbgs() << '\n';
        }
      );

      if (OverrideTableEntries.empty())
        return;

      B.addInt32(OverrideTableEntries.size());

      for (auto pair : OverrideTableEntries)
        emitMethodOverrideDescriptor(pair.first, pair.second);
    }

    void emitMethodOverrideDescriptor(SILDeclRef baseRef, SILDeclRef declRef) {
      auto descriptor = B.beginStruct(IGM.MethodOverrideDescriptorStructTy);

      // The class containing the base method.
      auto *baseClass = cast<ClassDecl>(baseRef.getDecl()->getDeclContext());
      IGM.IRGen.noteUseOfTypeContextDescriptor(baseClass, DontRequireMetadata);
      auto baseClassEntity = LinkEntity::forNominalTypeDescriptor(baseClass);
      auto baseClassDescriptor =
        IGM.getAddrOfLLVMVariableOrGOTEquivalent(baseClassEntity);
      descriptor.addRelativeAddress(baseClassDescriptor);

      // The base method.
      auto baseMethodEntity = LinkEntity::forMethodDescriptor(baseRef);
      auto baseMethodDescriptor =
        IGM.getAddrOfLLVMVariableOrGOTEquivalent(baseMethodEntity);
      descriptor.addRelativeAddress(baseMethodDescriptor);

      // The implementation of the override.
      if (auto entry = VTable->getEntry(IGM.getSILModule(), baseRef)) {
        assert(entry->getKind() == SILVTable::Entry::Kind::Override);
        auto *implFn = IGM.getAddrOfSILFunction(entry->getImplementation(),
                                                NotForDefinition);
        descriptor.addRelativeAddress(implFn);
      } else {
        // The method is removed by dead method elimination.
        // It should be never called. We add a pointer to an error function.
        descriptor.addRelativeAddressOrNull(nullptr);
      }

      descriptor.finishAndAddTo(B);
    }

    void addPlaceholder(MissingMemberDecl *MMD) {
      llvm_unreachable("cannot generate metadata with placeholders in it");
    }
    
    void addLayoutInfo() {

      // TargetRelativeDirectPointer<Runtime, const char> SuperclassType;
      if (auto superclassType = getType()->getSuperclass()) {
        GenericSignature genericSig = getType()->getGenericSignature();
        B.addRelativeAddress(IGM.getTypeRef(superclassType, genericSig,
                                            MangledTypeRefRole::Metadata)
                               .first);
      } else {
        B.addInt32(0);
      }

      auto properties = getType()->getStoredProperties();

      // union {
      //   uint32_t MetadataNegativeSizeInWords;
      //   RelativeDirectPointer<StoredClassMetadataBounds>
      //     ResilientMetadataBounds;
      // };
      if (!MetadataLayout) {
        // FIXME: do something meaningful for foreign classes?
        B.addInt32(0);
      } else if (!MetadataLayout->hasResilientSuperclass()) {
        B.addInt32(MetadataLayout->getSize().AddressPoint
                     / IGM.getPointerSize());
      } else {
        B.addRelativeAddress(
          IGM.getAddrOfClassMetadataBounds(getType(), NotForDefinition));
      }

      // union {
      //   uint32_t MetadataPositiveSizeInWords;
      //   ExtraClassContextFlags ExtraClassFlags;
      // };
      if (!MetadataLayout) {
        // FIXME: do something meaningful for foreign classes?
        B.addInt32(0);
      } else if (!MetadataLayout->hasResilientSuperclass()) {
        B.addInt32(MetadataLayout->getSize().getOffsetToEnd()
                     / IGM.getPointerSize());
      } else {
        ExtraClassDescriptorFlags flags;
        if (IGM.hasObjCResilientClassStub(getType()))
          flags.setObjCResilientClassStub(true);
        B.addInt32(flags.getOpaqueValue());
      }

      // uint32_t NumImmediateMembers;
      auto numImmediateMembers =
        (MetadataLayout ? MetadataLayout->getNumImmediateMembers() : 0);
      B.addInt32(numImmediateMembers);

      // uint32_t NumFields;
      B.addInt32(properties.size());

      // uint32_t FieldOffsetVectorOffset;
      B.addInt32(getFieldVectorOffset() / IGM.getPointerSize());
    }

    void addObjCResilientClassStubInfo() {
      if (IGM.getClassMetadataStrategy(getType()) !=
            ClassMetadataStrategy::Resilient)
        return;

      if (!IGM.hasObjCResilientClassStub(getType()))
        return;

      B.addRelativeAddress(
        IGM.getAddrOfObjCResilientClassStub(
          getType(), NotForDefinition,
          TypeMetadataAddress::AddressPoint));
    }

    void addCanonicalMetadataPrespecializations() {
      super::addCanonicalMetadataPrespecializations();
      auto specializations = IGM.IRGen.metadataPrespecializationsForType(Type);
      for (auto pair : specializations) {
        if (pair.second != TypeMetadataCanonicality::Canonical) {
          continue;
        }
        auto specialization = pair.first;
        auto *function = IGM.getAddrOfCanonicalSpecializedGenericTypeMetadataAccessFunction(specialization, NotForDefinition);
        B.addRelativeAddress(function);
      }
    }
  };
  
  class OpaqueTypeDescriptorBuilder
      : public ContextDescriptorBuilderBase<OpaqueTypeDescriptorBuilder>
  {
    using super = ContextDescriptorBuilderBase;

    OpaqueTypeDecl *O;
  public:
    
    OpaqueTypeDescriptorBuilder(IRGenModule &IGM, OpaqueTypeDecl *O)
      : super(IGM), O(O)
    {}
    
    void layout() {
      super::layout();
      addGenericSignature();
      addUnderlyingTypeAndConformances();
    }
    
    void addUnderlyingTypeAndConformances() {
      auto sig = O->getOpaqueInterfaceGenericSignature();
      auto underlyingType = Type(O->getUnderlyingInterfaceType())
        .subst(*O->getUnderlyingTypeSubstitutions())
        ->getCanonicalType(sig);

      auto contextSig = O->getGenericSignature().getCanonicalSignature();

      B.addRelativeAddress(IGM.getTypeRef(underlyingType, contextSig,
                                          MangledTypeRefRole::Metadata).first);
      
      auto opaqueType = O->getDeclaredInterfaceType()
                         ->castTo<OpaqueTypeArchetypeType>();
      
      for (auto proto : opaqueType->getConformsTo()) {
        auto conformance = ProtocolConformanceRef(proto);
        auto underlyingConformance = conformance
          .subst(O->getUnderlyingInterfaceType(),
                 *O->getUnderlyingTypeSubstitutions());

        // Skip protocols without Witness tables, e.g. @objc protocols.
        if (!Lowering::TypeConverter::protocolRequiresWitnessTable(
                underlyingConformance.getRequirement()))
          continue;

        auto witnessTableRef = IGM.emitWitnessTableRefString(
                                          underlyingType, underlyingConformance,
                                          contextSig,
                                          /*setLowBit*/ false);
        B.addRelativeAddress(witnessTableRef);
      }
    }
    
    bool isUniqueDescriptor() {
      switch (LinkEntity::forOpaqueTypeDescriptor(O)
                .getLinkage(NotForDefinition)) {
      case SILLinkage::Public:
      case SILLinkage::PublicExternal:
      case SILLinkage::Hidden:
      case SILLinkage::HiddenExternal:
      case SILLinkage::Private:
      case SILLinkage::PrivateExternal:
        return true;
        
      case SILLinkage::Shared:
      case SILLinkage::SharedExternal:
      case SILLinkage::PublicNonABI:
        return false;
      }
      llvm_unreachable("covered switch");
    }
    
    GenericSignature getGenericSignature() {
      return O->getOpaqueInterfaceGenericSignature();
    }
    
    ConstantReference getParent() {
      // VarDecls aren't normally contexts, but we still want to mangle
      // an anonymous context for one.
      if (IGM.IRGen.Opts.EnableAnonymousContextMangledNames) {
        if (auto namingVar = dyn_cast<VarDecl>(O->getNamingDecl())) {
          return ConstantReference(
                           IGM.getAddrOfAnonymousContextDescriptor(namingVar),
                           ConstantReference::Direct);
        }
      }
      
      DeclContext *parent = O->getNamingDecl()->getInnermostDeclContext();

      // If we have debug mangled names enabled for anonymous contexts, nest
      // the opaque type descriptor inside an anonymous context for the
      // defining function. This will let type reconstruction in the debugger
      // match the opaque context back into the AST.
      //
      // Otherwise, we can use the module context for nongeneric contexts.
      if (!IGM.IRGen.Opts.EnableAnonymousContextMangledNames
          && !parent->isGenericContext()) {
        parent = parent->getParentModule();
      }
      
      return IGM.getAddrOfContextDescriptorForParent(parent, parent,
                                                     /*fromAnonymous*/ false);
    }
    
    ContextDescriptorKind getContextKind() {
      return ContextDescriptorKind::OpaqueType;
    }
    
    void emit() {
      asImpl().layout();
      
      auto addr = IGM.getAddrOfOpaqueTypeDescriptor(O,
                                                    B.finishAndCreateFuture());
      auto var = cast<llvm::GlobalVariable>(addr);
      
      var->setConstant(true);
      IGM.setTrueConstGlobal(var);
      IGM.emitOpaqueTypeDescriptorAccessor(O);
    }
    
    uint16_t getKindSpecificFlags() {
      // Store the size of the type and conformances vector in the flags.
      auto opaqueType = O->getDeclaredInterfaceType()
        ->castTo<OpaqueTypeArchetypeType>();

      return 1 + opaqueType->getConformsTo().size();
    }
  };
} // end anonymous namespace

static void eraseExistingTypeContextDescriptor(IRGenModule &IGM,
                                               NominalTypeDecl *type) {
  // We may have emitted a partial type context descriptor with some empty
  // fields, and then later discovered we're emitting complete metadata.
  // Remove existing definitions of the type context so that we can regenerate
  // a complete descriptor.
  auto entity = IGM.getAddrOfTypeContextDescriptor(type, DontRequireMetadata);
  entity = entity->stripPointerCasts();
  auto existingContext = dyn_cast<llvm::GlobalVariable>(entity);
  if (existingContext && !existingContext->isDeclaration()) {
    existingContext->setInitializer(nullptr);
  }
}

void irgen::emitLazyTypeContextDescriptor(IRGenModule &IGM,
                                          NominalTypeDecl *type,
                                          RequireMetadata_t requireMetadata) {
  eraseExistingTypeContextDescriptor(IGM, type);

  if (auto sd = dyn_cast<StructDecl>(type)) {
    StructContextDescriptorBuilder(IGM, sd, requireMetadata).emit();
  } else if (auto ed = dyn_cast<EnumDecl>(type)) {
    EnumContextDescriptorBuilder(IGM, ed, requireMetadata).emit();
  } else if (auto cd = dyn_cast<ClassDecl>(type)) {
    ClassContextDescriptorBuilder(IGM, cd, requireMetadata).emit();
  } else {
    llvm_unreachable("type does not have a context descriptor");
  }
}

void irgen::emitLazyTypeMetadata(IRGenModule &IGM, NominalTypeDecl *type) {
  eraseExistingTypeContextDescriptor(IGM, type);

  if (requiresForeignTypeMetadata(type)) {
    emitForeignTypeMetadata(IGM, type);
  } else if (auto sd = dyn_cast<StructDecl>(type)) {
    emitStructMetadata(IGM, sd);
  } else if (auto ed = dyn_cast<EnumDecl>(type)) {
    emitEnumMetadata(IGM, ed);
  } else if (auto pd = dyn_cast<ProtocolDecl>(type)) {
    IGM.emitProtocolDecl(pd);
  } else {
    llvm_unreachable("should not have enqueued a class decl here!");
  }
}

void irgen::emitLazyMetadataAccessor(IRGenModule &IGM,
                                     NominalTypeDecl *nominal) {
  GenericArguments genericArgs;
  genericArgs.collectTypes(IGM, nominal);

  llvm::Function *accessor = IGM.getAddrOfGenericTypeMetadataAccessFunction(
      nominal, genericArgs.Types, ForDefinition);

  if (IGM.getOptions().optimizeForSize())
    accessor->addFnAttr(llvm::Attribute::NoInline);

  bool isReadNone = (genericArgs.Types.size() <=
                     NumDirectGenericTypeMetadataAccessFunctionArgs);

  emitCacheAccessFunction(
      IGM, accessor, /*cache*/ nullptr, CacheStrategy::None,
      [&](IRGenFunction &IGF, Explosion &params) {
        return emitGenericTypeMetadataAccessFunction(IGF, params, nominal,
                                                     genericArgs);
      },
      isReadNone);
}

void irgen::emitLazyCanonicalSpecializedMetadataAccessor(IRGenModule &IGM,
                                                         CanType theType) {
  llvm::Function *accessor =
      IGM.getAddrOfCanonicalSpecializedGenericTypeMetadataAccessFunction(
          theType, ForDefinition);

  if (IGM.getOptions().optimizeForSize()) {
    accessor->addFnAttr(llvm::Attribute::NoInline);
  }

  emitCacheAccessFunction(
      IGM, accessor, /*cache=*/nullptr, CacheStrategy::None,
      [&](IRGenFunction &IGF, Explosion &params) {
        return emitCanonicalSpecializedGenericTypeMetadataAccessFunction(
            IGF, params, theType);
      },
      /*isReadNone=*/true);
}

void irgen::emitLazySpecializedGenericTypeMetadata(IRGenModule &IGM,
                                                   CanType type) {
  switch (type->getKind()) {
  case TypeKind::Struct:
  case TypeKind::BoundGenericStruct:
    emitSpecializedGenericStructMetadata(IGM, type,
                                         *type.getStructOrBoundGenericStruct());
    break;
  case TypeKind::Enum:
  case TypeKind::BoundGenericEnum:
    emitSpecializedGenericEnumMetadata(IGM, type,
                                       *type.getEnumOrBoundGenericEnum());
    break;
  case TypeKind::Class:
  case TypeKind::BoundGenericClass:
    emitSpecializedGenericClassMetadata(IGM, type,
                                        *type.getClassOrBoundGenericClass());
    break;
  default:
    llvm_unreachable(
        "Cannot statically specialize metadata for generic types of"
        "kind other than struct, enum, and class.");
  }
}

llvm::Constant *
IRGenModule::getAddrOfSharedContextDescriptor(LinkEntity entity,
                                              ConstantInit definition,
                                              llvm::function_ref<void()> emit) {
  if (!definition) {
    // Generate the definition if it hasn't been generated yet.
    auto existing = GlobalVars.find(entity);
    if (existing == GlobalVars.end() ||
        !existing->second
        || cast<llvm::GlobalValue>(existing->second)->isDeclaration()) {
      
      // In some cases we have multiple declarations in the AST that end up
      // with the same context mangling (a clang module and its overlay,
      // equivalent extensions, etc.). These can share a context descriptor
      // at runtime.
      auto mangledName = entity.mangleAsString();
      if (auto otherDefinition = Module.getGlobalVariable(mangledName)) {
        GlobalVars.insert({entity, otherDefinition});
        return otherDefinition;
      }
      
      // Otherwise, emit the descriptor.
      emit();
    }
  }
  
  return getAddrOfLLVMVariable(entity,
                               definition,
                               DebugTypeInfo());
}

llvm::Constant *
IRGenModule::getAddrOfModuleContextDescriptor(ModuleDecl *D,
                                              ConstantInit definition) {
  auto entity = LinkEntity::forModuleDescriptor(D);
  return getAddrOfSharedContextDescriptor(entity, definition,
    [&]{ ModuleContextDescriptorBuilder(*this, D).emit(); });
}

llvm::Constant *
IRGenModule::getAddrOfObjCModuleContextDescriptor() {
  if (!ObjCModule)
    ObjCModule = ModuleDecl::create(
      Context.getIdentifier(MANGLING_MODULE_OBJC),
      Context);
  return getAddrOfModuleContextDescriptor(ObjCModule);
}

llvm::Constant *
IRGenModule::getAddrOfClangImporterModuleContextDescriptor() {
  if (!ClangImporterModule)
    ClangImporterModule = ModuleDecl::create(
      Context.getIdentifier(MANGLING_MODULE_CLANG_IMPORTER),
      Context);
  return getAddrOfModuleContextDescriptor(ClangImporterModule);
}

llvm::Constant *
IRGenModule::getAddrOfExtensionContextDescriptor(ExtensionDecl *ED,
                                                 ConstantInit definition) {
  auto entity = LinkEntity::forExtensionDescriptor(ED);
  return getAddrOfSharedContextDescriptor(entity, definition,
    [&]{ ExtensionContextDescriptorBuilder(*this, ED).emit(); });
}

llvm::Constant *
IRGenModule::getAddrOfAnonymousContextDescriptor(
                                     PointerUnion<DeclContext *, VarDecl *> DC,
                                     ConstantInit definition) {
  auto entity = LinkEntity::forAnonymousDescriptor(DC);
  return getAddrOfSharedContextDescriptor(entity, definition,
    [&]{ AnonymousContextDescriptorBuilder(*this, DC).emit(); });
}

llvm::Constant *
IRGenModule::getAddrOfOriginalModuleContextDescriptor(StringRef Name) {
  return getAddrOfModuleContextDescriptor(OriginalModules.insert({Name,
    ModuleDecl::create(Context.getIdentifier(Name), Context)})
                                          .first->getValue());
}

static void emitInitializeFieldOffsetVector(IRGenFunction &IGF,
                                            SILType T,
                                            llvm::Value *metadata,
                                            bool isVWTMutable,
                                       MetadataDependencyCollector *collector) {
  auto &IGM = IGF.IGM;

  auto *target = T.getNominalOrBoundGenericNominal();
  llvm::Value *fieldVector
    = emitAddressOfFieldOffsetVector(IGF, metadata, target)
      .getAddress();
  
  // Collect the stored properties of the type.
  llvm::SmallVector<VarDecl*, 4> storedProperties;
  for (auto prop : target->getStoredProperties()) {
    storedProperties.push_back(prop);
  }

  // Fill out an array with the field type metadata records.
  Address fields = IGF.createAlloca(
                   llvm::ArrayType::get(IGM.Int8PtrPtrTy,
                                        storedProperties.size()),
                   IGM.getPointerAlignment(), "classFields");
  IGF.Builder.CreateLifetimeStart(fields,
                  IGM.getPointerSize() * storedProperties.size());
  fields = IGF.Builder.CreateStructGEP(fields, 0, Size(0));

  unsigned index = 0;
  for (auto prop : storedProperties) {
    auto propTy = T.getFieldType(prop, IGF.getSILModule(),
                                 TypeExpansionContext::minimal());
    llvm::Value *metadata = emitTypeLayoutRef(IGF, propTy, collector);
    Address field = IGF.Builder.CreateConstArrayGEP(fields, index,
                                                    IGM.getPointerSize());
    IGF.Builder.CreateStore(metadata, field);
    ++index;
  }

  // Ask the runtime to lay out the struct or class.
  auto numFields = IGM.getSize(Size(storedProperties.size()));

  if (auto *classDecl = dyn_cast<ClassDecl>(target)) {
    // Compute class layout flags.
    ClassLayoutFlags flags = ClassLayoutFlags::Swift5Algorithm;

    switch (IGM.getClassMetadataStrategy(classDecl)) {
    case ClassMetadataStrategy::Resilient:
      break;

    case ClassMetadataStrategy::Singleton:
    case ClassMetadataStrategy::Update:
    case ClassMetadataStrategy::FixedOrUpdate:
      flags |= ClassLayoutFlags::HasStaticVTable;
      break;

    case ClassMetadataStrategy::Fixed:
      llvm_unreachable("Emitting metadata init for fixed class metadata?");
    }

    llvm::Value *dependency;
    
    switch (IGM.getClassMetadataStrategy(classDecl)) {
    case ClassMetadataStrategy::Resilient:
    case ClassMetadataStrategy::Singleton:
      // Call swift_initClassMetadata().
      dependency =
        IGF.Builder.CreateCall(IGM.getInitClassMetadata2Fn(),
                               {metadata,
                                IGM.getSize(Size(uintptr_t(flags))),
                                numFields, fields.getAddress(), fieldVector});
      break;

    case ClassMetadataStrategy::Update:
    case ClassMetadataStrategy::FixedOrUpdate:
      assert(IGM.Context.LangOpts.EnableObjCInterop);

      // Call swift_updateClassMetadata(). Note that the static metadata
      // already references the superclass in this case, but we still want
      // to ensure the superclass metadata is initialized first.
      dependency =
        IGF.Builder.CreateCall(IGM.getUpdateClassMetadata2Fn(),
                               {metadata,
                                IGM.getSize(Size(uintptr_t(flags))),
                                numFields, fields.getAddress(), fieldVector});
      break;

    case ClassMetadataStrategy::Fixed:
      llvm_unreachable("Emitting metadata init for fixed class metadata?");
    }

    // Collect any possible dependency from initializing the class; generally
    // this involves the superclass.
    assert(collector);
    collector->collect(IGF, dependency);

  } else {
    assert(isa<StructDecl>(target));

    // Compute struct layout flags.
    StructLayoutFlags flags = StructLayoutFlags::Swift5Algorithm;
    if (isVWTMutable)
      flags |= StructLayoutFlags::IsVWTMutable;

    // Call swift_initStructMetadata().
    IGF.Builder.CreateCall(IGM.getInitStructMetadataFn(),
                           {metadata, IGM.getSize(Size(uintptr_t(flags))),
                            numFields, fields.getAddress(), fieldVector});
  }

  IGF.Builder.CreateLifetimeEnd(fields,
                  IGM.getPointerSize() * storedProperties.size());
}

static void emitInitializeValueMetadata(IRGenFunction &IGF,
                                        NominalTypeDecl *nominalDecl,
                                        llvm::Value *metadata,
                                        bool isVWTMutable,
                                        MetadataDependencyCollector *collector) {
  auto loweredTy =
    IGF.IGM.getLoweredType(nominalDecl->getDeclaredTypeInContext());

  if (isa<StructDecl>(nominalDecl)) {
    auto &fixedTI = IGF.IGM.getTypeInfo(loweredTy);
    if (isa<FixedTypeInfo>(fixedTI)) return;

    emitInitializeFieldOffsetVector(IGF, loweredTy, metadata, isVWTMutable,
                                    collector);
  } else {
    assert(isa<EnumDecl>(nominalDecl));
    auto &strategy = getEnumImplStrategy(IGF.IGM, loweredTy);
    strategy.initializeMetadata(IGF, metadata, isVWTMutable, loweredTy,
                                collector);
  }
}

static void emitInitializeClassMetadata(IRGenFunction &IGF,
                                        ClassDecl *classDecl,
                                        const ClassLayout &fieldLayout,
                                        llvm::Value *metadata,
                                        MetadataDependencyCollector *collector) {
  auto &IGM = IGF.IGM;

  assert(IGM.getClassMetadataStrategy(classDecl)
         != ClassMetadataStrategy::Fixed);

  auto loweredTy =
    IGM.getLoweredType(classDecl->getDeclaredTypeInContext());

  // Set the superclass, fill out the field offset vector, and copy vtable
  // entries, generic requirements and field offsets from superclasses.
  emitInitializeFieldOffsetVector(IGF, loweredTy,
                                  metadata, /*VWT is mutable*/ false,
                                  collector);

  // Realizing the class with the ObjC runtime will copy back to the
  // field offset globals for us; but if ObjC interop is disabled, we
  // have to do that ourselves, assuming we didn't just emit them all
  // correctly in the first place.
  if (!IGM.ObjCInterop) {
    for (auto prop : classDecl->getStoredProperties()) {
      auto fieldInfo = fieldLayout.getFieldAccessAndElement(prop);
      if (fieldInfo.first == FieldAccess::NonConstantDirect) {
        Address offsetA = IGM.getAddrOfFieldOffset(prop, ForDefinition);

        // We can't use emitClassFieldOffset() here because that creates
        // an invariant load, which could be hoisted above the point
        // where the metadata becomes fully initialized
        auto slot =
          emitAddressOfClassFieldOffset(IGF, metadata, classDecl, prop);
        auto offsetVal = IGF.emitInvariantLoad(slot);
        IGF.Builder.CreateStore(offsetVal, offsetA);
      }
    }
  }
}

static MetadataKind getMetadataKind(NominalTypeDecl *nominalDecl) {
  if (isa<StructDecl>(nominalDecl))
    return MetadataKind::Struct;

  assert(isa<EnumDecl>(nominalDecl));
  return (nominalDecl->isOptionalDecl()
          ? MetadataKind::Optional
          : MetadataKind::Enum);
}

/*****************************************************************************/
/** Metadata Emission ********************************************************/
/*****************************************************************************/

namespace {
  /// An adapter class which turns a metadata layout class into a
  /// generic metadata layout class.
  template <class Impl, class DeclType>
  class GenericMetadataBuilderBase {
  protected:
    IRGenModule &IGM;
    DeclType *Target;
    ConstantStructBuilder &B;

    /// Set to true if the metadata record for the generic type has fields
    /// outside of the generic parameter vector.
    bool HasDependentMetadata = false;
    
    /// Set to true if the value witness table for the generic type is dependent
    /// on its generic parameters. Implies HasDependentMetadata.
    bool HasDependentVWT = false;
    
    GenericMetadataBuilderBase(IRGenModule &IGM, DeclType *Target,
                               ConstantStructBuilder &B)
      : IGM(IGM), Target(Target), B(B) {}

    /// Emit the instantiation cache variable for the template.
    void emitInstantiationCache() {
      auto cache = cast<llvm::GlobalVariable>(
        IGM.getAddrOfTypeMetadataInstantiationCache(Target, ForDefinition));
      auto init =
        llvm::ConstantAggregateZero::get(cache->getValueType());
      cache->setInitializer(init);
    }

    Impl &asImpl() { return *static_cast<Impl*>(this); }

    /// Emit the create function for the template.
    void emitInstantiationFunction() {
      // using MetadataInstantiator =
      //   Metadata *(TypeContextDescriptor *type,
      //              const void * const *arguments,
      //              const GenericMetadataPattern *pattern);
      llvm::Function *f =
        IGM.getAddrOfTypeMetadataInstantiationFunction(Target, ForDefinition);
      f->setAttributes(IGM.constructInitialAttributes());
      f->setDoesNotThrow();
      IGM.setHasNoFramePointer(f);

      IRGenFunction IGF(IGM, f);

      // Skip instrumentation when building for TSan to avoid false positives.
      // The synchronization for this happens in the Runtime and we do not see it.
      if (IGM.IRGen.Opts.Sanitizers & SanitizerKind::Thread)
        f->removeFnAttr(llvm::Attribute::SanitizeThread);

      if (IGM.DebugInfo)
        IGM.DebugInfo->emitArtificialFunction(IGF, f);

      Explosion params = IGF.collectParameters();
      llvm::Value *descriptor = params.claimNext();
      llvm::Value *args = params.claimNext();
      llvm::Value *templatePointer = params.claimNext();

      // Bind the generic arguments.
      if (Target->isGenericContext()) {
        Address argsArray(args, IGM.getPointerAlignment());
        emitPolymorphicParametersFromArray(IGF, Target, argsArray,
                                           MetadataState::Abstract);
      }

      // Allocate the metadata.
      llvm::Value *metadata =
        asImpl().emitAllocateMetadata(IGF, descriptor, args, templatePointer);

      IGF.Builder.CreateRet(metadata);
    }

    void emitCompletionFunction() {
      // using MetadataCompleter =
      //   MetadataDependency(Metadata *type,
      //                      MetadataCompletionContext *context,
      //                      const GenericMetadataPattern *pattern);
      emitMetadataCompletionFunction(IGM, Target,
        [&](IRGenFunction &IGF, llvm::Value *metadata,
            MetadataDependencyCollector *collector) {
        // Bind the generic arguments.
        // FIXME: this will be problematic if we ever try to bind superclass
        // types from type metadata!
        assert(Target->isGenericContext());
        auto type = Target->getDeclaredTypeInContext()->getCanonicalType();
        IGF.bindLocalTypeDataFromTypeMetadata(type, IsExact, metadata,
                                              MetadataState::Abstract);

        // A dependent VWT means that we have dependent metadata.
        if (HasDependentVWT)
          HasDependentMetadata = true;

        if (HasDependentMetadata)
          asImpl().emitInitializeMetadata(IGF, metadata, false, collector);
      });
    }

    /// The information necessary to fill in a GenericMetadataPartialPattern
    /// structure.
    struct PartialPattern {
      llvm::Constant *Data;
      Size DataOffset;
      Size DataSize;
    };
    void addPartialPattern(PartialPattern pattern) {
      // RelativeDirectPointer<void*> Pattern;
      B.addRelativeAddress(pattern.Data);

      // uint16_t OffsetInWords;
      B.addInt16(IGM.getOffsetInWords(pattern.DataOffset));

      // uint16_t SizeInWords;
      B.addInt16(IGM.getOffsetInWords(pattern.DataSize));
    }

  public:
    void createMetadataAccessFunction() {
      (void) getGenericTypeMetadataAccessFunction(IGM, Target, ForDefinition);
    }

    void layout() {
      asImpl().layoutHeader();

      // See also: [pre-5.2-extra-data-zeroing]
      // See also: [pre-5.3-extra-data-zeroing]
      if (asImpl().hasExtraDataPattern()) {
        asImpl().addExtraDataPattern();
      }

      // Immediate-members pattern.  This is only valid for classes.
      if (asImpl().hasImmediateMembersPattern()) {
        asImpl().addImmediateMembersPattern();
      }

      // We're done with the pattern now.
#ifndef NDEBUG
      auto finalOffset = B.getNextOffsetFromGlobal();
#endif

      asImpl().emitInstantiationDefinitions();

      assert(finalOffset == B.getNextOffsetFromGlobal() &&
             "emitInstantiationDefinitions added members to the pattern!");
    }

    // Emit the fields of GenericMetadataPattern.
    void layoutHeader() {
      // RelativePointer<MetadataInstantiator> InstantiationFunction;
      asImpl().addInstantiationFunction();

      // RelativePointer<MetadataCompleter> CompletionFunction;
      asImpl().addCompletionFunction();

      // ClassMetadataPatternFlags PatternFlags;
      asImpl().addPatternFlags();
    }

    void addInstantiationFunction() {
      auto function = IGM.getAddrOfTypeMetadataInstantiationFunction(Target,
                                                              NotForDefinition);
      B.addRelativeAddress(function);
    }

    void addCompletionFunction() {
      if (!asImpl().hasCompletionFunction()) {
        B.addInt32(0);
        return;
      }

      auto function = IGM.getAddrOfTypeMetadataCompletionFunction(Target,
                                                              NotForDefinition);
      B.addRelativeAddress(function);
    }

    void addPatternFlags() {
      GenericMetadataPatternFlags flags = asImpl().getPatternFlags();
      B.addInt32(flags.getOpaqueValue());
    }

    GenericMetadataPatternFlags getPatternFlags() {
      GenericMetadataPatternFlags flags;

      if (asImpl().hasExtraDataPattern())
        flags.setHasExtraDataPattern(true);

      return flags;
    }

    bool hasExtraDataPattern() {
      return false;
    }
    void addExtraDataPattern() {
      asImpl().addPartialPattern(asImpl().buildExtraDataPattern());
    }
    PartialPattern buildExtraDataPattern() {
      llvm_unreachable("no extra data pattern!");
    }

    bool hasImmediateMembersPattern() {
      return false;
    }
    void addImmediateMembersPattern() {
      asImpl().addPartialPattern(asImpl().buildImmediateMembersPattern());
    }
    PartialPattern buildImmediateMembersPattern() {
      llvm_unreachable("no immediate members pattern!");
    }

    void emitInstantiationDefinitions() {
      // Force the emission of the nominal type descriptor, although we
      // don't use it yet.
      (void) asImpl().emitNominalTypeDescriptor();

      // Emit the instantiation function.
      asImpl().emitInstantiationFunction();

      // Emit the completion function.
      if (asImpl().hasCompletionFunction())
        asImpl().emitCompletionFunction();

      // Emit the instantiation cache.
      asImpl().emitInstantiationCache();
    }
  };

template <class Impl, class DeclType>
  class GenericValueMetadataBuilderBase
         : public GenericMetadataBuilderBase<Impl, DeclType> {
    using super = GenericMetadataBuilderBase<Impl, DeclType>;
  protected:
    using super::IGM;
    using super::asImpl;
    using super::Target;
    using super::B;

    template <class... T>
    GenericValueMetadataBuilderBase(IRGenModule &IGM, DeclType *Target,
                                    ConstantStructBuilder &B)
      : super(IGM, Target, B) {}

    SILType getLoweredType() {
      return IGM.getLoweredType(Target->getDeclaredTypeInContext());
    }

  public:
    /// Emit the fields of a GenericValueMetadataPattern.
    void layoutHeader() {
      super::layoutHeader();

      // RelativeIndirectablePointer<const ValueWitnessTable> ValueWitnesses;
      asImpl().addValueWitnessTable();

    }

    GenericMetadataPatternFlags getPatternFlags() {
      auto flags = super::getPatternFlags();

      flags.value_setMetadataKind(getMetadataKind(Target));

      assert(!asImpl().hasImmediateMembersPattern());

      return flags;
    }

    void addValueWitnessTable() {
      ConstantReference table =
                              asImpl().emitValueWitnessTable(/*relative*/ true);
      B.addRelativeAddress(table);
    }
  
    void emitInitializeMetadata(IRGenFunction &IGF,
                                llvm::Value *metadata,
                                bool isVWTMutable,
                                MetadataDependencyCollector *collector) {
      emitInitializeValueMetadata(IGF, Target, metadata,
                                  isVWTMutable, collector);
    }
  };
} // end anonymous namespace

/// Create an access function for the given type which triggers the
/// in-place initialization path.
static void
createSingletonInitializationMetadataAccessFunction(IRGenModule &IGM,
                                                    NominalTypeDecl *typeDecl,
                                                    CanType type) {
  assert(!typeDecl->isGenericContext());

  (void) createTypeMetadataAccessFunction(IGM, type,
                                          CacheStrategy::SingletonInitialization,
                                          [&](IRGenFunction &IGF,
                                              DynamicMetadataRequest request,
                                              llvm::Constant *cacheVariable) {
    llvm::Value *descriptor =
      IGF.IGM.getAddrOfTypeContextDescriptor(typeDecl, RequireMetadata);
    auto responsePair =
      IGF.Builder.CreateCall(IGF.IGM.getGetSingletonMetadataFn(),
                             {request.get(IGF), descriptor});
    return MetadataResponse::handle(IGF, request, responsePair);
  });
}

/// Create an access function for the given non-generic type.
static void createNonGenericMetadataAccessFunction(IRGenModule &IGM,
                                                   NominalTypeDecl *typeDecl) {
  assert(!typeDecl->isGenericContext());
  auto type = typeDecl->getDeclaredType()->getCanonicalType();

  // If the type requires the in-place initialization pattern, use it.
  if (needsSingletonMetadataInitialization(IGM, typeDecl)) {
    createSingletonInitializationMetadataAccessFunction(IGM, typeDecl, type);
    return;
  }

  // Otherwise, use the lazy pattern, which should be emitted using a
  // direct reference to the metadata.
  createDirectTypeMetadataAccessFunction(IGM, type, /*allow existing*/ false);
}

// Classes

/// Emit the base-offset variable for the class.
static void emitClassMetadataBaseOffset(IRGenModule &IGM,
                                        ClassDecl *classDecl) {
  // Otherwise, we know the offset at compile time, even if our
  // clients do not, so just emit a constant.
  auto &layout = IGM.getClassMetadataLayout(classDecl);

  // Only classes defined in resilient modules, or those that have
  // a resilient superclass need this.
  if (!layout.hasResilientSuperclass() &&
      !IGM.hasResilientMetadata(classDecl, ResilienceExpansion::Minimal)) {
    return;
  }

  auto *offsetAddr =
    IGM.getAddrOfClassMetadataBounds(classDecl, ForDefinition);
  auto *offsetVar = cast<llvm::GlobalVariable>(offsetAddr);

  if (layout.hasResilientSuperclass()) {
    // If the superclass is resilient to us, we have to compute and
    // initialize the global when we initialize the metadata.
    auto init = llvm::ConstantAggregateZero::get(offsetVar->getValueType());

    offsetVar->setInitializer(init);
    offsetVar->setConstant(false);
    return;
  }

  auto immediateMembersOffset = layout.getStartOfImmediateMembers();
  auto size = layout.getSize();
  auto negativeSizeInWords = size.AddressPoint / IGM.getPointerSize();
  auto positiveSizeInWords = size.getOffsetToEnd() / IGM.getPointerSize();

  auto initTy = cast<llvm::StructType>(offsetVar->getValueType());
  auto *init = llvm::ConstantStruct::get(initTy, {
    llvm::ConstantInt::get(IGM.SizeTy, immediateMembersOffset.getValue()),
    llvm::ConstantInt::get(IGM.Int32Ty, negativeSizeInWords),
    llvm::ConstantInt::get(IGM.Int32Ty, positiveSizeInWords)
  });

  offsetVar->setInitializer(init);
  offsetVar->setConstant(true);
}

static Optional<llvm::Constant *>
getAddrOfDestructorFunction(IRGenModule &IGM, ClassDecl *classDecl) {
  auto dtorRef = SILDeclRef(classDecl->getDestructor(),
                            SILDeclRef::Kind::Deallocator);
  SILFunction *dtorFunc = IGM.getSILModule().lookUpFunction(dtorRef);
  if (!dtorFunc) return llvm::None;
  return IGM.getAddrOfSILFunction(dtorFunc, NotForDefinition);
}

static void emitFieldOffsetGlobals(IRGenModule &IGM,
                                   ClassDecl *classDecl,
                                   const ClassLayout &fragileLayout,
                                   const ClassLayout &resilientLayout) {
  for (auto prop : classDecl->getStoredProperties()) {
    auto fieldInfo = fragileLayout.getFieldAccessAndElement(prop);
    auto access = fieldInfo.first;
    auto element = fieldInfo.second;

    llvm::Constant *fieldOffsetOrZero;

    if (element.hasByteOffset()) {
      // Use a fixed offset if we have one.
      fieldOffsetOrZero = IGM.getSize(element.getByteOffset());
    } else {
      // Otherwise, leave a placeholder for the runtime to populate at runtime.
      fieldOffsetOrZero = IGM.getSize(Size(0));
    }

    switch (access) {
    case FieldAccess::ConstantDirect:
    case FieldAccess::NonConstantDirect: {
      // Emit a global variable storing the constant field offset.
      // If the superclass was imported from Objective-C, the offset
      // does not include the superclass size; we rely on the
      // Objective-C runtime sliding it down.
      //
      // TODO: Don't emit the symbol if field has a fixed offset and size
      // in all resilience domains
      auto offsetAddr = IGM.getAddrOfFieldOffset(prop, ForDefinition);
      auto offsetVar = cast<llvm::GlobalVariable>(offsetAddr.getAddress());
      offsetVar->setInitializer(fieldOffsetOrZero);

      // If the offset is constant in the resilient layout, it will not change
      // at runtime, and the global can be true const.
      //
      // If it is constant in the fragile layout only, newer Objective-C
      // runtimes will still update them in place, so make sure to check the
      // correct layout.
      //
      // The one exception to this rule is with empty fields with
      // ObjC-resilient heritage.  The ObjC runtime will attempt to slide
      // these offsets if it slides the rest of the class, and in doing so
      // it will compute a different offset than we computed statically.
      // But this is ultimately unimportant because we do not care about the
      // offset of an empty field.
      auto resilientInfo = resilientLayout.getFieldAccessAndElement(prop);
      if (resilientInfo.first == FieldAccess::ConstantDirect &&
          (!resilientInfo.second.isEmpty() ||
           !resilientLayout.mayRuntimeAssignNonZeroOffsetsToEmptyFields())) {
        // If it is constant in the resilient layout, it should be constant in
        // the fragile layout also.
        assert(access == FieldAccess::ConstantDirect);
        assert(element.hasByteOffset());
        offsetVar->setConstant(true);
      }

      break;
    }

    case FieldAccess::ConstantIndirect:
      // No global variable is needed.
      break;
    }
  }
}

static ClassFlags getClassFlags(ClassDecl *classDecl) {
  auto flags = ClassFlags();

  // Set a flag if the class uses Swift refcounting.
  auto type = classDecl->getDeclaredType()->getCanonicalType();
  if (type->getReferenceCounting() == ReferenceCounting::Native) {
    flags |= ClassFlags::UsesSwiftRefcounting;
  }

  // Set a flag if the class has a custom ObjC name.
  DeclAttributes attrs = classDecl->getAttrs();
  if (auto objc = attrs.getAttribute<ObjCAttr>()) {
    if (objc->getName())
      flags |= ClassFlags::HasCustomObjCName;
  }
  if (attrs.hasAttribute<ObjCRuntimeNameAttr>())
    flags |= ClassFlags::HasCustomObjCName;

  return flags;
}

namespace {
  /// Base class for layout of non-generic class metadata.
  template<class Impl>
  class ClassMetadataBuilderBase : public ClassMetadataVisitor<Impl> {
    using super = ClassMetadataVisitor<Impl>;

  protected:
    using NominalDecl = ClassDecl;
    using super::asImpl;
    using super::IGM;
    using super::Target;
    using super::VTable;

    ConstantStructBuilder &B;

    const ClassLayout &FieldLayout;
    const ClassMetadataLayout &MetadataLayout;

    Size AddressPoint;

  public:
    ClassMetadataBuilderBase(IRGenModule &IGM, ClassDecl *theClass,
                             ConstantStructBuilder &builder,
                             const ClassLayout &fieldLayout)
      : super(IGM, theClass), B(builder),
        FieldLayout(fieldLayout),
        MetadataLayout(IGM.getClassMetadataLayout(theClass)) {}

  public:
    SILType getLoweredType() {
      return IGM.getLoweredType(Target->getDeclaredTypeInContext());
    }

    void noteAddressPoint() {
      ClassMetadataVisitor<Impl>::noteAddressPoint();
      AddressPoint = B.getNextOffsetFromGlobal();
    }

    ClassFlags getClassFlags() { return ::getClassFlags(Target); }

    void addClassFlags() { B.addInt32((uint32_t)asImpl().getClassFlags()); }

    void noteResilientSuperclass() {}

    void noteStartOfImmediateMembers(ClassDecl *theClass) {}

    ConstantReference getValueWitnessTable(bool relativeReference) {
      assert(
          !relativeReference &&
          "Cannot get a relative reference to a class' value witness table.");
      switch (IGM.getClassMetadataStrategy(Target)) {
      case ClassMetadataStrategy::Resilient:
      case ClassMetadataStrategy::Singleton:
        // The runtime fills in the value witness table for us.
        return ConstantReference(
            llvm::ConstantPointerNull::get(IGM.WitnessTablePtrTy),
            swift::irgen::ConstantReference::Direct);

      case ClassMetadataStrategy::Update:
      case ClassMetadataStrategy::FixedOrUpdate:
      case ClassMetadataStrategy::Fixed: {
        // FIXME: Should this check HasImported instead?
        auto type = (Target->checkAncestry(AncestryFlags::ObjC)
                    ? IGM.Context.getAnyObjectType()
                    : IGM.Context.TheNativeObjectType);
        auto wtable = IGM.getAddrOfValueWitnessTable(type);
        return ConstantReference(wtable,
                                 swift::irgen::ConstantReference::Direct);
      }
      }
    }

    void addValueWitnessTable() {
      B.add(asImpl().getValueWitnessTable(false).getValue());
    }

    llvm::Constant *getAddrOfMetaclassObject(ForDefinition_t forDefinition) {
      return IGM.getAddrOfMetaclassObject(Target, forDefinition);
    }

    /// The 'metadata flags' field in a class is actually a pointer to
    /// the metaclass object for the class.
    ///
    /// NONAPPLE: This is only really required for ObjC interop; maybe
    /// suppress this for classes that don't need to be exposed to
    /// ObjC, e.g. for non-Apple platforms?
    void addMetadataFlags() {
      static_assert(unsigned(MetadataKind::Class) == 0,
                    "class metadata kind is non-zero?");

      if (IGM.ObjCInterop) {
        // Get the metaclass pointer as an intptr_t.
        auto metaclass = asImpl().getAddrOfMetaclassObject(NotForDefinition);
        auto flags =
          llvm::ConstantExpr::getPtrToInt(metaclass, IGM.MetadataKindTy);
        B.add(flags);
      } else {
        // On non-objc platforms just fill it with a null, there
        // is no Objective-C metaclass.
        // FIXME: Remove this to save metadata space.
        // rdar://problem/18801263
        B.addInt(IGM.MetadataKindTy, unsigned(MetadataKind::Class));
      }
    }

    llvm::Constant *getSuperclassMetadata() {
      Type type = Target->mapTypeIntoContext(Target->getSuperclass());
      auto *metadata =
          tryEmitConstantHeapMetadataRef(IGM, type->getCanonicalType(),
                                         /*allowUninit*/ false);
      return metadata;
    }

    bool shouldAddNullSuperclass() {
      // If we might have generic ancestry, leave a placeholder since
      // swift_initClassMetadata() will fill in the superclass.
      switch (IGM.getClassMetadataStrategy(Target)) {
      case ClassMetadataStrategy::Resilient:
      case ClassMetadataStrategy::Singleton:
        return true;
      case ClassMetadataStrategy::Update:
      case ClassMetadataStrategy::FixedOrUpdate:
      case ClassMetadataStrategy::Fixed:
        return false;
      }
    }

    void addSuperclass() {
      if (asImpl().shouldAddNullSuperclass()) {
        B.addNullPointer(IGM.TypeMetadataPtrTy);
        return;
      }

      // If this is a root class, use SwiftObject as our formal parent.
      if (!Target->hasSuperclass()) {
        // This is only required for ObjC interoperation.
        if (!IGM.ObjCInterop) {
          B.addNullPointer(IGM.TypeMetadataPtrTy);
          return;
        }

        // We have to do getAddrOfObjCClass ourselves here because
        // the ObjC runtime base needs to be ObjC-mangled but isn't
        // actually imported from a clang module.
        B.add(IGM.getAddrOfObjCClass(
                               IGM.getObjCRuntimeBaseForSwiftRootClass(Target),
                               NotForDefinition));
        return;
      }

      auto *metadata = asImpl().getSuperclassMetadata();
      assert(metadata != nullptr);
      B.add(metadata);
    }

    void addDestructorFunction() {
      if (auto ptr = getAddrOfDestructorFunction(IGM, Target)) {
        B.addSignedPointer(*ptr,
                           IGM.getOptions().PointerAuth.HeapDestructors,
                           PointerAuthEntity::Special::HeapDestructor);
      } else {
        // In case the optimizer removed the function. See comment in
        // addReifiedVTableEntry().
        B.addNullPointer(IGM.FunctionPtrTy);
      }
    }

    void addIVarDestroyer() {
      auto dtorFunc = IGM.getAddrOfIVarInitDestroy(Target,
                                                   /*isDestroyer=*/ true,
                                                   /*isForeign=*/ false,
                                                   NotForDefinition);
      if (dtorFunc) {
        B.addSignedPointer(*dtorFunc,
                           IGM.getOptions().PointerAuth.HeapDestructors,
                           PointerAuthEntity::Special::HeapDestructor);
      } else {
        B.addNullPointer(IGM.FunctionPtrTy);
      }
    }

    llvm::Constant *emitNominalTypeDescriptor() {
      return ClassContextDescriptorBuilder(IGM, Target, RequireMetadata).emit();
    }

    llvm::Constant *getNominalTypeDescriptor() {
      return emitNominalTypeDescriptor();
    }

    void addNominalTypeDescriptor() {
      B.addSignedPointer(asImpl().getNominalTypeDescriptor(),
                         IGM.getOptions().PointerAuth.TypeDescriptors,
                         PointerAuthEntity::Special::TypeDescriptor);
    }

    bool canBeConstant() {
      // TODO: the metadata global can actually be constant in a very
      // special case: it's not a pattern, ObjC interoperation isn't
      // required, there are no class fields, and there is nothing that
      // needs to be runtime-adjusted.
      return false;
    }

    void addInstanceAddressPoint() {
      // Right now, we never allocate fields before the address point.
      B.addInt32(0);
    }

    bool hasFixedLayout() { return FieldLayout.isFixedLayout(); }

    const ClassLayout &getFieldLayout() { return FieldLayout; }

    void addInstanceSize() {
      if (asImpl().hasFixedLayout()) {
        B.addInt32(asImpl().getFieldLayout().getSize().getValue());
      } else {
        // Leave a zero placeholder to be filled at runtime
        B.addInt32(0);
      }
    }
    
    void addInstanceAlignMask() {
      if (asImpl().hasFixedLayout()) {
        B.addInt16(asImpl().getFieldLayout().getAlignMask().getValue());
      } else {
        // Leave a zero placeholder to be filled at runtime
        B.addInt16(0);
      }
    }

    void addRuntimeReservedBits() {
      B.addInt16(0);
    }

    void addClassSize() {
      auto size = MetadataLayout.getSize();
      B.addInt32(size.FullSize.getValue());
    }

    void addClassAddressPoint() {
      // FIXME: Wrong
      auto size = MetadataLayout.getSize();
      B.addInt32(size.AddressPoint.getValue());
    }

    void addClassCacheData() {
      // We initially fill in these fields with addresses taken from
      // the ObjC runtime.
      // FIXME: Remove null data altogether rdar://problem/18801263
      B.add(IGM.getObjCEmptyCachePtr());
      B.add(IGM.getObjCEmptyVTablePtr());
    }

    llvm::Constant *getROData() { return emitClassPrivateData(IGM, Target); }

    uint64_t getClassDataPointerHasSwiftMetadataBits() {
      return IGM.UseDarwinPreStableABIBit ? 1 : 2;
    }

    void addClassDataPointer() {
      if (!IGM.ObjCInterop) {
        // with no Objective-C runtime, just give an empty pointer with the
        // swift bit set.
        // FIXME: Remove null data altogether rdar://problem/18801263
        B.addInt(IGM.IntPtrTy, 1);
        return;
      }

      // Derive the RO-data.
      llvm::Constant *data = asImpl().getROData();

      // Set a low bit to indicate this class has Swift metadata.
      auto bit = llvm::ConstantInt::get(
          IGM.IntPtrTy, asImpl().getClassDataPointerHasSwiftMetadataBits());

      // Emit data + bit.
      data = llvm::ConstantExpr::getPtrToInt(data, IGM.IntPtrTy);
      data = llvm::ConstantExpr::getAdd(data, bit);
      B.add(data);
    }

    void addReifiedVTableEntry(SILDeclRef fn) {
      // Find the vtable entry.
      assert(VTable && "no vtable?!");
      auto entry = VTable->getEntry(IGM.getSILModule(), fn);

      // The class is fragile. Emit a direct reference to the vtable entry.
      llvm::Constant *ptr;
      if (entry) {
        ptr = IGM.getAddrOfSILFunction(entry->getImplementation(),
                                       NotForDefinition);
      } else {
        // The method is removed by dead method elimination.
        // It should be never called. We add a pointer to an error function.
        ptr = llvm::ConstantExpr::getBitCast(IGM.getDeletedMethodErrorFn(),
                                             IGM.FunctionPtrTy);
      }

      auto &schema = IGM.getOptions().PointerAuth.SwiftClassMethods;
      B.addSignedPointer(ptr, schema, fn);
    }

    void addPlaceholder(MissingMemberDecl *m) {
      assert(m->getNumberOfVTableEntries() == 0
             && "cannot generate metadata with placeholders in it");
    }

    void addMethodOverride(SILDeclRef baseRef, SILDeclRef declRef) {}

    void createMetadataAccessFunction() {
      assert(!Target->isGenericContext());
      emitClassMetadataBaseOffset(IGM, Target);
      createNonGenericMetadataAccessFunction(IGM, Target);

      if (IGM.getClassMetadataStrategy(Target) == ClassMetadataStrategy::Fixed)
        return;

      emitMetadataCompletionFunction(
          IGM, Target,
          [&](IRGenFunction &IGF, llvm::Value *metadata,
              MetadataDependencyCollector *collector) {
        emitInitializeClassMetadata(IGF, Target, FieldLayout, metadata,
                                    collector);
      });
    }
  };

  static void
  addFixedFieldOffset(IRGenModule &IGM, ConstantStructBuilder &B, VarDecl *var,
                      std::function<Type(DeclContext *)> typeFromContext) {
    SILType baseType = SILType::getPrimitiveObjectType(
        typeFromContext(var->getDeclContext())->getCanonicalType());
    B.addInt(IGM.SizeTy, getClassFieldOffset(IGM, baseType, var).getValue());
  }

  /// A builder for non-generic class metadata which does not require any
  /// runtime initialization, or that only requires runtime initialization
  /// on newer Objective-C runtimes.
  class FixedClassMetadataBuilder :
      public ClassMetadataBuilderBase<FixedClassMetadataBuilder> {
    using super = ClassMetadataBuilderBase<FixedClassMetadataBuilder>;
    using super::IGM;
    using super::B;

  public:
    FixedClassMetadataBuilder(IRGenModule &IGM, ClassDecl *theClass,
                              ConstantStructBuilder &builder,
                              const ClassLayout &fieldLayout)
      : super(IGM, theClass, builder, fieldLayout) {}

    void addFieldOffset(VarDecl *var) {
      addFixedFieldOffset(IGM, B, var, [](DeclContext *dc) {
        return dc->getDeclaredTypeInContext();
      });
    }

    void addFieldOffsetPlaceholders(MissingMemberDecl *placeholder) {
      llvm_unreachable("Fixed class metadata cannot have missing members");
    }

    void addGenericArgument(GenericRequirement requirement,
                            ClassDecl *forClass) {
      llvm_unreachable("Fixed class metadata cannot have generic parameters");
    }

    void addGenericWitnessTable(GenericRequirement requirement,
                                ClassDecl *forClass) {
      llvm_unreachable("Fixed class metadata cannot have generic requirements");
    }
  };

  /// A builder for non-generic class metadata with resiliently-sized
  /// fields or generic ancestry.
  class SingletonClassMetadataBuilder :
      public ClassMetadataBuilderBase<SingletonClassMetadataBuilder> {
    using NominalDecl = StructDecl;
    using super = ClassMetadataBuilderBase<SingletonClassMetadataBuilder>;
    using super::IGM;
    using super::B;

  public:
    SingletonClassMetadataBuilder(IRGenModule &IGM, ClassDecl *theClass,
                                  ConstantStructBuilder &builder,
                                  const ClassLayout &fieldLayout)
      : super(IGM, theClass, builder, fieldLayout) {}

    void addFieldOffset(VarDecl *var) {
      // Field offsets are either copied from the superclass or calculated
      // at runtime.
      B.addInt(IGM.SizeTy, 0);
    }

    void addFieldOffsetPlaceholders(MissingMemberDecl *placeholder) {
      for (unsigned i = 0,
                    e = placeholder->getNumberOfFieldOffsetVectorEntries();
           i < e; ++i) {
        // Emit placeholder values for some number of stored properties we
        // know exist but aren't able to reference directly.
        B.addInt(IGM.SizeTy, 0);
      }
    }

    void addGenericArgument(GenericRequirement requirement,
                            ClassDecl *forClass) {
      // Filled in at runtime.
      B.addNullPointer(IGM.TypeMetadataPtrTy);
    }

    void addGenericWitnessTable(GenericRequirement requirement,
                                ClassDecl *forClass) {
      // Filled in at runtime.
      B.addNullPointer(IGM.WitnessTablePtrTy);
    }
  };

  /// A builder for metadata patterns for non-generic class with
  /// resilient ancestry.
  class ResilientClassMetadataBuilder {
    IRGenModule &IGM;
    ClassDecl *Target;
    ConstantStructBuilder &B;
    const ClassLayout &FieldLayout;

  public:
    ResilientClassMetadataBuilder(IRGenModule &IGM, ClassDecl *theClass,
                                  ConstantStructBuilder &builder,
                                  const ClassLayout &fieldLayout)
      : IGM(IGM), Target(theClass), B(builder), FieldLayout(fieldLayout) {}

    llvm::Constant *emitNominalTypeDescriptor() {
      return ClassContextDescriptorBuilder(IGM, Target, RequireMetadata).emit();
    }

    void layout() {
      emitNominalTypeDescriptor();

      addRelocationFunction();
      addDestructorFunction();
      addIVarDestroyer();
      addClassFlags();
      addClassDataPointer();
      addMetaclass();
    }

    void addRelocationFunction() {
      // We don't use this yet, but it's available as a future customization
      // point.
      B.addRelativeAddressOrNull(nullptr);
    }

    void addDestructorFunction() {
      auto function = getAddrOfDestructorFunction(IGM, Target);
      B.addRelativeAddressOrNull(function ? *function : nullptr);
    }

    void addIVarDestroyer() {
      auto function = IGM.getAddrOfIVarInitDestroy(Target,
                                                   /*isDestroyer=*/ true,
                                                   /*isForeign=*/ false,
                                                   NotForDefinition);
      B.addRelativeAddressOrNull(function ? *function : nullptr);
    }

    void addClassFlags() {
      B.addInt32((uint32_t) getClassFlags(Target));
    }

    void addClassDataPointer() {
      auto data = (IGM.ObjCInterop
                   ? emitClassPrivateData(IGM, Target)
                   : nullptr);
      B.addRelativeAddressOrNull(data);
    }

    void addMetaclass() {
      auto metaclass = (IGM.ObjCInterop
                        ? IGM.getAddrOfMetaclassObject(Target, NotForDefinition)
                        : nullptr);
      B.addRelativeAddressOrNull(metaclass);
    }

    void createMetadataAccessFunction() {
      assert(IGM.getClassMetadataStrategy(Target)
             == ClassMetadataStrategy::Resilient);

      assert(!Target->isGenericContext());
      emitClassMetadataBaseOffset(IGM, Target);
      createNonGenericMetadataAccessFunction(IGM, Target);

      emitMetadataCompletionFunction(
          IGM, Target,
          [&](IRGenFunction &IGF, llvm::Value *metadata,
              MetadataDependencyCollector *collector) {
        emitInitializeClassMetadata(IGF, Target, FieldLayout, metadata,
                                    collector);
      });
    }
  };

  /// A builder for GenericClassMetadataPattern objects.
  class GenericClassMetadataBuilder :
    public GenericMetadataBuilderBase<GenericClassMetadataBuilder,
                                      ClassDecl>
  {
    using super = GenericMetadataBuilderBase;

    const ClassLayout &FieldLayout;

    Optional<ConstantAggregateBuilderBase::PlaceholderPosition>
      ClassRODataOffset, MetaclassObjectOffset, MetaclassRODataOffset;
  public:
    GenericClassMetadataBuilder(IRGenModule &IGM, ClassDecl *theClass,
                                ConstantStructBuilder &B,
                                const ClassLayout &fieldLayout)
      : super(IGM, theClass, B), FieldLayout(fieldLayout)
    {
      // We need special initialization of metadata objects to trick the ObjC
      // runtime into initializing them.
      HasDependentMetadata = true;
    }

    void layoutHeader() {
      super::layoutHeader();

      // RelativePointer<HeapObjectDestroyer> Destroy;
      addDestructorFunction();

      // RelativePointer<ClassIVarDestroyer> IVarDestroyer;
      addIVarDestroyer();

      // ClassFlags Flags;
      B.addInt32((uint32_t) getClassFlags(Target));

      // uint16_t ClassRODataOffset;
      if (IGM.ObjCInterop)
        ClassRODataOffset = B.addPlaceholderWithSize(IGM.Int16Ty);
      else
        B.addInt16(0);

      // uint16_t MetaclassObjectOffset;
      if (IGM.ObjCInterop)
        MetaclassObjectOffset = B.addPlaceholderWithSize(IGM.Int16Ty);
      else
        B.addInt16(0);

      // uint16_t MetadataRODataOffset;
      if (IGM.ObjCInterop)
        MetaclassRODataOffset = B.addPlaceholderWithSize(IGM.Int16Ty);
      else
        B.addInt16(0);

      // uint16_t Reserved;
      B.addInt16(0);
    }

    llvm::Constant *emitNominalTypeDescriptor() {
      return ClassContextDescriptorBuilder(IGM, Target, RequireMetadata).emit();
    }

    GenericMetadataPatternFlags getPatternFlags() {
      auto flags = super::getPatternFlags();

      flags.class_setHasImmediateMembersPattern(hasImmediateMembersPattern());

      return flags;
    }

    void emitInstantiationDefinitions() {
      // Emit the base-offset variable.
      emitClassMetadataBaseOffset(IGM, Target);

      super::emitInstantiationDefinitions();
    }

    void addDestructorFunction() {
      auto function = getAddrOfDestructorFunction(IGM, Target);
      B.addRelativeAddressOrNull(function ? *function : nullptr);
    }

    void addIVarDestroyer() {
      auto function = IGM.getAddrOfIVarInitDestroy(Target,
                                                   /*isDestroyer=*/ true,
                                                   /*isForeign=*/ false,
                                                   NotForDefinition);
      B.addRelativeAddressOrNull(function ? *function : nullptr);
    }

    bool hasExtraDataPattern() {
      return IGM.ObjCInterop;
    }

    PartialPattern buildExtraDataPattern() {
      ConstantInitBuilder subBuilder(IGM);
      auto subB = subBuilder.beginStruct();
      subB.setPacked(true);

      // The offset of the pattern bytes in the overall extra-data section.
      // Any bytes before this will be zeroed.  Currently we don't take
      // advantage of this.
      Size patternOffset = Size(0);

      if (IGM.ObjCInterop) {
        // Add the metaclass object.
        B.fillPlaceholderWithInt(*MetaclassObjectOffset, IGM.Int16Ty,
          IGM.getOffsetInWords(patternOffset + subB.getNextOffsetFromGlobal()));
        addMetaclassObject(subB);

        // Add the RO-data objects.
        auto roDataPoints =
          emitClassPrivateDataFields(IGM, subB, Target);
        B.fillPlaceholderWithInt(*ClassRODataOffset, IGM.Int16Ty,
          IGM.getOffsetInWords(patternOffset + roDataPoints.first));
        B.fillPlaceholderWithInt(*MetaclassRODataOffset, IGM.Int16Ty,
          IGM.getOffsetInWords(patternOffset + roDataPoints.second));
      }

      auto patternSize = subB.getNextOffsetFromGlobal();

      auto global = subB.finishAndCreateGlobal("", IGM.getPointerAlignment(),
                                               /*constant*/ true);

      return { global, patternOffset, patternSize };
    }

    void addMetaclassObject(ConstantStructBuilder &B) {
      // isa
      ClassDecl *rootClass = getRootClassForMetaclass(IGM, Target);
      auto isa = IGM.getAddrOfMetaclassObject(rootClass, NotForDefinition);
      B.add(isa);
      // super, which is dependent if the superclass is generic
      B.addNullPointer(IGM.ObjCClassPtrTy);
      // cache
      B.add(IGM.getObjCEmptyCachePtr());
      // vtable
      B.add(IGM.getObjCEmptyVTablePtr());
      // rodata, which is always dependent
      B.addInt(IGM.IntPtrTy, 0);
    }

    bool hasImmediateMembersPattern() {
      // TODO: use the real field offsets if they're known statically.
      return false;
    }

    llvm::Value *emitAllocateMetadata(IRGenFunction &IGF,
                                      llvm::Value *descriptor,
                                      llvm::Value *arguments,
                                      llvm::Value *templatePointer) {
      // Sign the descriptor.
      auto schema = IGF.IGM.getOptions().PointerAuth.TypeDescriptorsAsArguments;
      if (schema) {
        auto authInfo = PointerAuthInfo::emit(
            IGF, schema, nullptr,
            PointerAuthEntity::Special::TypeDescriptorAsArgument);
        descriptor = emitPointerAuthSign(IGF, descriptor, authInfo);
      }

      auto metadata =
        IGF.Builder.CreateCall(IGM.getAllocateGenericClassMetadataFn(),
                               {descriptor, arguments, templatePointer});

      return metadata;
    }

    bool hasCompletionFunction() {
      // TODO: recognize cases where this is not required.
      // For example, under ObjCInterop mode we can move class realization
      // into the allocation phase if the superclass is trivial and there's
      // no layout to do.
      return true;
    }

    void emitInitializeMetadata(IRGenFunction &IGF,
                                llvm::Value *metadata,
                                bool isVWTMutable,
                                MetadataDependencyCollector *collector) {
      assert(!HasDependentVWT && "class should never have dependent VWT");
      emitInitializeClassMetadata(IGF, Target, FieldLayout,
                                  metadata, collector);
    }
  };

  template <template <typename> class MetadataBuilderBase, typename Impl>
  class SpecializedGenericNominalMetadataBuilderBase
      : public MetadataBuilderBase<Impl> {
    using super = MetadataBuilderBase<Impl>;

  protected:
    using super::asImpl;
    using super::getLoweredType;
    using super::IGM;
    using super::Target;
    using typename super::NominalDecl;

    CanType type;

  public:
    template <typename... Args>
    SpecializedGenericNominalMetadataBuilderBase(IRGenModule &IGM, CanType type,
                                                 NominalDecl &decl,
                                                 ConstantStructBuilder &B,
                                                 Args... args)
        : super(IGM, &decl, B, args...), type(type) {}

    void noteStartOfTypeSpecificMembers() {}

    llvm::Constant *getNominalTypeDescriptor() {
      return IGM.getAddrOfTypeContextDescriptor(Target, RequireMetadata);
    }

    SILType getLoweredType() { return SILType::getPrimitiveObjectType(type); }

    ConstantReference emitValueWitnessTable(bool relativeReference) {
      return irgen::emitValueWitnessTable(IGM, type, false, relativeReference);
    }

    ConstantReference getValueWitnessTable(bool relativeReference) {
      return emitValueWitnessTable(relativeReference);
    }

    void addGenericArgument(GenericRequirement requirement) {
      auto t = requirement.TypeParameter.subst(genericSubstitutions());
      ConstantReference ref = IGM.getAddrOfTypeMetadata(
          CanType(t), SymbolReferenceKind::Relative_Direct);
      this->B.add(ref.getDirectValue());
    }

    void addGenericWitnessTable(GenericRequirement requirement) {
      auto conformance = genericSubstitutions().lookupConformance(
          requirement.TypeParameter->getCanonicalType(), requirement.Protocol);
      ProtocolConformance *concreteConformance = conformance.getConcrete();

      llvm::Constant *addr;

      Type argument = requirement.TypeParameter.subst(genericSubstitutions());
      auto argumentNominal = argument->getAnyNominal();
      if (argumentNominal && argumentNominal->isGenericContext()) {
        // TODO: Statically specialize the witness table pattern for t's
        //       conformance.
        llvm_unreachable("Statically specializing metadata at generic types is "
                         "not supported.");
      } else {
        RootProtocolConformance *rootConformance =
            concreteConformance->getRootConformance();
        addr = IGM.getAddrOfWitnessTable(rootConformance);
      }

      this->B.add(addr);
    }

    SubstitutionMap genericSubstitutions() {
      return type->getContextSubstitutionMap(IGM.getSwiftModule(),
                                             type->getAnyNominal());
    }

    MetadataTrailingFlags getTrailingFlags() {
      MetadataTrailingFlags flags = super::getTrailingFlags();

      flags.setIsStaticSpecialization(true);
      flags.setIsCanonicalStaticSpecialization(
          irgen::isCanonicalInitializableTypeMetadataStaticallyAddressable(
              IGM, type));

      return flags;
    }
  };

  // FIXME: rdar://problem/58884416:
  //
  //        Without this template typealias, the following errors are produced
  //        when compiling on Linux and Windows, respectively:
  //
  //        template argument for template template parameter must be a class
  //        template or type alias template
  //
  //        invalid template argument for template parameter
  //        'MetadataBuilderBase', expected a class template
  //
  //        Once those issues are resolved, delete this typealias and directly
  //        use ClassMetadataBuilderBase in
  //        SpecializedGenericNominalMetadataBuilderBase.
  template <typename T>
  using WorkaroundRestateClassMetadataBuilderBase = ClassMetadataBuilderBase<T>;

  class SpecializedGenericClassMetadataBuilder
      : public SpecializedGenericNominalMetadataBuilderBase<
            WorkaroundRestateClassMetadataBuilderBase,
            SpecializedGenericClassMetadataBuilder> {
    using super = SpecializedGenericNominalMetadataBuilderBase<
        WorkaroundRestateClassMetadataBuilderBase,
        SpecializedGenericClassMetadataBuilder>;
    using super::type;

    // FIXME: Remove this class's FieldLayout.  The superclass has its own copy,
    //        but it seems to be garbage when it's read.
    const ClassLayout &FieldLayout;

  public:
    SpecializedGenericClassMetadataBuilder(IRGenModule &IGM, CanType type,
                                           ClassDecl &decl,
                                           ConstantStructBuilder &B,
                                           const ClassLayout &fieldLayout)
        : super(IGM, type, decl, B, fieldLayout), FieldLayout(fieldLayout) {}

    void addGenericArgument(GenericRequirement requirement,
                            ClassDecl *theClass) {
      super::addGenericArgument(requirement);
    }

    void addGenericWitnessTable(GenericRequirement requirement,
                                ClassDecl *theClass) {
      super::addGenericWitnessTable(requirement);
    }

    void addFieldOffsetPlaceholders(MissingMemberDecl *placeholder) {
      llvm_unreachable(
          "Prespecialized generic class metadata cannot have missing members");
    }

    void addFieldOffset(VarDecl *var) {
      addFixedFieldOffset(IGM, B, var, [&](DeclContext *dc) {
        return dc->mapTypeIntoContext(type);
      });
    }

    llvm::Constant *getAddrOfMetaclassObject(ForDefinition_t forDefinition) {
      return IGM.getAddrOfCanonicalSpecializedGenericMetaclassObject(
          type, forDefinition);
    }

    bool shouldAddNullSuperclass() { return false; }

    llvm::Constant *getSuperclassMetadata() {
      Type superclass = type->getSuperclass(/*useArchetypes=*/false);
      auto *metadata =
          IGM.getAddrOfTypeMetadata(superclass->getCanonicalType());
      return metadata;
    }

    uint64_t getClassDataPointerHasSwiftMetadataBits() {
      return super::getClassDataPointerHasSwiftMetadataBits() | 2;
    }

    llvm::Constant *getROData() {
      return emitSpecializedGenericClassPrivateData(IGM, Target, type);
    }

    ClassFlags getClassFlags() {
      auto flags = super::getClassFlags();

      flags |= ClassFlags::IsStaticSpecialization;
      flags |= ClassFlags::IsCanonicalStaticSpecialization;

      return flags;
    }

    bool hasFixedLayout() { return true; }

    const ClassLayout &getFieldLayout() { return FieldLayout; }
  };
} // end anonymous namespace

/// Emit the ObjC-compatible class symbol for a class.
/// Since LLVM and many system linkers do not have a notion of relative symbol
/// references, we emit the symbol as a global asm block.
static void emitObjCClassSymbol(IRGenModule &IGM,
                                ClassDecl *classDecl,
                                llvm::Constant *metadata) {
  auto entity = LinkEntity::forObjCClass(classDecl);
  LinkInfo link = LinkInfo::get(IGM, entity, ForDefinition);

  // Create the alias.
  auto *ptrTy = cast<llvm::PointerType>(metadata->getType());
  auto *alias = llvm::GlobalAlias::create(
      ptrTy->getElementType(), ptrTy->getAddressSpace(), link.getLinkage(),
      link.getName(), metadata, &IGM.Module);
  ApplyIRLinkage({link.getLinkage(), link.getVisibility(), link.getDLLStorage()})
      .to(alias, link.isForDefinition());
}

/// Emit the type metadata or metadata template for a class.
void irgen::emitClassMetadata(IRGenModule &IGM, ClassDecl *classDecl,
                              const ClassLayout &fragileLayout,
                              const ClassLayout &resilientLayout) {
  assert(!classDecl->isForeign());
  PrettyStackTraceDecl stackTraceRAII("emitting metadata for", classDecl);

  emitFieldOffsetGlobals(IGM, classDecl, fragileLayout, resilientLayout);

  // Set up a dummy global to stand in for the metadata object while we produce
  // relative references.
  ConstantInitBuilder builder(IGM);
  auto init = builder.beginStruct();
  init.setPacked(true);

  bool canBeConstant;

  auto strategy = IGM.getClassMetadataStrategy(classDecl);

  switch (strategy) {
  case ClassMetadataStrategy::Resilient: {
    if (classDecl->isGenericContext()) {
      GenericClassMetadataBuilder builder(IGM, classDecl, init,
                                          resilientLayout);
      builder.layout();
      canBeConstant = true;

      builder.createMetadataAccessFunction();
      break;
    }

    ResilientClassMetadataBuilder builder(IGM, classDecl, init,
                                          resilientLayout);
    builder.layout();
    canBeConstant = true;

    builder.createMetadataAccessFunction();
    break;
  }

  case ClassMetadataStrategy::Singleton:
  case ClassMetadataStrategy::Update: {
    SingletonClassMetadataBuilder builder(IGM, classDecl, init,
                                          resilientLayout);
    builder.layout();
    canBeConstant = builder.canBeConstant();

    builder.createMetadataAccessFunction();
    break;
  }

  case ClassMetadataStrategy::FixedOrUpdate:
  case ClassMetadataStrategy::Fixed: {
    FixedClassMetadataBuilder builder(IGM, classDecl, init,
                                      fragileLayout);
    builder.layout();
    canBeConstant = builder.canBeConstant();

    builder.createMetadataAccessFunction();
    break;
  }
  }

  CanType declaredType = classDecl->getDeclaredType()->getCanonicalType();

  StringRef section{};
  if (classDecl->isObjC() &&
      IGM.TargetInfo.OutputObjectFormat == llvm::Triple::MachO)
    section = "__DATA,__objc_data, regular";

  bool isPattern = (strategy == ClassMetadataStrategy::Resilient);
  auto var = IGM.defineTypeMetadata(declaredType, isPattern, canBeConstant,
                                    init.finishAndCreateFuture(), section);

  // If the class does not require dynamic initialization, or if it only
  // requires dynamic initialization on a newer Objective-C runtime, add it
  // to the Objctive-C class list.
  if (IGM.ObjCInterop) {
    switch (strategy) {
    case ClassMetadataStrategy::Resilient:
      // Even non-@objc classes can have Objective-C categories attached, so
      // we always emit a resilient class stub as long as -enable-objc-interop
      // is set.
      if (IGM.hasObjCResilientClassStub(classDecl)) {
        IGM.emitObjCResilientClassStub(classDecl);

        if (classDecl->isObjC()) {
          auto *stub = IGM.getAddrOfObjCResilientClassStub(
              classDecl, NotForDefinition,
              TypeMetadataAddress::AddressPoint);
          emitObjCClassSymbol(IGM, classDecl, stub);
        }
      }
      break;
    case ClassMetadataStrategy::Singleton:
      break;
    
    case ClassMetadataStrategy::Update:
    case ClassMetadataStrategy::FixedOrUpdate:
    case ClassMetadataStrategy::Fixed:
      if (classDecl->isObjC())
        emitObjCClassSymbol(IGM, classDecl, var);

      IGM.addObjCClass(var,
                classDecl->getAttrs().hasAttribute<ObjCNonLazyRealizationAttr>());
      break;
    }
  }
}

void irgen::emitSpecializedGenericClassMetadata(IRGenModule &IGM, CanType type,
                                                ClassDecl &decl) {
  assert(decl.isGenericContext());
  assert(IGM.getClassMetadataStrategy(&decl) ==
         ClassMetadataStrategy::Resilient);
  auto &context = type->getNominalOrBoundGenericNominal()->getASTContext();
  auto ty = type.getPointer();
  PrettyStackTraceType stackTraceRAII(
      context, "emitting prespecialized class metadata for", ty);

  SILType loweredType = SILType::getPrimitiveObjectType(type);
  auto &classTI = IGM.getTypeInfo(loweredType).as<ClassTypeInfo>();

  // Use the fragile layout when emitting metadata.
  auto &fragileLayout =
      classTI.getClassLayout(IGM, loweredType, /*forBackwardDeployment=*/true);

  ConstantInitBuilder initBuilder(IGM);
  auto init = initBuilder.beginStruct();
  init.setPacked(true);

  SpecializedGenericClassMetadataBuilder builder(IGM, type, decl, init,
                                                 fragileLayout);
  builder.layout();

  IGM.defineTypeMetadata(type, /*isPattern=*/false,
                         // Class metadata cannot be constant when Objective-C
                         // interop is enabled.  The reason is that the
                         // Objective-C runtime writes to the Swift metadata
                         // record during class realization.
                         /*canBeConstant=*/!IGM.ObjCInterop,
                         init.finishAndCreateFuture());
}

llvm::Value *IRGenFunction::emitInvariantLoad(Address address,
                                              const llvm::Twine &name) {
  auto load = Builder.CreateLoad(address, name);
  setInvariantLoad(load);
  return load;
}

void IRGenFunction::setInvariantLoad(llvm::LoadInst *load) {
  load->setMetadata(IGM.InvariantMetadataID, IGM.InvariantNode);
}

void IRGenFunction::setDereferenceableLoad(llvm::LoadInst *load,
                                           unsigned size) {
  auto sizeConstant = llvm::ConstantInt::get(IGM.Int64Ty, size);
  auto sizeNode = llvm::MDNode::get(IGM.getLLVMContext(),
                                  llvm::ConstantAsMetadata::get(sizeConstant));
  load->setMetadata(IGM.DereferenceableID, sizeNode);
}

/// Emit a load from the given metadata at a constant index.
///
/// The load is marked invariant. This function should not be called
/// on metadata objects that are in the process of being initialized.
static llvm::LoadInst *
emitInvariantLoadFromMetadataAtIndex(IRGenFunction &IGF,
                                     llvm::Value *metadata,
                                     llvm::Value **slotPtr,
                                     int index,
                                     llvm::Type *objectTy,
                               const Twine &suffix = Twine::createNull()) {
  auto result = emitLoadFromMetadataAtIndex(IGF, metadata, slotPtr,
                                            index, objectTy, suffix);
  IGF.setInvariantLoad(result);
  return result;
}

/// Given a type metadata pointer, load its value witness table.
llvm::Value *
IRGenFunction::emitValueWitnessTableRefForMetadata(llvm::Value *metadata) {
  auto witness = emitInvariantLoadFromMetadataAtIndex(*this, metadata, nullptr,
                                                      -1, IGM.WitnessTablePtrTy,
                                                      ".valueWitnesses");
  // A value witness table is dereferenceable to the number of value witness
  // pointers.
  
  // TODO: If we know the type statically has extra inhabitants, we know
  // there are more witnesses.
  auto numValueWitnesses
    = unsigned(ValueWitness::Last_RequiredValueWitness) + 1;
  setDereferenceableLoad(witness,
                         IGM.getPointerSize().getValue() * numValueWitnesses);
  return witness;
}

/// Given a lowered SIL type, load a value witness table that represents its
/// layout.
llvm::Value *
IRGenFunction::emitValueWitnessTableRef(SILType type,
                                        llvm::Value **metadataSlot) {
  return emitValueWitnessTableRef(type, MetadataState::Complete, metadataSlot);
}

llvm::Value *
IRGenFunction::emitValueWitnessTableRef(SILType type,
                                        DynamicMetadataRequest request,
                                        llvm::Value **metadataSlot) {
  assert(request.canResponseStatusBeIgnored());
  assert(!request.isStaticallyAbstract() &&
         "cannot make an abstract request for a value witness table");

  // See if we have a cached projection we can use.
  if (auto cached = tryGetLocalTypeDataForLayout(type,
                                  LocalTypeDataKind::forValueWitnessTable())) {
    if (metadataSlot)
      *metadataSlot = emitTypeMetadataRefForLayout(type, request);
    return cached;
  }
  
  auto metadata = emitTypeMetadataRefForLayout(type, request);
  if (metadataSlot) *metadataSlot = metadata;
  auto vwtable = emitValueWitnessTableRefForMetadata(metadata);
  setScopedLocalTypeDataForLayout(type,
                                  LocalTypeDataKind::forValueWitnessTable(),
                                  vwtable);
  return vwtable;
}

//===----------------------------------------------------------------------===//
// Value types (structs and enums)
//===----------------------------------------------------------------------===//

namespace {
  /// A helper class for laying out value metadata.
  template <class Base>
  class ValueMetadataBuilderBase : public Base {
  protected:
    using Base::IGM;
    using Base::Target;
    using Base::asImpl;

    using Base::Base;

  public:
    SILType getLoweredType() {
      return IGM.getLoweredType(Target->getDeclaredTypeInContext());
    }

    /// Create the runtime data structures and functions necessary to
    /// support in-place metadata initialization on this type.
    void maybeCreateSingletonMetadataInitialization() {
      if (!needsSingletonMetadataInitialization(IGM, Target))
        return;

      emitMetadataCompletionFunction(IGM, Target,
        [&](IRGenFunction &IGF, llvm::Value *metadata,
            MetadataDependencyCollector *collector) {
        emitInitializeValueMetadata(IGF, Target, metadata,
                                    /*vwt mutable*/true, collector);
      });
    }
  };
}


//===----------------------------------------------------------------------===//
// Structs
//===----------------------------------------------------------------------===//

namespace {
  /// An adapter for laying out struct metadata.
  template <class Impl>
  class StructMetadataBuilderBase
         : public ValueMetadataBuilderBase<StructMetadataVisitor<Impl>> {
    using super = ValueMetadataBuilderBase<StructMetadataVisitor<Impl>>;

    bool HasUnfilledFieldOffset = false;

  protected:
    ConstantStructBuilder &B;
    using NominalDecl = StructDecl;
    using super::IGM;
    using super::Target;
    using super::asImpl;
    using super::getLoweredType;

    StructMetadataBuilderBase(IRGenModule &IGM, StructDecl *theStruct,
                              ConstantStructBuilder &B)
      : super(IGM, theStruct), B(B) {
    }

  public:
    void noteStartOfTypeSpecificMembers() {}

    void addMetadataFlags() {
      B.addInt(IGM.MetadataKindTy, unsigned(getMetadataKind(Target)));
    }

    llvm::Constant *emitNominalTypeDescriptor() {
      auto descriptor =
        StructContextDescriptorBuilder(IGM, Target, RequireMetadata).emit();
      return descriptor;
    }

    llvm::Constant *getNominalTypeDescriptor() {
      return emitNominalTypeDescriptor();
    }

    void addNominalTypeDescriptor() {
      auto descriptor = asImpl().getNominalTypeDescriptor();
      B.addSignedPointer(descriptor,
                         IGM.getOptions().PointerAuth.TypeDescriptors,
                         PointerAuthEntity::Special::TypeDescriptor);
    }

    ConstantReference emitValueWitnessTable(bool relativeReference) {
      auto type = this->Target->getDeclaredType()->getCanonicalType();
      return irgen::emitValueWitnessTable(IGM, type, false, relativeReference);
    }

    ConstantReference getValueWitnessTable(bool relativeReference) {
      return emitValueWitnessTable(relativeReference);
    }

    void addValueWitnessTable() {
      B.add(asImpl().getValueWitnessTable(false).getValue());
    }

    void addFieldOffset(VarDecl *var) {
      assert(var->hasStorage() &&
             "storing field offset for computed property?!");
      SILType structType = asImpl().getLoweredType();

      llvm::Constant *offset =
        emitPhysicalStructMemberFixedOffset(IGM, structType, var);
      // If we have a fixed offset, add it. Otherwise, leave zero as a
      // placeholder.
      if (offset) {
        B.add(offset);
      } else {
        asImpl().flagUnfilledFieldOffset();
        B.addInt(IGM.Int32Ty, 0);
      }
    }

    void noteEndOfFieldOffsets() {
      B.addAlignmentPadding(super::IGM.getPointerAlignment());
    }

    void addGenericArgument(GenericRequirement requirement) {
      llvm_unreachable("Concrete type metadata cannot have generic parameters");
    }

    void addGenericWitnessTable(GenericRequirement requirement) {
      llvm_unreachable("Concrete type metadata cannot have generic requirements");
    }

    bool hasTrailingFlags() {
      return IGM.shouldPrespecializeGenericMetadata();
    }

    void addTrailingFlags() {
      auto flags = asImpl().getTrailingFlags();

      B.addInt(IGM.Int64Ty, flags.getOpaqueValue());
    }

    MetadataTrailingFlags getTrailingFlags() {
      MetadataTrailingFlags flags;

      return flags;
    }

    void flagUnfilledFieldOffset() {
      HasUnfilledFieldOffset = true;
    }

    bool canBeConstant() {
      return !HasUnfilledFieldOffset;
    }
  };

  class StructMetadataBuilder
      : public StructMetadataBuilderBase<StructMetadataBuilder> {
  public:
    StructMetadataBuilder(IRGenModule &IGM, StructDecl *theStruct,
                          ConstantStructBuilder &B)
        : StructMetadataBuilderBase(IGM, theStruct, B) {}

    void createMetadataAccessFunction() {
      createNonGenericMetadataAccessFunction(IGM, Target);
      maybeCreateSingletonMetadataInitialization();
    }
  };

  /// Emit a value witness table for a fixed-layout generic type, or a template
  /// if the value witness table is dependent on generic parameters.
  static ConstantReference
  getValueWitnessTableForGenericValueType(IRGenModule &IGM,
                                          NominalTypeDecl *decl,
                                          bool &dependent) {
    CanType unboundType
      = decl->getDeclaredType()->getCanonicalType();
    
    dependent = hasDependentValueWitnessTable(IGM, unboundType);
    return emitValueWitnessTable(IGM, unboundType, dependent,
                                 /*relative reference*/ true);
  }
  
  /// A builder for metadata templates.
  class GenericStructMetadataBuilder :
    public GenericValueMetadataBuilderBase<GenericStructMetadataBuilder,
                                           StructDecl> {
    using super = GenericValueMetadataBuilderBase;

  public:
    GenericStructMetadataBuilder(IRGenModule &IGM, StructDecl *theStruct,
                                 ConstantStructBuilder &B)
      : super(IGM, theStruct, B) {}

    llvm::Value *emitAllocateMetadata(IRGenFunction &IGF,
                                      llvm::Value *descriptor,
                                      llvm::Value *arguments,
                                      llvm::Value *templatePointer) {
      auto &layout = IGM.getMetadataLayout(Target);
      auto extraSize = layout.getSize().getOffsetToEnd()
                         - IGM.getOffsetOfStructTypeSpecificMetadataMembers();
      auto extraSizeV = IGM.getSize(extraSize);

      // Sign the descriptor.
      auto schema = IGF.IGM.getOptions().PointerAuth.TypeDescriptorsAsArguments;
      if (schema) {
        auto authInfo = PointerAuthInfo::emit(
            IGF, schema, nullptr,
            PointerAuthEntity::Special::TypeDescriptorAsArgument);
        descriptor = emitPointerAuthSign(IGF, descriptor, authInfo);
      }

      return IGF.Builder.CreateCall(IGM.getAllocateGenericValueMetadataFn(),
                                    {descriptor, arguments, templatePointer,
                                     extraSizeV});
    }

    void flagUnfilledFieldOffset() {
      // We just assume this might happen.
    }

    llvm::Constant *emitNominalTypeDescriptor() {
      return StructContextDescriptorBuilder(IGM, Target, RequireMetadata).emit();
    }

    GenericMetadataPatternFlags getPatternFlags() {
      auto flags = super::getPatternFlags();

      if (hasTrailingFlags()) {
        flags.setHasTrailingFlags(true);
      }

      return flags;
    }

    ConstantReference emitValueWitnessTable(bool relativeReference) {
      assert(relativeReference && "should only relative reference");
      return getValueWitnessTableForGenericValueType(IGM, Target,
                                                     HasDependentVWT);
    }

    bool hasTrailingFlags() {
      return IGM.shouldPrespecializeGenericMetadata();
    }

    bool hasKnownFieldOffsets() {
      auto &ti = IGM.getTypeInfo(getLoweredType());
      if (!isa<FixedTypeInfo>(ti))
        return false;

      if (Target->getStoredProperties().empty())
        return false;

      return true;
    }

    bool hasExtraDataPattern() {
      return hasKnownFieldOffsets() || hasTrailingFlags();
    }

    /// If present, the extra data pattern consists of one or both of the
    /// following:
    ///
    /// - the field offset vector
    /// - the trailing flags
    PartialPattern buildExtraDataPattern() {
      ConstantInitBuilder builder(IGM);
      auto init = builder.beginStruct();

      struct Scanner : StructMetadataScanner<Scanner> {
        GenericStructMetadataBuilder &Outer;
        SILType Type;
        ConstantStructBuilder &B;
        Scanner(GenericStructMetadataBuilder &outer, IRGenModule &IGM, StructDecl *target, SILType type,
                ConstantStructBuilder &B)
            : StructMetadataScanner(IGM, target), Outer(outer), Type(type), B(B) {}

        void addFieldOffset(VarDecl *field) {
          if (!Outer.hasKnownFieldOffsets()) {
            return;
          }
          auto offset = emitPhysicalStructMemberFixedOffset(IGM, Type, field);
          if (offset) {
            B.add(offset);
            return;
          }
          assert(IGM.getTypeInfo(
                        Type.getFieldType(field, IGM.getSILModule(),
                                          TypeExpansionContext::minimal()))
                     .isKnownEmpty(ResilienceExpansion::Maximal));
          B.addInt32(0);
        }

        void noteEndOfFieldOffsets() {
          B.addAlignmentPadding(IGM.getPointerAlignment());
        }

        void addTrailingFlags() { B.addInt64(0); }
      };
      Scanner(*this, IGM, Target, getLoweredType(), init).layout();
      Size structSize = init.getNextOffsetFromGlobal();

      auto global = init.finishAndCreateGlobal("", IGM.getPointerAlignment(),
                                               /*constant*/ true);

      auto &layout = IGM.getMetadataLayout(Target);

      bool offsetUpToTrailingFlags = hasTrailingFlags() && !hasKnownFieldOffsets(); 
      Size zeroingStart = IGM.getOffsetOfStructTypeSpecificMetadataMembers();
      Offset zeroingEnd = offsetUpToTrailingFlags 
                            ? layout.getTrailingFlagsOffset()
                            : layout.getFieldOffsetVectorOffset();
      return { global,
               zeroingEnd.getStatic()
                 - zeroingStart,
               structSize };
    }

    bool hasCompletionFunction() {
      return !isa<FixedTypeInfo>(IGM.getTypeInfo(getLoweredType()));
    }
  };

  // FIXME: rdar://problem/58884416:
  //
  //        Without this template typealias, the following errors are produced
  //        when compiling on Linux and Windows, respectively:
  //
  //        template argument for template template parameter must be a class
  //        template or type alias template
  //
  //        invalid template argument for template parameter
  //        'MetadataBuilderBase', expected a class template
  //
  //        Once those issues are resolved, delete this typealias and directly
  //        use StructMetadataBuilderBase in
  //        SpecializedGenericNominalMetadataBuilderBase.
  template <typename T>
  using WorkaroundRestateStructMetadataBuilderBase =
      StructMetadataBuilderBase<T>;

  class SpecializedGenericStructMetadataBuilder
      : public SpecializedGenericNominalMetadataBuilderBase<
            WorkaroundRestateStructMetadataBuilderBase,
            SpecializedGenericStructMetadataBuilder> {
    using super = SpecializedGenericNominalMetadataBuilderBase<
        WorkaroundRestateStructMetadataBuilderBase,
        SpecializedGenericStructMetadataBuilder>;

  public:
    SpecializedGenericStructMetadataBuilder(IRGenModule &IGM, CanType type,
                                            StructDecl &decl,
                                            ConstantStructBuilder &B)
        : super(IGM, type, decl, B) {}
  };

} // end anonymous namespace

/// Emit the type metadata or metadata template for a struct.
void irgen::emitStructMetadata(IRGenModule &IGM, StructDecl *structDecl) {
  PrettyStackTraceDecl stackTraceRAII("emitting metadata for", structDecl);
  ConstantInitBuilder initBuilder(IGM);
  auto init = initBuilder.beginStruct();
  init.setPacked(true);

  bool isPattern;
  bool canBeConstant;
  if (structDecl->isGenericContext()) {
    GenericStructMetadataBuilder builder(IGM, structDecl, init);
    builder.layout();
    isPattern = true;
    canBeConstant = true;

    builder.createMetadataAccessFunction();
  } else {
    StructMetadataBuilder builder(IGM, structDecl, init);
    builder.layout();
    isPattern = false;
    canBeConstant = builder.canBeConstant();

    builder.createMetadataAccessFunction();
  }

  CanType declaredType = structDecl->getDeclaredType()->getCanonicalType();

  IGM.defineTypeMetadata(declaredType, isPattern, canBeConstant,
                         init.finishAndCreateFuture());
}

void irgen::emitSpecializedGenericStructMetadata(IRGenModule &IGM, CanType type,
                                                 StructDecl &decl) {
  Type ty = type.getPointer();
  auto &context = type->getNominalOrBoundGenericNominal()->getASTContext();
  PrettyStackTraceType stackTraceRAII(
      context, "emitting prespecialized metadata for", ty);
  ConstantInitBuilder initBuilder(IGM);
  auto init = initBuilder.beginStruct();
  init.setPacked(true);

  bool isPattern = false;

  SpecializedGenericStructMetadataBuilder builder(IGM, type, decl, init);
  builder.layout();

  bool canBeConstant = builder.canBeConstant();
  IGM.defineTypeMetadata(type, isPattern, canBeConstant,
                         init.finishAndCreateFuture());
}

// Enums

static Optional<Size> getConstantPayloadSize(IRGenModule &IGM,
                                             EnumDecl *enumDecl,
                                             CanType enumTy) {
  auto &enumTI = IGM.getTypeInfoForUnlowered(enumTy);
  if (!enumTI.isFixedSize(ResilienceExpansion::Maximal)) {
    return None;
  }

  assert((!enumTI.isFixedSize(ResilienceExpansion::Minimal) || enumDecl->isGenericContext()) &&
         "non-generic, non-resilient enums don't need payload size in metadata");
  auto &strategy = getEnumImplStrategy(IGM, enumTy);
  return Size(strategy.getPayloadSizeForMetadata());
}

static Optional<Size> getConstantPayloadSize(IRGenModule &IGM,
                                             EnumDecl *enumDecl) {
  auto enumTy = enumDecl->getDeclaredTypeInContext()->getCanonicalType();
  return getConstantPayloadSize(IGM, enumDecl, enumTy);
}

namespace {

  template<class Impl>
  class EnumMetadataBuilderBase
         : public ValueMetadataBuilderBase<EnumMetadataVisitor<Impl>> {
    using super = ValueMetadataBuilderBase<EnumMetadataVisitor<Impl>>;
    bool HasUnfilledPayloadSize = false;

  protected:
    using NominalDecl = EnumDecl;
    ConstantStructBuilder &B;
    using super::asImpl;
    using super::IGM;
    using super::Target;

    EnumMetadataBuilderBase(IRGenModule &IGM, EnumDecl *theEnum,
                            ConstantStructBuilder &B)
      : super(IGM, theEnum), B(B) {
    }

  public:
    void noteStartOfTypeSpecificMembers() {}

    void addMetadataFlags() {
      B.addInt(IGM.MetadataKindTy, unsigned(getMetadataKind(Target)));
    }

    ConstantReference emitValueWitnessTable(bool relativeReference) {
      auto type = Target->getDeclaredType()->getCanonicalType();
      return irgen::emitValueWitnessTable(IGM, type, false, relativeReference);
    }

    ConstantReference getValueWitnessTable(bool relativeReference) {
      return emitValueWitnessTable(relativeReference);
    }

    void addValueWitnessTable() {
      B.add(asImpl().getValueWitnessTable(false).getValue());
    }

    llvm::Constant *emitNominalTypeDescriptor() {
      auto descriptor =
        EnumContextDescriptorBuilder(IGM, Target, RequireMetadata).emit();
      return descriptor;
    }

    llvm::Constant *getNominalTypeDescriptor() {
      return emitNominalTypeDescriptor();
    }

    void addNominalTypeDescriptor() {
      B.addSignedPointer(asImpl().getNominalTypeDescriptor(),
                         IGM.getOptions().PointerAuth.TypeDescriptors,
                         PointerAuthEntity::Special::TypeDescriptor);
    }

    void addGenericArgument(GenericRequirement requirement) {
      llvm_unreachable("Concrete type metadata cannot have generic parameters");
    }

    void addGenericWitnessTable(GenericRequirement requirement) {
      llvm_unreachable("Concrete type metadata cannot have generic requirements");
    }

    bool hasTrailingFlags() { return IGM.shouldPrespecializeGenericMetadata(); }

    void addTrailingFlags() {
      auto flags = asImpl().getTrailingFlags();

      B.addInt(IGM.Int64Ty, flags.getOpaqueValue());
    }

    MetadataTrailingFlags getTrailingFlags() {
      MetadataTrailingFlags flags;

      return flags;
    }

    Optional<Size> getPayloadSize() {
      return getConstantPayloadSize(IGM, Target);
    }

    void addPayloadSize() {
      auto payloadSize = asImpl().getPayloadSize();
      if (!payloadSize) {
        B.addInt(IGM.IntPtrTy, 0);
        HasUnfilledPayloadSize = true;
        return;
      }

      B.addInt(IGM.IntPtrTy, payloadSize->getValue());
    }

    bool canBeConstant() {
      return !HasUnfilledPayloadSize;
    }
  };

  // FIXME: rdar://problem/58884416
  //
  //        Without this template typealias, the following errors are produced
  //        when compiling on Linux and Windows, respectively:
  //
  //        template argument for template template parameter must be a class
  //        template or type alias template
  //
  //        invalid template argument for template parameter
  //        'MetadataBuilderBase', expected a class template
  //
  //        Once those issues are resolved, delete this typealias and directly
  //        use EnumMetadataBuilderBase in
  //        SpecializedGenericNominalMetadataBuilderBase.
  template <typename T>
  using WorkaroundRestateEnumMetadataBuilderBase = EnumMetadataBuilderBase<T>;

  class SpecializedGenericEnumMetadataBuilder
      : public SpecializedGenericNominalMetadataBuilderBase<
            WorkaroundRestateEnumMetadataBuilderBase,
            SpecializedGenericEnumMetadataBuilder> {

    using super = SpecializedGenericNominalMetadataBuilderBase<
        WorkaroundRestateEnumMetadataBuilderBase,
        SpecializedGenericEnumMetadataBuilder>;

    CanType type;

  public:
    SpecializedGenericEnumMetadataBuilder(IRGenModule &IGM, CanType type,
                                          EnumDecl &decl,
                                          ConstantStructBuilder &B)
        : super(IGM, type, decl, B), type(type){};

    Optional<Size> getPayloadSize() {
      return getConstantPayloadSize(IGM, Target, type);
    }
  };

  class EnumMetadataBuilder
      : public EnumMetadataBuilderBase<EnumMetadataBuilder> {
  public:
    EnumMetadataBuilder(IRGenModule &IGM, EnumDecl *theEnum,
                        ConstantStructBuilder &B)
        : EnumMetadataBuilderBase(IGM, theEnum, B) {}

    void createMetadataAccessFunction() {
      createNonGenericMetadataAccessFunction(IGM, Target);
      maybeCreateSingletonMetadataInitialization();
    }
  };

  class GenericEnumMetadataBuilder
    : public GenericValueMetadataBuilderBase<GenericEnumMetadataBuilder,
                                             EnumDecl> {
    using super = GenericValueMetadataBuilderBase;

  public:
    GenericEnumMetadataBuilder(IRGenModule &IGM, EnumDecl *theEnum,
                               ConstantStructBuilder &B)
      : super(IGM, theEnum, B) {}

    llvm::Value *emitAllocateMetadata(IRGenFunction &IGF,
                                      llvm::Value *descriptor,
                                      llvm::Value *arguments,
                                      llvm::Value *templatePointer) {
      auto &layout = IGM.getMetadataLayout(Target);
      auto extraSize = layout.getSize().getOffsetToEnd()
                         - IGM.getOffsetOfEnumTypeSpecificMetadataMembers();
      auto extraSizeV = IGM.getSize(extraSize);

      // Sign the descriptor.
      auto schema = IGF.IGM.getOptions().PointerAuth.TypeDescriptorsAsArguments;
      if (schema) {
        auto authInfo = PointerAuthInfo::emit(
            IGF, schema, nullptr,
            PointerAuthEntity::Special::TypeDescriptorAsArgument);
        descriptor = emitPointerAuthSign(IGF, descriptor, authInfo);
      }

      return
        IGF.Builder.CreateCall(IGM.getAllocateGenericValueMetadataFn(),
                               {descriptor, arguments, templatePointer,
                                extraSizeV});
    }

    bool hasTrailingFlags() {
      return IGM.shouldPrespecializeGenericMetadata();
    }

    bool hasKnownPayloadSize() {
      auto &layout = IGM.getMetadataLayout(Target);
      return layout.hasPayloadSizeOffset() && (bool)getConstantPayloadSize(IGM, Target);
    }

    bool hasExtraDataPattern() {
      return hasKnownPayloadSize() || hasTrailingFlags();
    }

    /// If present, the extra data pattern consists of one or both of the
    /// following:
    ///
    /// - the payload-size
    /// - the trailing flags
    PartialPattern buildExtraDataPattern() {
      ConstantInitBuilder builder(IGM);
      auto init = builder.beginStruct();

      auto &layout = IGM.getMetadataLayout(Target);

      if (layout.hasPayloadSizeOffset()) {
        if (auto size = getConstantPayloadSize(IGM, Target)) {
          init.addSize(*size);
        }
      }
      if (hasTrailingFlags()) {
        init.addInt64(0);
      }
      Size structSize = init.getNextOffsetFromGlobal();

      auto global = init.finishAndCreateGlobal("", IGM.getPointerAlignment(),
                                               /*constant*/ true);

      bool offsetUpToTrailingFlags = hasTrailingFlags() && !hasKnownPayloadSize(); 
      Size zeroingStart = IGM.getOffsetOfEnumTypeSpecificMetadataMembers();
      Offset zeroingEnd = offsetUpToTrailingFlags 
                            ? layout.getTrailingFlagsOffset()
                            : layout.getPayloadSizeOffset();
      return { global,
               zeroingEnd.getStatic()
                 - zeroingStart,
               structSize };
    }

    llvm::Constant *emitNominalTypeDescriptor() {
      return EnumContextDescriptorBuilder(IGM, Target, RequireMetadata).emit();
    }

    GenericMetadataPatternFlags getPatternFlags() {
      auto flags = super::getPatternFlags();

      if (hasTrailingFlags()) {
        flags.setHasTrailingFlags(true);
      }

      return flags;
    }

    ConstantReference emitValueWitnessTable(bool relativeReference) {
      assert(relativeReference && "should only relative reference");
      return getValueWitnessTableForGenericValueType(IGM, Target,
                                                     HasDependentVWT);
    }

    bool hasCompletionFunction() {
      return !isa<FixedTypeInfo>(IGM.getTypeInfo(getLoweredType()));
    }
  };

} // end anonymous namespace

void irgen::emitEnumMetadata(IRGenModule &IGM, EnumDecl *theEnum) {
  PrettyStackTraceDecl stackTraceRAII("emitting metadata for", theEnum);
  ConstantInitBuilder initBuilder(IGM);
  auto init = initBuilder.beginStruct();
  init.setPacked(true);
  
  bool isPattern;
  bool canBeConstant;
  if (theEnum->isGenericContext()) {
    GenericEnumMetadataBuilder builder(IGM, theEnum, init);
    builder.layout();
    isPattern = true;
    canBeConstant = true;

    builder.createMetadataAccessFunction();
  } else {
    EnumMetadataBuilder builder(IGM, theEnum, init);
    builder.layout();
    isPattern = false;
    canBeConstant = builder.canBeConstant();

    builder.createMetadataAccessFunction();
  }

  CanType declaredType = theEnum->getDeclaredType()->getCanonicalType();

  IGM.defineTypeMetadata(declaredType, isPattern, canBeConstant,
                         init.finishAndCreateFuture());
}

void irgen::emitSpecializedGenericEnumMetadata(IRGenModule &IGM, CanType type,
                                               EnumDecl &decl) {
  assert(decl.isGenericContext());
  Type ty = type.getPointer();
  auto &context = type->getNominalOrBoundGenericNominal()->getASTContext();
  PrettyStackTraceType stackTraceRAII(
      context, "emitting prespecialized metadata for", ty);
  ConstantInitBuilder initBuilder(IGM);
  auto init = initBuilder.beginStruct();
  init.setPacked(true);

  SpecializedGenericEnumMetadataBuilder builder(IGM, type, decl, init);
  builder.layout();

  bool canBeConstant = builder.canBeConstant();
  IGM.defineTypeMetadata(type, /*isPattern=*/false, canBeConstant,
                         init.finishAndCreateFuture());
}

llvm::Value *IRGenFunction::emitObjCSelectorRefLoad(StringRef selector) {
  llvm::Constant *loadSelRef = IGM.getAddrOfObjCSelectorRef(selector);
  llvm::Value *loadSel =
    Builder.CreateLoad(Address(loadSelRef, IGM.getPointerAlignment()));

  // When generating JIT'd code, we need to call sel_registerName() to force
  // the runtime to unique the selector. For non-JIT'd code, the linker will
  // do it for us.
  if (IGM.IRGen.Opts.UseJIT) {
    loadSel = Builder.CreateCall(IGM.getObjCSelRegisterNameFn(), loadSel);
  }

  return loadSel;
}

//===----------------------------------------------------------------------===//
// Foreign types
//===----------------------------------------------------------------------===//

namespace {
  /// An adapter that turns a metadata layout class into a foreign metadata
  /// layout class.
  ///
  /// Foreign metadata is generated for declarations that are
  /// synthesized by the Clang importer from C declarations, meaning they don't
  /// have a single Swift binary that is responsible for their emission.
  /// In this case, we emit the record into every binary that needs it, with
  /// a header with a unique identifier string that the runtime can use to pick
  /// the first-used instance as the canonical instance for a process.
  template<typename Impl, typename Base>
  class ForeignMetadataBuilderBase : public Base {
    using super = Base;

  protected:
    using super::IGM;
    using super::Target;
    using super::asImpl;
    using super::B;

    template <class... T>
    ForeignMetadataBuilderBase(T &&...args) : super(std::forward<T>(args)...) {}

    Size AddressPoint = Size::invalid();
    bool CanBeConstant = true;

  public:
    void noteAddressPoint() {
      AddressPoint = B.getNextOffsetFromGlobal();
    }

    bool canBeConstant() {
      return CanBeConstant;
    }

    Size getOffsetOfAddressPoint() const { return AddressPoint; }

    void createMetadataAccessFunction() {
      if (asImpl().needsMetadataCompletionFunction())
        asImpl().createMetadataCompletionFunction();

      auto type = cast<NominalType>(asImpl().getTargetType());

      (void) createTypeMetadataAccessFunction(IGM, type, CacheStrategy::Lazy,
                                              [&](IRGenFunction &IGF,
                                                  DynamicMetadataRequest request,
                                                llvm::Constant *cacheVariable) {
        auto candidate = IGF.IGM.getAddrOfTypeMetadata(type);
        auto call = IGF.Builder.CreateCall(IGF.IGM.getGetForeignTypeMetadataFn(),
                                           {request.get(IGF), candidate});
        call->addAttribute(llvm::AttributeList::FunctionIndex,
                           llvm::Attribute::NoUnwind);
        call->addAttribute(llvm::AttributeList::FunctionIndex,
                           llvm::Attribute::ReadNone);

        return MetadataResponse::handle(IGF, request, call);
      });
    }

    bool needsMetadataCompletionFunction() {
      return needsForeignMetadataCompletionFunction(IGM, Target);
    }

    void createMetadataCompletionFunction() {
      // Note that we can't call this until we've finished laying out the
      // metadata because otherwise we'll try to reenter when we ask for
      // the metadata candidate.
      emitMetadataCompletionFunction(IGM, Target,
        [&](IRGenFunction &IGF, llvm::Value *metadata,
            MetadataDependencyCollector *collector) {
        asImpl().emitInitializeMetadata(IGF, metadata, collector);
      });
    }
  };

  class ForeignClassMetadataBuilder;
  class ForeignClassMetadataBuilderBase :
      public ForeignClassMetadataVisitor<ForeignClassMetadataBuilder> {
  protected:
    ConstantStructBuilder &B;

    ForeignClassMetadataBuilderBase(IRGenModule &IGM, ClassDecl *target,
                                    ConstantStructBuilder &B)
      : ForeignClassMetadataVisitor(IGM, target), B(B) {}
  };

  /// A builder for ForeignClassMetadata.
  class ForeignClassMetadataBuilder :
      public ForeignMetadataBuilderBase<ForeignClassMetadataBuilder,
                                        ForeignClassMetadataBuilderBase> {
  public:
    ForeignClassMetadataBuilder(IRGenModule &IGM, ClassDecl *target,
                                ConstantStructBuilder &B)
        : ForeignMetadataBuilderBase(IGM, target, B) {
      if (IGM.getOptions().LazyInitializeClassMetadata)
        CanBeConstant = false;
    }

    void emitInitializeMetadata(IRGenFunction &IGF, llvm::Value *metadata,
                                MetadataDependencyCollector *collector) {
      if (!Target->hasSuperclass()) {
        assert(IGM.getOptions().LazyInitializeClassMetadata &&
               "should have superclass if not lazy initializing class metadata");
        return;
      }

      // Emit a reference to the superclass.
      auto superclass = IGF.emitAbstractTypeMetadataRef(
                                   Target->getSuperclass()->getCanonicalType());

      // Dig out the address of the superclass field and store.
      auto &layout = IGF.IGM.getForeignMetadataLayout(Target);
      Address addr(metadata, IGM.getPointerAlignment());
      addr = IGF.Builder.CreateElementBitCast(addr, IGM.TypeMetadataPtrTy);
      auto superclassField =
        createPointerSizedGEP(IGF, addr,
                              layout.getSuperClassOffset().getStaticOffset());
      IGF.Builder.CreateStore(superclass, superclassField);
    }

    // Visitor methods.

    void addValueWitnessTable() {
      // The runtime will fill in the default VWT during allocation for the
      // foreign class metadata.
      //
      // As of Swift 5.1, the runtime will fill in a default VWT during
      // allocation of foreign class metadata.  We rely on this for correctness
      // on COFF, where we can't necessarily reference the stanard VWT from the
      // metadata candidate, but it is a good optimization everywhere.
      //
      // The default VWT uses ObjC-compatible reference counting if ObjC interop
      // is enabled and Swift-compatible reference counting otherwise.  That is
      // currently always good enough for foreign classes, so we can
      // unconditionally rely on the default VWT.
      //
      // FIXME: take advantage of this on other targets when targeting a
      // sufficiently recent runtime.
      if (IGM.getOptions().LazyInitializeClassMetadata)
        return B.addNullPointer(IGM.WitnessTablePtrTy);

      // Without Objective-C interop, foreign classes must still use
      // Swift native reference counting.
      auto type = (IGM.ObjCInterop
                   ? IGM.Context.getAnyObjectType()
                   : IGM.Context.TheNativeObjectType);
      auto wtable = IGM.getAddrOfValueWitnessTable(type);
      B.add(wtable);
    }

    void addMetadataFlags() {
      B.addInt(IGM.MetadataKindTy, (unsigned) MetadataKind::ForeignClass);
    }

    void addNominalTypeDescriptor() {
      auto descriptor =
        ClassContextDescriptorBuilder(this->IGM, Target, RequireMetadata).emit();
      B.addSignedPointer(descriptor,
                         IGM.getOptions().PointerAuth.TypeDescriptors,
                         PointerAuthEntity::Special::TypeDescriptor);
    }

    void addSuperclass() {
      // Always leave the superclass pointer unfilled.  We'll have to
      // unique it during initialization anyway, so we might as well spare
      // ourselves the load-time work.
      B.addNullPointer(IGM.TypeMetadataPtrTy);

      // But remember if we might need to change it.
      if (Target->hasSuperclass())
        CanBeConstant = false;
    }

    void addReservedWord() {
      B.addNullPointer(IGM.Int8PtrTy);
    }
  };
  
  /// A builder for ForeignStructMetadata.
  class ForeignStructMetadataBuilder :
    public ForeignMetadataBuilderBase<ForeignStructMetadataBuilder,
                      StructMetadataBuilderBase<ForeignStructMetadataBuilder>>
  {
  public:
    ForeignStructMetadataBuilder(IRGenModule &IGM, StructDecl *target,
                                 ConstantStructBuilder &builder)
        : ForeignMetadataBuilderBase(IGM, target, builder) {}
    
    CanType getTargetType() const {
      return Target->getDeclaredType()->getCanonicalType();
    }

    void createMetadataCompletionFunction() {
      llvm_unreachable("foreign structs never require completion");
    }

    void addValueWitnessTable() {
      B.add(emitValueWitnessTable(/*relative*/ false).getValue());
    }

    void flagUnfilledFieldOffset() {
      llvm_unreachable("foreign type with non-fixed layout?");
    }
  };
  
  /// A builder for ForeignEnumMetadata.
  class ForeignEnumMetadataBuilder :
    public ForeignMetadataBuilderBase<ForeignEnumMetadataBuilder,
                      EnumMetadataBuilderBase<ForeignEnumMetadataBuilder>>
  {
  public:
    ForeignEnumMetadataBuilder(IRGenModule &IGM, EnumDecl *target,
                               ConstantStructBuilder &builder)
      : ForeignMetadataBuilderBase(IGM, target, builder) {}
    
    CanType getTargetType() const {
      return Target->getDeclaredType()->getCanonicalType();
    }

    void createMetadataCompletionFunction() {
      llvm_unreachable("foreign enums never require completion");
    }

    void addValueWitnessTable() {
      B.add(emitValueWitnessTable(/*relative*/ false).getValue());
    }
    
    void addPayloadSize() const {
      llvm_unreachable("nongeneric enums shouldn't need payload size in metadata");
    }
  };
} // end anonymous namespace

bool irgen::requiresForeignTypeMetadata(CanType type) {
  if (NominalTypeDecl *nominal = type->getAnyNominal()) {
    return requiresForeignTypeMetadata(nominal);
  }

  return false;
}

bool irgen::requiresForeignTypeMetadata(NominalTypeDecl *decl) {
  if (auto *clas = dyn_cast<ClassDecl>(decl)) {
    switch (clas->getForeignClassKind()) {
    case ClassDecl::ForeignKind::Normal:
    case ClassDecl::ForeignKind::RuntimeOnly:
      return false;
    case ClassDecl::ForeignKind::CFType:
      return true;
    }
    llvm_unreachable("bad foreign class kind");
  }

  return isa<ClangModuleUnit>(decl->getModuleScopeContext()) &&
    !isa<ProtocolDecl>(decl);
}

void irgen::emitForeignTypeMetadata(IRGenModule &IGM, NominalTypeDecl *decl) {
  auto type = decl->getDeclaredType()->getCanonicalType();

  // Create a temporary base for relative references.
  ConstantInitBuilder builder(IGM);
  auto init = builder.beginStruct();
  init.setPacked(true);

  if (auto classDecl = dyn_cast<ClassDecl>(decl)) {
    assert(classDecl->getForeignClassKind() == ClassDecl::ForeignKind::CFType);

    ForeignClassMetadataBuilder builder(IGM, classDecl, init);
    builder.layout();

    IGM.defineTypeMetadata(type, /*isPattern=*/false,
                           builder.canBeConstant(),
                           init.finishAndCreateFuture());
    builder.createMetadataAccessFunction();
  } else if (auto structDecl = dyn_cast<StructDecl>(decl)) {
    assert(isa<ClangModuleUnit>(structDecl->getModuleScopeContext()));

    ForeignStructMetadataBuilder builder(IGM, structDecl, init);
    builder.layout();

    IGM.defineTypeMetadata(type, /*isPattern=*/false,
                           builder.canBeConstant(),
                           init.finishAndCreateFuture());
    builder.createMetadataAccessFunction();
  } else if (auto enumDecl = dyn_cast<EnumDecl>(decl)) {
    assert(enumDecl->hasClangNode());
    
    ForeignEnumMetadataBuilder builder(IGM, enumDecl, init);
    builder.layout();

    IGM.defineTypeMetadata(type, /*isPattern=*/false,
                           builder.canBeConstant(),
                           init.finishAndCreateFuture());
    builder.createMetadataAccessFunction();
  } else {
    llvm_unreachable("foreign metadata for unexpected type?!");
  }
}

// Protocols

/// Get the runtime identifier for a special protocol, if any.
SpecialProtocol irgen::getSpecialProtocolID(ProtocolDecl *P) {
  auto known = P->getKnownProtocolKind();
  if (!known)
    return SpecialProtocol::None;
  switch (*known) {
  case KnownProtocolKind::Error:
    return SpecialProtocol::Error;
    
  // The other known protocols aren't special at runtime.
  case KnownProtocolKind::Sequence:
  case KnownProtocolKind::IteratorProtocol:
  case KnownProtocolKind::RawRepresentable:
  case KnownProtocolKind::Equatable:
  case KnownProtocolKind::Hashable:
  case KnownProtocolKind::CaseIterable:
  case KnownProtocolKind::Comparable:
  case KnownProtocolKind::SIMDScalar:
  case KnownProtocolKind::BinaryInteger:
  case KnownProtocolKind::ObjectiveCBridgeable:
  case KnownProtocolKind::DestructorSafeContainer:
  case KnownProtocolKind::SwiftNewtypeWrapper:
  case KnownProtocolKind::ExpressibleByArrayLiteral:
  case KnownProtocolKind::ExpressibleByBooleanLiteral:
  case KnownProtocolKind::ExpressibleByDictionaryLiteral:
  case KnownProtocolKind::ExpressibleByExtendedGraphemeClusterLiteral:
  case KnownProtocolKind::ExpressibleByFloatLiteral:
  case KnownProtocolKind::ExpressibleByIntegerLiteral:
  case KnownProtocolKind::ExpressibleByStringInterpolation:
  case KnownProtocolKind::ExpressibleByStringLiteral:
  case KnownProtocolKind::ExpressibleByNilLiteral:
  case KnownProtocolKind::ExpressibleByUnicodeScalarLiteral:
  case KnownProtocolKind::ExpressibleByColorLiteral:
  case KnownProtocolKind::ExpressibleByImageLiteral:
  case KnownProtocolKind::ExpressibleByFileReferenceLiteral:
  case KnownProtocolKind::ExpressibleByBuiltinBooleanLiteral:
  case KnownProtocolKind::ExpressibleByBuiltinExtendedGraphemeClusterLiteral:
  case KnownProtocolKind::ExpressibleByBuiltinFloatLiteral:
  case KnownProtocolKind::ExpressibleByBuiltinIntegerLiteral:
  case KnownProtocolKind::ExpressibleByBuiltinStringLiteral:
  case KnownProtocolKind::ExpressibleByBuiltinUnicodeScalarLiteral:
  case KnownProtocolKind::OptionSet:
  case KnownProtocolKind::BridgedNSError:
  case KnownProtocolKind::BridgedStoredNSError:
  case KnownProtocolKind::CFObject:
  case KnownProtocolKind::ErrorCodeProtocol:
  case KnownProtocolKind::CodingKey:
  case KnownProtocolKind::Encodable:
  case KnownProtocolKind::Decodable:
  case KnownProtocolKind::StringInterpolationProtocol:
  case KnownProtocolKind::AdditiveArithmetic:
  case KnownProtocolKind::Differentiable:
  case KnownProtocolKind::FloatingPoint:
  case KnownProtocolKind::Actor:
    return SpecialProtocol::None;
  }

  llvm_unreachable("Not a valid KnownProtocolKind.");
}

/// Emit global structures associated with the given protocol. This comprises
/// the protocol descriptor, and for ObjC interop, references to the descriptor
/// that the ObjC runtime uses for uniquing.
void IRGenModule::emitProtocolDecl(ProtocolDecl *protocol) {
  PrettyStackTraceDecl stackTraceRAII("emitting metadata for", protocol);

  // Emit remote reflection metadata for the protocol.
  emitFieldDescriptor(protocol);

  // If the protocol is Objective-C-compatible, go through the path that
  // produces an ObjC-compatible protocol_t.
  if (protocol->isObjC()) {
    // In JIT mode, we need to create protocol descriptors using the ObjC
    // runtime in JITted code.
    if (IRGen.Opts.UseJIT)
      return;
    
    // Native ObjC protocols are emitted on-demand in ObjC and uniqued by the
    // runtime; we don't need to try to emit a unique descriptor symbol for them.
    if (protocol->hasClangNode())
      return;
    
    getObjCProtocolGlobalVars(protocol);
    return;
  }

  SILDefaultWitnessTable *defaultWitnesses = nullptr;
  if (isResilient(protocol, ResilienceExpansion::Minimal))
    defaultWitnesses = getSILModule().lookUpDefaultWitnessTable(protocol);

  {
    ProtocolDescriptorBuilder builder(*this, protocol, defaultWitnesses);
    builder.emit();
  }

  // Note that we emitted this protocol.
  SwiftProtocols.push_back(protocol);
}

//===----------------------------------------------------------------------===//
// Generic requirements.
//===----------------------------------------------------------------------===//

/// Add a generic requirement to the given constant struct builder.
static void addGenericRequirement(IRGenModule &IGM, ConstantStructBuilder &B,
                                  GenericRequirementsMetadata &metadata,
                                  GenericSignature sig,
                                  GenericRequirementFlags flags,
                                  Type paramType,
                                  llvm::function_ref<void ()> addReference) {
  if (flags.hasKeyArgument())
    ++metadata.NumGenericKeyArguments;
  if (flags.hasExtraArgument())
    ++metadata.NumGenericExtraArguments;

  B.addInt(IGM.Int32Ty, flags.getIntValue());
  auto typeName =
      IGM.getTypeRef(paramType, nullptr, MangledTypeRefRole::Metadata).first;
  B.addRelativeAddress(typeName);
  addReference();
}

GenericRequirementsMetadata irgen::addGenericRequirements(
                                   IRGenModule &IGM, ConstantStructBuilder &B,
                                   GenericSignature sig,
                                   ArrayRef<Requirement> requirements) {
  assert(sig);
  GenericRequirementsMetadata metadata;
  for (auto &requirement : requirements) {
    ++metadata.NumRequirements;

    switch (auto kind = requirement.getKind()) {
    case RequirementKind::Layout:
      switch (auto layoutKind =
                requirement.getLayoutConstraint()->getKind()) {
      case LayoutConstraintKind::Class: {
        // Encode the class constraint.
        auto flags = GenericRequirementFlags(GenericRequirementKind::Layout,
                                             /*key argument*/ false,
                                             /*extra argument*/ false);
        addGenericRequirement(IGM, B, metadata, sig, flags,
                              requirement.getFirstType(),
         [&]{ B.addInt32((uint32_t)GenericRequirementLayoutKind::Class); });
        break;
      }
      default:
        // No other layout constraints are supported in source-level Swift
        // today.
        llvm_unreachable("shouldn't show up in ABI");
      }
      break;

    case RequirementKind::Conformance: {
      auto protocol = requirement.getSecondType()->castTo<ProtocolType>()
        ->getDecl();
      bool needsWitnessTable =
        Lowering::TypeConverter::protocolRequiresWitnessTable(protocol);
      auto flags = GenericRequirementFlags(GenericRequirementKind::Protocol,
                                           /*key argument*/needsWitnessTable,
                                           /*extra argument*/false);
      auto descriptorRef =
        IGM.getConstantReferenceForProtocolDescriptor(protocol);
      addGenericRequirement(IGM, B, metadata, sig, flags,
                            requirement.getFirstType(),
        [&]{
          unsigned tag = unsigned(descriptorRef.isIndirect());
          if (protocol->isObjC())
            tag |= 0x02;
          
          B.addTaggedRelativeOffset(IGM.RelativeAddressTy,
                                    descriptorRef.getValue(),
                                    tag);
        });
      break;
    }

    case RequirementKind::SameType:
    case RequirementKind::Superclass: {
      auto abiKind = kind == RequirementKind::SameType
        ? GenericRequirementKind::SameType
        : GenericRequirementKind::BaseClass;

      auto flags = GenericRequirementFlags(abiKind, false, false);
      auto typeName =
          IGM.getTypeRef(requirement.getSecondType(), nullptr,
                         MangledTypeRefRole::Metadata).first;

      addGenericRequirement(IGM, B, metadata, sig, flags,
                            requirement.getFirstType(),
        [&]{ B.addRelativeAddress(typeName); });

      // ABI TODO: Same type and superclass constraints also imply
      // "same conformance" constraints on any protocol requirements of
      // the constrained type, which we should emit.
      break;
    }
    }
  }

  return metadata;
}

//===----------------------------------------------------------------------===//
// Other metadata.
//===----------------------------------------------------------------------===//

llvm::Value *irgen::emitMetatypeInstanceType(IRGenFunction &IGF,
                                             llvm::Value *metatypeMetadata) {
  // The instance type field of MetatypeMetadata is immediately after
  // the isa field.
  return emitInvariantLoadFromMetadataAtIndex(IGF, metatypeMetadata, nullptr, 1,
                                              IGF.IGM.TypeMetadataPtrTy);
}

void IRGenModule::emitOpaqueTypeDecl(OpaqueTypeDecl *D) {
  // Emit the opaque type descriptor.
  OpaqueTypeDescriptorBuilder(*this, D).emit();
}

bool irgen::methodRequiresReifiedVTableEntry(IRGenModule &IGM,
                                             const SILVTable *vtable,
                                             SILDeclRef method) {
  Optional<SILVTable::Entry> entry
    = vtable->getEntry(IGM.getSILModule(), method);
  LLVM_DEBUG(llvm::dbgs() << "looking at vtable:\n";
             vtable->print(llvm::dbgs()));
  if (!entry) {
    LLVM_DEBUG(llvm::dbgs() << "vtable entry in "
                            << vtable->getClass()->getName()
                            << " for ";
               method.print(llvm::dbgs());
               llvm::dbgs() << " is not available\n");
    return true;
  }
  LLVM_DEBUG(llvm::dbgs() << "entry: ";
             entry->print(llvm::dbgs());
             llvm::dbgs() << "\n");
  
  // We may be able to elide the vtable entry, ABI permitting, if it's not
  // overridden.
  if (!entry->isNonOverridden()) {
    LLVM_DEBUG(llvm::dbgs() << "vtable entry in "
                            << vtable->getClass()->getName()
                            << " for ";
               method.print(llvm::dbgs());
               llvm::dbgs() << " is overridden\n");
    return true;
  }
  
  // Does the ABI require a vtable entry to exist? If the class the vtable
  // entry originates from is public,
  // and it's either marked fragile or part of a non-resilient module, then
  // other modules will directly address vtable offsets and we can't remove
  // vtable entries.
  auto originatingClass =
    cast<ClassDecl>(method.getOverriddenVTableEntry().getDecl()->getDeclContext());

  if (originatingClass->getEffectiveAccess() >= AccessLevel::Public) {
    // If the class is public,
    // and it's either marked fragile or part of a non-resilient module, then
    // other modules will directly address vtable offsets and we can't remove
    // vtable entries.
    if (!originatingClass->isResilient()) {
      LLVM_DEBUG(llvm::dbgs() << "vtable entry in "
                              << vtable->getClass()->getName()
                              << " for ";
                 method.print(llvm::dbgs());
                 llvm::dbgs() << " originates from a public fragile class\n");
      return true;
    }
  }
    
  // Otherwise, we can leave this method out of the runtime vtable.
  LLVM_DEBUG(llvm::dbgs() << "vtable entry in "
                          << vtable->getClass()->getName()
                          << " for ";
             method.print(llvm::dbgs());
             llvm::dbgs() << " can be elided\n");
  return false;
}
