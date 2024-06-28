//===--- SILGenConvert.cpp - Type Conversion Routines ---------------------===//
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

#include "SILGen.h"
#include "ArgumentSource.h"
#include "Conversion.h"
#include "Initialization.h"
#include "LValue.h"
#include "RValue.h"
#include "Scope.h"
#include "SwitchEnumBuilder.h"
#include "swift/AST/ASTContext.h"
#include "swift/AST/Decl.h"
#include "swift/AST/ProtocolConformance.h"
#include "swift/AST/SubstitutionMap.h"
#include "swift/AST/Types.h"
#include "swift/Basic/Assertions.h"
#include "swift/Basic/SourceManager.h"
#include "swift/Basic/type_traits.h"
#include "swift/SIL/SILArgument.h"
#include "swift/SIL/TypeLowering.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/Support/SaveAndRestore.h"
#include "llvm/Support/raw_ostream.h"

using namespace swift;
using namespace Lowering;

// FIXME: With some changes to their callers, all of the below functions
// could be re-worked to use emitInjectEnum().
ManagedValue
SILGenFunction::emitInjectOptional(SILLocation loc,
                                   const TypeLowering &optTL,
                                   SGFContext ctxt,
                      llvm::function_ref<ManagedValue(SGFContext)> generator) {
  SILType optTy = optTL.getLoweredType();
  SILType objectTy = optTy.getOptionalObjectType();
  assert(objectTy && "expected type was not optional");

  auto someDecl = getASTContext().getOptionalSomeDecl();

  // If the value is loadable, just emit and wrap.
  // TODO: honor +0 contexts?
  if (optTL.isLoadable() || !silConv.useLoweredAddresses()) {
    ManagedValue objectResult = generator(SGFContext());
    return B.createEnum(loc, objectResult, someDecl, optTy);
  }

  // Otherwise it's address-only; try to avoid spurious copies by
  // evaluating into the context.

  // Prepare a buffer for the object value.
  return B.bufferForExpr(
      loc, optTy.getObjectType(), optTL, ctxt,
      [&](SILValue optBuf) {
        auto objectBuf = B.createInitEnumDataAddr(loc, optBuf, someDecl, objectTy);

        // Evaluate the value in-place into that buffer.
        TemporaryInitialization init(objectBuf, CleanupHandle::invalid());
        ManagedValue objectResult = generator(SGFContext(&init));
        if (!objectResult.isInContext()) {
          objectResult.ensurePlusOne(*this, loc)
              .forwardInto(*this, loc, objectBuf);
        }

        // Finalize the outer optional buffer.
        B.createInjectEnumAddr(loc, optBuf, someDecl);
      });
}

void SILGenFunction::emitInjectOptionalValueInto(SILLocation loc,
                                                 ArgumentSource &&value,
                                                 SILValue dest,
                                                 const TypeLowering &optTL) {
  SILType optType = optTL.getLoweredType();
  assert(dest->getType() == optType.getAddressType());
  auto loweredPayloadTy = optType.getOptionalObjectType();
  assert(loweredPayloadTy);

  // Project out the payload area.
  auto someDecl = getASTContext().getOptionalSomeDecl();
  auto destPayload =
    B.createInitEnumDataAddr(loc, dest, someDecl,
                             loweredPayloadTy.getAddressType());
  
  // Emit the value into the payload area.
  TemporaryInitialization emitInto(destPayload, CleanupHandle::invalid());
  std::move(value).forwardInto(*this, &emitInto);
  
  // Inject the tag.
  B.createInjectEnumAddr(loc, dest, someDecl);
}

void SILGenFunction::emitInjectOptionalNothingInto(SILLocation loc, 
                                                   SILValue dest,
                                                   const TypeLowering &optTL) {
  assert(optTL.getLoweredType().getOptionalObjectType());

  B.createInjectEnumAddr(loc, dest, getASTContext().getOptionalNoneDecl());
}
      
/// Return a value for an optional ".None" of the specified type. This only
/// works for loadable enum types.
SILValue SILGenFunction::getOptionalNoneValue(SILLocation loc,
                                              const TypeLowering &optTL) {
  assert((optTL.isLoadable() || !silConv.useLoweredAddresses()) &&
         "Address-only optionals cannot use this");
  assert(optTL.getLoweredType().getOptionalObjectType());

  return B.createEnum(loc, SILValue(), getASTContext().getOptionalNoneDecl(),
                      optTL.getLoweredType());
}

/// Return a value for an optional ".Some(x)" of the specified type. This only
/// works for loadable enum types.
ManagedValue SILGenFunction::
getOptionalSomeValue(SILLocation loc, ManagedValue value,
                     const TypeLowering &optTL) {
  assert((optTL.isLoadable() || !silConv.useLoweredAddresses()) &&
         "Address-only optionals cannot use this");
  SILType optType = optTL.getLoweredType();
  auto formalOptType = optType.getASTType();
  (void)formalOptType;

  assert(formalOptType.getOptionalObjectType());
  auto someDecl = getASTContext().getOptionalSomeDecl();

  return B.createEnum(loc, value, someDecl, optTL.getLoweredType());
}

auto SILGenFunction::emitSourceLocationArgs(SourceLoc sourceLoc,
                                            SILLocation emitLoc)
-> SourceLocArgs {
  auto &ctx = getASTContext();
  
  std::string filename = "";
  unsigned line = 0;
  unsigned column = 0;
  if (sourceLoc.isValid()) {
    filename = getMagicFileIDString(sourceLoc);
    std::tie(line, column) =
        ctx.SourceMgr.getPresumedLineAndColumnForLoc(sourceLoc);
  }
  
  bool isASCII = true;
  for (unsigned char c : filename) {
    if (c > 127) {
      isASCII = false;
      break;
    }
  }
  
  auto wordTy = SILType::getBuiltinWordType(ctx);
  auto i1Ty = SILType::getBuiltinIntegerType(1, ctx);
  
  SourceLocArgs result;
  SILValue literal = B.createStringLiteral(emitLoc, StringRef(filename),
                                           StringLiteralInst::Encoding::UTF8);
  result.filenameStartPointer =
      ManagedValue::forObjectRValueWithoutOwnership(literal);
  // File length
  literal = B.createIntegerLiteral(emitLoc, wordTy, filename.size());
  result.filenameLength =
      ManagedValue::forObjectRValueWithoutOwnership(literal);
  // File is ascii
  literal = B.createIntegerLiteral(emitLoc, i1Ty, isASCII);
  result.filenameIsAscii =
      ManagedValue::forObjectRValueWithoutOwnership(literal);
  // Line
  literal = B.createIntegerLiteral(emitLoc, wordTy, line);
  result.line = ManagedValue::forObjectRValueWithoutOwnership(literal);
  // Column
  literal = B.createIntegerLiteral(emitLoc, wordTy, column);
  result.column = ManagedValue::forObjectRValueWithoutOwnership(literal);

  return result;
}

ManagedValue
SILGenFunction::emitPreconditionOptionalHasValue(SILLocation loc,
                                                 ManagedValue optional,
                                                 bool isImplicitUnwrap) {
  // Generate code to the optional is present, and if not, abort with a message
  // (provided by the stdlib).
  SILBasicBlock *contBB = createBasicBlock();
  SILBasicBlock *failBB = createBasicBlock();

  bool hadCleanup = optional.hasCleanup();
  bool hadLValue = optional.isLValue();

  auto someDecl = getASTContext().getOptionalSomeDecl();
  auto noneDecl = getASTContext().getOptionalNoneDecl();

  bool isAddress = optional.getType().isAddress();
  bool isBorrow = !optional.isPlusOneOrTrivial(*this);
  SwitchEnumInst *switchEnum = nullptr;
  if (isAddress) {
    // We forward in the creation routine for
    // unchecked_take_enum_data_addr. switch_enum_addr is a +0 operation.
    B.createSwitchEnumAddr(loc, optional.getValue(),
                           /*defaultDest*/ nullptr,
                           {{someDecl, contBB}, {noneDecl, failBB}});
  } else if (isBorrow) {
    hadCleanup = false;
    hadLValue = false;
    switchEnum = B.createSwitchEnum(loc, optional.getValue(),
                                    /*defaultDest*/ nullptr,
                                    {{someDecl, contBB}, {noneDecl, failBB}});
  } else {
    optional = optional.ensurePlusOne(*this, loc);
    hadCleanup = true;
    hadLValue = false;
    switchEnum = B.createSwitchEnum(loc, optional.forward(*this),
                                    /*defaultDest*/ nullptr,
                                    {{someDecl, contBB}, {noneDecl, failBB}});
  }
  B.emitBlock(failBB);

  // Call the standard library implementation of _diagnoseUnexpectedNilOptional.
  if (auto diagnoseFailure =
        getASTContext().getDiagnoseUnexpectedNilOptional()) {
    auto args = emitSourceLocationArgs(loc.getSourceLoc(), loc);
    
    auto i1Ty = SILType::getBuiltinIntegerType(1, getASTContext());
    auto isImplicitUnwrapLiteral =
      B.createIntegerLiteral(loc, i1Ty, isImplicitUnwrap);
    auto isImplicitUnwrapValue =
        ManagedValue::forObjectRValueWithoutOwnership(isImplicitUnwrapLiteral);

    emitApplyOfLibraryIntrinsic(loc, diagnoseFailure, SubstitutionMap(),
                                {
                                  args.filenameStartPointer,
                                  args.filenameLength,
                                  args.filenameIsAscii,
                                  args.line,
                                  isImplicitUnwrapValue
                                },
                                SGFContext());
  }

  B.createUnreachable(ArtificialUnreachableLocation());
  B.clearInsertionPoint();
  B.emitBlock(contBB);

  ManagedValue result;
  if (isAddress) {
    SILType payloadType = optional.getType().getOptionalObjectType();
    result =
        B.createUncheckedTakeEnumDataAddr(loc, optional, someDecl, payloadType);
  } else {
    result = B.createOptionalSomeResult(switchEnum);
  }

  if (hadCleanup) {
    return result;
  }

  if (hadLValue) {
    return ManagedValue::forLValue(result.forward(*this));
  }

  return ManagedValue::forBorrowedRValue(result.forward(*this));
}

SILValue SILGenFunction::emitDoesOptionalHaveValue(SILLocation loc,
                                                   SILValue addrOrValue) {
  auto boolTy = SILType::getBuiltinIntegerType(1, getASTContext());
  SILValue yes = B.createIntegerLiteral(loc, boolTy, 1);
  SILValue no = B.createIntegerLiteral(loc, boolTy, 0);
  auto someDecl = getASTContext().getOptionalSomeDecl();
  
  if (addrOrValue->getType().isAddress())
    return B.createSelectEnumAddr(loc, addrOrValue, boolTy, no,
                                  std::make_pair(someDecl, yes));
  return B.createSelectEnum(loc, addrOrValue, boolTy, no,
                            std::make_pair(someDecl, yes));
}

ManagedValue SILGenFunction::emitCheckedGetOptionalValueFrom(SILLocation loc,
                                                      ManagedValue src,
                                                      bool isImplicitUnwrap,
                                                      const TypeLowering &optTL,
                                                      SGFContext C) {
  // TODO: Make this take optTL.
  return emitPreconditionOptionalHasValue(loc, src, isImplicitUnwrap);
}

ManagedValue SILGenFunction::emitUncheckedGetOptionalValueFrom(
    SILLocation loc, ManagedValue addrOrValue, const TypeLowering &optTL,
    SGFContext C) {
  SILType origPayloadTy = addrOrValue.getType().getOptionalObjectType();

  auto someDecl = getASTContext().getOptionalSomeDecl();

  // Take the payload from the optional.
  if (!addrOrValue.getType().isAddress()) {
    return B.createUncheckedEnumData(loc, addrOrValue, someDecl);
  }

  // Cheat a bit in the +0 case--UncheckedTakeEnumData will never actually
  // invalidate an Optional enum value. This is specific to optionals.
  ManagedValue payload = B.createUncheckedTakeEnumDataAddr(
      loc, addrOrValue, someDecl, origPayloadTy);
  if (!optTL.isLoadable())
    return payload;

  // If we do not have a cleanup on our address, use a load_borrow.
  if (!payload.hasCleanup()) {
    return B.createLoadBorrow(loc, payload);
  }

  // Otherwise, perform a load take.
  return B.createLoadTake(loc, payload);
}

ManagedValue
SILGenFunction::emitOptionalSome(SILLocation loc, SILType optTy,
                                 ValueProducerRef produceValue,
                                 SGFContext C) {
  // If we're emitting into a conversion, try to peephole the
  // injection into it.
  if (auto optInit = C.getAsConversion()) {
    const auto &optConversion = optInit->getConversion();

    auto adjustment = optConversion.adjustForInitialOptionalInjection();

    // If the adjustment gives us a conversion that produces an optional
    // value, that completely takes over emission.  This generally happens
    // only because of bridging.
    if (adjustment.isInjection()) {
      return optInit->emitWithAdjustedConversion(*this, loc,
                                      adjustment.getInjectionConversion(),
                                                 produceValue);

    // If the adjustment gives us a conversion that produces a non-optional
    // value, we need to produce the value under that conversion and then
    // inject that into an optional.  We can do that by recursing.  This
    // will terminate because the recursive call to emitOptionalSome gets
    // passed a strictly "smaller" context: the parent context of the
    // converting context we were passed.
    } else if (adjustment.isValue()) {
      auto produceConvertedValue = [&](SILGenFunction &SGF,
                                       SILLocation loc,
                                       SGFContext C) {
        return SGF.emitConvertedRValue(loc, adjustment.getValueConversion(),
                                       C, produceValue);
      };
      auto result = emitOptionalSome(loc, optConversion.getLoweredResultType(),
                                     produceConvertedValue,
                                     optInit->getFinalContext());
      optInit->initWithConvertedValue(*this, loc, result);
      optInit->finishInitialization(*this);
      return ManagedValue::forInContext();
    }
  }

  auto &optTL = getTypeLowering(optTy);

  // If the type is loadable or we're not lowering address-only types
  // in SILGen, use a simple scalar pattern.
  if (!silConv.useLoweredAddresses() || optTL.isLoadable()) {
    auto value = produceValue(*this, loc, SGFContext());
    return getOptionalSomeValue(loc, value, optTL);
  }

  // Otherwise, emit into memory, preferably into an address from
  // the context.

  // Get an address to emit into.
  SILValue optAddr = getBufferForExprResult(loc, optTy, C);

  auto someDecl = getASTContext().getOptionalSomeDecl();

  auto valueTy = optTy.getOptionalObjectType();
  auto &valueTL = getTypeLowering(valueTy);

  // Project the value buffer within the address.
  SILValue valueAddr =
    B.createInitEnumDataAddr(loc, optAddr, someDecl,
                             valueTy.getAddressType());

  // Emit into the value buffer.
  auto valueInit = useBufferAsTemporary(valueAddr, valueTL);
  ManagedValue value = produceValue(*this, loc, SGFContext(valueInit.get()));
  if (!value.isInContext()) {
    valueInit->copyOrInitValueInto(*this, loc, value, /*isInit*/ true);
    valueInit->finishInitialization(*this);
  }

  // Kill the cleanup on the value.
  valueInit->getManagedAddress().forward(*this);

  // Finish the optional.
  B.createInjectEnumAddr(loc, optAddr, someDecl);

  return manageBufferForExprResult(optAddr, optTL, C);
}

/// Emit an optional-to-optional transformation.
ManagedValue
SILGenFunction::emitOptionalToOptional(SILLocation loc,
                                       ManagedValue input,
                                       SILType resultTy,
                                       ValueTransformRef transformValue,
                                       SGFContext C) {
  auto &Ctx = getASTContext();

  // If the input is known to be 'none' just emit a 'none' value of the right
  // result type right away.
  auto &resultTL = getTypeLowering(resultTy);

  if (auto *EI = dyn_cast<EnumInst>(input.getValue())) {
    if (EI->getElement() == Ctx.getOptionalNoneDecl()) {
      if (!(resultTL.isAddressOnly() && silConv.useLoweredAddresses())) {
        SILValue none = B.createEnum(loc, SILValue(), EI->getElement(),
                                     resultTy);
        return emitManagedRValueWithCleanup(none);
      }
    }
  }

  // Otherwise perform a dispatch.
  auto contBB = createBasicBlock();
  auto isNotPresentBB = createBasicBlock();
  auto isPresentBB = createBasicBlock();

  // All conversions happen at +1.
  input = input.ensurePlusOne(*this, loc);

  SwitchEnumBuilder SEBuilder(B, loc, input);
  SILType noOptResultTy = resultTy.getOptionalObjectType();
  assert(noOptResultTy);

  // Create a temporary for the output optional.
  //
  // If the result is address-only, we need to return something in memory,
  // otherwise the result is the BBArgument in the merge point.
  // TODO: use the SGFContext passed in.
  ManagedValue resultAddress;
  bool addressOnly = resultTL.isAddressOnly() && silConv.useLoweredAddresses();
  if (addressOnly) {
    resultAddress = emitManagedBufferWithCleanup(
        emitTemporaryAllocation(loc, resultTy), resultTL);
  }

  ValueOwnershipKind resultOwnership = OwnershipKind::Any;
  SEBuilder.addOptionalSomeCase(
      isPresentBB, contBB, [&](ManagedValue input, SwitchCaseFullExpr &&scope) {
        // If we have an address only type, we want to match the old behavior of
        // transforming the underlying type instead of the optional type. This
        // ensures that we use the more efficient non-generic code paths when
        // possible.
        if (getTypeLowering(input.getType()).isAddressOnly() &&
            silConv.useLoweredAddresses()) {
          auto *someDecl = Ctx.getOptionalSomeDecl();
          input = B.createUncheckedTakeEnumDataAddr(
              loc, input, someDecl, input.getType().getOptionalObjectType());
        }

        ManagedValue result = transformValue(*this, loc, input, noOptResultTy,
                                             SGFContext());
        resultOwnership = result.getValue()->getOwnershipKind();
        if (!addressOnly) {
          SILValue some = B.createOptionalSome(loc, result).forward(*this);
          return scope.exitAndBranch(loc, some);
        }

        RValue R(*this, loc, noOptResultTy.getASTType(), result);
        ArgumentSource resultValueRV(loc, std::move(R));
        emitInjectOptionalValueInto(loc, std::move(resultValueRV),
                                    resultAddress.getValue(), resultTL);
        return scope.exitAndBranch(loc);
      });

  SEBuilder.addOptionalNoneCase(
      isNotPresentBB, contBB,
      [&](ManagedValue input, SwitchCaseFullExpr &&scope) {
        if (!addressOnly) {
          SILValue none =
              B.createManagedOptionalNone(loc, resultTy).forward(*this);
          return scope.exitAndBranch(loc, none);
        }

        emitInjectOptionalNothingInto(loc, resultAddress.getValue(), resultTL);
        return scope.exitAndBranch(loc);
      });

  std::move(SEBuilder).emit();

  B.emitBlock(contBB);
  if (addressOnly)
    return resultAddress;

  // This phi's ownership is derived from the transformed value's
  // ownership, not the input ownership. Transformation can convert a value with
  // no ownership to an owned value.
  return B.createPhi(resultTL.getLoweredType(), resultOwnership);
}

SILGenFunction::OpaqueValueRAII::~OpaqueValueRAII() {
  auto entry = Self.OpaqueValues.find(OpaqueValue);
  assert(entry != Self.OpaqueValues.end());
  Self.OpaqueValues.erase(entry);
}

RValue
SILGenFunction::emitPointerToPointer(SILLocation loc,
                                     ManagedValue input,
                                     CanType inputType,
                                     CanType outputType,
                                     SGFContext C) {
  auto converter = getASTContext().getConvertPointerToPointerArgument();

  auto origValue = input;
  if (silConv.useLoweredAddresses()) {
    // The generic function currently always requires indirection, but pointers
    // are always loadable.
    auto origBuf = emitTemporaryAllocation(loc, input.getType());
    B.emitStoreValueOperation(loc, input.forward(*this), origBuf,
                              StoreOwnershipQualifier::Init);
    origValue = emitManagedBufferWithCleanup(origBuf);
  }
  // Invoke the conversion intrinsic to convert to the destination type.
  auto *M = SGM.M.getSwiftModule();
  auto *proto = getPointerProtocol();
  auto firstSubMap = inputType->getContextSubstitutionMap(M, proto);
  auto secondSubMap = outputType->getContextSubstitutionMap(M, proto);

  auto genericSig = converter->getGenericSignature();
  auto subMap =
    SubstitutionMap::combineSubstitutionMaps(firstSubMap,
                                             secondSubMap,
                                             CombineSubstitutionMaps::AtIndex,
                                             1, 0,
                                             genericSig);
  
  return emitApplyOfLibraryIntrinsic(loc, converter, subMap, origValue, C);
}


namespace {

/// This is an initialization for an address-only existential in memory.
class ExistentialInitialization final : public SingleBufferInitialization {
  SILValue existential;
  CanType concreteFormalType;
  ArrayRef<ProtocolConformanceRef> conformances;
  ExistentialRepresentation repr;
  
  // Initialized lazily when the address for initialization is demanded.
  SILValue concreteBuffer;
  CleanupHandle deinitExistentialCleanup;
public:
  /// \param existential The existential container
  /// \param concreteFormalType Unlowered AST type of value
  /// \param conformances Conformances for concrete type to existential's
  ///        protocols
  ExistentialInitialization(SILGenFunction &SGF,
                            SILValue existential,
                            CanType concreteFormalType,
                            ArrayRef<ProtocolConformanceRef> conformances,
                            ExistentialRepresentation repr)
    : existential(existential),
      concreteFormalType(concreteFormalType),
      conformances(conformances),
      repr(repr)
  {
    assert(existential->getType().isAddress());
    
    // Create a cleanup to deallocate an allocated but uninitialized concrete
    // type buffer.
    // It won't be activated until that buffer is formed later, though.
    deinitExistentialCleanup =
      SGF.enterDeinitExistentialCleanup(CleanupState::Dormant,
                                        existential, concreteFormalType, repr);

  }
  
  SILValue getAddressForInPlaceInitialization(SILGenFunction &SGF,
                                              SILLocation loc) override {
    // Create the buffer when needed, because in some cases the type may
    // be the opened type from another existential that hasn't been opened
    // at the point the existential destination was formed.
    assert(!concreteBuffer && "concrete buffer already formed?!");
    
    auto concreteLoweredType =
        SGF.getLoweredType(AbstractionPattern::getOpaque(), concreteFormalType);
    
    switch (repr) {
    case ExistentialRepresentation::Opaque: {
      concreteBuffer = SGF.B.createInitExistentialAddr(loc, existential,
                                           concreteFormalType,
                                           concreteLoweredType.getAddressType(),
                                           conformances);
      break;
    }
    case ExistentialRepresentation::Boxed: {
      auto box = SGF.B.createAllocExistentialBox(loc,
                       existential->getType().getObjectType(),
                       concreteFormalType,
                       conformances);
      concreteBuffer = SGF.B.createProjectExistentialBox(loc,
                                           concreteLoweredType.getAddressType(),
                                           box);
      SGF.B.createStore(loc, box, existential,
                        StoreOwnershipQualifier::Init);
      break;
    }
    case ExistentialRepresentation::Class:
    case ExistentialRepresentation::Metatype:
    case ExistentialRepresentation::None:
      llvm_unreachable("not supported");
    }
    
    // Activate the cleanup to deallocate the buffer we just allocated, should
    SGF.Cleanups.setCleanupState(deinitExistentialCleanup,
                                 CleanupState::Active);

    return concreteBuffer;
  }

  bool isInPlaceInitializationOfGlobal() const override {
    return isa_and_nonnull<GlobalAddrInst>(existential);
  }

  void finishInitialization(SILGenFunction &SGF) override {
    SingleBufferInitialization::finishInitialization(SGF);
    // We've fully initialized the existential by this point, so we can
    // retire the partial cleanup.
    SGF.Cleanups.setCleanupState(deinitExistentialCleanup,
                                 CleanupState::Dead);
  }
};

} // end anonymous namespace

ManagedValue SILGenFunction::emitExistentialErasure(
                            SILLocation loc,
                            CanType concreteFormalType,
                            const TypeLowering &concreteTL,
                            const TypeLowering &existentialTL,
                            ArrayRef<ProtocolConformanceRef> conformances,
                            SGFContext C,
                            llvm::function_ref<ManagedValue (SGFContext)> F,
                            bool allowEmbeddedNSError) {
  // Mark the needed conformances as used.
  for (auto conformance : conformances)
    SGM.useConformance(conformance);

  // If we're erasing to the 'Error' type, we might be able to get an NSError
  // representation more efficiently.
  auto &ctx = getASTContext();
  if (ctx.LangOpts.EnableObjCInterop && conformances.size() == 1 &&
      conformances[0].getRequirement() == ctx.getErrorDecl() &&
      ctx.getNSErrorDecl()) {
    // If the concrete type is NSError or a subclass thereof, just erase it
    // directly.
    auto nsErrorType = ctx.getNSErrorType()->getCanonicalType();
    if (nsErrorType->isExactSuperclassOf(concreteFormalType)) {
      ManagedValue nsError =  F(SGFContext());
      if (nsErrorType != concreteFormalType) {
        nsError = B.createUpcast(loc, nsError, getLoweredType(nsErrorType));
      }
      return emitBridgedToNativeError(loc, nsError);
    }

    // If the concrete type is known to conform to _BridgedStoredNSError,
    // call the _nsError witness getter to extract the NSError directly,
    // then just erase the NSError.
    auto storedNSErrorConformance =
        SGM.getConformanceToBridgedStoredNSError(loc, concreteFormalType);
    if (storedNSErrorConformance) {
      auto nsErrorVar = SGM.getNSErrorRequirement(loc);
      if (!nsErrorVar) return emitUndef(existentialTL.getLoweredType());

      SubstitutionMap nsErrorVarSubstitutions;

      // Devirtualize.  Maybe this should be done implicitly by
      // emitPropertyLValue?
      if (storedNSErrorConformance.isConcrete()) {
        if (auto normal = dyn_cast<NormalProtocolConformance>(
                storedNSErrorConformance.getConcrete())) {
          if (auto witnessVar = normal->getWitness(nsErrorVar)) {
            nsErrorVar = cast<VarDecl>(witnessVar.getDecl());
            nsErrorVarSubstitutions = witnessVar.getSubstitutions();
          }
        }
      }

      ManagedValue nativeError = F(SGFContext());

      FormalEvaluationScope writebackScope(*this);
      ManagedValue nsError =
          emitRValueForStorageLoad(
              loc, nativeError, concreteFormalType,
              /*super*/ false, nsErrorVar, PreparedArguments(),
              nsErrorVarSubstitutions,
              AccessSemantics::Ordinary, nsErrorType, SGFContext())
              .getAsSingleValue(*this, loc);

      return emitBridgedToNativeError(loc, nsError);
    }

    // Otherwise, if it's an archetype, try calling the _getEmbeddedNSError()
    // witness to try to dig out the embedded NSError.  But don't do this
    // when we're being called recursively.
    if (isa<ArchetypeType>(concreteFormalType) && allowEmbeddedNSError) {
      auto contBB = createBasicBlock();
      auto isNotPresentBB = createBasicBlock();
      auto isPresentBB = createBasicBlock();

      // Call swift_stdlib_getErrorEmbeddedNSError to attempt to extract an
      // NSError from the value.
      auto getEmbeddedNSErrorFn = SGM.getGetErrorEmbeddedNSError(loc);
      if (!getEmbeddedNSErrorFn)
        return emitUndef(existentialTL.getLoweredType());

      auto getEmbeddedNSErrorSubstitutions =
        SubstitutionMap::getProtocolSubstitutions(ctx.getErrorDecl(),
                                                  concreteFormalType,
                                                  conformances[0]);

      ManagedValue concreteValue = F(SGFContext());
      ManagedValue potentialNSError =
        emitApplyOfLibraryIntrinsic(loc,
                                    getEmbeddedNSErrorFn,
                                    getEmbeddedNSErrorSubstitutions,
                                    { concreteValue.copy(*this, loc) },
                                    SGFContext())
          .getAsSingleValue(*this, loc);

      // We're going to consume 'concreteValue' in exactly one branch,
      // so kill its cleanup now and recreate it on both branches.
      (void) concreteValue.forward(*this);

      // Check whether we got an NSError back.
      std::pair<EnumElementDecl*, SILBasicBlock*> cases[] = {
        { ctx.getOptionalSomeDecl(), isPresentBB },
        { ctx.getOptionalNoneDecl(), isNotPresentBB }
      };
      auto *switchEnum =
          B.createSwitchEnum(loc, potentialNSError.forward(*this),
                             /*default*/ nullptr, cases);

      // If we did get an NSError, emit the existential erasure from that
      // NSError.
      B.emitBlock(isPresentBB);
      SILValue branchArg;
      {
        // Don't allow cleanups to escape the conditional block.
        FullExpr presentScope(Cleanups, CleanupLocation(loc));
        enterDestroyCleanup(concreteValue.getValue());

        // Receive the error value.  It's typed as an 'AnyObject' for
        // layering reasons, so perform an unchecked cast down to NSError.
        auto nsError = B.createOptionalSomeResult(switchEnum);
        nsError = B.createUncheckedRefCast(loc, nsError, 
                                           getLoweredType(nsErrorType));

        branchArg = emitBridgedToNativeError(loc, nsError).forward(*this);
      }
      B.createBranch(loc, contBB, branchArg);

      // If we did not get an NSError, just directly emit the existential.
      // Since this is a recursive call, make sure we don't end up in this
      // path again.
      B.emitBlock(isNotPresentBB);
      {
        FullExpr presentScope(Cleanups, CleanupLocation(loc));
        concreteValue = emitManagedRValueWithCleanup(concreteValue.getValue());
        branchArg = emitExistentialErasure(loc, concreteFormalType, concreteTL,
                                           existentialTL, conformances,
                                           SGFContext(),
                                           [&](SGFContext C) {
                                             return concreteValue;
                                           },
                                           /*allowEmbeddedNSError=*/false)
                      .forward(*this);
      }
      B.createBranch(loc, contBB, branchArg);

      // Continue.
      B.emitBlock(contBB);

      SILValue existentialResult = contBB->createPhiArgument(
          existentialTL.getLoweredType(), OwnershipKind::Owned);
      return emitManagedRValueWithCleanup(existentialResult, existentialTL);
    }
  }

  switch (existentialTL.getLoweredType().getObjectType()
            .getPreferredExistentialRepresentation(concreteFormalType)) {
  case ExistentialRepresentation::None:
    llvm_unreachable("not an existential type");
  case ExistentialRepresentation::Metatype: {
    assert(existentialTL.isLoadable());

    SILValue metatype = F(SGFContext()).getUnmanagedValue();
    assert(metatype->getType().castTo<AnyMetatypeType>()->getRepresentation()
             == MetatypeRepresentation::Thick);

    auto upcast =
      B.createInitExistentialMetatype(loc, metatype,
                      existentialTL.getLoweredType(),
                      conformances);
    return ManagedValue::forObjectRValueWithoutOwnership(upcast);
  }
  case ExistentialRepresentation::Class: {
    assert(existentialTL.isLoadable());

    ManagedValue sub = F(SGFContext());
    assert(concreteFormalType->isBridgeableObjectType());
    return B.createInitExistentialRef(loc, existentialTL.getLoweredType(),
                                      concreteFormalType, sub, conformances);
  }
  case ExistentialRepresentation::Boxed: {
    // We defer allocation of the box to when the address is demanded.
    // Create a stack slot to hold the box once it's allocated.
    SILValue boxValue;
    auto buf = B.bufferForExpr(
      loc, existentialTL.getLoweredType(), existentialTL, C,
      [&](SILValue existential) {
        // Initialize the existential in-place.
        ExistentialInitialization init(*this, existential,
                                       concreteFormalType,
                                       conformances,
                                       ExistentialRepresentation::Boxed);
        ManagedValue mv = F(SGFContext(&init));
        if (!mv.isInContext()) {
          init.copyOrInitValueInto(*this, loc, mv.ensurePlusOne(*this, loc),
                                    /*init*/ true);
          init.finishInitialization(*this);
        }
      });

    if (buf.isInContext()) {
      return buf;
    }

    auto value = B.createLoad(loc, buf.forward(*this),
                              LoadOwnershipQualifier::Take);
    return emitManagedRValueWithCleanup(value);
  }
  case ExistentialRepresentation::Opaque: {
  
    // If the concrete value is a pseudogeneric archetype, first erase it to
    // its upper bound.
    auto anyObjectTy = getASTContext().getAnyObjectType();
    auto eraseToAnyObject =
    [&, concreteFormalType, F](SGFContext C) -> ManagedValue {
      auto concreteValue = F(SGFContext());
      assert(concreteFormalType->isBridgeableObjectType());
      return B.createInitExistentialRef(
          loc, SILType::getPrimitiveObjectType(anyObjectTy), concreteFormalType,
          concreteValue, conformances);
    };

    if (this->F.getLoweredFunctionType()->isPseudogeneric()) {
      if (anyObjectTy && concreteFormalType->is<ArchetypeType>()) {
        concreteFormalType = anyObjectTy;

        // The original conformances are no good because they have the wrong
        // (pseudogeneric) subject type.
        auto *M = SGM.M.getSwiftModule();
        conformances = M->collectExistentialConformances(
            concreteFormalType, anyObjectTy);
        F = eraseToAnyObject;
      }
    }

    if (!silConv.useLoweredAddresses()) {
      // We should never create new buffers just for init_existential under
      // opaque values mode: This is a case of an opaque value that we can
      // "treat" as a by-value one
      ManagedValue sub = F(SGFContext());
      return B.createInitExistentialValue(
          loc, existentialTL.getLoweredType(), concreteFormalType,
          sub, conformances);
    }

    // Allocate the existential.
    return B.bufferForExpr(
        loc, existentialTL.getLoweredType(), existentialTL, C,
        [&](SILValue existential) {
          // Initialize the existential in-place.
          ExistentialInitialization init(*this, existential,
                                         concreteFormalType,
                                         conformances,
                                         ExistentialRepresentation::Opaque);
          ManagedValue mv = F(SGFContext(&init));
          if (!mv.isInContext()) {
            init.copyOrInitValueInto(*this, loc, mv.ensurePlusOne(*this, loc),
                                      /*init*/ true);
            init.finishInitialization(*this);
          }
        });
  }
  }

  llvm_unreachable("Unhandled ExistentialRepresentation in switch.");
}

ManagedValue SILGenFunction::emitClassMetatypeToObject(SILLocation loc,
                                                       ManagedValue v,
                                                       SILType resultTy) {
  SILValue value = v.getUnmanagedValue();

  // Convert the metatype to objc representation.
  auto metatypeTy = value->getType().castTo<MetatypeType>();
  auto objcMetatypeTy = CanMetatypeType::get(metatypeTy.getInstanceType(),
                                             MetatypeRepresentation::ObjC);
  value = B.createThickToObjCMetatype(loc, value,
                           SILType::getPrimitiveObjectType(objcMetatypeTy));
  
  // Convert to an object reference.
  value = B.createObjCMetatypeToObject(loc, value, resultTy);
  return emitManagedRValueWithCleanup(value);
}

ManagedValue SILGenFunction::emitExistentialMetatypeToObject(SILLocation loc,
                                                             ManagedValue v,
                                                             SILType resultTy) {
  SILValue value = v.getUnmanagedValue();
  
  // Convert the metatype to objc representation.
  auto metatypeTy = value->getType().castTo<ExistentialMetatypeType>();
  auto objcMetatypeTy = CanExistentialMetatypeType::get(
                                              metatypeTy.getInstanceType(),
                                              MetatypeRepresentation::ObjC);
  value = B.createThickToObjCMetatype(loc, value,
                               SILType::getPrimitiveObjectType(objcMetatypeTy));
  
  // Convert to an object reference.
  value = B.createObjCExistentialMetatypeToObject(loc, value, resultTy);
  
  return emitManagedRValueWithCleanup(value);
}

ManagedValue SILGenFunction::emitProtocolMetatypeToObject(SILLocation loc,
                                                          CanType inputTy,
                                                          SILType resultTy) {
  auto protocolType = inputTy->castTo<MetatypeType>()->getInstanceType();
  if (auto existential = protocolType->getAs<ExistentialType>())
    protocolType = existential->getConstraintType();

  ProtocolDecl *protocol = protocolType->castTo<ProtocolType>()->getDecl();

  SILValue value = B.createObjCProtocol(loc, protocol, resultTy);
  
  // Protocol objects, despite being global objects, inherit default reference
  // counting semantics from NSObject, so we need to retain the protocol
  // reference when we use it to prevent it being released and attempting to
  // deallocate itself. It doesn't matter if we ever actually clean up that
  // retain though.
  value = B.createCopyValue(loc, value);
  return emitManagedRValueWithCleanup(value);
}

ManagedValue
SILGenFunction::emitOpenExistential(
       SILLocation loc,
       ManagedValue existentialValue,
       SILType loweredOpenedType,
       AccessKind accessKind) {
  assert(isInFormalEvaluationScope());

  SILType existentialType = existentialValue.getType();
  switch (existentialType.getPreferredExistentialRepresentation()) {
  case ExistentialRepresentation::Opaque: {
    // With CoW existentials we can't consume the boxed value inside of
    // the existential. (We could only do so after a uniqueness check on
    // the box holding the value).
    if (existentialType.isAddress()) {
      OpenedExistentialAccess allowedAccess =
          getOpenedExistentialAccessFor(accessKind);
      if (!loweredOpenedType.isAddress()) {
        assert(!silConv.useLoweredAddresses() &&
               "Non-address loweredOpenedType is only allowed under opaque "
               "value mode");
        loweredOpenedType = loweredOpenedType.getAddressType();
      }
      SILValue archetypeValue =
        B.createOpenExistentialAddr(loc, existentialValue.getValue(),
                                    loweredOpenedType, allowedAccess);
      return ManagedValue::forBorrowedAddressRValue(archetypeValue);
    } else {
      // borrow the existential and return an unmanaged opened value.
      return B.createOpenExistentialValue(
          loc, existentialValue, loweredOpenedType);
    }
  }
  case ExistentialRepresentation::Metatype:
    assert(existentialType.isObject());
    return B.createOpenExistentialMetatype(
        loc, existentialValue, loweredOpenedType);
  case ExistentialRepresentation::Class:
    assert(existentialType.isObject());
    return B.createOpenExistentialRef(loc, existentialValue, loweredOpenedType);
  case ExistentialRepresentation::Boxed:
    if (existentialType.isAddress()) {
      existentialValue = emitLoad(loc, existentialValue.getValue(),
                                  getTypeLowering(existentialType),
                                  SGFContext::AllowGuaranteedPlusZero,
                                  IsNotTake);
    }

    existentialType = existentialValue.getType();
    assert(existentialType.isObject());
    if (loweredOpenedType.isAddress()) {
      return B.createOpenExistentialBox(loc, existentialValue,
                                        loweredOpenedType);
    } else {
      assert(!silConv.useLoweredAddresses());
      return B.createOpenExistentialBoxValue(
        loc, existentialValue, loweredOpenedType);
    }
  case ExistentialRepresentation::None:
    llvm_unreachable("not existential");
  }
  llvm_unreachable("covered switch");
}

ManagedValue SILGenFunction::manageOpaqueValue(ManagedValue value,
                                               SILLocation loc,
                                               SGFContext C) {
  // If the opaque value is consumable, we can just return the
  // value with a cleanup. There is no need to retain it separately.
  if (value.isPlusOneOrTrivial(*this))
    return value;

  // If the context wants a +0 value, guaranteed or immediate, we can
  // give it to them, because OpenExistential emission guarantees the
  // value.
  if (C.isGuaranteedPlusZeroOk())
    return value;

  // If the context has an initialization a buffer, copy there instead
  // of making a temporary allocation.
  if (auto I = C.getEmitInto()) {
    I->copyOrInitValueInto(*this, loc, value, /*init*/ false);
    I->finishInitialization(*this);
    return ManagedValue::forInContext();
  }

  // Otherwise, copy the value into a temporary.
  return value.copyUnmanaged(*this, loc);
}

ManagedValue SILGenFunction::emitAsOrig(SILLocation loc,
                                        AbstractionPattern origType,
                                        CanType substType,
                                        SILType expectedTy,
                                        SGFContext C,
                                        ValueProducerRef produceValue) {
  // If the lowered substituted type already matches the substitution,
  // we can just emit directly.
  auto loweredSubstTy = getLoweredType(substType);
  if (loweredSubstTy.getASTType() == expectedTy.getASTType()) {
    auto result = produceValue(*this, loc, C);

    // For convenience, force the result into the destination.
    if (auto init = C.getEmitInto(); init && !result.isInContext()) {
      result.forwardInto(*this, loc, init);
      return ManagedValue::forInContext();
    }
    return result;
  }

  auto conversion =
    Conversion::getSubstToOrig(origType, substType, loweredSubstTy, expectedTy);
  auto result = emitConvertedRValue(loc, conversion, C, produceValue);

  // emitConvertedRValue always forces results into the context.
  assert((C.getEmitInto() != nullptr) == result.isInContext());
  return result;
}

ManagedValue SILGenFunction::emitConvertedRValue(Expr *E,
                                                 const Conversion &conversion,
                                                 SGFContext C) {
  return emitConvertedRValue(E, conversion, C,
      [&](SILGenFunction &SGF, SILLocation loc, SGFContext C) {
    return emitRValueAsSingleValue(E, C);
  });
}

ManagedValue SILGenFunction::emitConvertedRValue(SILLocation loc,
                                                 const Conversion &conversion,
                                                 SGFContext C,
                                                 ValueProducerRef produceValue){
  // If we're emitting into a converting context, check whether we can
  // peephole the conversions together.
  if (auto outerConversion = C.getAsConversion()) {
    if (outerConversion->tryPeephole(*this, loc, conversion, produceValue)) {
      outerConversion->finishInitialization(*this);
      return ManagedValue::forInContext();
    }
  }

  // Otherwise, set up a reabstracting context and try to emit into that.
  ConvertingInitialization init(conversion, C);
  auto result = produceValue(*this, loc, SGFContext(&init));
  auto finishedResult = init.finishEmission(*this, loc, result);
  return finishedResult;
}

ManagedValue
ConvertingInitialization::finishEmission(SILGenFunction &SGF,
                                         SILLocation loc,
                                         ManagedValue formalResult) {
  switch (getState()) {
  case Uninitialized:
    assert(!formalResult.isInContext());
    State = Extracted;
    return TheConversion.emit(SGF, loc, formalResult, FinalContext);

  case Initialized:
    llvm_unreachable("initialization never finished");

  case PackExpanding:
  case FinishedPackExpanding:
    llvm_unreachable("cannot mix this with pack emission");

  case Finished:
    assert(formalResult.isInContext());
    assert(!Value.isInContext() || FinalContext.getEmitInto());
    State = Extracted;
    return Value;

  case Extracted:
    llvm_unreachable("value already extracted");
  }
  llvm_unreachable("bad state");
}

void ConvertingInitialization::
       performPackExpansionInitialization(SILGenFunction &SGF,
                                          SILLocation loc,
                                          SILValue indexWithinComponent,
                      llvm::function_ref<void(Initialization *into)> fn) {
  // Bookkeeping.
  assert(getState() == Uninitialized);
  State = PackExpanding;

  auto finalInit = FinalContext.getEmitInto();
  assert(finalInit); // checked by canPerformPackExpansionInitialization
  finalInit->performPackExpansionInitialization(
                                      SGF, loc, indexWithinComponent,
                                      [&](Initialization *subEltInit) {
    // FIXME: translate the subst types into the element context.
    ConvertingInitialization eltInit(getConversion(), SGFContext(subEltInit));
    fn(&eltInit);
  });
}

static std::optional<CombinedConversions>
combineConversions(SILGenFunction &SGF, const Conversion &outer,
                   const Conversion &inner);

bool ConvertingInitialization::tryPeephole(SILGenFunction &SGF,
                                           SILLocation loc,
                                           ManagedValue origValue,
                                           Conversion innerConversion) {
  return tryPeephole(SGF, loc, innerConversion,
      [&](SILGenFunction &SGF, SILLocation loc, SGFContext C) {
    return origValue;
  });
}

bool ConvertingInitialization::tryPeephole(SILGenFunction &SGF,
                                           Expr *E,
                                           Conversion innerConversion) {
  return tryPeephole(SGF, E, innerConversion,
      [&](SILGenFunction &SGF, SILLocation loc, SGFContext C) {
    return SGF.emitRValueAsSingleValue(E, C);
  });
}

bool ConvertingInitialization::tryPeephole(SILGenFunction &SGF, SILLocation loc,
                                           Conversion innerConversion,
                                           ValueProducerRef produceOrigValue) {
  const auto &outerConversion = getConversion();
  auto combined = combineConversions(SGF, outerConversion, innerConversion);
  if (!combined)
    return false;

  assert(!combined->second || combined->first);

  ManagedValue result;
  if (!combined->first) {
    result = produceOrigValue(SGF, loc, FinalContext);
  } else if (!combined->second) {
    result = SGF.emitConvertedRValue(loc, *combined->first, FinalContext,
                                     produceOrigValue);
  } else {
    // Compute the first result without any context.  We know that we won't
    // be able to combine these conversions, so computing them together
    // is just a waste of time, and it runs the risk of an infinite recursion
    // if we screwed something up.
    auto firstResult =
      SGF.emitConvertedRValue(loc, *combined->first, SGFContext(),
                              produceOrigValue);
    result = combined->second->emit(SGF, loc, firstResult, FinalContext);
  }

  initWithConvertedValue(SGF, loc, result);
  return true;
}

void ConvertingInitialization::copyOrInitValueInto(SILGenFunction &SGF,
                                                   SILLocation loc,
                                                   ManagedValue formalValue,
                                                   bool isInit) {
  assert(getState() == Uninitialized && "already have saved value?");

  // TODO: take advantage of borrowed inputs?
  if (!isInit) formalValue = formalValue.copy(SGF, loc);

  // Convert the value.
  auto value = TheConversion.emit(SGF, loc, formalValue, FinalContext);

 initWithConvertedValue(SGF, loc, value);
}

void ConvertingInitialization::initWithConvertedValue(SILGenFunction &SGF,
                                                      SILLocation loc,
                                                      ManagedValue value) {
  assert(getState() == Uninitialized);
  auto finalInit = FinalContext.getEmitInto();
  if (value.isInContext()) {
    assert(finalInit);
  } else if (finalInit) {
    value.ensurePlusOne(SGF, loc).forwardInto(SGF, loc, finalInit);
    value = ManagedValue::forInContext();
  }

  assert(value.isInContext() == (finalInit != nullptr));
  Value = value;
  State = Initialized;
}

ManagedValue
ConvertingInitialization::emitWithAdjustedConversion(SILGenFunction &SGF,
                                          SILLocation loc,
                                          Conversion adjustedConversion,
                                          ValueProducerRef produceValue) {
  ConvertingInitialization init(adjustedConversion, getFinalContext());
  auto result = produceValue(SGF, loc, SGFContext(&init));
  result = init.finishEmission(SGF, loc, result);
  initWithConvertedValue(SGF, loc, result);
  finishInitialization(SGF);
  return ManagedValue::forInContext();
}

ManagedValue Conversion::emit(SILGenFunction &SGF, SILLocation loc,
                              ManagedValue value, SGFContext C) const {
  switch (getKind()) {
  case AnyErasure:
  case BridgingSubtype:
  case Subtype:
    return SGF.emitTransformedValue(loc, value, getSourceType(),
                                    getResultType(), C);

  case ForceOptional: {
    auto &optTL = SGF.getTypeLowering(value.getType());
    return SGF.emitCheckedGetOptionalValueFrom(loc, value,
                                               /*isForceUnwrap*/ true,
                                               optTL, C);
  }

  case BridgeToObjC:
    return SGF.emitNativeToBridgedValue(loc, value,
                                        getSourceType(),
                                        getResultType(),
                                        getLoweredResultType(), C);

  case ForceAndBridgeToObjC: {
    auto &tl = SGF.getTypeLowering(value.getType());
    auto sourceValueType = getSourceType().getOptionalObjectType();
    value = SGF.emitCheckedGetOptionalValueFrom(loc, value,
                                                /*isImplicitUnwrap*/ true,
                                                tl, SGFContext());
    return SGF.emitNativeToBridgedValue(loc, value, sourceValueType,
                                        getResultType(),
                                        getLoweredResultType(), C);
  }

  case BridgeFromObjC:
    return SGF.emitBridgedToNativeValue(loc, value,
                                        getSourceType(), getResultType(),
                                        getLoweredResultType(), C);

  case BridgeResultFromObjC:
    return SGF.emitBridgedToNativeValue(loc, value,
                                        getSourceType(), getResultType(),
                                        getLoweredResultType(), C,
                                        /*isResult*/ true);

  case Reabstract:
    assert(value.getType().getObjectType() ==
           getReabstractionInputLoweredType().getObjectType());
    return SGF.emitTransformedValue(loc, value,
                                    getReabstractionInputOrigType(),
                                    getReabstractionInputSubstType(),
                                    getReabstractionOutputOrigType(),
                                    getReabstractionOutputSubstType(),
                                    getReabstractionOutputLoweredType(), C);
  }
  llvm_unreachable("bad kind");
}

OptionalInjectionConversion
Conversion::adjustForInitialOptionalInjection() const {
  switch (getKind()) {
  case Reabstract:
    return OptionalInjectionConversion::forValue(
      getReabstract(
        getReabstractionInputOrigType().getOptionalObjectType(),
        getReabstractionInputSubstType().getOptionalObjectType(),
        getReabstractionInputLoweredType().getOptionalObjectType(),
        getReabstractionOutputOrigType().getOptionalObjectType(),
        getReabstractionOutputSubstType().getOptionalObjectType(),
        getReabstractionOutputLoweredType().getOptionalObjectType())
    );

  case Subtype:
    return OptionalInjectionConversion::forValue(
      getSubtype(
        getSourceType().getOptionalObjectType(),
        getResultType().getOptionalObjectType(),
        getLoweredResultType().getOptionalObjectType())
    );

  // TODO: can these actually happen?
  case ForceOptional:
  case ForceAndBridgeToObjC:
  case BridgingSubtype:
    return OptionalInjectionConversion();

  case AnyErasure:
  case BridgeToObjC:
  case BridgeFromObjC:
  case BridgeResultFromObjC:
    return OptionalInjectionConversion::forInjection(
      getBridging(getKind(), getSourceType().getOptionalObjectType(),
                  getResultType(), getLoweredResultType(),
                  isBridgingExplicit())
    );
  }
  llvm_unreachable("bad kind");
}

std::optional<Conversion>
Conversion::adjustForInitialOptionalConversions(CanType newSourceType) const {
  switch (getKind()) {
  case Reabstract:
    // TODO: handle reabstraction conversions here, too.
    return std::nullopt;

  case ForceOptional:
  case ForceAndBridgeToObjC:
    return std::nullopt;

  case BridgingSubtype:
  case Subtype:
  case AnyErasure:
  case BridgeToObjC:
  case BridgeFromObjC:
  case BridgeResultFromObjC:
    return Conversion::getBridging(getKind(), newSourceType,
                                   getResultType(), getLoweredResultType(),
                                   isBridgingExplicit());
  }
  llvm_unreachable("bad kind");
}

std::optional<Conversion> Conversion::adjustForInitialForceValue() const {
  switch (getKind()) {
  case Reabstract:
  case AnyErasure:
  case BridgeFromObjC:
  case BridgeResultFromObjC:
  case ForceOptional:
  case ForceAndBridgeToObjC:
  case BridgingSubtype:
  case Subtype:
    return std::nullopt;

  case BridgeToObjC: {
    auto sourceOptType = getSourceType().wrapInOptionalType();
    return Conversion::getBridging(ForceAndBridgeToObjC,
                                   sourceOptType, getResultType(),
                                   getLoweredResultType(),
                                   isBridgingExplicit());
  }
  }
  llvm_unreachable("bad kind");
}

void Conversion::dump() const {
  print(llvm::errs());
  llvm::errs() << '\n';
}

static void printReabstraction(const Conversion &conversion,
                               llvm::raw_ostream &out, StringRef name) {
  out << name << "(inputOrig: ";
  conversion.getReabstractionInputOrigType().print(out);
  out << ", inputSubst: ";
  conversion.getReabstractionInputSubstType().print(out);
  out << ", inputLowered: ";
  conversion.getReabstractionInputLoweredType().print(out);
  out << ", outputOrig: ";
  conversion.getReabstractionOutputOrigType().print(out);
  out << ", outputSubst: ";
  conversion.getReabstractionOutputSubstType().print(out);
  out << ", outputLowered: ";
  conversion.getReabstractionOutputLoweredType().print(out);
  out << ')';
}

static void printBridging(const Conversion &conversion, llvm::raw_ostream &out,
                          StringRef name) {
  out << name << "(from: ";
  conversion.getSourceType().print(out);
  out << ", to: ";
  conversion.getResultType().print(out);
  out << ", explicit: " << conversion.isBridgingExplicit() << ')';
}

void Conversion::print(llvm::raw_ostream &out) const {
  switch (getKind()) {
  case Reabstract:
    return printReabstraction(*this, out, "Reabstract");
  case AnyErasure:
    return printBridging(*this, out, "AnyErasure");
  case BridgingSubtype:
    return printBridging(*this, out, "BridgingSubtype");
  case Subtype:
    return printBridging(*this, out, "Subtype");
  case ForceOptional:
    return printBridging(*this, out, "ForceOptional");
  case BridgeToObjC:
    return printBridging(*this, out, "BridgeToObjC");
  case ForceAndBridgeToObjC:
    return printBridging(*this, out, "ForceAndBridgeToObjC");
  case BridgeFromObjC:
    return printBridging(*this, out, "BridgeFromObjC");
  case BridgeResultFromObjC:
    return printBridging(*this, out, "BridgeResultFromObjC");
  }
  llvm_unreachable("bad kind");
}

static bool areRelatedTypesForBridgingPeephole(CanType sourceType,
                                               CanType resultType) {
  if (sourceType == resultType)
    return true;

  if (auto resultObjType = resultType.getOptionalObjectType()) {
    // Optional-to-optional.
    if (auto sourceObjType = sourceType.getOptionalObjectType()) {
      return areRelatedTypesForBridgingPeephole(sourceObjType, resultObjType);
    }

    // Optional injection.
    return areRelatedTypesForBridgingPeephole(sourceType, resultObjType);
  }

  // If the result type is AnyObject, then we can always apply the bridge
  // via Any.
  if (resultType->isAnyObject()) {
    // ... as long as the source type is not an Optional.
    if (sourceType->isBridgeableObjectType())
      return true;
  }

  // TODO: maybe other class existentials? Existential conversions?
  // They probably aren't important here.

  // All the other rules only apply to class types.
  if (!sourceType->mayHaveSuperclass() ||
      !resultType->mayHaveSuperclass())
    return false;

  // Walk up the class hierarchy looking for an exact match.
  while (auto superclass = sourceType->getSuperclass()) {
    sourceType = superclass->getCanonicalType();
    if (sourceType == resultType)
      return true;
  }

  // Otherwise, we don't know how to do this conversion.
  return false;
}

/// Does the given conversion turn a non-class type into Any, taking into
/// account optional-to-optional conversions?
static bool isValueToAnyConversion(CanType from, CanType to) {
  while (auto toObj = to.getOptionalObjectType()) {
    to = toObj;
    if (auto fromObj = from.getOptionalObjectType()) {
      from = fromObj;
    }
  }

  assert(to->isAny());

  // Types that we can easily transform into AnyObject:
  //   - classes and class-bounded archetypes
  //   - class existentials, even if not pure-@objc
  //   - @convention(objc) metatypes
  //   - @convention(block) functions
  return !from->isAnyClassReferenceType() &&
         !from->isBridgeableObjectType();
}

/// Check whether this conversion is Any??? to AnyObject???.  If the result
/// type is less optional, it doesn't count.
static bool isMatchedAnyToAnyObjectConversion(CanType from, CanType to) {
  while (auto fromObject = from.getOptionalObjectType()) {
    auto toObject = to.getOptionalObjectType();
    if (!toObject) return false;
    from = fromObject;
    to = toObject;
  }

  if (from->isAny()) {
    assert(to->lookThroughAllOptionalTypes()->isAnyObject());
    return true;
  }
  return false;
}

Conversion
Conversion::withSourceType(SILGenFunction &SGF, CanType substType) const {
  return withSourceType(AbstractionPattern(substType), substType,
                        SGF.getLoweredType(substType));
}

Conversion
Conversion::withSourceType(AbstractionPattern origType,
                           CanType substType, SILType loweredType) const {
  switch (getKind()) {
  case Reabstract:
    return getReabstract(origType, substType, loweredType,
                         getReabstractionOutputOrigType(),
                         getReabstractionOutputSubstType(),
                         getReabstractionOutputLoweredType());
  case Subtype:
    return getSubtype(substType, getResultType(), getLoweredResultType());
  default:
    llvm_unreachable("operation not supported on specialized bridging "
                     "conversions");
  }
}

Conversion
Conversion::withResultType(AbstractionPattern origType,
                           CanType substType, SILType loweredType) const {
  switch (getKind()) {
  case Reabstract:
    return getReabstract(getReabstractionInputOrigType(),
                         getReabstractionInputSubstType(),
                         getReabstractionInputLoweredType(),
                         origType, substType, loweredType);
  case Subtype:
    return getSubtype(getSourceType(), substType, loweredType);
  default:
    llvm_unreachable("operation not supported on specialized bridging "
                     "conversions");
  }
}

/// Can a sequence of conversions from type1 -> type2 -> type3 be represented
/// as a conversion from type1 -> type3, or does that lose critical information?
static bool isCombinableConversionImpl(CanType type1,
                                         CanType type2,
                                         CanType type3) {
  if (type1 == type2 || type2 == type3) return true;

  // If the final result type is optional, then either we've got two
  // optional->optional conversions or we injected into optional in at
  // least one of the stages.  Our analysis of how to do the conversion is
  // going to be sensitive to the static optional depth, so make sure we
  // don't lose that.
  if (auto object3 = type3.getOptionalObjectType()) {
    if (auto object2 = type2.getOptionalObjectType()) {
      // If we have optional -> optional conversions at both stages,
      // look through them all.
      if (auto object1 = type1.getOptionalObjectType()) {
        return isCombinableConversionImpl(object1, object2, object3);

      // If we have an injection in the first stage, we'll still know we have
      // an injection in the overall conversion.
      } else {
        return isCombinableConversionImpl(type1, object2, object3);
      }

    // We have an injection in the second stage.  If we lose optionality
    // in the first stage (i.e. we're converting an optional to an
    // existential), then the combined conversion will be misinterpreted
    // as an optional-to-optional conversion.
    } else if (type1.getOptionalObjectType()) {
      return false;

    // Otherwise, we're preserving that we have an injection overall.
    } else {
      return isCombinableConversionImpl(type1, type2, object3);
    }
  }

  // When we open an existential, we bind the erased type; this type should
  // not change if we combine the conversions.  The binding looks
  // polymorphically through certain types but not through others.
  if (type3.isExistentialType()) {
    // We need to consider type2 to see if it has structure that
    // would make it non-polymorphic, like if it's an optional.

    // Existentials (including existential metatypes) are polymorphic.
    if (type2.isAnyExistentialType())
      return true;

    // Class types are polymorphic.
    if (type2.isAnyClassReferenceType())
      return true;

    // Metatypes are polymorphic.
    if (isa<MetatypeType>(type2))
      return true;

    // Otherwise, no.  Since we know that type1 != type2, we know that type2
    // must have some kind of subtype-supporting structure; with the cases
    // above ruled out, that must be either an optional or a function type.
    // Note that, with an optional, we can probably still dynamically cast
    // successfully, but that's not the standard we need to enforce here.
    return false;
  }

  // If we have a function or tuple type, we need to see if we have a
  // non-peepholeable conversion in the subconversions.

  if (auto tuple3 = dyn_cast<TupleType>(type3)) {
    auto tuple2 = cast<TupleType>(type2);
    auto tuple1 = cast<TupleType>(type1);
    assert(tuple1->getNumElements() == tuple3->getNumElements());
    assert(tuple2->getNumElements() == tuple3->getNumElements());
    for (auto i : range(tuple3->getNumElements())) {
      if (!isCombinableConversionImpl(tuple1.getElementType(i),
                                      tuple2.getElementType(i),
                                      tuple3.getElementType(i)))
        return false;
    }
    return true;
  }

  if (auto fn3 = dyn_cast<AnyFunctionType>(type3)) {
    auto fn2 = cast<AnyFunctionType>(type2);
    auto fn1 = cast<AnyFunctionType>(type1);
    assert(fn1->getNumParams() == fn3->getNumParams());
    assert(fn2->getNumParams() == fn3->getNumParams());
    if (!isCombinableConversionImpl(fn1.getResult(),
                                    fn2.getResult(),
                                    fn3.getResult()))
      return false;
    for (auto i : range(fn3->getNumParams())) {
      // Note the reversal for invariance.
      if (!isCombinableConversionImpl(fn3.getParams()[i].getParameterType(),
                                      fn2.getParams()[i].getParameterType(),
                                      fn1.getParams()[i].getParameterType()))
        return false;
    }
    return true;
  }

  if (auto exp3 = dyn_cast<PackExpansionType>(type3)) {
    auto exp2 = cast<PackExpansionType>(type2);
    auto exp1 = cast<PackExpansionType>(type1);
    return isCombinableConversionImpl(exp1.getPatternType(),
                                      exp2.getPatternType(),
                                      exp3.getPatternType());
  }

  // The only remaining types that support subtyping are classes and
  // metatypes, and we can definitely just convert those.
  return true;
}

/// Can we combine the given conversions so that we go straight from
/// innerSrcType to outerDestType, or does that lose information?
static bool isCombinableConversion(const Conversion &inner,
                                   const Conversion &outer) {
  assert(inner.getResultType() == outer.getSourceType() &&
         "unexpected intermediate conversion");

  return isCombinableConversionImpl(inner.getSourceType(),
                                    inner.getResultType(),
                                    outer.getResultType());
}

/// Given that we cannot combine the given conversions, at least
/// "salvage" them to propagate semantically-critical contextual
/// type information inward.
static std::optional<CombinedConversions>
salvageUncombinableConversion(SILGenFunction &SGF,
                              const Conversion &inner,
                              const Conversion &outer) {
  // If the outer type is `@isolated(any)`, and the intermediate type
  // is non-isolated, propagate the `@isolated(any)` conversion inwards.
  // We don't want to do this if the intermediate function has some
  // explicit isolation because we need to honor that conversion even
  // if it's not the formal isolation of the source function (e.g. if
  // the user coerces a nonisolated function to a @MainActor function
  // type).  But if the intermediate function type is non-isolated, the
  // actual closure might still be isolated, either because we're
  // type-checking in some mode that doesn't propagate isolation in types
  // or because the isolation isn't representable in the type system
  // (e.g. it's isolated to some capture).
  if (auto outerOutputFnType =
        dyn_cast<AnyFunctionType>(outer.getResultType())) {
    auto intermediateFnType = cast<AnyFunctionType>(outer.getSourceType());
    if (outerOutputFnType->getIsolation().isErased() &&
        intermediateFnType->getIsolation().isNonIsolated()) {
      // Construct new intermediate orig/subst/lowered types that are
      // just the old intermediate type with `@isolated(any)`.
      auto newIntermediateSubstType = intermediateFnType.withExtInfo(
        intermediateFnType->getExtInfo().withIsolation(
          FunctionTypeIsolation::forErased()));
      auto newIntermediateOrigType =
        AbstractionPattern(newIntermediateSubstType);
      auto newIntermediateLoweredType =
        SGF.getLoweredType(newIntermediateSubstType);

      // Construct the new conversions with the new intermediate type.
      return CombinedConversions(
               inner.withResultType(newIntermediateOrigType,
                                    newIntermediateSubstType,
                                    newIntermediateLoweredType),
               outer.withSourceType(SGF, newIntermediateSubstType));
    }
  }

  return std::nullopt;
}

static std::optional<CombinedConversions>
combineReabstract(SILGenFunction &SGF,
                  const Conversion &outer,
                  const Conversion &inner) {
  // We can never combine conversions in a way that would lose information
  // about the intermediate types.
  if (!isCombinableConversion(inner, outer))
    return salvageUncombinableConversion(SGF, inner, outer);

  // Recognize when the whole conversion is an identity.
  if (inner.getReabstractionInputLoweredType().getObjectType() ==
      outer.getReabstractionOutputLoweredType().getObjectType())
    return CombinedConversions();

  // Produce a single conversion that goes straight from the inner input
  // to the outer output.
  return CombinedConversions(
    Conversion::getReabstract(inner.getReabstractionInputOrigType(),
                              inner.getReabstractionInputSubstType(),
                              inner.getReabstractionInputLoweredType(),
                              outer.getReabstractionOutputOrigType(),
                              outer.getReabstractionOutputSubstType(),
                              outer.getReabstractionOutputLoweredType())
  );
}

static std::optional<CombinedConversions>
combineSubtypeIntoReabstract(SILGenFunction &SGF,
                             const Conversion &outer,
                             const Conversion &inner) {
  // We can never combine conversions in a way that would lose information
  // about the intermediate types.
  if (!isCombinableConversion(inner, outer))
    return salvageUncombinableConversion(SGF, inner, outer);

  auto inputSubstType = inner.getSourceType();
  auto inputOrigType = AbstractionPattern(inputSubstType);
  auto inputLoweredTy = SGF.getLoweredType(inputOrigType, inputSubstType);

  return CombinedConversions(
    Conversion::getReabstract(
      inputOrigType, inputSubstType, inputLoweredTy,
      outer.getReabstractionOutputOrigType(),
      outer.getReabstractionOutputSubstType(),
      outer.getReabstractionOutputLoweredType())
  );
}

static std::optional<CombinedConversions>
combineSubtype(SILGenFunction &SGF,
               const Conversion &outer, const Conversion &inner) {
  if (!isCombinableConversion(inner, outer))
    return salvageUncombinableConversion(SGF, inner, outer);

  return CombinedConversions(
    Conversion::getSubtype(inner.getSourceType(), outer.getResultType(),
                           outer.getLoweredResultType())
  );
}

static std::optional<CombinedConversions>
combineBridging(SILGenFunction &SGF,
               const Conversion &outer, const Conversion &inner) {
  bool outerExplicit = outer.isBridgingExplicit();
  bool innerExplicit = inner.isBridgingExplicit();

  // Never peephole if both conversions are explicit; there might be
  // something the user's trying to do which we don't understand.
  if (outerExplicit && innerExplicit)
    return std::nullopt;

  // Otherwise, we can peephole if we understand the resulting conversion
  // and applying the peephole doesn't change semantics.

  CanType sourceType = inner.getSourceType();
  CanType intermediateType = inner.getResultType();
  assert(intermediateType == outer.getSourceType());

  // If we're doing a peephole involving a force, we want to propagate
  // the force to the source value.  If it's not in fact optional, that
  // won't work.
  bool forced = outer.getKind() == Conversion::ForceAndBridgeToObjC;
  if (forced) {
    sourceType = sourceType.getOptionalObjectType();
    if (!sourceType)
      return std::nullopt;

    intermediateType = intermediateType.getOptionalObjectType();
    assert(intermediateType);
  }

  CanType resultType = outer.getResultType();
  SILType loweredSourceTy = SGF.getLoweredType(sourceType);
  SILType loweredResultTy = outer.getLoweredResultType();

  auto applyPeephole = [&](const std::optional<Conversion> &conversion) {
    if (!forced) {
      if (!conversion)
        return CombinedConversions();
      return CombinedConversions(*conversion);
    }

    auto forceConversion =
      Conversion::getBridging(Conversion::ForceOptional,
                              inner.getSourceType(), sourceType,
                              loweredSourceTy);
    if (conversion)
      return CombinedConversions(forceConversion, *conversion);
    return CombinedConversions(forceConversion);
  };

  // Converting to Any doesn't do anything semantically special, so we
  // can apply the peephole unconditionally.
  if (isMatchedAnyToAnyObjectConversion(intermediateType, resultType)) {
    if (loweredSourceTy == loweredResultTy) {
      return applyPeephole(std::nullopt);
    } else if (isValueToAnyConversion(sourceType, intermediateType)) {
      return applyPeephole(
        Conversion::getBridging(Conversion::BridgeToObjC,
                                sourceType, resultType, loweredResultTy));
    } else {
      return applyPeephole(
        Conversion::getBridging(Conversion::BridgingSubtype,
                                sourceType, resultType, loweredResultTy));
    }
  }

  // Otherwise, undoing a bridging conversions can change semantics by
  // e.g. removing a copy, so we shouldn't do it unless the special
  // syntactic bridging peephole applies.  That requires one of the
  // conversions to be explicit.
  // TODO: use special SILGen to preserve semantics in this case,
  // e.g. by making a copy.
  if (!outerExplicit && !innerExplicit) {
    return std::nullopt;
  }

  // Okay, now we're in the domain of the bridging peephole: an
  // explicit bridging conversion can cancel out an implicit bridge
  // between related types.

  // If the source and destination types have exactly the same
  // representation, then (1) they're related and (2) we can directly
  // emit into the context.
  if (loweredSourceTy.getObjectType() == loweredResultTy.getObjectType()) {
    return applyPeephole(std::nullopt);
  }

  // Look for a subtype relationship between the source and destination.
  if (areRelatedTypesForBridgingPeephole(sourceType, resultType)) {
    return applyPeephole(
      Conversion::getBridging(Conversion::BridgingSubtype,
                              sourceType, resultType, loweredResultTy));
  }

  // If the inner conversion is a result conversion that removes
  // optionality, and the non-optional source type is a subtype of the
  // value type, this is just an implicit force.
  if (!forced &&
      inner.getKind() == Conversion::BridgeResultFromObjC) {
    if (auto sourceValueType = sourceType.getOptionalObjectType()) {
      if (!intermediateType.getOptionalObjectType() &&
          areRelatedTypesForBridgingPeephole(sourceValueType, resultType)) {
        forced = true;
        sourceType = sourceValueType;
        loweredSourceTy = loweredSourceTy.getOptionalObjectType();
        return applyPeephole(
          Conversion::getBridging(Conversion::BridgingSubtype,
                                  sourceValueType, resultType, loweredResultTy));
      }
    }
  }

  return std::nullopt;
}

/// TODO: this would really be a lot cleaner if it just returned a
/// std::optional<Conversion>.
static std::optional<CombinedConversions>
combineConversions(SILGenFunction &SGF, const Conversion &outer,
                   const Conversion &inner) {
  switch (outer.getKind()) {
  case Conversion::Reabstract:
    switch (inner.getKind()) {
    case Conversion::Reabstract:
      return combineReabstract(SGF, outer, inner);

    case Conversion::Subtype:
      return combineSubtypeIntoReabstract(SGF, outer, inner);

    default:
      return std::nullopt;
    }

  case Conversion::Subtype:
    if (inner.getKind() == Conversion::Subtype)
      return combineSubtype(SGF, outer, inner);
    return std::nullopt;

  case Conversion::AnyErasure:
  case Conversion::BridgingSubtype:
  case Conversion::BridgeFromObjC:
  case Conversion::BridgeResultFromObjC:
    // TODO: maybe peephole bridging through a Swift type?
    // This isn't actually something that happens in normal code generation.
    return std::nullopt;

  case Conversion::ForceOptional:
    return std::nullopt;

  case Conversion::ForceAndBridgeToObjC:
  case Conversion::BridgeToObjC:
    switch (inner.getKind()) {
    case Conversion::AnyErasure:
    case Conversion::BridgeFromObjC:
    case Conversion::BridgeResultFromObjC:
      return combineBridging(SGF, outer, inner);

    default:
      return std::nullopt;
    }
  }
  llvm_unreachable("bad kind");
}

bool Lowering::canPeepholeConversions(SILGenFunction &SGF,
                                      const Conversion &outer,
                                      const Conversion &inner) {
  return combineConversions(SGF, outer, inner).has_value();
}
