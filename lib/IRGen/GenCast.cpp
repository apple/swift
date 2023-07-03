//===--- GenCast.cpp - Swift IR Generation for dynamic casts --------------===//
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
//  This file implements IR generation for dynamic casts.
//
//===----------------------------------------------------------------------===//

#include "GenCast.h"

#include "Explosion.h"
#include "GenEnum.h"
#include "GenExistential.h"
#include "GenHeap.h"
#include "GenProto.h"
#include "IRGenDebugInfo.h"
#include "IRGenFunction.h"
#include "IRGenModule.h"
#include "MetadataRequest.h"
#include "TypeInfo.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/Module.h"

#include "swift/AST/ExistentialLayout.h"
#include "swift/AST/IRGenOptions.h"
#include "swift/SIL/DynamicCasts.h"
#include "swift/SIL/SILInstruction.h"
#include "swift/SIL/SILModule.h"
#include "swift/SIL/TypeLowering.h"
#include "swift/ABI/MetadataValues.h"

using namespace swift;
using namespace irgen;

/// Compute the flags to pass to swift_dynamicCast.
static DynamicCastFlags getDynamicCastFlags(CastConsumptionKind consumptionKind,
                                            CheckedCastMode mode) {
  DynamicCastFlags flags = DynamicCastFlags::Default;

  if (mode == CheckedCastMode::Unconditional)
    flags |= DynamicCastFlags::Unconditional;
  if (shouldDestroyOnFailure(consumptionKind))
    flags |= DynamicCastFlags::DestroyOnFailure;
  if (shouldTakeOnSuccess(consumptionKind))
    flags |= DynamicCastFlags::TakeOnSuccess;

  return flags;
}

/// Emit a checked cast, starting with a value in memory.
llvm::Value *irgen::emitCheckedCast(IRGenFunction &IGF,
                                    Address src,
                                    CanType srcType,
                                    Address dest,
                                    CanType targetType,
                                    CastConsumptionKind consumptionKind,
                                    CheckedCastMode mode) {
  // TODO: attempt to specialize this based on the known types.

  DynamicCastFlags flags = getDynamicCastFlags(consumptionKind, mode);

  // Cast both addresses to opaque pointer type.
  dest = IGF.Builder.CreateElementBitCast(dest, IGF.IGM.OpaqueTy);
  src = IGF.Builder.CreateElementBitCast(src, IGF.IGM.OpaqueTy);

  // Load type metadata for the source's static type and the target type.
  llvm::Value *srcMetadata = IGF.emitTypeMetadataRef(srcType);
  llvm::Value *targetMetadata = IGF.emitTypeMetadataRef(targetType);

  llvm::Value *args[] = {
    dest.getAddress(), src.getAddress(),
    srcMetadata, targetMetadata,
    IGF.IGM.getSize(Size(unsigned(flags)))
  };

  auto call =
      IGF.Builder.CreateCall(IGF.IGM.getDynamicCastFunctionPointer(), args);
  call->setDoesNotThrow();

  return call;
}

FailableCastResult irgen::emitClassIdenticalCast(IRGenFunction &IGF,
                                                 llvm::Value *from,
                                                 SILType fromType,
                                                 SILType toType,
                                                 GenericSignature fnSig) {
  // Check metatype objects directly. Don't try to find their meta-metatype.
  auto isMetatype = false;
  if (auto metaType = toType.getAs<MetatypeType>()) {
    isMetatype = true;
    assert(metaType->getRepresentation() != MetatypeRepresentation::ObjC &&
           "not implemented");
    toType = IGF.IGM.getLoweredType(metaType.getInstanceType());
  }
  // Emit a reference to the heap metadata for the target type.

  // If we're allowed to do a conservative check, try to just use the
  // global class symbol.  If the class has been re-allocated, this
  // might not be the heap metadata actually in use, and hence the
  // test might fail; but it's a much faster check.
  // TODO: use ObjC class references
  llvm::Value *targetMetadata;
  if ((targetMetadata =
           tryEmitConstantHeapMetadataRef(IGF.IGM, toType.getASTType(),
                                          /*allowUninitialized*/ false))) {
    // ok
  } else {
    targetMetadata
      = emitClassHeapMetadataRef(IGF, toType.getASTType(),
                                 MetadataValueType::ObjCClass,
                                 MetadataState::Complete,
                                 /*allowUninitialized*/ false);
  }

  // Handle checking a metatype object's type by directly comparing the address
  // of the metatype value to the subclass's static metatype instance.
  //
  // %1 = value_metatype $Super.Type, %0 : $A
  // checked_cast_br [exact] %1 : $Super.Type to $Sub.Type
  // =>
  // icmp eq %1, @metadata.Sub
  llvm::Value *objectMetadata = isMetatype ? from :
    emitHeapMetadataRefForHeapObject(IGF, from, fromType, fnSig);

  objectMetadata = IGF.Builder.CreateBitCast(objectMetadata,
                                             targetMetadata->getType());
  llvm::Value *cond = IGF.Builder.CreateICmpEQ(objectMetadata, targetMetadata);
  return {cond, from};
}

/// Returns an ArrayRef with the set of arguments to pass to a dynamic cast call.
///
/// `argsBuf` should be passed in as a reference to an array with three nullptr
/// values at the end. These will be dropped from the return ArrayRef for a
/// conditional cast, or filled in with source location arguments for an
/// unconditional cast.
template<unsigned n>
static ArrayRef<llvm::Value*>
getDynamicCastArguments(IRGenFunction &IGF,
                        llvm::Value *(&argsBuf)[n], CheckedCastMode mode
                        /*TODO , SILLocation location*/)
{
  switch (mode) {
  case CheckedCastMode::Unconditional:
    // TODO: Pass along location info if available for unconditional casts, so
    // that the runtime error for a failed cast can report the source of the
    // error from user code.
    argsBuf[n-3] = llvm::ConstantPointerNull::get(IGF.IGM.Int8PtrTy);
    argsBuf[n-2] = llvm::ConstantInt::get(IGF.IGM.Int32Ty, 0);
    argsBuf[n-1] = llvm::ConstantInt::get(IGF.IGM.Int32Ty, 0);
    return argsBuf;
      
  case CheckedCastMode::Conditional:
    return llvm::makeArrayRef(argsBuf, n-3);
    break;
  }
  llvm_unreachable("covered switch");
}

/// Emit a checked unconditional downcast of a class value.
llvm::Value *irgen::emitClassDowncast(IRGenFunction &IGF, llvm::Value *from,
                                      CanType toType, CheckedCastMode mode) {
  // Emit the value we're casting from.
  if (from->getType() != IGF.IGM.Int8PtrTy)
    from = IGF.Builder.CreateBitOrPointerCast(from, IGF.IGM.Int8PtrTy);

  // Emit a reference to the metadata and figure out what cast
  // function to use.
  llvm::Value *metadataRef;
  FunctionPointer castFn;

  // If true, the target class is not known at compile time because it is a
  // class-bounded archetype or the dynamic Self type.
  bool nonSpecificClass = false;

  // Get the best known type information about the destination type.
  ClassDecl *destClass = nullptr;
  if (auto archetypeTy = dyn_cast<ArchetypeType>(toType)) {
    nonSpecificClass = true;
    if (auto superclassTy = archetypeTy->getSuperclass())
      destClass = superclassTy->getClassOrBoundGenericClass();
  } else if (auto selfTy = dyn_cast<DynamicSelfType>(toType)) {
    nonSpecificClass = true;
    destClass = selfTy->getSelfType()->getClassOrBoundGenericClass();
  } else {
    destClass = toType.getClassOrBoundGenericClass();
    assert(destClass != nullptr);
  }

  // If the destination type is known to have a Swift-compatible
  // implementation, use the most specific entrypoint.
  if (destClass && destClass->hasKnownSwiftImplementation()) {
    metadataRef = IGF.emitTypeMetadataRef(toType);

    switch (mode) {
    case CheckedCastMode::Unconditional:
      castFn = IGF.IGM.getDynamicCastClassUnconditionalFunctionPointer();
      break;
    case CheckedCastMode::Conditional:
      castFn = IGF.IGM.getDynamicCastClassFunctionPointer();
      break;
    }

  // If the destination type is a CF type or a non-specific
  // class-bounded archetype, use the most general cast entrypoint.
  } else if (nonSpecificClass ||
             destClass->getForeignClassKind()==ClassDecl::ForeignKind::CFType) {
    metadataRef = IGF.emitTypeMetadataRef(toType);

    switch (mode) {
    case CheckedCastMode::Unconditional:
      castFn = IGF.IGM.getDynamicCastUnknownClassUnconditionalFunctionPointer();
      break;
    case CheckedCastMode::Conditional:
      castFn = IGF.IGM.getDynamicCastUnknownClassFunctionPointer();
      break;
    }

  // Otherwise, use the ObjC-specific entrypoint.
  } else {
    metadataRef = emitObjCHeapMetadataRef(IGF, destClass);

    switch (mode) {
    case CheckedCastMode::Unconditional:
      castFn = IGF.IGM.getDynamicCastObjCClassUnconditionalFunctionPointer();
      break;
    case CheckedCastMode::Conditional:
      castFn = IGF.IGM.getDynamicCastObjCClassFunctionPointer();
      break;
    }
  }

  if (metadataRef->getType() != IGF.IGM.Int8PtrTy)
    metadataRef = IGF.Builder.CreateBitCast(metadataRef, IGF.IGM.Int8PtrTy);

  // Call the (unconditional) dynamic cast.
  llvm::Value *argsBuf[] = {
    from,
    metadataRef,
    nullptr,
    nullptr,
    nullptr,
  };

  auto call = IGF.Builder.CreateCall(
      castFn, getDynamicCastArguments(IGF, argsBuf, mode));
  call->setDoesNotThrow();

  llvm::Type *subTy = IGF.getTypeInfoForUnlowered(toType).getStorageType();
  return IGF.Builder.CreateBitCast(call, subTy);
}

/// Emit a checked cast of a metatype.
void irgen::emitMetatypeDowncast(IRGenFunction &IGF,
                                 llvm::Value *metatype,
                                 CanMetatypeType toMetatype,
                                 CheckedCastMode mode,
                                 Explosion &ex) {
  // Pick a runtime entry point and target metadata based on what kind of
  // representation we're casting.
  FunctionPointer castFn;
  llvm::Value *toMetadata;

  switch (toMetatype->getRepresentation()) {
  case MetatypeRepresentation::Thick: {
    // Get the Swift metadata for the type we're checking.
    toMetadata = IGF.emitTypeMetadataRef(toMetatype.getInstanceType());
    switch (mode) {
    case CheckedCastMode::Unconditional:
      castFn = IGF.IGM.getDynamicCastMetatypeUnconditionalFunctionPointer();
      break;
    case CheckedCastMode::Conditional:
      castFn = IGF.IGM.getDynamicCastMetatypeFunctionPointer();
      break;
    }
    break;
  }

  case MetatypeRepresentation::ObjC: {
    assert(IGF.IGM.ObjCInterop && "should have objc runtime");

    // Get the ObjC metadata for the type we're checking.
    toMetadata = emitClassHeapMetadataRef(IGF, toMetatype.getInstanceType(),
                                          MetadataValueType::ObjCClass,
                                          MetadataState::Complete);
    switch (mode) {
    case CheckedCastMode::Unconditional:
      castFn =
          IGF.IGM.getDynamicCastObjCClassMetatypeUnconditionalFunctionPointer();
      break;
    case CheckedCastMode::Conditional:
      castFn = IGF.IGM.getDynamicCastObjCClassMetatypeFunctionPointer();
      break;
    }
    break;
  }

  case MetatypeRepresentation::Thin:
    llvm_unreachable("not implemented");
  }

  llvm::Value *argsBuf[] = {
    metatype,
    toMetadata,
    nullptr,
    nullptr,
    nullptr,
  };

  auto call = IGF.Builder.CreateCall(
      castFn, getDynamicCastArguments(IGF, argsBuf, mode));
  call->setDoesNotThrow();
  ex.add(call);
}

/// Emit a Protocol* value referencing an ObjC protocol.
llvm::Value *irgen::emitReferenceToObjCProtocol(IRGenFunction &IGF,
                                                ProtocolDecl *proto) {
  assert(proto->isObjC() && "not an objc protocol");

  // Get the address of the global variable the protocol reference gets
  // indirected through.
  auto protocolRefAddr =
      IGF.IGM.getAddrOfObjCProtocolRef(proto, NotForDefinition);
  return IGF.Builder.CreateLoad(protocolRefAddr);
}

/// Emit a helper function to look up \c numProtocols witness tables given
/// a value and a type metadata reference.
///
/// If \p checkClassConstraint is true, we must emit an explicit check that the
/// instance is a class.
///
/// If \p checkSuperclassConstraint is true, we are given an additional parameter
/// with a superclass type in it, and must emit a check that the instance is a
/// subclass of the given class.
///
/// The function's input type is (value, metadataValue, superclass?, protocol...)
/// The function's output type is (value, witnessTable...)
///
/// The value is NULL if the cast failed.
static FunctionPointer
emitExistentialScalarCastFn(IRGenModule &IGM, unsigned numProtocols,
                            CheckedCastMode mode, bool checkClassConstraint,
                            bool checkSuperclassConstraint) {
  assert(!checkSuperclassConstraint || checkClassConstraint);

  // Build the function name.
  llvm::SmallString<32> name;
  {
    llvm::raw_svector_ostream os(name);
    os << "dynamic_cast_existential_";
    os << numProtocols;
    if (checkSuperclassConstraint)
      os << "_superclass";
    else if (checkClassConstraint)
      os << "_class";
    switch (mode) {
    case CheckedCastMode::Unconditional:
      os << "_unconditional";
      break;
    case CheckedCastMode::Conditional:
      os << "_conditional";
      break;
    }
  }
  
  // Build the function type.
  llvm::SmallVector<llvm::Type *, 4> argTys;
  llvm::SmallVector<llvm::Type *, 4> returnTys;
  argTys.push_back(IGM.Int8PtrTy);
  argTys.push_back(IGM.TypeMetadataPtrTy);
  returnTys.push_back(IGM.Int8PtrTy);
  if (checkSuperclassConstraint)
    argTys.push_back(IGM.TypeMetadataPtrTy);
  for (unsigned i = 0; i < numProtocols; ++i) {
    argTys.push_back(IGM.ProtocolDescriptorPtrTy);
    returnTys.push_back(IGM.WitnessTablePtrTy);
  }
  
  llvm::Type *returnTy = llvm::StructType::get(IGM.getLLVMContext(), returnTys);

  auto fn = IGM.getOrCreateHelperFunction(
      name, returnTy, argTys, [&](IRGenFunction &IGF) {
        Explosion args = IGF.collectParameters();

        auto value = args.claimNext();
        auto ref = args.claimNext();
        auto failBB = IGF.createBasicBlock("fail");
        auto conformsToProtocol = IGM.getConformsToProtocolFunctionPointer();

        Explosion rets;
        rets.add(value);

        // Check the class constraint if necessary.
        if (checkSuperclassConstraint) {
          auto superclassMetadata = args.claimNext();
          auto castFn = IGF.IGM.getDynamicCastMetatypeFunctionPointer();
          auto castResult =
              IGF.Builder.CreateCall(castFn, {ref, superclassMetadata});

          // FIXME: Eventually, we may want to throw.
          castResult->setDoesNotThrow();

          auto isClass = IGF.Builder.CreateICmpNE(
              castResult,
              llvm::ConstantPointerNull::get(IGF.IGM.TypeMetadataPtrTy));

          auto contBB = IGF.createBasicBlock("cont");
          IGF.Builder.CreateCondBr(isClass, contBB, failBB);
          IGF.Builder.emitBlock(contBB);
        } else if (checkClassConstraint) {
          auto isClass =
              IGF.Builder.CreateCall(IGM.getIsClassTypeFunctionPointer(), ref);
          auto contBB = IGF.createBasicBlock("cont");
          IGF.Builder.CreateCondBr(isClass, contBB, failBB);
          IGF.Builder.emitBlock(contBB);
        }

        // Look up each protocol conformance we want.
        for (unsigned i = 0; i < numProtocols; ++i) {
          auto proto = args.claimNext();
          auto witness =
              IGF.Builder.CreateCall(conformsToProtocol, {ref, proto});
          auto isNull = IGF.Builder.CreateICmpEQ(
              witness, llvm::ConstantPointerNull::get(IGM.WitnessTablePtrTy));
          auto contBB = IGF.createBasicBlock("cont");
          IGF.Builder.CreateCondBr(isNull, failBB, contBB);

          IGF.Builder.emitBlock(contBB);
          rets.add(witness);
        }

        // If we succeeded, return the witnesses.
        IGF.emitScalarReturn(returnTy, rets);

        // If we failed, return nil or trap.
        IGF.Builder.emitBlock(failBB);
        switch (mode) {
        case CheckedCastMode::Conditional: {
          auto null = llvm::ConstantStruct::getNullValue(returnTy);
          IGF.Builder.CreateRet(null);
          break;
        }

        case CheckedCastMode::Unconditional: {
          IGF.emitTrap("type cast failed", /*EmitUnreachable=*/true);
          break;
        }
        }
      });
  auto fnType = llvm::FunctionType::get(returnTy, argTys, false);
  auto sig = Signature(fnType, {}, IGM.DefaultCC);
  return FunctionPointer::forDirect(FunctionPointer::Kind::Function, fn,
                                    nullptr, sig);
}

llvm::Value *irgen::emitMetatypeToAnyObjectDowncast(IRGenFunction &IGF,
                                                    llvm::Value *metatypeValue,
                                                    CanAnyMetatypeType type,
                                                    CheckedCastMode mode) {
  // If ObjC interop is enabled, casting a metatype to AnyObject succeeds
  // if the metatype is for a class.
  if (!IGF.IGM.ObjCInterop)
    return nullptr;
  
  switch (type->getRepresentation()) {
  case MetatypeRepresentation::ObjC:
    // Metatypes that can be represented as ObjC trivially cast to AnyObject.
    return IGF.Builder.CreateBitCast(metatypeValue, IGF.IGM.ObjCPtrTy);

  case MetatypeRepresentation::Thin:
    // Metatypes that can be thin would never be classes.
    // TODO: Final class metatypes could in principle be thin.
    assert(!type.getInstanceType()->mayHaveSuperclass()
           && "classes should not have thin metatypes (yet)");
    return nullptr;
    
  case MetatypeRepresentation::Thick: {
    auto instanceTy = type.getInstanceType();
    // Is the type obviously a class?
    if (instanceTy->mayHaveSuperclass()) {
      // Get the ObjC metadata for the class.
      auto heapMetadata = emitClassHeapMetadataRefForMetatype(IGF,metatypeValue,
                                                              instanceTy);
      return IGF.Builder.CreateBitCast(heapMetadata, IGF.IGM.ObjCPtrTy);
    }
    
    // If it's not a class, we can't handle it here
    if (!isa<ArchetypeType>(instanceTy) && !isa<ExistentialMetatypeType>(type)) {
      return nullptr;
    }

    // Ask the runtime whether this is class metadata.
    FunctionPointer castFn;
    switch (mode) {
    case CheckedCastMode::Conditional:
      castFn =
          IGF.IGM.getDynamicCastMetatypeToObjectConditionalFunctionPointer();
      break;
    case CheckedCastMode::Unconditional:
      castFn =
          IGF.IGM.getDynamicCastMetatypeToObjectUnconditionalFunctionPointer();
      break;
    }

    llvm::Value *argsBuf[] = {
      metatypeValue,
      nullptr,
      nullptr,
      nullptr,
    };

    auto call = IGF.Builder.CreateCall(
        castFn, getDynamicCastArguments(IGF, argsBuf, mode));
    return call;
  }
  }
  llvm_unreachable("invalid metatype representation");
}


/// Emit a checked cast to a protocol or protocol composition.
void irgen::emitScalarExistentialDowncast(
    IRGenFunction &IGF, llvm::Value *value, SILType srcType, SILType destType,
    CheckedCastMode mode, llvm::Optional<MetatypeRepresentation> metatypeKind,
    GenericSignature fnSig, Explosion &ex) {
  auto srcInstanceType = srcType.getASTType();
  auto destInstanceType = destType.getASTType();
  while (auto metatypeType = dyn_cast<ExistentialMetatypeType>(
           destInstanceType)) {
    destInstanceType = metatypeType.getInstanceType();
    srcInstanceType = cast<AnyMetatypeType>(srcInstanceType).getInstanceType();
  }

  auto layout = destInstanceType.getExistentialLayout();

  // Look up witness tables for the protocols that need them and get
  // references to the ObjC Protocol* values for the objc protocols.
  SmallVector<llvm::Value*, 4> objcProtos;
  SmallVector<llvm::Value*, 4> witnessTableProtos;

  bool hasClassConstraint = layout.requiresClass();
  bool hasClassConstraintByProtocol = false;

  bool hasSuperclassConstraint = bool(layout.explicitSuperclass);

  for (auto protoDecl : layout.getProtocols()) {
    // If the protocol introduces a class constraint, track whether we need
    // to check for it independent of protocol witnesses.
    if (protoDecl->requiresClass()) {
      assert(hasClassConstraint);
      hasClassConstraintByProtocol = true;
    }

    if (Lowering::TypeConverter::protocolRequiresWitnessTable(protoDecl)) {
      auto descriptor = IGF.IGM.getAddrOfProtocolDescriptor(protoDecl);
      witnessTableProtos.push_back(descriptor);
    }

    if (protoDecl->isObjC())
      objcProtos.push_back(emitReferenceToObjCProtocol(IGF, protoDecl));
  }
  
  llvm::Type *resultType;
  if (metatypeKind) {
    switch (*metatypeKind) {
    case MetatypeRepresentation::Thin:
      llvm_unreachable("can't cast to thin metatype");
    case MetatypeRepresentation::Thick:
      resultType = IGF.IGM.TypeMetadataPtrTy;
      break;
    case MetatypeRepresentation::ObjC:
      resultType = IGF.IGM.ObjCClassPtrTy;
      break;
    }
  } else {
    auto schema = IGF.getTypeInfo(destType).getSchema();
    resultType = schema[0].getScalarType();
  }

  // The source of a scalar cast is statically known to be a class or a
  // metatype, so we only have to check the class constraint in two cases:
  //
  // 1) The destination type has a superclass constraint that is
  //    more derived than what the source type is known to be.
  //
  // 2) We are casting between metatypes, in which case the source might
  //    be a non-class metatype.
  bool checkClassConstraint = false;
  if ((bool)metatypeKind &&
      hasClassConstraint &&
      !hasClassConstraintByProtocol &&
      !srcInstanceType->mayHaveSuperclass())
    checkClassConstraint = true;

  // If the source has an equal or more derived superclass constraint than
  // the destination, we can elide the superclass check.
  //
  // Note that destInstanceType is always an existential type, so calling
  // getSuperclass() returns the superclass constraint of the existential,
  // not the superclass of some concrete class.
  bool checkSuperclassConstraint = false;
  if (hasSuperclassConstraint) {
    Type srcSuperclassType = srcInstanceType;
    if (srcSuperclassType->isExistentialType()) {
      srcSuperclassType = srcSuperclassType->getSuperclass();
      // Look for an AnyObject superclass (getSuperclass() returns nil).
      if (!srcSuperclassType && srcInstanceType->isClassExistentialType())
        checkSuperclassConstraint = true;
    }
    if (srcSuperclassType) {
      checkSuperclassConstraint =
        !destInstanceType->getSuperclass()->isExactSuperclassOf(
          srcSuperclassType);
    }
  }

  if (checkSuperclassConstraint)
    checkClassConstraint = true;

  llvm::Value *resultValue = value;

  // If we don't have anything we really need to check, then trivially succeed.
  if (objcProtos.empty() && witnessTableProtos.empty() &&
      !checkClassConstraint) {
    resultValue = IGF.Builder.CreateBitCast(value, resultType);
    ex.add(resultValue);
    return;
  }

  // Check the ObjC protocol conformances if there were any.
  llvm::Value *objcCast = nullptr;
  if (!objcProtos.empty()) {
    // Get the ObjC instance or class object to check for these conformances.
    llvm::Value *objcObject;
    if (metatypeKind) {
      switch (*metatypeKind) {
      case MetatypeRepresentation::Thin:
        llvm_unreachable("can't cast to thin metatype");
      case MetatypeRepresentation::Thick: {
        // The metadata might be for a non-class type, which wouldn't have
        // an ObjC class object.
        objcObject = nullptr;
        break;
      }
      case MetatypeRepresentation::ObjC:
        // Metatype is already an ObjC object.
        objcObject = value;
        break;
      }
    } else {
      // Class instance is already an ObjC object.
      objcObject = value;
    }
    if (objcObject)
      objcObject = IGF.Builder.CreateBitCast(objcObject,
                                             IGF.IGM.UnknownRefCountedPtrTy);
    
    // Pick the cast function based on the cast mode and on whether we're
    // casting a Swift metatype or ObjC object.
    FunctionPointer castFn;
    switch (mode) {
    case CheckedCastMode::Unconditional:
      castFn =
          objcObject
              ? IGF.IGM.getDynamicCastObjCProtocolUnconditionalFunctionPointer()
              : IGF.IGM
                    .getDynamicCastTypeToObjCProtocolUnconditionalFunctionPointer();
      break;
    case CheckedCastMode::Conditional:
      castFn =
          objcObject
              ? IGF.IGM.getDynamicCastObjCProtocolConditionalFunctionPointer()
              : IGF.IGM
                    .getDynamicCastTypeToObjCProtocolConditionalFunctionPointer();
      break;
    }
    llvm::Value *objcCastObject = objcObject ? objcObject : value;
    
    Address protoRefsBuf = IGF.createAlloca(
                                        llvm::ArrayType::get(IGF.IGM.Int8PtrTy,
                                                             objcProtos.size()),
                                        IGF.IGM.getPointerAlignment(),
                                        "objc_protocols");
    protoRefsBuf =
        IGF.Builder.CreateElementBitCast(protoRefsBuf, IGF.IGM.Int8PtrTy);

    for (unsigned index : indices(objcProtos)) {
      Address protoRefSlot = IGF.Builder.CreateConstArrayGEP(
                                                     protoRefsBuf, index,
                                                     IGF.IGM.getPointerSize());
      IGF.Builder.CreateStore(objcProtos[index], protoRefSlot);
      ++index;
    }

    llvm::Value *argsBuf[] = {
      objcCastObject,
      IGF.IGM.getSize(Size(objcProtos.size())),
      protoRefsBuf.getAddress(),
      nullptr,
      nullptr,
      nullptr,
    };

    auto call = IGF.Builder.CreateCall(
        castFn, getDynamicCastArguments(IGF, argsBuf, mode));
    objcCast = call;
    resultValue = IGF.Builder.CreateBitCast(objcCast, resultType);
  }

  // If we don't need to look up any witness tables, we're done.
  if (witnessTableProtos.empty() && !checkClassConstraint) {
    ex.add(resultValue);
    return;
  }

  // If we're doing a conditional cast, and the ObjC protocol checks failed,
  // then the cast is done.
  llvm::Optional<ConditionalDominanceScope> condition;
  llvm::BasicBlock *origBB = nullptr, *successBB = nullptr, *contBB = nullptr;
  if (!objcProtos.empty()) {
    switch (mode) {
    case CheckedCastMode::Unconditional:
      break;
    case CheckedCastMode::Conditional: {
      origBB = IGF.Builder.GetInsertBlock();
      successBB = IGF.createBasicBlock("success");
      contBB = IGF.createBasicBlock("cont");
      auto isNull = IGF.Builder.CreateICmpEQ(objcCast,
                               llvm::ConstantPointerNull::get(
                                 cast<llvm::PointerType>(objcCast->getType())));
      IGF.Builder.CreateCondBr(isNull, contBB, successBB);
      IGF.Builder.emitBlock(successBB);
      condition.emplace(IGF);
    }
    }
  }

  // Get the Swift type metadata for the type.
  llvm::Value *metadataValue;
  if (metatypeKind) {
    switch (*metatypeKind) {
    case MetatypeRepresentation::Thin:
      llvm_unreachable("can't cast to thin metatype");
    case MetatypeRepresentation::Thick:
      // The value is already a native metatype.
      metadataValue = value;
      break;
    case MetatypeRepresentation::ObjC:
      // Get the type metadata from the ObjC class, which may be a wrapper.
      metadataValue = emitObjCMetadataRefForMetadata(IGF, value);
    }
  } else {
    // Get the type metadata for the instance.
    metadataValue = emitDynamicTypeOfHeapObject(IGF, value,
                                                MetatypeRepresentation::Thick,
                                                srcType,
                                                fnSig);
  }

  // Look up witness tables for the protocols that need them.
  auto fn = emitExistentialScalarCastFn(IGF.IGM,
                                        witnessTableProtos.size(),
                                        mode,
                                        checkClassConstraint,
                                        checkSuperclassConstraint);

  llvm::SmallVector<llvm::Value *, 4> args;

  if (resultValue->getType() != IGF.IGM.Int8PtrTy)
    resultValue = IGF.Builder.CreateBitCast(resultValue, IGF.IGM.Int8PtrTy);
  args.push_back(resultValue);

  args.push_back(metadataValue);

  if (checkSuperclassConstraint)
    args.push_back(IGF.emitTypeMetadataRef(CanType(layout.explicitSuperclass)));

  for (auto proto : witnessTableProtos)
    args.push_back(proto);

  auto valueAndWitnessTables = IGF.Builder.CreateCall(fn, args);

  resultValue = IGF.Builder.CreateExtractValue(valueAndWitnessTables, 0);
  if (resultValue->getType() != resultType)
    resultValue = IGF.Builder.CreateBitCast(resultValue, resultType);
  ex.add(resultValue);

  for (unsigned i = 0, e = witnessTableProtos.size(); i < e; ++i) {
    auto wt = IGF.Builder.CreateExtractValue(valueAndWitnessTables, i + 1);
    ex.add(wt);
  }

  // If we had conditional ObjC checks, join the failure paths.
  if (contBB) {
    condition.reset();
    IGF.Builder.CreateBr(contBB);
    IGF.Builder.emitBlock(contBB);
    
    // Return null on the failure path.
    Explosion successEx = std::move(ex);
    ex.reset();
    
    while (!successEx.empty()) {
      auto successVal = successEx.claimNext();
      auto failureVal = llvm::Constant::getNullValue(successVal->getType());
      auto phi = IGF.Builder.CreatePHI(successVal->getType(), 2);
      phi->addIncoming(successVal, successBB);
      phi->addIncoming(failureVal, origBB);
      ex.add(phi);
    }
  }
}

/// Emit a checked cast of a scalar value.
///
/// This is not just an implementation of emitCheckedCast for scalar types;
/// it imposes strict restrictions on the source and target types that ensure
/// that the actual value isn't changed in any way, thus preserving its
/// reference identity.
///
/// These restrictions are set by \c canSILUseScalarCheckedCastInstructions.
/// Essentially, both the source and target types must be one of:
///   - a (possibly generic) concrete class type,
///   - a class-bounded archetype,
///   - a class-bounded existential,
///   - a concrete metatype, or
///   - an existential metatype.
///
/// Furthermore, if the target type is a metatype, the source type must be
/// a metatype.  This restriction isn't obviously necessary; it's just that
/// the runtime support for checking that an object instance is a metatype
/// isn't exposed.
void irgen::emitScalarCheckedCast(IRGenFunction &IGF,
                                  Explosion &value,
                                  SILType sourceLoweredType,
                                  CanType sourceFormalType,
                                  SILType targetLoweredType,
                                  CanType targetFormalType,
                                  CheckedCastMode mode,
                                  GenericSignature fnSig,
                                  Explosion &out) {
  assert(sourceLoweredType.isObject());
  assert(targetLoweredType.isObject());

  llvm::BasicBlock *nilCheckBB = nullptr;
  llvm::BasicBlock *nilMergeBB = nullptr;

  // Merge the nil check and return the merged result: either nil or the value.
  auto returnNilCheckedResult = [&](IRBuilder &Builder,
                                    Explosion &nonNilResult) {
    if (nilCheckBB) {
      auto notNilBB = Builder.GetInsertBlock();
      Builder.CreateBr(nilMergeBB);

      Builder.emitBlock(nilMergeBB);
      // Insert result phi.
      Explosion result;
      while (!nonNilResult.empty()) {
        auto val = nonNilResult.claimNext();
        auto valTy = cast<llvm::PointerType>(val->getType());
        auto nil = llvm::ConstantPointerNull::get(valTy);
        auto phi = Builder.CreatePHI(valTy, 2);
        phi->addIncoming(nil, nilCheckBB);
        phi->addIncoming(val, notNilBB);
        result.add(phi);
      }
      out = std::move(result);
    } else {
      out = std::move(nonNilResult);
    }
  };

  bool sourceWrappedInOptional = false;

  if (auto sourceOptObjectType = sourceLoweredType.getOptionalObjectType()) {
    // Translate the value from an enum representation to a possibly-null
    // representation.  Note that we assume that this projection is safe
    // for the particular case of an optional class-reference or metatype
    // value.
    Explosion optValue;
    auto someDecl = IGF.IGM.Context.getOptionalSomeDecl();
    emitProjectLoadableEnum(IGF, sourceLoweredType, value, someDecl, optValue);

    assert(value.empty());
    value = std::move(optValue);
    sourceLoweredType = sourceOptObjectType;
    sourceFormalType = sourceFormalType.getOptionalObjectType();
    sourceWrappedInOptional = true;

    // We need a null-check because the runtime function can't handle null in
    // some of the cases.
    if (targetLoweredType.isExistentialType()) {
      auto &Builder = IGF.Builder;
      auto val = value.getAll()[0];
      auto isNotNil = Builder.CreateICmpNE(
          val, llvm::ConstantPointerNull::get(
                   cast<llvm::PointerType>(val->getType())));
      auto *isNotNilContBB = llvm::BasicBlock::Create(IGF.IGM.getLLVMContext());
      nilMergeBB = llvm::BasicBlock::Create(IGF.IGM.getLLVMContext());
      nilCheckBB = Builder.GetInsertBlock();
      Builder.CreateCondBr(isNotNil, isNotNilContBB, nilMergeBB);

      Builder.emitBlock(isNotNilContBB);
    }
  }

  // If the source value is a metatype, either do a metatype-to-metatype
  // cast or cast it to an object instance and continue.
  if (auto sourceMetatype = sourceLoweredType.getAs<AnyMetatypeType>()) {
    llvm::Value *metatypeVal = nullptr;
    if (sourceMetatype->getRepresentation() != MetatypeRepresentation::Thin)
      metatypeVal = value.claimNext();

    // If the metatype is existential, there may be witness tables in the
    // value, which we don't need.
    // TODO: In existential-to-existential casts, we should carry over common
    // witness tables from the source to the destination.
    (void)value.claimAll();

    SmallVector<ProtocolDecl*, 1> protocols;

    // Casts to existential metatypes.
    if (auto existential = targetLoweredType.getAs<ExistentialMetatypeType>()) {
      emitScalarExistentialDowncast(IGF, metatypeVal, sourceLoweredType,
                                    targetLoweredType, mode,
                                    existential->getRepresentation(),
                                    fnSig,
                                    out);
      return;

    // Casts to concrete metatypes.
    } else if (auto destMetaType = targetLoweredType.getAs<MetatypeType>()) {
      emitMetatypeDowncast(IGF, metatypeVal, destMetaType, mode, out);
      return;
    }

    // Otherwise, this is a metatype-to-object cast.
    assert(targetLoweredType.isAnyClassReferenceType());

    // Can we convert the metatype value to AnyObject using Obj-C machinery?
    llvm::Value *object =
      emitMetatypeToAnyObjectDowncast(IGF, metatypeVal, sourceMetatype, mode);

    if (object == nullptr) {
      // Obj-C cast routine failed, use swift_dynamicCast instead

      if (sourceMetatype->getRepresentation() == MetatypeRepresentation::Thin
          || metatypeVal == nullptr) {
        // Earlier stages *should* never generate a checked cast with a thin metatype argument.
        // TODO: Move this assertion up to apply to all checked cast operations.
        // In assert builds, enforce this by failing here:
        assert(false && "Invalid SIL: General checked_cast_br cannot have thin argument");
        // In non-assert builds, stay compatible with previous behavior by emitting a null load.
        object = llvm::ConstantPointerNull::get(IGF.IGM.ObjCPtrTy);
      } else {
        Address src = IGF.createAlloca(metatypeVal->getType(),
                                       IGF.IGM.getPointerAlignment(),
                                       "castSrc");
        IGF.Builder.CreateStore(metatypeVal, src);
        llvm::PointerType *destPtrType = IGF.IGM.getStoragePointerType(targetLoweredType);
        Address dest = IGF.createAlloca(destPtrType,
                                        IGF.IGM.getPointerAlignment(),
                                        "castDest");
        IGF.Builder.CreateStore(llvm::ConstantPointerNull::get(destPtrType), dest);
        llvm::Value *success = emitCheckedCast(IGF,
                                               src, sourceFormalType,
                                               dest, targetFormalType,
                                               CastConsumptionKind::TakeAlways,
                                               mode);
        llvm::Value *successResult = IGF.Builder.CreateLoad(dest);
        llvm::Value *failureResult = llvm::ConstantPointerNull::get(destPtrType);
        llvm::Value *result = IGF.Builder.CreateSelect(success, successResult, failureResult);
        object = std::move(result);
      }
    }

    sourceFormalType = IGF.IGM.Context.getAnyObjectType();
    sourceLoweredType = SILType::getPrimitiveObjectType(sourceFormalType);

    // Continue, pretending that the source value was an (optional) value.
    Explosion newValue;
    newValue.add(object);
    value = std::move(newValue);
  }

  assert(!targetLoweredType.is<AnyMetatypeType>() &&
         "scalar cast of class reference to metatype is unimplemented");

  // If the source type is existential, project out the class pointer.
  //
  // TODO: if we're casting to an existential type, don't throw away the
  // protocol conformance information we already have.
  llvm::Value *instance;
  if (sourceLoweredType.isExistentialType()) {
    instance = emitClassExistentialProjection(IGF, value, sourceLoweredType,
                                              CanArchetypeType(), fnSig);
  } else {
    instance = value.claimNext();
  }

  if (targetFormalType.isExistentialType()) {
    Explosion outRes;
    emitScalarExistentialDowncast(IGF, instance, sourceLoweredType,
                                  targetLoweredType, mode,
                                  /*not a metatype*/ llvm::None, fnSig, outRes);
    returnNilCheckedResult(IGF.Builder, outRes);
    return;
  }

  if (llvm::Value *fastResult = emitFastClassCastIfPossible(
          IGF, instance, sourceFormalType, targetFormalType,
          sourceWrappedInOptional, nilCheckBB, nilMergeBB)) {
    Explosion fastExplosion;
    fastExplosion.add(fastResult);
    returnNilCheckedResult(IGF.Builder, fastExplosion);
    return;
  }

  Explosion outRes;
  llvm::Value *result = emitClassDowncast(IGF, instance, targetFormalType,
                                          mode);
  out.add(result);
}

/// When casting a class instance to a final class, we can directly compare
/// the isa-pointer with address of the metadata. This avoids a call to
/// `swift_dynamicCastClass`.
/// It also avoids a call to the metadata accessor of the class (which calls
/// `swift_getInitializedObjCClass`). For comparing the metadata pointers it's
/// not required that the metadata is fully initialized.
llvm::Value *irgen::emitFastClassCastIfPossible(
    IRGenFunction &IGF, llvm::Value *instance, CanType sourceFormalType,
    CanType targetFormalType, bool sourceWrappedInOptional,
    llvm::BasicBlock *&nilCheckBB, llvm::BasicBlock *&nilMergeBB) {
  if (!doesCastPreserveOwnershipForTypes(IGF.IGM.getSILModule(),
                                         sourceFormalType, targetFormalType)) {
    return nullptr;
  }

  // This does not include generic classes.
  auto classTy = dyn_cast<ClassType>(targetFormalType);
  if (!classTy)
    return nullptr;

  ClassDecl *toClass = classTy->getDecl();

  // Also exclude non-generic classes nested inside other generic types.
  if (toClass->isGenericContext())
    return nullptr;

  // TODO: we could use the ClassHierarchyAnalysis do also handle "effectively"
  // final classes, e.g. not-subclassed internal classes in WMO.
  // This would need some rearchitecting of ClassHierarchyAnalysis to make it
  // available in IRGen.
  if (!toClass->isFinal())
    return nullptr;

  AncestryOptions forbidden = AncestryOptions(AncestryFlags::ObjC) |
                              AncestryFlags::Resilient |
                              AncestryFlags::ResilientOther |
                              AncestryFlags::ClangImported |
                              AncestryFlags::ObjCObjectModel;
  if (toClass->checkAncestry() & forbidden)
    return nullptr;

  // If the source was originally wrapped in an Optional, check it for nil now.
  if (sourceWrappedInOptional) {
    auto isNotNil = IGF.Builder.CreateICmpNE(
        instance, llvm::ConstantPointerNull::get(
                      cast<llvm::PointerType>(instance->getType())));
    auto *isNotNilContBB = llvm::BasicBlock::Create(IGF.IGM.getLLVMContext());
    nilMergeBB = llvm::BasicBlock::Create(IGF.IGM.getLLVMContext());
    nilCheckBB = IGF.Builder.GetInsertBlock();
    IGF.Builder.CreateCondBr(isNotNil, isNotNilContBB, nilMergeBB);

    IGF.Builder.emitBlock(isNotNilContBB);
  }

  // Get the metadata pointer of the destination class type.
  llvm::Value *destMetadata = IGF.IGM.getAddrOfTypeMetadata(targetFormalType);
  if (IGF.IGM.IRGen.Opts.LazyInitializeClassMetadata) {
    llvm::Function *accessor =
        IGF.IGM.getAddrOfTypeMetadataAccessFunction(targetFormalType,
                                                    NotForDefinition);
    auto request = DynamicMetadataRequest(MetadataState::Complete);
    // We know that we are not in a generic class context, so we can safely
    // determine that the call here does not need to take that into account.
    auto response =
        IGF.emitGenericTypeMetadataAccessFunctionCall(accessor, {}, request);
    destMetadata = response.getMetadata();
  }
  llvm::Value *lhs = IGF.Builder.CreateBitCast(destMetadata, IGF.IGM.Int8PtrTy);
    
  // Load the isa pointer.
  llvm::Value *objMetadata = emitHeapMetadataRefForHeapObject(IGF, instance,
      sourceFormalType, GenericSignature(), /*suppress cast*/ true);
  llvm::Value *rhs = IGF.Builder.CreateBitCast(objMetadata, IGF.IGM.Int8PtrTy);
  
  // return isa_ptr == metadata_ptr ? instance : nullptr
  llvm::Value *isEqual = IGF.Builder.CreateCmp(llvm::CmpInst::Predicate::ICMP_EQ,
                                              lhs, rhs);
  auto *instanceTy = cast<llvm::PointerType>(instance->getType());
  auto *nullPtr = llvm::ConstantPointerNull::get(instanceTy);
  auto *select = IGF.Builder.CreateSelect(isEqual, instance, nullPtr);
  llvm::Type *destTy = IGF.getTypeInfoForUnlowered(targetFormalType).getStorageType();
  return IGF.Builder.CreateBitCast(select, destTy);
}
