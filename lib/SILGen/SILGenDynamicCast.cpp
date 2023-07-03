//===--- SILGenDynamicCast.cpp - SILGen for dynamic casts -----------------===//
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

#include "SILGenDynamicCast.h"

#include "Initialization.h"
#include "RValue.h"
#include "Scope.h"
#include "ExitableFullExpr.h"
#include "swift/SIL/DynamicCasts.h"
#include "swift/SIL/SILArgument.h"
#include "swift/SIL/TypeLowering.h"

using namespace swift;
using namespace Lowering;

namespace {
  class CheckedCastEmitter {
    SILGenFunction &SGF;
    SILLocation Loc;
    CanType SourceType;
    CanType TargetType;

    enum class CastStrategy : uint8_t {
      Address,
      Scalar,
    };
    CastStrategy Strategy;

  public:
    CheckedCastEmitter(SILGenFunction &SGF, SILLocation loc,
                       Type sourceType, Type targetType)
      : SGF(SGF), Loc(loc), SourceType(sourceType->getCanonicalType()),
        TargetType(targetType->getCanonicalType()),
        Strategy(computeStrategy()) {
    }

    bool isOperandIndirect() const {
      return Strategy == CastStrategy::Address;
    }

    ManagedValue emitOperand(Expr *operand) {
      AbstractionPattern mostGeneral = SGF.SGM.Types.getMostGeneralAbstraction();
      auto &origSourceTL = SGF.getTypeLowering(mostGeneral, SourceType);

      SGFContext ctx;

      std::unique_ptr<TemporaryInitialization> temporary;
      if (isOperandIndirect()) {
        temporary = SGF.emitTemporary(Loc, origSourceTL);
        ctx = SGFContext(temporary.get());
      }

      auto result = SGF.emitRValueAsOrig(operand, mostGeneral,
                                         origSourceTL, ctx);

      if (isOperandIndirect()) {
        // Force the result into the temporary if it's not already there.
        if (!result.isInContext()) {
          result.forwardInto(SGF, Loc, temporary->getAddress());
          temporary->finishInitialization(SGF);
        }
        return temporary->getManagedAddress();
      }

      return result;
    }

    RValue emitUnconditionalCast(ManagedValue operand, SGFContext ctx) {
      // The cast functions don't know how to work with anything but
      // the most general possible abstraction level.
      AbstractionPattern abstraction = SGF.SGM.Types.getMostGeneralAbstraction();
      auto &origTargetTL = SGF.getTypeLowering(abstraction, TargetType);
      auto &substTargetTL = SGF.getTypeLowering(TargetType);
      bool hasAbstraction =
        (origTargetTL.getLoweredType() != substTargetTL.getLoweredType());

      // If we're using checked_cast_addr, take the operand (which
      // should be an address) and build into the destination buffer.
      if (Strategy == CastStrategy::Address) {
        SILValue resultBuffer =
          createAbstractResultBuffer(hasAbstraction, origTargetTL, ctx);
        SGF.B.createUnconditionalCheckedCastAddr(Loc,
                                             operand.forward(SGF), SourceType,
                                             resultBuffer, TargetType);
        return RValue(SGF, Loc, TargetType,
                      finishFromResultBuffer(hasAbstraction, resultBuffer,
                                             abstraction, origTargetTL, ctx));
      }

      ManagedValue result =
        SGF.B.createUnconditionalCheckedCast(Loc, operand,
                                             origTargetTL.getLoweredType(),
                                             TargetType);
      return RValue(SGF, Loc, TargetType,
                    finishFromResultScalar(hasAbstraction, result,
                                           CastConsumptionKind::TakeAlways,
                                           abstraction, origTargetTL, ctx));
    }

    /// Emit a conditional cast.
    void emitConditional(
        ManagedValue operand, CastConsumptionKind consumption, SGFContext ctx,
        llvm::function_ref<void(ManagedValue)> handleTrue,
        llvm::function_ref<void(llvm::Optional<ManagedValue>)> handleFalse,
        ProfileCounter TrueCount = ProfileCounter(),
        ProfileCounter FalseCount = ProfileCounter()) {
      // The cast instructions don't know how to work with anything
      // but the most general possible abstraction level.
      AbstractionPattern abstraction =
          SGF.SGM.Types.getMostGeneralAbstraction();
      auto &origTargetTL = SGF.getTypeLowering(abstraction, TargetType);
      auto &substTargetTL = SGF.getTypeLowering(TargetType);
      bool hasAbstraction =
          (origTargetTL.getLoweredType() != substTargetTL.getLoweredType());

      SILBasicBlock *falseBB = SGF.B.splitBlockForFallthrough();
      SILBasicBlock *trueBB = SGF.B.splitBlockForFallthrough();

      // Emit the branch.
      ManagedValue operandValue;
      SILValue resultBuffer;
      if (Strategy == CastStrategy::Address) {
        assert(operand.getType().isAddress());
        resultBuffer =
            createAbstractResultBuffer(hasAbstraction, origTargetTL, ctx);
        SGF.B.createCheckedCastAddrBranch(
            Loc, consumption, operand.forward(SGF), SourceType, resultBuffer,
            TargetType, trueBB, falseBB, TrueCount, FalseCount);
      } else {
        // Tolerate being passed an address here.  It comes up during switch
        // emission.
        operandValue = std::move(operand);
        if (operandValue.getType().isAddress()) {
          operandValue = SGF.B.createLoadTake(Loc, operandValue);
        }

        // If we are not supposed to destroy this value on failure, then we need
        // to borrow it.
        if (!shouldDestroyOnFailure(consumption)) {
          operandValue = operandValue.borrow(SGF, Loc);
        }
        SGF.B.createCheckedCastBranch(Loc, /*exact*/ false, operandValue,
                                      origTargetTL.getLoweredType(), TargetType,
                                      trueBB, falseBB, TrueCount, FalseCount);
      }

      // Emit the success block.
      SGF.B.setInsertionPoint(trueBB);
      {
        FullExpr scope(SGF.Cleanups, CleanupLocation(Loc));

        ManagedValue result;
        if (Strategy == CastStrategy::Address) {
          result = finishFromResultBuffer(hasAbstraction, resultBuffer,
                                          abstraction, origTargetTL, ctx);
        } else {
          // If we had copy_on_success, then we need to use a guaranteed
          // argument.
          assert(!shouldTakeOnSuccess(consumption)
                 || operandValue.getOwnershipKind().isCompatibleWith(
                        OwnershipKind::Owned)
                        && "cast consumption does not match ownership");
          ManagedValue termResult =
              SGF.B.createForwardedTermResult(origTargetTL.getLoweredType());
          result =
              finishFromResultScalar(hasAbstraction, termResult, consumption,
                                     abstraction, origTargetTL, ctx);
        }

        handleTrue(result);
        assert(!SGF.B.hasValidInsertionPoint() && "handler did not end block");
      }

      // Emit the failure block.
      SGF.B.setInsertionPoint(falseBB);
      {
        FullExpr scope(SGF.Cleanups, CleanupLocation(Loc));

        // If we have an address only type, do not handle the consumption
        // rules. These are handled for us by the user.
        if (Strategy == CastStrategy::Address) {
          handleFalse(llvm::None);
          assert(!SGF.B.hasValidInsertionPoint() &&
                 "handler did not end block");
          return;
        }

        // Otherwise, we use the following strategy:
        //
        // 1. If we have a take_always, we create a phi node argument for the
        // failure case and a scope for that so that it is immediately
        // destroyed.
        //
        // 2. If we have a take_on_success or copy_on_success, then on failure,
        // we propagate through the default argument, but do not clean it up. On
        // the false case, our user must treat the taken value as a new value.
        if (shouldDestroyOnFailure(consumption)) {
          {
            FullExpr argScope(SGF.Cleanups, CleanupLocation(Loc));
            SGF.B.createForwardedTermResult(operandValue.getType());
          }
          handleFalse(llvm::None);
          assert(!SGF.B.hasValidInsertionPoint() &&
                 "handler did not end block");
          return;
        }
        ManagedValue result =
            SGF.B.createForwardedTermResult(operandValue.getType());
        switch (consumption) {
        case CastConsumptionKind::BorrowAlways:
        case CastConsumptionKind::CopyOnSuccess:
          handleFalse(llvm::None);
          break;
        case CastConsumptionKind::TakeAlways:
        case CastConsumptionKind::TakeOnSuccess:
          handleFalse(result);
          break;
        }

        assert(!SGF.B.hasValidInsertionPoint() && "handler did not end block");
      }
    }

    SILValue createAbstractResultBuffer(bool hasAbstraction,
                                        const TypeLowering &origTargetTL,
                                        SGFContext ctx) {
      if (!hasAbstraction) {
        if (auto address = ctx.getAddressForInPlaceInitialization(SGF, Loc))
          return address;
      }

      return SGF.emitTemporaryAllocation(Loc, origTargetTL.getLoweredType());
    }

    ManagedValue finishFromResultBuffer(bool hasAbstraction, SILValue buffer,
                                        AbstractionPattern abstraction,
                                        const TypeLowering &origTargetTL,
                                        SGFContext ctx) {
      if (!hasAbstraction) {
        if (ctx.finishInPlaceInitialization(SGF))
          return ManagedValue::forInContext();
      }

      ManagedValue result;
      if (!origTargetTL.isAddressOnly() || !SGF.useLoweredAddresses()) {
        result = SGF.emitLoad(Loc, buffer, origTargetTL, ctx, IsTake);
      } else {
        result = SGF.emitManagedBufferWithCleanup(buffer, origTargetTL);
      }

      if (hasAbstraction) {
        result =
            SGF.emitOrigToSubstValue(Loc, result, abstraction, TargetType, ctx);
      }
      return result;
    }

    /// Our cast succeeded and gave us this abstracted value.
    ManagedValue finishFromResultScalar(bool hasAbstraction, ManagedValue value,
                                        CastConsumptionKind consumption,
                                        AbstractionPattern abstraction,
                                        const TypeLowering &origTargetTL,
                                        SGFContext ctx) {
      ManagedValue result = value;
      // Copy the result if this is copy-on-success.
      if (!shouldTakeOnSuccess(consumption))
        result = result.copy(SGF, Loc);

      // Re-abstract if necessary.
      if (hasAbstraction) {
        result =
            SGF.emitOrigToSubstValue(Loc, result, abstraction, TargetType, ctx);
      }

      return result;
    }

  private:
    CastStrategy computeStrategy() const {
      if (canSILUseScalarCheckedCastInstructions(SGF.SGM.M, SourceType,
                                                 TargetType))
        return CastStrategy::Scalar;
      return CastStrategy::Address;
    }
  };
} // end anonymous namespace

void SILGenFunction::emitCheckedCastBranch(
    SILLocation loc, Expr *source, Type targetType, SGFContext ctx,
    llvm::function_ref<void(ManagedValue)> handleTrue,
    llvm::function_ref<void(llvm::Optional<ManagedValue>)> handleFalse,
    ProfileCounter TrueCount, ProfileCounter FalseCount) {
  CheckedCastEmitter emitter(*this, loc, source->getType(), targetType);
  ManagedValue operand = emitter.emitOperand(source);
  emitter.emitConditional(operand, CastConsumptionKind::TakeAlways, ctx,
                          handleTrue, handleFalse, TrueCount, FalseCount);
}

void SILGenFunction::emitCheckedCastBranch(
    SILLocation loc, ConsumableManagedValue src, Type sourceType,
    CanType targetType, SGFContext ctx,
    llvm::function_ref<void(ManagedValue)> handleTrue,
    llvm::function_ref<void(llvm::Optional<ManagedValue>)> handleFalse,
    ProfileCounter TrueCount, ProfileCounter FalseCount) {
  CheckedCastEmitter emitter(*this, loc, sourceType, targetType);
  emitter.emitConditional(src.getFinalManagedValue(), src.getFinalConsumption(),
                          ctx, handleTrue, handleFalse, TrueCount, FalseCount);
}

/// Emit a collection downcast expression.
///
/// \param conditional Whether to emit a conditional downcast; if
/// false, this will emit a forced downcast.
static RValue emitCollectionDowncastExpr(SILGenFunction &SGF,
                                         ManagedValue source,
                                         Type sourceType,
                                         SILLocation loc,
                                         Type destType,
                                         SGFContext C,
                                         bool conditional) {
  // Compute substitutions for the intrinsic call.
  auto fromCollection = sourceType->getCanonicalType();
  auto toCollection = destType->getCanonicalType();
  // Get the intrinsic function.
  FuncDecl *fn = nullptr;
  if (fromCollection->isArray()) {
    fn = conditional ? SGF.SGM.getArrayConditionalCast(loc)
                     : SGF.SGM.getArrayForceCast(loc);
  } else if (fromCollection->isDictionary()) {
    fn = (conditional
           ? SGF.SGM.getDictionaryDownCastConditional(loc)
           : SGF.SGM.getDictionaryDownCast(loc));
  } else if (fromCollection->isSet()) {
    fn = (conditional
           ? SGF.SGM.getSetDownCastConditional(loc)
           : SGF.SGM.getSetDownCast(loc));
  } else {
    llvm_unreachable("unsupported collection upcast kind");
  }

  return SGF.emitCollectionConversion(loc, fn, fromCollection, toCollection,
                                      source, C);
}

static ManagedValue
adjustForConditionalCheckedCastOperand(SILLocation loc, ManagedValue src,
                                       CanType sourceType, CanType targetType,
                                       SILGenFunction &SGF) {
  // Reabstract to the most general abstraction, and put it into a
  // temporary if necessary.
  
  // Figure out if we need the value to be in a temporary.
  bool requiresAddress =
    !canSILUseScalarCheckedCastInstructions(SGF.SGM.M, sourceType, targetType);
  
  AbstractionPattern abstraction = SGF.SGM.M.Types.getMostGeneralAbstraction();
  auto &srcAbstractTL = SGF.getTypeLowering(abstraction, sourceType);
  
  bool hasAbstraction = (src.getType() != srcAbstractTL.getLoweredType());
  
  // Fast path: no re-abstraction required.
  if (!hasAbstraction && (!requiresAddress || src.getType().isAddress()))
    return src;
  
  std::unique_ptr<TemporaryInitialization> init;
  if (requiresAddress) {
    init = SGF.emitTemporary(loc, srcAbstractTL);

    if (hasAbstraction)
      src = SGF.emitSubstToOrigValue(loc, src, abstraction, sourceType);

    // Okay, if all we need to do is drop the value in an address,
    // this is easy.
    SGF.B.emitStoreValueOperation(loc, src.forward(SGF), init->getAddress(),
                                  StoreOwnershipQualifier::Init);
    init->finishInitialization(SGF);
    return init->getManagedAddress();
  }
  
  assert(hasAbstraction);
  assert(src.getType().isObject() &&
         "address-only type with abstraction difference?");
  
  // Produce the value at +1.
  return SGF.emitSubstToOrigValue(loc, src, abstraction, sourceType);
}


RValue Lowering::emitUnconditionalCheckedCast(SILGenFunction &SGF,
                                              SILLocation loc,
                                              Expr *operand,
                                              Type targetType,
                                              CheckedCastKind castKind,
                                              SGFContext C) {
  // Handle collection downcasts directly; they have specific library
  // entry points.
  if (castKind == CheckedCastKind::ArrayDowncast ||
      castKind == CheckedCastKind::DictionaryDowncast ||
      castKind == CheckedCastKind::SetDowncast) {
    ManagedValue operandMV = SGF.emitRValueAsSingleValue(operand);
    return emitCollectionDowncastExpr(SGF, operandMV, operand->getType(), loc,
                                      targetType, C,
                                      /*conditional=*/false);
  }

  CheckedCastEmitter emitter(SGF, loc, operand->getType(),
                             targetType);
  ManagedValue operandValue = emitter.emitOperand(operand);
  return emitter.emitUnconditionalCast(operandValue, C);
}

RValue Lowering::emitConditionalCheckedCast(
    SILGenFunction &SGF, SILLocation loc, ManagedValue operand,
    Type operandType, Type optTargetType, CheckedCastKind castKind,
    SGFContext C, ProfileCounter TrueCount, ProfileCounter FalseCount) {
  // Drill into the result type.
  CanType resultObjectType =
      optTargetType->getCanonicalType().getOptionalObjectType();
  assert(resultObjectType);

  // Handle collection downcasts directly; they have specific library
  // entry points.
  if (castKind == CheckedCastKind::ArrayDowncast ||
      castKind == CheckedCastKind::DictionaryDowncast ||
      castKind == CheckedCastKind::SetDowncast) {
    return emitCollectionDowncastExpr(SGF, operand, operandType, loc,
                                      resultObjectType, C,
                                      /*conditional=*/true);
  }

  operand = adjustForConditionalCheckedCastOperand(loc, operand,
                                               operandType->getCanonicalType(),
                                                   resultObjectType, SGF);

  auto someDecl = SGF.getASTContext().getOptionalSomeDecl();
  auto &resultTL = SGF.getTypeLowering(optTargetType);

  // Set up a result buffer if desirable/required.
  SILValue resultBuffer;
  SILValue resultObjectBuffer;
  llvm::Optional<TemporaryInitialization> resultObjectTemp;
  SGFContext resultObjectCtx;
  if ((resultTL.isAddressOnly() && SGF.useLoweredAddresses())
      || (C.getEmitInto()
          && C.getEmitInto()->canPerformInPlaceInitialization())) {
    SILType resultTy = resultTL.getLoweredType();
    resultBuffer = SGF.getBufferForExprResult(loc, resultTy, C);
    resultObjectBuffer = SGF.B.createInitEnumDataAddr(
        loc, resultBuffer, someDecl,
        resultTy.getOptionalObjectType().getAddressType());
    resultObjectTemp.emplace(resultObjectBuffer, CleanupHandle::invalid());
    resultObjectCtx = SGFContext(&resultObjectTemp.value());
  }

  // Prepare a jump destination here.
  ExitableFullExpr scope(SGF, CleanupLocation(loc));

  auto operandCMV = ConsumableManagedValue::forOwned(operand);
  assert(operandCMV.getFinalConsumption() == CastConsumptionKind::TakeAlways);

  SGF.emitCheckedCastBranch(
      loc, operandCMV, operandType, resultObjectType, resultObjectCtx,
      // The success path.
      [&](ManagedValue objectValue) {
        // If we're not emitting into a temporary, just wrap up the result
        // in Some and go to the continuation block.
        if (!resultObjectTemp) {
          auto some = SGF.B.createEnum(loc, objectValue.forward(SGF), someDecl,
                                       resultTL.getLoweredType());
          SGF.Cleanups.emitBranchAndCleanups(scope.getExitDest(), loc, {some});
          return;
        }

        // Otherwise, make sure the value is in the context.
        if (!objectValue.isInContext()) {
          objectValue.forwardInto(SGF, loc, resultObjectBuffer);
        }
        SGF.B.createInjectEnumAddr(loc, resultBuffer, someDecl);
        SGF.Cleanups.emitBranchAndCleanups(scope.getExitDest(), loc);
      },
      // The failure path.
      [&](llvm::Optional<ManagedValue> Value) {
        // We always are performing a take here, so Value should be llvm::None
        // since the object should have been destroyed immediately in the fail
        // block.
        assert(!Value.has_value() && "Expected a take_always consumption kind");
        auto noneDecl = SGF.getASTContext().getOptionalNoneDecl();

        // If we're not emitting into a temporary, just wrap up the result
        // in None and go to the continuation block.
        if (!resultObjectTemp) {
          auto none = SGF.B.createEnum(loc, nullptr, noneDecl,
                                       resultTL.getLoweredType());
          SGF.Cleanups.emitBranchAndCleanups(scope.getExitDest(), loc, {none});

          // Just construct the enum directly in the context.
        } else {
          SGF.B.createInjectEnumAddr(loc, resultBuffer, noneDecl);
          SGF.Cleanups.emitBranchAndCleanups(scope.getExitDest(), loc);
        }
      },
      TrueCount, FalseCount);

  // Enter the continuation block.
  SILBasicBlock *contBlock = scope.exit();

  ManagedValue result;
  if (resultObjectTemp) {
    result = SGF.manageBufferForExprResult(resultBuffer, resultTL, C);
  } else {
    auto argument = contBlock->createPhiArgument(resultTL.getLoweredType(),
                                                 OwnershipKind::Owned);
    result = SGF.emitManagedRValueWithCleanup(argument, resultTL);
  }

  return RValue(SGF, loc, optTargetType->getCanonicalType(), result);
}

SILValue Lowering::emitIsa(SILGenFunction &SGF, SILLocation loc,
                           Expr *operand, Type targetType,
                           CheckedCastKind castKind) {
  // Handle collection downcasts separately.
  if (castKind == CheckedCastKind::ArrayDowncast ||
      castKind == CheckedCastKind::DictionaryDowncast ||
      castKind == CheckedCastKind::SetDowncast) {
    ManagedValue operandMV = SGF.emitRValueAsSingleValue(operand);
    ManagedValue optValue = emitCollectionDowncastExpr(
                              SGF, operandMV, operand->getType(), loc,
                              targetType,
                              SGFContext(), /*conditional=*/true)
      .getAsSingleValue(SGF, loc);

    // Materialize the input.
    SILValue optValueTemp;
    if (optValue.getType().isAddress()) {
      optValueTemp = optValue.forward(SGF);
    } else {
      optValueTemp = SGF.emitTemporaryAllocation(loc, optValue.getType());
      optValue.forwardInto(SGF, loc, optValueTemp);
    }

    return SGF.emitDoesOptionalHaveValue(loc, optValueTemp);
  }

  // Prepare a jump destination here.
  ExitableFullExpr scope(SGF, CleanupLocation(loc));

  auto i1Ty = SILType::getBuiltinIntegerType(1, SGF.getASTContext());

  // When we pass in an expr, we perform a take_always cast.
  SGF.emitCheckedCastBranch(
      loc, operand, targetType, SGFContext(),
      [&](ManagedValue value) {
        SILValue yes = SGF.B.createIntegerLiteral(loc, i1Ty, 1);
        SGF.Cleanups.emitBranchAndCleanups(scope.getExitDest(), loc, yes);
      },
      [&](llvm::Optional<ManagedValue> Value) {
        assert(!Value.has_value() && "Expected take_always semantics");
        SILValue no = SGF.B.createIntegerLiteral(loc, i1Ty, 0);
        SGF.Cleanups.emitBranchAndCleanups(scope.getExitDest(), loc, no);
      });

  auto contBB = scope.exit();
  auto isa = contBB->createPhiArgument(i1Ty, OwnershipKind::None);
  return isa;
}

