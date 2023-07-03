//===--- GenKeyPath.cpp - IRGen support for key path objects --------------===//
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
//  This file contains code for emitting key path patterns, which can be used
//  by the standard library to instantiate key path objects.
//
//===----------------------------------------------------------------------===//

#include "Callee.h"
#include "ClassLayout.h"
#include "ConstantBuilder.h"
#include "Explosion.h"
#include "GenClass.h"
#include "GenDecl.h"
#include "GenMeta.h"
#include "GenPointerAuth.h"
#include "GenProto.h"
#include "GenStruct.h"
#include "GenTuple.h"
#include "GenType.h"
#include "GenericRequirement.h"
#include "IRGenDebugInfo.h"
#include "IRGenFunction.h"
#include "IRGenMangler.h"
#include "IRGenModule.h"
#include "MetadataLayout.h"
#include "ProtocolInfo.h"
#include "StructLayout.h"
#include "TypeInfo.h"
#include "llvm/ADT/SetVector.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Function.h"
#include "swift/SIL/SILInstruction.h"
#include "swift/SIL/SILLocation.h"
#include "swift/SIL/TypeLowering.h"
#include "swift/ABI/KeyPath.h"
#include "swift/ABI/HeapObject.h"
#include "swift/AST/ASTContext.h"
#include "swift/AST/Decl.h"
#include "swift/AST/DiagnosticEngine.h"
#include "swift/AST/DiagnosticsIRGen.h"
#include "swift/AST/GenericEnvironment.h"
#include "swift/AST/ParameterList.h"
#include "swift/AST/Types.h"
#include "swift/Basic/Statistic.h"
#include "swift/IRGen/Linking.h"

using namespace swift;
using namespace irgen;

#define DEBUG_TYPE "IRGen key paths"
STATISTIC(NumTrivialPropertyDescriptors, "# of trivial property descriptors");
STATISTIC(NumNonTrivialPropertyDescriptors, "# of nontrivial property descriptors");

enum KeyPathAccessor {
  Getter,
  Setter,
  Equals,
  Hash,
};

static void
bindPolymorphicArgumentsFromComponentIndices(IRGenFunction &IGF,
                                     GenericEnvironment *genericEnv,
                                     ArrayRef<GenericRequirement> requirements,
                                     llvm::Value *args,
                                     llvm::Value *size,
                                     bool hasSubscriptIndices) {
  if (!genericEnv)
    return;
  
  // The generic environment is marshaled into the end of the component
  // argument area inside the instance. Bind the generic information out of
  // the buffer.
  if (hasSubscriptIndices) {
    auto genericArgsSize = llvm::ConstantInt::get(IGF.IGM.SizeTy,
      requirements.size() * IGF.IGM.getPointerSize().getValue());

    auto genericArgsOffset = IGF.Builder.CreateSub(size, genericArgsSize);
    args =
        IGF.Builder.CreateInBoundsGEP(IGF.IGM.Int8Ty, args, genericArgsOffset);
  }

  bindFromGenericRequirementsBuffer(
      IGF, requirements,
      Address(args, IGF.IGM.Int8Ty, IGF.IGM.getPointerAlignment()),
      MetadataState::Complete, genericEnv->getForwardingSubstitutionMap());
}

static llvm::Function *
getAccessorForComputedComponent(IRGenModule &IGM,
                                const KeyPathPatternComponent &component,
                                KeyPathAccessor whichAccessor,
                                GenericEnvironment *genericEnv,
                                ArrayRef<GenericRequirement> requirements,
                                bool hasSubscriptIndices) {
  SILFunction *accessor;
  switch (whichAccessor) {
  case Getter:
    accessor = component.getComputedPropertyGetter();
    break;
  case Setter:
    accessor = component.getComputedPropertySetter();
    break;
  case Equals:
    accessor = component.getSubscriptIndexEquals();
    break;
  case Hash:
    accessor = component.getSubscriptIndexHash();
    break;
  }
  
  // If the accessor is not generic, and locally available, we can use it as is.
  // If it's only externally available, we need a local thunk to relative-
  // reference.
  if (requirements.empty() &&
      !isAvailableExternally(accessor->getLinkage()) &&
      &IGM == IGM.IRGen.getGenModule(accessor)) {
    return IGM.getAddrOfSILFunction(accessor, NotForDefinition);
  }
  auto accessorFn = IGM.getAddrOfSILFunction(accessor, NotForDefinition);

  auto accessorFnTy = accessorFn->getFunctionType();

  // Otherwise, we need a thunk to unmarshal the generic environment from the
  // argument area. It'd be nice to have a good way to represent this
  // directly in SIL, of course...
  const char *thunkName;
  unsigned numArgsToForward;

  switch (whichAccessor) {
  case Getter:
    thunkName = "keypath_get";
    numArgsToForward = 2;
    break;
  case Setter:
    thunkName = "keypath_set";
    numArgsToForward = 2;
    break;
  case Equals:
    thunkName = "keypath_equals";
    numArgsToForward = 2;
    break;
  case Hash:
    thunkName = "keypath_hash";
    numArgsToForward = 1;
    break;
  }

  SmallVector<llvm::Type *, 4> thunkParams;
  for (unsigned i = 0; i < numArgsToForward; ++i)
    thunkParams.push_back(accessorFnTy->getParamType(i));
  
  switch (whichAccessor) {
  case Getter:
  case Setter:
    thunkParams.push_back(IGM.Int8PtrTy);
    break;
  case Equals:
  case Hash:
    break;
  }
  thunkParams.push_back(IGM.SizeTy);

  auto thunkType = llvm::FunctionType::get(accessorFnTy->getReturnType(),
                                           thunkParams,
                                           /*vararg*/ false);
  
  auto accessorThunk = llvm::Function::Create(thunkType,
    llvm::GlobalValue::PrivateLinkage, thunkName, IGM.getModule());
  accessorThunk->setAttributes(IGM.constructInitialAttributes());
  accessorThunk->setCallingConv(IGM.SwiftCC);

  switch (whichAccessor) {
  case Getter: {
    // Original accessor's args should be @in or @out, meaning they won't be
    // captured or aliased.
    accessorThunk->addParamAttr(0, llvm::Attribute::NoCapture);
    accessorThunk->addParamAttr(0, llvm::Attribute::NoAlias);
    accessorThunk->addParamAttr(1, llvm::Attribute::NoCapture);
    accessorThunk->addParamAttr(1, llvm::Attribute::NoAlias);
    // Output is sret.
    auto structRetTy = accessorFn->getParamStructRetType(0);
    accessorThunk->addParamAttr(0, llvm::Attribute::getWithStructRetType(
                                       IGM.getLLVMContext(), structRetTy));
    break;
  }
  case Setter:
    // Original accessor's args should be @in or @out, meaning they won't be
    // captured or aliased.
    accessorThunk->addParamAttr(0, llvm::Attribute::NoCapture);
    accessorThunk->addParamAttr(0, llvm::Attribute::NoAlias);
    accessorThunk->addParamAttr(1, llvm::Attribute::NoCapture);
    accessorThunk->addParamAttr(1, llvm::Attribute::NoAlias);
    break;
  case Equals:
  case Hash:
    break;
  }

  {
    IRGenFunction IGF(IGM, accessorThunk);
    if (IGM.DebugInfo)
      IGM.DebugInfo->emitArtificialFunction(IGF, accessorThunk);
      
    auto params = IGF.collectParameters();
    Explosion forwardedArgs;
    forwardedArgs.add(params.claim(numArgsToForward));
    
    llvm::Value *componentArgsBuf;
    switch (whichAccessor) {
    case Getter:
    case Setter:
      // The component arguments are passed alongside the base being projected.
      componentArgsBuf = params.claimNext();
      // Pass the argument pointer down to the underlying function, if it
      // wants it.
      if (hasSubscriptIndices) {
        forwardedArgs.add(componentArgsBuf);
      }
      break;
    case Equals:
    case Hash:
      // We're operating directly on the component argument buffer.
      componentArgsBuf = forwardedArgs.getAll()[0];
      break;
    }
    auto componentArgsBufSize = params.claimNext();
    bindPolymorphicArgumentsFromComponentIndices(IGF,
                                                 genericEnv, requirements,
                                                 componentArgsBuf,
                                                 componentArgsBufSize,
                                                 hasSubscriptIndices);
    
    // Use the bound generic metadata to form a call to the original generic
    // accessor.
    if (genericEnv) {
      WitnessMetadata ignoreWitnessMetadata;
      auto forwardingSubs = genericEnv->getForwardingSubstitutionMap();
      emitPolymorphicArguments(IGF, accessor->getLoweredFunctionType(),
                               forwardingSubs,
                               &ignoreWitnessMetadata,
                               forwardedArgs);
    }
    auto fnPtr =
        FunctionPointer::forDirect(IGM, accessorFn, /*secondaryValue*/ nullptr,
                                   accessor->getLoweredFunctionType());
    auto call = IGF.Builder.CreateCall(fnPtr, forwardedArgs.claimAll());
    
    if (call->getType()->isVoidTy())
      IGF.Builder.CreateRetVoid();
    else
      IGF.Builder.CreateRet(call);
  }
  
  return accessorThunk;
}

static llvm::Function *
getLayoutFunctionForComputedComponent(IRGenModule &IGM,
                                    const KeyPathPatternComponent &component,
                                    GenericEnvironment *genericEnv,
                                    ArrayRef<GenericRequirement> requirements) {
  // Generate a function that returns the expected size and alignment necessary
  // to store captured generic context and subscript index arguments.
  auto retTy = llvm::StructType::get(IGM.getLLVMContext(),
                                     {IGM.SizeTy, IGM.SizeTy});
  auto fnTy = llvm::FunctionType::get(
    retTy, { IGM.Int8PtrTy }, /*vararg*/ false);
    
  auto layoutFn = llvm::Function::Create(fnTy,
    llvm::GlobalValue::PrivateLinkage, "keypath_get_arg_layout", IGM.getModule());
  layoutFn->setAttributes(IGM.constructInitialAttributes());
  layoutFn->setCallingConv(IGM.SwiftCC);
    
  {
    IRGenFunction IGF(IGM, layoutFn);
    if (IGM.DebugInfo)
      IGM.DebugInfo->emitArtificialFunction(IGF, layoutFn);
    // Unmarshal the generic environment from the argument buffer.
    auto parameters = IGF.collectParameters();
    auto args = parameters.claimNext();
    
    if (genericEnv) {
      bindFromGenericRequirementsBuffer(
          IGF, requirements,
          Address(args, IGM.Int8Ty, IGF.IGM.getPointerAlignment()),
          MetadataState::Complete, genericEnv->getForwardingSubstitutionMap());
    }
    
    // Run through the captured index types to determine the size and alignment
    // needed. Start with pointer alignment for the generic environment.
    llvm::Value *size = llvm::ConstantInt::get(IGM.SizeTy, 0);
    llvm::Value *alignMask = llvm::ConstantInt::get(IGM.SizeTy, 0);

    for (auto &index : component.getSubscriptIndices()) {
      auto ty = genericEnv
        ? genericEnv->mapTypeIntoContext(IGM.getSILModule(), index.LoweredType)
        : index.LoweredType;
      auto &ti = IGM.getTypeInfo(ty);
      auto indexSize = ti.getSize(IGF, ty);
      auto indexAlign = ti.getAlignmentMask(IGF, ty);
      
      auto notIndexAlign = IGF.Builder.CreateNot(indexAlign);
      
      size = IGF.Builder.CreateAdd(size, indexAlign);
      size = IGF.Builder.CreateAnd(size, notIndexAlign);
      size = IGF.Builder.CreateAdd(size, indexSize);
      
      alignMask = IGF.Builder.CreateOr(alignMask, indexAlign);
    }
    
    // If there's generic environment to capture, then it's stored as a block
    // of pointer-aligned words after the captured values.
    
    auto genericsSize = llvm::ConstantInt::get(IGM.SizeTy,
      IGM.getPointerSize().getValue() * requirements.size());
    auto genericsAlign = llvm::ConstantInt::get(IGM.SizeTy,
      IGM.getPointerAlignment().getValue() - 1);
    auto notGenericsAlign = llvm::ConstantExpr::getNot(genericsAlign);
    size = IGF.Builder.CreateAdd(size, genericsAlign);
    size = IGF.Builder.CreateAnd(size, notGenericsAlign);
    size = IGF.Builder.CreateAdd(size, genericsSize);
    alignMask = IGF.Builder.CreateOr(alignMask, genericsAlign);

    llvm::Value *retValue = IGF.Builder.CreateInsertValue(
      llvm::UndefValue::get(retTy), size, 0);
    retValue = IGF.Builder.CreateInsertValue(
      retValue, alignMask, 1);
      
    IGF.Builder.CreateRet(retValue);
  }
  
  return layoutFn;
}

static llvm::Constant *
getWitnessTableForComputedComponent(IRGenModule &IGM,
                                    const KeyPathPatternComponent &component,
                                    GenericEnvironment *genericEnv,
                                    ArrayRef<GenericRequirement> requirements) {
  // If the only thing we're capturing is generic environment, then we can
  // use a prefab witness table from the runtime. A null reference will be
  // filled in by the runtime.
  if (component.getSubscriptIndices().empty()) {
    return nullptr;
  }
  
  // Are the index values trivial?
  bool isTrivial = true;
  for (auto &component : component.getSubscriptIndices()) {
    auto ty = genericEnv
      ? genericEnv->mapTypeIntoContext(IGM.getSILModule(), component.LoweredType)
      : component.LoweredType;
    auto &ti = IGM.getTypeInfo(ty);
    isTrivial &= ti.isTriviallyDestroyable(ResilienceExpansion::Minimal);
  }
  
  llvm::Constant *destroy = nullptr;
  llvm::Constant *copy;
  if (isTrivial) {
    // We can use prefab witnesses for handling trivial copying and destruction.
    // A null destructor witness signals that the payload is trivial.
    copy = IGM.getCopyKeyPathTrivialIndicesFn();
  } else {
    // Generate a destructor for this set of indices.
    {
      auto destroyType = llvm::FunctionType::get(IGM.VoidTy,
                                                 {IGM.Int8PtrTy, IGM.SizeTy},
                                                 /*vararg*/ false);
      auto destroyFn = llvm::Function::Create(destroyType,
        llvm::GlobalValue::PrivateLinkage, "keypath_destroy", IGM.getModule());
      destroy = destroyFn;
      destroyFn->setAttributes(IGM.constructInitialAttributes());
      destroyFn->setCallingConv(IGM.SwiftCC);
      
      IRGenFunction IGF(IGM, destroyFn);
      if (IGM.DebugInfo)
        IGM.DebugInfo->emitArtificialFunction(IGF, destroyFn);
    
      auto params = IGF.collectParameters();
      auto componentArgsBuf = params.claimNext();
      auto componentArgsBufSize = params.claimNext();
      bindPolymorphicArgumentsFromComponentIndices(IGF,
                                     genericEnv, requirements,
                                     componentArgsBuf,
                                     componentArgsBufSize,
                                     !component.getSubscriptIndices().empty());
      
      llvm::Value *offset = nullptr;
      for (auto &component : component.getSubscriptIndices()) {
        auto ty = genericEnv
          ? genericEnv->mapTypeIntoContext(IGM.getSILModule(),
                                           component.LoweredType)
          : component.LoweredType;
        auto &ti = IGM.getTypeInfo(ty);
        if (offset) {
          auto align = ti.getAlignmentMask(IGF, ty);
          auto notAlign = IGF.Builder.CreateNot(align);
          offset = IGF.Builder.CreateAdd(offset, align);
          offset = IGF.Builder.CreateAnd(offset, notAlign);
        } else {
          offset = llvm::ConstantInt::get(IGM.SizeTy, 0);
        }
        auto elt =
            IGF.Builder.CreateInBoundsGEP(IGM.Int8Ty, componentArgsBuf, offset);
        auto eltAddr = ti.getAddressForPointer(
          IGF.Builder.CreateBitCast(elt, ti.getStorageType()->getPointerTo()));
        ti.destroy(IGF, eltAddr, ty,
                   true /*witness table: need it to be fast*/);
        auto size = ti.getSize(IGF, ty);
        offset = IGF.Builder.CreateAdd(offset, size);
      }
      IGF.Builder.CreateRetVoid();
    }
    // Generate a copier for this set of indices.
    {
      auto copyType = llvm::FunctionType::get(IGM.VoidTy,
                                              {IGM.Int8PtrTy, IGM.Int8PtrTy,
                                               IGM.SizeTy},
                                              /*vararg*/ false);
      auto copyFn = llvm::Function::Create(copyType,
        llvm::GlobalValue::PrivateLinkage, "keypath_copy", IGM.getModule());
      copy = copyFn;
      copyFn->setAttributes(IGM.constructInitialAttributes());
      copyFn->setCallingConv(IGM.SwiftCC);
      
      IRGenFunction IGF(IGM, copyFn);
      if (IGM.DebugInfo)
        IGM.DebugInfo->emitArtificialFunction(IGF, copyFn);
    
      auto params = IGF.collectParameters();
      auto sourceArgsBuf = params.claimNext();
      auto destArgsBuf = params.claimNext();
      auto componentArgsBufSize = params.claimNext();
      bindPolymorphicArgumentsFromComponentIndices(IGF,
                                     genericEnv, requirements,
                                     sourceArgsBuf,
                                     componentArgsBufSize,
                                     !component.getSubscriptIndices().empty());
      
      // Copy over the index values.
      llvm::Value *offset = nullptr;
      for (auto &component : component.getSubscriptIndices()) {
        auto ty = genericEnv
          ? genericEnv->mapTypeIntoContext(IGM.getSILModule(),
                                           component.LoweredType)
          : component.LoweredType;
        auto &ti = IGM.getTypeInfo(ty);
        if (offset) {
          auto align = ti.getAlignmentMask(IGF, ty);
          auto notAlign = IGF.Builder.CreateNot(align);
          offset = IGF.Builder.CreateAdd(offset, align);
          offset = IGF.Builder.CreateAnd(offset, notAlign);
        } else {
          offset = llvm::ConstantInt::get(IGM.SizeTy, 0);
        }
        auto sourceElt =
            IGF.Builder.CreateInBoundsGEP(IGM.Int8Ty, sourceArgsBuf, offset);
        auto destElt =
            IGF.Builder.CreateInBoundsGEP(IGM.Int8Ty, destArgsBuf, offset);
        auto sourceEltAddr = ti.getAddressForPointer(
          IGF.Builder.CreateBitCast(sourceElt,
                                    ti.getStorageType()->getPointerTo()));
        auto destEltAddr = ti.getAddressForPointer(
          IGF.Builder.CreateBitCast(destElt,
                                    ti.getStorageType()->getPointerTo()));

        ti.initializeWithCopy(IGF, destEltAddr, sourceEltAddr, ty, false);
        auto size = ti.getSize(IGF, ty);
        offset = IGF.Builder.CreateAdd(offset, size);
      }
      
      // Copy over the generic environment.
      if (genericEnv) {
        auto envAlignMask = llvm::ConstantInt::get(IGM.SizeTy,
          IGM.getPointerAlignment().getMaskValue());
        auto notAlignMask = IGF.Builder.CreateNot(envAlignMask);
        offset = IGF.Builder.CreateAdd(offset, envAlignMask);
        offset = IGF.Builder.CreateAnd(offset, notAlignMask);

        auto sourceEnv =
            IGF.Builder.CreateInBoundsGEP(IGM.Int8Ty, sourceArgsBuf, offset);
        auto destEnv =
            IGF.Builder.CreateInBoundsGEP(IGM.Int8Ty, destArgsBuf, offset);

        auto align = IGM.getPointerAlignment().getValue();
        IGF.Builder.CreateMemCpy(destEnv, llvm::MaybeAlign(align), sourceEnv,
                                 llvm::MaybeAlign(align),
                                 IGM.getPointerSize().getValue() *
                                     requirements.size());
      }
      
      IGF.Builder.CreateRetVoid();
    }
  }
  
  auto equals = getAccessorForComputedComponent(IGM, component, Equals,
                                      genericEnv, requirements,
                                      !component.getSubscriptIndices().empty());
  auto hash = getAccessorForComputedComponent(IGM, component, Hash,
                                      genericEnv, requirements,
                                      !component.getSubscriptIndices().empty());
  
  ConstantInitBuilder builder(IGM);
  ConstantStructBuilder fields = builder.beginStruct();
  auto schemaKeyPath = IGM.getOptions().PointerAuth.KeyPaths;
  if (destroy)
    fields.addSignedPointer(destroy, schemaKeyPath,
                            PointerAuthEntity::Special::KeyPathDestroy);
  else
    fields.addNullPointer(IGM.FunctionPtrTy);
  fields.addSignedPointer(copy, schemaKeyPath,
                          PointerAuthEntity::Special::KeyPathCopy);
  fields.addSignedPointer(equals, schemaKeyPath,
                          PointerAuthEntity::Special::KeyPathEquals);
  fields.addSignedPointer(hash, schemaKeyPath,
                          PointerAuthEntity::Special::KeyPathHash);
  return fields.finishAndCreateGlobal(
      "keypath_witnesses", IGM.getPointerAlignment(), /*constant*/ true,
      llvm::GlobalVariable::PrivateLinkage);
}

/// Information about each index operand for a key path pattern that is used
/// to lay out and consume the argument packet.
struct KeyPathIndexOperand {
  SILType LoweredType;
  const KeyPathPatternComponent *LastUser;
};

static llvm::Function *
getInitializerForComputedComponent(IRGenModule &IGM,
           const KeyPathPatternComponent &component,
           ArrayRef<KeyPathIndexOperand> operands,
           GenericEnvironment *genericEnv,
           ArrayRef<GenericRequirement> requirements) {
  auto fnTy = llvm::FunctionType::get(IGM.VoidTy,
    { /*src*/ IGM.Int8PtrTy,
      /*dest*/ IGM.Int8PtrTy }, /*vararg*/ false);
      
  auto initFn = llvm::Function::Create(fnTy,
    llvm::GlobalValue::PrivateLinkage, "keypath_arg_init", IGM.getModule());
  initFn->setAttributes(IGM.constructInitialAttributes());
  initFn->setCallingConv(IGM.SwiftCC);
    
  {
    IRGenFunction IGF(IGM, initFn);
    if (IGM.DebugInfo)
      IGM.DebugInfo->emitArtificialFunction(IGF, initFn);

    auto params = IGF.collectParameters();
    // Pointer to the argument packet passed into swift_getKeyPath
    auto src = params.claimNext();
    // Pointer to the destination component's argument buffer
    auto dest = params.claimNext();
    
    SmallVector<Address, 4> srcAddresses;
    int lastOperandNeeded = -1;
    for (auto &index : component.getSubscriptIndices()) {
      lastOperandNeeded = std::max(lastOperandNeeded, (int)index.Operand);
    }
    
    llvm::Value *offset;
    
    if (genericEnv) {
      // We'll copy over the generic environment after we copy in the indexes.
      offset = llvm::ConstantInt::get(IGM.SizeTy,
        IGM.getPointerSize().getValue() * requirements.size());

      // Bind the generic environment from the argument buffer.
      bindFromGenericRequirementsBuffer(
          IGF, requirements,
          Address(src, IGM.Int8Ty, IGF.IGM.getPointerAlignment()),
          MetadataState::Complete, genericEnv->getForwardingSubstitutionMap());

    } else {
      offset = llvm::ConstantInt::get(IGM.SizeTy, 0);
    }
    
    // Figure out the offsets of the operands in the source buffer.
    for (int i = 0; i <= lastOperandNeeded; ++i) {
      auto ty = genericEnv
        ? genericEnv->mapTypeIntoContext(IGM.getSILModule(),
                                         operands[i].LoweredType)
        : operands[i].LoweredType;
      
      auto &ti = IGM.getTypeInfo(ty);

      if (i != 0 || genericEnv) {
        auto alignMask = ti.getAlignmentMask(IGF, ty);
        auto notAlignMask = IGF.Builder.CreateNot(alignMask);
        offset = IGF.Builder.CreateAdd(offset, alignMask);
        offset = IGF.Builder.CreateAnd(offset, notAlignMask);
      }

      auto ptr = IGF.Builder.CreateInBoundsGEP(IGM.Int8Ty, src, offset);
      auto addr = ti.getAddressForPointer(IGF.Builder.CreateBitCast(
        ptr, ti.getStorageType()->getPointerTo()));
      srcAddresses.push_back(addr);
      
      auto size = ti.getSize(IGF, ty);
      offset = IGF.Builder.CreateAdd(offset, size);
    }
    
    offset = llvm::ConstantInt::get(IGM.SizeTy, 0);
    
    // Transfer the operands we want into the destination buffer.
    for (unsigned i : indices(component.getSubscriptIndices())) {
      auto &index = component.getSubscriptIndices()[i];
      
      auto ty = genericEnv
        ? genericEnv->mapTypeIntoContext(IGM.getSILModule(),
                                         index.LoweredType)
        : index.LoweredType;
      
      auto &ti = IGM.getTypeInfo(ty);
      
      if (i != 0) {
        auto alignMask = ti.getAlignmentMask(IGF, ty);
        auto notAlignMask = IGF.Builder.CreateNot(alignMask);
        offset = IGF.Builder.CreateAdd(offset, alignMask);
        offset = IGF.Builder.CreateAnd(offset, notAlignMask);
      }

      auto ptr = IGF.Builder.CreateInBoundsGEP(IGM.Int8Ty, dest, offset);
      auto destAddr = ti.getAddressForPointer(IGF.Builder.CreateBitCast(
        ptr, ti.getStorageType()->getPointerTo()));
      
      // The last component using an operand can move the value out of the
      // buffer.
      if (&component == operands[index.Operand].LastUser) {
        ti.initializeWithTake(IGF, destAddr, srcAddresses[index.Operand], ty,
                              false);
      } else {
        ti.initializeWithCopy(IGF, destAddr, srcAddresses[index.Operand], ty,
                              false);
      }
      auto size = ti.getSize(IGF, ty);
      offset = IGF.Builder.CreateAdd(offset, size);
    }
    
    // Transfer the generic environment.
    // External components don't need to store the key path environment (and
    // can't), since they need to already have enough information to function
    // independently of any context using the component.
    if (genericEnv) {
      auto destGenericEnv = dest;
      if (!component.getSubscriptIndices().empty()) {
        auto genericEnvAlignMask = llvm::ConstantInt::get(IGM.SizeTy,
          IGM.getPointerAlignment().getMaskValue());
        auto notGenericEnvAlignMask = IGF.Builder.CreateNot(genericEnvAlignMask);
        offset = IGF.Builder.CreateAdd(offset, genericEnvAlignMask);
        offset = IGF.Builder.CreateAnd(offset, notGenericEnvAlignMask);
        destGenericEnv =
            IGF.Builder.CreateInBoundsGEP(IGM.Int8Ty, dest, offset);
      }
      
      auto align = IGM.getPointerAlignment().getValue();
      IGF.Builder.CreateMemCpy(
          destGenericEnv, llvm::MaybeAlign(align), src, llvm::MaybeAlign(align),
          IGM.getPointerSize().getValue() * requirements.size());
    }
    IGF.Builder.CreateRetVoid();
  }
  return initFn;
}

static llvm::Constant *
emitMetadataTypeRefForKeyPath(IRGenModule &IGM, CanType type,
                              CanGenericSignature sig) {
  // Produce a mangled name for the type.
  auto constant = IGM.getTypeRef(type, sig, MangledTypeRefRole::Metadata).first;
  
  // Mask the bottom bit to tell the key path runtime this is a mangled name
  // rather than a direct reference.
  auto bitConstant = llvm::ConstantInt::get(IGM.IntPtrTy, 1);
  return llvm::ConstantExpr::getGetElementPtr(IGM.Int8Ty, constant,
                                              bitConstant);
}

static unsigned getClassFieldIndex(ClassDecl *classDecl, VarDecl *property) {
  SmallVector<ClassDecl *, 3> superclasses;
  for (auto *superDecl = classDecl; superDecl != nullptr;
       superDecl = superDecl->getSuperclassDecl()) {
    superclasses.push_back(superDecl);
  }

  std::reverse(superclasses.begin(), superclasses.end());

  unsigned index = 0;
  for (auto *superDecl : superclasses) {
    for (auto *other : superDecl->getStoredProperties()) {
      if (other == property)
        return index;
      ++index;
    }
  }

  llvm_unreachable("Did not find stored property in class");
}

static void
emitKeyPathComponent(IRGenModule &IGM,
                     ConstantStructBuilder &fields,
                     const KeyPathPatternComponent &component,
                     bool isInstantiableOnce,
                     GenericEnvironment *genericEnv,
                     ArrayRef<GenericRequirement> requirements,
                     CanType baseTy,
                     ArrayRef<KeyPathIndexOperand> operands,
                     bool hasSubscriptIndices) {
  assert(fields.getNextOffsetFromGlobal() % Alignment(4) == Size(0)
         && "must be 32-bit-aligned here");

  SILType loweredBaseTy;
  loweredBaseTy = IGM.getLoweredType(AbstractionPattern::getOpaque(),
                                     baseTy->getWithoutSpecifierType());
  // TODO: Eliminate GenericContextScope entirely
  GenericContextScope scope(
      IGM, genericEnv
               ? genericEnv->getGenericSignature().getCanonicalSignature()
               : nullptr);
  switch (auto kind = component.getKind()) {
  case KeyPathPatternComponent::Kind::StoredProperty: {
    auto property = cast<VarDecl>(component.getStoredPropertyDecl());
    
    auto addFixedOffset = [&](bool isStruct, bool isLet,
                              llvm::Constant *offset) {
      if (auto offsetInt = dyn_cast_or_null<llvm::ConstantInt>(offset)) {
        auto offsetValue = offsetInt->getValue().getZExtValue();
        if (KeyPathComponentHeader::offsetCanBeInline(offsetValue)) {
          auto header = isStruct
            ? KeyPathComponentHeader
                ::forStructComponentWithInlineOffset(isLet, offsetValue)
            : KeyPathComponentHeader
                ::forClassComponentWithInlineOffset(isLet, offsetValue);
          fields.addInt32(header.getData());
          return;
        }
      }
      auto header = isStruct
        ? KeyPathComponentHeader::forStructComponentWithOutOfLineOffset(isLet)
        : KeyPathComponentHeader::forClassComponentWithOutOfLineOffset(isLet);
      fields.addInt32(header.getData());
      fields.add(llvm::ConstantExpr::getTruncOrBitCast(offset, IGM.Int32Ty));
    };
    
    // For a struct stored property, we may know the fixed offset of the field,
    // or we may need to fetch it out of the type's metadata at instantiation
    // time.
    if (auto theStruct = loweredBaseTy.getStructOrBoundGenericStruct()) {
      if (auto offset = emitPhysicalStructMemberFixedOffset(IGM,
                                                            loweredBaseTy,
                                                            property)) {
        // We have a known constant fixed offset.
        addFixedOffset(/*struct*/ true, property->isLet(), offset);
        break;
      }

      // If the offset isn't fixed, try instead to get the field offset out
      // of the type metadata at instantiation time.
      auto &metadataLayout = IGM.getMetadataLayout(theStruct);
      auto fieldOffset = metadataLayout.getStaticFieldOffset(property);

      auto header = KeyPathComponentHeader
        ::forStructComponentWithUnresolvedFieldOffset(property->isLet());
      fields.addInt32(header.getData());
      fields.addInt32(fieldOffset.getValue());
      break;
    }

    auto *classDecl = baseTy->getClassOrBoundGenericClass();
    auto loweredClassTy = loweredBaseTy;

    // Recover class decl from superclass constraint
    if (!classDecl && genericEnv) {
      auto ty = genericEnv->mapTypeIntoContext(baseTy)->getCanonicalType();
      auto archetype = dyn_cast<ArchetypeType>(ty);
      if (archetype && archetype->requiresClass()) {
        auto superClassTy = ty->getSuperclass(false)->getCanonicalType();
        classDecl = superClassTy->getClassOrBoundGenericClass();
        loweredClassTy =
            IGM.getLoweredType(AbstractionPattern::getOpaque(),
                               superClassTy->getWithoutSpecifierType());
      }
    }

    // For a class, we may know the fixed offset of a field at compile time,
    // or we may need to fetch it at instantiation time. Depending on the
    // ObjC-ness and resilience of the class hierarchy, there might be a few
    // different ways we need to go about this.
    if (loweredClassTy.getClassOrBoundGenericClass()) {

      // Use the property's class type to determine the field access.
      auto propertyBaseDecl = property->getDeclContext()->getSelfClassDecl();
      auto currentBaseTy =
          loweredClassTy.getASTType()->getSuperclassForDecl(propertyBaseDecl);
      assert(currentBaseTy->getClassOrBoundGenericClass() == propertyBaseDecl);
      loweredClassTy =
          IGM.getLoweredType(AbstractionPattern::getOpaque(), currentBaseTy);

      auto loweredBaseContextTy =
          SILType::getPrimitiveObjectType(loweredClassTy.getASTType());
      if (!loweredClassTy.getASTType()->hasArchetype())
        loweredBaseContextTy = SILType::getPrimitiveObjectType(
            GenericEnvironment::mapTypeIntoContext(genericEnv,
                                                   loweredClassTy.getASTType())
                ->getCanonicalType());

      switch (getClassFieldAccess(IGM, loweredBaseContextTy, property)) {
      case FieldAccess::ConstantDirect: {
        // Known compile-time constant field offset.
        auto offset = tryEmitConstantClassFragilePhysicalMemberOffset(
            IGM, loweredClassTy, property);
        assert(offset && "no constant offset for ConstantDirect field?!");
        addFixedOffset(/*struct*/ false, property->isLet(), offset);
        break;
      }
      case FieldAccess::NonConstantDirect: {
        // A constant offset that's determined at class realization time.
        // We have to load the offset from a global ivar.
        //
        // This means the field offset is constant at runtime, but is not known
        // at compile time.
        auto header = KeyPathComponentHeader
          ::forClassComponentWithUnresolvedIndirectOffset(property->isLet());
        fields.addInt32(header.getData());
        auto offsetRef = IGM.getAddrOfLLVMVariableOrGOTEquivalent(
          LinkEntity::forFieldOffset(property));
        fields.addRelativeAddress(offsetRef);
        break;
      }
      case FieldAccess::ConstantIndirect: {
        // An offset that depends on the instance's generic parameterization,
        // but whose field offset is at a known metadata offset.
        auto header = KeyPathComponentHeader
          ::forClassComponentWithUnresolvedFieldOffset(property->isLet());
        fields.addInt32(header.getData());

        // FIXME: This doesn't support classes with resilient ancestry, because
        // the offset into the metadata is itself not constant.
        //
        // SILGen emits the descriptor as a computed property in this case.
        auto fieldOffset = getClassFieldOffsetOffset(
            IGM, loweredClassTy.getClassOrBoundGenericClass(), property);
        fields.addInt32(fieldOffset.getValue());
        break;
      }
      }
      break;
    }
    llvm_unreachable("not struct or class");
  }
  case KeyPathPatternComponent::Kind::GettableProperty:
  case KeyPathPatternComponent::Kind::SettableProperty: {
    // If the component references an external property, encode that in a
    // header before the local attempt header, so that we can consult the
    // external descriptor at instantiation time.
    //
    // Note that when compiling inlinable functions, we can have external
    // declarations that point within the same module. Just ignore those.
    auto externalDecl = component.getExternalDecl();
    if (externalDecl &&
        externalDecl->getModuleContext() != IGM.getSwiftModule()) {
      SmallVector<llvm::Constant *, 4> externalSubArgs;
      auto componentSig = externalDecl->getInnermostDeclContext()
        ->getGenericSignatureOfContext();

      auto componentCanSig = componentSig.getCanonicalSignature();
      auto subs = component.getExternalSubstitutions();
      if (!subs.empty()) {
        enumerateGenericSignatureRequirements(
            componentCanSig, [&](GenericRequirement reqt) {
              auto substType =
                  reqt.getTypeParameter().subst(subs)->getCanonicalType();

              // FIXME: This seems wrong. We used to just mangle opened archetypes as
              // their interface type. Let's make that explicit now.
              substType = substType
                              .transformRec([](Type t) -> llvm::Optional<Type> {
                                if (auto *openedExistential =
                                        t->getAs<OpenedArchetypeType>())
                                  return openedExistential->getInterfaceType();
                                return llvm::None;
                              })
                              ->getCanonicalType();

              if (reqt.isAnyMetadata()) {
                // Type requirement.
                externalSubArgs.push_back(emitMetadataTypeRefForKeyPath(
                    IGM, substType, componentCanSig));
              } else {
                assert(reqt.isAnyWitnessTable());

                // Protocol requirement.
                auto conformance = subs.lookupConformance(
                    reqt.getTypeParameter()->getCanonicalType(),
                    reqt.getProtocol());
                externalSubArgs.push_back(IGM.emitWitnessTableRefString(
                    substType, conformance,
                    genericEnv ? genericEnv->getGenericSignature() :
                                 componentCanSig,
                    /*shouldSetLowBit*/ true));
              }
            });
      }
      fields.addInt32(
        KeyPathComponentHeader::forExternalComponent(externalSubArgs.size())
          .getData());
      auto descriptor = IGM.getAddrOfLLVMVariableOrGOTEquivalent(
        LinkEntity::forPropertyDescriptor(externalDecl));
      fields.addRelativeAddress(descriptor);
      for (auto *arg : externalSubArgs)
        fields.addRelativeAddress(arg);
    }
  
    // Encode the settability.
    bool settable = kind == KeyPathPatternComponent::Kind::SettableProperty;
    bool mutating = settable && component.isComputedSettablePropertyMutating();
    KeyPathComponentHeader::ComputedPropertyKind componentKind;
    if (settable) {
      componentKind = mutating
        ? KeyPathComponentHeader::SettableMutating
        : KeyPathComponentHeader::SettableNonmutating;
    } else {
      componentKind = KeyPathComponentHeader::GetOnly;
    }
    
    // Lower the id reference.
    auto id = component.getComputedPropertyId();
    KeyPathComponentHeader::ComputedPropertyIDKind idKind;
    llvm::Constant *idValue;
    KeyPathComponentHeader::ComputedPropertyIDResolution idResolution;
    switch (id.getKind()) {
    case KeyPathPatternComponent::ComputedPropertyId::Function: {
      idKind = KeyPathComponentHeader::Pointer;
      // FIXME: Does this need to be signed?
      auto idRef = IGM.getAddrOfLLVMVariableOrGOTEquivalent(
        LinkEntity::forSILFunction(id.getFunction()));
      
      idValue = idRef.getValue();
      // If we got an indirect reference, we'll need to resolve it at
      // instantiation time.
      idResolution = idRef.isIndirect()
        ? KeyPathComponentHeader::IndirectPointer
        // Compute absolute reference from relative reference if target supports it.
        // Otherwise, embed absolute reference directly.
        : (IGM.getOptions().CompactAbsoluteFunctionPointer
          ? KeyPathComponentHeader::ResolvedAbsolute
          : KeyPathComponentHeader::Resolved);
      break;
    }
    case KeyPathPatternComponent::ComputedPropertyId::DeclRef: {
      auto declRef = id.getDeclRef();
    
      // Foreign method refs identify using a selector
      // reference, which is doubly-indirected and filled in with a unique
      // pointer by dyld.
      if (declRef.isForeign) {
        assert(IGM.ObjCInterop && "foreign keypath component w/o objc interop?!");
        idKind = KeyPathComponentHeader::Pointer;
        // FIXME: In non-JIT mode, ideally we would just refer to the selector
        // reference variable here with an indirectpointer resolution,
        // but ld64 section coalescing on the __objc_sel section can break
        // relative references (and on some platforms, mach-o just doesn't
        // support the necessary relocations).
        // As a workaround, generate a stub function to resolve the selector.
        //
        // Note that we'd need to do this anyway in JIT mode because we would
        // need to unique the selector at runtime anyway.
        auto selectorName = IGM.getObjCSelectorName(declRef);
        SmallString<32> fnName;
        fnName.append("keypath_get_selector_");
        fnName.append(selectorName);
        auto fn = IGM.getOrCreateHelperFunction(fnName, IGM.Int8PtrTy,
                                                {IGM.Int8PtrTy},
                                      [&selectorName](IRGenFunction &subIGF) {
          auto selectorValue = subIGF.emitObjCSelectorRefLoad(selectorName);
          subIGF.Builder.CreateRet(selectorValue);
        });

        idValue = fn;
        idResolution = KeyPathComponentHeader::FunctionCall;
      } else {
        if (auto overridden = declRef.getOverriddenVTableEntry())
          declRef = overridden;
        if (auto overridden = declRef.getOverriddenWitnessTableEntry())
          declRef = overridden;

        auto dc = declRef.getDecl()->getDeclContext();

        // We can use a method descriptor if we have a class or resilient
        // protocol.
        if (isa<ClassDecl>(dc) ||
            IGM.isResilient(cast<NominalTypeDecl>(dc),
                            ResilienceExpansion::Minimal)) {
          idKind = KeyPathComponentHeader::Pointer;
          auto idRef = IGM.getAddrOfLLVMVariableOrGOTEquivalent(
            LinkEntity::forMethodDescriptor(declRef));

          idValue = idRef.getValue();
          idResolution = idRef.isIndirect()
            ? KeyPathComponentHeader::IndirectPointer
            : KeyPathComponentHeader::Resolved;
          break;
        }
      
        idKind = KeyPathComponentHeader::VTableOffset;
        auto methodProto = cast<ProtocolDecl>(dc);
        auto &protoInfo = IGM.getProtocolInfo(methodProto,
                                              ProtocolInfoKind::Full);
        auto index = protoInfo.getFunctionIndex(declRef);
        idValue = llvm::ConstantInt::get(IGM.SizeTy, -index.getValue());
        idResolution = KeyPathComponentHeader::Resolved;
      }
      break;
    }
    case KeyPathPatternComponent::ComputedPropertyId::Property:
      // Use the index of the stored property within the aggregate to key
      // the property.
      auto property = id.getProperty();
      idKind = KeyPathComponentHeader::StoredPropertyIndex;
      auto *classDecl = baseTy->getClassOrBoundGenericClass();
      auto loweredClassTy = loweredBaseTy;
      // Recover class decl from superclass constraint
      if (!classDecl && genericEnv) {
        auto ty = genericEnv->mapTypeIntoContext(baseTy)->getCanonicalType();
        auto archetype = dyn_cast<ArchetypeType>(ty);
        if (archetype && archetype->requiresClass()) {
          auto superClassTy = ty->getSuperclass(false)->getCanonicalType();
          classDecl = superClassTy->getClassOrBoundGenericClass();
          loweredClassTy =
              IGM.getLoweredType(AbstractionPattern::getOpaque(),
                                 superClassTy->getWithoutSpecifierType());
        }
      }
      if (auto struc = baseTy->getStructOrBoundGenericStruct()) {
        // Scan the stored properties of the struct to find the index. We should
        // only ever use a struct field as a uniquing key from inside the
        // struct's own module, so this is OK.
        idResolution = KeyPathComponentHeader::Resolved;
        llvm::Optional<unsigned> structIdx;
        unsigned i = 0;
        for (auto storedProp : struc->getStoredProperties()) {
          if (storedProp == property) {
            structIdx = i;
            break;
          }
          ++i;
        }
        assert(structIdx && "not a stored property of the struct?!");
        idValue = llvm::ConstantInt::get(IGM.SizeTy, structIdx.value());
      } else if (classDecl) {
        // TODO: This field index would require runtime resolution with Swift
        // native class resilience. We never directly access ObjC-imported
        // ivars so we can disregard ObjC ivar resilience for this computation
        // and start counting at the Swift native root.
        if (loweredClassTy.getASTType()->hasTypeParameter())
          loweredClassTy = SILType::getPrimitiveObjectType(
              GenericEnvironment::mapTypeIntoContext(
                  genericEnv, loweredClassTy.getASTType())
                  ->getCanonicalType());
        switch (getClassFieldAccess(IGM, loweredClassTy, property)) {
        case FieldAccess::ConstantDirect:
        case FieldAccess::ConstantIndirect:
        case FieldAccess::NonConstantDirect:
          idResolution = KeyPathComponentHeader::Resolved;
          idValue = llvm::ConstantInt::get(IGM.SizeTy,
                                       getClassFieldIndex(classDecl, property));
          break;
        }
        
      } else {
        llvm_unreachable("neither struct nor class");
      }
      break;
    }
    
    auto header = KeyPathComponentHeader::forComputedProperty(componentKind,
                                     idKind, !isInstantiableOnce, idResolution);
    
    fields.addInt32(header.getData());
    switch (idKind) {
    case KeyPathComponentHeader::Pointer:
      // Use a relative offset to the referent if value is not absolute.
      if (idResolution == KeyPathComponentHeader::ResolvedAbsolute) {
        fields.add(idValue);
      } else {
        fields.addRelativeAddress(idValue);
      }
      break;

    case KeyPathComponentHeader::VTableOffset:
    case KeyPathComponentHeader::StoredPropertyIndex:
      // Store the offset as an i32.
      fields.add(llvm::ConstantExpr::getTruncOrBitCast(idValue, IGM.Int32Ty));
      break;
    }

    // Push the accessors, possibly thunked to marshal generic environment.
    fields.addCompactFunctionReference(
      getAccessorForComputedComponent(IGM, component, Getter,
                                      genericEnv, requirements,
                                      hasSubscriptIndices));
    if (settable)
      fields.addCompactFunctionReference(
        getAccessorForComputedComponent(IGM, component, Setter,
                                        genericEnv, requirements,
                                        hasSubscriptIndices));

    if (!isInstantiableOnce) {
      // If there's generic context or subscript indexes, embed as
      // arguments in the component. Thunk the SIL-level accessors to give the
      // runtime implementation a polymorphically-callable interface.

      fields.addCompactFunctionReference(
        getLayoutFunctionForComputedComponent(IGM, component,
                                              genericEnv, requirements));
      
      // Set up a "witness table" for the component that handles copying,
      // destroying, equating, and hashing the captured contents of the
      // component.
      if (auto witnessTable =
            getWitnessTableForComputedComponent(IGM, component,
                                                genericEnv, requirements)) {
        fields.addRelativeAddress(witnessTable);
      } else {
        // If there are only generic parameters, we can use a prefab witness
        // table from the runtime. Leaving a null reference here will let
        // the runtime fill it in.
        fields.addInt32(0);
      }
      
      // Add an initializer function that copies generic arguments out of the
      // pattern argument buffer into the instantiated object.
      fields.addCompactFunctionReference(
        getInitializerForComputedComponent(IGM, component, operands,
                                           genericEnv, requirements));
    }
    break;
  }
  case KeyPathPatternComponent::Kind::OptionalChain:
    fields.addInt32(KeyPathComponentHeader::forOptionalChain().getData());
    break;
  case KeyPathPatternComponent::Kind::OptionalForce:
    fields.addInt32(KeyPathComponentHeader::forOptionalForce().getData());
    break;
  case KeyPathPatternComponent::Kind::OptionalWrap:
    fields.addInt32(KeyPathComponentHeader::forOptionalWrap().getData());
    break;
  case KeyPathPatternComponent::Kind::TupleElement:
    assert(baseTy->is<TupleType>() && "not a tuple");

    SILType loweredTy = IGM.getLoweredType(AbstractionPattern::getOpaque(),
                                           baseTy);

    // Tuple with fixed layout
    //
    // This code is ALSO executed in the case of a tuple with dynamic layout,
    // (see below) but only if `component.getTupleIndex()` is 0 - in that case
    // the compiler knows that the tuple element is always at offset 0.
    // TODO: If this is behavior is not desired we should find a way to skip to
    // the next section of code e.g. check if baseTy has archetypes?
    if (auto offset = getFixedTupleElementOffset(IGM, loweredTy,
                                                 component.getTupleIndex())) {
      auto header = KeyPathComponentHeader
                      ::forStructComponentWithInlineOffset(/*isLet*/ false,
                                                           offset->getValue());

      fields.addInt32(header.getData());
      break;
    }

    // Tuple with dynamic layout
    auto elementOffset = getStaticTupleElementOffset(IGM,
                                                     loweredTy,
                                                     component.getTupleIndex());

    auto header = KeyPathComponentHeader
      ::forStructComponentWithUnresolvedFieldOffset(/*isLet*/ false);
    fields.addInt32(header.getData());
    fields.addInt32(elementOffset.getValue());
    break;

    llvm_unreachable("could not get tuple element offset");
  }
}

llvm::Constant *
IRGenModule::getAddrOfKeyPathPattern(KeyPathPattern *pattern,
                                     SILLocation diagLoc) {
  // See if we already emitted this.
  auto found = KeyPathPatterns.find(pattern);
  if (found != KeyPathPatterns.end())
    return found->second;
  
  // Gather type arguments from the root and leaf types of the key path.
  auto rootTy = pattern->getRootType();
  auto valueTy = pattern->getValueType();

  // Check for parameterization, whether by subscript indexes or by the generic
  // environment. If there isn't any, we can instantiate the pattern in-place.
  bool isInstantiableOnce = pattern->getNumOperands() == 0
    && !pattern->getGenericSignature();

  // Collect the required parameters for the keypath's generic environment.
  SmallVector<GenericRequirement, 4> requirements;
  
  auto *genericEnv = pattern->getGenericSignature().getGenericEnvironment();
  enumerateGenericSignatureRequirements(pattern->getGenericSignature(),
    [&](GenericRequirement reqt) { requirements.push_back(reqt); });

  // Start building the key path pattern.
  ConstantInitBuilder builder(*this);
  ConstantStructBuilder fields = builder.beginStruct();
  fields.setPacked(true);
  // If the pattern has no parameterization, add a pointer to a cache variable
  // that can be used for the one-time initialization of the key path.
  if (isInstantiableOnce) {
    auto onceVar = new llvm::GlobalVariable(Module, OnceTy,
                                            /*constant*/ false,
                                            llvm::GlobalValue::PrivateLinkage,
                                            llvm::ConstantInt::get(OnceTy, 0),
                                            "keypath_once");
    onceVar->setAlignment(llvm::MaybeAlign(getPointerAlignment().getValue()));
    fields.addRelativeAddress(onceVar);
  } else {
    fields.addInt32(0);
  }

  // Add the generic environment.
  fields.addRelativeAddressOrNull(
    getAddrOfGenericEnvironment(pattern->getGenericSignature()));
  // Store type references for the root and leaf.
  fields.addRelativeAddress(
    emitMetadataTypeRefForKeyPath(*this, rootTy,
                                  pattern->getGenericSignature()));
  fields.addRelativeAddress(
    emitMetadataTypeRefForKeyPath(*this, valueTy,
                                  pattern->getGenericSignature()));
  
  // Add a pointer to the ObjC KVC compatibility string, if there is one, or
  // null otherwise.
  if (!pattern->getObjCString().empty()) {
    auto objcString = getAddrOfGlobalString(pattern->getObjCString(),
                                            /*relatively addressed*/ true);
    fields.addRelativeAddress(objcString);
  } else {
    fields.addInt32(0);
  }
  
  // Leave a placeholder for the buffer header, since we need to know the full
  // buffer size to fill it in.
  auto headerPlaceholder = fields.addPlaceholderWithSize(Int32Ty);
  auto startOfKeyPathBuffer = fields.getNextOffsetFromGlobal();
  
  // Build out the components.
  auto baseTy = rootTy;
  
  // Collect the order and types of any captured index operands, which will
  // determine the layout of the buffer that gets passed to the initializer
  // for each component.
  SmallVector<KeyPathIndexOperand, 4> operands;
  operands.resize(pattern->getNumOperands());
  for (auto &component : pattern->getComponents()) {
    switch (component.getKind()) {
    case KeyPathPatternComponent::Kind::GettableProperty:
    case KeyPathPatternComponent::Kind::SettableProperty:
      for (auto &index : component.getSubscriptIndices()) {
        operands[index.Operand].LoweredType = index.LoweredType;
        operands[index.Operand].LastUser = &component;
      }
      break;
    case KeyPathPatternComponent::Kind::StoredProperty:
    case KeyPathPatternComponent::Kind::OptionalChain:
    case KeyPathPatternComponent::Kind::OptionalForce:
    case KeyPathPatternComponent::Kind::OptionalWrap:
    case KeyPathPatternComponent::Kind::TupleElement:
      break;
    }
  }
  
  for (unsigned i : indices(pattern->getComponents())) {
    auto &component = pattern->getComponents()[i];
    
    emitKeyPathComponent(*this, fields, component, isInstantiableOnce,
                         genericEnv, requirements,
                         baseTy, operands,
                         !component.getSubscriptIndices().empty());
    
    // For all but the last component, we pack in the type of the component.
    if (i + 1 != pattern->getComponents().size()) {
      fields.addRelativeAddress(
        emitMetadataTypeRefForKeyPath(*this,
                                      component.getComponentType(),
                                      pattern->getGenericSignature()));
    }
    baseTy = component.getComponentType();
  }
  
  // Save the total size of the buffer.
  Size componentSize = fields.getNextOffsetFromGlobal()
    - startOfKeyPathBuffer;
  
  // We now have enough info to build the header.
  KeyPathBufferHeader header(componentSize.getValue(), isInstantiableOnce,
                             /*reference prefix*/ false);
  // Add the header, followed by the components.
  fields.fillPlaceholder(headerPlaceholder,
                         llvm::ConstantInt::get(Int32Ty, header.getData()));
  
  // Create the global variable.
  // TODO: The pattern could be immutable if
  // it isn't instantiable in place, and if we made the type metadata accessor
  // references private, it could go in true-const memory.
  auto patternVar = fields.finishAndCreateGlobal("keypath",
                                          getPointerAlignment(),
                                          /*constant*/ false,
                                          llvm::GlobalVariable::PrivateLinkage);
  setTrueConstGlobal(patternVar);
  KeyPathPatterns.insert({pattern, patternVar});
  return patternVar;
}

void IRGenModule::emitSILProperty(SILProperty *prop) {
  if (prop->isTrivial()) {
    ++NumTrivialPropertyDescriptors;
    // All trivial property descriptors can share a single definition in the
    // translation unit.
    if (!TheTrivialPropertyDescriptor) {
      // Emit a definition if we don't have one yet.
      ConstantInitBuilder builder(*this);
      ConstantStructBuilder fields = builder.beginStruct();
      fields.addInt32(
        _SwiftKeyPathComponentHeader_TrivialPropertyDescriptorMarker);
      auto var = cast<llvm::GlobalVariable>(
        getAddrOfPropertyDescriptor(prop->getDecl(),
                                    fields.finishAndCreateFuture()));
      var->setConstant(true);
      var->setUnnamedAddr(llvm::GlobalValue::UnnamedAddr::Global);
      var->setAlignment(llvm::MaybeAlign(4));

      TheTrivialPropertyDescriptor = var;
    } else {
      auto entity = LinkEntity::forPropertyDescriptor(prop->getDecl());
      auto linkInfo = LinkInfo::get(*this, entity, ForDefinition);
      auto *GA = llvm::GlobalAlias::create(linkInfo.getLinkage(),
                                           linkInfo.getName(),
                                           TheTrivialPropertyDescriptor);
      ApplyIRLinkage({linkInfo.getLinkage(),
                      linkInfo.getVisibility(),
                      IRGen.Opts.InternalizeSymbols
                          ? llvm::GlobalValue::DefaultStorageClass
                          : llvm::GlobalValue::DLLExportStorageClass})
          .to(GA, linkInfo.isForDefinition());
    }
    return;
  }

  ++NumNonTrivialPropertyDescriptors;

  ConstantInitBuilder builder(*this);
  ConstantStructBuilder fields = builder.beginStruct();
  fields.setPacked(true);
  
  bool hasSubscriptIndices = false;
  bool isInstantiableInPlace = true;
  if (prop->getDecl()->getInnermostDeclContext()->isGenericContext()) {
    isInstantiableInPlace = false;
  }
  
  if (auto subscript = dyn_cast<SubscriptDecl>(prop->getDecl())) {
    hasSubscriptIndices = subscript->getIndices()->size() != 0;
    isInstantiableInPlace &= !hasSubscriptIndices;
  }
  
  auto genericEnv = prop->getDecl()->getInnermostDeclContext()
                        ->getGenericEnvironmentOfContext();
  SmallVector<GenericRequirement, 4> requirements;
  CanGenericSignature genericSig;
  if (genericEnv) {
    genericSig = prop->getDecl()
                     ->getInnermostDeclContext()
                     ->getGenericSignatureOfContext()
                     .getCanonicalSignature();
    enumerateGenericSignatureRequirements(genericSig,
      [&](GenericRequirement reqt) { requirements.push_back(reqt); });
  }
  
  emitKeyPathComponent(*this, fields, *prop->getComponent(),
                       isInstantiableInPlace, genericEnv, requirements,
                       prop->getDecl()->getInnermostDeclContext()
                                      ->getInnermostTypeContext()
                                      ->getSelfInterfaceType()
                                      ->getReducedType(genericSig),
                       {},
                       hasSubscriptIndices);
  
  auto var = cast<llvm::GlobalVariable>(
    getAddrOfPropertyDescriptor(prop->getDecl(),
                                fields.finishAndCreateFuture()));
  var->setConstant(true);
  var->setUnnamedAddr(llvm::GlobalValue::UnnamedAddr::Global);
  var->setAlignment(llvm::MaybeAlign(4));
}
