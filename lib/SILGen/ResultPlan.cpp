//===--- ResultPlan.cpp ---------------------------------------------------===//
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

#include "ResultPlan.h"
#include "Callee.h"
#include "Conversion.h"
#include "Initialization.h"
#include "LValue.h"
#include "RValue.h"
#include "SILGenFunction.h"
#include "swift/AST/GenericEnvironment.h"
#include "swift/SIL/AbstractionPatternGenerators.h"

using namespace swift;
using namespace Lowering;

//===----------------------------------------------------------------------===//
//                                Result Plans
//===----------------------------------------------------------------------===//

void ResultPlan::finishAndAddTo(SILGenFunction &SGF, SILLocation loc,
                                ArrayRef<ManagedValue> &directResults,
                                SILValue bridgedForeignError,
                                RValue &result) {
  auto rvalue = finish(SGF, loc, directResults, bridgedForeignError);
  assert(!rvalue.isInContext());
  result.addElement(std::move(rvalue));
}

namespace {

/// A result plan for evaluating an indirect result into the address
/// associated with an initialization.
class InPlaceInitializationResultPlan final : public ResultPlan {
  Initialization *init;

public:
  InPlaceInitializationResultPlan(Initialization *init) : init(init) {}

  RValue finish(SILGenFunction &SGF, SILLocation loc,
                ArrayRef<ManagedValue> &directResults,
                SILValue bridgedForeignError) override {
    init->finishInitialization(SGF);
    return RValue::forInContext();
  }
  void
  gatherIndirectResultAddrs(SILGenFunction &SGF, SILLocation loc,
                            SmallVectorImpl<SILValue> &outList) const override {
    outList.emplace_back(init->getAddressForInPlaceInitialization(SGF, loc));
  }
};

/// A cleanup that handles the delayed emission of an indirect buffer for opened
/// Self arguments.
class IndirectOpenedSelfCleanup final : public Cleanup {
  SILValue box;
public:
  IndirectOpenedSelfCleanup()
    : box()
  {}
  
  void setBox(SILValue b) {
    assert(!box && "buffer already set?!");
    box = b;
  }
  
  void emit(SILGenFunction &SGF, CleanupLocation loc, ForUnwind_t forUnwind)
  override {
    assert(box && "buffer never emitted before activating cleanup?!");
    auto theBox = box;
    if (SGF.getASTContext().SILOpts.supportsLexicalLifetimes(SGF.getModule())) {
      if (auto *bbi = cast<BeginBorrowInst>(theBox)) {
        SGF.B.createEndBorrow(loc, bbi);
        theBox = bbi->getOperand();
      }
    }
    SGF.B.createDeallocBox(loc, theBox);
  }
  
  void dump(SILGenFunction &SGF) const override {
    llvm::errs() << "IndirectOpenedSelfCleanup\n";
    if (box)
      box->print(llvm::errs());
  }
};

/// Map a type expressed in terms of opened archetypes into a context-free
/// dependent type, and return a substitution map with generic parameters
/// corresponding to each distinct root opened archetype.
static std::pair<CanType, SubstitutionMap>
mapTypeOutOfOpenedExistentialContext(CanType t) {
  auto &ctx = t->getASTContext();

  SmallVector<OpenedArchetypeType *, 4> openedTypes;
  t->getRootOpenedExistentials(openedTypes);

  SmallVector<GenericTypeParamType *, 2> params;
  SmallVector<Requirement, 2> requirements;
  for (const unsigned i : indices(openedTypes)) {
    auto *param = GenericTypeParamType::get(
        /*isParameterPack*/ false, /*depth*/ 0, /*index*/ i, ctx);
    params.push_back(param);

    Type constraintTy = openedTypes[i]->getExistentialType();
    if (auto existentialTy = constraintTy->getAs<ExistentialType>())
      constraintTy = existentialTy->getConstraintType();

    requirements.emplace_back(RequirementKind::Conformance, param,
                              constraintTy);
  }

  const auto mappedSubs = SubstitutionMap::get(
      swift::buildGenericSignature(ctx, nullptr, params, requirements),
      [&](SubstitutableType *t) -> Type {
        return openedTypes[cast<GenericTypeParamType>(t)->getIndex()];
      },
      MakeAbstractConformanceForGenericType());

  const auto mappedTy = t.subst(
      [&](SubstitutableType *t) -> Type {
        auto *archTy = cast<ArchetypeType>(t);
        const auto index = std::find(openedTypes.begin(), openedTypes.end(),
                                     archTy->getRoot()) -
                           openedTypes.begin();
        assert(index != openedTypes.end() - openedTypes.begin());

        if (auto *dmt =
                archTy->getInterfaceType()->getAs<DependentMemberType>()) {
          return dmt->substRootParam(params[index],
                                     MakeAbstractConformanceForGenericType());
        }

        return params[index];
      },
      MakeAbstractConformanceForGenericType());

  return std::make_pair(mappedTy->getCanonicalType(), mappedSubs);
}

/// A result plan for an indirectly-returned opened existential value.
///
/// This defers allocating the temporary for the result to a later point so that
/// it happens after the arguments are evaluated.
class IndirectOpenedSelfResultPlan final : public ResultPlan {
  AbstractionPattern origType;
  CanType substType;
  CleanupHandle handle = CleanupHandle::invalid();
  mutable SILValue resultBox, resultBuf;

public:
  IndirectOpenedSelfResultPlan(SILGenFunction &SGF,
                               AbstractionPattern origType,
                               CanType substType)
    : origType(origType), substType(substType)
  {
    // Create a cleanup to deallocate the stack buffer at the proper scope.
    // We won't emit the buffer till later, after arguments have been opened,
    // though.
    SGF.Cleanups.pushCleanupInState<IndirectOpenedSelfCleanup>(
                                                         CleanupState::Dormant);
    handle = SGF.Cleanups.getCleanupsDepth();
  }
  
  void
  gatherIndirectResultAddrs(SILGenFunction &SGF, SILLocation loc,
                            SmallVectorImpl<SILValue> &outList) const override {
    assert(!resultBox && "already created temporary?!");
    
    // We allocate the buffer as a box because the scope nesting won't clean
    // this up with good stack discipline relative to any stack allocations that
    // occur during argument emission. Escape analysis during mandatory passes
    // ought to clean this up.

    auto resultTy = SGF.getLoweredType(origType, substType).getASTType();
    CanType layoutTy;
    SubstitutionMap layoutSubs;
    std::tie(layoutTy, layoutSubs) =
        mapTypeOutOfOpenedExistentialContext(resultTy);

    CanGenericSignature layoutSig =
        layoutSubs.getGenericSignature().getCanonicalSignature();
    auto boxLayout =
        SILLayout::get(SGF.getASTContext(), layoutSig,
                       SILField(layoutTy->getReducedType(layoutSig), true),
                       /*captures generics*/ false);

    resultBox = SGF.B.createAllocBox(loc,
      SILBoxType::get(SGF.getASTContext(),
                      boxLayout,
                      layoutSubs));
    if (SGF.getASTContext().SILOpts.supportsLexicalLifetimes(SGF.getModule())) {
      resultBox = SGF.B.createBeginBorrow(loc, resultBox, /*isLexical=*/true);
    }

    // Complete the cleanup to deallocate this buffer later, after we're
    // finished with the argument.
    static_cast<IndirectOpenedSelfCleanup&>(SGF.Cleanups.getCleanup(handle))
      .setBox(resultBox);
    SGF.Cleanups.setCleanupState(handle, CleanupState::Active);

    resultBuf = SGF.B.createProjectBox(loc, resultBox, 0);
    outList.emplace_back(resultBuf);
  }

  RValue finish(SILGenFunction &SGF, SILLocation loc,
                ArrayRef<ManagedValue> &directResults,
                SILValue bridgedForeignError) override {
    assert(resultBox && "never emitted temporary?!");
    
    // Lower the unabstracted result type.
    auto &substTL = SGF.getTypeLowering(substType);

    ManagedValue value;
    // If the value isn't address-only, go ahead and load.
    if (!substTL.isAddressOnly()) {
      auto load = substTL.emitLoad(SGF.B, loc, resultBuf,
                                   LoadOwnershipQualifier::Take);
      value = SGF.emitManagedRValueWithCleanup(load);
    } else {
      value = SGF.emitManagedRValueWithCleanup(resultBuf);
    }

    // A Self return should never be further abstracted. It's also never emitted
    // into context; we disable that optimization because Self may not even
    // be available to pre-allocate a stack buffer before we prepare a call.
    return RValue(SGF, loc, substType, value);
  }
};

/// A result plan for working with a single value and potentially
/// reabstracting it.  The value can actually be a tuple if the
/// abstraction is opaque.
class ScalarResultPlan final : public ResultPlan {
  std::unique_ptr<TemporaryInitialization> temporary;
  AbstractionPattern origType;
  CanType substType;
  Initialization *init;
  SILFunctionTypeRepresentation rep;

public:
  ScalarResultPlan(std::unique_ptr<TemporaryInitialization> &&temporary,
                   AbstractionPattern origType, CanType substType,
                   Initialization *init,
                   SILFunctionTypeRepresentation rep)
      : temporary(std::move(temporary)), origType(origType),
        substType(substType), init(init), rep(rep) {}

  RValue finish(SILGenFunction &SGF, SILLocation loc,
                ArrayRef<ManagedValue> &directResults,
                SILValue bridgedForeignError) override {
    // Claim the value:
    ManagedValue value;

    // If we were created with a temporary, that address was passed as
    // an indirect result.
    if (temporary) {
      // Establish the cleanup.
      temporary->finishInitialization(SGF);
      value = temporary->getManagedAddress();

      auto &substTL = SGF.getTypeLowering(value.getType());

      // If the value isn't address-only, go ahead and load.
      if (!substTL.isAddressOnly()) {
        auto load = substTL.emitLoad(SGF.B, loc, value.forward(SGF),
                                     LoadOwnershipQualifier::Take);
        value = SGF.emitManagedRValueWithCleanup(load);
      }

      // Otherwise, it was returned as a direct result.
    } else {
      value = directResults.front();
      directResults = directResults.slice(1);
    }

    return finish(SGF, loc, value, origType, substType, init, rep);
  }

  static RValue finish(SILGenFunction &SGF, SILLocation loc,
                       ManagedValue value,
                       AbstractionPattern origType, CanType substType,
                       Initialization *init,
                       SILFunctionTypeRepresentation rep) {
    // Reabstract the value if the types don't match.  This can happen
    // due to either substitution reabstractions or bridging.
    SILType loweredResultTy = SGF.getLoweredType(substType);
    if (value.getType().hasAbstractionDifference(rep, loweredResultTy)) {
      Conversion conversion = [&] {
        // Assume that a C-language API doesn't have substitution
        // reabstractions.  This shouldn't be necessary, but
        // emitOrigToSubstValue can get upset.
        if (getSILFunctionLanguage(rep) == SILFunctionLanguage::C) {
          return Conversion::getBridging(Conversion::BridgeResultFromObjC,
                                         origType.getType(), substType,
                                         loweredResultTy);
        } else {
          return Conversion::getOrigToSubst(origType, substType,
                                            loweredResultTy);
        }
      }();

      // Attempt to peephole this conversion into the context.
      if (init) {
        if (auto outerConversion = init->getAsConversion()) {
          if (outerConversion->tryPeephole(SGF, loc, value, conversion)) {
            outerConversion->finishInitialization(SGF);
            return RValue::forInContext();
          }
        }
      }

      // If that wasn't possible, just apply the conversion.
      value = conversion.emit(SGF, loc, value, SGFContext(init));

      // If that successfully emitted into the initialization, we're done.
      if (value.isInContext()) {
        return RValue::forInContext();
      }
    }

    // Otherwise, forcibly emit into the initialization if it exists.
    if (init) {
      init->copyOrInitValueInto(SGF, loc, value, /*init*/ true);
      init->finishInitialization(SGF);
      return RValue::forInContext();

      // Otherwise, we've got the r-value we want.
    } else {
      return RValue(SGF, loc, substType, value);
    }
  }

  void
  gatherIndirectResultAddrs(SILGenFunction &SGF, SILLocation loc,
                            SmallVectorImpl<SILValue> &outList) const override {
    if (!temporary)
      return;
    outList.emplace_back(temporary->getAddress());
  }
};

/// A result plan which calls copyOrInitValueInto on an Initialization
/// using a temporary buffer initialized by a sub-plan.
class InitValueFromTemporaryResultPlan final : public ResultPlan {
  Initialization *init;
  CanType substType;
  ResultPlanPtr subPlan;
  std::unique_ptr<TemporaryInitialization> temporary;

public:
  InitValueFromTemporaryResultPlan(
      Initialization *init, CanType substType,
      ResultPlanPtr &&subPlan,
      std::unique_ptr<TemporaryInitialization> &&temporary)
      : init(init), substType(substType), subPlan(std::move(subPlan)),
        temporary(std::move(temporary)) {}

  RValue finish(SILGenFunction &SGF, SILLocation loc,
                ArrayRef<ManagedValue> &directResults,
                SILValue bridgedForeignError) override {
    RValue subResult = subPlan->finish(SGF, loc, directResults,
                                       bridgedForeignError);
    assert(subResult.isInContext() && "sub-plan didn't emit into context?");
    (void)subResult;

    ManagedValue value = temporary->getManagedAddress();

    if (init) {
      init->copyOrInitValueInto(SGF, loc, value, /*init*/ true);
      init->finishInitialization(SGF);

      return RValue::forInContext();
    }

    return RValue(SGF, loc, substType, value);
  }

  void
  gatherIndirectResultAddrs(SILGenFunction &SGF, SILLocation loc,
                            SmallVectorImpl<SILValue> &outList) const override {
    subPlan->gatherIndirectResultAddrs(SGF, loc, outList);
  }
};

/// A result plan which calls copyOrInitValueInto using the result of
/// a sub-plan.
class InitValueFromRValueResultPlan final : public ResultPlan {
  Initialization *init;
  ResultPlanPtr subPlan;

public:
  InitValueFromRValueResultPlan(Initialization *init, ResultPlanPtr &&subPlan)
      : init(init), subPlan(std::move(subPlan)) {}

  RValue finish(SILGenFunction &SGF, SILLocation loc,
                ArrayRef<ManagedValue> &directResults,
                SILValue bridgedForeignError) override {
    RValue subResult = subPlan->finish(SGF, loc, directResults,
                                       bridgedForeignError);
    ManagedValue value = std::move(subResult).getAsSingleValue(SGF, loc);

    init->copyOrInitValueInto(SGF, loc, value, /*init*/ true);
    init->finishInitialization(SGF);

    return RValue::forInContext();
  }

  void
  gatherIndirectResultAddrs(SILGenFunction &SGF, SILLocation loc,
                            SmallVectorImpl<SILValue> &outList) const override {
    subPlan->gatherIndirectResultAddrs(SGF, loc, outList);
  }
};

/// A result plan which breaks a @pack_out result into some number of
/// components.
class PackExpansionResultPlan : public ResultPlan {
  SILValue PackAddr;
  SmallVector<ResultPlanPtr, 4> ComponentPlans;

public:
  PackExpansionResultPlan(ResultPlanBuilder &builder, SILValue packAddr,
                          llvm::Optional<ArrayRef<Initialization *>> inits,
                          AbstractionPattern origExpansionType,
                          CanTupleEltTypeArrayRef substEltTypes)
      : PackAddr(packAddr) {
    assert(!inits || inits->size() == substEltTypes.size());

    auto packTy = packAddr->getType().castTo<SILPackType>();
    auto formalPackType =
      CanPackType::get(packTy->getASTContext(), substEltTypes);
    auto origPatternType = origExpansionType.getPackExpansionPatternType();

    ComponentPlans.reserve(substEltTypes.size());
    for (auto i : indices(substEltTypes)) {
      Initialization *init = inits ? (*inits)[i] : nullptr;
      CanType substEltType = substEltTypes[i];

      if (isa<PackExpansionType>(substEltType)) {
        ComponentPlans.emplace_back(
          builder.buildPackExpansionIntoPack(packAddr, formalPackType, i,
                                             init, origPatternType));
      } else {
        ComponentPlans.emplace_back(
          builder.buildScalarIntoPack(packAddr, formalPackType, i,
                                      init, origPatternType));
      }
    }
  }

  RValue finish(SILGenFunction &SGF, SILLocation loc,
                ArrayRef<ManagedValue> &directResults,
                SILValue bridgedForeignError) override {
    for (auto &componentPlan : ComponentPlans) {
      auto componentRV = componentPlan->finish(SGF, loc, directResults,
                                               bridgedForeignError);
      assert(componentRV.isInContext()); (void) componentRV;
    }
    return RValue::forInContext();
  }

  void finishAndAddTo(SILGenFunction &SGF, SILLocation loc,
                      ArrayRef<ManagedValue> &directResults,
                      SILValue bridgedForeignError,
                      RValue &result) override {
    for (auto &componentPlan : ComponentPlans) {
      componentPlan->finishAndAddTo(SGF, loc, directResults,
                                    bridgedForeignError, result);
    }
  }

  void gatherIndirectResultAddrs(SILGenFunction &SGF, SILLocation loc,
                      SmallVectorImpl<SILValue> &outList) const override {
    outList.push_back(PackAddr);
  }
};

/// A result plan which transforms a pack expansion component.
class PackTransformResultPlan final : public ResultPlan {
  /// The address of the pack.  The addresses of the tuple elements
  /// have been written into the pack elements for the given component.
  SILValue PackAddr;

  /// A formal pack type with the same shape as the pack.
  CanPackType FormalPackType;

  /// The index of the pack expansion component within the pack.
  unsigned ComponentIndex;

  /// An initialization that the expansion elements should be fed into.
  Initialization *EmitInto;

  /// The abstraction pattern of the expansion type of the expansion.
  AbstractionPattern OrigPatternType;

  SILFunctionTypeRepresentation Rep;

public:
  PackTransformResultPlan(SILValue packAddr, CanPackType formalPackType,
                          unsigned componentIndex, Initialization *init,
                          AbstractionPattern origType,
                          SILFunctionTypeRepresentation rep)
    : PackAddr(packAddr), FormalPackType(formalPackType),
      ComponentIndex(componentIndex), EmitInto(init),
      OrigPatternType(origType), Rep(rep) {}

  void gatherIndirectResultAddrs(SILGenFunction &SGF, SILLocation loc,
                      SmallVectorImpl<SILValue> &outList) const override {
    llvm_unreachable("should not be gathering from an expansion plan");
  }

  RValue finish(SILGenFunction &SGF, SILLocation loc,
                ArrayRef<ManagedValue> &directResults,
                SILValue bridgedForeignError) override {
    // We opened a generic environment for the loop prior to the call
    // which wrote element addresses into the pack.  We can't open the
    // same environment twice in a function, though, so we need a new
    // environment.
    auto eltPatternTy =
      PackAddr->getType().castTo<SILPackType>()
                         ->getSILElementType(ComponentIndex);
    auto result = SGF.createOpenedElementValueEnvironment(eltPatternTy);
    auto openedEnv = result.first;
    auto eltAddrTy = result.second;

    // Loop over the pack, initializing each value with the appropriate
    // element.
    SGF.emitDynamicPackLoop(loc, FormalPackType, ComponentIndex, openedEnv,
                            [&](SILValue indexWithinComponent,
                                SILValue expansionIndex,
                                SILValue packIndex) {
      EmitInto->performPackExpansionInitialization(SGF, loc,
                                                   indexWithinComponent,
                                          [&](Initialization *eltInit) {
        // Pull the element address out of the pack, which is cheaper
        // than re-projecting it from the tuple.
        auto eltAddr =
          SGF.B.createPackElementGet(loc, packIndex, PackAddr, eltAddrTy);

        // Move the value into the destination.
        ManagedValue eltMV = [&] {
          auto &eltTL = SGF.getTypeLowering(eltAddrTy);
          if (!eltTL.isAddressOnly()) {
            auto load = eltTL.emitLoad(SGF.B, loc, eltAddr,
                                       LoadOwnershipQualifier::Take);
            eltMV = SGF.emitManagedRValueWithCleanup(load, eltTL);
          } else {
            eltMV = SGF.emitManagedBufferWithCleanup(eltAddr, eltTL);
          }
          return eltMV;
        }();

        // Map the formal type into the generic environment.
        auto substType = FormalPackType.getElementType(ComponentIndex);
        substType = cast<PackExpansionType>(substType).getPatternType();
        if (openedEnv) {
          substType = openedEnv->mapContextualPackTypeIntoElementContext(
                        substType);
        }

        // Finish in the normal way for scalar results.
        RValue rvalue =
          ScalarResultPlan::finish(SGF, loc, eltMV, OrigPatternType,
                                   substType, eltInit, Rep);
        assert(rvalue.isInContext()); (void) rvalue;
      });
    });

    EmitInto->finishInitialization(SGF);
    return RValue::forInContext();
  }
};

/// A result plan which produces a larger RValue from a bunch of
/// components.
class TupleRValueResultPlan final : public ResultPlan {
  CanType substType;
  SmallVector<ResultPlanPtr, 4> origEltPlans;

public:
  TupleRValueResultPlan(ResultPlanBuilder &builder, AbstractionPattern origType,
                        CanType substType)
      : substType(substType) {
    // Create plans for all the elements.
    origEltPlans.reserve(origType.getNumTupleElements());
    origType.forEachTupleElement(substType,
                                 [&](TupleElementGenerator &origElt) {
      AbstractionPattern origEltType = origElt.getOrigType();
      auto substEltTypes = origElt.getSubstTypes();
      if (!origElt.isOrigPackExpansion()) {
        origEltPlans.push_back(
          builder.build(nullptr, origEltType, substEltTypes[0]));
      } else {
        origEltPlans.push_back(builder.buildForPackExpansion(
            llvm::None, origEltType, substEltTypes));
      }
    });
  }

  RValue finish(SILGenFunction &SGF, SILLocation loc,
                ArrayRef<ManagedValue> &directResults,
                SILValue bridgedForeignError) override {
    RValue tupleRV(substType);

    // Finish all the component tuples.
    for (auto &plan : origEltPlans) {
      plan->finishAndAddTo(SGF, loc, directResults, bridgedForeignError,
                           tupleRV);
    }

    return tupleRV;
  }

  void
  gatherIndirectResultAddrs(SILGenFunction &SGF, SILLocation loc,
                            SmallVectorImpl<SILValue> &outList) const override {
    for (const auto &plan : origEltPlans) {
      plan->gatherIndirectResultAddrs(SGF, loc, outList);
    }
  }
};

/// A result plan which evaluates into the sub-components
/// of a splittable tuple initialization.
class TupleInitializationResultPlan final : public ResultPlan {
  Initialization *tupleInit;
  SmallVector<InitializationPtr, 4> eltInitsBuffer;
  SmallVector<ResultPlanPtr, 4> eltPlans;
  bool origTupleVanishes;

public:
  TupleInitializationResultPlan(ResultPlanBuilder &builder,
                                Initialization *tupleInit,
                                AbstractionPattern origType,
                                CanType substType,
                                bool origTupleVanishes)
      : tupleInit(tupleInit), origTupleVanishes(origTupleVanishes) {

    // Get the sub-initializations.
    SmallVector<Initialization*, 4> eltInits;
    if (origTupleVanishes) {
      eltInits.push_back(tupleInit);
    } else {
      MutableArrayRef<InitializationPtr> ownedEltInits
        = tupleInit->splitIntoTupleElements(builder.SGF, builder.loc,
                                            substType, eltInitsBuffer);

      // The ownership of these inits is maintained in eltInitsBuffer
      // (or tupleInit internally), but we need to create a temporary
      // array of unowned references to the inits, after which we can
      // throw away the ArrayRef that was returned to us.
      eltInits.reserve(ownedEltInits.size());
      for (auto &eltInit : ownedEltInits) {
        eltInits.push_back(eltInit.get());
      }
    }

    // Create plans for all the sub-initializations.
    eltPlans.reserve(origType.getNumTupleElements());
    origType.forEachTupleElement(substType,
                                 [&](TupleElementGenerator &elt) {
      auto origEltType = elt.getOrigType();
      auto substEltTypes = elt.getSubstTypes();
      if (!elt.isOrigPackExpansion()) {
        Initialization *eltInit = eltInits[elt.getSubstIndex()];
        eltPlans.push_back(builder.build(eltInit, origEltType,
                                         substEltTypes[0]));
      } else {
        auto componentInits = llvm::makeArrayRef(eltInits)
               .slice(elt.getSubstIndex(), substEltTypes.size());
        eltPlans.push_back(builder.buildForPackExpansion(componentInits,
                                                         origEltType,
                                                         substEltTypes));
      }
    });
  }

  RValue finish(SILGenFunction &SGF, SILLocation loc,
                ArrayRef<ManagedValue> &directResults,
                SILValue bridgedForeignError) override {
    for (auto &plan : eltPlans) {
      RValue eltRV = plan->finish(SGF, loc, directResults,
                                  bridgedForeignError);
      assert(eltRV.isInContext());
      (void)eltRV;
    }

    // Finish the tuple initialization; but if the tuple vanished,
    // this is handled in the loop above.
    if (!origTupleVanishes) {
      tupleInit->finishInitialization(SGF);
    }

    return RValue::forInContext();
  }

  void
  gatherIndirectResultAddrs(SILGenFunction &SGF, SILLocation loc,
                            SmallVectorImpl<SILValue> &outList) const override {
    for (const auto &eltPlan : eltPlans) {
      eltPlan->gatherIndirectResultAddrs(SGF, loc, outList);
    }
  }
};

class ForeignAsyncInitializationPlan final : public ResultPlan {
  SILLocation loc;
  CalleeTypeInfo calleeTypeInfo;
  SILType opaqueResumeType;
  SILValue resumeBuf;
  SILValue continuation;
  ExecutorBreadcrumb breadcrumb;
  
public:
  ForeignAsyncInitializationPlan(SILGenFunction &SGF, SILLocation loc,
                                 const CalleeTypeInfo &calleeTypeInfo)
    : loc(loc), calleeTypeInfo(calleeTypeInfo)
  {
    // Allocate space to receive the resume value when the continuation is
    // resumed.
    opaqueResumeType = SGF.getLoweredType(AbstractionPattern::getOpaque(),
                                          calleeTypeInfo.substResultType);
    resumeBuf = SGF.emitTemporaryAllocation(loc, opaqueResumeType);
  }
  
  void
  gatherIndirectResultAddrs(SILGenFunction &SGF, SILLocation loc,
                            SmallVectorImpl<SILValue> &outList) const override {
    // A foreign async function shouldn't have any indirect results.
  }

  ManagedValue
  emitForeignAsyncCompletionHandler(SILGenFunction &SGF,
                                    AbstractionPattern origFormalType,
                                    SILLocation loc) override {
    // Get the current continuation for the task.
    bool throws =
        calleeTypeInfo.foreign.async->completionHandlerErrorParamIndex()
            .has_value() ||
        calleeTypeInfo.foreign.error.has_value();

    continuation = SGF.B.createGetAsyncContinuationAddr(loc, resumeBuf,
                               calleeTypeInfo.substResultType, throws);

    // Wrap the Builtin.RawUnsafeContinuation in an
    // UnsafeContinuation<T, E>.
    auto continuationDecl = SGF.getASTContext().getUnsafeContinuationDecl();

    auto errorTy = throws
      ? SGF.getASTContext().getErrorExistentialType()
      : SGF.getASTContext().getNeverType();
    auto continuationTy = BoundGenericType::get(continuationDecl, Type(),
                                                { calleeTypeInfo.substResultType, errorTy })
      ->getCanonicalType();
    auto wrappedContinuation =
        SGF.B.createStruct(loc,
                           SILType::getPrimitiveObjectType(continuationTy),
                           {continuation});

    // Stash it in a buffer for a block object.
    auto blockStorageTy = SILType::getPrimitiveAddressType(
        SILBlockStorageType::get(continuationTy));
    auto blockStorage = SGF.emitTemporaryAllocation(loc, blockStorageTy);
    auto continuationAddr = SGF.B.createProjectBlockStorage(loc, blockStorage);
    SGF.B.createStore(loc, wrappedContinuation, continuationAddr,
                      StoreOwnershipQualifier::Trivial);

    // Get the block invocation function for the given completion block type.
    auto completionHandlerIndex = calleeTypeInfo.foreign.async
      ->completionHandlerParamIndex();
    auto impTy = SGF.getSILType(calleeTypeInfo.substFnType
                                      ->getParameters()[completionHandlerIndex],
                                calleeTypeInfo.substFnType);
    bool handlerIsOptional;
    CanSILFunctionType impFnTy;
    if (auto impObjTy = impTy.getOptionalObjectType()) {
      handlerIsOptional = true;
      impFnTy = cast<SILFunctionType>(impObjTy.getASTType());
    } else {
      handlerIsOptional = false;
      impFnTy = cast<SILFunctionType>(impTy.getASTType());
    }
    auto env = SGF.F.getGenericEnvironment();
    auto sig = env ? env->getGenericSignature().getCanonicalSignature()
                   : CanGenericSignature();
    SILFunction *impl =
        SGF.SGM.getOrCreateForeignAsyncCompletionHandlerImplFunction(
            cast<SILFunctionType>(
                impFnTy->mapTypeOutOfContext()->getReducedType(sig)),
            continuationTy->mapTypeOutOfContext()->getReducedType(sig),
            origFormalType, sig, *calleeTypeInfo.foreign.async,
            calleeTypeInfo.foreign.error);
    auto impRef = SGF.B.createFunctionRef(loc, impl);
    
    // Initialize the block object for the completion handler.
    SILValue block = SGF.B.createInitBlockStorageHeader(loc, blockStorage,
                          impRef, SILType::getPrimitiveObjectType(impFnTy),
                          SGF.getForwardingSubstitutionMap());
    
    // Wrap it in optional if the callee expects it.
    if (handlerIsOptional) {
      block = SGF.B.createOptionalSome(loc, block, impTy);
    }
    
    // We don't need to manage the block because it's still on the stack. We
    // know we won't escape it locally so the callee can be responsible for
    // _Block_copy-ing it.
    return ManagedValue::forUnmanaged(block);
  }

  void deferExecutorBreadcrumb(ExecutorBreadcrumb &&crumb) override {
    assert(!breadcrumb.needsEmit() && "overwriting an existing breadcrumb?");
    breadcrumb = std::move(crumb);
  }

  RValue finish(SILGenFunction &SGF, SILLocation loc,
                ArrayRef<ManagedValue> &directResults,
                SILValue bridgedForeignError) override {
    // There should be no direct results from the call.
    assert(directResults.empty());
    
    // Await the continuation we handed off to the completion handler.
    SILBasicBlock *resumeBlock = SGF.createBasicBlock();
    SILBasicBlock *errorBlock = nullptr;
    bool throws =
        calleeTypeInfo.foreign.async->completionHandlerErrorParamIndex()
            .has_value() ||
        calleeTypeInfo.foreign.error.has_value();
    if (throws) {
      errorBlock = SGF.createBasicBlock(FunctionSection::Postmatter);
    }

    auto *awaitBB = SGF.B.getInsertionBB();
    if (bridgedForeignError) {
      // Avoid a critical edge from the block which branches to the await and
      // foreign error blocks to the await block (to which the error block will
      // be made to branch in a moment) by introducing a trampoline which will
      // branch to the await block.
      awaitBB = SGF.createBasicBlock();
      SGF.B.createBranch(loc, awaitBB);

      // Finish emitting the foreign error block:
      // (1) fulfill the unsafe continuation with the foreign error
      // (2) branch to the await block
      {
        // First, fulfill the unsafe continuation with the foreign error.
        // Currently, that block's code looks something like
        //     %foreignError = ... : $*Optional<NSError>
        //     %converter = function_ref _convertNSErrorToError(_:)
        //     %error = apply %converter(%foreignError)
        //     [... insert here ...]
        //     destroy_value %error
        //     destroy_value %foreignError
        // Insert code to fulfill it after the native %error is defined.  That
        // code should structure the RawUnsafeContinuation (continuation) into
        // an appropriately typed UnsafeContinuation and then pass that together
        // with (a copy of) the error to
        // _resumeUnsafeThrowingContinuationWithError.
        // [foreign_error_block_with_foreign_async_convention]
        SGF.B.setInsertionPoint(
            ++bridgedForeignError->getDefiningInstruction()->getIterator());

        auto continuationDecl = SGF.getASTContext().getUnsafeContinuationDecl();

        auto errorTy = SGF.getASTContext().getErrorExistentialType();
        auto continuationBGT =
            BoundGenericType::get(continuationDecl, Type(),
                                  {calleeTypeInfo.substResultType, errorTy});
        auto env = SGF.F.getGenericEnvironment();
        auto sig = env ? env->getGenericSignature().getCanonicalSignature()
                       : CanGenericSignature();
        auto mappedContinuationTy =
            continuationBGT->mapTypeOutOfContext()->getReducedType(sig);
        auto resumeType =
            cast<BoundGenericType>(mappedContinuationTy).getGenericArgs()[0];
        auto continuationTy = continuationBGT->getCanonicalType();

        auto errorIntrinsic =
            SGF.SGM.getResumeUnsafeThrowingContinuationWithError();
        Type replacementTypes[] = {
            SGF.F.mapTypeIntoContext(resumeType)->getCanonicalType()};
        auto subs = SubstitutionMap::get(errorIntrinsic->getGenericSignature(),
                                         replacementTypes,
                                         ArrayRef<ProtocolConformanceRef>{});
        auto wrappedContinuation = SGF.B.createStruct(
            loc, SILType::getPrimitiveObjectType(continuationTy),
            {continuation});

        auto continuationMV = ManagedValue::forObjectRValueWithoutOwnership(
            SILValue(wrappedContinuation));
        SGF.emitApplyOfLibraryIntrinsic(
            loc, errorIntrinsic, subs,
            {continuationMV, ManagedValue::forCopyOwnedObjectRValue(
                                 SGF, loc, bridgedForeignError,
                                 ManagedValue::ScopeKind::Lexical)},
            SGFContext());

        // Second, emit a branch from the end of the foreign error block to the
        // await block, to await the continuation which was just fulfilled.
        SGF.B.setInsertionPoint(
            bridgedForeignError->getDefiningInstruction()->getParent());
        SGF.B.createBranch(loc, awaitBB);
      }

      SGF.B.emitBlock(awaitBB);
    }
    SGF.B.createAwaitAsyncContinuation(loc, continuation, resumeBlock, errorBlock);
    
    // Propagate an error if we have one.
    if (errorBlock) {
      SGF.B.emitBlock(errorBlock);
      breadcrumb.emit(SGF, loc);
      
      Scope errorScope(SGF, loc);

      auto errorTy = SGF.getASTContext().getErrorExistentialType();
      auto errorVal = SGF.B.createTermResult(
        SILType::getPrimitiveObjectType(errorTy), OwnershipKind::Owned);

      SGF.emitThrow(loc, errorVal, true);
    }
    
    SGF.B.emitBlock(resumeBlock);
    breadcrumb.emit(SGF, loc);
    
    // The incoming value is the maximally-abstracted result type of the
    // continuation. Move it out of the resume buffer and reabstract it if
    // necessary.
    auto resumeResult =
        SGF.emitLoad(loc, resumeBuf, AbstractionPattern::getOpaque(),
                     calleeTypeInfo.substResultType,
                     SGF.getTypeLowering(calleeTypeInfo.substResultType),
                     SGFContext(), IsTake);

    return RValue(SGF, loc, calleeTypeInfo.substResultType, resumeResult);
  }
};

class ForeignErrorInitializationPlan final : public ResultPlan {
  SILLocation loc;
  LValue lvalue;
  ResultPlanPtr subPlan;
  ManagedValue managedErrorTemp;
  CanType unwrappedPtrType;
  PointerTypeKind ptrKind;
  bool isOptional;
  CanType errorPtrType;

public:
  ForeignErrorInitializationPlan(SILGenFunction &SGF, SILLocation loc,
                                 const CalleeTypeInfo &calleeTypeInfo,
                                 ResultPlanPtr &&subPlan)
      : loc(loc), subPlan(std::move(subPlan)) {
    unsigned errorParamIndex =
        calleeTypeInfo.foreign.error->getErrorParameterIndex();
    auto substFnType = calleeTypeInfo.substFnType;
    SILParameterInfo errorParameter =
        substFnType->getParameters()[errorParamIndex];
    // We assume that there's no interesting reabstraction here beyond a layer
    // of optional.
    errorPtrType = errorParameter.getArgumentType(
        SGF.SGM.M, substFnType, SGF.getTypeExpansionContext());
    unwrappedPtrType = errorPtrType;
    Type unwrapped = errorPtrType->getOptionalObjectType();
    isOptional = (bool) unwrapped;

    if (unwrapped)
      unwrappedPtrType = unwrapped->getCanonicalType();

    auto errorType =
        CanType(unwrappedPtrType->getAnyPointerElementType(ptrKind));

    // In cases when from swift, we call objc imported methods written like so:
    //
    // (1) - (BOOL)submit:(NSError *_Nonnull __autoreleasing *_Nullable)errorOut;
    //
    // the clang importer will successfully import the given method as having a
    // non-null NSError. This doesn't follow the normal convention where we
    // expect the NSError to be Optional<NSError>. In order to preserve source
    // compatibility, we want to allow SILGen to handle this behavior. Luckily
    // in this case, NSError and Optional<NSError> are layout compatible, so we
    // can just pass in the Optional<NSError> and everything works.
    if (auto nsErrorTy = SGF.getASTContext().getNSErrorType()->getCanonicalType()) {
      if (errorType == nsErrorTy) {
        errorType = errorType.wrapInOptionalType();
      }
    }

    auto &errorTL = SGF.getTypeLowering(errorType);

    // Allocate a temporary.
    // It's flagged with "hasDynamicLifetime" because it's not possible to
    // statically verify the lifetime of the value.
    SILValue errorTemp =
        SGF.emitTemporaryAllocation(loc, errorTL.getLoweredType(),
                                    /*hasDynamicLifetime*/ true);

    // Nil-initialize it.
    SGF.emitInjectOptionalNothingInto(loc, errorTemp, errorTL);

    // Enter a cleanup to destroy the value there.
    managedErrorTemp = SGF.emitManagedBufferWithCleanup(errorTemp, errorTL);

    // Create the appropriate pointer type.
    lvalue = LValue::forAddress(SGFAccessKind::ReadWrite,
                                ManagedValue::forLValue(errorTemp),
                                /*TODO: enforcement*/ llvm::None,
                                AbstractionPattern(errorType), errorType);
  }

  void deferExecutorBreadcrumb(ExecutorBreadcrumb &&breadcrumb) override {
    subPlan->deferExecutorBreadcrumb(std::move(breadcrumb));
  }

  RValue finish(SILGenFunction &SGF, SILLocation loc,
                ArrayRef<ManagedValue> &directResults,
                SILValue bridgedForeignError) override {
    return subPlan->finish(SGF, loc, directResults, bridgedForeignError);
  }

  void
  gatherIndirectResultAddrs(SILGenFunction &SGF, SILLocation loc,
                            SmallVectorImpl<SILValue> &outList) const override {
    subPlan->gatherIndirectResultAddrs(SGF, loc, outList);
  }

  ManagedValue
  emitForeignAsyncCompletionHandler(SILGenFunction &SGF,
                                    AbstractionPattern origFormalType,
                                    SILLocation loc) override {
    return subPlan->emitForeignAsyncCompletionHandler(SGF, origFormalType, loc);
  }

  llvm::Optional<std::pair<ManagedValue, ManagedValue>>
  emitForeignErrorArgument(SILGenFunction &SGF, SILLocation loc) override {
    SILGenFunction::PointerAccessInfo pointerInfo = {
      unwrappedPtrType, ptrKind, SGFAccessKind::ReadWrite
    };
    auto pointerValue =
        SGF.emitLValueToPointer(loc, std::move(lvalue), pointerInfo);

    // Wrap up in an Optional if called for.
    if (isOptional) {
      auto &optTL = SGF.getTypeLowering(errorPtrType);
      pointerValue = SGF.getOptionalSomeValue(loc, pointerValue, optTL);
    }

    return std::make_pair(managedErrorTemp, pointerValue);
  }
};

} // end anonymous namespace

//===----------------------------------------------------------------------===//
//                            Result Plan Builder
//===----------------------------------------------------------------------===//

/// Build a result plan for the results of an apply.
///
/// If the initialization is non-null, the result plan will emit into it.
ResultPlanPtr ResultPlanBuilder::buildTopLevelResult(Initialization *init,
                                                     SILLocation loc) {
  // First check if we have a foreign error and/or async convention.
  if (auto foreignError = calleeTypeInfo.foreign.error) {
    // Handle the foreign error first.
    //
    // The plan needs to be built using the formal result type after foreign-error
    // adjustment.
    switch (foreignError->getKind()) {
    // These conventions make the formal result type ().
    case ForeignErrorConvention::ZeroResult:
    case ForeignErrorConvention::NonZeroResult:
      assert(calleeTypeInfo.substResultType->isVoid() ||
             calleeTypeInfo.foreign.async);
      allResults.clear();
      break;

    // These conventions leave the formal result alone.
    case ForeignErrorConvention::ZeroPreservedResult:
    case ForeignErrorConvention::NonNilError:
      break;

    // This convention changes the formal result to the optional object type; we
    // need to make our own make SILResultInfo array.
    case ForeignErrorConvention::NilResult: {
      assert(allResults.size() == 1);
      auto substFnTy = calleeTypeInfo.substFnType;
      CanType objectType = allResults[0]
                               .getReturnValueType(SGF.SGM.M, substFnTy,
                                                   SGF.getTypeExpansionContext())
                               .getOptionalObjectType();
      SILResultInfo optResult = allResults[0].getWithInterfaceType(objectType);
      allResults.clear();
      allResults.push_back(optResult);
      break;
    }
    }

    ResultPlanPtr subPlan;
    if (auto foreignAsync = calleeTypeInfo.foreign.async) {
      subPlan = ResultPlanPtr(
          new ForeignAsyncInitializationPlan(SGF, loc, calleeTypeInfo));
    } else {
      subPlan = build(init, calleeTypeInfo.origResultType.value(),
                      calleeTypeInfo.substResultType);
    }
    return ResultPlanPtr(new ForeignErrorInitializationPlan(
        SGF, loc, calleeTypeInfo, std::move(subPlan)));
  } else if (auto foreignAsync = calleeTypeInfo.foreign.async) {
    // Create a result plan that gets the result schema from the completion
    // handler callback's arguments.
    return ResultPlanPtr(
        new ForeignAsyncInitializationPlan(SGF, loc, calleeTypeInfo));
  } else {
    // Otherwise, we can just call build.
    return build(init, calleeTypeInfo.origResultType.value(),
                 calleeTypeInfo.substResultType);
  }
}

/// Build a result plan for the results of an apply.
///
/// If the initialization is non-null, the result plan will emit into it.
ResultPlanPtr ResultPlanBuilder::build(Initialization *init,
                                       AbstractionPattern origType,
                                       CanType substType) {
  // Destructure original tuples.
  if (origType.isTuple()) {
    return buildForTuple(init, origType, substType);
  }

  assert(!origType.isPackExpansion() &&
         "should've been handled when destructuring tuples");

  // Otherwise, grab the next result.
  auto result = allResults.pop_back_val();

  return buildForScalar(init, origType, substType, result);
}

ResultPlanPtr ResultPlanBuilder::buildForScalar(Initialization *init,
                                                AbstractionPattern origType,
                                                CanType substType,
                                                SILResultInfo result) {
  auto calleeTy = calleeTypeInfo.substFnType;
  
  // If the result is indirect, and we have an address to emit into, and
  // there are no abstraction differences, then just do it.
  if (init && init->canPerformInPlaceInitialization() &&
      SGF.silConv.isSILIndirect(result) &&
      !SGF.getLoweredType(substType).getAddressType().hasAbstractionDifference(
          calleeTypeInfo.getOverrideRep(),
          result.getSILStorageType(SGF.SGM.M, calleeTy,
                                   SGF.getTypeExpansionContext()))) {
    return ResultPlanPtr(new InPlaceInitializationResultPlan(init));
  }

  // Otherwise, we need to:
  //   - get the value, either directly or indirectly
  //   - possibly reabstract it
  //   - store it to the destination
  // We could break this down into different ResultPlan implementations,
  // but it's easier not to.
  
  // If the result type involves an indirectly-returned opened existential,
  // then we need to evaluate the arguments first in order to have access to
  // the opened Self type. A special result plan defers allocating the stack
  // slot to the point the call is emitted.
  if (result
          .getReturnValueType(SGF.SGM.M, calleeTy,
                              SGF.getTypeExpansionContext())
          ->hasOpenedExistential() &&
      SGF.silConv.isSILIndirect(result)) {
    return ResultPlanPtr(
      new IndirectOpenedSelfResultPlan(SGF, origType, substType));
  }

  // Create a temporary if the result is indirect.
  std::unique_ptr<TemporaryInitialization> temporary;
  if (SGF.silConv.isSILIndirect(result)) {
    auto &resultTL = SGF.getTypeLowering(result.getReturnValueType(
        SGF.SGM.M, calleeTy, SGF.getTypeExpansionContext()));
    SILLocation tmpLoc(loc);
    tmpLoc.markAutoGenerated();
    temporary = SGF.emitTemporary(tmpLoc, resultTL);
  }

  return ResultPlanPtr(new ScalarResultPlan(
      std::move(temporary), origType, substType, init,
      calleeTypeInfo.getOverrideRep()));
}

ResultPlanPtr ResultPlanBuilder::buildForPackExpansion(
    llvm::Optional<ArrayRef<Initialization *>> inits,
    AbstractionPattern origExpansionType, CanTupleEltTypeArrayRef substTypes) {
  assert(!inits || inits->size() == substTypes.size());

  // Pack expansions in the original result type always turn into
  // a single @pack_out result.
  auto result = allResults.pop_back_val();
  assert(result.isPack());
  auto packTy =
    result.getSILStorageType(SGF.SGM.M, calleeTypeInfo.substFnType,
                             SGF.getTypeExpansionContext());
  assert(packTy.castTo<SILPackType>()->getNumElements() == substTypes.size());

  // TODO: try to just forward a single pack

  // Allocate a pack to serve as the element.
  auto packAddr =
    SGF.emitTemporaryPackAllocation(loc, packTy.getObjectType());

  return ResultPlanPtr(new PackExpansionResultPlan(*this, packAddr, inits,
                                                   origExpansionType, substTypes));
}

ResultPlanPtr
ResultPlanBuilder::buildPackExpansionIntoPack(SILValue packAddr,
                                              CanPackType formalPackType,
                                              unsigned componentIndex,
                                              Initialization *init,
                                        AbstractionPattern origPatternType) {
  assert(init && init->canPerformPackExpansionInitialization());

  // Create an opened-element environment sufficient for working with
  // values of the pack expansion type.
  auto packTy = packAddr->getType().castTo<SILPackType>();
  auto result = SGF.createOpenedElementValueEnvironment(
                                packTy->getSILElementType(componentIndex));
  auto openedEnv = result.first;
  auto eltTy = result.second;

  // This code would be much easier to write, and more efficient
  // dynamically, if we could form packs by pack-applying a coroutine.
  // Instead, we have to initialize a tuple if we don't fall into the
  // (narrow but important) special case where we can just forward
  // addresses into the pack.

  // If the expansion addresses can just be forwarded into the pack,
  // we can emit a dynamic loop to do that now.
  if (init->canPerformInPlacePackInitialization(openedEnv, eltTy)) {
    SGF.emitDynamicPackLoop(loc, formalPackType, componentIndex, openedEnv,
                            [&](SILValue indexWithinComponent,
                                SILValue expansionPackIndex,
                                SILValue packIndex) {
      auto eltAddr =
        init->getAddressForInPlacePackInitialization(SGF, loc, eltTy);
      SGF.B.createPackElementSet(loc, eltAddr, packIndex, packAddr);
    });

    // The result plan just needs to finish the initialization when
    // it's finished.
    return ResultPlanPtr(new InPlaceInitializationResultPlan(init));
  }

  // Otherwise, make a tuple temporary and write the element addresses
  // into the pack.
  auto tupleTy = CanTupleType(TupleType::get(
              {packTy->getElementType(componentIndex)}, SGF.getASTContext()));
  auto tupleAddr = SGF.emitTemporaryAllocation(loc,
                                    SILType::getPrimitiveObjectType(tupleTy));

  SGF.emitDynamicPackLoop(loc, formalPackType, componentIndex, openedEnv,
                          [&](SILValue indexWithinComponent,
                              SILValue expansionPackIndex,
                              SILValue packIndex) {
    auto eltAddr = SGF.B.createTuplePackElementAddr(loc, expansionPackIndex,
                                                    tupleAddr, eltTy);
    SGF.B.createPackElementSet(loc, eltAddr, packIndex, packAddr);
  });

  // The result plan will write into `init` during finish().
  return ResultPlanPtr(
    new PackTransformResultPlan(packAddr, formalPackType,
                                componentIndex, init, origPatternType,
                                calleeTypeInfo.getOverrideRep()));
}

ResultPlanPtr
ResultPlanBuilder::buildScalarIntoPack(SILValue packAddr,
                                       CanPackType formalPackType,
                                       unsigned componentIndex,
                                       Initialization *init,
                                       AbstractionPattern origType) {
  assert(!origType.isPackExpansion());
  auto substType = formalPackType.getElementType(componentIndex);
  assert(!isa<PackExpansionType>(substType));

  // Fake up an @out result.
  auto loweredEltType = packAddr->getType().castTo<SILPackType>()
                                           ->getElementType(componentIndex);
  SILResultInfo resultInfo(loweredEltType, ResultConvention::Indirect);

  // Use the normal scalar emission path to gather an indirect result
  // of that type.
  auto plan = buildForScalar(init, origType, substType, resultInfo);

  // Immediately gather the indirect result.
  SmallVector<SILValue, 1> indirectResults;
  plan->gatherIndirectResultAddrs(SGF, loc, indirectResults);
  assert(indirectResults.size() == 1);
  auto eltAddr = indirectResults.front();

  // Write that into the pack.
  auto packIndex =
    SGF.B.createScalarPackIndex(loc, componentIndex, formalPackType);
  SGF.B.createPackElementSet(loc, eltAddr, packIndex, packAddr);

  return plan;
}

ResultPlanPtr ResultPlanBuilder::buildForTuple(Initialization *init,
                                               AbstractionPattern origType,
                                               CanType substType) {
  // If we have an initialization, and we can split the initialization,
  // emit directly into the initialization.  If the orig tuple vanishes,
  // that counts as the initialization being splittable.
  if (init) {
    bool vanishes = origType.doesTupleVanish();
    if (vanishes || init->canSplitIntoTupleElements()) {
      return ResultPlanPtr(
        new TupleInitializationResultPlan(*this, init, origType, substType,
                                          vanishes));
    }
  }

  auto substTupleType = dyn_cast<TupleType>(substType);
  bool substHasPackExpansion =
    (substTupleType && substTupleType.containsPackExpansionType());

  // Otherwise, if the tuple contains a pack expansion, we'll need to
  // initialize a single buffer one way or another: either we're giving
  // this to RValue (which wants a single value for tuples with pack
  // expansions) or we'll have to call copyOrInitValueInto on init
  // (which expects a single value).  Create a temporary, build into
  // that, and then call the initialization.
  //
  // We also use this path when we have an init and the type is
  // address-only, because we'll need to call copyOrInitValueInto and
  // we'll get better code by building that up indirectly.  But we don't
  // do that if we're not using lowered addresses because we prefer to
  // build tuples with scalar operations.
  auto &substTL = SGF.getTypeLowering(substType);
  assert(substTL.isAddressOnly() || !substHasPackExpansion);
  if (substTL.isAddressOnly() &&
      (substHasPackExpansion ||
       (init != nullptr && SGF.F.getConventions().useLoweredAddresses()))) {
    // Create a temporary.
    auto temporary = SGF.emitTemporary(loc, substTL);

    // Build a sub-plan to emit into the temporary.
    auto subplan = buildForTuple(temporary.get(), origType, substType);

    // Make a plan to produce the final result from that.
    return ResultPlanPtr(new InitValueFromTemporaryResultPlan(
        init, substType, std::move(subplan), std::move(temporary)));
  }

  // If we don't have an initialization, just build the individual
  // components.
  if (!init) {
    return ResultPlanPtr(new TupleRValueResultPlan(*this, origType, substType));
  }

  // Build a sub-plan that doesn't know about the initialization.
  auto subplan = buildForTuple(nullptr, origType, substType);

  // Make a plan that calls copyOrInitValueInto.
  return ResultPlanPtr(
      new InitValueFromRValueResultPlan(init, std::move(subplan)));
}

ResultPlanPtr
ResultPlanBuilder::computeResultPlan(SILGenFunction &SGF,
                                     const CalleeTypeInfo &calleeTypeInfo,
                                     SILLocation loc, SGFContext evalContext) {
  ResultPlanBuilder builder(SGF, loc, calleeTypeInfo);
  return builder.buildTopLevelResult(evalContext.getEmitInto(), loc);
}
