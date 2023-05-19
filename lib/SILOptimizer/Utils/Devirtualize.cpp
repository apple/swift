//===--- Devirtualize.cpp - Helper for devirtualizing apply ---------------===//
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

#define DEBUG_TYPE "sil-devirtualize-utility"
#include "swift/SILOptimizer/Utils/Devirtualize.h"
#include "swift/AST/Decl.h"
#include "swift/AST/GenericSignature.h"
#include "swift/AST/ProtocolConformance.h"
#include "swift/AST/SubstitutionMap.h"
#include "swift/AST/Types.h"
#include "swift/SIL/InstructionUtils.h"
#include "swift/SIL/OptimizationRemark.h"
#include "swift/SIL/SILDeclRef.h"
#include "swift/SIL/SILFunction.h"
#include "swift/SIL/SILInstruction.h"
#include "swift/SIL/SILModule.h"
#include "swift/SIL/SILType.h"
#include "swift/SIL/SILValue.h"
#include "swift/SILOptimizer/Analysis/ClassHierarchyAnalysis.h"
#include "swift/SILOptimizer/Utils/InstOptUtils.h"
#include "llvm/ADT/SmallSet.h"
#include "llvm/ADT/Statistic.h"
#include "llvm/Support/Casting.h"
using namespace swift;

STATISTIC(NumClassDevirt, "Number of class_method applies devirtualized");
STATISTIC(NumWitnessDevirt, "Number of witness_method applies devirtualized");

//===----------------------------------------------------------------------===//
//                         Class Method Optimization
//===----------------------------------------------------------------------===//

void swift::getAllSubclasses(ClassHierarchyAnalysis *cha, ClassDecl *cd,
                             CanType classType, SILModule &module,
                             ClassHierarchyAnalysis::ClassList &subs) {
  // Collect the direct and indirect subclasses for the class.
  // Sort these subclasses in the order they should be tested by the
  // speculative devirtualization. Different strategies could be used,
  // E.g. breadth-first, depth-first, etc.
  // Currently, let's use the breadth-first strategy.
  // The exact static type of the instance should be tested first.
  auto &directSubs = cha->getDirectSubClasses(cd);
  auto &indirectSubs = cha->getIndirectSubClasses(cd);

  subs.append(directSubs.begin(), directSubs.end());
  subs.append(indirectSubs.begin(), indirectSubs.end());

  // FIXME: This is wrong -- we could have a non-generic class nested
  // inside a generic class
  if (isa<BoundGenericClassType>(classType)) {
    // Filter out any subclasses that do not inherit from this
    // specific bound class.
    auto removedIt =
        std::remove_if(subs.begin(), subs.end(), [&classType](ClassDecl *sub) {
          // FIXME: Add support for generic subclasses.
          if (sub->isGenericContext())
            return false;
          auto subCanTy = sub->getDeclaredInterfaceType()->getCanonicalType();
          // Handle the usual case here: the class in question
          // should be a real subclass of a bound generic class.
          return !classType->isBindableToSuperclassOf(subCanTy);
        });
    subs.erase(removedIt, subs.end());
  }
}

/// Returns true, if a method implementation corresponding to
/// the class_method applied to an instance of the class cd is
/// effectively final, i.e. it is statically known to be not overridden
/// by any subclasses of the class cd.
///
/// \p applySite  invocation instruction
/// \p classType type of the instance
/// \p cd  static class of the instance whose method is being invoked
/// \p cha class hierarchy analysis
static bool isEffectivelyFinalMethod(FullApplySite applySite, CanType classType,
                                     ClassDecl *cd,
                                     ClassHierarchyAnalysis *cha) {
  if (cd && cd->isFinal())
    return true;

  auto *cmi = cast<MethodInst>(applySite.getCallee());

  if (!calleesAreStaticallyKnowable(applySite.getModule(), cmi->getMember()))
    return false;

  auto *method = cmi->getMember().getAbstractFunctionDecl();
  assert(method && "Expected abstract function decl!");
  assert(!method->isFinal() && "Unexpected indirect call to final method!");

  // If this method is not overridden in the module,
  // there is no other implementation.
  if (!method->isOverridden())
    return true;

  // Class declaration may be nullptr, e.g. for cases like:
  // func foo<C:Base>(c: C) {}, where C is a class, but
  // it does not have a class decl.
  if (!cd)
    return false;

  if (!cha)
    return false;

  // We can analyze the class hierarchy rooted at this class and
  // eventually devirtualize a method call more efficiently.

  ClassHierarchyAnalysis::ClassList subs;
  getAllSubclasses(cha, cd, classType, applySite.getModule(), subs);

  // This is the implementation of the method to be used
  // if the exact class of the instance would be cd.
  auto *ImplMethod = cd->findImplementingMethod(method);

  // First, analyze all direct subclasses.
  for (auto S : subs) {
    // Check if the subclass overrides a method and provides
    // a different implementation.
    auto *ImplFD = S->findImplementingMethod(method);
    if (ImplFD != ImplMethod)
      return false;
  }

  return true;
}

/// Check if a given class is final in terms of a current
/// compilation, i.e.:
/// - it is really final
/// - or it is private and has not sub-classes
/// - or it is an internal class without sub-classes and
///   it is a whole-module compilation.
static bool isKnownFinalClass(ClassDecl *cd, SILModule &module,
                              ClassHierarchyAnalysis *cha) {
  if (cd->isFinal())
    return true;

  // Only handle classes defined within the SILModule's associated context.
  if (!cd->isChildContextOf(module.getAssociatedContext()))
    return false;

  if (!cd->hasAccess())
    return false;

  // Only consider 'private' members, unless we are in whole-module compilation.
  switch (cd->getEffectiveAccess()) {
  case AccessLevel::Open:
    return false;
  case AccessLevel::Public:
  case AccessLevel::Package:
  case AccessLevel::Internal:
    if (!module.isWholeModule())
      return false;
    break;
  case AccessLevel::FilePrivate:
  case AccessLevel::Private:
    break;
  }

  // Take the ClassHierarchyAnalysis into account.
  // If a given class has no subclasses and
  // - private
  // - or internal and it is a WMO compilation
  // then this class can be considered final for the purpose
  // of devirtualization.
  if (cha) {
    if (!cha->hasKnownDirectSubclasses(cd)) {
      switch (cd->getEffectiveAccess()) {
      case AccessLevel::Open:
        return false;
      case AccessLevel::Public:
      case AccessLevel::Package:
      case AccessLevel::Internal:
        if (!module.isWholeModule())
          return false;
        break;
      case AccessLevel::FilePrivate:
      case AccessLevel::Private:
        break;
      }

      return true;
    }
  }

  return false;
}

// Attempt to get the instance for S, whose static type is the same as
// its exact dynamic type, returning a null SILValue() if we cannot find it.
// The information that a static type is the same as the exact dynamic,
// can be derived e.g.:
// - from a constructor or
// - from a successful outcome of a checked_cast_br [exact] instruction.
SILValue swift::getInstanceWithExactDynamicType(SILValue instance,
                                                ClassHierarchyAnalysis *cha) {
  auto *f = instance->getFunction();
  auto &module = f->getModule();

  while (instance) {
    instance = stripCasts(instance);

    if (isa<AllocRefInst>(instance) || isa<MetatypeInst>(instance)) {
      if (instance->getType().getASTType()->hasDynamicSelfType())
        return SILValue();
      return instance;
    }

    auto *arg = dyn_cast<SILArgument>(instance);
    if (!arg)
      break;

    auto *singlePred = arg->getParent()->getSinglePredecessorBlock();
    if (!singlePred) {
      if (!isa<SILFunctionArgument>(arg))
        break;
      auto *cd = arg->getType().getClassOrBoundGenericClass();
      // Check if this class is effectively final.
      if (!cd || !isKnownFinalClass(cd, module, cha))
        break;
      return arg;
    }

    // Traverse the chain of predecessors.
    if (isa<BranchInst>(singlePred->getTerminator())
        || isa<CondBranchInst>(singlePred->getTerminator())) {
      instance = cast<SILPhiArgument>(arg)->getIncomingPhiValue(singlePred);
      continue;
    }

    // If it is a BB argument received on a success branch
    // of a checked_cast_br, then we know its exact type.
    auto *ccbi = dyn_cast<CheckedCastBranchInst>(singlePred->getTerminator());
    if (!ccbi)
      break;
    if (!ccbi->isExact() || ccbi->getSuccessBB() != arg->getParent())
      break;
    return instance;
  }

  return SILValue();
}

/// Try to determine the exact dynamic type of an object.
/// returns the exact dynamic type of the object, or an empty type if the exact
/// type could not be determined.
SILType swift::getExactDynamicType(SILValue instance,
                                   ClassHierarchyAnalysis *cha,
                                   bool forUnderlyingObject) {
  auto *f = instance->getFunction();
  auto &module = f->getModule();

  // Set of values to be checked for their exact types.
  SmallVector<SILValue, 8> worklist;
  // The detected type of the underlying object.
  SILType resultType;
  // Set of processed values.
  llvm::SmallSet<SILValue, 8> processed;
  worklist.push_back(instance);

  while (!worklist.empty()) {
    auto v = worklist.pop_back_val();
    if (!v)
      return SILType();
    if (processed.count(v))
      continue;
    processed.insert(v);
    // For underlying object strip casts and projections.
    // For the object itself, simply strip casts.
    v = forUnderlyingObject ? getUnderlyingObject(v) : stripCasts(v);

    if (isa<AllocRefInst>(v) || isa<MetatypeInst>(v)) {
      if (resultType && resultType != v->getType())
        return SILType();
      resultType = v->getType();
      continue;
    }

    if (isa<LiteralInst>(v)) {
      if (resultType && resultType != v->getType())
        return SILType();
      resultType = v->getType();
      continue;
    }

    if (isa<StructInst>(v) || isa<TupleInst>(v) || isa<EnumInst>(v)) {
      if (resultType && resultType != v->getType())
        return SILType();
      resultType = v->getType();
      continue;
    }

    if (forUnderlyingObject) {
      if (isa<AllocationInst>(v)) {
        if (resultType && resultType != v->getType())
          return SILType();
        resultType = v->getType();
        continue;
      }
    }

    auto arg = dyn_cast<SILArgument>(v);
    if (!arg) {
      // We don't know what it is.
      return SILType();
    }

    if (auto *fArg = dyn_cast<SILFunctionArgument>(arg)) {
      // Bail on metatypes for now.
      if (fArg->getType().is<AnyMetatypeType>()) {
        return SILType();
      }
      auto *cd = fArg->getType().getClassOrBoundGenericClass();
      // If it is not class and it is a trivial type, then it
      // should be the exact type.
      if (!cd && fArg->getType().isTrivial(*f)) {
        if (resultType && resultType != fArg->getType())
          return SILType();
        resultType = fArg->getType();
        continue;
      }

      if (!cd) {
        // It is not a class or a trivial type, so we don't know what it is.
        return SILType();
      }

      // Check if this class is effectively final.
      if (!isKnownFinalClass(cd, module, cha)) {
        return SILType();
      }

      if (resultType && resultType != fArg->getType())
        return SILType();
      resultType = fArg->getType();
      continue;
    }

    auto *singlePred = arg->getParent()->getSinglePredecessorBlock();
    if (singlePred) {
      // If it is a BB argument received on a success branch
      // of a checked_cast_br, then we know its exact type.
      auto *ccbi = dyn_cast<CheckedCastBranchInst>(singlePred->getTerminator());
      if (ccbi && ccbi->isExact() && ccbi->getSuccessBB() == arg->getParent()) {
        if (resultType && resultType != arg->getType())
          return SILType();
        resultType = arg->getType();
        continue;
      }
    }

    // It is a BB argument, look through incoming values. If they all have the
    // same exact type, then we consider it to be the type of the BB argument.
    SmallVector<SILValue, 4> incomingValues;
    if (arg->getSingleTerminatorOperands(incomingValues)) {
      for (auto inValue : incomingValues) {
        worklist.push_back(inValue);
      }
      continue;
    }

    // The exact type is unknown.
    return SILType();
  }

  return resultType;
}

/// Try to determine the exact dynamic type of the underlying object.
/// returns the exact dynamic type of a value, or an empty type if the exact
/// type could not be determined.
SILType
swift::getExactDynamicTypeOfUnderlyingObject(SILValue instance,
                                             ClassHierarchyAnalysis *cha) {
  return getExactDynamicType(instance, cha, /* forUnderlyingObject */ true);
}

// Start with the substitutions from the apply.
// Try to propagate them to find out the real substitutions required
// to invoke the method.
static SubstitutionMap
getSubstitutionsForCallee(SILModule &module, CanSILFunctionType baseCalleeType,
                          CanType derivedSelfType, FullApplySite applySite) {

  // If the base method is not polymorphic, no substitutions are required,
  // even if we originally had substitutions for calling the derived method.
  if (!baseCalleeType->isPolymorphic())
    return SubstitutionMap();

  // Add any generic substitutions for the base class.
  Type baseSelfType = baseCalleeType->getSelfParameter().getArgumentType(
      module, baseCalleeType,
      applySite.getFunction()->getTypeExpansionContext());
  if (auto metatypeType = baseSelfType->getAs<MetatypeType>())
    baseSelfType = metatypeType->getInstanceType();

  auto *baseClassDecl = baseSelfType->getClassOrBoundGenericClass();
  assert(baseClassDecl && "not a class method");

  unsigned baseDepth = 0;
  SubstitutionMap baseSubMap;
  if (auto baseClassSig = baseClassDecl->getGenericSignatureOfContext()) {
    baseDepth = baseClassSig.getGenericParams().back()->getDepth() + 1;

    // Compute the type of the base class, starting from the
    // derived class type and the type of the method's self
    // parameter.
    Type derivedClass = derivedSelfType;
    if (auto metatypeType = derivedClass->getAs<MetatypeType>())
      derivedClass = metatypeType->getInstanceType();
    baseSubMap = derivedClass->getContextSubstitutionMap(
        module.getSwiftModule(), baseClassDecl);
  }

  SubstitutionMap origSubMap = applySite.getSubstitutionMap();

  Type calleeSelfType =
      applySite.getOrigCalleeType()->getSelfParameter().getArgumentType(
          module, applySite.getOrigCalleeType(),
          applySite.getFunction()->getTypeExpansionContext());
  if (auto metatypeType = calleeSelfType->getAs<MetatypeType>())
    calleeSelfType = metatypeType->getInstanceType();
  auto *calleeClassDecl = calleeSelfType->getClassOrBoundGenericClass();
  assert(calleeClassDecl && "self is not a class type");

  // Add generic parameters from the method itself, ignoring any generic
  // parameters from the derived class.
  unsigned origDepth = 0;
  if (auto calleeClassSig = calleeClassDecl->getGenericSignatureOfContext())
    origDepth = calleeClassSig.getGenericParams().back()->getDepth() + 1;

  auto baseCalleeSig = baseCalleeType->getInvocationGenericSignature();

  return
    SubstitutionMap::combineSubstitutionMaps(baseSubMap,
                                             origSubMap,
                                             CombineSubstitutionMaps::AtDepth,
                                             baseDepth,
                                             origDepth,
                                             baseCalleeSig);
}

// Return the new apply and true if a cast required CFG modification.
static std::pair<ApplyInst *, bool /* changedCFG */>
replaceApplyInst(SILBuilder &builder, SILLocation loc, ApplyInst *oldAI,
                 SILValue newFn, SubstitutionMap newSubs,
                 ArrayRef<SILValue> newArgs, ArrayRef<SILValue> newArgBorrows) {
  auto *newAI =
      builder.createApply(loc, newFn, newSubs, newArgs,
                          oldAI->getApplyOptions());

  if (!newArgBorrows.empty()) {
    for (SILValue arg : newArgBorrows) {
      builder.createEndBorrow(loc, arg);
    }
  }

  // Check if any casting is required for the return value.  newAI cannot be a
  // guaranteed value, so this cast cannot generate borrow scopes and it can be
  // used anywhere the original oldAI was used.
  auto castRes = castValueToABICompatibleType(
    &builder, loc, newAI, newAI->getType(), oldAI->getType(), /*usePoints*/ {});

  oldAI->replaceAllUsesWith(castRes.first);
  return {newAI, castRes.second};
}

// Return the new try_apply and true if a cast required CFG modification.
static std::pair<TryApplyInst *, bool /* changedCFG */>
replaceTryApplyInst(SILBuilder &builder, SILLocation loc, TryApplyInst *oldTAI,
                    SILValue newFn, SubstitutionMap newSubs,
                    ArrayRef<SILValue> newArgs, SILFunctionConventions conv,
                    ArrayRef<SILValue> newArgBorrows) {
  SILBasicBlock *normalBB = oldTAI->getNormalBB();
  SILBasicBlock *resultBB = nullptr;

  SILType newResultTy =
      conv.getSILResultType(builder.getTypeExpansionContext());

  // Does the result value need to be casted?
  auto oldResultTy = normalBB->getArgument(0)->getType();
  bool resultCastRequired = newResultTy != oldResultTy;

  // Create a new normal BB only if the result of the new apply differs
  // in type from the argument of the original normal BB.
  if (!resultCastRequired) {
    resultBB = normalBB;
  } else {
    resultBB = builder.getFunction().createBasicBlockBefore(normalBB);
    resultBB->createPhiArgument(newResultTy, OwnershipKind::Owned);
  }

  // We can always just use the original error BB because we'll be
  // deleting the edge to it from the old TAI.
  SILBasicBlock *errorBB = oldTAI->getErrorBB();

  // Insert a try_apply here.
  // Note that this makes this block temporarily double-terminated!
  // We won't fix that until deleteDevirtualizedApply.
  auto newTAI =
      builder.createTryApply(loc, newFn, newSubs, newArgs, resultBB, errorBB,
                             oldTAI->getApplyOptions());

  if (!newArgBorrows.empty()) {
    builder.setInsertionPoint(normalBB->begin());
    for (SILValue arg : newArgBorrows) {
      builder.createEndBorrow(loc, arg);
    }
    builder.setInsertionPoint(errorBB->begin());
    for (SILValue arg : newArgBorrows) {
      builder.createEndBorrow(loc, arg);
    }
  }

  if (resultCastRequired) {
    builder.setInsertionPoint(resultBB);

    SILValue resultValue = resultBB->getArgument(0);
    // resultValue cannot be a guaranteed value, so this cast cannot generate
    // borrow scopes and it can be used anywhere the original oldAI was
    // used--usePoints are not required.
    std::tie(resultValue, std::ignore) = castValueToABICompatibleType(
        &builder, loc, resultValue, newResultTy, oldResultTy, /*usePoints*/ {});
    builder.createBranch(loc, normalBB, {resultValue});
  }

  builder.setInsertionPoint(normalBB->begin());
  return {newTAI, resultCastRequired};
}

// Return the new begin_apply and true if a cast required CFG modification.
static std::pair<BeginApplyInst *, bool /* changedCFG */>
replaceBeginApplyInst(SILBuilder &builder, SILLocation loc,
                      BeginApplyInst *oldBAI, SILValue newFn,
                      SubstitutionMap newSubs, ArrayRef<SILValue> newArgs,
                      ArrayRef<SILValue> newArgBorrows) {
  bool changedCFG = false;
  auto *newBAI = builder.createBeginApply(loc, newFn, newSubs, newArgs,
                                          oldBAI->getApplyOptions());

  // Forward the token.
  oldBAI->getTokenResult()->replaceAllUsesWith(newBAI->getTokenResult());

  auto oldYields = oldBAI->getYieldedValues();
  auto newYields = newBAI->getYieldedValues();
  assert(oldYields.size() == newYields.size());

  for (auto i : indices(oldYields)) {
    auto oldYield = oldYields[i];
    auto newYield = newYields[i];
    // Insert any end_borrow if the yielded value before the token's uses.
    SmallVector<SILInstruction *, 4> users(
      makeUserIteratorRange(oldYield->getUses()));
    auto yieldCastRes = castValueToABICompatibleType(
      &builder, loc, newYield, newYield->getType(), oldYield->getType(),
      users);
    oldYield->replaceAllUsesWith(yieldCastRes.first);
    changedCFG |= yieldCastRes.second;
  }

  if (newArgBorrows.empty())
    return {newBAI, changedCFG};

  SILValue token = newBAI->getTokenResult();

  // The token will only be used by end_apply and abort_apply. Use that to
  // insert the end_borrows we need /after/ those uses.
  for (auto *use : token->getUses()) {
    SILBuilderWithScope borrowBuilder(
        &*std::next(use->getUser()->getIterator()),
        builder.getBuilderContext());
    for (SILValue borrow : newArgBorrows) {
      borrowBuilder.createEndBorrow(loc, borrow);
    }
  }

  return {newBAI, changedCFG};
}

// Return the new partial_apply and true if a cast required CFG modification.
static std::pair<PartialApplyInst *, bool /* changedCFG */>
replacePartialApplyInst(SILBuilder &builder, SILLocation loc,
                        PartialApplyInst *oldPAI, SILValue newFn,
                        SubstitutionMap newSubs, ArrayRef<SILValue> newArgs) {
  auto convention =
      oldPAI->getType().getAs<SILFunctionType>()->getCalleeConvention();
  auto *newPAI =
      builder.createPartialApply(loc, newFn, newSubs, newArgs, convention);

  // Check if any casting is required for the partially-applied function.
  // A non-guaranteed cast needs no usePoints.
  assert(newPAI->getOwnershipKind() != OwnershipKind::Guaranteed);
  auto castRes = castValueToABICompatibleType(
    &builder, loc, newPAI, newPAI->getType(), oldPAI->getType(),
    /*usePoints*/ {});
  oldPAI->replaceAllUsesWith(castRes.first);

  return {newPAI, castRes.second};
}

// Return the new apply and true if the CFG was also modified.
static std::pair<ApplySite, bool /* changedCFG */>
replaceApplySite(SILBuilder &builder, SILLocation loc, ApplySite oldAS,
                 SILValue newFn, SubstitutionMap newSubs,
                 ArrayRef<SILValue> newArgs, SILFunctionConventions conv,
                 ArrayRef<SILValue> newArgBorrows) {
  switch (oldAS.getKind()) {
  case ApplySiteKind::ApplyInst: {
    auto *oldAI = cast<ApplyInst>(oldAS);
    return replaceApplyInst(builder, loc, oldAI, newFn, newSubs, newArgs,
                            newArgBorrows);
  }
  case ApplySiteKind::TryApplyInst: {
    auto *oldTAI = cast<TryApplyInst>(oldAS);
    return replaceTryApplyInst(builder, loc, oldTAI, newFn, newSubs, newArgs,
                               conv, newArgBorrows);
  }
  case ApplySiteKind::BeginApplyInst: {
    auto *oldBAI = dyn_cast<BeginApplyInst>(oldAS);
    return replaceBeginApplyInst(builder, loc, oldBAI, newFn, newSubs, newArgs,
                                 newArgBorrows);
  }
  case ApplySiteKind::PartialApplyInst: {
    assert(newArgBorrows.empty());
    auto *oldPAI = cast<PartialApplyInst>(oldAS);
    return replacePartialApplyInst(builder, loc, oldPAI, newFn, newSubs,
                                   newArgs);
  }
  }
  llvm_unreachable("covered switch");
}

/// Delete an apply site that's been successfully devirtualized.
void swift::deleteDevirtualizedApply(ApplySite old) {
  auto *oldApply = old.getInstruction();
  recursivelyDeleteTriviallyDeadInstructions(oldApply, true);
}

SILFunction *swift::getTargetClassMethod(SILModule &module, ClassDecl *cd,
                                         MethodInst *mi) {
  assert((isa<ClassMethodInst>(mi) || isa<SuperMethodInst>(mi))
         && "Only class_method and super_method instructions are supported");

  SILDeclRef member = mi->getMember();
  return module.lookUpFunctionInVTable(cd, member);
}

CanType swift::getSelfInstanceType(CanType classOrMetatypeType) {
  if (auto metaType = dyn_cast<MetatypeType>(classOrMetatypeType))
    classOrMetatypeType = metaType.getInstanceType();

  if (auto selfType = dyn_cast<DynamicSelfType>(classOrMetatypeType))
    classOrMetatypeType = selfType.getSelfType();

  return classOrMetatypeType;
}

/// Check if it is possible to devirtualize an Apply instruction
/// and a class member obtained using the class_method instruction into
/// a direct call to a specific member of a specific class.
///
/// \p applySite is the apply to devirtualize.
/// \p cd is the class declaration we are devirtualizing for.
/// return true if it is possible to devirtualize, false - otherwise.
bool swift::canDevirtualizeClassMethod(FullApplySite applySite, ClassDecl *cd,
                                       OptRemark::Emitter *ore,
                                       bool isEffectivelyFinalMethod) {

  LLVM_DEBUG(llvm::dbgs() << "    Trying to devirtualize : "
                          << *applySite.getInstruction());

  SILModule &module = applySite.getModule();

  auto *mi = cast<MethodInst>(applySite.getCallee());

  // Find the implementation of the member which should be invoked.
  auto *f = getTargetClassMethod(module, cd, mi);

  // If we do not find any such function, we have no function to devirtualize
  // to... so bail.
  if (!f) {
    LLVM_DEBUG(llvm::dbgs() << "        FAIL: Could not find matching VTable "
                               "or vtable method for this class.\n");
    return false;
  }

  // We need to disable the  “effectively final” opt if a function is inlinable
  if (isEffectivelyFinalMethod && applySite.getFunction()->isSerialized()) {
    LLVM_DEBUG(llvm::dbgs() << "        FAIL: Could not optimize function "
                               "because it is an effectively-final inlinable: "
                            << applySite.getFunction()->getName() << "\n");
    return false;
  }

  // Mandatory inlining does class method devirtualization. I'm not sure if this
  // is really needed, but some test rely on this.
  // So even for Onone functions we have to do it if the SILStage is raw.
  if (f->getModule().getStage() != SILStage::Raw && !f->shouldOptimize()) {
    // Do not consider functions that should not be optimized.
    LLVM_DEBUG(llvm::dbgs()
               << "        FAIL: Could not optimize function "
               << " because it is marked no-opt: " << f->getName() << "\n");
    return false;
  }

  if (applySite.getFunction()->isSerialized()) {
    // function_ref inside fragile function cannot reference a private or
    // hidden symbol.
    if (!f->hasValidLinkageForFragileRef())
      return false;
  }

  // devirtualizeClassMethod below does not support this case. It currently
  // assumes it can try_apply call the target.
  if (!f->getLoweredFunctionType()->hasErrorResult()
      && isa<TryApplyInst>(applySite.getInstruction())) {
    LLVM_DEBUG(llvm::dbgs() << "        FAIL: Trying to devirtualize a "
          "try_apply but vtable entry has no error result.\n");
    return false;
  }

  return true;
}

/// Devirtualize an apply of a class method.
///
/// \p applySite is the apply to devirtualize.
/// \p ClassOrMetatype is a class value or metatype value that is the
///    self argument of the apply we will devirtualize.
/// return the result value of the new ApplyInst if created one or null.
///
/// Return the new apply and true if the CFG was also modified.
std::pair<FullApplySite, bool /* changedCFG */>
swift::devirtualizeClassMethod(FullApplySite applySite,
                               SILValue classOrMetatype, ClassDecl *cd,
                               OptRemark::Emitter *ore) {
  bool changedCFG = false;
  LLVM_DEBUG(llvm::dbgs() << "    Trying to devirtualize : "
                          << *applySite.getInstruction());

  SILModule &module = applySite.getModule();
  auto *mi = cast<MethodInst>(applySite.getCallee());

  auto *f = getTargetClassMethod(module, cd, mi);

  CanSILFunctionType genCalleeType = f->getLoweredFunctionTypeInContext(
      TypeExpansionContext(*applySite.getFunction()));

  SubstitutionMap subs = getSubstitutionsForCallee(
      module, genCalleeType, classOrMetatype->getType().getASTType(),
      applySite);
  CanSILFunctionType substCalleeType = genCalleeType;
  if (genCalleeType->isPolymorphic())
    substCalleeType = genCalleeType->substGenericArgs(
        module, subs, TypeExpansionContext(*applySite.getFunction()));
  SILFunctionConventions substConv(substCalleeType, module);

  SILBuilderWithScope builder(applySite.getInstruction());
  SILLocation loc = applySite.getLoc();
  auto *fri = builder.createFunctionRefFor(loc, f);

  // Create the argument list for the new apply, casting when needed
  // in order to handle covariant indirect return types and
  // contravariant argument types.
  SmallVector<SILValue, 8> newArgs;

  // If we have a value that is owned, but that we are going to use in as a
  // guaranteed argument, we need to borrow/unborrow the argument. Otherwise, we
  // will introduce new consuming uses. In contrast, if we have an owned value,
  // we are ok due to the forwarding nature of upcasts.
  SmallVector<SILValue, 8> newArgBorrows;

  auto indirectResultArgIter = applySite.getIndirectSILResults().begin();
  for (auto resultTy : substConv.getIndirectSILResultTypes(
           applySite.getFunction()->getTypeExpansionContext())) {
    auto castRes = castValueToABICompatibleType(
        &builder, loc, *indirectResultArgIter, indirectResultArgIter->getType(),
        resultTy, {applySite.getInstruction()});
    newArgs.push_back(castRes.first);
    changedCFG |= castRes.second;
    ++indirectResultArgIter;
  }

  auto paramArgIter = applySite.getArgumentsWithoutIndirectResults().begin();
  // Skip the last parameter, which is `self`. Add it below.
  for (auto param : substConv.getParameters()) {
    auto paramType =
        substConv.getSILType(param, builder.getTypeExpansionContext());
    SILValue arg = *paramArgIter;
    if (builder.hasOwnership() && arg->getType().isObject() &&
        arg->getOwnershipKind() == OwnershipKind::Owned &&
        param.isGuaranteed()) {
      SILBuilderWithScope borrowBuilder(applySite.getInstruction(), builder);
      arg = borrowBuilder.createBeginBorrow(loc, arg);
      newArgBorrows.push_back(arg);
    }
    auto argCastRes =
      castValueToABICompatibleType(&builder, loc, arg,
                                   paramArgIter->getType(), paramType,
                                   {applySite.getInstruction()});

    newArgs.push_back(argCastRes.first);
    changedCFG |= argCastRes.second;
    ++paramArgIter;
  }
  ApplySite newAS;
  bool neededCFGChange;
  std::tie(newAS, neededCFGChange) = replaceApplySite(
      builder, loc, applySite, fri, subs, newArgs, substConv, newArgBorrows);
  changedCFG |= neededCFGChange;
  FullApplySite newAI = FullApplySite::isa(newAS.getInstruction());
  assert(newAI);

  LLVM_DEBUG(llvm::dbgs() << "        SUCCESS: " << f->getName() << "\n");
  if (ore)
    ore->emit([&]() {
      using namespace OptRemark;
      return RemarkPassed("ClassMethodDevirtualized",
                          *applySite.getInstruction())
             << "Devirtualized call to class method " << NV("Method", f);
    });
  ++NumClassDevirt;

  return {newAI, changedCFG};
}

std::pair<FullApplySite, bool> swift::tryDevirtualizeClassMethod(
    FullApplySite applySite, SILValue classInstance, ClassDecl *cd,
    OptRemark::Emitter *ore, bool isEffectivelyFinalMethod) {
  if (!canDevirtualizeClassMethod(applySite, cd, ore, isEffectivelyFinalMethod))
    return {FullApplySite(), false};
  return devirtualizeClassMethod(applySite, classInstance, cd, ore);
}

//===----------------------------------------------------------------------===//
//                        Witness Method Optimization
//===----------------------------------------------------------------------===//

/// Compute substitutions for making a direct call to a SIL function with
/// @convention(witness_method) convention.
///
/// Such functions have a substituted generic signature where the
/// abstract `Self` parameter from the original type of the protocol
/// requirement is replaced by a concrete type.
///
/// Thus, the original substitutions of the apply instruction that
/// are written in terms of the requirement's generic signature need
/// to be remapped to substitutions suitable for the witness signature.
///
/// Supported remappings are:
///
/// - (Concrete witness thunk) Original substitutions:
///   [Self := ConcreteType, R0 := X0, R1 := X1, ...]
/// - Requirement generic signature:
///   <Self : P, R0, R1, ...>
/// - Witness thunk generic signature:
///   <W0, W1, ...>
/// - Remapped substitutions:
///   [W0 := X0, W1 := X1, ...]
///
/// - (Class witness thunk) Original substitutions:
///   [Self := C<A0, A1>, T0 := X0, T1 := X1, ...]
/// - Requirement generic signature:
///   <Self : P, R0, R1, ...>
/// - Witness thunk generic signature:
///   <Self : C<B0, B1>, B0, B1, W0, W1, ...>
/// - Remapped substitutions:
///   [Self := C<B0, B1>, B0 := A0, B1 := A1, W0 := X0, W1 := X1]
///
/// - (Default witness thunk) Original substitutions:
///   [Self := ConcreteType, R0 := X0, R1 := X1, ...]
/// - Requirement generic signature:
///   <Self : P, R0, R1, ...>
/// - Witness thunk generic signature:
///   <Self : P, W0, W1, ...>
/// - Remapped substitutions:
///   [Self := ConcreteType, W0 := X0, W1 := X1, ...]
///
/// \param conformanceRef The (possibly-specialized) conformance
/// \param requirementSig The generic signature of the requirement
/// \param witnessThunkSig The generic signature of the witness method
/// \param origSubMap The substitutions from the call instruction
/// \param isSelfAbstract True if the Self type of the witness method is
/// still abstract (i.e., not a concrete type).
/// \param classWitness The ClassDecl if this is a class witness method
static SubstitutionMap
getWitnessMethodSubstitutions(
    ASTContext &ctx,
    ProtocolConformanceRef conformanceRef,
    GenericSignature requirementSig,
    GenericSignature witnessThunkSig,
    SubstitutionMap origSubMap,
    bool isSelfAbstract,
    ClassDecl *classWitness) {

  if (witnessThunkSig.isNull())
    return SubstitutionMap();

  if (isSelfAbstract && !classWitness)
    return origSubMap;

  assert(!conformanceRef.isAbstract());
  auto conformance = conformanceRef.getConcrete();

  auto selfType = conformance->getProtocol()->getSelfInterfaceType();

  // If `Self` maps to a bound generic type, this gives us the
  // substitutions for the concrete type's generic parameters.
  auto baseSubMap = conformance->getSubstitutionMap();

  unsigned baseDepth = 0;
  auto *rootConformance = conformance->getRootConformance();
  if (auto conformingTypeSig = rootConformance->getGenericSignature())
    baseDepth = conformingTypeSig.getGenericParams().back()->getDepth() + 1;

  // witnessThunkSig begins with the optional class 'Self', followed by the
  // generic parameters of the concrete conforming type, followed by the
  // generic parameters of the protocol requirement, if any.
  //
  // - The 'Self' parameter is replaced with the conforming type.
  // - The conforming type's generic parameters are replaced by the
  //   conformance substitutions.
  // - The protocol requirement's generic parameters are replaced from the
  //   substitution map at the call site.
  return SubstitutionMap::get(
      witnessThunkSig,
      [&](SubstitutableType *type) {
        auto *paramType = type->castTo<GenericTypeParamType>();
        unsigned depth = paramType->getDepth();

        if (classWitness != nullptr) {
          if (depth == 0) {
            assert(paramType->getIndex() == 0);
            return selfType.subst(origSubMap);
          }

          --depth;
        }

        if (depth < baseDepth) {
          paramType = GenericTypeParamType::get(
              paramType->isParameterPack(),
              depth, paramType->getIndex(), ctx);

          return Type(paramType).subst(baseSubMap);
        }

        depth = depth - baseDepth + 1;

        paramType = GenericTypeParamType::get(
            paramType->isParameterPack(),
            depth, paramType->getIndex(), ctx);
        return Type(paramType).subst(origSubMap);
      },
      [&](CanType type, Type substType, ProtocolDecl *proto) {
        auto *paramType = type->getRootGenericParam();
        unsigned depth = paramType->getDepth();

        if (classWitness != nullptr) {
          if (depth == 0) {
            assert(type->isEqual(paramType));
            assert(paramType->getIndex() == 0);
            return conformanceRef;
          }

          --depth;
        }

        if (depth < baseDepth) {
          type = CanType(type.transform([&](Type t) -> Type {
            if (t->isEqual(paramType)) {
              return GenericTypeParamType::get(
                  paramType->isParameterPack(),
                  depth, paramType->getIndex(), ctx);
            }

            assert(!t->is<GenericTypeParamType>());
            return t;
          }));

          return baseSubMap.lookupConformance(type, proto);
        }

        depth = depth - baseDepth + 1;

        type = CanType(type.transform([&](Type t) -> Type {
          if (t->isEqual(paramType)) {
            return GenericTypeParamType::get(
                paramType->isParameterPack(),
                depth, paramType->getIndex(), ctx);
          }

          assert(!t->is<GenericTypeParamType>());
          return t;
        }));

        return origSubMap.lookupConformance(type, proto);
      });
}

SubstitutionMap
swift::getWitnessMethodSubstitutions(SILModule &module, ApplySite applySite,
                                     SILFunction *f,
                                     ProtocolConformanceRef cRef) {
  auto witnessFnTy = f->getLoweredFunctionTypeInContext(
      TypeExpansionContext(*applySite.getFunction()));
  assert(witnessFnTy->getRepresentation() ==
         SILFunctionTypeRepresentation::WitnessMethod);

  auto requirementSig = applySite.getOrigCalleeType()->getInvocationGenericSignature();
  auto witnessThunkSig = witnessFnTy->getInvocationGenericSignature();

  SubstitutionMap origSubs = applySite.getSubstitutionMap();

  auto &ctx = module.getASTContext();
  bool isSelfAbstract =
      witnessFnTy
          ->getSelfInstanceType(
              module, applySite.getFunction()->getTypeExpansionContext())
          ->is<GenericTypeParamType>();
  auto *classWitness = witnessFnTy->getWitnessMethodClass(
      module, applySite.getFunction()->getTypeExpansionContext());

  return ::getWitnessMethodSubstitutions(ctx, cRef, requirementSig,
                                         witnessThunkSig, origSubs,
                                         isSelfAbstract, classWitness);
}

/// Generate a new apply of a function_ref to replace an apply of a
/// witness_method when we've determined the actual function we'll end
/// up calling.
///
/// Return the new apply and true if the CFG was also modified.
static std::pair<ApplySite, bool>
devirtualizeWitnessMethod(ApplySite applySite, SILFunction *f,
                          ProtocolConformanceRef cRef,
                          OptRemark::Emitter *ore) {
  bool changedCFG = false;
  // We know the witness thunk and the corresponding set of substitutions
  // required to invoke the protocol method at this point.
  auto &module = applySite.getModule();

  // Collect all the required substitutions.
  //
  // The complete set of substitutions may be different, e.g. because the found
  // witness thunk f may have been created by a specialization pass and have
  // additional generic parameters.
  auto subMap = getWitnessMethodSubstitutions(module, applySite, f, cRef);

  // Figure out the exact bound type of the function to be called by
  // applying all substitutions.
  auto typeExpansionContext =
      applySite.getFunction()->getTypeExpansionContext();
  auto calleeCanType = f->getLoweredFunctionTypeInContext(typeExpansionContext);
  auto substCalleeCanType =
      calleeCanType->substGenericArgs(module, subMap, typeExpansionContext);

  // Collect arguments from the apply instruction.
  SmallVector<SILValue, 4> arguments;
  SmallVector<SILValue, 4> borrowedArgs;

  // Iterate over the non self arguments and add them to the
  // new argument list, upcasting when required.
  SILBuilderWithScope argBuilder(applySite.getInstruction());
  SILFunctionConventions substConv(substCalleeCanType, module);
  unsigned substArgIdx = applySite.getCalleeArgIndexOfFirstAppliedArg();
  for (auto arg : applySite.getArguments()) {
    auto paramInfo = substConv.getSILArgumentConvention(substArgIdx);
    auto paramType =
        substConv.getSILArgumentType(substArgIdx++, typeExpansionContext);
    if (arg->getType() != paramType) {
      if (argBuilder.hasOwnership() &&
          applySite.getKind() != ApplySiteKind::PartialApplyInst &&
          arg->getType().isObject() &&
          arg->getOwnershipKind() == OwnershipKind::Owned &&
          paramInfo.isGuaranteedConvention()) {
        SILBuilderWithScope borrowBuilder(applySite.getInstruction(),
                                          argBuilder);
        arg = borrowBuilder.createBeginBorrow(applySite.getLoc(), arg);
        borrowedArgs.push_back(arg);
      }
      auto argCastRes = castValueToABICompatibleType(
        &argBuilder, applySite.getLoc(), arg, arg->getType(), paramType,
        applySite.getInstruction());
      arg = argCastRes.first;
      changedCFG |= argCastRes.second;
    }
    arguments.push_back(arg);
  }
  assert(substArgIdx == substConv.getNumSILArguments());

  // Replace old apply instruction by a new apply instruction that invokes
  // the witness thunk.
  SILBuilderWithScope applyBuilder(applySite.getInstruction());
  SILLocation loc = applySite.getLoc();
  auto *fri = applyBuilder.createFunctionRefFor(loc, f);

  ApplySite newApplySite;
  bool neededCFGChange = false;
  std::tie(newApplySite, neededCFGChange) =
      replaceApplySite(applyBuilder, loc, applySite, fri, subMap, arguments,
                       substConv, borrowedArgs);
  changedCFG |= neededCFGChange;

  if (ore)
    ore->emit([&]() {
      using namespace OptRemark;
      return RemarkPassed("WitnessMethodDevirtualized",
                          *applySite.getInstruction())
             << "Devirtualized call to " << NV("Method", f);
    });
  ++NumWitnessDevirt;
  return {newApplySite, changedCFG};
}

static bool isNonGenericThunkOfGenericExternalFunction(SILFunction *thunk) {
  if (!thunk->isThunk())
    return false;
  if (thunk->getGenericSignature())
    return false;
  for (SILBasicBlock &block : *thunk) {
    for (SILInstruction &inst : block) {
      if (FullApplySite fas = FullApplySite::isa(&inst)) {
        if (SILFunction *calledFunc = fas.getReferencedFunctionOrNull()) {
          if (fas.hasSubstitutions() && !calledFunc->isDefinition())
            return true;
        }
      }
    }
  }
  return false;
}

static bool canDevirtualizeWitnessMethod(ApplySite applySite, bool isMandatory) {
  SILFunction *f;
  SILWitnessTable *wt;

  auto *wmi = cast<WitnessMethodInst>(applySite.getCallee());

  std::tie(f, wt) = applySite.getModule().lookUpFunctionInWitnessTable(
      wmi->getConformance(), wmi->getMember(), SILModule::LinkingMode::LinkAll);

  if (!f)
    return false;

  if (applySite.getFunction()->isSerialized()) {
    // function_ref inside fragile function cannot reference a private or
    // hidden symbol.
    if (!f->hasValidLinkageForFragileRef())
      return false;
  }

  // devirtualizeWitnessMethod below does not support this case. It currently
  // assumes it can try_apply call the target.
  if (!f->getLoweredFunctionType()->hasErrorResult()
      && isa<TryApplyInst>(applySite.getInstruction())) {
    LLVM_DEBUG(llvm::dbgs() << "        FAIL: Trying to devirtualize a "
          "try_apply but wtable entry has no error result.\n");
    return false;
  }

  // The following check is for performance reasons: if `f` is a non-generic thunk
  // which calls a (not inlinable) generic function in the defining module, it's
  // more efficient to not devirtualize, but call the non-generic thunk - even though
  // it's done through the witness table.
  // Example:
  // ```
  //   protocol P {
  //     func f(x: [Int])   // not generic
  //   }
  //   struct S: P {
  //     func f(x: some RandomAccessCollection<Int>) { ... } // generic
  //   }
  // ```
  // In the defining module, the generic conformance can be specialized (which is not
  // possible in the client module, because it's not inlinable).
  if (!isMandatory && isNonGenericThunkOfGenericExternalFunction(f)) {
    return false;
  }

  // FIXME: devirtualizeWitnessMethod does not support cases with covariant
  // 'Self'-rooted type parameters nested inside a collection type, like
  // '[Self]' or '[* : Self.A]', because it doesn't know how to deal with
  // associated collection upcasts.
  const Type interfaceTy = wmi->getMember()
                               .getDecl()
                               ->getInterfaceType()
                               // Skip the 'self' parameter.
                               ->castTo<AnyFunctionType>()
                               ->getResult();

  if (!interfaceTy->hasTypeParameter())
    return true;

  auto *const selfGP = wmi->getLookupProtocol()->getProtocolSelfType();
  auto isSelfRootedTypeParameter = [selfGP](Type T) -> bool {
    if (!T->hasTypeParameter())
      return false;

    if (T->isTypeParameter()) {
      return T->getRootGenericParam()->isEqual(selfGP);
    }

    return false;
  };

  return !interfaceTy.findIf([&](Type T) -> bool {
    if (!T->hasTypeParameter())
      return false;

    if (T->isArray() || T->isDictionary()) {
      return T.findIf(isSelfRootedTypeParameter);
    }

    if (auto *FT = T->getAs<FunctionType>()) {
      for (const auto &Param : FT->getParams()) {
        if (Param.isVariadic() && T.findIf(isSelfRootedTypeParameter))
          return true;
      }
    }

    return false;
  });
}

/// In the cases where we can statically determine the function that
/// we'll call to, replace an apply of a witness_method with an apply
/// of a function_ref, returning the new apply.
std::pair<ApplySite, bool>
swift::tryDevirtualizeWitnessMethod(ApplySite applySite,
                                    OptRemark::Emitter *ore,
                                    bool isMandatory) {
  if (!canDevirtualizeWitnessMethod(applySite, isMandatory))
    return {ApplySite(), false};

  SILFunction *f;
  SILWitnessTable *wt;

  auto *wmi = cast<WitnessMethodInst>(applySite.getCallee());

  std::tie(f, wt) = applySite.getModule().lookUpFunctionInWitnessTable(
      wmi->getConformance(), wmi->getMember(), SILModule::LinkingMode::LinkAll);

  return devirtualizeWitnessMethod(applySite, f, wmi->getConformance(), ore);
}

//===----------------------------------------------------------------------===//
//                              Top Level Driver
//===----------------------------------------------------------------------===//

/// Attempt to devirtualize the given apply if possible, and return a
/// new instruction in that case, or nullptr otherwise.
///
/// Return the new apply and true if the CFG was also modified.
std::pair<ApplySite, bool>
swift::tryDevirtualizeApply(ApplySite applySite, ClassHierarchyAnalysis *cha,
                            OptRemark::Emitter *ore, bool isMandatory) {
  LLVM_DEBUG(llvm::dbgs() << "    Trying to devirtualize: "
                          << *applySite.getInstruction());

  // Devirtualize apply instructions that call witness_method instructions:
  //
  //   %8 = witness_method $Optional<UInt16>, #LogicValue.boolValue!getter
  //   %9 = apply %8<Self = CodeUnit?>(%6#1) : ...
  //
  if (isa<WitnessMethodInst>(applySite.getCallee()))
    return tryDevirtualizeWitnessMethod(applySite, ore, isMandatory);

  // TODO: check if we can also de-virtualize partial applies of class methods.
  FullApplySite fas = FullApplySite::isa(applySite.getInstruction());
  if (!fas)
    return {ApplySite(), false};

  /// Optimize a class_method and alloc_ref pair into a direct function
  /// reference:
  ///
  /// \code
  /// %XX = alloc_ref $Foo
  /// %YY = class_method %XX : $Foo, #Foo.get : $@convention(method)...
  /// \endcode
  ///
  ///  or
  ///
  /// %XX = metatype $...
  /// %YY = class_method %XX : ...
  ///
  ///  into
  ///
  /// %YY = function_ref @...
  if (auto *cmi = dyn_cast<ClassMethodInst>(fas.getCallee())) {
    auto instance = stripUpCasts(cmi->getOperand());
    auto classType = getSelfInstanceType(instance->getType().getASTType());
    auto *cd = classType.getClassOrBoundGenericClass();

    if (isEffectivelyFinalMethod(fas, classType, cd, cha))
      return tryDevirtualizeClassMethod(fas, instance, cd, ore,
                                        true /*isEffectivelyFinalMethod*/);

    // Try to check if the exact dynamic type of the instance is statically
    // known.
    if (auto instance = getInstanceWithExactDynamicType(cmi->getOperand(), cha)) {
      // Update the classDecl, because we are stripping casts more aggressively
      // in getInstanceWithExactDynamicType than in stripUpCasts.
      CanType classType = getSelfInstanceType(instance->getType().getASTType());
      // This should never be null - make the check just to be on the safe side.
      if (ClassDecl *cd = classType.getClassOrBoundGenericClass())
        return tryDevirtualizeClassMethod(fas, instance, cd, ore);
      return {ApplySite(), false};
    }

    if (auto exactTy = getExactDynamicType(cmi->getOperand(), cha)) {
      if (exactTy == cmi->getOperand()->getType())
        return tryDevirtualizeClassMethod(fas, cmi->getOperand(), cd, ore);
    }
  }

  if (isa<SuperMethodInst>(fas.getCallee())) {
    auto instance = fas.getArguments().back();
    auto classType = getSelfInstanceType(instance->getType().getASTType());
    auto *cd = classType.getClassOrBoundGenericClass();

    return tryDevirtualizeClassMethod(fas, instance, cd, ore);
  }

  return {ApplySite(), false};
}

bool swift::canDevirtualizeApply(FullApplySite applySite,
                                 ClassHierarchyAnalysis *cha) {
  LLVM_DEBUG(llvm::dbgs() << "    Trying to devirtualize: "
                          << *applySite.getInstruction());

  // Devirtualize apply instructions that call witness_method instructions:
  //
  //   %8 = witness_method $Optional<UInt16>, #LogicValue.boolValue!getter
  //   %9 = apply %8<Self = CodeUnit?>(%6#1) : ...
  //
  if (isa<WitnessMethodInst>(applySite.getCallee()))
    return canDevirtualizeWitnessMethod(applySite, /*isMandatory*/ false);

  /// Optimize a class_method and alloc_ref pair into a direct function
  /// reference:
  ///
  /// \code
  /// %XX = alloc_ref $Foo
  /// %YY = class_method %XX : $Foo, #Foo.get : $@convention(method)...
  /// \endcode
  ///
  ///  or
  ///
  /// %XX = metatype $...
  /// %YY = class_method %XX : ...
  ///
  ///  into
  ///
  /// %YY = function_ref @...
  if (auto *cmi = dyn_cast<ClassMethodInst>(applySite.getCallee())) {
    auto instance = stripUpCasts(cmi->getOperand());
    auto classType = getSelfInstanceType(instance->getType().getASTType());
    auto *cd = classType.getClassOrBoundGenericClass();

    if (isEffectivelyFinalMethod(applySite, classType, cd, cha))
      return canDevirtualizeClassMethod(applySite, cd, nullptr /*ore*/,
                                        true /*isEffectivelyFinalMethod*/);

    // Try to check if the exact dynamic type of the instance is statically
    // known.
    if (auto instance = getInstanceWithExactDynamicType(cmi->getOperand(), cha)) {
      CanType classType = getSelfInstanceType(instance->getType().getASTType());
      ClassDecl *cd = classType.getClassOrBoundGenericClass();
      return cd && canDevirtualizeClassMethod(applySite, cd);
    }

    if (auto exactTy = getExactDynamicType(cmi->getOperand(), cha)) {
      if (exactTy == cmi->getOperand()->getType())
        return canDevirtualizeClassMethod(applySite, cd);
    }
  }

  if (isa<SuperMethodInst>(applySite.getCallee())) {
    auto instance = applySite.getArguments().back();
    auto classType = getSelfInstanceType(instance->getType().getASTType());
    auto *cd = classType.getClassOrBoundGenericClass();

    return canDevirtualizeClassMethod(applySite, cd);
  }

  return false;
}
