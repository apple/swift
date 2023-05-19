//===--- MetadataRequest.cpp - IR generation for metadata requests --------===//
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
//  This file implements IR generation for accessing metadata.
//
//===----------------------------------------------------------------------===//

#include "MetadataRequest.h"

#include "Callee.h"
#include "ConstantBuilder.h"
#include "Explosion.h"
#include "ExtendedExistential.h"
#include "FixedTypeInfo.h"
#include "GenArchetype.h"
#include "GenClass.h"
#include "GenMeta.h"
#include "GenPack.h"
#include "GenPointerAuth.h"
#include "GenProto.h"
#include "GenType.h"
#include "GenericArguments.h"
#include "GenericRequirement.h"
#include "IRGenDebugInfo.h"
#include "IRGenFunction.h"
#include "IRGenMangler.h"
#include "IRGenModule.h"
#include "swift/AST/ASTContext.h"
#include "swift/AST/CanTypeVisitor.h"
#include "swift/AST/DiagnosticsIRGen.h"
#include "swift/AST/ExistentialLayout.h"
#include "swift/AST/GenericEnvironment.h"
#include "swift/AST/IRGenOptions.h"
#include "swift/AST/SubstitutionMap.h"
#include "swift/ClangImporter/ClangModule.h"
#include "swift/IRGen/Linking.h"
#include "swift/SIL/FormalLinkage.h"
#include "swift/SIL/TypeLowering.h"
#include "llvm/ADT/SmallSet.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/IR/Constant.h"
#include "llvm/IR/GlobalVariable.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/FormatVariadic.h"
#include <algorithm>

using namespace swift;
using namespace irgen;

llvm::Value *DynamicMetadataRequest::get(IRGenFunction &IGF) const {
  if (isStatic()) {
    return IGF.IGM.getSize(Size(StaticRequest.getOpaqueValue()));
  } else {
    return DynamicRequest;
  }
}

llvm::Value *DynamicMetadataRequest::getRequiredState(IRGenFunction &IGF) const{
  if (isStatic()) {
    return IGF.IGM.getSize(Size(size_t(StaticRequest.getState())));
  }

  auto request = DynamicRequest;

  static_assert(MetadataRequest::State_bit == 0,
                "code below is not doing any shifts");

  uint32_t mask =
    ((uint32_t(1) << MetadataRequest::State_width) - 1);
  auto requiredState =
    IGF.Builder.CreateAnd(request,
                          llvm::ConstantInt::get(IGF.IGM.SizeTy, mask));
  return requiredState;
}

MetadataResponse MetadataResponse::getUndef(IRGenFunction &IGF) {
  return forComplete(llvm::UndefValue::get(IGF.IGM.TypeMetadataPtrTy));
}

MetadataResponse
MetadataResponse::handle(IRGenFunction &IGF, DynamicMetadataRequest request,
                         llvm::Value *pair) {
  assert(pair->getType() == IGF.IGM.TypeMetadataResponseTy);

  // If the request is statically known to produce a complete result,
  // we never even need to extract the status value.
  if (request.isStaticallyBlockingComplete()) {
    auto value = IGF.Builder.CreateExtractValue(pair, 0);
    return MetadataResponse::forComplete(value);
  }

  // Otherwise, split the response.
  auto split = IGF.Builder.CreateSplit<2>(pair);

  // If the request has a collector installed, check the dependency now.
  if (auto collector = request.getDependencyCollector()) {
    collector->checkDependency(IGF, request, split[0], split[1]);
  }

  // Compute the static lower bound on the metadata's dynamic state.
  // This will include any refinements from having branched for the
  // dependency collector.
  auto staticBound = request.getStaticLowerBoundOnResponseState();

  auto response = MetadataResponse(split[0], split[1], staticBound);
  return response;
}

llvm::Value *MetadataResponse::combine(IRGenFunction &IGF) const {
  assert(isValid());
  assert(hasDynamicState() && "cannot combine response without dynamic state");
  return IGF.Builder.CreateCombine(IGF.IGM.TypeMetadataResponseTy,
                                   {Metadata, getDynamicState()});
}

void MetadataResponse::ensureDynamicState(IRGenFunction &IGF) & {
  assert(isValid());

  // If we already have a dynamic state, bail out.
  if (hasDynamicState()) return;

  // If we're statically known complete, we can just fill in
  // MetadataState::Complete.
  if (isStaticallyKnownComplete()) {
    DynamicState = getCompletedState(IGF.IGM);
    return;
  }

  // Otherwise, we need to check the state dynamically.  Do a non-blocking
  // request for complete metadata.
  auto request = MetadataRequest(MetadataState::Complete,
                                 /*non-blocking*/ true);
  *this = emitGetTypeMetadataDynamicState(IGF, request, Metadata);
}

llvm::Constant *MetadataResponse::getCompletedState(IRGenModule &IGM) {
  return IGM.getSize(Size(size_t(MetadataState::Complete)));
}

llvm::Value *MetadataDependency::combine(IRGenFunction &IGF) const {
  if (isTrivial()) {
    return getTrivialCombinedDependency(IGF.IGM);
  }

  return IGF.Builder.CreateCombine(IGF.IGM.TypeMetadataDependencyTy,
                                   {RequiredMetadata, RequiredState});
}

llvm::Constant *
MetadataDependency::getTrivialCombinedDependency(IRGenModule &IGM) {
  return llvm::ConstantAggregateZero::get(IGM.TypeMetadataDependencyTy);
}

void MetadataDependencyCollector::checkDependency(IRGenFunction &IGF,
                                              DynamicMetadataRequest request,
                                                  llvm::Value *metadata,
                                                  llvm::Value *metadataState) {
  // Having either both or neither of the PHIs is normal.
  // Having just RequiredState means that we already finalized this collector
  // and shouldn't be using it anymore.
  assert((!RequiredMetadata || RequiredState) &&
         "checking dependencies on a finished collector");

  // If the request is statically always satisfied, the operation cannot
  // have failed.
  if (request.isStaticallyAlwaysSatisfied())
    return;

  // Otherwise, we need to pull out the response state and compare it against
  // the request state.
  llvm::Value *requiredState = request.getRequiredState(IGF);

  // More advanced metadata states are lower numbers.
  static_assert(MetadataStateIsReverseOrdered,
                "relying on the ordering of MetadataState here");
  auto satisfied = IGF.Builder.CreateICmpULE(metadataState, requiredState);

  emitCheckBranch(IGF, satisfied, metadata, requiredState);
}

void MetadataDependencyCollector::collect(IRGenFunction &IGF,
                                          llvm::Value *dependency) {
  // Having either both or neither of the PHIs is normal.
  // Having just RequiredState means that we already finalized this collector
  // and shouldn't be using it anymore.
  assert((!RequiredMetadata || RequiredState) &&
         "checking dependencies on a finished collector");

  assert(dependency->getType() == IGF.IGM.TypeMetadataDependencyTy);

  // Split the dependency.
  auto metadata = IGF.Builder.CreateExtractValue(dependency, 0);
  auto requiredState = IGF.Builder.CreateExtractValue(dependency, 1);

  // We have a dependency if the metadata is non-null; otherwise we're
  // satisfied and can continue.
  auto satisfied = IGF.Builder.CreateIsNull(metadata);
  emitCheckBranch(IGF, satisfied, metadata, requiredState);
}

void MetadataDependencyCollector::emitCheckBranch(IRGenFunction &IGF,
                                                  llvm::Value *satisfied,
                                                  llvm::Value *metadata,
                                                  llvm::Value *requiredState) {
  // Lazily create the final continuation block and phis.
  if (!RequiredMetadata) {
    auto contBB = IGF.createBasicBlock("metadata-dependencies.cont");
    RequiredMetadata =
      llvm::PHINode::Create(IGF.IGM.TypeMetadataPtrTy, 4, "", contBB);
    RequiredState = llvm::PHINode::Create(IGF.IGM.SizeTy, 4, "", contBB);
  }

  // Conditionally branch to the final continuation block.
  auto satisfiedBB = IGF.createBasicBlock("dependency-satisfied");
  auto curBB = IGF.Builder.GetInsertBlock();
  RequiredMetadata->addIncoming(metadata, curBB);
  RequiredState->addIncoming(requiredState, curBB);
  IGF.Builder.CreateCondBr(satisfied, satisfiedBB,
                           RequiredMetadata->getParent());

  // Otherwise resume emitting code on the main path.
  IGF.Builder.emitBlock(satisfiedBB);
}

MetadataDependency MetadataDependencyCollector::finish(IRGenFunction &IGF) {
  assert((!RequiredMetadata || RequiredState) &&
         "finishing an already-finished collector");

  // If we never branched with a dependency, the result is trivial.
  if (RequiredMetadata == nullptr)
    return MetadataDependency();

  llvm::BasicBlock *curBB = IGF.Builder.GetInsertBlock();
  assert(curBB);
  auto contBB = RequiredMetadata->getParent();
  IGF.Builder.CreateBr(contBB);
  RequiredMetadata->addIncoming(
    llvm::ConstantPointerNull::get(IGF.IGM.TypeMetadataPtrTy),
                                curBB);
  RequiredState->addIncoming(llvm::ConstantInt::get(IGF.IGM.SizeTy, 0), curBB);

  IGF.Builder.emitBlock(contBB);

  auto result = MetadataDependency(RequiredMetadata, RequiredState);

  // Clear RequiredMetadata to tell the destructor that we finished.
  // We leave RequiredState in place so that we can detect attempts to
  // add 
  RequiredMetadata = nullptr;

  return result;
}

static bool usesExtendedExistentialMetadata(CanType type) {
  auto layout = type.getExistentialLayout();
  // If there are parameterized protocol types that we want to
  // treat as equal to unparameterized protocol types (maybe
  // something like `P<some Any>`?), then AST type canonicalization
  // should turn them into unparameterized protocol types.  If the
  // structure makes it to IRGen, we have to honor that decision that
  // they represent different types.
  return layout.containsParameterized;
}

static Optional<std::pair<CanExistentialType, /*depth*/ unsigned>>
usesExtendedExistentialMetadata(CanExistentialMetatypeType type) {
  unsigned depth = 1;
  auto cur = type.getInstanceType();
  while (auto metatype = dyn_cast<ExistentialMetatypeType>(cur)) {
    cur = metatype.getInstanceType();
    depth++;
  }

  // The only existential types that don't currently use ExistentialType
  // are Any and AnyObject, which don't use extended metadata.
  if (usesExtendedExistentialMetadata(cur)) {
    // HACK: The AST for an existential metatype of a (parameterized) protocol
    // still directly wraps the existential type as its instance, which means
    // we need to reconstitute the enclosing ExistentialType.
    assert(cur->isExistentialType());
    if (!cur->is<ExistentialType>()) {
      cur = ExistentialType::get(cur)->getCanonicalType();
    }
    return std::make_pair(cast<ExistentialType>(cur), depth);
  }
  return None;
}

llvm::Constant *IRGenModule::getAddrOfStringForMetadataRef(
    StringRef symbolName,
    unsigned alignment,
    bool shouldSetLowBit,
    llvm::function_ref<ConstantInitFuture (ConstantInitBuilder &)> body) {
  // Call this to form the return value.
  auto returnValue = [&](llvm::Constant *addr) {
    if (!shouldSetLowBit)
      return addr;

    auto bitConstant = llvm::ConstantInt::get(IntPtrTy, 1);
    return llvm::ConstantExpr::getGetElementPtr(Int8Ty, addr, bitConstant);
  };

  // Check whether we already have an entry with this name.
  auto &entry = StringsForTypeRef[symbolName];
  if (entry.second) {
    return returnValue(entry.second);
  }

  // Construct the initializer.
  ConstantInitBuilder builder(*this);
  auto finished = body(builder);

  auto var = new llvm::GlobalVariable(Module, finished.getType(),
                                      /*constant*/ true,
                                      llvm::GlobalValue::LinkOnceODRLinkage,
                                      nullptr,
                                      symbolName);

  ApplyIRLinkage(IRLinkage::InternalLinkOnceODR).to(var);
  if (alignment)
    var->setAlignment(llvm::MaybeAlign(alignment));
  setTrueConstGlobal(var);
  var->setSection(getReflectionTypeRefSectionName());

  finished.installInGlobal(var);

  // Drill down to the i8* at the beginning of the constant.
  auto addr = llvm::ConstantExpr::getBitCast(var, Int8PtrTy);
  StringsForTypeRef[symbolName] = { var, addr };

  return returnValue(addr);
}

llvm::Constant *IRGenModule::getAddrOfStringForTypeRef(StringRef str,
                                                       MangledTypeRefRole role){
  return getAddrOfStringForTypeRef(SymbolicMangling{str.str(), {}}, role);
}

llvm::Constant *IRGenModule::getAddrOfStringForTypeRef(
                                             const SymbolicMangling &mangling,
                                             MangledTypeRefRole role) {
  // Create a symbol name for the symbolic mangling. This is used as the
  // uniquing key both for ODR coalescing and within this TU.
  IRGenMangler mangler;
  std::string symbolName =
    mangler.mangleSymbolNameForSymbolicMangling(mangling, role);

  // See if we emitted the constant already.
  auto &entry = StringsForTypeRef[symbolName];
  if (entry.second) {
    return entry.second;
  }
  
  ConstantInitBuilder B(*this);
  auto S = B.beginStruct();
  S.setPacked(true);

  switch (role) {
  case MangledTypeRefRole::DefaultAssociatedTypeWitness:
    // The 0xFF prefix identifies a default associated type witness.
    S.addInt(Int8Ty,
             ProtocolRequirementFlags::AssociatedTypeInProtocolContextByte);
    break;

  case MangledTypeRefRole::FlatUnique:
    assert(mangling.SymbolicReferences.empty());
    break;

  case MangledTypeRefRole::Metadata:
  case MangledTypeRefRole::Reflection:
  case MangledTypeRefRole::FieldMetadata:
    break;
  }

  unsigned pos = 0;
  for (auto &symbolic : mangling.SymbolicReferences) {
    using SymbolicReferent = IRGenMangler::SymbolicReferent;
    const SymbolicReferent &referent = symbolic.first;

    assert(symbolic.second >= pos
           && "references should be ordered");
    if (symbolic.second != pos) {
      // Emit the preceding literal chunk.
      auto literalChunk = StringRef(mangling.String.data() + pos,
                                    symbolic.second - pos);
      auto literal = llvm::ConstantDataArray::getString(getLLVMContext(),
                                                        literalChunk,
                                                        /*null*/ false);
      S.add(literal);
    }
    
    ConstantReference ref;
    unsigned char kind;
    switch (referent.getKind()) {
    case SymbolicReferent::NominalType: {
      auto type = const_cast<NominalTypeDecl*>(referent.getNominalType());
      if (auto proto = dyn_cast<ProtocolDecl>(type)) {
        // The symbolic reference is to the protocol descriptor of the
        // referenced protocol.
        ref = getAddrOfLLVMVariableOrGOTEquivalent(
          LinkEntity::forProtocolDescriptor(proto));
      } else {
        // The symbolic reference is to the type context descriptor of the
        // referenced type.
        IRGen.noteUseOfTypeContextDescriptor(type, DontRequireMetadata);
        ref = getAddrOfLLVMVariableOrGOTEquivalent(
          LinkEntity::forNominalTypeDescriptor(type));
      }
      // \1 - direct reference, \2 - indirect reference
      kind = (ref.isIndirect() ? 0x02 : 0x01);
      break;
    }
    case SymbolicReferent::OpaqueType: {
      auto opaque = const_cast<OpaqueTypeDecl*>(referent.getOpaqueType());
      IRGen.noteUseOfOpaqueTypeDescriptor(opaque);
      ref = getAddrOfLLVMVariableOrGOTEquivalent(
                                   LinkEntity::forOpaqueTypeDescriptor(opaque));
      kind = (ref.isIndirect() ? 0x02 : 0x01);
      break;
    }
    case SymbolicReferent::ExtendedExistentialTypeShape: {
      auto shapeInfo =
        ExtendedExistentialTypeShapeInfo::get(
          referent.getType()->getCanonicalType());
      ref = ConstantReference(
              emitExtendedExistentialTypeShape(*this, shapeInfo),
              ConstantReference::Direct);
      kind = (shapeInfo.isUnique() ? 0x0a : 0x0b);
      break;
    }
    }
    
    // add kind byte
    S.add(llvm::ConstantInt::get(Int8Ty, kind));
    // add relative reference
    S.addRelativeAddress(ref.getValue());
    pos = symbolic.second + 5;
  }
  
  // Add the last literal bit, if any.
  if (pos != mangling.String.size()) {
    auto literalChunk = StringRef(mangling.String.data() + pos,
                                  mangling.String.size() - pos);
    auto literal = llvm::ConstantDataArray::getString(getLLVMContext(),
                                                      literalChunk,
                                                      /*null*/ false);
    S.add(literal);
  }
  
  // And a null terminator!
  S.addInt(Int8Ty, 0);
  
  auto finished = S.finishAndCreateFuture();
  auto var = new llvm::GlobalVariable(Module, finished.getType(),
                                      /*constant*/ true,
                                      llvm::GlobalValue::LinkOnceODRLinkage,
                                      nullptr,
                                      symbolName);
  ApplyIRLinkage(IRLinkage::InternalLinkOnceODR).to(var);
  var->setAlignment(llvm::MaybeAlign(2));
  setTrueConstGlobal(var);
  var->setSection(getReflectionTypeRefSectionName());
  
  finished.installInGlobal(var);
  
  // Drill down to the i8* at the beginning of the constant.
  auto addr = llvm::ConstantExpr::getBitCast(var, Int8PtrTy);
  entry = {var, addr};
  
  return addr;
}

llvm::Value *irgen::emitObjCMetadataRefForMetadata(IRGenFunction &IGF,
                                                   llvm::Value *classPtr) {
  assert(IGF.IGM.Context.LangOpts.EnableObjCInterop);
  classPtr = IGF.Builder.CreateBitCast(classPtr, IGF.IGM.ObjCClassPtrTy);
  
  // Fetch the metadata for that class.
  auto call = IGF.Builder.CreateCall(
      IGF.IGM.getGetObjCClassMetadataFunctionPointer(), classPtr);
  call->setDoesNotThrow();
  call->setDoesNotAccessMemory();
  return call;
}

/// Emit a reference to the Swift metadata for an Objective-C class.
static llvm::Value *emitObjCMetadataRef(IRGenFunction &IGF,
                                        ClassDecl *theClass) {
  // Derive a pointer to the Objective-C class.
  auto classPtr = emitObjCHeapMetadataRef(IGF, theClass);
  
  return emitObjCMetadataRefForMetadata(IGF, classPtr);
}

// Get the type that exists at runtime to represent a compile-time type.
CanType IRGenModule::getRuntimeReifiedType(CanType type) {
  // Leave type-erased ObjC generics with their generic arguments unbound, since
  // the arguments do not exist at runtime.
  return CanType(type.transform([&](Type t) -> Type {
    if (CanType(t).isTypeErasedGenericClassType()) {
      return t->getAnyNominal()->getDeclaredType()->getCanonicalType();
    }
    return t;
  }));
}

CanType IRGenModule::substOpaqueTypesWithUnderlyingTypes(CanType type) {
  // Substitute away opaque types whose underlying types we're allowed to
  // assume are constant.
  if (type->hasOpaqueArchetype()) {
    ReplaceOpaqueTypesWithUnderlyingTypes replacer(
        getSwiftModule(), ResilienceExpansion::Maximal,
        getSILModule().isWholeModule());
    auto underlyingTy =
        type.subst(replacer, replacer, SubstFlags::SubstituteOpaqueArchetypes)
            ->getCanonicalType();
    return underlyingTy;
  }

  return type;
}

SILType IRGenModule::substOpaqueTypesWithUnderlyingTypes(
    SILType type, CanGenericSignature genericSig) {
  // Substitute away opaque types whose underlying types we're allowed to
  // assume are constant.
  if (type.getASTType()->hasOpaqueArchetype()) {
    ReplaceOpaqueTypesWithUnderlyingTypes replacer(
        getSwiftModule(), ResilienceExpansion::Maximal,
        getSILModule().isWholeModule());
    auto underlyingTy =
        type.subst(getSILModule(), replacer, replacer, genericSig,
                   /*substitute opaque*/ true);
    return underlyingTy;
  }

  return type;
}

std::pair<CanType, ProtocolConformanceRef>
IRGenModule::substOpaqueTypesWithUnderlyingTypes(CanType type,
                                           ProtocolConformanceRef conformance) {
  // Substitute away opaque types whose underlying types we're allowed to
  // assume are constant.
  if (type->hasOpaqueArchetype()) {
    ReplaceOpaqueTypesWithUnderlyingTypes replacer(
        getSwiftModule(), ResilienceExpansion::Maximal,
        getSILModule().isWholeModule());
    auto substConformance = conformance.subst(
        type, replacer, replacer, SubstFlags::SubstituteOpaqueArchetypes);
    auto underlyingTy =
        type.subst(replacer, replacer, SubstFlags::SubstituteOpaqueArchetypes)
            ->getCanonicalType();
    return std::make_pair(underlyingTy, substConformance);
  }

  return std::make_pair(type, conformance);
}


/// Attempts to return a constant heap metadata reference for a
/// class type.  This is generally only valid for specific kinds of
/// ObjC reference, like superclasses or category references.
llvm::Constant *
irgen::tryEmitConstantHeapMetadataRef(IRGenModule &IGM,
                                      CanType type,
                                      bool allowDynamicUninitialized) {
  auto theDecl = type->getClassOrBoundGenericClass();
  assert(theDecl && "emitting constant heap metadata ref for non-class type?");

  switch (IGM.getClassMetadataStrategy(theDecl)) {
  case ClassMetadataStrategy::Resilient:
  case ClassMetadataStrategy::Singleton:
    if (!allowDynamicUninitialized)
      return nullptr;
    break;

  case ClassMetadataStrategy::Update:
  case ClassMetadataStrategy::FixedOrUpdate:
  case ClassMetadataStrategy::Fixed:
    break;
  }

  // For imported classes, use the ObjC class symbol.
  if (!hasKnownSwiftMetadata(IGM, theDecl))
    return IGM.getAddrOfObjCClass(theDecl, NotForDefinition);

  return IGM.getAddrOfTypeMetadata(type);
}

/// Attempts to return a constant type metadata reference for a
/// nominal type.
ConstantReference
irgen::tryEmitConstantTypeMetadataRef(IRGenModule &IGM, CanType type,
                                      SymbolReferenceKind refKind) {
  if (IGM.isStandardLibrary())
    return ConstantReference();
  if (isCanonicalCompleteTypeMetadataStaticallyAddressable(IGM, type))
    return ConstantReference();
  return IGM.getAddrOfTypeMetadata(type, refKind);
}

/// Emit a reference to an ObjC class.  In general, the only things
/// you're allowed to do with the address of an ObjC class symbol are
/// (1) send ObjC messages to it (in which case the message will be
/// forwarded to the real class, if one exists) or (2) put it in
/// various data sections where the ObjC runtime will properly arrange
/// things.  Therefore, we must typically force the initialization of
/// a class when emitting a reference to it.
llvm::Value *irgen::emitObjCHeapMetadataRef(IRGenFunction &IGF,
                                            ClassDecl *theClass,
                                            bool allowUninitialized) {
  // If the class is visible only through the Objective-C runtime, form the
  // appropriate runtime call.
  if (theClass->getForeignClassKind() == ClassDecl::ForeignKind::RuntimeOnly) {
    SmallString<64> scratch;
    auto className =
        IGF.IGM.getAddrOfGlobalString(theClass->getObjCRuntimeName(scratch));
    return IGF.Builder.CreateCall(IGF.IGM.getLookUpClassFunctionPointer(),
                                  className);
  }

  assert(!theClass->isForeign());

  Address classRef = IGF.IGM.getAddrOfObjCClassRef(theClass);
  auto classObject = IGF.Builder.CreateLoad(classRef);
  if (allowUninitialized) return classObject;

  // TODO: memoize this the same way that we memoize Swift type metadata?
  return IGF.Builder.CreateCall(IGF.IGM.getFixedClassInitializationFn(),
                                classObject);
}

static MetadataResponse emitNominalPrespecializedGenericMetadataRef(
    IRGenFunction &IGF, NominalTypeDecl *theDecl, CanType theType,
    DynamicMetadataRequest request,
    SpecializedMetadataCanonicality canonicality) {
  assert(isCompleteSpecializedNominalTypeMetadataStaticallyAddressable(
      IGF.IGM, *theDecl, theType, canonicality));
  // We are applying generic parameters to a generic type.
  assert(theType->getAnyNominal() == theDecl);

  // Check to see if we've maybe got a local reference already.
  if (auto cache = IGF.tryGetLocalTypeMetadata(theType, request))
    return cache;

  switch (canonicality) {
  case CanonicalSpecializedMetadata: {
    auto metadata = IGF.IGM.getAddrOfTypeMetadata(theType);
    return MetadataResponse::forComplete(metadata);
  }
  case NoncanonicalSpecializedMetadata: {
    auto cacheVariable =
        IGF.IGM.getAddrOfNoncanonicalSpecializedGenericTypeMetadataCacheVariable(theType);
    auto call = IGF.Builder.CreateCall(
        IGF.IGM.getGetCanonicalSpecializedMetadataFunctionPointer(),
        {request.get(IGF),
         IGF.IGM.getAddrOfTypeMetadata(theType,
                                       TypeMetadataCanonicality::Noncanonical),
         cacheVariable});
    call->setDoesNotThrow();
    call->setCallingConv(IGF.IGM.SwiftCC);
    return MetadataResponse::handle(IGF, request, call);
  }
  }
  llvm_unreachable("unhandled metadata canonicality");
}

static llvm::Value *
emitIdempotentClassMetadataInitialization(IRGenFunction &IGF,
                                          llvm::Value *metadata) {
  if (IGF.IGM.ObjCInterop) {
    metadata = IGF.Builder.CreateBitCast(metadata, IGF.IGM.ObjCClassPtrTy);
    metadata = IGF.Builder.CreateCall(IGF.IGM.getFixedClassInitializationFn(),
                                      metadata);
    metadata = IGF.Builder.CreateBitCast(metadata, IGF.IGM.TypeMetadataPtrTy);
  }

  return metadata;
}

/// Returns a metadata reference for a nominal type.
///
/// This is only valid in a couple of special cases:
/// 1) The nominal type is generic, in which case we emit a call to the
///    generic metadata accessor function, which must be defined separately.
/// 2) The nominal type is a value type with a fixed size from this
///    resilience domain, in which case we can reference the constant
///    metadata directly.
/// 3) The nominal type is a class with known Swift metadata and
///   a fixed layout from this resilience domain, in which case we only
///   need perform idempotent class initialization to realize it
///   in the ObjC runtime.
///
/// In any other case, a metadata accessor should be called instead.
static MetadataResponse emitNominalMetadataRef(IRGenFunction &IGF,
                                               NominalTypeDecl *theDecl,
                                               CanType theType,
                                               DynamicMetadataRequest request) {
  assert(!isa<ProtocolDecl>(theDecl));

  if (!theDecl->isGenericContext()) {
    assert(!IGF.IGM.isResilient(theDecl, ResilienceExpansion::Maximal));
    if (auto response = IGF.tryGetLocalTypeMetadata(theType, request)) {
      return response;
    }

    llvm::Value *metadata = IGF.IGM.getAddrOfTypeMetadata(theType);
    
    // We need to realize classes with the ObjC runtime.
    if (auto c = dyn_cast<ClassDecl>(theDecl)) {
      
      assert(hasKnownSwiftMetadata(IGF.IGM, c));
      metadata = emitIdempotentClassMetadataInitialization(IGF, metadata);
    }
    auto response = MetadataResponse::forComplete(metadata);
    IGF.setScopedLocalTypeMetadata(theType, response);
    return response;
  }

  // We are applying generic parameters to a generic type.
  assert(theType->isSpecialized() &&
         theType->getAnyNominal() == theDecl);

  // Check to see if we've maybe got a local reference already.
  if (auto cache = IGF.tryGetLocalTypeMetadata(theType, request))
    return cache;

  // Grab the substitutions.
  GenericArguments genericArgs;
  genericArgs.collect(IGF, theType);
  assert((!genericArgs.Values.empty() ||
          theDecl->getGenericSignature()->areAllParamsConcrete()) &&
         "no generic args?!");

  MetadataResponse response;

  if (isCompleteSpecializedNominalTypeMetadataStaticallyAddressable(
          IGF.IGM, *theDecl, theType, CanonicalSpecializedMetadata)) {
    response = emitNominalPrespecializedGenericMetadataRef(
        IGF, theDecl, theType, request, CanonicalSpecializedMetadata);
  } else if (isCompleteSpecializedNominalTypeMetadataStaticallyAddressable(
                 IGF.IGM, *theDecl, theType, NoncanonicalSpecializedMetadata)) {
    response = emitNominalPrespecializedGenericMetadataRef(
        IGF, theDecl, theType, request, NoncanonicalSpecializedMetadata);
  } else if (auto theClass = dyn_cast<ClassDecl>(theDecl)) {
    if (isSpecializedNominalTypeMetadataStaticallyAddressable(
            IGF.IGM, *theClass, theType, CanonicalSpecializedMetadata,
            ForUseOnlyFromAccessor)) {
      llvm::Function *accessor =
          IGF.IGM
              .getAddrOfCanonicalSpecializedGenericTypeMetadataAccessFunction(
                  theType, NotForDefinition);

      response =
          IGF.emitGenericTypeMetadataAccessFunctionCall(accessor, {}, request);
    }
  }

  if (!response.isValid()) {
    // Call the generic metadata accessor function.
    llvm::Function *accessor =
        IGF.IGM.getAddrOfGenericTypeMetadataAccessFunction(
            theDecl, genericArgs.Types, NotForDefinition);

    response = IGF.emitGenericTypeMetadataAccessFunctionCall(
        accessor, genericArgs.Values, request);
  }

  IGF.setScopedLocalTypeMetadata(theType, response);
  return response;
}

bool irgen::isSpecializedNominalTypeMetadataStaticallyAddressable(
    IRGenModule &IGM, NominalTypeDecl &nominal, CanType type,
    SpecializedMetadataCanonicality canonicality,
    SpecializedMetadataUsageIsOnlyFromAccessor onlyFromAccessor) {
  assert(nominal.isGenericContext());

  if (!IGM.shouldPrespecializeGenericMetadata()) {
    return false;
  }

  if (type->hasArchetype()) {
    return false;
  }

  switch (canonicality) {
  case CanonicalSpecializedMetadata:
    if (IGM.getSILModule().isWholeModule()) {
      // Canonical prespecializations can only be emitted within the module
      // where the generic type is itself defined, since it is the module where
      // the metadata accessor is defined.
      if (IGM.getSwiftModule() != nominal.getModuleContext()) {
        return false;
      }
    } else {
      // If whole module optimization is not enabled, we can only construct a
      // canonical prespecialization if the usage is in the same *file* as that
      // containing the type's decl!  The reason is that the generic metadata
      // accessor is defined in the IRGenModule corresponding to the source file
      // containing the type's decl.
      SourceFile *nominalFile = nominal.getDeclContext()->getParentSourceFile();
      if (auto *moduleFile = IGM.IRGen.getSourceFile(&IGM)) {
        if (nominalFile != moduleFile) {
          return false;
        }
      }
    }
    break;
  case NoncanonicalSpecializedMetadata:
    // Non-canonical metadata prespecializations for a type cannot be formed
    // within the module that defines that type.
    if (IGM.getSwiftModule() == nominal.getModuleContext()) {
      return false;
    }
    if (nominal.isResilient(IGM.getSwiftModule(),
                            ResilienceExpansion::Maximal)) {
      return false;
    }
    break;
  }

  if (auto *theClass = dyn_cast<ClassDecl>(&nominal)) {
    if (theClass->hasResilientMetadata(IGM.getSwiftModule(),
                                       ResilienceExpansion::Maximal)) {
      return false;
    }
    AncestryOptions flags = theClass->checkAncestry();
    if (flags & (AncestryOptions(AncestryFlags::ResilientOther) |
                 AncestryOptions(AncestryFlags::ClangImported))) {
      return false;
    }
    if (auto *theSuperclass = theClass->getSuperclassDecl()) {
      auto superclassType =
          type->getSuperclass(/*useArchetypes=*/false)->getCanonicalType();
      if (!isCanonicalInitializableTypeMetadataStaticallyAddressable(
              IGM, superclassType) &&
          !tryEmitConstantHeapMetadataRef(
              IGM, superclassType,
              /*allowDynamicUninitialized=*/false)) {
        return false;
      }
    }
  }

  auto *generic = type.getAnyGeneric();
  assert(generic);
  auto *environment = generic->getGenericEnvironment();
  assert(environment);
  auto substitutions =
      type->getContextSubstitutionMap(IGM.getSwiftModule(), &nominal);

  auto allArgumentsAreStaticallyAddressable =
      llvm::all_of(environment->getGenericParams(), [&](auto parameter) {
        auto signature = environment->getGenericSignature();
        const auto protocols = signature->getRequiredProtocols(parameter);
        auto argument = ((Type *)parameter)->subst(substitutions);
        auto canonicalType = argument->getCanonicalType();
        auto witnessTablesAreReferenceable = [&]() {
          return llvm::all_of(protocols, [&](ProtocolDecl *protocol) {
            auto conformance =
                signature->lookupConformance(canonicalType, protocol);
            if (!conformance.isConcrete()) {
              return false;
            }
            auto rootConformance =
                conformance.getConcrete()->getRootConformance();
            return !IGM.isDependentConformance(rootConformance) &&
                   !IGM.isResilientConformance(rootConformance);
          });
        };
        // TODO: Once witness tables are statically specialized, check whether
        // the
        //       ConformanceInfo returns nullptr from tryGetConstantTable.
        auto isGenericWithoutPrespecializedConformance = [&]() {
          auto genericArgument = argument->getAnyGeneric();
          return genericArgument && genericArgument->isGenericContext() &&
                 (protocols.size() > 0);
        };
        auto metadataAccessIsTrivial = [&]() {
          if (onlyFromAccessor) {
            // If an accessor is being used, then the accessor will be able to
            // initialize the arguments, i.e. register classes with the ObjC
            // runtime.
            return irgen::
                isCanonicalInitializableTypeMetadataStaticallyAddressable(
                    IGM, canonicalType);
          } else {
            return irgen::isCanonicalCompleteTypeMetadataStaticallyAddressable(
                IGM, canonicalType);
          }
        };
        return !isGenericWithoutPrespecializedConformance() &&
               metadataAccessIsTrivial() && witnessTablesAreReferenceable();
      });
  return allArgumentsAreStaticallyAddressable &&
         IGM.getTypeInfoForUnlowered(type).isFixedSize(
             ResilienceExpansion::Maximal);
}

bool irgen::isCompleteSpecializedNominalTypeMetadataStaticallyAddressable(
    IRGenModule &IGM, NominalTypeDecl &nominal, CanType type,
    SpecializedMetadataCanonicality canonicality) {
  if (isa<ClassType>(type) || isa<BoundGenericClassType>(type)) {
    // TODO: On platforms without ObjC interop, we can do direct access to
    // class metadata.
    return false;
  }
  // Prespecialized struct/enum metadata gets no dedicated accessor yet and so
  // cannot do the work of registering the generic arguments which are classes
  // with the ObjC runtime.  Concretely, the following cannot be prespecialized
  // yet:
  //   Struct<Klass<Int>>
  //   Enum<Klass<Int>>
  return isSpecializedNominalTypeMetadataStaticallyAddressable(
      IGM, nominal, type, canonicality, NotForUseOnlyFromAccessor);
}

/// Is there a known address for canonical specialized metadata?  The metadata
/// there may need initialization before it is complete.
bool irgen::isCanonicalInitializableTypeMetadataStaticallyAddressable(
    IRGenModule &IGM, CanType type) {
  if (isCanonicalCompleteTypeMetadataStaticallyAddressable(IGM, type)) {
    // The address of the complete metadata is the address of the abstract
    // metadata.
    return true;
  }

  NominalTypeDecl *nominal;
  if ((nominal = type->getAnyNominal()) && nominal->isGenericContext()) {
    // Prespecialized class metadata gets a dedicated accessor which can do
    // the work of registering the class and its arguments with the ObjC
    // runtime.
    // Concretely, Clazz<Klass<Int>> can be prespecialized.
    return isSpecializedNominalTypeMetadataStaticallyAddressable(
        IGM, *nominal, type, CanonicalSpecializedMetadata,
        ForUseOnlyFromAccessor);
  }

  return false;
}

bool irgen::isNoncanonicalCompleteTypeMetadataStaticallyAddressable(
    IRGenModule &IGM, CanType type) {
  // If the canonical metadata record can be statically addressed, then there
  // should be no visible non-canonical metadata record to address.
  if (isCanonicalCompleteTypeMetadataStaticallyAddressable(IGM, type)) {
    return false;
  }

  if (isa<BoundGenericStructType>(type) || isa<BoundGenericEnumType>(type)) {
    auto nominalType = cast<BoundGenericType>(type);
    auto *nominalDecl = nominalType->getDecl();

    // Imported type metadata always requires an accessor.
    if (isa<ClangModuleUnit>(nominalDecl->getModuleScopeContext()))
      return false;

    return isCompleteSpecializedNominalTypeMetadataStaticallyAddressable(
        IGM, *nominalDecl, type, NoncanonicalSpecializedMetadata);
  }
  return false;
}

/// Is complete metadata for the given type available at a fixed address?
bool irgen::isCanonicalCompleteTypeMetadataStaticallyAddressable(
    IRGenModule &IGM, CanType type) {
  assert(!type->hasArchetype());

  // Value type metadata only requires dynamic initialization on first
  // access if it contains a resilient type.
  if (isa<StructType>(type) || isa<EnumType>(type)) {
    auto nominalType = cast<NominalType>(type);
    auto *nominalDecl = nominalType->getDecl();

    // Imported type metadata always requires an accessor.
    if (isa<ClangModuleUnit>(nominalDecl->getModuleScopeContext()))
      return false;

    if (nominalDecl->isGenericContext())
      return isCompleteSpecializedNominalTypeMetadataStaticallyAddressable(
          IGM, *nominalDecl, type, CanonicalSpecializedMetadata);

    auto expansion = ResilienceExpansion::Maximal;

    // Resiliently-sized metadata access always requires an accessor.
    return IGM.getTypeInfoForUnlowered(type).isFixedSize(expansion);
  }

  // The empty tuple type has a singleton metadata.
  if (auto tuple = dyn_cast<TupleType>(type))
    return tuple->getNumElements() == 0;
  
  // Any and AnyObject have singleton metadata.
  if (type->isAny() || type->isAnyObject())
    return true;

  // The builtin types generally don't require metadata, but some of them
  // have nodes in the runtime anyway.
  if (isa<BuiltinType>(type))
    return true;

  // SIL box types are artificial, but for the purposes of dynamic layout,
  // we use the NativeObject metadata.
  if (isa<SILBoxType>(type))
    return true;

  if (isa<BoundGenericStructType>(type) || isa<BoundGenericEnumType>(type)) {
    auto nominalType = cast<BoundGenericType>(type);
    auto *nominalDecl = nominalType->getDecl();

    // Imported type metadata always requires an accessor.
    if (isa<ClangModuleUnit>(nominalDecl->getModuleScopeContext()))
      return false;

    return isCompleteSpecializedNominalTypeMetadataStaticallyAddressable(
        IGM, *nominalDecl, type, CanonicalSpecializedMetadata);
  }

  return false;
}

/// Should requests for the given type's metadata be cached?
bool irgen::shouldCacheTypeMetadataAccess(IRGenModule &IGM, CanType type) {
  // DynamicSelfType is actually local.
  if (type->hasDynamicSelfType())
    return false;

  // Nongeneric, nonresilient classes with known Swift metadata need to be
  // realized with the Objective-C runtime, but that only requires a single
  // runtime call that already has a fast path exit for already-realized
  // classes, so we don't need to put up another layer of caching in front.
  //
  // TODO: On platforms without ObjC interop, we can do direct access to
  // Swift metadata without a runtime call at all.
  if (auto classDecl = type.getClassOrBoundGenericClass()) {
    if (!hasKnownSwiftMetadata(IGM, classDecl))
      return true;
    if (classDecl->isGenericContext() &&
        isSpecializedNominalTypeMetadataStaticallyAddressable(
            IGM, *classDecl, type, CanonicalSpecializedMetadata,
            ForUseOnlyFromAccessor))
      return false;
    auto strategy = IGM.getClassMetadataStrategy(classDecl);
    return strategy != ClassMetadataStrategy::Fixed;
  }

  // Trivially accessible metadata does not need a cache.
  if (isCanonicalCompleteTypeMetadataStaticallyAddressable(IGM, type))
    return false;

  if (isNoncanonicalCompleteTypeMetadataStaticallyAddressable(IGM, type))
    return false;

  return true;
}

/// Should requests for the given type's metadata go through an accessor?
static bool shouldTypeMetadataAccessUseAccessor(IRGenModule &IGM, CanType type){
  // Anything that requires caching should go through an accessor to outline
  // the cache check.
  if (shouldCacheTypeMetadataAccess(IGM, type))
    return true;
  
  // Fixed-metadata classes don't require caching, but we still want to go
  // through the accessor to outline the ObjC realization.
  // TODO: On non-Apple platforms, fixed classes should not need any
  // initialization so should be directly addressable.
  if (isa<ClassType>(type)) {
    return true;
  }
  
  return false;
}

/// Return the standard access strategy for getting a non-dependent
/// type metadata object.
MetadataAccessStrategy irgen::getTypeMetadataAccessStrategy(CanType type) {
  // We should not be emitting accessors for partially-substituted
  // generic types.
  assert(!type->hasArchetype());

  // Non-generic structs, enums, and classes are special cases.
  //
  // Note that while protocol types don't have a metadata pattern,
  // we still require an accessor since we actually want to get
  // the metadata for the existential type.
  //
  // This needs to kept in sync with hasRequiredTypeMetadataAccessPattern.
  auto nominal = type->getAnyNominal();
  if (nominal && !isa<ProtocolDecl>(nominal)) {
    // Metadata accessors for fully-substituted generic types are
    // emitted with shared linkage.
    if (nominal->isGenericContext() && !nominal->isObjC()) {
      if (type->isSpecialized())
        return MetadataAccessStrategy::NonUniqueAccessor;
      assert(type->hasUnboundGenericType());
    }

    if (type->isForeignReferenceType())
      return MetadataAccessStrategy::PublicUniqueAccessor;

    if (requiresForeignTypeMetadata(nominal))
      return MetadataAccessStrategy::ForeignAccessor;

    // If the type doesn't guarantee that it has an access function,
    // we might have to use a non-unique accessor.

    // Everything else requires accessors.
    switch (getDeclLinkage(nominal)) {
    case FormalLinkage::PublicUnique:
      return MetadataAccessStrategy::PublicUniqueAccessor;
    case FormalLinkage::HiddenUnique:
      return MetadataAccessStrategy::HiddenUniqueAccessor;
    case FormalLinkage::Private:
      return MetadataAccessStrategy::PrivateAccessor;

    case FormalLinkage::PublicNonUnique:
      return MetadataAccessStrategy::NonUniqueAccessor;
    }
    llvm_unreachable("bad formal linkage");
  }

  // Everything else requires a shared accessor function.
  return MetadataAccessStrategy::NonUniqueAccessor;
}

/// Emit a string encoding the labels in the given tuple type.
static llvm::Constant *getTupleLabelsString(IRGenModule &IGM,
                                            CanTupleType type) {
  bool hasLabels = false;
  llvm::SmallString<128> buffer;
  for (auto &elt : type->getElements()) {
    if (elt.hasName()) {
      hasLabels = true;
      buffer.append(elt.getName().str());
    }

    // Each label is space-terminated.
    buffer += ' ';
  }

  // If there are no labels, use a null pointer.
  if (!hasLabels) {
    return llvm::ConstantPointerNull::get(IGM.Int8PtrTy);
  }

  // Otherwise, create a new string literal.
  // This method implicitly adds a null terminator.
  return IGM.getAddrOfGlobalString(buffer);
}

static llvm::Constant *emitEmptyTupleTypeMetadataRef(IRGenModule &IGM) {
  llvm::Constant *fullMetadata = IGM.getEmptyTupleMetadata();
  llvm::Constant *indices[] = {
    llvm::ConstantInt::get(IGM.Int32Ty, 0),
    llvm::ConstantInt::get(IGM.Int32Ty, 1)
  };
  return llvm::ConstantExpr::getInBoundsGetElementPtr(
      IGM.FullExistentialTypeMetadataStructTy, fullMetadata, indices);
}

/// Emit metadata for a tuple type containing one or more pack expansions, eg
/// (T, repeat each U, v: V, repeat each W).
static MetadataResponse emitDynamicTupleTypeMetadataRef(IRGenFunction &IGF,
                                                        CanTupleType type,
                                                        DynamicMetadataRequest request) {
  CanPackType packType = type.getInducedPackType();

  // Begin by computing the number of elements in the tuple type.
  auto *shapeExpression = IGF.emitPackShapeExpression(packType);
  llvm::BasicBlock *trueBB = nullptr, *falseBB = nullptr, *restBB = nullptr;
  llvm::BasicBlock *unwrappedBB = nullptr;
  llvm::Value *unwrapped = nullptr;

  // A tuple type containing zero or one non-pack-expansions might contain
  // exactly one element after substitution, in which case the tuple
  // "vanishes" and gets unwrapped. This behavior is implemented in both
  // compile-time type substitution, and runtime type metadata instantiation,
  // ensuring consistent behavior.
  if (type->getNumScalarElements() <= 1) {
    ConditionalDominanceScope scope(IGF);

    // Test if the runtime length of the pack type is exactly 1.
    auto *one = llvm::ConstantInt::get(IGF.IGM.SizeTy, 1);
    auto *isOne = IGF.Builder.CreateICmpEQ(shapeExpression, one);

    trueBB = IGF.createBasicBlock("vanishing-tuple");
    falseBB = IGF.createBasicBlock("actual-tuple");

    IGF.Builder.CreateCondBr(isOne, trueBB, falseBB);

    IGF.Builder.emitBlock(trueBB);

    // If the length is 1, directly emit the metadata for the first pack element.
    ArrayRef<ProtocolConformanceRef> conformances;
    llvm::SmallVector<llvm::Value *, 2> wtables;

    auto *index = llvm::ConstantInt::get(IGF.IGM.SizeTy, 0);
    auto *value = emitTypeMetadataPackElementRef(
        IGF, packType, conformances, index, request, wtables);

    // FIXME: Should emitTypeMetadataPackElementRef() preserve the dynamic state?
    auto response = MetadataResponse::forBounded(
        value, request.getStaticLowerBoundOnResponseState());
    response.ensureDynamicState(IGF);

    unwrapped = response.combine(IGF);
    unwrappedBB = IGF.Builder.GetInsertBlock();

    assert(wtables.empty());

    restBB = IGF.createBasicBlock("tuple-rest");
    IGF.Builder.CreateBr(restBB);

    IGF.Builder.emitBlock(falseBB);
  }

  llvm::CallInst *call = nullptr;

  {
    ConditionalDominanceScope scope(IGF);

    // Otherwise, we know that either statically or dynamically, we have more than
    // one element. Emit the pack.
    auto addr = emitTypeMetadataPack(IGF, packType, MetadataState::Abstract);

    auto *pointerToFirst = IGF.Builder.CreatePointerCast(
        addr.getAddressPointer(), IGF.IGM.TypeMetadataPtrPtrTy);

    // Call swift_getTupleMetadata().
    llvm::Value *args[] = {
      request.get(IGF),
      shapeExpression,
      pointerToFirst,
      getTupleLabelsString(IGF.IGM, type),
      llvm::ConstantPointerNull::get(IGF.IGM.WitnessTablePtrTy) // proposed
    };

    call = IGF.Builder.CreateCall(
        IGF.IGM.getGetTupleMetadataFunctionPointer(), args);
    call->setCallingConv(IGF.IGM.SwiftCC);
    call->setDoesNotThrow();

    // Clean up the pack.
    Optional<unsigned> elementCount = 0;
    if (auto *constant = dyn_cast<llvm::ConstantInt>(shapeExpression))
      elementCount = constant->getValue().getZExtValue();

    cleanupTypeMetadataPack(IGF, addr, elementCount);
  }

  // Control flow join with the one-element case.
  llvm::Value *result = nullptr;
  if (unwrapped != nullptr) {
    IGF.Builder.CreateBr(restBB);
    IGF.Builder.emitBlock(restBB);

    auto *phi = IGF.Builder.CreatePHI(IGF.IGM.TypeMetadataResponseTy, 2);
    phi->addIncoming(unwrapped, unwrappedBB);
    phi->addIncoming(call, call->getParent());

    result = phi;
  } else {
    result = call;
  }

  return MetadataResponse::handle(IGF, request, result);
}

static MetadataResponse emitTupleTypeMetadataRef(IRGenFunction &IGF,
                                                 CanTupleType type,
                                                 DynamicMetadataRequest request) {
  if (type->containsPackExpansionType())
    return emitDynamicTupleTypeMetadataRef(IGF, type, request);

  auto getElementMetadata = [&](CanType type) {
    // Just request the elements to be abstract so that we can always build
    // the metadata.
    // TODO: if we have a collector, or if this is a blocking request, maybe
    // we should build a stronger request?
    return IGF.emitTypeMetadataRef(type, MetadataState::Abstract).getMetadata();
  };

  switch (type->getNumElements()) {
  case 0:
    return MetadataResponse::forComplete(
                                        emitEmptyTupleTypeMetadataRef(IGF.IGM));

  case 1:
    // For metadata purposes, we consider a singleton tuple to be
    // isomorphic to its element type. ???
    return IGF.emitTypeMetadataRef(type.getElementType(0), request);

  case 2: {
    auto elt0Metadata = getElementMetadata(type.getElementType(0));
    auto elt1Metadata = getElementMetadata(type.getElementType(1));

    llvm::Value *args[] = {
      request.get(IGF),
      elt0Metadata, elt1Metadata,
      getTupleLabelsString(IGF.IGM, type),
      llvm::ConstantPointerNull::get(IGF.IGM.WitnessTablePtrTy) // proposed
    };

    auto call = IGF.Builder.CreateCall(
        IGF.IGM.getGetTupleMetadata2FunctionPointer(), args);
    call->setCallingConv(IGF.IGM.SwiftCC);
    call->setDoesNotThrow();

    return MetadataResponse::handle(IGF, request, call);
  }

  case 3: {
    auto elt0Metadata = getElementMetadata(type.getElementType(0));
    auto elt1Metadata = getElementMetadata(type.getElementType(1));
    auto elt2Metadata = getElementMetadata(type.getElementType(2));

    llvm::Value *args[] = {
      request.get(IGF),
      elt0Metadata, elt1Metadata, elt2Metadata,
      getTupleLabelsString(IGF.IGM, type),
      llvm::ConstantPointerNull::get(IGF.IGM.WitnessTablePtrTy) // proposed
    };

    auto call = IGF.Builder.CreateCall(
        IGF.IGM.getGetTupleMetadata3FunctionPointer(), args);
    call->setCallingConv(IGF.IGM.SwiftCC);
    call->setDoesNotThrow();

    return MetadataResponse::handle(IGF, request, call);
  }
  default:
    return emitDynamicTupleTypeMetadataRef(IGF, type, request);
  }
}

static Address createGenericArgumentsArray(IRGenFunction &IGF,
                                           ArrayRef<llvm::Value *> args) {
  // Allocate an array to pass the arguments.
  auto argsBufferTy = llvm::ArrayType::get(IGF.IGM.Int8PtrTy, args.size());
  auto argsBuffer =
    IGF.createAlloca(argsBufferTy, IGF.IGM.getPointerAlignment());

  // Mark the beginning of the array lifetime.
  IGF.Builder.CreateLifetimeStart(argsBuffer,
                                  IGF.IGM.getPointerSize() * args.size());

  // Fill in the buffer.
  for (unsigned i : indices(args)) {
    Address elt = IGF.Builder.CreateStructGEP(argsBuffer, i,
                                              IGF.IGM.getPointerSize() * i);
    auto *arg = IGF.Builder.CreateBitOrPointerCast(args[i], IGF.IGM.Int8PtrTy);
    IGF.Builder.CreateStore(arg, elt);
  }

  return argsBuffer;
}

static void destroyGenericArgumentsArray(IRGenFunction &IGF,
                                         Address argsBuffer,
                                         ArrayRef<llvm::Value *> args) {
  IGF.Builder.CreateLifetimeEnd(argsBuffer,
                                IGF.IGM.getPointerSize() * args.size());
}

namespace {
  /// A visitor class for emitting a reference to a metatype object.
  /// This implements a "raw" access, useful for implementing cache
  /// functions or for implementing dependent accesses.
  ///
  /// If the access requires runtime initialization, that initialization
  /// must be dependency-ordered-before any load that carries a dependency
  /// from the resulting metadata pointer.
  class EmitTypeMetadataRef
    : public CanTypeVisitor<EmitTypeMetadataRef, MetadataResponse,
                            DynamicMetadataRequest> {
  private:
    IRGenFunction &IGF;
  public:
    EmitTypeMetadataRef(IRGenFunction &IGF) : IGF(IGF) {}

    MetadataResponse emitDirectMetadataRef(CanType type) {
      return MetadataResponse::forComplete(IGF.IGM.getAddrOfTypeMetadata(type));
    }

    /// The given type should use opaque type info.  We assume that
    /// the runtime always provides an entry for such a type.
    MetadataResponse visitBuiltinIntegerType(CanBuiltinIntegerType type,
                                             DynamicMetadataRequest request) {
      // If the size isn't a power up two, round up to the next power of two
      // and use the corresponding integer type.
      auto &opaqueTI = cast<FixedTypeInfo>(IGF.IGM.getTypeInfoForLowered(type));
      unsigned numBits = opaqueTI.getFixedSize().getValueInBits();
      if (!llvm::isPowerOf2_32(numBits)) {
        numBits = llvm::NextPowerOf2(numBits);
        type = CanBuiltinIntegerType(
                 BuiltinIntegerType::get(numBits, IGF.IGM.Context));
      }

      return emitDirectMetadataRef(type);
    }

    MetadataResponse
    visitBuiltinIntegerLiteralType(CanBuiltinIntegerLiteralType type,
                                   DynamicMetadataRequest request) {
      return emitDirectMetadataRef(type);
    }

    MetadataResponse
    visitBuiltinNativeObjectType(CanBuiltinNativeObjectType type,
                                 DynamicMetadataRequest request) {
      return emitDirectMetadataRef(type);
    }

    MetadataResponse
    visitBuiltinBridgeObjectType(CanBuiltinBridgeObjectType type,
                                 DynamicMetadataRequest request) {
      return emitDirectMetadataRef(type);
    }

    MetadataResponse
    visitBuiltinUnsafeValueBufferType(CanBuiltinUnsafeValueBufferType type,
                                      DynamicMetadataRequest request) {
      return emitDirectMetadataRef(type);
    }

    MetadataResponse
    visitBuiltinRawPointerType(CanBuiltinRawPointerType type,
                               DynamicMetadataRequest request) {
      return emitDirectMetadataRef(type);
    }

    MetadataResponse
    visitBuiltinRawUnsafeContinuationType(CanBuiltinRawUnsafeContinuationType type,
                                          DynamicMetadataRequest request) {
      return emitDirectMetadataRef(type);
    }

    MetadataResponse
    visitBuiltinJobType(CanBuiltinJobType type,
                        DynamicMetadataRequest request) {
      return emitDirectMetadataRef(type);
    }

    MetadataResponse
    visitBuiltinExecutorType(CanBuiltinExecutorType type,
                             DynamicMetadataRequest request) {
      return emitDirectMetadataRef(type);
    }

    MetadataResponse
    visitBuiltinPackIndexType(CanBuiltinPackIndexType type,
                              DynamicMetadataRequest request) {
      llvm_unreachable("metadata unsupported for this builtin type");
    }

    MetadataResponse
    visitBuiltinFloatType(CanBuiltinFloatType type,
                          DynamicMetadataRequest request) {
      return emitDirectMetadataRef(type);
    }

    MetadataResponse
    visitBuiltinVectorType(CanBuiltinVectorType type,
                           DynamicMetadataRequest request) {
      return emitDirectMetadataRef(type);
    }

    MetadataResponse visitNominalType(CanNominalType type,
                                      DynamicMetadataRequest request) {
      assert(!type->isExistentialType());
      return emitNominalMetadataRef(IGF, type->getDecl(), type, request);
    }

    MetadataResponse visitBoundGenericType(CanBoundGenericType type,
                                           DynamicMetadataRequest request) {
      assert(!type->isExistentialType());
      return emitNominalMetadataRef(IGF, type->getDecl(), type, request);
    }

    MetadataResponse visitPackType(CanPackType type,
                                   DynamicMetadataRequest request) {
      return emitTypeMetadataPackRef(IGF, type, request);
    }

    MetadataResponse visitSILPackType(CanSILPackType type,
                                      DynamicMetadataRequest request) {
      llvm_unreachable("cannot emit metadata for a SIL pack type");
    }

    MetadataResponse visitPackExpansionType(CanPackExpansionType type,
                                            DynamicMetadataRequest request) {
      llvm_unreachable("cannot emit metadata for a pack expansion by itself");
    }

    MetadataResponse visitTupleType(CanTupleType type,
                                    DynamicMetadataRequest request) {
      if (auto cached = tryGetLocal(type, request))
        return cached;

      auto response = emitTupleTypeMetadataRef(IGF, type, request);

      return setLocal(type, response);
    }

    MetadataResponse visitGenericFunctionType(CanGenericFunctionType type,
                                              DynamicMetadataRequest request) {
      IGF.unimplemented(SourceLoc(),
                        "metadata ref for generic function type");
      return MetadataResponse::getUndef(IGF);
    }

    llvm::Value *getFunctionParameterRef(AnyFunctionType::CanParam &param) {
      auto type = param.getPlainType()->getCanonicalType();
      return IGF.emitAbstractTypeMetadataRef(type);
    }

    MetadataResponse visitFunctionType(CanFunctionType type,
                                       DynamicMetadataRequest request) {
      if (auto metatype = tryGetLocal(type, request))
        return metatype;

      auto result =
        IGF.emitAbstractTypeMetadataRef(type->getResult()->getCanonicalType());

      auto params = type.getParams();
      auto numParams = params.size();

      // Retrieve the ABI parameter flags from the type-level parameter
      // flags.
      auto getABIParameterFlags = [](ParameterTypeFlags flags) {
        return ParameterFlags()
            .withValueOwnership(flags.getValueOwnership())
            .withVariadic(flags.isVariadic())
            .withAutoClosure(flags.isAutoClosure())
            .withNoDerivative(flags.isNoDerivative())
            .withIsolated(flags.isIsolated());
      };

      bool hasParameterFlags = false;
      for (auto param : params) {
        if (!getABIParameterFlags(param.getParameterFlags()).isNone()) {
          hasParameterFlags = true;
          break;
        }
      }

      // Map the convention to a runtime metadata value.
      FunctionMetadataConvention metadataConvention;
      bool isEscaping = false;
      switch (type->getRepresentation()) {
      case FunctionTypeRepresentation::Swift:
        metadataConvention = FunctionMetadataConvention::Swift;
        isEscaping = !type->isNoEscape();
        break;
      case FunctionTypeRepresentation::Thin:
        metadataConvention = FunctionMetadataConvention::Thin;
        break;
      case FunctionTypeRepresentation::Block:
        metadataConvention = FunctionMetadataConvention::Block;
        break;
      case FunctionTypeRepresentation::CFunctionPointer:
        metadataConvention = FunctionMetadataConvention::CFunctionPointer;
        break;
      }

      FunctionMetadataDifferentiabilityKind metadataDifferentiabilityKind;
      switch (type->getDifferentiabilityKind()) {
      case DifferentiabilityKind::NonDifferentiable:
        metadataDifferentiabilityKind =
            FunctionMetadataDifferentiabilityKind::NonDifferentiable;
        break;
      case DifferentiabilityKind::Normal:
        metadataDifferentiabilityKind =
            FunctionMetadataDifferentiabilityKind::Normal;
        break;
      case DifferentiabilityKind::Linear:
        metadataDifferentiabilityKind =
            FunctionMetadataDifferentiabilityKind::Linear;
        break;
      case DifferentiabilityKind::Forward:
        metadataDifferentiabilityKind =
            FunctionMetadataDifferentiabilityKind::Forward;
        break;
      case DifferentiabilityKind::Reverse:
        metadataDifferentiabilityKind =
            FunctionMetadataDifferentiabilityKind::Reverse;
        break;
      }

      auto flags = FunctionTypeFlags()
                       .withNumParameters(numParams)
                       .withConvention(metadataConvention)
                       .withAsync(type->isAsync())
                       .withConcurrent(type->isSendable())
                       .withThrows(type->isThrowing())
                       .withParameterFlags(hasParameterFlags)
                       .withEscaping(isEscaping)
                       .withDifferentiable(type->isDifferentiable())
                       .withGlobalActor(!type->getGlobalActor().isNull());

      auto flagsVal = llvm::ConstantInt::get(IGF.IGM.SizeTy,
                                             flags.getIntValue());
      llvm::Value *diffKindVal = nullptr;
      if (type->isDifferentiable()) {
        assert(metadataDifferentiabilityKind.isDifferentiable());
        diffKindVal = llvm::ConstantInt::get(
            IGF.IGM.SizeTy, metadataDifferentiabilityKind.getIntValue());
      } else if (type->getGlobalActor()) {
        diffKindVal = llvm::ConstantInt::get(
            IGF.IGM.SizeTy,
            FunctionMetadataDifferentiabilityKind::NonDifferentiable);
      }

      auto collectParameters =
          [&](llvm::function_ref<void(unsigned, llvm::Value *,
                                      ParameterFlags flags)>
                  processor) {
            for (auto index : indices(params)) {
              auto param = params[index];
              auto flags = param.getParameterFlags();

              auto parameterFlags = getABIParameterFlags(flags);
              processor(index, getFunctionParameterRef(param), parameterFlags);
            }
          };

      auto constructSimpleCall =
          [&](llvm::SmallVectorImpl<llvm::Value *> &arguments)
          -> FunctionPointer {
        arguments.push_back(flagsVal);

        collectParameters([&](unsigned i, llvm::Value *typeRef,
                              ParameterFlags flags) {
          arguments.push_back(typeRef);
          if (hasParameterFlags)
            arguments.push_back(
                llvm::ConstantInt::get(IGF.IGM.Int32Ty, flags.getIntValue()));
        });

        arguments.push_back(result);

        switch (params.size()) {
        case 0:
          return IGF.IGM.getGetFunctionMetadata0FunctionPointer();

        case 1:
          return IGF.IGM.getGetFunctionMetadata1FunctionPointer();

        case 2:
          return IGF.IGM.getGetFunctionMetadata2FunctionPointer();

        case 3:
          return IGF.IGM.getGetFunctionMetadata3FunctionPointer();

        default:
          llvm_unreachable("supports only 1/2/3 parameter functions");
        }
      };

      switch (numParams) {
      case 0:
      case 1:
      case 2:
      case 3: {
        if (!hasParameterFlags && !type->isDifferentiable() &&
            !type->getGlobalActor()) {
          llvm::SmallVector<llvm::Value *, 8> arguments;
          auto metadataFn = constructSimpleCall(arguments);
          auto *call = IGF.Builder.CreateCall(metadataFn, arguments);
          call->setDoesNotThrow();
          return setLocal(CanType(type), MetadataResponse::forComplete(call));
        }

        // If function type has parameter flags or is differentiable or has a
        // global actor, emit the most general function to retrieve them.
        LLVM_FALLTHROUGH;
      }

      default:
        assert((!params.empty() || type->isDifferentiable() ||
                type->getGlobalActor()) &&
               "0 parameter case should be specialized unless it is a "
               "differentiable function or has a global actor");

        auto *const Int32Ptr = IGF.IGM.Int32Ty->getPointerTo();
        llvm::SmallVector<llvm::Value *, 8> arguments;

        arguments.push_back(flagsVal);

        if (diffKindVal) {
          arguments.push_back(diffKindVal);
        }

        ConstantInitBuilder paramFlags(IGF.IGM);
        auto flagsArr = paramFlags.beginArray();

        Address parameters;
        if (!params.empty()) {
          auto arrayTy =
              llvm::ArrayType::get(IGF.IGM.TypeMetadataPtrTy, numParams);
          parameters = IGF.createAlloca(
              arrayTy, IGF.IGM.getTypeMetadataAlignment(), "function-parameters");

          IGF.Builder.CreateLifetimeStart(parameters,
                                          IGF.IGM.getPointerSize() * numParams);

          collectParameters([&](unsigned i, llvm::Value *typeRef,
                                ParameterFlags flags) {
            auto argPtr = IGF.Builder.CreateStructGEP(parameters, i,
                                                      IGF.IGM.getPointerSize());
            IGF.Builder.CreateStore(typeRef, argPtr);
            if (i == 0)
              arguments.push_back(argPtr.getAddress());

            if (hasParameterFlags)
              flagsArr.addInt32(flags.getIntValue());
          });
        } else {
          auto parametersPtr =
              llvm::ConstantPointerNull::get(
                IGF.IGM.TypeMetadataPtrTy->getPointerTo());
          arguments.push_back(parametersPtr);
        }

        if (hasParameterFlags) {
          auto *flagsVar = flagsArr.finishAndCreateGlobal(
              "parameter-flags", IGF.IGM.getPointerAlignment(),
              /* constant */ true);
          arguments.push_back(IGF.Builder.CreateBitCast(flagsVar, Int32Ptr));
        } else {
          flagsArr.abandon();
          arguments.push_back(llvm::ConstantPointerNull::get(Int32Ptr));
        }

        arguments.push_back(result);

        if (Type globalActor = type->getGlobalActor()) {
          arguments.push_back(
              IGF.emitAbstractTypeMetadataRef(globalActor->getCanonicalType()));
        }

        auto getMetadataFn =
            type->getGlobalActor()
                ? (IGF.IGM.isConcurrencyAvailable()
                       ? IGF.IGM
                             .getGetFunctionMetadataGlobalActorFunctionPointer()
                       : IGF.IGM
                             .getGetFunctionMetadataGlobalActorBackDeployFunctionPointer())
            : type->isDifferentiable()
                ? IGF.IGM.getGetFunctionMetadataDifferentiableFunctionPointer()
                : IGF.IGM.getGetFunctionMetadataFunctionPointer();

        auto call = IGF.Builder.CreateCall(getMetadataFn, arguments);
        call->setDoesNotThrow();

        if (parameters.isValid())
          IGF.Builder.CreateLifetimeEnd(parameters,
                                        IGF.IGM.getPointerSize() * numParams);

        return setLocal(type, MetadataResponse::forComplete(call));
      }
    }

    MetadataResponse visitMetatypeType(CanMetatypeType type,
                                       DynamicMetadataRequest request) {
      // FIXME: We shouldn't accept a lowered metatype here, but we need to
      // represent Optional<@objc_metatype T.Type> as an AST type for ABI
      // reasons.
      
      // assert(!type->hasRepresentation()
      //       && "should not be asking for a representation-specific metatype "
      //          "metadata");
      
      if (auto metatype = tryGetLocal(type, request))
        return metatype;

      auto instMetadata =
        IGF.emitAbstractTypeMetadataRef(type.getInstanceType());

      auto fn = IGF.IGM.getGetMetatypeMetadataFunctionPointer();
      auto call = IGF.Builder.CreateCall(fn, instMetadata);
      call->setDoesNotThrow();

      return setLocal(type, MetadataResponse::forComplete(call));
    }

    MetadataResponse
    visitExistentialMetatypeType(CanExistentialMetatypeType type,
                                 DynamicMetadataRequest request) {
      if (auto metatype = tryGetLocal(type, request))
        return metatype;

      // Existential metatypes for extended existentials don't use
      // ExistentialMetatypeMetadata.
      if (usesExtendedExistentialMetadata(type)) {
        auto metadata = emitExtendedExistentialTypeMetadata(type);
        return setLocal(type, MetadataResponse::forComplete(metadata));
      }

      // Otherwise, emit the instance type metadata and wrap it in an
      // ExistentialMetatypeMetadata.
      auto instMetadata =
        IGF.emitAbstractTypeMetadataRef(type.getExistentialInstanceType());

      auto fn = IGF.IGM.getGetExistentialMetatypeMetadataFunctionPointer();
      auto call = IGF.Builder.CreateCall(fn, instMetadata);
      call->setDoesNotThrow();

      return setLocal(type, MetadataResponse::forComplete(call));
    }

    MetadataResponse visitModuleType(CanModuleType type,
                                     DynamicMetadataRequest request) {
      IGF.unimplemented(SourceLoc(), "metadata ref for module type");
      return MetadataResponse::getUndef(IGF);
    }

    MetadataResponse visitDynamicSelfType(CanDynamicSelfType type,
                                          DynamicMetadataRequest request) {
      return MetadataResponse::forComplete(IGF.getDynamicSelfMetadata());
    }
      
    MetadataResponse visitExistentialType(CanExistentialType type,
                                          DynamicMetadataRequest request) {
      if (auto metadata = tryGetLocal(type, request))
        return metadata;

      // These currently aren't wrapped in ExistentialType, but we
      // can future-proof against them ending up in this path.
      if (type->isAny() || type->isAnyObject())
        return emitSingletonExistentialTypeMetadata(type);

      auto metadata = emitExistentialTypeMetadata(type);
      return setLocal(type, MetadataResponse::forComplete(metadata));
    }

    MetadataResponse emitSingletonExistentialTypeMetadata(CanType type) {
      assert(type->isAny() || type->isAnyObject());

      // Any and AnyObject have singleton metadata in the runtime.
      llvm::Constant *singletonMetadata = nullptr;
      if (type->isAny())
        singletonMetadata = IGF.IGM.getAnyExistentialMetadata();
      if (type->isAnyObject())
        singletonMetadata = IGF.IGM.getAnyObjectExistentialMetadata();
      
      llvm::Constant *indices[] = {
        llvm::ConstantInt::get(IGF.IGM.Int32Ty, 0),
        llvm::ConstantInt::get(IGF.IGM.Int32Ty, 1)
      };
      return MetadataResponse::forComplete(
          llvm::ConstantExpr::getInBoundsGetElementPtr(
              IGF.IGM.FullExistentialTypeMetadataStructTy, singletonMetadata, indices));
    }

    llvm::Value *emitExistentialTypeMetadata(CanExistentialType type) {
      auto layout = type.getExistentialLayout();

      if (layout.containsParameterized) {
        return emitExtendedExistentialTypeMetadata(type);
      }

      SmallVector<ProtocolDecl *, 4> protocols;
      for (auto proto : layout.getProtocols()) {
        if (!proto->isMarkerProtocol())
          protocols.push_back(proto);
      }

      // Collect references to the protocol descriptors.
      auto descriptorArrayTy
        = llvm::ArrayType::get(IGF.IGM.ProtocolDescriptorRefTy,
                               protocols.size());
      Address descriptorArray = IGF.createAlloca(descriptorArrayTy,
                                                 IGF.IGM.getPointerAlignment(),
                                                 "protocols");
      IGF.Builder.CreateLifetimeStart(descriptorArray,
                                   IGF.IGM.getPointerSize() * protocols.size());
      descriptorArray = IGF.Builder.CreateElementBitCast(
          descriptorArray, IGF.IGM.ProtocolDescriptorRefTy);

      unsigned index = 0;
      for (auto *protoDecl : protocols) {
        llvm::Value *ref = emitProtocolDescriptorRef(IGF, protoDecl);

        Address slot = IGF.Builder.CreateConstArrayGEP(descriptorArray,
                                               index, IGF.IGM.getPointerSize());
        IGF.Builder.CreateStore(ref, slot);
        ++index;
      }

      // Note: ProtocolClassConstraint::Class is 0, ::Any is 1.
      auto classConstraint =
        llvm::ConstantInt::get(IGF.IGM.Int1Ty,
                               !layout.requiresClass());
      llvm::Value *superclassConstraint =
        llvm::ConstantPointerNull::get(IGF.IGM.TypeMetadataPtrTy);
      if (auto superclass = layout.explicitSuperclass) {
        superclassConstraint = IGF.emitAbstractTypeMetadataRef(
          CanType(superclass));
      }

      auto call = IGF.Builder.CreateCall(
          IGF.IGM.getGetExistentialMetadataFunctionPointer(),
          {classConstraint, superclassConstraint,
           IGF.IGM.getSize(Size(protocols.size())),
           descriptorArray.getAddress()});
      call->setDoesNotThrow();
      IGF.Builder.CreateLifetimeEnd(descriptorArray,
                                   IGF.IGM.getPointerSize() * protocols.size());
      return call;
    }

    llvm::Value *emitExtendedExistentialTypeMetadata(CanType type) {
      assert(type.isAnyExistentialType());
      auto shapeInfo = ExtendedExistentialTypeShapeInfo::get(type);
      llvm::Constant *shape =
        emitExtendedExistentialTypeShape(IGF.IGM, shapeInfo);
      bool shapeIsUnique = shapeInfo.isUnique();

      // Emit a reference to the extended existential shape,
      // signed appropriately.
      shape = llvm::ConstantExpr::getBitCast(shape, IGF.IGM.Int8PtrTy);
      if (auto &schema = shapeIsUnique
            ? IGF.getOptions().PointerAuth.ExtendedExistentialTypeShape
            : IGF.getOptions().PointerAuth.NonUniqueExtendedExistentialTypeShape) {
        shape = IGF.IGM.getConstantSignedPointer(shape, schema,
                                                 PointerAuthEntity(),
                                                 /*address*/ nullptr);
      }

      // Emit the generalization arguments.
      GenericArguments genericArgs;
      Address argsBuffer;
      llvm::Value *argsPointer;
      if (shapeInfo.genSubs.empty()) {
        argsPointer = llvm::UndefValue::get(IGF.IGM.Int8PtrPtrTy);
      } else {
        genericArgs.collect(IGF, shapeInfo.genSubs);
        argsBuffer = createGenericArgumentsArray(IGF, genericArgs.Values);
        argsPointer =
          IGF.Builder.CreateBitCast(argsBuffer.getAddress(),
                                    IGF.IGM.Int8PtrPtrTy);
      }

      // Call the metadata access function in the runtime.
      auto call = IGF.Builder.CreateCall(
          shapeIsUnique
              ? IGF.IGM
                    .getGetExtendedExistentialTypeMetadataUniqueFunctionPointer()
              : IGF.IGM.getGetExtendedExistentialTypeMetadataFunctionPointer(),
          {shape, argsPointer});
      call->setDoesNotThrow();

      // Destroy the generalization arguments array, if we made one.
      if (!shapeInfo.genSubs.empty())
        destroyGenericArgumentsArray(IGF, argsBuffer, genericArgs.Values);

      return call;
    }

    MetadataResponse visitProtocolType(CanProtocolType type,
                                       DynamicMetadataRequest request) {
      assert(false && "constraint type should be wrapped in existential type");

      CanExistentialType existential(
          ExistentialType::get(type)->castTo<ExistentialType>());

      if (auto metatype = tryGetLocal(existential, request))
        return metatype;

      auto metadata = emitExistentialTypeMetadata(existential);
      return setLocal(type, MetadataResponse::forComplete(metadata));
    }

    MetadataResponse
    visitProtocolCompositionType(CanProtocolCompositionType type,
                                 DynamicMetadataRequest request) {
      if (type->isAny() || type->isAnyObject())
        return emitSingletonExistentialTypeMetadata(type);

      assert(false && "constraint type should be wrapped in existential type");

      CanExistentialType existential(
          ExistentialType::get(type)->castTo<ExistentialType>());

      if (auto metatype = tryGetLocal(existential, request))
        return metatype;

      auto metadata = emitExistentialTypeMetadata(existential);
      return setLocal(type, MetadataResponse::forComplete(metadata));
    }

    MetadataResponse
    visitParameterizedProtocolType(CanParameterizedProtocolType type,
                                   DynamicMetadataRequest request) {
      llvm_unreachable("constraint type should be wrapped in existential type");
    }

    MetadataResponse visitReferenceStorageType(CanReferenceStorageType type,
                                               DynamicMetadataRequest request) {
      llvm_unreachable("reference storage type should have been converted by "
                       "SILGen");
    }
    MetadataResponse visitSILFunctionType(CanSILFunctionType type,
                                          DynamicMetadataRequest request) {
      llvm_unreachable("should not be asking for metadata of a lowered SIL "
                       "function type--SILGen should have used the AST type");
    }
    MetadataResponse visitSILTokenType(CanSILTokenType type,
                                          DynamicMetadataRequest request) {
      llvm_unreachable("should not be asking for metadata of a SILToken type");
    }
    MetadataResponse
    visitSILMoveOnlyWrappedType(CanSILMoveOnlyWrappedType type,
                                DynamicMetadataRequest request) {
      llvm_unreachable("should not be asking for metadata of a move only type");
    }
    MetadataResponse visitArchetypeType(CanArchetypeType type,
                                        DynamicMetadataRequest request) {
      if (auto packArchetypeType = dyn_cast<PackArchetypeType>(type))
        return emitPackArchetypeMetadataRef(IGF, packArchetypeType, request);

      return emitArchetypeTypeMetadataRef(IGF, type, request);
    }

    MetadataResponse visitGenericTypeParamType(CanGenericTypeParamType type,
                                               DynamicMetadataRequest request) {
      llvm_unreachable("dependent type should have been substituted by Sema or SILGen");
    }

    MetadataResponse visitDependentMemberType(CanDependentMemberType type,
                                              DynamicMetadataRequest request) {
      llvm_unreachable("dependent type should have been substituted by Sema or SILGen");
    }

    MetadataResponse visitLValueType(CanLValueType type,
                                     DynamicMetadataRequest request) {
      llvm_unreachable("lvalue type should have been lowered by SILGen");
    }
    MetadataResponse visitInOutType(CanInOutType type,
                                    DynamicMetadataRequest request) {
      llvm_unreachable("inout type should have been lowered by SILGen");
    }
    MetadataResponse visitErrorType(CanErrorType type,
                                    DynamicMetadataRequest request) {
      llvm_unreachable("error type should not appear in IRGen");
    }

    // These types are artificial types used for internal purposes and
    // should never appear in a metadata request.
#define INTERNAL_ONLY_TYPE(ID)                                               \
    MetadataResponse visit##ID##Type(Can##ID##Type type,                     \
                                     DynamicMetadataRequest request) {       \
      llvm_unreachable("cannot ask for metadata of compiler-internal type"); \
    }
    INTERNAL_ONLY_TYPE(SILBlockStorage)
    INTERNAL_ONLY_TYPE(BuiltinDefaultActorStorage)
    INTERNAL_ONLY_TYPE(BuiltinNonDefaultDistributedActorStorage)
#undef INTERNAL_ONLY_TYPE

    MetadataResponse visitSILBoxType(CanSILBoxType type,
                                     DynamicMetadataRequest request) {
      // The Builtin.NativeObject metadata can stand in for boxes.
      return emitDirectMetadataRef(type->getASTContext().TheNativeObjectType);
    }

    /// Try to find the metatype in local data.
    MetadataResponse tryGetLocal(CanType type, DynamicMetadataRequest request) {
      return IGF.tryGetLocalTypeMetadata(type, request);
    }

    /// Set the metatype in local data.
    MetadataResponse setLocal(CanType type, MetadataResponse response) {
      IGF.setScopedLocalTypeMetadata(type, response);
      return response;
    }
  };
} // end anonymous namespace

/// Emit a type metadata reference without using an accessor function.
static MetadataResponse emitDirectTypeMetadataRef(IRGenFunction &IGF,
                                                  CanType type,
                                           DynamicMetadataRequest request) {
  return EmitTypeMetadataRef(IGF).visit(type, request);
}

static bool isLoadFrom(llvm::Value *value, Address address) {
  if (auto load = dyn_cast<llvm::LoadInst>(value)) {
    return load->getOperand(0) == address.getAddress();
  }
  return false;
}

/// Emit the body of a cache accessor.
///
/// If cacheVariable is null, we perform the direct access every time.
/// This is used for metadata accessors that come about due to resilience,
/// where the direct access is completely trivial.
void irgen::emitCacheAccessFunction(IRGenModule &IGM, llvm::Function *accessor,
                                    llvm::Constant *cacheVariable,
                                    llvm::Type *cacheTy,
                                    CacheStrategy cacheStrategy,
                                    CacheEmitter getValue, bool isReadNone) {
  assert((cacheStrategy == CacheStrategy::None) == (cacheVariable == nullptr));
  accessor->setDoesNotThrow();
  // Don't inline cache functions, since doing so has little impact on
  // overall performance.
  accessor->addFnAttr(llvm::Attribute::NoInline);
  // Accessor functions don't need frame pointers.
  IGM.setHasNoFramePointer(accessor);
  IGM.setColocateMetadataSection(accessor);

  // This function is logically 'readnone': the caller does not need
  // to reason about any side effects or stores it might perform.
  if (isReadNone)
    accessor->setDoesNotAccessMemory();

  IRGenFunction IGF(IGM, accessor);
  if (IGM.DebugInfo)
    IGM.DebugInfo->emitArtificialFunction(IGF, accessor);

  auto parameters = IGF.collectParameters();

  bool returnsResponse =
    (accessor->getReturnType() == IGM.TypeMetadataResponseTy);

  switch (cacheStrategy) {

  // If there's no cache variable, just perform the direct access.
  case CacheStrategy::None: {
    auto response = getValue(IGF, parameters);
    llvm::Value *ret;
    if (returnsResponse) {
      response.ensureDynamicState(IGF);
      ret = response.combine(IGF);
    } else {
      assert(response.isStaticallyKnownComplete());
      ret = response.getMetadata();
    }
    IGF.Builder.CreateRet(ret);
    return;
  }

  // For in-place initialization, drill to the first element of the cache.
  case CacheStrategy::SingletonInitialization:
    cacheVariable =
      llvm::ConstantExpr::getBitCast(cacheVariable,
                                     IGM.TypeMetadataPtrTy->getPointerTo());
    break;

  case CacheStrategy::Lazy:
    break;
  }

  llvm::Constant *null =
      llvm::ConstantPointerNull::get(cast<llvm::PointerType>(cacheTy));

  Address cache(cacheVariable, cacheTy, IGM.getPointerAlignment());

  // Okay, first thing, check the cache variable.
  //
  // Conceptually, this needs to establish memory ordering with the
  // store we do later in the function: if the metadata value is
  // non-null, we must be able to see any stores performed by the
  // initialization of the metadata.  However, any attempt to read
  // from the metadata will be address-dependent on the loaded
  // metadata pointer, which is sufficient to provide adequate
  // memory ordering guarantees on all the platforms we care about:
  // ARM has special rules about address dependencies, and x86's
  // memory ordering is strong enough to guarantee the visibility
  // even without the address dependency.
  //
  // And we do not need to worry about the compiler because the
  // address dependency naturally forces an order to the memory
  // accesses.
  //
  // Therefore, we can perform a completely naked load here.
  // FIXME: Technically should be "consume", but that introduces barriers in the
  // current LLVM ARM backend.
  auto load = IGF.Builder.CreateLoad(cache);
  // Make this barrier explicit when building for TSan to avoid false positives.
  if (IGM.IRGen.Opts.Sanitizers & SanitizerKind::Thread)
    load->setOrdering(llvm::AtomicOrdering::Acquire);

  // Compare the load result against null.
  auto isNullBB = IGF.createBasicBlock("cacheIsNull");
  auto contBB = IGF.createBasicBlock("cont");
  llvm::Value *comparison = IGF.Builder.CreateICmpEQ(load, null);
  IGF.Builder.CreateCondBr(comparison, isNullBB, contBB);
  auto loadBB = IGF.Builder.GetInsertBlock();

  // If the load yielded null, emit the type metadata.
  IGF.Builder.emitBlock(isNullBB);
  
  MetadataResponse response = getValue(IGF, parameters);

  // Ensure that we have a dynamically-correct state value.
  llvm::Constant *completedState = nullptr;
  if (returnsResponse) {
    completedState = MetadataResponse::getCompletedState(IGM);
    response.ensureDynamicState(IGF);
  }

  auto directResult = response.getMetadata();

  // Emit a branch around the caching code if we're working with responses
  // and the fetched result is not complete.  We can avoid doing this if
  // the response is statically known to be complete, and we don't need to
  // do it if this is an in-place initialization cache because the store
  // is done within the runtime.
  llvm::BasicBlock *completionCheckBB = nullptr;
  llvm::Value *directState = nullptr;
  if (cacheStrategy == CacheStrategy::SingletonInitialization) {
    directState = response.getDynamicState();
    completionCheckBB = IGF.Builder.GetInsertBlock();
  } else {
    if (returnsResponse &&
        !response.isStaticallyKnownComplete()) {
      completionCheckBB = IGF.Builder.GetInsertBlock();
      directState = response.getDynamicState();

      auto isCompleteBB = IGF.createBasicBlock("is_complete");
      auto isComplete =
        IGF.Builder.CreateICmpEQ(directState, completedState);

      IGF.Builder.CreateCondBr(isComplete, isCompleteBB, contBB);
      IGF.Builder.emitBlock(isCompleteBB);
    }

    // Store it back to the cache variable.  This needs to be a store-release
    // because it needs to propagate memory visibility to the other threads
    // that can access the cache: the initializing stores might be visible
    // to this thread, but they aren't transitively guaranteed to be visible
    // to other threads unless this is a store-release.
    //
    // However, we can skip this if the value was actually loaded from the
    // cache.  This is a simple, if hacky, peephole that's useful for the
    // code in emitOnceTypeMetadataAccessFunctionBody.
    if (!isLoadFrom(directResult, cache)) {
      IGF.Builder.CreateStore(directResult, cache)
        ->setAtomic(llvm::AtomicOrdering::Release);
    }
  }

  IGF.Builder.CreateBr(contBB);
  auto storeBB = IGF.Builder.GetInsertBlock();

  // Emit the continuation block.
  IGF.Builder.emitBlock(contBB);

  // Add a phi for the metadata value.
  auto phi = IGF.Builder.CreatePHI(null->getType(), 3);
  phi->addIncoming(load, loadBB);
  phi->addIncoming(directResult, storeBB);

  // Add a phi for the metadata state if we're returning a response.
  llvm::Value *stateToReturn = nullptr;
  if (directState) {
    if (storeBB != completionCheckBB)
      phi->addIncoming(directResult, completionCheckBB);

    auto completionStatePHI = IGF.Builder.CreatePHI(IGM.SizeTy, 3);
    completionStatePHI->addIncoming(completedState, loadBB);
    completionStatePHI->addIncoming(directState, completionCheckBB);
    if (storeBB != completionCheckBB)
      completionStatePHI->addIncoming(completedState, storeBB);
    stateToReturn = completionStatePHI;
  } else if (returnsResponse) {
    stateToReturn = completedState;
  }

  // Build the return value.
  llvm::Value *ret;
  if (returnsResponse) {
    ret = MetadataResponse(phi, stateToReturn, MetadataState::Abstract)
            .combine(IGF);
  } else {
    ret = phi;
  }

  IGF.Builder.CreateRet(ret);
}

MetadataResponse
IRGenFunction::emitGenericTypeMetadataAccessFunctionCall(
                                              llvm::Function *accessFunction,
                                              ArrayRef<llvm::Value *> args,
                                              DynamicMetadataRequest request) {

  SmallVector<llvm::Value *, 8> callArgs;

  // Add the metadata request argument.
  callArgs.push_back(request.get(*this));

  Address argsBuffer;
  bool allocatedArgsBuffer = false;
  if (args.size() > NumDirectGenericTypeMetadataAccessFunctionArgs) {
    argsBuffer = createGenericArgumentsArray(*this, args);
    allocatedArgsBuffer = true;

    // Add the buffer to the call arguments.
    callArgs.push_back(
      Builder.CreateBitCast(argsBuffer.getAddress(), IGM.Int8PtrPtrTy));
  } else {
    callArgs.append(args.begin(), args.end());
  }

  auto call = Builder.CreateCall(accessFunction->getFunctionType(),
                                 accessFunction, callArgs);
  call->setDoesNotThrow();
  call->setCallingConv(IGM.SwiftCC);
  call->addFnAttr(allocatedArgsBuffer
                      ? llvm::Attribute::InaccessibleMemOrArgMemOnly
                      : llvm::Attribute::ReadNone);

  // If we allocated a buffer for the arguments, end its lifetime.
  if (allocatedArgsBuffer)
    destroyGenericArgumentsArray(*this, argsBuffer, args);

  return MetadataResponse::handle(*this, request, call);
}

MetadataResponse irgen::emitGenericTypeMetadataAccessFunction(
    IRGenFunction &IGF, Explosion &params, NominalTypeDecl *nominal,
    GenericArguments &genericArgs) {
  auto &IGM = IGF.IGM;
  
  llvm::Value *descriptor =
    IGM.getAddrOfTypeContextDescriptor(nominal, RequireMetadata);

  // Sign the descriptor.
  auto schema = IGF.IGM.getOptions().PointerAuth.TypeDescriptorsAsArguments;
  if (schema) {
    auto authInfo = PointerAuthInfo::emit(
        IGF, schema, nullptr,
        PointerAuthEntity::Special::TypeDescriptorAsArgument);
    descriptor = emitPointerAuthSign(IGF, descriptor, authInfo);
  }

  auto request = params.claimNext();

  bool checkPrespecialized =
      IGM.IRGen.metadataPrespecializationsForType(nominal).size() > 0;

  auto numArguments = genericArgs.Types.size();

  llvm::Value *result;
  if (numArguments > NumDirectGenericTypeMetadataAccessFunctionArgs) {
    // swift_getGenericMetadata's calling convention is already cleverly
    // laid out to minimize the assembly language size of the thunk.
    // The caller passed us an appropriate buffer with the arguments.
    auto argsBuffer =
        Address(params.claimNext(), IGM.Int8PtrTy, IGM.getPointerAlignment());
    llvm::Value *arguments =
      IGF.Builder.CreateBitCast(argsBuffer.getAddress(), IGM.Int8PtrTy);

    // Make the call.
    llvm::CallInst *call;
    if (checkPrespecialized) {
      call = IGF.Builder.CreateCall(
          IGM.getGetCanonicalPrespecializedGenericMetadataFunctionPointer(),
          {request, arguments, descriptor,
           IGM.getAddrOfCanonicalPrespecializedGenericTypeCachingOnceToken(
               nominal)});
    } else {
      call = IGF.Builder.CreateCall(IGM.getGetGenericMetadataFunctionPointer(),
                                    {request, arguments, descriptor});
    }
    call->setDoesNotThrow();
    call->setCallingConv(IGM.SwiftCC);
    call->addFnAttr(llvm::Attribute::ReadOnly);
    result = call;
  } else {
    static_assert(NumDirectGenericTypeMetadataAccessFunctionArgs == 3,
                  "adjust this if you change "
                  "NumDirectGenericTypeMetadataAccessFunctionArgs");
    // Factor out the buffer shuffling for metadata accessors that take their
    // arguments directly, so that the accessor function itself only needs to
    // materialize the nominal type descriptor and call this thunk.
    auto generateThunkFn = [&IGM,
                            checkPrespecialized](IRGenFunction &subIGF) {
      subIGF.CurFn->setOnlyReadsMemory();
      subIGF.CurFn->setWillReturn();
      subIGF.CurFn->setCallingConv(IGM.SwiftCC);
      if (IGM.DebugInfo)
        IGM.DebugInfo->emitArtificialFunction(subIGF, subIGF.CurFn);
      IGM.setHasNoFramePointer(subIGF.CurFn);

      auto params = subIGF.collectParameters();
      auto request = params.claimNext();
      auto arg0 = params.claimNext();
      auto arg1 = params.claimNext();
      auto arg2 = params.claimNext();
      auto descriptor = params.claimNext();
      llvm::Value *token = nullptr;
      if (checkPrespecialized) {
        token = params.claimNext();
      }

      // Allocate a buffer with enough storage for the arguments.
      auto argsBufferTy =
        llvm::ArrayType::get(IGM.Int8PtrTy,
                             NumDirectGenericTypeMetadataAccessFunctionArgs);
      auto argsBuffer = subIGF.createAlloca(argsBufferTy,
                                         IGM.getPointerAlignment(),
                                         "generic.arguments");
      subIGF.Builder.CreateLifetimeStart(argsBuffer,
       IGM.getPointerSize() * NumDirectGenericTypeMetadataAccessFunctionArgs);

      auto arg0Buf = subIGF.Builder.CreateConstInBoundsGEP2_32(argsBufferTy,
                                               argsBuffer.getAddress(), 0, 0);
      subIGF.Builder.CreateStore(arg0, arg0Buf, IGM.getPointerAlignment());
      auto arg1Buf = subIGF.Builder.CreateConstInBoundsGEP2_32(argsBufferTy,
                                               argsBuffer.getAddress(), 0, 1);
      subIGF.Builder.CreateStore(arg1, arg1Buf, IGM.getPointerAlignment());
      auto arg2Buf = subIGF.Builder.CreateConstInBoundsGEP2_32(argsBufferTy,
                                               argsBuffer.getAddress(), 0, 2);
      subIGF.Builder.CreateStore(arg2, arg2Buf, IGM.getPointerAlignment());

      // Make the call.
      auto argsAddr = subIGF.Builder.CreateBitCast(argsBuffer.getAddress(),
                                                   IGM.Int8PtrTy);

      llvm::Value *result;
      if (checkPrespecialized) {
        result = subIGF.Builder.CreateCall(
            IGM.getGetCanonicalPrespecializedGenericMetadataFunctionPointer(),
            {request, argsAddr, descriptor, token});
      } else {
        result = subIGF.Builder.CreateCall(
            IGM.getGetGenericMetadataFunctionPointer(),
            {request, argsAddr, descriptor});
      }
      subIGF.Builder.CreateRet(result);
    };
    llvm::Function *thunkFn;
    if (checkPrespecialized) {
      thunkFn = cast<llvm::Function>(IGM.getOrCreateHelperFunction(
          "__swift_instantiateCanonicalPrespecializedGenericMetadata",
          IGM.TypeMetadataResponseTy,
          {
              IGM.SizeTy,                     // request
              IGM.Int8PtrTy,                  // arg 0
              IGM.Int8PtrTy,                  // arg 1
              IGM.Int8PtrTy,                  // arg 2
              IGM.TypeContextDescriptorPtrTy, // type context descriptor
              IGM.OnceTy->getPointerTo()      // token pointer
          },
          generateThunkFn,
          /*noinline*/ true));
    } else {
      thunkFn = cast<llvm::Function>(IGM.getOrCreateHelperFunction(
          "__swift_instantiateGenericMetadata", IGM.TypeMetadataResponseTy,
          {
              IGM.SizeTy,                    // request
              IGM.Int8PtrTy,                 // arg 0
              IGM.Int8PtrTy,                 // arg 1
              IGM.Int8PtrTy,                 // arg 2
              IGM.TypeContextDescriptorPtrTy // type context descriptor
          },
          generateThunkFn,
          /*noinline*/ true));
    }
    IGM.setColocateMetadataSection(thunkFn);

    // Call out to the helper.
    auto getNextParam = [&]() -> llvm::Value * {
      auto *param = params.claimNext();
      if (param->getType()->isPointerTy())
        return IGF.Builder.CreateBitCast(param, IGM.Int8PtrTy);
      return IGF.Builder.CreateIntToPtr(param, IGM.Int8PtrTy);
    };

    auto arg0 = numArguments >= 1
      ? getNextParam()
      : llvm::UndefValue::get(IGM.Int8PtrTy);
    auto arg1 = numArguments >= 2
      ? getNextParam()
      : llvm::UndefValue::get(IGM.Int8PtrTy);
    auto arg2 = numArguments >= 3
      ? getNextParam()
      : llvm::UndefValue::get(IGM.Int8PtrTy);

    llvm::CallInst *call;
    if (checkPrespecialized) {
      auto *token =
          IGM.getAddrOfCanonicalPrespecializedGenericTypeCachingOnceToken(
              nominal);
      call = IGF.Builder.CreateCall(
          thunkFn->getFunctionType(), thunkFn,
          {request, arg0, arg1, arg2, descriptor, token});
    } else {
      call = IGF.Builder.CreateCall(thunkFn->getFunctionType(), thunkFn,
                                    {request, arg0, arg1, arg2, descriptor});
    }
    call->setDoesNotAccessMemory();
    call->setDoesNotThrow();
    call->setCallingConv(IGM.SwiftCC);
    
    result = call;
  }
  
  return MetadataResponse::handle(IGF, DynamicMetadataRequest(request), result);
}

static void
emitIdempotentCanonicalSpecializedClassMetadataInitializationComponent(
    IRGenFunction &IGF, CanType theType,
    llvm::SmallSet<CanType, 16> &initializedTypes) {
  if (initializedTypes.count(theType) > 0) {
    return;
  }
  initializedTypes.insert(theType);
  auto *classDecl = theType->getClassOrBoundGenericClass();
  assert(classDecl);
  if (classDecl->isGenericContext()) {
    llvm::Function *accessor =
        IGF.IGM.getAddrOfCanonicalSpecializedGenericTypeMetadataAccessFunction(
            theType, NotForDefinition);

    auto request = DynamicMetadataRequest(MetadataState::Complete);
    IGF.emitGenericTypeMetadataAccessFunctionCall(accessor, {}, request);
  } else {
    llvm::Function *accessor =
        IGF.IGM.getAddrOfTypeMetadataAccessFunction(theType, NotForDefinition);
    auto request = DynamicMetadataRequest(MetadataState::Complete);
    IGF.emitGenericTypeMetadataAccessFunctionCall(accessor, {}, request);
  }
}

MetadataResponse
irgen::emitCanonicalSpecializedGenericTypeMetadataAccessFunction(
    IRGenFunction &IGF, Explosion &params, CanType theType) {
  assert(isa<ClassDecl>(theType->getAnyNominal()));

  auto request = params.claimNext();
  // The metadata request that is passed to a canonical specialized generic
  // metadata accessor is ignored because complete metadata is always returned.
  (void)request;
  llvm::SmallSet<CanType, 16> initializedTypes;

  auto *nominal = theType->getAnyNominal();
  assert(nominal);
  assert(isa<ClassDecl>(nominal));
  assert(nominal->isGenericContext());
  assert(!theType->hasUnboundGenericType());

  auto requirements = GenericTypeRequirements(IGF.IGM, nominal);
  auto substitutions =
      theType->getContextSubstitutionMap(IGF.IGM.getSwiftModule(), nominal);
  for (auto requirement : requirements.getRequirements()) {
    if (requirement.isAnyWitnessTable()) {
      continue;
    }
    assert(requirement.isMetadata()); // FIXME: packs and counts
    auto parameter = requirement.getTypeParameter();
    auto noncanonicalArgument = parameter.subst(substitutions);
    auto argument = noncanonicalArgument->getCanonicalType();
    if (auto *classDecl = argument->getClassOrBoundGenericClass()) {
      emitIdempotentCanonicalSpecializedClassMetadataInitializationComponent(
          IGF, argument, initializedTypes);
    }
  }
  Type superclassType = theType->getSuperclass(/*useArchetypes=*/false);
  if (superclassType) {
    emitIdempotentCanonicalSpecializedClassMetadataInitializationComponent(
        IGF, superclassType->getCanonicalType(), initializedTypes);
  }

  auto *uninitializedMetadata = IGF.IGM.getAddrOfTypeMetadata(theType);
  initializedTypes.insert(theType);
  auto *initializedMetadata =
      emitIdempotentClassMetadataInitialization(IGF, uninitializedMetadata);
  return MetadataResponse::forComplete(initializedMetadata);
}

/// Emit the body of a metadata accessor function for the given type.
///
/// This function is appropriate for ordinary situations where the
/// construction of the metadata value just involves calling idempotent
/// metadata-construction functions.  It is not used for the in-place
/// initialization of non-generic nominal type metadata.
static MetadataResponse
emitDirectTypeMetadataAccessFunctionBody(IRGenFunction &IGF,
                                         DynamicMetadataRequest request,
                                         CanType type) {
  assert(!type->hasArchetype() &&
         "cannot emit metadata accessor for context-dependent type");

  // We only take this path for non-generic nominal types.
  auto typeDecl = type->getAnyNominal();
  if (!typeDecl)
    return emitDirectTypeMetadataRef(IGF, type, request);

  if (typeDecl->isGenericContext() &&
      !(isa<ClassDecl>(typeDecl) &&
        isa<ClangModuleUnit>(typeDecl->getModuleScopeContext()))) {
    // This is a metadata accessor for a fully substituted generic type.
    return emitDirectTypeMetadataRef(IGF, type, request);
  }

  // We should never be emitting a metadata accessor for resilient nominal
  // types outside of their defining module.  We'd only do that anyway for
  // types that don't guarantee the existence of a non-unique access
  // function, and that should never be true of a resilient type with
  // external availability.
  //
  // (The type might still not have a statically-known layout.  It just
  // can't be resilient at the top level: we have to know its immediate
  // members, or we can't even begin to approach the problem of emitting
  // metadata for it.)
  assert(!IGF.IGM.isResilient(typeDecl, ResilienceExpansion::Maximal));

  // We should never be emitting a metadata accessor for foreign type
  // metadata using this function.
  assert(!requiresForeignTypeMetadata(typeDecl));

  if (auto classDecl = dyn_cast<ClassDecl>(typeDecl)) {
    // For known-Swift metadata, we can perform a direct reference with
    // potentially idempotent initialization.
    if (hasKnownSwiftMetadata(IGF.IGM, classDecl))
      return emitDirectTypeMetadataRef(IGF, type, request);
  
    // Classes that might not have Swift metadata use a different
    // access pattern.
    return MetadataResponse::forComplete(emitObjCMetadataRef(IGF, classDecl));
  }

  // We should not be doing more serious work along this path.
  assert(isCanonicalCompleteTypeMetadataStaticallyAddressable(IGF.IGM, type));

  // Okay, everything else is built from a Swift metadata object.
  llvm::Constant *metadata = IGF.IGM.getAddrOfTypeMetadata(type);

  return MetadataResponse::forComplete(metadata);
}

static llvm::Function *getAccessFunctionPrototype(IRGenModule &IGM,
                                                  CanType type,
                                               ForDefinition_t forDefinition) {
  assert(!type->hasArchetype());
  // Type should be bound unless it's type erased.
  assert(type.isTypeErasedGenericClassType()
           ? !isa<BoundGenericType>(type)
           : !isa<UnboundGenericType>(type));

  return IGM.getAddrOfTypeMetadataAccessFunction(type, forDefinition);
}

llvm::Function *
irgen::getOtherwiseDefinedTypeMetadataAccessFunction(IRGenModule &IGM,
                                                     CanType type) {
  return getAccessFunctionPrototype(IGM, type, NotForDefinition);
}

/// Get or create an accessor function to the given non-dependent type.
llvm::Function *
irgen::createTypeMetadataAccessFunction(IRGenModule &IGM, CanType type,
                                        CacheStrategy cacheStrategy,
                                        MetadataAccessGenerator generator,
                                        bool allowExistingDefinition) {
  // Get the prototype.
  auto accessor = getAccessFunctionPrototype(IGM, type, ForDefinition);

  // If we're not supposed to define the accessor, or if we already
  // have defined it, just return the pointer.
  if (!accessor->empty()) {
    assert(allowExistingDefinition &&
           "repeat definition of access function!");
    return accessor;
  }

  // Okay, define the accessor.
  llvm::Constant *cacheVariable = nullptr;
  llvm::Type *cacheTy = nullptr;

  // If our preferred access method is to go via an accessor, it means
  // there is some non-trivial computation that needs to be cached.
  if (!shouldCacheTypeMetadataAccess(IGM, type)) {
    cacheStrategy = CacheStrategy::None;
  } else {
    switch (cacheStrategy) {
    // Nothing to do.
    case CacheStrategy::None:
      break;

    // For lazy initialization, the cache variable is just a pointer.
    case CacheStrategy::Lazy:
      cacheVariable = IGM.getAddrOfTypeMetadataLazyCacheVariable(type);
      cacheTy = IGM.TypeMetadataPtrTy;
      break;

    // For in-place initialization, drill down to the first element.
    case CacheStrategy::SingletonInitialization:
      cacheVariable = IGM.getAddrOfTypeMetadataSingletonInitializationCache(
                                          type->getAnyNominal(), ForDefinition);
      cacheTy = IGM.TypeMetadataPtrTy;
      break;
    }

    if (IGM.getOptions().optimizeForSize())
      accessor->addFnAttr(llvm::Attribute::NoInline);
  }

  emitCacheAccessFunction(IGM, accessor, cacheVariable, cacheTy, cacheStrategy,
                          [&](IRGenFunction &IGF, Explosion &params) {
                            auto request =
                                DynamicMetadataRequest(params.claimNext());
                            return generator(IGF, request, cacheVariable);
                          });

  return accessor;
}

/// Emit a standard accessor function to the given non-dependent type.
llvm::Function *
irgen::createDirectTypeMetadataAccessFunction(IRGenModule &IGM, CanType type,
                                              bool allowExistingDefinition) {
  return createTypeMetadataAccessFunction(IGM, type, CacheStrategy::Lazy,
                                          [&](IRGenFunction &IGF,
                                              DynamicMetadataRequest request,
                                              llvm::Constant *cacheVariable) {
    // We should not be called with ForDefinition for nominal types
    // that require in-place initialization.
    return emitDirectTypeMetadataAccessFunctionBody(IGF, request, type);
  }, allowExistingDefinition);
}

/// Get or create an accessor function to the given generic type.
llvm::Function *
irgen::getGenericTypeMetadataAccessFunction(IRGenModule &IGM,
                                            NominalTypeDecl *nominal,
                                            ForDefinition_t shouldDefine) {
  assert(nominal->isGenericContext());
  assert(!nominal->isTypeErasedGenericClass());

  GenericArguments genericArgs;
  genericArgs.collectTypes(IGM, nominal);

  llvm::Function *accessor =
    IGM.getAddrOfGenericTypeMetadataAccessFunction(
        nominal, genericArgs.Types, shouldDefine);
  if (shouldDefine)
    IGM.setColocateMetadataSection(accessor);

  // If we're not supposed to define the accessor, or if we already
  // have defined it, just return the pointer.
  if (!shouldDefine || !accessor->empty())
    return accessor;

  IGM.IRGen.noteUseOfMetadataAccessor(nominal);

  return accessor;
}

static bool shouldAccessByMangledName(IRGenModule &IGM, CanType type) {
  // Never access by mangled name if we've been asked not to.
  if (IGM.getOptions().DisableConcreteTypeMetadataMangledNameAccessors)
    return false;

  // Do not access by mangled name if the runtime won't understand it.
  if (mangledNameIsUnknownToDeployTarget(IGM, type))
    return false;

  // A nongeneric nominal type with nontrivial metadata has an accessor
  // already we can just call.
  if (auto nom = dyn_cast<NominalType>(type)) {
    if (!isa<ProtocolDecl>(nom->getDecl())
        && (!nom->getDecl()->isGenericContext()
            || nom->getDecl()->getGenericSignature()->areAllParamsConcrete())) {
      return false;
    }
  }

  return true;

// The visitor below can be used to fine-tune a heuristic to decide whether
// demangling might be better for code size than open-coding an access. In
// my experiments on the Swift standard library and Apple SDK overlays,
// always demangling seemed to have the biggest code size benefit.
#if false
  // Guess the number of calls and addresses we need to materialize a
  // metadata record in code.
  struct OpenCodedMetadataAccessWeightVisitor
      : CanTypeVisitor<OpenCodedMetadataAccessWeightVisitor>
  {
    IRGenModule &IGM;
    unsigned NumCalls = 0, NumAddresses = 0;
    
    OpenCodedMetadataAccessWeightVisitor(IRGenModule &IGM)
      : IGM(IGM) {}
    
    void visitBoundGenericType(CanBoundGenericType bgt) {
      // Need to materialize all the arguments, then call the metadata
      // accessor.
      //
      // TODO: Also need to count the parent type's generic arguments.
      for (auto arg : bgt->getGenericArgs()) {
        visit(arg);
      }
      NumCalls += 1;
    }

    void visitNominalType(CanNominalType nom) {
      // Some nominal types have trivially-referenceable metadata symbols,
      // others may require accessors to trigger instantiation.
      //
      // TODO: Also need to count the parent type's generic arguments.
      if (!shouldCacheTypeMetadataAccess(IGM, nom)) {
        NumAddresses += 1;
      } else {
        NumCalls += 1;
      }
    }

    void visitPackType(CanPackType tup) {
      llvm_unreachable("Unimplemented!");
    }

    void visitPackExpansionType(CanPackExpansionType tup) {
      llvm_unreachable("Unimplemented!");
    }

    void visitTupleType(CanTupleType tup) {
      // The empty tuple has trivial metadata.
      if (tup->getNumElements() == 0) {
        NumAddresses += 1;
        return;
      }
      // Need to materialize the element types, then call the getTupleMetadata
      // accessor.
      for (auto elt : tup.getElementTypes()) {
        visit(elt);
      }
      NumCalls += 1;
    }
    
    void visitAnyFunctionType(CanAnyFunctionType fun) {
      // Need to materialize the arguments and return, then call the
      // getFunctionMetadata accessor.
      for (auto arg : fun.getParams()) {
        visit(arg.getPlainType());
      }
      visit(fun.getResult());
      
      NumCalls += 1;
    }
    
    void visitMetatypeType(CanMetatypeType meta) {
      // Need to materialize the instance type, then call the
      // getMetatypeMetadata accessor.
      visit(meta.getInstanceType());
      NumCalls += 1;
    }
    
    void visitProtocolType(CanProtocolType proto) {
      // Need to reference the protocol descriptor, then call the
      // getExistentialTypeMetadata accessor.
      NumAddresses += 1;
      NumCalls += 1;
    }
    
    void visitBuiltinType(CanBuiltinType b) {
      // Builtins always have trivial metadata.
      NumAddresses += 1;
    }
    
    void visitProtocolCompositionType(CanProtocolCompositionType comp) {
      unsigned numMembers = comp->getMembers().size();
      // The empty compositions Any and AnyObject are trivial.
      if (numMembers == 0) {
        NumAddresses += 1;
        return;
      }
      // Need to materialize the base class, if any.
      if (comp->getMembers().front()->getClassOrBoundGenericClass()) {
        visit(CanType(comp->getMembers().front()));
        numMembers -= 1;
      }
      // Need to reference the protocol descriptors for each protocol.
      NumAddresses += numMembers;
      // Finally, call the getExistentialTypeMetadata accessor.
      NumCalls += 1;
    }
    
    void visitExistentialMetatypeType(CanExistentialMetatypeType meta) {
      // Extended existential metatypes just emit a different shape
      // and don't do any wrapping.
      if (auto typeAndDepth = usesExtendedExistentialMetadata(meta)) {
        return visit(typeAndDepth.first);
      }

      // The number of accesses turns out the same as the instance type,
      // but instead of getExistentialTypeMetadata, we call
      // getExistentialMetatypeMetadata
      visit(meta.getInstanceType());
    }
    
    // Shouldn't emit metadata for other kinds of types.
    void visitType(CanType t) {
      llvm_unreachable("unhandled type?!");
    }
  };
  
  OpenCodedMetadataAccessWeightVisitor visitor(IGM);
  
  visitor.visit(type);
  
  // If we need more than one accessor call, or the access requires too many
  // arguments, the mangled name accessor is probably more compact.
  return visitor.NumCalls > 1 || visitor.NumAddresses > 1;
#endif
  
}

static bool canIssueIncompleteMetadataRequests(IRGenModule &IGM) {
  // We can only answer blocking complete metadata requests with the <=5.1
  // runtime ABI entry points.
  auto &context = IGM.getSwiftModule()->getASTContext();
  auto deploymentAvailability =
      AvailabilityContext::forDeploymentTarget(context);
  return deploymentAvailability.isContainedIn(
      context.getTypesInAbstractMetadataStateAvailability());
}

/// Emit a call to a type metadata accessor using a mangled name.
static MetadataResponse
emitMetadataAccessByMangledName(IRGenFunction &IGF, CanType type,
                                DynamicMetadataRequest request) {
  assert(!isa<PackType>(type));

  auto &IGM = IGF.IGM;

  // We can only answer blocking complete metadata requests with the <=5.1
  // runtime ABI entry points.
  assert((request.isStaticallyBlockingComplete() ||
          (request.isStaticallyAbstract() &&
           canIssueIncompleteMetadataRequests(IGM))) &&
         "can only form complete metadata by mangled name");

  llvm::Constant *mangledString;
  unsigned mangledStringSize;
  std::tie(mangledString, mangledStringSize) =
    IGM.getTypeRef(type, CanGenericSignature(), MangledTypeRefRole::Metadata);

  // Android AArch64 reserves the top byte of the address for memory tagging
  // since Android 11, so only use the bottom 23 bits to store this size
  // and the 24th bit to signal that there is a size.
  if (IGM.Triple.isAndroid() && IGM.Triple.getArch() == llvm::Triple::aarch64)
    assert(mangledStringSize < 0x00800001u &&
           "8MB of mangled name ought to be enough for Android AArch64");
  else
    assert(mangledStringSize < 0x80000000u &&
           "2GB of mangled name ought to be enough for anyone");
  
  // Get or create the cache variable if necessary.
  auto cache = IGM.getAddrOfTypeMetadataDemanglingCacheVariable(type,
                                                                ConstantInit());
  
  if (cast<llvm::GlobalVariable>(cache->stripPointerCasts())->isDeclaration()) {
    ConstantInitBuilder builder(IGM);
    auto structBuilder = builder.beginStruct();
    
    // A "negative" 64-bit value in the cache indicates the uninitialized state.
    // Which word has that bit in the {i32, i32} layout depends on endianness.

    if (IGM.getModule()->getDataLayout().isBigEndian()) {
      structBuilder.addInt32(-mangledStringSize);
      structBuilder.addRelativeAddress(mangledString);
    } else {
      structBuilder.addRelativeAddress(mangledString);
      structBuilder.addInt32(-mangledStringSize);
    }

    auto init = structBuilder.finishAndCreateFuture();
    cache = IGM.getAddrOfTypeMetadataDemanglingCacheVariable(type, init);
  }

  // Get or create a shared helper function to do the instantiation.
  auto instantiationFnName =
      request.isStaticallyAbstract()
          ? "__swift_instantiateConcreteTypeFromMangledNameAbstract"
          : "__swift_instantiateConcreteTypeFromMangledName";
  auto generateInstantiationFn = [&IGM, request](IRGenFunction &subIGF) {
    subIGF.CurFn->setOnlyReadsMemory();
    subIGF.CurFn->setWillReturn();
    IGM.setHasNoFramePointer(subIGF.CurFn);
    if (IGM.DebugInfo)
      IGM.DebugInfo->emitArtificialFunction(subIGF, subIGF.CurFn);
    IGM.setColocateMetadataSection(subIGF.CurFn);

    auto params = subIGF.collectParameters();
    auto cache = params.claimNext();

    // Load the existing cache value.
    // Conceptually, this needs to establish memory ordering with the
    // store we do later in the function: if the metadata value is
    // non-null, we must be able to see any stores performed by the
    // initialization of the metadata.  However, any attempt to read
    // from the metadata will be address-dependent on the loaded
    // metadata pointer, which is sufficient to provide adequate
    // memory ordering guarantees on all the platforms we care about:
    // ARM has special rules about address dependencies, and x86's
    // memory ordering is strong enough to guarantee the visibility
    // even without the address dependency.
    //
    // And we do not need to worry about the compiler because the
    // address dependency naturally forces an order to the memory
    // accesses.
    //
    // Therefore, we can perform a completely naked load here.
    // FIXME: Technically should be "consume", but that introduces barriers
    // in the current LLVM ARM backend.
    auto cacheWordAddr = subIGF.Builder.CreateBitCast(cache,
                                                 IGM.Int64Ty->getPointerTo());
    auto load = subIGF.Builder.CreateLoad(
        Address(cacheWordAddr, IGM.Int64Ty, Alignment(8)));
    // Make this barrier explicit when building for TSan to avoid false positives.
    if (IGM.IRGen.Opts.Sanitizers & SanitizerKind::Thread)
      load->setOrdering(llvm::AtomicOrdering::Acquire);
    else
      load->setOrdering(llvm::AtomicOrdering::Monotonic);

    // Compare the load result to see if it's negative.
    auto isUnfilledBB = subIGF.createBasicBlock("");
    auto contBB = subIGF.createBasicBlock("");
    llvm::Value *comparison = subIGF.Builder.CreateICmpSLT(load,
                                      llvm::ConstantInt::get(IGM.Int64Ty, 0));

    // Check if the 24th bit is set on Android AArch64 and only instantiate the
    // type metadata if it is, as otherwise it might be negative only because
    // of the memory tag on Android.
    if (IGM.Triple.isAndroid() &&
        IGM.Triple.getArch() == llvm::Triple::aarch64) {

      auto getBitAfterAndroidTag = subIGF.Builder.CreateAnd(
        load, llvm::ConstantInt::get(IGM.Int64Ty, 0x0080000000000000));
      auto checkNotAndroidTag = subIGF.Builder.CreateICmpNE(
        getBitAfterAndroidTag, llvm::ConstantInt::get(IGM.Int64Ty, 0));

      comparison = subIGF.Builder.CreateAnd(comparison, checkNotAndroidTag);
    }

    comparison = subIGF.Builder.CreateExpect(comparison,
                                       llvm::ConstantInt::get(IGM.Int1Ty, 0));
    subIGF.Builder.CreateCondBr(comparison, isUnfilledBB, contBB);
    auto loadBB = subIGF.Builder.GetInsertBlock();

    // If the load is negative, emit the call to instantiate the type
    // metadata.
    subIGF.Builder.SetInsertPoint(&subIGF.CurFn->back());
    subIGF.Builder.emitBlock(isUnfilledBB);

    // Break up the loaded value into size and relative address to the
    // string.
    auto size = subIGF.Builder.CreateAShr(load, 32);
    size = subIGF.Builder.CreateTruncOrBitCast(size, IGM.SizeTy);
    size = subIGF.Builder.CreateNeg(size);

    auto stringAddrOffset = subIGF.Builder.CreateTrunc(load,
                                                       IGM.Int32Ty);
    stringAddrOffset = subIGF.Builder.CreateSExtOrBitCast(stringAddrOffset,
                                                          IGM.SizeTy);
    auto stringAddrBase = subIGF.Builder.CreatePtrToInt(cache, IGM.SizeTy);
    if (IGM.getModule()->getDataLayout().isBigEndian()) {
      stringAddrBase = subIGF.Builder.CreateAdd(stringAddrBase,
                                      llvm::ConstantInt::get(IGM.SizeTy, 4));
    }
    auto stringAddr = subIGF.Builder.CreateAdd(stringAddrBase,
                                               stringAddrOffset);
    stringAddr = subIGF.Builder.CreateIntToPtr(stringAddr, IGM.Int8PtrTy);

    llvm::CallInst *call;
    if (request.isStaticallyAbstract()) {
      call = subIGF.Builder.CreateCall(
          IGM.getGetTypeByMangledNameInContextInMetadataStateFunctionPointer(),
          {llvm::ConstantInt::get(IGM.SizeTy, (size_t)MetadataState::Abstract),
           stringAddr, size,
           // TODO: Use mangled name lookup in generic
           // contexts?
           llvm::ConstantPointerNull::get(IGM.TypeContextDescriptorPtrTy),
           llvm::ConstantPointerNull::get(IGM.Int8PtrPtrTy)});
    } else {
      call = subIGF.Builder.CreateCall(
          IGM.getGetTypeByMangledNameInContextFunctionPointer(),
          {stringAddr, size,
           // TODO: Use mangled name lookup in generic
           // contexts?
           llvm::ConstantPointerNull::get(IGM.TypeContextDescriptorPtrTy),
           llvm::ConstantPointerNull::get(IGM.Int8PtrPtrTy)});
    }
    call->setDoesNotThrow();
    call->setOnlyReadsMemory();
    call->setCallingConv(IGM.SwiftCC);

    // Store the result back to the cache. Metadata instantiation should
    // already have emitted the necessary barriers to publish the instantiated
    // metadata to other threads, so we only need to expose the pointer.
    // Worst case, another thread might race with us and reinstantiate the
    // exact same metadata pointer.
    auto resultWord = subIGF.Builder.CreatePtrToInt(call, IGM.SizeTy);
    resultWord = subIGF.Builder.CreateZExtOrBitCast(resultWord, IGM.Int64Ty);
    auto store = subIGF.Builder.CreateStore(resultWord, cacheWordAddr,
                                            Alignment(8));
    store->setOrdering(llvm::AtomicOrdering::Monotonic);
    subIGF.Builder.CreateBr(contBB);

    subIGF.Builder.SetInsertPoint(loadBB);
    subIGF.Builder.emitBlock(contBB);
    auto phi = subIGF.Builder.CreatePHI(IGM.Int64Ty, 2);
    phi->addIncoming(load, loadBB);
    phi->addIncoming(resultWord, isUnfilledBB);

    auto resultAddr = subIGF.Builder.CreateTruncOrBitCast(phi, IGM.SizeTy);
    resultAddr = subIGF.Builder.CreateIntToPtr(resultAddr,
                                               IGM.TypeMetadataPtrTy);
    subIGF.Builder.CreateRet(resultAddr);
  };
  auto instantiationFn = cast<llvm::Function>(IGM.getOrCreateHelperFunction(
      instantiationFnName, IGF.IGM.TypeMetadataPtrTy, cache->getType(),
      generateInstantiationFn,
      /*noinline*/ true));

  auto call = IGF.Builder.CreateCall(instantiationFn->getFunctionType(),
                                     instantiationFn, cache);
  call->setDoesNotThrow();
  call->setOnlyReadsMemory();
  
  auto response = MetadataResponse::forComplete(call);
  
  IGF.setScopedLocalTypeMetadata(type, response);
  return response;
}

/// Emit a call to the type metadata accessor for the given function.
static MetadataResponse
emitCallToTypeMetadataAccessFunction(IRGenFunction &IGF, CanType type,
                                     DynamicMetadataRequest request) {
  // If we already cached the metadata, use it.
  if (auto local = IGF.tryGetLocalTypeMetadata(type, request))
    return local;

  // If the metadata would require multiple runtime calls to build, emit a
  // single access by mangled name instead, if we're asking for complete
  // metadata.
  //
  if ((request.isStaticallyBlockingComplete() ||
       (request.isStaticallyAbstract() &&
        canIssueIncompleteMetadataRequests(IGF.IGM))) &&
      shouldAccessByMangledName(IGF.IGM, type)) {
    return emitMetadataAccessByMangledName(IGF, type, request);
  }

  auto *accessor = getOrCreateTypeMetadataAccessFunction(IGF.IGM, type);
  llvm::CallInst *call = IGF.Builder.CreateCall(accessor->getFunctionType(),
                                                accessor, {request.get(IGF)});
  call->setCallingConv(IGF.IGM.SwiftCC);
  call->setDoesNotAccessMemory();
  call->setDoesNotThrow();

  auto response = MetadataResponse::handle(IGF, request, call);
  
  // Save the metadata for future lookups.
  IGF.setScopedLocalTypeMetadata(type, response);
  
  return response;
}

llvm::Value *IRGenFunction::emitAbstractTypeMetadataRef(CanType type) {
  return emitTypeMetadataRef(type, MetadataState::Abstract).getMetadata();
}

/// Produce the type metadata pointer for the given type.
llvm::Value *IRGenFunction::emitTypeMetadataRef(CanType type) {
  return emitTypeMetadataRef(type, MetadataState::Complete).getMetadata();
}

/// Produce the type metadata pointer for the given type.
MetadataResponse
IRGenFunction::emitTypeMetadataRef(CanType type,
                                   DynamicMetadataRequest request) {
  type = IGM.getRuntimeReifiedType(type);
  // Look through any opaque types we're allowed to.
  type = IGM.substOpaqueTypesWithUnderlyingTypes(type);

  // If we're asking for the metadata of the type that dynamic Self is known
  // to be equal to, we can just use the self metadata.
  if (SelfTypeIsExact && SelfType == type) {
    return MetadataResponse::forComplete(getDynamicSelfMetadata());
  }
  
  if (type->hasArchetype() ||
      !shouldTypeMetadataAccessUseAccessor(IGM, type) ||
      isa<PackType>(type)) {
    return emitDirectTypeMetadataRef(*this, type, request);
  }

  return emitCallToTypeMetadataAccessFunction(*this, type, request);
}

/// Return the address of a function that will return type metadata 
/// for the given non-dependent type.
llvm::Function *irgen::getOrCreateTypeMetadataAccessFunction(IRGenModule &IGM,
                                                             CanType type) {
  type = IGM.getRuntimeReifiedType(type);

  assert(!type->hasArchetype() &&
         "cannot create global function to return dependent type metadata");

  switch (getTypeMetadataAccessStrategy(type)) {
  case MetadataAccessStrategy::ForeignAccessor:
  case MetadataAccessStrategy::PublicUniqueAccessor:
  case MetadataAccessStrategy::HiddenUniqueAccessor:
  case MetadataAccessStrategy::PrivateAccessor:
    return getOtherwiseDefinedTypeMetadataAccessFunction(IGM, type);
  case MetadataAccessStrategy::NonUniqueAccessor:
    return createDirectTypeMetadataAccessFunction(IGM, type,
                                                  /*allow existing*/true);
  }
  llvm_unreachable("bad type metadata access strategy");
}

namespace {
  /// A visitor class for emitting a reference to type metatype for a
  /// SILType, i.e. a lowered representation type.  In general, the type
  /// metadata produced here might not correspond to the formal type that
  /// would belong to the unlowered type.  For correctness, it is important
  /// not to cache the result as if it were the metadata for a formal type
  /// unless the type actually cannot possibly be a formal type, e.g. because
  /// it is one of the special lowered type kinds like SILFunctionType.
  ///
  /// NOTE: If you modify the special cases in this, you should update
  /// isTypeMetadataForLayoutAccessible in SIL.cpp.
class EmitTypeMetadataRefForLayout
    : public CanTypeVisitor<EmitTypeMetadataRefForLayout, CanType> {
public:
  EmitTypeMetadataRefForLayout() {}

  /// For most types, we can just emit the usual metadata.
  CanType visitType(CanType t) { return t; }

  CanType visitBoundGenericEnumType(CanBoundGenericEnumType ty) {
    // Optionals have a lowered payload type, so we recurse here.
    if (auto objectTy = ty.getOptionalObjectType()) {
      auto payloadTy = visit(objectTy);
      if (payloadTy == objectTy)
        return ty;
      auto &C = ty->getASTContext();
      auto optDecl = C.getOptionalDecl();
      return CanType(BoundGenericEnumType::get(optDecl, Type(), payloadTy));
    }

    // Otherwise, generic arguments are not lowered.
    return ty;
  }

  CanType visitPackType(CanPackType ty) {
    llvm_unreachable("");
  }

  CanType visitPackExpansionType(CanPackExpansionType ty) {
    return ty;
  }

  CanType visitTupleType(CanTupleType ty) {
    bool changed = false;
    SmallVector<TupleTypeElt, 4> loweredElts;
    loweredElts.reserve(ty->getNumElements());

    for (auto i : indices(ty->getElementTypes())) {
      auto substEltType = ty.getElementType(i);

      CanType loweredSubstEltType = visit(substEltType);
      changed = (changed || substEltType != loweredSubstEltType);

      loweredElts.push_back(ty->getElement(i).getWithType(loweredSubstEltType));
    }

    if (!changed)
      return ty;

    return CanTupleType(TupleType::get(loweredElts, ty->getASTContext()));
  }

  CanType visitAnyFunctionType(CanAnyFunctionType ty) {
    llvm_unreachable("not a SIL type");
  }

  CanType visitSILFunctionType(CanSILFunctionType ty) {
    // All function types have the same layout regardless of arguments or
    // abstraction level. Use the metadata for () -> () for thick functions,
    // or AnyObject for block functions.
    auto &C = ty->getASTContext();
    switch (ty->getRepresentation()) {
    case SILFunctionType::Representation::Thin:
    case SILFunctionType::Representation::Method:
    case SILFunctionType::Representation::WitnessMethod:
    case SILFunctionType::Representation::ObjCMethod:
    case SILFunctionType::Representation::CXXMethod:
    case SILFunctionType::Representation::CFunctionPointer:
    case SILFunctionType::Representation::Closure:
      // A thin function looks like a plain pointer.
      // FIXME: Except for extra inhabitants?
      return C.TheRawPointerType;
    case SILFunctionType::Representation::Thick: {
      // All function types look like () -> ().
      // FIXME: It'd be nice not to have to call through the runtime here.
      //
      // FIXME: Verify ExtInfo state is correct, not working by accident.
      CanFunctionType::ExtInfo info;
      return CanFunctionType::get({}, C.TheEmptyTupleType, info);
    }
    case SILFunctionType::Representation::Block:
      // All block types look like AnyObject.
      return C.getAnyObjectType();
    }

    llvm_unreachable("Not a valid SILFunctionType.");
  }

  CanType visitAnyMetatypeType(CanAnyMetatypeType ty) {
    assert(ty->hasRepresentation() && "not a lowered metatype");
    auto &C = ty->getASTContext();
    switch (ty->getRepresentation()) {
    case MetatypeRepresentation::Thin:
      // Thin metatypes are empty, so they look like the empty tuple type.
      return C.TheEmptyTupleType;

    case MetatypeRepresentation::Thick:
    case MetatypeRepresentation::ObjC:
      // Thick and ObjC metatypes look like pointers with extra inhabitants.
      // Get the metatype metadata from the runtime.
      // FIXME: It'd be nice not to need a runtime call here; we should just
      // have a standard aligned-pointer type metadata.
      return ty;
    }

    llvm_unreachable("Not a valid MetatypeRepresentation.");
  }
};
} // end anonymous namespace

llvm::Value *IRGenFunction::emitTypeMetadataRefForLayout(SILType type) {
  return emitTypeMetadataRefForLayout(type, MetadataState::Complete);
}

llvm::Value *
IRGenFunction::emitTypeMetadataRefForLayout(SILType ty,
                                            DynamicMetadataRequest request) {
  assert(request.canResponseStatusBeIgnored());

  if (auto response =
          tryGetLocalTypeMetadataForLayout(ty.getObjectType(), request)) {
    assert(request.canResponseStatusBeIgnored() || !response.isValid());
    return response.getMetadata();
  }

  // Map to a layout equivalent AST type.
  auto layoutEquivalentType =
      EmitTypeMetadataRefForLayout().visit(ty.getASTType());
  auto response = emitTypeMetadataRef(layoutEquivalentType, request);
  setScopedLocalTypeMetadataForLayout(ty.getObjectType(), response);
  return response.getMetadata();
}

namespace {

  /// A visitor class for emitting a reference to a type layout struct.
  /// There are a few ways we can emit it:
  ///
  /// - If the type is fixed-layout and we have visibility of its value
  ///   witness table (or one close enough), we can project the layout struct
  ///   from it.
  /// - If the type is fixed layout, we can emit our own copy of the layout
  ///   struct.
  /// - If the type is dynamic-layout, we have to instantiate its metadata
  ///   and project out its metadata. (FIXME: This leads to deadlocks in
  ///   recursive cases, though we can avoid many deadlocks because most
  ///   valid recursive types bottom out in fixed-sized types like classes
  ///   or pointers.)
  class EmitTypeLayoutRef
    : public CanTypeVisitor<EmitTypeLayoutRef, llvm::Value *,
                            DynamicMetadataRequest> {
  private:
    IRGenFunction &IGF;
  public:
    EmitTypeLayoutRef(IRGenFunction &IGF) : IGF(IGF) {}

    llvm::Value *emitFromValueWitnessTablePointer(llvm::Value *vwtable) {
      llvm::Value *indexConstant = llvm::ConstantInt::get(IGF.IGM.Int32Ty,
                               (unsigned)ValueWitness::First_TypeLayoutWitness);
      return IGF.Builder.CreateInBoundsGEP(IGF.IGM.Int8PtrTy, vwtable,
                                           indexConstant);
    }

    /// Emit the type layout by projecting it from a value witness table to
    /// which we have linkage.
    llvm::Value *emitFromValueWitnessTable(CanType t) {
      auto *vwtable = IGF.IGM.getAddrOfValueWitnessTable(t);
      return emitFromValueWitnessTablePointer(vwtable);
    }

    /// Emit the type layout by projecting it from dynamic type metadata.
    llvm::Value *emitFromTypeMetadata(CanType t,
                                      DynamicMetadataRequest request) {
      auto *vwtable =
        IGF.emitValueWitnessTableRef(IGF.IGM.getLoweredType(t), request);
      return emitFromValueWitnessTablePointer(vwtable);
    }

    /// Given that the type is fixed-layout, emit the type layout by
    /// emitting a global layout for it.
    llvm::Value *emitFromFixedLayout(CanType t) {
      auto layout = tryEmitFromFixedLayout(t);
      assert(layout && "type must be fixed-size to call emitFromFixedLayout");
      return layout;
    }

    /// If the type is fixed-layout, emit the type layout by
    /// emitting a global layout for it.
    llvm::Value *tryEmitFromFixedLayout(CanType t) {
      auto &ti = IGF.getTypeInfo(SILType::getPrimitiveObjectType(t));
      if (auto fixedTI = dyn_cast<FixedTypeInfo>(&ti))
        return IGF.IGM.emitFixedTypeLayout(t, *fixedTI);
      return nullptr;
    }

    bool hasVisibleValueWitnessTable(CanType t) const {
      // Some builtin and structural types have value witnesses exported from
      // the runtime.
      auto &C = IGF.IGM.Context;
      if (t == C.TheEmptyTupleType
          || t == C.TheNativeObjectType
          || t == C.TheBridgeObjectType
          || t == C.TheRawPointerType
          || t == C.getAnyObjectType())
        return true;
      if (auto intTy = dyn_cast<BuiltinIntegerType>(t)) {
        auto width = intTy->getWidth();
        if (width.isPointerWidth())
          return true;
        if (width.isFixedWidth()) {
          switch (width.getFixedWidth()) {
          case 8:
          case 16:
          case 32:
          case 64:
          case 128:
          case 256:
            return true;
          default:
            return false;
          }
        }
        return false;
      }

      // TODO: If a nominal type is in the same source file as we're currently
      // emitting, we would be able to see its value witness table.
      return false;
    }

    /// Fallback default implementation.
    llvm::Value *visitType(CanType t, DynamicMetadataRequest request) {
      auto silTy = IGF.IGM.getLoweredType(t);
      auto &ti = IGF.getTypeInfo(silTy);

      // If the type is in the same source file, or has a common value
      // witness table exported from the runtime, we can project from the
      // value witness table instead of emitting a new record.
      if (hasVisibleValueWitnessTable(t))
        return emitFromValueWitnessTable(t);

      // If the type is a singleton aggregate, the field's layout is equivalent
      // to the aggregate's.
      if (SILType singletonFieldTy = getSingletonAggregateFieldType(IGF.IGM,
                                             silTy, ResilienceExpansion::Maximal))
        return visit(singletonFieldTy.getASTType(), request);

      // If the type is fixed-layout, emit a copy of its layout.
      if (auto fixed = dyn_cast<FixedTypeInfo>(&ti))
        return IGF.IGM.emitFixedTypeLayout(t, *fixed);

      return emitFromTypeMetadata(t, request);
    }
      
    llvm::Value *visitAnyFunctionType(CanAnyFunctionType type,
                                      DynamicMetadataRequest request) {
      llvm_unreachable("not a SIL type");
    }
      
    llvm::Value *visitSILFunctionType(CanSILFunctionType type,
                                      DynamicMetadataRequest request) {
      // All function types have the same layout regardless of arguments or
      // abstraction level. Use the value witness table for
      // @convention(blah) () -> () from the runtime.
      auto &C = type->getASTContext();
      switch (type->getRepresentation()) {
      case SILFunctionType::Representation::Thin:
      case SILFunctionType::Representation::Method:
      case SILFunctionType::Representation::WitnessMethod:
      case SILFunctionType::Representation::ObjCMethod:
      case SILFunctionType::Representation::CXXMethod:
      case SILFunctionType::Representation::CFunctionPointer:
      case SILFunctionType::Representation::Closure:
        // A thin function looks like a plain pointer.
        // FIXME: Except for extra inhabitants?
        return emitFromValueWitnessTable(C.TheRawPointerType);
      case SILFunctionType::Representation::Thick: {
        // All function types look like () -> ().
        // FIXME: Verify ExtInfo state is correct, not working by accident.
        CanFunctionType::ExtInfo info;
        return emitFromValueWitnessTable(
            CanFunctionType::get({}, C.TheEmptyTupleType, info));
      }
      case SILFunctionType::Representation::Block:
        // All block types look like AnyObject.
        return emitFromValueWitnessTable(C.getAnyObjectType());
      }

      llvm_unreachable("Not a valid SILFunctionType.");
    }

    llvm::Value *visitAnyMetatypeType(CanAnyMetatypeType type,
                                      DynamicMetadataRequest request) {
      
      assert(type->hasRepresentation()
             && "not a lowered metatype");

      switch (type->getRepresentation()) {
      case MetatypeRepresentation::Thin: {
        // Thin metatypes are empty, so they look like the empty tuple type.
        return emitFromValueWitnessTable(IGF.IGM.Context.TheEmptyTupleType);
      }
      case MetatypeRepresentation::Thick:
        if (isa<ExistentialMetatypeType>(type)) {
          return emitFromFixedLayout(type);
        }
        // Otherwise, this is a metatype that looks like a pointer.
        LLVM_FALLTHROUGH;
      case MetatypeRepresentation::ObjC:
        // Thick metatypes look like pointers with spare bits.
        return emitFromValueWitnessTable(
                     CanMetatypeType::get(IGF.IGM.Context.TheNativeObjectType));
      }

      llvm_unreachable("Not a valid MetatypeRepresentation.");
    }

    llvm::Value *visitAnyClassType(ClassDecl *classDecl,
                                   DynamicMetadataRequest request) {
      // All class types have the same layout.
      auto type = classDecl->getDeclaredType()->getCanonicalType();
      switch (type->getReferenceCounting()) {
      case ReferenceCounting::Native:
        return emitFromValueWitnessTable(IGF.IGM.Context.TheNativeObjectType);

      case ReferenceCounting::ObjC:
      case ReferenceCounting::Block:
      case ReferenceCounting::Unknown:
        return emitFromValueWitnessTable(IGF.IGM.Context.getAnyObjectType());

      case ReferenceCounting::Bridge:
      case ReferenceCounting::Error:
        llvm_unreachable("classes shouldn't have this kind of refcounting");
      case ReferenceCounting::None:
      case ReferenceCounting::Custom:
        llvm_unreachable(
            "Foreign reference types don't conform to 'AnyClass'.");
      }

      llvm_unreachable("Not a valid ReferenceCounting.");
    }

    llvm::Value *visitClassType(CanClassType type,
                                DynamicMetadataRequest request) {
      return visitAnyClassType(type->getClassOrBoundGenericClass(), request);
    }

    llvm::Value *visitBoundGenericClassType(CanBoundGenericClassType type,
                                            DynamicMetadataRequest request) {
      return visitAnyClassType(type->getClassOrBoundGenericClass(), request);
    }

    llvm::Value *visitPackType(CanPackType type,
                               DynamicMetadataRequest request) {
      llvm_unreachable("");
    }

    llvm::Value *visitPackExpansionType(CanPackExpansionType type,
                                        DynamicMetadataRequest request) {
      llvm_unreachable("");
    }

    llvm::Value *visitTupleType(CanTupleType type,
                                DynamicMetadataRequest request) {
      // Tuples containing pack expansion types are completely dynamic.
      if (type->containsPackExpansionType())
        return emitFromTypeMetadata(type, request);

      // Single-element tuples have exactly the same layout as their elements.
      if (type->getNumElements() == 1) {
        return visit(type.getElementType(0), request);
      }

      // If the type is fixed-layout, use a global layout.
      if (auto layout = tryEmitFromFixedLayout(type))
        return layout;

      // TODO: check for cached VWT / metadata for the type.

      // Use swift_getTupleTypeLayout to compute a layout.

      // Create a buffer to hold the result.  We don't have any reasonable
      // way to scope the lifetime of this.
      auto resultPtr = IGF.createAlloca(IGF.IGM.FullTypeLayoutTy,
                                        IGF.IGM.getPointerAlignment())
                          .getAddress();

      switch (type->getNumElements()) {
      case 0:
      case 1:
        llvm_unreachable("filtered out above");

      case 2: {
        auto elt0 = visit(type.getElementType(0), request);
        auto elt1 = visit(type.getElementType(1), request);

        // Ignore the offset.
        auto call =
            IGF.Builder.CreateCall(IGF.IGM.getGetTupleLayout2FunctionPointer(),
                                   {resultPtr, elt0, elt1});
        call->setDoesNotThrow();

        break;
      }

      case 3: {
        auto elt0 = visit(type.getElementType(0), request);
        auto elt1 = visit(type.getElementType(1), request);
        auto elt2 = visit(type.getElementType(2), request);

        // Ignore the offsets.
        auto call =
            IGF.Builder.CreateCall(IGF.IGM.getGetTupleLayout3FunctionPointer(),
                                   {resultPtr, elt0, elt1, elt2});
        call->setDoesNotThrow();

        break;
      }

      default: {
        // Allocate a temporary array for the element layouts.
        auto eltLayoutsArraySize =
          IGF.IGM.getPointerSize() * type->getNumElements();
        auto eltLayoutsArray =
          IGF.createAlloca(IGF.IGM.Int8PtrPtrTy,
                           IGF.IGM.getSize(Size(type->getNumElements())),
                           IGF.IGM.getPointerAlignment());
        IGF.Builder.CreateLifetimeStart(eltLayoutsArray, eltLayoutsArraySize);

        // Emit layouts for all the elements and store them into the array.
        for (auto i : indices(type.getElementTypes())) {
          auto eltLayout = visit(type.getElementType(i), request);
          auto eltLayoutSlot =
            i == 0 ? eltLayoutsArray
                   : IGF.Builder.CreateConstArrayGEP(eltLayoutsArray, i,
                                                     IGF.IGM.getPointerSize());
          IGF.Builder.CreateStore(eltLayout, eltLayoutSlot);
        }

        // Ignore the offsets.
        auto offsetsPtr =
          llvm::ConstantPointerNull::get(IGF.IGM.Int32Ty->getPointerTo());

        // Flags.
        auto flags = TupleTypeFlags().withNumElements(type->getNumElements());
        auto flagsValue = IGF.IGM.getSize(Size(flags.getIntValue()));

        // Compute the layout.
        auto call = IGF.Builder.CreateCall(
            IGF.IGM.getGetTupleLayoutFunctionPointer(),
            {resultPtr, offsetsPtr, flagsValue, eltLayoutsArray.getAddress()});
        call->setDoesNotThrow();

        // We're done with the buffer.
        IGF.Builder.CreateLifetimeEnd(eltLayoutsArray, eltLayoutsArraySize);

        break;
      }
      }

      // Cast resultPtr to i8**, our general currency type for type layouts.
      resultPtr = IGF.Builder.CreateBitCast(resultPtr, IGF.IGM.Int8PtrPtrTy);
      return resultPtr;
    }
  };

} // end anonymous namespace

llvm::Value *irgen::emitTypeLayoutRef(IRGenFunction &IGF, SILType type,
                                      MetadataDependencyCollector *collector) {
  auto request =
    DynamicMetadataRequest::getNonBlocking(MetadataState::LayoutComplete,
                                           collector);
  assert(request.canResponseStatusBeIgnored());
  return EmitTypeLayoutRef(IGF).visit(type.getASTType(), request);
}

/// Given a class metatype, produce the necessary heap metadata
/// reference.  This is generally the metatype pointer, but may
/// instead be a reference type.
llvm::Value *irgen::emitClassHeapMetadataRefForMetatype(IRGenFunction &IGF,
                                                        llvm::Value *metatype,
                                                        CanType type) {
  // If the type is known to have Swift metadata, this is trivial.
  if (hasKnownSwiftMetadata(IGF.IGM, type))
    return metatype;

  // Otherwise, we may have to unwrap an ObjC class wrapper.
  assert(IGF.IGM.Context.LangOpts.EnableObjCInterop);
  metatype = IGF.Builder.CreateBitCast(metatype, IGF.IGM.TypeMetadataPtrTy);
  
  // Fetch the metadata for that class.
  auto call = IGF.Builder.CreateCall(
      IGF.IGM.getGetObjCClassFromMetadataFunctionPointer(), metatype);
  call->setDoesNotThrow();
  call->setDoesNotAccessMemory();
  return call;
}

/// Produce the heap metadata pointer for the given class type.  For
/// Swift-defined types, this is equivalent to the metatype for the
/// class, but for Objective-C-defined types, this is the class
/// object.
llvm::Value *irgen::emitClassHeapMetadataRef(IRGenFunction &IGF, CanType type,
                                             MetadataValueType desiredType,
                                             DynamicMetadataRequest request,
                                             bool allowUninitialized) {
  assert(request.canResponseStatusBeIgnored() &&
         "emitClassHeapMetadataRef only supports satisfied requests");
  assert(type->mayHaveSuperclass() || type->isTypeErasedGenericClassType());

  // Archetypes may or may not be ObjC classes and need unwrapping to get at
  // the class object.
  if (auto archetype = dyn_cast<ArchetypeType>(type)) {
    // Look up the Swift metadata from context.
    auto archetypeMeta = IGF.emitTypeMetadataRef(type, request).getMetadata();
    // Get the class pointer.
    auto classPtr = emitClassHeapMetadataRefForMetatype(IGF, archetypeMeta,
                                                        archetype);
    if (desiredType == MetadataValueType::ObjCClass)
      classPtr = IGF.Builder.CreateBitCast(classPtr, IGF.IGM.ObjCClassPtrTy);
    return classPtr;
  }
  
  if (ClassDecl *theClass = dyn_cast_or_null<ClassDecl>(type->getAnyNominal())) {
    if (!hasKnownSwiftMetadata(IGF.IGM, theClass)) {
      llvm::Value *result =
        emitObjCHeapMetadataRef(IGF, theClass, allowUninitialized);
      if (desiredType == MetadataValueType::TypeMetadata)
        result = IGF.Builder.CreateBitCast(result, IGF.IGM.TypeMetadataPtrTy);
      return result;
    }
  }

  llvm::Value *result = IGF.emitTypeMetadataRef(type, request).getMetadata();
  if (desiredType == MetadataValueType::ObjCClass)
    result = IGF.Builder.CreateBitCast(result, IGF.IGM.ObjCClassPtrTy);
  return result;
}

/// Emit a metatype value for a known type.
void irgen::emitMetatypeRef(IRGenFunction &IGF, CanMetatypeType type,
                            Explosion &explosion) {
  switch (type->getRepresentation()) {
  case MetatypeRepresentation::Thin:
    // Thin types have a trivial representation.
    break;

  case MetatypeRepresentation::Thick:
    explosion.add(IGF.emitTypeMetadataRef(type.getInstanceType()));
    break;

  case MetatypeRepresentation::ObjC:
    explosion.add(emitClassHeapMetadataRef(IGF, type.getInstanceType(),
                                           MetadataValueType::ObjCClass,
                                           MetadataState::Complete));
    break;
  }
}

static bool canCheckStateWithBranch(DynamicMetadataRequest request,
                                    MetadataResponse response) {
  assert(request.getDependencyCollector() == nullptr ||
         (request.isStatic() && request.getStaticRequest().isNonBlocking()));

  return (response.hasDynamicState() &&
          request.getDependencyCollector() != nullptr);
}

MetadataResponse
irgen::emitCheckTypeMetadataState(IRGenFunction &IGF,
                                  DynamicMetadataRequest request,
                                  MetadataResponse response) {
  // Note that the structure of this function is mirrored in
  // getCheckTypeMetadataStateCost.

  // If the request is already satisfied by the response, we don't need
  // to check anything.
  if (request.isSatisfiedBy(response))
    return response;

  auto metadata = response.getMetadata();

  // Try to check the already-fetched dynamic state against the required state.
  if (canCheckStateWithBranch(request, response)) {
    auto dynamicState = response.getDynamicState();
    request.getDependencyCollector()
          ->checkDependency(IGF, request, metadata, dynamicState);

    return MetadataResponse(metadata, dynamicState,
                            request.getStaticRequest().getState());
  }

  // Otherwise, we have to ask the runtime.
  return emitGetTypeMetadataDynamicState(IGF, request, metadata);
}

OperationCost
irgen::getCheckTypeMetadataStateCost(DynamicMetadataRequest request,
                                     MetadataResponse response) {
  if (request.isSatisfiedBy(response))
    return OperationCost::Free;

  if (canCheckStateWithBranch(request, response))
    return OperationCost::Arithmetic;

  return OperationCost::Call;
}

/// Call swift_checkMetadataState.
MetadataResponse
irgen::emitGetTypeMetadataDynamicState(IRGenFunction &IGF,
                                       DynamicMetadataRequest request,
                                       llvm::Value *metadata) {
  auto call =
      IGF.Builder.CreateCall(IGF.IGM.getCheckMetadataStateFunctionPointer(),
                             {request.get(IGF), metadata});
  call->setCallingConv(IGF.IGM.SwiftCC);

  return MetadataResponse::handle(IGF, request, call);
}
