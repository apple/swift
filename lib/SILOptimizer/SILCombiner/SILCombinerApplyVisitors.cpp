//===--- SILCombinerApplyVisitors.cpp -------------------------------------===//
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

#define DEBUG_TYPE "sil-combine"

#include "SILCombiner.h"

#include "swift/AST/GenericSignature.h"
#include "swift/AST/Module.h"
#include "swift/AST/SemanticAttrs.h"
#include "swift/AST/SubstitutionMap.h"
#include "swift/Basic/Range.h"
#include "swift/SIL/DebugUtils.h"
#include "swift/SIL/DynamicCasts.h"
#include "swift/SIL/InstructionUtils.h"
#include "swift/SIL/NodeBits.h"
#include "swift/SIL/PatternMatch.h"
#include "swift/SIL/SILBuilder.h"
#include "swift/SIL/SILVisitor.h"
#include "swift/SILOptimizer/Analysis/ARCAnalysis.h"
#include "swift/SILOptimizer/Analysis/AliasAnalysis.h"
#include "swift/SILOptimizer/Analysis/ValueTracking.h"
#include "swift/SILOptimizer/Utils/CFGOptUtils.h"
#include "swift/SILOptimizer/Utils/Existential.h"
#include "swift/SILOptimizer/Utils/KeyPathProjector.h"
#include "swift/SILOptimizer/Utils/OwnershipOptUtils.h"
#include "swift/SILOptimizer/Utils/ValueLifetime.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/Statistic.h"
#include <utility>

using namespace swift;
using namespace swift::PatternMatch;

STATISTIC(NumOptimizedKeypaths, "Number of optimized keypath instructions");

/// Remove pointless reabstraction thunk closures.
///   partial_apply %reabstraction_thunk_typeAtoB(
///      partial_apply %reabstraction_thunk_typeBtoA %closure_typeB))
///   ->
///   %closure_typeB
static bool foldInverseReabstractionThunks(PartialApplyInst *PAI,
                                           SILCombiner *Combiner) {
  auto PAIArg = isPartialApplyOfReabstractionThunk(PAI);
  if (!PAIArg)
    return false;

  auto *PAI2 = dyn_cast<PartialApplyInst>(PAIArg);
  if (!PAI2)
    return false;

  if (!hasOneNonDebugUse(PAI2))
    return false;

  auto PAI2Arg = isPartialApplyOfReabstractionThunk(PAI2);
  if (!PAI2Arg)
    return false;

  // The types must match.
  if (PAI->getType() != PAI2->getArgument(0)->getType())
    return false;

  // Replace the partial_apply(partial_apply(X)) by X and remove the
  // partial_applies.

  Combiner->replaceInstUsesWith(*PAI, PAI2->getArgument(0));
  Combiner->eraseInstFromFunction(*PAI);
  assert(onlyHaveDebugUses(PAI2) && "Should not have any uses");
  Combiner->eraseInstFromFunction(*PAI2);

  return true;
}

SILInstruction *SILCombiner::visitPartialApplyInst(PartialApplyInst *pai) {
  // partial_apply without any substitutions or arguments is just a
  // thin_to_thick_function. thin_to_thick_function supports only thin operands.
  if (!pai->hasSubstitutions() && (pai->getNumArguments() == 0) &&
      pai->getSubstCalleeType()->getRepresentation() ==
          SILFunctionTypeRepresentation::Thin) {
    if (!pai->isOnStack())
      return Builder.createThinToThickFunction(pai->getLoc(), pai->getCallee(),
                                               pai->getType());

    // Remove dealloc_stack of partial_apply [stack].
    // Iterating while delete use a copy.
    SmallVector<Operand *, 8> uses(pai->getUses());
    for (auto *use : uses)
      if (auto *dealloc = dyn_cast<DeallocStackInst>(use->getUser()))
        eraseInstFromFunction(*dealloc);
    auto *thinToThick = Builder.createThinToThickFunction(
        pai->getLoc(), pai->getCallee(), pai->getType());
    replaceInstUsesWith(*pai, thinToThick);
    eraseInstFromFunction(*pai);
    return nullptr;
  }

  // partial_apply %reabstraction_thunk_typeAtoB(
  //    partial_apply %reabstraction_thunk_typeBtoA %closure_typeB))
  // -> %closure_typeB
  if (foldInverseReabstractionThunks(pai, this))
    return nullptr;

  bool argsAreKeptAlive = tryOptimizeApplyOfPartialApply(
      pai, Builder.getBuilderContext(), getInstModCallbacks());
  if (argsAreKeptAlive)
    invalidatedStackNesting = true;

  // Try to delete the partial_apply.
  // In case it became dead because of tryOptimizeApplyOfPartialApply, we don't
  // need to copy all arguments again (to extend their lifetimes), because it
  // was already done in tryOptimizeApplyOfPartialApply.
  if (tryDeleteDeadClosure(pai, getInstModCallbacks(), !argsAreKeptAlive))
    invalidatedStackNesting = true;

  return nullptr;
}

SILInstruction *
SILCombiner::optimizeApplyOfConvertFunctionInst(FullApplySite AI,
                                                ConvertFunctionInst *CFI) {
  // We only handle simplification of static function references. If we don't
  // have one, bail.
  SILValue funcOper = CFI->getOperand();
  if (auto *TTI = dyn_cast<ThinToThickFunctionInst>(funcOper))
    funcOper = TTI->getOperand();

  if (!isa<FunctionRefInst>(funcOper) &&
      // Optimizing partial_apply will then enable the partial_apply -> apply peephole.
      !isa<PartialApplyInst>(funcOper))
    return nullptr;

  // Grab our relevant callee types...
  CanSILFunctionType SubstCalleeTy = AI.getSubstCalleeType();
  auto ConvertCalleeTy = funcOper->getType().castTo<SILFunctionType>();

  // ... and make sure they have no unsubstituted generics. If they do, bail.
  if (SubstCalleeTy->hasArchetype() || ConvertCalleeTy->hasArchetype())
    return nullptr;

  // Ok, we can now perform our transformation. Grab AI's operands and the
  // relevant types from the ConvertFunction function type and AI.
  Builder.setCurrentDebugScope(AI.getDebugScope());
  OperandValueArrayRef Ops = AI.getArguments();
  SILFunctionConventions substConventions(SubstCalleeTy, CFI->getModule());
  SILFunctionConventions convertConventions(ConvertCalleeTy, CFI->getModule());
  auto context = AI.getFunction()->getTypeExpansionContext();
  auto oldOpRetTypes = substConventions.getIndirectSILResultTypes(context);
  auto newOpRetTypes = convertConventions.getIndirectSILResultTypes(context);
  auto oldOpParamTypes = substConventions.getParameterSILTypes(context);
  auto newOpParamTypes = convertConventions.getParameterSILTypes(context);

  llvm::SmallVector<SILValue, 8> Args;
  auto convertOp = [&](SILValue Op, SILType OldOpType, SILType NewOpType) {
    // Convert function takes refs to refs, address to addresses, and leaves
    // other types alone.
    if (OldOpType.isAddress()) {
      assert(NewOpType.isAddress() && "Addresses should map to addresses.");
      auto UAC = Builder.createUncheckedAddrCast(AI.getLoc(), Op, NewOpType);
      Args.push_back(UAC);
    } else if (OldOpType.getASTType() != NewOpType.getASTType()) {
      auto URC =
          Builder.createUncheckedForwardingCast(AI.getLoc(), Op, NewOpType);
      Args.push_back(URC);
    } else {
      Args.push_back(Op);
    }
  };

  unsigned OpI = 0;
  
  auto newRetI = newOpRetTypes.begin();
  auto oldRetI = oldOpRetTypes.begin();
  
  for (auto e = newOpRetTypes.end(); newRetI != e;
       ++OpI, ++newRetI, ++oldRetI) {
    convertOp(Ops[OpI], *oldRetI, *newRetI);
  }
  
  auto newParamI = newOpParamTypes.begin();
  auto oldParamI = oldOpParamTypes.begin();
  for (auto e = newOpParamTypes.end(); newParamI != e;
       ++OpI, ++newParamI, ++oldParamI) {
    convertOp(Ops[OpI], *oldParamI, *newParamI);
  }

  // Convert the direct results if they changed.
  auto oldResultTy = SubstCalleeTy
    ->getDirectFormalResultsType(AI.getModule(),
                                 AI.getFunction()->getTypeExpansionContext());
  auto newResultTy = ConvertCalleeTy
    ->getDirectFormalResultsType(AI.getModule(),
                                 AI.getFunction()->getTypeExpansionContext());
  
  // Create the new apply inst.
  if (auto *TAI = dyn_cast<TryApplyInst>(AI)) {
    // If the results need to change, create a new landing block to do that
    // conversion.
    auto normalBB = TAI->getNormalBB();
    if (oldResultTy != newResultTy) {
      normalBB = AI.getFunction()->createBasicBlockBefore(TAI->getNormalBB());
      Builder.setInsertionPoint(normalBB);
      SmallVector<SILValue, 4> branchArgs;
      
      auto oldOpResultTypes = substConventions.getDirectSILResultTypes(context);
      auto newOpResultTypes = convertConventions.getDirectSILResultTypes(context);
      
      auto oldRetI = oldOpResultTypes.begin();
      auto newRetI = newOpResultTypes.begin();
      auto origArgs = TAI->getNormalBB()->getArguments();
      auto origArgI = origArgs.begin();
      for (auto e = newOpResultTypes.end(); newRetI != e;
           ++oldRetI, ++newRetI, ++origArgI) {
        auto arg = normalBB->createPhiArgument(*newRetI, (*origArgI)->getOwnershipKind());
        auto converted =
          Builder.createUncheckedForwardingCast(AI.getLoc(), arg, *oldRetI);
        branchArgs.push_back(converted);
      }
      
      Builder.createBranch(AI.getLoc(), TAI->getNormalBB(), branchArgs);
    }
    
    return Builder.createTryApply(AI.getLoc(), funcOper, SubstitutionMap(), Args,
                                  normalBB, TAI->getErrorBB(),
                                  TAI->getApplyOptions());
  }

  // Match the throwing bit of the underlying function_ref. We assume that if
  // we got this far it is legal to perform the transformation (since
  // otherwise, we would be creating malformed SIL).
  ApplyOptions Options = AI.getApplyOptions();
  Options -= ApplyFlags::DoesNotThrow;
  if (funcOper->getType().castTo<SILFunctionType>()->hasErrorResult())
    Options |= ApplyFlags::DoesNotThrow;
  ApplyInst *NAI = Builder.createApply(AI.getLoc(), funcOper, SubstitutionMap(),
                                       Args, Options);
  SILInstruction *result = NAI;
  
  if (oldResultTy != newResultTy) {
    result =
      Builder.createUncheckedForwardingCast(AI.getLoc(), NAI, oldResultTy);
  }
  
  return result;
}

/// Try to optimize a keypath application with an apply instruction.
///
/// Replaces (simplified SIL):
///   %kp = keypath ...
///   apply %keypath_runtime_function(%addr, %kp, %root_object)
/// with:
///   %addr = struct_element_addr/ref_element_addr %root_object
///   ...
///   load/store %addr
bool SILCombiner::tryOptimizeKeypathApplication(ApplyInst *AI,
                                          SILFunction *callee) {
  if (AI->getNumArguments() != 3)
    return false;

  SILValue keyPath, rootAddr, valueAddr;
  bool isSet = false;
  if (callee->getName() == "swift_setAtWritableKeyPath" ||
      callee->getName() == "swift_setAtReferenceWritableKeyPath") {
    keyPath = AI->getArgument(1);
    rootAddr = AI->getArgument(0);
    valueAddr = AI->getArgument(2);
    isSet = true;
  } else if (callee->getName() == "swift_getAtKeyPath") {
    keyPath = AI->getArgument(2);
    rootAddr = AI->getArgument(1);
    valueAddr = AI->getArgument(0);
  } else {
    return false;
  }
  
  auto projector = KeyPathProjector::create(keyPath, rootAddr,
                                            AI->getLoc(), Builder);
  if (!projector)
    return false;
  
  KeyPathProjector::AccessType accessType;
  if (isSet) accessType = KeyPathProjector::AccessType::Set;
  else accessType = KeyPathProjector::AccessType::Get;
  
  projector->project(accessType, [&](SILValue projectedAddr) {
    if (isSet) {
      Builder.createCopyAddr(AI->getLoc(), valueAddr, projectedAddr,
                             IsTake, IsInitialization);
    } else {
      Builder.createCopyAddr(AI->getLoc(), projectedAddr, valueAddr,
                             IsNotTake, IsInitialization);
    }
  });
  
  eraseInstFromFunction(*AI);
  ++NumOptimizedKeypaths;
  return true;
}

/// Replaces a call of the getter of AnyKeyPath._storedInlineOffset with a
/// "constant" offset, in case of a keypath literal.
///
/// "Constant" offset means a series of struct_element_addr and
/// tuple_element_addr instructions with a 0-pointer as base address.
/// These instructions can then be lowered to "real" constants in IRGen for
/// concrete types, or to metatype offset lookups for generic or resilient types.
///
/// Replaces:
///   %kp = keypath ...
///   %offset = apply %_storedInlineOffset_method(%kp)
/// with:
///   %zero = integer_literal $Builtin.Word, 0
///   %null_ptr = unchecked_trivial_bit_cast %zero to $Builtin.RawPointer
///   %null_addr = pointer_to_address %null_ptr
///   %projected_addr = struct_element_addr %null_addr
///    ... // other address projections
///   %offset_ptr = address_to_pointer %projected_addr
///   %offset_builtin_int = unchecked_trivial_bit_cast %offset_ptr
///   %offset_int = struct $Int (%offset_builtin_int)
///   %offset = enum $Optional<Int>, #Optional.some!enumelt, %offset_int
bool SILCombiner::tryOptimizeKeypathOffsetOf(ApplyInst *AI,
                                             FuncDecl *calleeFn,
                                             KeyPathInst *kp) {
  auto *accessor = dyn_cast<AccessorDecl>(calleeFn);
  if (!accessor || !accessor->isGetter())
    return false;

  AbstractStorageDecl *storage = accessor->getStorage();
  DeclName name = storage->getName();
  if (!name.isSimpleName() ||
      (name.getBaseIdentifier().str() != "_storedInlineOffset"))
    return false;

  KeyPathPattern *pattern = kp->getPattern();
  SubstitutionMap patternSubs = kp->getSubstitutions();
  SILFunction *f = AI->getFunction();
  SILType rootTy = f->getLoweredType(Lowering::AbstractionPattern::getOpaque(),
      pattern->getRootType().subst(patternSubs)->getCanonicalType());

  SILType parentTy = rootTy;
  
  // First check if _storedInlineOffset would return an offset or nil. Basically
  // only stored struct and tuple elements produce an offset. Everything else
  // (e.g. computed properties, class properties) result in nil.
  bool hasOffset = true;
  for (const KeyPathPatternComponent &component : pattern->getComponents()) {
    switch (component.getKind()) {
    case KeyPathPatternComponent::Kind::StoredProperty: {

      // Handle the special case of C tail-allocated arrays. IRGen would
      // generate an undef offset for struct_element_addr of C tail-allocated
      // arrays.
      VarDecl *propDecl = component.getStoredPropertyDecl();
      if (propDecl->hasClangNode() && propDecl->getInterfaceType()->isVoid())
        return false;

      if (!parentTy.getStructOrBoundGenericStruct())
        hasOffset = false;
      break;
    }
    case KeyPathPatternComponent::Kind::TupleElement:
      break;
    case KeyPathPatternComponent::Kind::GettableProperty:
    case KeyPathPatternComponent::Kind::SettableProperty:
      // We cannot predict the offset of fields in resilient types, because it's
      // unknown if a resilient field is a computed or stored property.
      if (component.getExternalDecl())
        return false;
      hasOffset = false;
      break;
    case KeyPathPatternComponent::Kind::OptionalChain:
    case KeyPathPatternComponent::Kind::OptionalForce:
    case KeyPathPatternComponent::Kind::OptionalWrap:
      hasOffset = false;
      break;
    }
    parentTy = f->getLoweredType(Lowering::AbstractionPattern::getOpaque(),
                                 component.getComponentType());
  }

  SILLocation loc = AI->getLoc();
  SILValue result;

  if (hasOffset) {
    SILType rootAddrTy = rootTy.getAddressType();
    SILValue rootAddr = Builder.createBaseAddrForOffset(loc, rootAddrTy);

    auto projector = KeyPathProjector::create(kp, rootAddr, loc, Builder);
    if (!projector)
      return false;

    // Create the address projections of the keypath.
    SILType ptrType = SILType::getRawPointerType(Builder.getASTContext());
    SILValue offsetPtr;
    projector->project(KeyPathProjector::AccessType::Get, [&](SILValue addr) {
      offsetPtr = Builder.createAddressToPointer(loc, addr, ptrType,
                                          /*needsStackProtection=*/ false);
    });

    // The result of the _storedInlineOffset call should be Optional<Int>. If
    // not, something is wrong with the stdlib. Anyway, if it's not like we
    // expect, bail.
    SILType intType = AI->getType().getOptionalObjectType();
    if (!intType)
      return false;
    StructDecl *intDecl = intType.getStructOrBoundGenericStruct();
    if (!intDecl || intDecl->getStoredProperties().size() != 1)
      return false;
    VarDecl *member = intDecl->getStoredProperties()[0];
    CanType builtinIntTy = member->getInterfaceType()->getCanonicalType();
    if (!isa<BuiltinIntegerType>(builtinIntTy))
      return false;

    // Convert the projected address back to an optional integer.
    SILValue offset = Builder.createUncheckedReinterpretCast(
        loc, offsetPtr, SILType::getPrimitiveObjectType(builtinIntTy));
    SILValue offsetInt = Builder.createStruct(loc, intType, { offset });
    result = Builder.createOptionalSome(loc, offsetInt, AI->getType());
  } else {
    // The keypath has no offset.
    result = Builder.createOptionalNone(loc, AI->getType());
  }
  AI->replaceAllUsesWith(result);
  eraseInstFromFunction(*AI);
  ++NumOptimizedKeypaths;
  return true;
}

/// Try to optimize a keypath KVC string access on a literal key path.
///
/// Replace:
///   %kp = keypath (objc "blah", ...)
///   %string = apply %keypath_kvcString_method(%kp)
/// With:
///   %string = string_literal "blah"
bool SILCombiner::tryOptimizeKeypathKVCString(ApplyInst *AI,
                                              FuncDecl *calleeFn,
                                              KeyPathInst *kp) {
  if (!calleeFn->getAttrs()
        .hasSemanticsAttr(semantics::KEYPATH_KVC_KEY_PATH_STRING))
    return false;
  
  // Method should return `String?`
  auto &C = calleeFn->getASTContext();
  auto objTy = AI->getType().getOptionalObjectType();
  if (!objTy || !objTy.getASTType()->isString())
    return false;
  
  auto objcString = kp->getPattern()->getObjCString();
  
  SILValue literalValue;
  if (objcString.empty()) {
    // Replace with a nil String value.
    literalValue = Builder.createEnum(AI->getLoc(), SILValue(),
                                      C.getOptionalNoneDecl(),
                                      AI->getType());
  } else {
    // Construct a literal String from the ObjC string.
    auto init = C.getStringBuiltinInitDecl(C.getStringDecl());
    if (!init)
      return false;
    auto initRef = SILDeclRef(init.getDecl(), SILDeclRef::Kind::Allocator);
    auto initFn = AI->getModule().loadFunction(initRef.mangle(),
                                               SILModule::LinkingMode::LinkAll);
    if (!initFn)
      return false;

    auto stringValue = Builder.createStringLiteral(AI->getLoc(), objcString,
                                             StringLiteralInst::Encoding::UTF8);
    auto stringLen = Builder.createIntegerLiteral(AI->getLoc(),
                                                SILType::getBuiltinWordType(C),
                                                objcString.size());
    auto isAscii = Builder.createIntegerLiteral(AI->getLoc(),
                                          SILType::getBuiltinIntegerType(1, C),
                                          C.isASCIIString(objcString));
    auto metaTy =
      CanMetatypeType::get(objTy.getASTType(), MetatypeRepresentation::Thin);
    auto self = Builder.createMetatype(AI->getLoc(),
                                     SILType::getPrimitiveObjectType(metaTy));
    
    auto initFnRef = Builder.createFunctionRef(AI->getLoc(), initFn);
    auto string = Builder.createApply(AI->getLoc(),
                                      initFnRef, {},
                                      {stringValue, stringLen, isAscii, self});
    
    literalValue = Builder.createEnum(AI->getLoc(), string,
                                      C.getOptionalSomeDecl(), AI->getType());
  }

  AI->replaceAllUsesWith(literalValue);
  eraseInstFromFunction(*AI);
  ++NumOptimizedKeypaths;
  return true;
}

bool SILCombiner::tryOptimizeKeypath(ApplyInst *AI) {
  if (SILFunction *callee = AI->getReferencedFunctionOrNull()) {
    return tryOptimizeKeypathApplication(AI, callee);
  }
  
  // Try optimize keypath method calls.
  auto *methodInst = dyn_cast<ClassMethodInst>(AI->getCallee());
  if (!methodInst)
    return false;
  
  if (AI->getNumArguments() != 1) {
    return false;
  }

  SILDeclRef callee = methodInst->getMember();
  if (!callee.hasDecl()) {
    return false;
  }
  auto *calleeFn = dyn_cast<FuncDecl>(callee.getDecl());
  if (!calleeFn)
    return false;

  KeyPathInst *kp = KeyPathProjector::getLiteralKeyPath(AI->getArgument(0));
  if (!kp || !kp->hasPattern())
    return false;
  
  if (tryOptimizeKeypathOffsetOf(AI, calleeFn, kp))
    return true;

  if (tryOptimizeKeypathKVCString(AI, calleeFn, kp))
    return true;

  return false;
}

/// Try to optimize a keypath application with an apply instruction.
///
/// Replaces (simplified SIL):
///   %kp = keypath ...
///   %inout_addr = begin_apply %keypath_runtime_function(%kp, %root_object)
///   // use %inout_addr
///   end_apply
/// with:
///   %addr = struct_element_addr/ref_element_addr %root_object
///   // use %inout_addr
bool SILCombiner::tryOptimizeInoutKeypath(BeginApplyInst *AI) {
  // Disable in OSSA because KeyPathProjector is not fully ported
  if (AI->getFunction()->hasOwnership())
    return false;

  SILFunction *callee = AI->getReferencedFunctionOrNull();
  if (!callee)
    return false;

  if (AI->getNumArguments() != 2)
    return false;

  SILValue keyPath = AI->getArgument(1);
  SILValue rootAddr = AI->getArgument(0);
  bool isModify = false;
  if (callee->getName() == "swift_modifyAtWritableKeyPath" ||
      callee->getName() == "swift_modifyAtReferenceWritableKeyPath") {
    isModify = true;
  } else if (callee->getName() != "swift_readAtKeyPath") {
    return false;
  }

  SILInstructionResultArray yields = AI->getYieldedValues();
  if (yields.size() != 1)
    return false;

  SILValue valueAddr = yields[0];
  Operand *AIUse = AI->getTokenResult()->getSingleUse();
  if (!AIUse)
    return false;
  EndApplyInst *endApply = dyn_cast<EndApplyInst>(AIUse->getUser());
  if (!endApply)
    return false;
  
  auto projector = KeyPathProjector::create(keyPath, rootAddr,
                                            AI->getLoc(), Builder);
  if (!projector)
    return false;
    
  KeyPathProjector::AccessType accessType;
  if (isModify) accessType = KeyPathProjector::AccessType::Modify;
  else accessType = KeyPathProjector::AccessType::Get;
  
  projector->project(accessType, [&](SILValue projectedAddr) {
    // Replace the projected address.
    valueAddr->replaceAllUsesWith(projectedAddr);
    
    // Skip to the end of the key path application before cleaning up.
    Builder.setInsertionPoint(endApply);
  });

  eraseInstFromFunction(*endApply);
  eraseInstFromFunction(*AI);
  ++NumOptimizedKeypaths;
  return true;
}

bool
SILCombiner::recursivelyCollectARCUsers(UserListTy &Uses, ValueBase *Value) {
  // FIXME: We could probably optimize this case too
  if (auto *AI = dyn_cast<ApplyInst>(Value))
    if (AI->hasIndirectResults())
      return false;

  for (auto *Use : Value->getUses()) {
    SILInstruction *Inst = Use->getUser();
    if (isa<RefCountingInst>(Inst) || isa<DestroyValueInst>(Inst) ||
        isa<DebugValueInst>(Inst) || isa<EndBorrowInst>(Inst)) {
      Uses.push_back(Inst);
      continue;
    }
    if (isa<TupleExtractInst>(Inst) || isa<StructExtractInst>(Inst) ||
        isa<CopyValueInst>(Inst) || isa<BeginBorrowInst>(Inst) ||
        isa<PointerToAddressInst>(Inst)) {
      Uses.push_back(Inst);
      if (recursivelyCollectARCUsers(Uses, cast<SingleValueInstruction>(Inst)))
        continue;
    }
    return false;
  }
  return true;
}

bool SILCombiner::eraseApply(FullApplySite FAS, const UserListTy &Users) {

  // Compute the places where we have to insert release-instructions for the
  // owned arguments. This must not be done before the result of the
  // apply is destroyed. Therefore we compute the lifetime of the apply-result.

  // TODO: this is not required anymore when we have ownership SIL. But with
  // the current SIL it can happen that the retain of a parameter is moved
  // _after_ the apply.
  // When we have ownership SIL we can just destroy the parameters at the apply
  // location.

  ValueLifetimeAnalysis VLA(FAS.getInstruction(), Users);
  ValueLifetimeAnalysis::Frontier Frontier;
  if (Users.empty()) {
    // If the call does not have any ARC-uses or if there is no return value at
    // all, we insert the argument release instructions right before the call.
    Frontier.push_back(FAS.getInstruction());
  } else {
    if (!VLA.computeFrontier(Frontier, ValueLifetimeAnalysis::DontModifyCFG))
      return false;
    // As we are extending the lifetimes of owned parameters, we have to make
    // sure that no dealloc_ref or dealloc_stack_ref instructions are
    // within this extended liferange.
    // It could be that the dealloc_ref is deallocating a parameter and then
    // we would have a release after the dealloc.
    if (VLA.containsDeallocRef(Frontier))
      return false;
  }

  // Release and destroy any owned or in-arguments.
  auto FuncType = FAS.getOrigCalleeType();
  assert(FuncType->getParameters().size() == FAS.getNumArguments() &&
         "mismatching number of arguments");
  for (SILInstruction *FrontierInst : Frontier) {
    Builder.setInsertionPoint(FrontierInst);
    for (int i = 0, e = FAS.getNumArguments(); i < e; ++i) {
      SILParameterInfo PI = FuncType->getParameters()[i];
      auto Arg = FAS.getArgument(i);
      switch (PI.getConvention()) {
        case ParameterConvention::Indirect_In:
        case ParameterConvention::Direct_Owned:
        case ParameterConvention::Pack_Owned:
          Builder.emitDestroyOperation(FAS.getLoc(), Arg);
          break;
        case ParameterConvention::Indirect_In_Guaranteed:
        case ParameterConvention::Indirect_Inout:
        case ParameterConvention::Indirect_InoutAliasable:
        case ParameterConvention::Direct_Unowned:
        case ParameterConvention::Direct_Guaranteed:
        case ParameterConvention::Pack_Guaranteed:
        case ParameterConvention::Pack_Inout:
          break;
      }
    }
  }

  // Erase all of the reference counting instructions (in reverse order to have
  // no dangling uses).
  for (auto rit = Users.rbegin(), re = Users.rend(); rit != re; ++rit)
    eraseInstFromFunction(**rit);

  // And the Apply itself.
  eraseInstFromFunction(*FAS.getInstruction());

  return true;
}

/// This routine replaces the old witness method inst with a new one.
void SILCombiner::replaceWitnessMethodInst(
    WitnessMethodInst *WMI, SILBuilderContext &BuilderCtx, CanType ConcreteType,
    const ProtocolConformanceRef ConformanceRef) {
  SILBuilderWithScope WMIBuilder(WMI, BuilderCtx);
  auto *NewWMI = WMIBuilder.createWitnessMethod(
      WMI->getLoc(), ConcreteType, ConformanceRef, WMI->getMember(),
      WMI->getType());
  WMI->replaceAllUsesWith(NewWMI);
  if (WMI->use_empty())
    eraseInstFromFunction(*WMI);
}

// This function determines concrete type of an opened existential argument
// using ProtocolConformanceAnalysis. The concrete type of the argument can be a
// class, struct, or an enum.
//
// If some ConcreteOpenedExistentialInfo is returned, then new cast instructions
// have already been added to Builder's tracking list. If the caller can't make
// real progress then it must reset the Builder.
llvm::Optional<ConcreteOpenedExistentialInfo>
SILCombiner::buildConcreteOpenedExistentialInfoFromSoleConformingType(
    Operand &ArgOperand) {
  SILInstruction *AI = ArgOperand.getUser();
  SILModule &M = AI->getModule();
  SILFunction *F = AI->getFunction();

  // SoleConformingType is only applicable in whole-module compilation.
  if (!M.isWholeModule())
    return llvm::None;

  // Determine the protocol.
  ProtocolDecl *PD = nullptr;
  WitnessMethodInst *WMI = nullptr;
  FullApplySite FAS = FullApplySite::isa(AI);
  if (FAS && (WMI = dyn_cast<WitnessMethodInst>(FAS.getCallee())) &&
      (FAS.getSelfArgumentOperand().get()  == ArgOperand.get())) {
    // If the witness method mutates self, we cannot replace self.
    //
    // FIXME: Remove this out-dated check for mutating self. canReplaceCopiedArg
    // is supposed to handle this case.
    if (FAS.getOrigCalleeType()->getSelfParameter().isIndirectMutating())
      return llvm::None;
    PD = WMI->getLookupProtocol();
  } else {
    auto ArgType = ArgOperand.get()->getType();
    auto SwiftArgType = ArgType.getASTType();
    /// If the argtype is an opened existential conforming to a protocol type
    /// and that the protocol type has a sole conformance, then we can propagate
    /// concrete type for it as well.
    ArchetypeType *archetypeTy;
    if (SwiftArgType->isOpenedExistential() &&
        (archetypeTy = dyn_cast<ArchetypeType>(SwiftArgType)) &&
        (archetypeTy->getConformsTo().size() == 1)) {
      PD = archetypeTy->getConformsTo()[0];
    } else if (ArgType.isExistentialType() && !ArgType.isAnyObject() &&
               !SwiftArgType->isAny()) {
      PD = dyn_cast_or_null<ProtocolDecl>(SwiftArgType->getAnyNominal());
    }
  }

  if (!PD)
    return llvm::None;

  // Determine the sole conforming type.
  CanType ConcreteType;
  if (!PCA->getSoleConformingType(PD, CHA, ConcreteType))
    return llvm::None;

  // Determine OpenedArchetypeDef and SubstitutionMap.
  ConcreteOpenedExistentialInfo COAI(ArgOperand, ConcreteType, PD);
  if (!COAI.CEI)
    return llvm::None;

  const OpenedArchetypeInfo &OAI = COAI.OAI;
  ConcreteExistentialInfo &SoleCEI = *COAI.CEI;
  assert(SoleCEI.isValid());

  if (SoleCEI.ConcreteValue)
    return COAI;

  // Create SIL type for the concrete type.
  SILType concreteSILType = F->getLoweredType(ConcreteType);

  // Prepare the code by adding UncheckedCast instructions that cast opened
  // existentials to concrete types. Set the ConcreteValue of CEI.
  if (auto *OER = dyn_cast<OpenExistentialRefInst>(OAI.OpenedArchetypeValue)) {
    // If we have an owned open_existential_ref, we only optimize for now if our
    // open_existential_ref has a single non-debug consuming use that is a
    // destroy_value.
    if (OER->getForwardingOwnershipKind() != OwnershipKind::Owned) {
      // We use OER as the insertion point so that
      SILBuilderWithScope b(std::next(OER->getIterator()), Builder);
      auto loc = RegularLocation::getAutoGeneratedLocation();
      SoleCEI.ConcreteValue =
          b.createUncheckedRefCast(loc, OER, concreteSILType);
      return COAI;
    }

    auto *consumingUse = OER->getSingleConsumingUse();
    if (!consumingUse || !isa<DestroyValueInst>(consumingUse->getUser())) {
      return llvm::None;
    }

    // We use std::next(OER) as the insertion point so that we can reuse the
    // destroy_value of consumingUse.
    SILBuilderWithScope b(std::next(OER->getIterator()), Builder);
    auto loc = RegularLocation::getAutoGeneratedLocation();
    auto *uri = b.createUncheckedRefCast(loc, OER, concreteSILType);
    SoleCEI.ConcreteValue = uri;
    replaceInstUsesWith(*OER, uri);
    return COAI;
  }

  if (auto *OEA = dyn_cast<OpenExistentialAddrInst>(OAI.OpenedArchetypeValue)) {
    // Bail if ConcreteSILType is not the same SILType as the type stored in the
    // existential after maximal reabstraction.
    auto abstractionPattern = Lowering::AbstractionPattern::getOpaque();
    auto abstractTy = F->getLoweredType(abstractionPattern, ConcreteType);
    if (abstractTy != concreteSILType)
      return llvm::None;

    SoleCEI.ConcreteValue =
      Builder.createUncheckedAddrCast(
        OEA->getLoc(), OEA, concreteSILType.getAddressType());
    return COAI;
  }
  // Bail if OpenArchetypeInfo recognizes any additional opened archetype
  // producers. This shouldn't be hit currently because metatypes don't
  // conform to protocols.
  return llvm::None;
}

// This function builds a ConcreteExistentialInfo by first following the data
// flow chain from the ArgOperand. Otherwise, we check if the operand is of
// protocol type that conforms to a single concrete type.
llvm::Optional<ConcreteOpenedExistentialInfo>
SILCombiner::buildConcreteOpenedExistentialInfo(Operand &ArgOperand) {
  // Build a ConcreteOpenedExistentialInfo following the data flow chain of the
  // ArgOperand through the open_existential backward to an init_existential.
  ConcreteOpenedExistentialInfo COEI(ArgOperand);
  if (COEI.CEI)
    return COEI;

  // Use SoleConformingType information.
  return buildConcreteOpenedExistentialInfoFromSoleConformingType(ArgOperand);
}

// Build ConcreteExistentialInfo for every existential argument of an Apply
// instruction including Self.
void SILCombiner::buildConcreteOpenedExistentialInfos(
    FullApplySite Apply,
    llvm::SmallDenseMap<unsigned, ConcreteOpenedExistentialInfo> &COEIs,
    SILBuilderContext &BuilderCtx) {
  for (unsigned ArgIdx = 0, e = Apply.getNumArguments(); ArgIdx < e;
       ++ArgIdx) {
    auto ArgASTType = Apply.getArgument(ArgIdx)->getType().getASTType();
    if (!ArgASTType->hasArchetype())
      continue;

    auto OptionalCOEI =
        buildConcreteOpenedExistentialInfo(Apply.getArgumentOperands()[ArgIdx]);
    if (!OptionalCOEI.has_value())
      continue;
    auto COEI = OptionalCOEI.value();
    assert(COEI.isValid());
    COEIs.try_emplace(ArgIdx, COEI);
  }
}

/// Given an Apply and an argument value produced by InitExistentialAddrInst,
/// return true if the argument can be replaced by a copy of its value.
///
/// FIXME: remove this helper when we can assume SIL opaque values.
static bool canReplaceCopiedArg(FullApplySite Apply, SILValue Arg,
                                DominanceAnalysis *DA, unsigned ArgIdx) {
  auto *IEA = dyn_cast<InitExistentialAddrInst>(Arg);
  // Only init_existential_addr may be copied.
  if (!IEA)
    return false;

  auto *DT = DA->get(Apply.getFunction());
  auto *AI = Apply.getInstruction();
  SILValue existentialAddr = IEA->getOperand();

  // If we peeked through an InitEnumDataAddr or some such, then don't assume we
  // can reuse the copied value. It's likely destroyed by
  // UncheckedTakeEnumDataInst before the copy.
  auto *ASI = dyn_cast<AllocStackInst>(existentialAddr);
  if (!ASI)
    return false;

  // Return true only if the given value is guaranteed to be initialized across
  // the given call site.
  //
  // It's possible for an address to be initialized/deinitialized/reinitialized.
  // Rather than keeping track of liveness, we very conservatively check that
  // all deinitialization occurs after the call.
  auto isDestroy = [](Operand *use) {
    switch (use->getUser()->getKind()) {
    default:
      return false;
    case SILInstructionKind::DestroyAddrInst:
    case SILInstructionKind::DeinitExistentialAddrInst:
      return true;
    case SILInstructionKind::CopyAddrInst: {
      auto *copy = cast<CopyAddrInst>(use->getUser());
      return copy->getSrc() == use->get() && copy->isTakeOfSrc();
    }
    }
  };
  for (auto use : existentialAddr->getUses()) {
    SILInstruction *user = use->getUser();
    if (isDestroy(use)) {
      if (!DT->properlyDominates(AI, user))
        return false;
    } else {
      // The caller has to guarantee that there are no other instructions which
      // use the address. This is done in findInitExistential called from
      // the constructor of ConcreteExistentialInfo.
      assert(isa<CopyAddrInst>(user) || isa<InitExistentialAddrInst>(user) ||
             isa<OpenExistentialAddrInst>(user) ||
             isa<DeallocStackInst>(user) ||
             isa<ApplyInst>(user) || isa<TryApplyInst>(user) ||
             user->isDebugInstruction() && "Unexpected instruction");
    }
  }
  return true;
}

/// Determine if the result type or argument types of the given apply, except
/// for the argument at \p SkipArgIdx, contain an opened archetype rooted
/// on \p RootOA.
static bool applyInvolvesOpenedArchetypeWithRoot(FullApplySite Apply,
                                                 OpenedArchetypeType *RootOA,
                                                 unsigned SkipArgIdx) {
  if (Apply.getType().getASTType()->hasOpenedExistentialWithRoot(RootOA)) {
    return true;
  }

  const auto NumApplyArgs = Apply.getNumArguments();
  for (unsigned Idx = 0; Idx < NumApplyArgs; ++Idx) {
    if (Idx == SkipArgIdx)
      continue;
    if (Apply.getArgument(Idx)
            ->getType()
            .getASTType()
            ->hasOpenedExistentialWithRoot(RootOA)) {
      return true;
    }
  }

  return false;
}

// Check the legal conditions under which a Arg parameter (specified as ArgIdx)
// can be replaced with a concrete type. Concrete type info is passed as CEI
// argument.
bool SILCombiner::canReplaceArg(FullApplySite Apply,
                                const OpenedArchetypeInfo &OAI,
                                const ConcreteExistentialInfo &CEI,
                                unsigned ArgIdx) {
  // Don't specialize apply instructions if the result type references
  // OpenedArchetype, because this optimization does not know how to substitute
  // types in the users of this apply. In the function type substitution below,
  // all references to OpenedArchetype will be substituted. So walk the type to
  // find all possible references, such as returning Optional<OpenedArchetype>.
  // The same holds for other arguments or indirect result that refer to the
  // OpenedArchetype, because the following optimization will rewrite only the
  // argument at ArgIdx.
  //
  // Note that the language does not allow Self to occur in contravariant
  // position. However, SIL does allow this and it can happen as a result of
  // upstream transformations. Since this is bail-out logic, it must handle
  // all verifiable SIL.
  if (applyInvolvesOpenedArchetypeWithRoot(Apply, OAI.OpenedArchetype,
                                           ArgIdx)) {
    return false;
  }

  // If the convention is mutating, then the existential must have been
  // initialized by copying the concrete value (regardless of whether
  // CEI.isConcreteValueCopied is true). Replacing the existential address with
  // the concrete address would result in mutation of the wrong object.
  auto origConv = Apply.getOrigCalleeConv();
  if (origConv.getParamInfoForSILArg(ArgIdx).isIndirectMutating())
    return false;

  // If either the initialized existential or opened existential was copied,
  // then check that the original value can be passed as the new argument.
  if (CEI.isConcreteValueCopied
      && (!CEI.ConcreteValue
          || !canReplaceCopiedArg(Apply, CEI.ConcreteValue, DA, ArgIdx))) {
    return false;
  }
  // It is safe to replace Arg.
  return true;
}

/// Track temporary copies required for argument substitution when rewriting an
/// apply's argument types from an opened existential types to concrete types.
///
/// This is relevant for non-mutating arguments that are consumed by the call
/// (@in or @owned convention).
struct ConcreteArgumentCopy {
  SILValue origArg;
  AllocStackInst *tempArg;

  ConcreteArgumentCopy(SILValue origArg, AllocStackInst *tempArg)
      : origArg(origArg), tempArg(tempArg) {
    assert(origArg->getType().isAddress());
  }

  static llvm::Optional<ConcreteArgumentCopy>
  generate(const ConcreteExistentialInfo &existentialInfo, ApplySite apply,
           unsigned argIdx, SILBuilderContext &builderCtx) {
    SILParameterInfo paramInfo =
        apply.getOrigCalleeConv().getParamInfoForSILArg(argIdx);
    // Mutation should have been checked before we get this far.
    assert(!paramInfo.isIndirectMutating()
           && "A mutated opened existential value can't be replaced");

    if (!paramInfo.isConsumed())
      return llvm::None;

    SILValue origArg = apply.getArgument(argIdx);
    // TODO_sil_opaque: With SIL opaque values, a formally indirect argument
    // may be passed as a SIL object. In this case, generate a copy_value for
    // the new argument and a destroy_value for the old argument, as should
    // also be done for owned references.
    assert(origArg->getType().isAddress() == paramInfo.isFormalIndirect());

    // If argument convention is direct, then the existential reference was
    // originally consumed by the call. After substitution, the concrete
    // reference will be consumed by the call. This maintains the correct
    // reference count.
    //
    // FIXME_ownership: to maintain ownership SSA, generate a copy_value from
    // the concrete reference for the new argument (record this copy as a
    // union with tempArgCopy above). After emitting the apply, emit a
    // destroy_value of the existential, which is no longer consumed by the
    // call.
    if (!paramInfo.isFormalIndirect())
      return llvm::None;

    SILBuilderWithScope builder(apply.getInstruction(), builderCtx);
    auto loc = apply.getLoc();
    auto *asi =
        builder.createAllocStack(loc, existentialInfo.ConcreteValue->getType());
    // If the type is an address, simple copy it.
    if (existentialInfo.ConcreteValue->getType().isAddress()) {
      builder.createCopyAddr(loc, existentialInfo.ConcreteValue, asi, IsNotTake,
                             IsInitialization_t::IsInitialization);
    } else {
      // Otherwise, we probably got the value from the source of a store
      // instruction so, create a store into the temporary argument.
      auto copy =
          builder.emitCopyValueOperation(loc, existentialInfo.ConcreteValue);
      builder.emitStoreValueOperation(loc, copy, asi,
                                      StoreOwnershipQualifier::Init);
    }
    return ConcreteArgumentCopy(origArg, asi);
  }
};

SILValue SILCombiner::canCastArg(FullApplySite Apply,
                                 const OpenedArchetypeInfo &OAI,
                                 const ConcreteExistentialInfo &CEI,
                                 unsigned ArgIdx) {
  if (!CEI.ConcreteValue || CEI.ConcreteType->isOpenedExistential() ||
      !CEI.ConcreteValue->getType().isAddress())
    return SILValue();

  // Don't specialize apply instructions if the result type references
  // OpenedArchetype, because this optimization does not know how to substitute
  // types in the users of this apply. In the function type substitution below,
  // all references to OpenedArchetype will be substituted. So walk the type to
  // find all possible references, such as returning Optional<OpenedArchetype>.
  // The same holds for other arguments or indirect result that refer to the
  // OpenedArchetype, because the following optimization will rewrite only the
  // argument at ArgIdx.
  //
  // Note that the language does not allow Self to occur in contravariant
  // position. However, SIL does allow this and it can happen as a result of
  // upstream transformations. Since this is bail-out logic, it must handle
  // all verifiable SIL.
  if (applyInvolvesOpenedArchetypeWithRoot(Apply, OAI.OpenedArchetype,
                                           ArgIdx)) {
    return SILValue();
  }

  return Builder.createUncheckedAddrCast(
      Apply.getLoc(), Apply.getArgument(ArgIdx), CEI.ConcreteValue->getType());
}
/// Rewrite the given method apply instruction in terms of the provided concrete
/// type information.
///
/// If the rewrite is successful, the original apply will be removed and the new
/// apply is returned. Otherwise, the original apply will not be removed and
/// nullptr is returned.
///
/// Creates a new apply instruction that uses the concrete type instead of the
/// existential type. Type substitution will be performed from all occurrences
/// of CEI.OpenedArchetype to the replacement type CEI.ConcreteType within the
/// applied function type. The single self argument of the apply will be
/// rewritten. This helps the devirtualizer to replace witness_method by
/// class_method instructions and then devirtualize.
///
/// Note that the substituted type, CEI.OpenedArchetype, is the same type as the
/// self argument for nonstatic methods, but for static methods self is the
/// metatype instead. For witness methods, CEI.OpenedArchetype is usually the
/// same as WMI->getLookupType() but differs in the unusual situation in which
/// the witness method is looked up using a different opened archetype.
///
/// FIXME: Protocol methods (witness or default) that return Self will be given
/// a new return type. This implementation fails to update the type signature of
/// SSA uses in those cases. Currently we bail out on methods that return Self.
SILInstruction *SILCombiner::createApplyWithConcreteType(
    FullApplySite Apply,
    const llvm::SmallDenseMap<unsigned, ConcreteOpenedExistentialInfo> &COAIs,
    SILBuilderContext &BuilderCtx) {
  // Ensure that the callee is polymorphic.
  assert(Apply.getOrigCalleeType()->isPolymorphic());

  // Create the new set of arguments to apply including their substitutions.
  SubstitutionMap NewCallSubs = Apply.getSubstitutionMap();
  SmallVector<SILValue, 8> NewArgs;
  unsigned ArgIdx = 0;
  // Push the indirect result arguments.
  for (unsigned EndIdx = Apply.getSubstCalleeConv().getSILArgIndexOfFirstParam();
       ArgIdx < EndIdx; ++ArgIdx) {
      NewArgs.push_back(Apply.getArgument(ArgIdx));
  }

  // Transform the parameter arguments.
  SmallVector<ConcreteArgumentCopy, 4> concreteArgCopies;
  for (unsigned EndIdx = Apply.getNumArguments(); ArgIdx < EndIdx; ++ArgIdx) {
    auto ArgIt = COAIs.find(ArgIdx);
    if (ArgIt == COAIs.end()) {
      // Use the old argument if it does not have a valid concrete existential.
      NewArgs.push_back(Apply.getArgument(ArgIdx));
      continue;
    }
    const OpenedArchetypeInfo &OAI = ArgIt->second.OAI;
    const ConcreteExistentialInfo &CEI = *ArgIt->second.CEI;
    assert(CEI.isValid());

    // Check for Arg's concrete type propagation legality.
    if (!canReplaceArg(Apply, OAI, CEI, ArgIdx)) {

      // As on last fall-back try to cast the argument.
      if (auto cast = canCastArg(Apply, OAI, CEI, ArgIdx)) {
        NewArgs.push_back(cast);
        // Form a new set of substitutions where the argument is
        // replaced with a concrete type.
        NewCallSubs = NewCallSubs.subst(
            [&](SubstitutableType *type) -> Type {
              if (type == OAI.OpenedArchetype)
                return CEI.ConcreteType;
              return type;
            },
            [&](CanType origTy, Type substTy,
                ProtocolDecl *proto) -> ProtocolConformanceRef {
              if (origTy->isEqual(OAI.OpenedArchetype)) {
                assert(substTy->isEqual(CEI.ConcreteType));
                // Do a conformance lookup on this witness requirement using the
                // existential's conformances. The witness requirement may be a
                // base type of the existential's requirements.
                return CEI.lookupExistentialConformance(proto);
              }
              return ProtocolConformanceRef(proto);
            });
        continue;
      }
      // Otherwise, use the original argument.
      NewArgs.push_back(Apply.getArgument(ArgIdx));
      continue;
    }

    // Ensure that we have a concrete value to propagate.
    assert(CEI.ConcreteValue);

    auto argSub =
        ConcreteArgumentCopy::generate(CEI, Apply, ArgIdx, BuilderCtx);
    if (argSub) {
      concreteArgCopies.push_back(*argSub);
      NewArgs.push_back(argSub->tempArg);
    } else {
      NewArgs.push_back(CEI.ConcreteValue);
    }

    // Form a new set of substitutions where the argument is
    // replaced with a concrete type.
    NewCallSubs = NewCallSubs.subst(
        [&](SubstitutableType *type) -> Type {
          if (type == OAI.OpenedArchetype)
            return CEI.ConcreteType;
          return type;
        },
        [&](CanType origTy, Type substTy,
            ProtocolDecl *proto) -> ProtocolConformanceRef {
          if (origTy->isEqual(OAI.OpenedArchetype)) {
            assert(substTy->isEqual(CEI.ConcreteType));
            // Do a conformance lookup on this witness requirement using the
            // existential's conformances. The witness requirement may be a
            // base type of the existential's requirements.
            return CEI.lookupExistentialConformance(proto);
          }
          return ProtocolConformanceRef(proto);
        });
  }

  // We need to make sure that we can a) update Apply to use the new args and b)
  // at least one argument has changed. If no arguments have changed, we need
  // to return nullptr. Otherwise, we will have an infinite loop.
  auto context = Apply.getFunction()->getTypeExpansionContext();
  auto substTy = Apply.getCallee()
                     ->getType()
                     .substGenericArgs(Apply.getModule(), NewCallSubs, context)
                     .getAs<SILFunctionType>();
  SILFunctionConventions conv(substTy,
                              SILModuleConventions(Apply.getModule()));
  bool canUpdateArgs = true;
  bool madeUpdate = false;
  for (unsigned index = 0; index < conv.getNumSILArguments(); ++index) {
    // Make sure that *all* the arguments in both the new substitution function
    // and our vector of new arguments have the same type.
    canUpdateArgs &=
        conv.getSILArgumentType(index, context) == NewArgs[index]->getType();
    // Make sure that we have changed at least one argument.
    madeUpdate |=
        NewArgs[index]->getType() != Apply.getArgument(index)->getType();
  }

  // If we can't update the args (because of a type mismatch) or the args don't
  // change, bail out by removing the instructions we've added and returning
  // nullptr.
  if (!canUpdateArgs || !madeUpdate) {
    // Remove any new instructions created while attempting to optimize this
    // apply. Since the apply was never rewritten, if they aren't removed here,
    // they will be removed later as dead when visited by SILCombine, causing
    // SILCombine to loop infinitely, creating and destroying the casts.
    //
    // Use a new deleter with no callbacks so we can pretend this never
    // happened. Otherwise SILCombine will infinitely iterate. This works as
    // long as the instructions in this tracking list were never added to the
    // SILCombine Worklist.
    InstructionDeleter deleter;
    for (SILInstruction *inst : *Builder.getTrackingList()) {
      deleter.trackIfDead(inst);
    }
    deleter.cleanupDeadInstructions();
    Builder.getTrackingList()->clear();
    return nullptr;
  }
  // Now create the new apply instruction.
  SILBuilderWithScope ApplyBuilder(Apply.getInstruction(), BuilderCtx);
  FullApplySite NewApply;
  if (auto *TAI = dyn_cast<TryApplyInst>(Apply))
    NewApply = ApplyBuilder.createTryApply(
        Apply.getLoc(), Apply.getCallee(), NewCallSubs, NewArgs,
        TAI->getNormalBB(), TAI->getErrorBB(),
        TAI->getApplyOptions());
  else
    NewApply = ApplyBuilder.createApply(
        Apply.getLoc(), Apply.getCallee(), NewCallSubs, NewArgs,
        cast<ApplyInst>(Apply)->getApplyOptions());

  if (auto NewAI = dyn_cast<ApplyInst>(NewApply))
    replaceInstUsesWith(*cast<ApplyInst>(Apply.getInstruction()), NewAI);

  auto nextI = std::next(NewApply.getInstruction()->getIterator());
  eraseInstFromFunction(*Apply.getInstruction(), nextI);

  // cleanup immediately after the call on all paths reachable from the call.
  SmallVector<SILInstruction *, 2> cleanupPositions;
  if (nextI != NewApply.getParent()->end())
    cleanupPositions.push_back(&*nextI);
  else {
    for (auto &succ : NewApply.getParent()->getSuccessors())
      cleanupPositions.push_back(&*succ.getBB()->begin());
  }
  for (SILInstruction *cleanupPos : cleanupPositions) {
    // For any argument that was copied from the original value, destroy the old
    // argument (was must have been previously consumed by the call) and
    // deallocate the temporary copy.
    SILBuilder cleanupBuilder(cleanupPos, BuilderCtx, NewApply.getDebugScope());
    auto cleanupLoc = RegularLocation::getAutoGeneratedLocation();
    for (ConcreteArgumentCopy &argCopy : llvm::reverse(concreteArgCopies)) {
      cleanupBuilder.createDestroyAddr(cleanupLoc, argCopy.origArg);
      cleanupBuilder.createDeallocStack(cleanupLoc, argCopy.tempArg);
    }
  }
  return NewApply.getInstruction();
}

/// Rewrite a witness method's lookup type from an archetype to a concrete type.
/// Example:
///   %existential = alloc_stack $Protocol
///   %value = init_existential_addr %existential : $Concrete
///   copy_addr ... to %value
///   %opened = open_existential_addr %existential
///   %witness = witness_method $@opened(...) Protocol
///   apply %witness<$@opened(...) Protocol>(%opened)
///
/// ==> apply %witness<$Concrete>(%existential)
SILInstruction *
SILCombiner::propagateConcreteTypeOfInitExistential(FullApplySite Apply,
                                                    WitnessMethodInst *WMI) {
  // We do not perform this optimization in OSSA. In OSSA, we will have opaque
  // values we will redo this.
  if (WMI->getFunction()->hasOwnership())
    return nullptr;

  // Check if it is legal to perform the propagation.
  if (WMI->getConformance().isConcrete())
    return nullptr;

  // If the lookup type is not an opened existential type,
  // it cannot be made more concrete.
  if (!WMI->getLookupType()->isOpenedExistential())
    return nullptr;

  // Try to derive the concrete type and the related conformance of self and
  // other existential arguments by searching either for a preceding
  // init_existential or looking up sole conforming type.
  //
  // buildConcreteOpenedExistentialInfo takes a SILBuilderContext because it may
  // insert an unchecked cast to the concrete type, and it tracks the definition
  // of any opened archetype needed to use the concrete type.
  SILBuilderContext BuilderCtx(Builder.getModule(), Builder.getTrackingList());
  llvm::SmallDenseMap<unsigned, ConcreteOpenedExistentialInfo> COEIs;
  buildConcreteOpenedExistentialInfos(Apply, COEIs, BuilderCtx);

  // Bail, if no argument has a concrete existential to propagate.
  if (COEIs.empty())
    return nullptr;

  auto SelfCOEIIt =
      COEIs.find(Apply.getCalleeArgIndex(Apply.getSelfArgumentOperand()));

  // If no SelfCOEI is found, then just update the Apply with new COEIs for
  // other arguments.
  if (SelfCOEIIt == COEIs.end())
    return createApplyWithConcreteType(Apply, COEIs, BuilderCtx);

  auto &SelfCOEI = SelfCOEIIt->second;
  assert(SelfCOEI.isValid());

  const ConcreteExistentialInfo &SelfCEI = *SelfCOEI.CEI;
  assert(SelfCEI.isValid());

  // Get the conformance of the init_existential type, which is passed as the
  // self argument, on the witness' protocol.
  ProtocolConformanceRef SelfConformance =
      SelfCEI.lookupExistentialConformance(WMI->getLookupProtocol());

  // Propagate the concrete type into a callee-operand, which is a
  // witness_method instruction. It's ok to rewrite the witness method in terms
  // of a concrete type without rewriting the apply itself. In fact, doing so
  // may allow the Devirtualizer pass to finish the job.
  //
  // If we create a new instruction that’s the same as the old one we’ll
  // cause an infinite loop:
  // NewWMI will be added to the Builder’s tracker list.
  // SILCombine, in turn, uses the tracker list to populate the worklist
  // As such, if we don’t remove the witness method later on in the pass, we
  // are stuck:
  // We will re-create the same instruction and re-populate the worklist
  // with it.
  if (SelfCEI.ConcreteType != WMI->getLookupType() ||
      SelfConformance != WMI->getConformance()) {
    replaceWitnessMethodInst(WMI, BuilderCtx, SelfCEI.ConcreteType,
                             SelfConformance);
  }

  /// Create the new apply instruction using concrete types for arguments.
  return createApplyWithConcreteType(Apply, COEIs, BuilderCtx);
}

/// Rewrite a protocol extension lookup type from an archetype to a concrete
/// type.
/// Example:
///   %ref = alloc_ref $C
///   %existential = init_existential_ref %ref : $C : $C, $P
///   %opened = open_existential_ref %existential : $P to $@opened
///   %f = function_ref @defaultMethod
///   apply %f<@opened P>(%opened)
///
/// ==> apply %f<C : P>(%ref)
SILInstruction *
SILCombiner::propagateConcreteTypeOfInitExistential(FullApplySite Apply) {
  if (Apply.getFunction()->hasOwnership())
    return nullptr;

  // This optimization requires a generic argument.
  if (!Apply.hasSubstitutions())
    return nullptr;

  // Try to derive the concrete type and the related conformance of self and
  // other existential arguments by searching either for a preceding
  // init_existential or looking up sole conforming type.
  llvm::SmallDenseMap<unsigned, ConcreteOpenedExistentialInfo> COEIs;
  SILBuilderContext BuilderCtx(Builder.getModule(), Builder.getTrackingList());
  buildConcreteOpenedExistentialInfos(Apply, COEIs, BuilderCtx);

  // Bail, if no argument has a concrete existential to propagate.
  if (COEIs.empty())
    return nullptr;

  // At least one COEI is present, so cast instructions may already have been
  // inserted. We must either rewrite the apply or delete the casts and reset
  // the Builder's tracking list.
  return createApplyWithConcreteType(Apply, COEIs, BuilderCtx);
}

/// Should replace a call to `getContiguousArrayStorageType<A>(for:)` by the
/// metadata constructor of the return type.
///  getContiguousArrayStorageType<Int>(for:)
///    => metatype @thick ContiguousArrayStorage<Int>.Type
/// We know that `getContiguousArrayStorageType` will not return the AnyObject
/// type optimization for any non class or objc existential type instantiation.
static bool shouldReplaceCallByMetadataConstructor(CanType storageMetaTy) {
  auto metaTy = dyn_cast<MetatypeType>(storageMetaTy);
  if (!metaTy || metaTy->getRepresentation() != MetatypeRepresentation::Thick)
    return false;

  auto storageTy = metaTy.getInstanceType()->getCanonicalType();
  if (!storageTy->is_ContiguousArrayStorage())
    return false;

  auto boundGenericTy = dyn_cast<BoundGenericType>(storageTy);
  if (!boundGenericTy)
    return false;

  auto genericArgs = boundGenericTy->getGenericArgs();
  if (genericArgs.size() != 1)
    return false;
  auto ty = genericArgs[0]->getCanonicalType();
  if (ty->getStructOrBoundGenericStruct() || ty->getEnumOrBoundGenericEnum() ||
      isa<BuiltinVectorType>(ty) || isa<BuiltinIntegerType>(ty) ||
      isa<BuiltinFloatType>(ty) || isa<TupleType>(ty) ||
      isa<AnyFunctionType>(ty) ||
      (ty->isAnyExistentialType() && !ty->isObjCExistentialType()))
    return true;

  return false;
}

SILInstruction *SILCombiner::visitApplyInst(ApplyInst *AI) {
  Builder.setCurrentDebugScope(AI->getDebugScope());
  // apply{partial_apply(x,y)}(z) -> apply(z,x,y) is triggered
  // from visitPartialApplyInst(), so bail here.
  if (isa<PartialApplyInst>(AI->getCallee()))
    return nullptr;

  SILValue callee = AI->getCallee();
  if (auto *cee = dyn_cast<ConvertEscapeToNoEscapeInst>(callee)) {
    callee = cee->getOperand();
  }
  if (auto *CFI = dyn_cast<ConvertFunctionInst>(callee))
    return optimizeApplyOfConvertFunctionInst(AI, CFI);

  if (tryOptimizeKeypath(AI))
    return nullptr;

  // Optimize readonly functions with no meaningful users.
  SILFunction *SF = AI->getReferencedFunctionOrNull();
  if (SF && SF->getEffectsKind() < EffectsKind::ReleaseNone) {
    UserListTy Users;
    if (recursivelyCollectARCUsers(Users, AI)) {
      if (eraseApply(AI, Users))
        return nullptr;
    }
    // We found a user that we can't handle.
  }

  if (SF) {
    if (SF->hasSemanticsAttr(semantics::ARRAY_UNINITIALIZED)) {
      UserListTy Users;
      // If the uninitialized array is only written into then it can be removed.
      if (recursivelyCollectARCUsers(Users, AI)) {
        if (eraseApply(AI, Users))
          return nullptr;
      }
    }
    if (SF->hasSemanticsAttr(semantics::ARRAY_GET_CONTIGUOUSARRAYSTORAGETYPE)) {
      auto silTy = AI->getType();
      auto storageTy = AI->getType().getASTType();

      // getContiguousArrayStorageType<Int> => ContiguousArrayStorage<Int>
      if (shouldReplaceCallByMetadataConstructor(storageTy)) {
        auto metatype = Builder.createMetatype(AI->getLoc(), silTy);
        AI->replaceAllUsesWith(metatype);
        eraseInstFromFunction(*AI);
        return nullptr;
      }
    }
  }

  // (apply (thin_to_thick_function f)) to (apply f)
  if (auto *TTTFI = dyn_cast<ThinToThickFunctionInst>(AI->getCallee())) {
    // We currently don't remove any possible retain associated with the thick
    // function when rewriting the callsite. This should be ok because the
    // ABI normally expects a guaranteed callee.
    if (!AI->getOrigCalleeType()->isCalleeConsumed())
      return cloneFullApplySiteReplacingCallee(AI, TTTFI->getOperand(),
                                               Builder.getBuilderContext())
          .getInstruction();
  }

  // (apply (witness_method)) -> propagate information about
  // a concrete type from init_existential_addr or init_existential_ref.
  if (auto *WMI = dyn_cast<WitnessMethodInst>(AI->getCallee())) {
    if (propagateConcreteTypeOfInitExistential(AI, WMI)) {
      return nullptr;
    }
  }

  // (apply (function_ref method_from_protocol_extension)) ->
  // propagate information about a concrete type from init_existential_addr or
  // init_existential_ref.
  if (isa<FunctionRefInst>(AI->getCallee())) {
    if (propagateConcreteTypeOfInitExistential(AI)) {
      return nullptr;
    }
  }

  return nullptr;
}

SILInstruction *SILCombiner::visitBeginApplyInst(BeginApplyInst *BAI) {
  if (tryOptimizeInoutKeypath(BAI))
    return nullptr;
  return nullptr;
}

bool SILCombiner::
isTryApplyResultNotUsed(UserListTy &AcceptedUses, TryApplyInst *TAI) {
  SILBasicBlock *NormalBB = TAI->getNormalBB();
  SILBasicBlock *ErrorBB = TAI->getErrorBB();

  // The results of a try_apply are not only the normal and error return values,
  // but also the decision whether it throws or not. Therefore we have to check
  // if both, the normal and the error block, are empty and lead to a common
  // destination block.

  // Check if the normal and error blocks have a common single successor.
  auto *NormalBr = dyn_cast<BranchInst>(NormalBB->getTerminator());
  if (!NormalBr)
    return false;
  auto *ErrorBr = dyn_cast<BranchInst>(ErrorBB->getTerminator());
  if (!ErrorBr || ErrorBr->getDestBB() != NormalBr->getDestBB())
    return false;

  assert(NormalBr->getNumArgs() == ErrorBr->getNumArgs() &&
         "mismatching number of arguments for the same destination block");

  // Check if both blocks pass the same arguments to the common destination.
  for (unsigned Idx = 0, End = NormalBr->getNumArgs(); Idx < End; ++Idx) {
    if (NormalBr->getArg(Idx) != ErrorBr->getArg(Idx))
      return false;
  }

  // Check if the normal and error results only have ARC operations as uses.
  if (!recursivelyCollectARCUsers(AcceptedUses, NormalBB->getArgument(0)))
    return false;
  if (!recursivelyCollectARCUsers(AcceptedUses, ErrorBB->getArgument(0)))
    return false;

  InstructionSet UsesSet(NormalBB->getFunction());
  for (auto *I : AcceptedUses)
    UsesSet.insert(I);

  // Check if the normal and error blocks are empty, except the ARC uses.
  for (auto &I : *NormalBB) {
    if (!UsesSet.contains(&I) && !isa<TermInst>(&I))
      return false;
  }
  for (auto &I : *ErrorBB) {
    if (!UsesSet.contains(&I) && !isa<TermInst>(&I))
      return false;
  }
  return true;
}

SILInstruction *SILCombiner::visitTryApplyInst(TryApplyInst *AI) {
  // apply{partial_apply(x,y)}(z) -> apply(z,x,y) is triggered
  // from visitPartialApplyInst(), so bail here.
  if (isa<PartialApplyInst>(AI->getCallee()))
    return nullptr;

  // Optimize readonly functions with no meaningful users.
  SILFunction *Fn = AI->getReferencedFunctionOrNull();
  if (Fn && Fn->getEffectsKind() < EffectsKind::ReleaseNone) {
    UserListTy Users;
    if (isTryApplyResultNotUsed(Users, AI)) {
      SILBasicBlock *BB = AI->getParent();
      SILBasicBlock *NormalBB = AI->getNormalBB();
      SILBasicBlock *ErrorBB = AI->getErrorBB();
      SILLocation Loc = AI->getLoc();
      const SILDebugScope *DS = AI->getDebugScope();
      if (eraseApply(AI, Users)) {
        // Replace the try_apply with a cond_br false, which will be removed by
        // SimplifyCFG. We don't want to modify the CFG in SILCombine.
        Builder.setInsertionPoint(BB);
        Builder.setCurrentDebugScope(DS);
        auto *TrueLit = Builder.createIntegerLiteral(Loc,
                  SILType::getBuiltinIntegerType(1, Builder.getASTContext()), 0);
        Builder.createCondBranch(Loc, TrueLit, NormalBB, ErrorBB);

        NormalBB->eraseArgument(0);
        ErrorBB->eraseArgument(0);
        return nullptr;
      }
    }
    // We found a user that we can't handle.
  }

  // (try_apply (thin_to_thick_function f)) to (try_apply f)
  if (auto *TTTFI = dyn_cast<ThinToThickFunctionInst>(AI->getCallee())) {
    // We currently don't remove any possible retain associated with the thick
    // function when rewriting the callsite. This should be ok because the
    // ABI normally expects a guaranteed callee.
    if (!AI->getOrigCalleeType()->isCalleeConsumed())
      return cloneFullApplySiteReplacingCallee(AI, TTTFI->getOperand(),
                                               Builder.getBuilderContext())
          .getInstruction();
  }

  // (apply (witness_method)) -> propagate information about
  // a concrete type from init_existential_addr or init_existential_ref.
  if (auto *WMI = dyn_cast<WitnessMethodInst>(AI->getCallee())) {
    if (propagateConcreteTypeOfInitExistential(AI, WMI)) {
      return nullptr;
    }
  }

  // (apply (function_ref method_from_protocol_extension)) ->
  // propagate information about a concrete type from init_existential_addr or
  // init_existential_ref.
  if (isa<FunctionRefInst>(AI->getCallee())) {
    if (propagateConcreteTypeOfInitExistential(AI)) {
      return nullptr;
    }
  }

  return nullptr;
}
