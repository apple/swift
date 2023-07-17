//===--- GenArchetype.cpp - Swift IR Generation for Archetype Types -------===//
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
//  This file implements IR generation for archetype types in Swift.
//
//===----------------------------------------------------------------------===//

#include "GenArchetype.h"

#include "swift/AST/ASTContext.h"
#include "swift/AST/Decl.h"
#include "swift/AST/GenericEnvironment.h"
#include "swift/AST/IRGenOptions.h"
#include "swift/AST/Types.h"
#include "swift/IRGen/Linking.h"
#include "swift/SIL/SILValue.h"
#include "swift/SIL/TypeLowering.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/IR/Constant.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Module.h"
#include "llvm/Support/raw_ostream.h"

#include "EnumPayload.h"
#include "Explosion.h"
#include "FixedTypeInfo.h"
#include "GenClass.h"
#include "GenHeap.h"
#include "GenMeta.h"
#include "GenOpaque.h"
#include "GenPointerAuth.h"
#include "GenPoly.h"
#include "GenProto.h"
#include "GenType.h"
#include "HeapTypeInfo.h"
#include "IRGenDebugInfo.h"
#include "IRGenFunction.h"
#include "IRGenModule.h"
#include "MetadataRequest.h"
#include "Outlining.h"
#include "ProtocolInfo.h"
#include "ResilientTypeInfo.h"
#include "TypeInfo.h"
#include "TypeLayout.h"

using namespace swift;
using namespace irgen;

MetadataResponse
irgen::emitArchetypeTypeMetadataRef(IRGenFunction &IGF,
                                    CanArchetypeType archetype,
                                    DynamicMetadataRequest request) {
  assert(!isa<PackArchetypeType>(archetype));

  // Check for an existing cache entry.
  if (auto response = IGF.tryGetLocalTypeMetadata(archetype, request))
    return response;
  
  // If this is an opaque archetype, we'll need to instantiate using its
  // descriptor.
  if (auto opaque = dyn_cast<OpaqueTypeArchetypeType>(archetype)) {
    if (opaque->isRoot())
      return emitOpaqueTypeMetadataRef(IGF, opaque, request);
  }

  // If there's no local or opaque metadata, it must be a nested type.
  assert(archetype->getParent() && "Not a nested archetype");

  CanArchetypeType parent(archetype->getParent());
  AssociatedType association(
      archetype->getInterfaceType()->castTo<DependentMemberType>()
        ->getAssocType());

  MetadataResponse response =
    emitAssociatedTypeMetadataRef(IGF, parent, association, request);

  setTypeMetadataName(IGF.IGM, response.getMetadata(), archetype);

  IGF.setScopedLocalTypeMetadata(archetype, response);

  return response;
}

namespace {

/// A type implementation for an ArchetypeType, otherwise known as a
/// type variable: for example, Self in a protocol declaration, or T
/// in a generic declaration like foo<T>(x : T) -> T.  The critical
/// thing here is that performing an operation involving archetypes
/// is dependent on the witness binding we can see.
class OpaqueArchetypeTypeInfo
  : public ResilientTypeInfo<OpaqueArchetypeTypeInfo>
{
  OpaqueArchetypeTypeInfo(llvm::Type *type, IsABIAccessible_t abiAccessible)
      : ResilientTypeInfo(type, IsCopyable, abiAccessible) {}

public:
  static const OpaqueArchetypeTypeInfo *
  create(llvm::Type *type, IsABIAccessible_t abiAccessible) {
    return new OpaqueArchetypeTypeInfo(type, abiAccessible);
  }

  void collectMetadataForOutlining(OutliningMetadataCollector &collector,
                                   SILType T) const override {
    // We'll need formal type metadata for this archetype.
    collector.collectTypeMetadataForLayout(T);
  }

  TypeLayoutEntry
  *buildTypeLayoutEntry(IRGenModule &IGM,
                        SILType T,
                        bool useStructLayouts) const override {
    return IGM.typeLayoutCache.getOrCreateArchetypeEntry(T.getObjectType());
  }
};

/// A type implementation for a class archetype, that is, an archetype
/// bounded by a class protocol constraint. These archetypes can be
/// represented by a refcounted pointer instead of an opaque value buffer.
/// If ObjC interop is disabled, we can use Swift refcounting entry
/// points, otherwise we have to use the unknown ones.
class ClassArchetypeTypeInfo
  : public HeapTypeInfo<ClassArchetypeTypeInfo>
{
  ReferenceCounting RefCount;

  ClassArchetypeTypeInfo(llvm::PointerType *storageType,
                         Size size, const SpareBitVector &spareBits,
                         Alignment align,
                         ReferenceCounting refCount)
    : HeapTypeInfo(refCount, storageType, size, spareBits, align),
      RefCount(refCount)
  {}

public:
  static const ClassArchetypeTypeInfo *create(llvm::PointerType *storageType,
                                         Size size, const SpareBitVector &spareBits,
                                         Alignment align,
                                         ReferenceCounting refCount) {
    return new ClassArchetypeTypeInfo(storageType, size, spareBits, align,
                                      refCount);
  }

  ReferenceCounting getReferenceCounting() const {
    return RefCount;
  }
};

class FixedSizeArchetypeTypeInfo
  : public PODSingleScalarTypeInfo<FixedSizeArchetypeTypeInfo, LoadableTypeInfo>
{
  FixedSizeArchetypeTypeInfo(llvm::Type *type, Size size, Alignment align,
                             const SpareBitVector &spareBits)
      : PODSingleScalarTypeInfo(type, size, spareBits, align) {}

public:
  static const FixedSizeArchetypeTypeInfo *
  create(llvm::Type *type, Size size, Alignment align,
         const SpareBitVector &spareBits) {
    return new FixedSizeArchetypeTypeInfo(type, size, align, spareBits);
  }
};

} // end anonymous namespace

/// Emit a single protocol witness table reference.
llvm::Value *irgen::emitArchetypeWitnessTableRef(IRGenFunction &IGF,
                                                 CanArchetypeType archetype,
                                                 ProtocolDecl *protocol) {
  assert(Lowering::TypeConverter::protocolRequiresWitnessTable(protocol) &&
         "looking up witness table for protocol that doesn't have one");

  // The following approach assumes that a protocol will only appear in
  // an archetype's conformsTo array if the archetype is either explicitly
  // constrained to conform to that protocol (in which case we should have
  // a cache entry for it) or there's an associated type declaration with
  // that protocol listed as a direct requirement.

  auto localDataKind =
    LocalTypeDataKind::forAbstractProtocolWitnessTable(protocol);

  // Check immediately for an existing cache entry.
  // TODO: don't give this absolute precedence over other access paths.
  auto wtable = IGF.tryGetLocalTypeData(archetype, localDataKind);
  if (wtable) return wtable;

  auto environment = archetype->getGenericEnvironment();

  // Otherwise, ask the generic signature for the best path to the conformance.
  // TODO: this isn't necessarily optimal if the direct conformance isn't
  // concretely available; we really ought to be comparing the full paths
  // to this conformance from concrete sources.

  auto signature = environment->getGenericSignature().getCanonicalSignature();
  auto archetypeDepType = archetype->getInterfaceType();

  auto astPath = signature->getConformancePath(archetypeDepType, protocol);

  auto i = astPath.begin(), e = astPath.end();
  assert(i != e && "empty path!");

  // The first entry in the path is a direct requirement of the signature,
  // for which we should always have local type data available.
  CanType rootArchetype =
    environment->mapTypeIntoContext(i->first)->getCanonicalType();
  ProtocolDecl *rootProtocol = i->second;

  // Turn the rest of the path into a MetadataPath.
  auto lastProtocol = rootProtocol;
  MetadataPath path;
  while (++i != e) {
    auto &entry = *i;
    CanType depType = CanType(entry.first);
    ProtocolDecl *requirement = entry.second;

    const ProtocolInfo &lastPI =
        IGF.IGM.getProtocolInfo(lastProtocol,
                                ProtocolInfoKind::RequirementSignature);

    // If it's a type parameter, it's self, and this is a base protocol
    // requirement.
    if (isa<GenericTypeParamType>(depType)) {
      assert(depType->isEqual(lastProtocol->getSelfInterfaceType()));
      WitnessIndex index = lastPI.getBaseIndex(requirement);
      path.addInheritedProtocolComponent(index);

    // Otherwise, it's an associated conformance requirement.
    } else {
      AssociatedConformance association(lastProtocol, depType, requirement);
      WitnessIndex index = lastPI.getAssociatedConformanceIndex(association);
      path.addAssociatedConformanceComponent(index);
    }

    lastProtocol = requirement;
  }
  assert(lastProtocol == protocol);

  auto rootWTable = IGF.tryGetLocalTypeData(rootArchetype,
              LocalTypeDataKind::forAbstractProtocolWitnessTable(rootProtocol));
  // Fetch an opaque type's witness table if it wasn't cached yet.
  if (!rootWTable) {
    if (auto opaqueRoot = dyn_cast<OpaqueTypeArchetypeType>(rootArchetype)) {
      rootWTable = emitOpaqueTypeWitnessTableRef(IGF, opaqueRoot,
                                                 rootProtocol);
    }
    assert(rootWTable && "root witness table not bound in local context!");
  }

  wtable = path.followFromWitnessTable(IGF, rootArchetype,
                                       ProtocolConformanceRef(rootProtocol),
                                       MetadataResponse::forComplete(rootWTable),
                                       /*request*/ MetadataState::Complete,
                                       nullptr).getMetadata();

  IGF.setScopedLocalTypeData(archetype, localDataKind, wtable);
  return wtable;
}

MetadataResponse
irgen::emitAssociatedTypeMetadataRef(IRGenFunction &IGF,
                                     CanArchetypeType origin,
                                     AssociatedType association,
                                     DynamicMetadataRequest request) {
  // Find the conformance of the origin to the associated type's protocol.
  llvm::Value *wtable = emitArchetypeWitnessTableRef(IGF, origin,
                                               association.getSourceProtocol());

  // Find the origin's type metadata.
  llvm::Value *originMetadata =
    emitArchetypeTypeMetadataRef(IGF, origin, MetadataState::Abstract)
      .getMetadata();

  return emitAssociatedTypeMetadataRef(IGF, originMetadata, wtable,
                                       association, request);
}

const TypeInfo *TypeConverter::convertArchetypeType(ArchetypeType *archetype) {
  assert(isExemplarArchetype(archetype) && "lowering non-exemplary archetype");

  auto layout = archetype->getLayoutConstraint();

  // If the archetype is class-constrained, use a class pointer
  // representation.
  if (layout && layout->isRefCounted()) {
    auto refcount = archetype->getReferenceCounting();

    llvm::PointerType *reprTy;

    // If the archetype has a superclass constraint, it has at least the
    // retain semantics of its superclass, and it can be represented with
    // the supertype's pointer type.
    if (auto super = archetype->getSuperclass()) {
      auto &superTI = IGM.getTypeInfoForUnlowered(super);
      reprTy = cast<llvm::PointerType>(superTI.StorageType);
    } else {
      if (refcount == ReferenceCounting::Native) {
        reprTy = IGM.RefCountedPtrTy;
      } else {
        reprTy = IGM.UnknownRefCountedPtrTy;
      }
    }

    // As a hack, assume class archetypes never have spare bits. There's a
    // corresponding hack in MultiPayloadEnumImplStrategy::completeEnumTypeLayout
    // to ignore spare bits of dependent-typed payloads.
    auto spareBits =
      SpareBitVector::getConstant(IGM.getPointerSize().getValueInBits(), false);

    return ClassArchetypeTypeInfo::create(reprTy,
                                      IGM.getPointerSize(),
                                      spareBits,
                                      IGM.getPointerAlignment(),
                                      refcount);
  }

  // If the archetype is trivial fixed-size layout-constrained, use a fixed size
  // representation.
  if (layout && layout->isFixedSizeTrivial()) {
    Size size(layout->getTrivialSizeInBytes());
    auto layoutAlignment = layout->getAlignmentInBytes();
    assert(layoutAlignment && "layout constraint alignment should not be 0");
    Alignment align(layoutAlignment);
    auto spareBits =
      SpareBitVector::getConstant(size.getValueInBits(), false);
    // Get an integer type of the required size.
    auto ProperlySizedIntTy = SILType::getBuiltinIntegerType(
        size.getValueInBits(), IGM.getSwiftModule()->getASTContext());
    auto storageType = IGM.getStorageType(ProperlySizedIntTy);
    return FixedSizeArchetypeTypeInfo::create(storageType, size, align,
                                              spareBits);
  }

  // If the archetype is a trivial layout-constrained, use a POD
  // representation. This type is not loadable, but it is known
  // to be a POD.
  if (layout && layout->isAddressOnlyTrivial()) {
    // TODO: Create NonFixedSizeArchetypeTypeInfo and return it.
  }

  // Otherwise, for now, always use an opaque indirect type.
  llvm::Type *storageType = IGM.OpaqueTy;

  // Opaque result types can be private and from a different module. In this
  // case we can't access their type metadata from another module.
  IsABIAccessible_t abiAccessible = IsABIAccessible;
  if (auto opaqueArchetype = dyn_cast<OpaqueTypeArchetypeType>(archetype)) {
    auto &currentSILModule = IGM.getSILModule();
    abiAccessible =
        currentSILModule.isTypeMetadataAccessible(archetype->getCanonicalType())
            ? IsABIAccessible
            : IsNotABIAccessible;
  }
  return OpaqueArchetypeTypeInfo::create(storageType, abiAccessible);
}

llvm::Value *irgen::emitDynamicTypeOfOpaqueArchetype(IRGenFunction &IGF,
                                                     Address addr,
                                                     SILType type) {
  auto archetype = type.castTo<ArchetypeType>();
  // Acquire the archetype's static metadata.
  llvm::Value *metadata =
    emitArchetypeTypeMetadataRef(IGF, archetype, MetadataState::Complete)
      .getMetadata();
  return IGF.Builder.CreateCall(
      IGF.IGM.getGetDynamicTypeFunctionPointer(),
      {addr.getAddress(), metadata, llvm::ConstantInt::get(IGF.IGM.Int1Ty, 0)});
}

static void
withOpaqueTypeGenericArgs(IRGenFunction &IGF,
                          CanOpaqueTypeArchetypeType archetype,
                          llvm::function_ref<void (llvm::Value*)> body) {
  // Collect the generic arguments of the opaque decl.
  auto opaqueDecl = archetype->getDecl();
  auto generics = opaqueDecl->getGenericSignatureOfContext();
  llvm::Value *genericArgs;
  Address alloca;
  Size allocaSize(0);
  if (!generics || generics->areAllParamsConcrete()) {
    genericArgs = llvm::UndefValue::get(IGF.IGM.Int8PtrTy);
  } else {
    SmallVector<llvm::Value *, 4> args;
    SmallVector<llvm::Type *, 4> types;

    enumerateGenericSignatureRequirements(
        opaqueDecl->getGenericSignature().getCanonicalSignature(),
        [&](GenericRequirement reqt) {
          auto arg = emitGenericRequirementFromSubstitutions(
              IGF, reqt, MetadataState::Abstract,
              archetype->getSubstitutions());
          args.push_back(arg);
          types.push_back(args.back()->getType());
        });
    auto bufTy = llvm::StructType::get(IGF.IGM.getLLVMContext(), types);
    alloca = IGF.createAlloca(bufTy, IGF.IGM.getPointerAlignment());
    allocaSize = IGF.IGM.getPointerSize() * args.size();
    IGF.Builder.CreateLifetimeStart(alloca, allocaSize);
    for (auto i : indices(args)) {
      IGF.Builder.CreateStore(args[i],
          IGF.Builder.CreateStructGEP(alloca, i, i * IGF.IGM.getPointerSize()));
    }
    genericArgs = IGF.Builder.CreateBitCast(alloca.getAddress(),
                                            IGF.IGM.Int8PtrTy);
  }
  
  // Pass them down to the body.
  body(genericArgs);

  // End the alloca's lifetime, if we allocated one.
  if (alloca.getAddress()) {
    IGF.Builder.CreateLifetimeEnd(alloca, allocaSize);
  }
}

bool shouldUseOpaqueTypeDescriptorAccessor(OpaqueTypeDecl *opaque) {
  auto *namingDecl = opaque->getNamingDecl();

  // Don't emit accessors for abstract storage that is not dynamic or a dynamic
  // replacement.
  if (auto *abstractStorage = dyn_cast<AbstractStorageDecl>(namingDecl)) {
    return abstractStorage->hasAnyNativeDynamicAccessors() ||
           abstractStorage->getDynamicallyReplacedDecl();
  }

  // Don't emit accessors for functions that are not dynamic or dynamic
  // replacements.
  return namingDecl->shouldUseNativeDynamicDispatch() ||
         (bool)namingDecl->getDynamicallyReplacedDecl();
}

static llvm::Value *
getAddressOfOpaqueTypeDescriptor(IRGenFunction &IGF,
                                 OpaqueTypeDecl *opaqueDecl) {
  auto &IGM = IGF.IGM;

  // Support dynamically replacing the return type as part of dynamic function
  // replacement.
  if (!IGM.getOptions().shouldOptimize() &&
      shouldUseOpaqueTypeDescriptorAccessor(opaqueDecl)) {
    auto descriptorAccessor = IGM.getAddrOfOpaqueTypeDescriptorAccessFunction(
        opaqueDecl, NotForDefinition, false);
    auto desc = IGF.Builder.CreateCall(descriptorAccessor, {});
    desc->setDoesNotThrow();
    desc->setCallingConv(IGM.SwiftCC);
    return desc;
  }
  return IGM.getAddrOfOpaqueTypeDescriptor(opaqueDecl, ConstantInit());
}

MetadataResponse irgen::emitOpaqueTypeMetadataRef(IRGenFunction &IGF,
                                          CanOpaqueTypeArchetypeType archetype,
                                          DynamicMetadataRequest request) {
  bool signedDescriptor = IGF.IGM.getAvailabilityContext().isContainedIn(
    IGF.IGM.Context.getSignedDescriptorAvailability());

  auto accessorFn = signedDescriptor ?
    IGF.IGM.getGetOpaqueTypeMetadata2FunctionPointer() :
    IGF.IGM.getGetOpaqueTypeMetadataFunctionPointer();

  auto opaqueDecl = archetype->getDecl();
  auto genericParam = archetype->getInterfaceType()
      ->castTo<GenericTypeParamType>();
  auto *descriptor = getAddressOfOpaqueTypeDescriptor(IGF, opaqueDecl);
  auto indexValue = llvm::ConstantInt::get(
      IGF.IGM.SizeTy, genericParam->getIndex());

  // Sign the descriptor.
  auto schema =
    IGF.IGM.getOptions().PointerAuth.OpaqueTypeDescriptorsAsArguments;
  if (schema && signedDescriptor) {
    auto authInfo = PointerAuthInfo::emit(
        IGF, schema, nullptr,
        PointerAuthEntity::Special::OpaqueTypeDescriptorAsArgument);
    descriptor = emitPointerAuthSign(IGF, descriptor, authInfo);
  }

  llvm::CallInst *result = nullptr;
  withOpaqueTypeGenericArgs(IGF, archetype,
    [&](llvm::Value *genericArgs) {
      result = IGF.Builder.CreateCall(accessorFn,
                       {request.get(IGF), genericArgs, descriptor, indexValue});
      result->setDoesNotThrow();
      result->setCallingConv(IGF.IGM.SwiftCC);
      result->addFnAttr(llvm::Attribute::ReadOnly);
    });
  assert(result);
  
  auto response = MetadataResponse::handle(IGF, request, result);
  IGF.setScopedLocalTypeMetadata(archetype, response);
  return response;
}

llvm::Value *irgen::emitOpaqueTypeWitnessTableRef(IRGenFunction &IGF,
                                          CanOpaqueTypeArchetypeType archetype,
                                          ProtocolDecl *protocol) {
  bool signedDescriptor = IGF.IGM.getAvailabilityContext().isContainedIn(
    IGF.IGM.Context.getSignedDescriptorAvailability());

  auto accessorFn = signedDescriptor ?
    IGF.IGM.getGetOpaqueTypeConformance2FunctionPointer() :
    IGF.IGM.getGetOpaqueTypeConformanceFunctionPointer();
  auto opaqueDecl = archetype->getDecl();
  assert(archetype->isRoot() && "Can only follow from the root");

  llvm::Value *descriptor = getAddressOfOpaqueTypeDescriptor(IGF, opaqueDecl);

  // Sign the descriptor.
  auto schema =
    IGF.IGM.getOptions().PointerAuth.OpaqueTypeDescriptorsAsArguments;
  if (schema && signedDescriptor) {
    auto authInfo = PointerAuthInfo::emit(
        IGF, schema, nullptr,
        PointerAuthEntity::Special::OpaqueTypeDescriptorAsArgument);
    descriptor = emitPointerAuthSign(IGF, descriptor, authInfo);
  }

  // Compute the index at which this witness table resides.
  unsigned index = opaqueDecl->getOpaqueGenericParams().size();
  auto opaqueReqs =
      opaqueDecl->getOpaqueInterfaceGenericSignature().getRequirements();
  bool found = false;
  for (const auto &req : opaqueReqs) {
    auto reqProto = opaqueTypeRequiresWitnessTable(opaqueDecl, req);
    if (!reqProto)
      continue;

    // Is this requirement the one we're looking for?
    if (reqProto == protocol &&
        req.getFirstType()->isEqual(archetype->getInterfaceType())) {
      found = true;
      break;
    }

    ++index;
  }

  (void)found;
  assert(found && "Opaque type does not conform to protocol");
  auto indexValue = llvm::ConstantInt::get(IGF.IGM.SizeTy, index);
  
  llvm::CallInst *result = nullptr;
  withOpaqueTypeGenericArgs(IGF, archetype,
    [&](llvm::Value *genericArgs) {
      result = IGF.Builder.CreateCall(accessorFn,
                                   {genericArgs, descriptor, indexValue});
      result->setDoesNotThrow();
      result->setCallingConv(IGF.IGM.SwiftCC);
      result->addFnAttr(llvm::Attribute::ReadOnly);
    });
  assert(result);
  
  IGF.setScopedLocalTypeData(archetype,
                 LocalTypeDataKind::forAbstractProtocolWitnessTable(protocol),
                 result);
  return result;
}
