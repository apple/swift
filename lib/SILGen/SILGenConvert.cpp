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
  result.filenameStartPointer = ManagedValue::forUnmanaged(literal);
  // File length
  literal = B.createIntegerLiteral(emitLoc, wordTy, filename.size());
  result.filenameLength = ManagedValue::forUnmanaged(literal);
  // File is ascii
  literal = B.createIntegerLiteral(emitLoc, i1Ty, isASCII);
  result.filenameIsAscii = ManagedValue::forUnmanaged(literal);
  // Line
  literal = B.createIntegerLiteral(emitLoc, wordTy, line);
  result.line = ManagedValue::forUnmanaged(literal);
  // Column
  literal = B.createIntegerLiteral(emitLoc, wordTy, column);
  result.column = ManagedValue::forUnmanaged(literal);
  
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

  // If we have an object, make sure the object is at +1. All switch_enum of
  // objects is done at +1.
  bool isAddress = optional.getType().isAddress();
  SwitchEnumInst *switchEnum = nullptr;
  if (isAddress) {
    // We forward in the creation routine for
    // unchecked_take_enum_data_addr. switch_enum_addr is a +0 operation.
    B.createSwitchEnumAddr(loc, optional.getValue(),
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
      ManagedValue::forUnmanaged(isImplicitUnwrapLiteral);
    
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

  return ManagedValue::forUnmanaged(result.forward(*this));
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
  // If the conversion is a bridging conversion from an optional type,
  // do a bridging conversion from the non-optional type instead.
  // TODO: should this be a general thing for all conversions?
  if (auto optInit = C.getAsConversion()) {
    const auto &optConversion = optInit->getConversion();
    if (optConversion.isBridging()) {
      auto sourceValueType =
          optConversion.getBridgingSourceType().getOptionalObjectType();
      assert(sourceValueType);
      if (auto valueConversion =
            optConversion.adjustForInitialOptionalConversions(sourceValueType)){
        return optInit->emitWithAdjustedConversion(*this, loc, *valueConversion,
                                                   produceValue);
      }
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
    return ManagedValue::forUnmanaged(upcast);
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
          concreteValue, {});
    };

    if (this->F.getLoweredFunctionType()->isPseudogeneric()) {
      if (anyObjectTy && concreteFormalType->is<ArchetypeType>()) {
        concreteFormalType = anyObjectTy;
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
      return ManagedValue::forUnmanaged(archetypeValue);
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

static ManagedValue
emitPeepholedConversions(SILGenFunction &SGF, SILLocation loc,
                                   const Conversion &outerConversion,
                                   const Conversion &innerConversion,
                                   ConversionPeepholeHint hint,
                                   SGFContext C,
                                   ValueProducerRef produceOrigValue) {
  auto produceValue = [&](SGFContext C) {
    if (!hint.isForced()) {
      return produceOrigValue(SGF, loc, C);
    }

    auto value = produceOrigValue(SGF, loc, SGFContext());
    auto &optTL = SGF.getTypeLowering(value.getType());
    // isForceUnwrap is hardcoded true because hint.isForced() is only
    // set by implicit force unwraps.
    return SGF.emitCheckedGetOptionalValueFrom(loc, value,
                                               /*isForceUnwrap*/ true,
                                               optTL, C);
  };

  auto getBridgingSourceType = [&] {
    CanType sourceType = innerConversion.getBridgingSourceType();
    if (hint.isForced())
      sourceType = sourceType.getOptionalObjectType();
    return sourceType;
  };
  auto getBridgingResultType = [&] {
    return outerConversion.getBridgingResultType();
  };
  auto getBridgingLoweredResultType = [&] {
    return outerConversion.getBridgingLoweredResultType();
  };

  switch (hint.getKind()) {
  case ConversionPeepholeHint::Identity:
    return produceValue(C);

  case ConversionPeepholeHint::BridgeToAnyObject: {
    auto value = produceValue(SGFContext());
    return SGF.emitNativeToBridgedValue(loc, value, getBridgingSourceType(),
                                        getBridgingResultType(),
                                        getBridgingLoweredResultType(), C);
  }

  case ConversionPeepholeHint::Subtype: {
    // Otherwise, emit and convert.
    // TODO: if the context allows +0, use it in more situations.
    auto value = produceValue(SGFContext());
    SILType loweredResultTy = getBridgingLoweredResultType();

    // Nothing to do if the value already has the right representation.
    if (value.getType().getObjectType() == loweredResultTy.getObjectType())
      return value;

    CanType sourceType = getBridgingSourceType();
    CanType resultType = getBridgingResultType();
    return SGF.emitTransformedValue(loc, value, sourceType, resultType, C);
  }
  }
  llvm_unreachable("bad kind");
}

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
                                           ValueProducerRef produceValue) {
  const auto &outerConversion = getConversion();
  auto hint = canPeepholeConversions(SGF, outerConversion, innerConversion);
  if (!hint)
    return false;

  ManagedValue value = emitPeepholedConversions(SGF, loc, outerConversion,
                                                innerConversion, *hint,
                                                FinalContext, produceValue);
  
  // The callers to tryPeephole assume that the initialization is ready to be
  // finalized after returning. If this conversion sits on top of another
  // initialization, forward the value into the underlying initialization and
  // report the value as emitted in context.
  if (FinalContext.getEmitInto() && !value.isInContext()) {
    value.ensurePlusOne(SGF, loc).forwardInto(SGF, loc, FinalContext.getEmitInto());
    value = ManagedValue::forInContext();
  }
  setConvertedValue(value);
  return true;
}

void ConvertingInitialization::copyOrInitValueInto(SILGenFunction &SGF,
                                                   SILLocation loc,
                                                   ManagedValue formalValue,
                                                   bool isInit) {
  assert(getState() == Uninitialized && "already have saved value?");

  // TODO: take advantage of borrowed inputs?
  if (!isInit) formalValue = formalValue.copy(SGF, loc);
  State = Initialized;
  
  Value = TheConversion.emit(SGF, loc, formalValue, FinalContext);
  
  if (FinalContext.getEmitInto() && !Value.isInContext()) {
    Value.forwardInto(SGF, loc, FinalContext.getEmitInto());
    Value = ManagedValue::forInContext();
  }
}

ManagedValue
ConvertingInitialization::emitWithAdjustedConversion(SILGenFunction &SGF,
                                          SILLocation loc,
                                          Conversion adjustedConversion,
                                          ValueProducerRef produceValue) {
  ConvertingInitialization init(adjustedConversion, getFinalContext());
  auto result = produceValue(SGF, loc, SGFContext(&init));
  result = init.finishEmission(SGF, loc, result);
  setConvertedValue(result);
  finishInitialization(SGF);
  return ManagedValue::forInContext();
}

llvm::Optional<AbstractionPattern>
ConvertingInitialization::getAbstractionPattern() const {
  if (TheConversion.isReabstraction()) {
    return TheConversion.getReabstractionOrigType();
  }
  return llvm::None;
}

ManagedValue Conversion::emit(SILGenFunction &SGF, SILLocation loc,
                              ManagedValue value, SGFContext C) const {
  switch (getKind()) {
  case AnyErasure:
    return SGF.emitTransformedValue(loc, value, getBridgingSourceType(),
                                    getBridgingResultType(), C);

  case BridgeToObjC:
    return SGF.emitNativeToBridgedValue(loc, value,
                                        getBridgingSourceType(),
                                        getBridgingResultType(),
                                        getBridgingLoweredResultType(), C);

  case ForceAndBridgeToObjC: {
    auto &tl = SGF.getTypeLowering(value.getType());
    auto sourceValueType = getBridgingSourceType().getOptionalObjectType();
    value = SGF.emitCheckedGetOptionalValueFrom(loc, value,
                                                /*isImplicitUnwrap*/ true,
                                                tl, SGFContext());
    return SGF.emitNativeToBridgedValue(loc, value, sourceValueType,
                                        getBridgingResultType(),
                                        getBridgingLoweredResultType(), C);
  }

  case BridgeFromObjC:
    return SGF.emitBridgedToNativeValue(loc, value,
                                        getBridgingSourceType(),
                                        getBridgingResultType(),
                                        getBridgingLoweredResultType(), C);

  case BridgeResultFromObjC:
    return SGF.emitBridgedToNativeValue(loc, value,
                                        getBridgingSourceType(),
                                        getBridgingResultType(),
                                        getBridgingLoweredResultType(), C,
                                        /*isResult*/ true);

  case SubstToOrig:
    return SGF.emitSubstToOrigValue(loc, value,
                                    getReabstractionOrigType(),
                                    getReabstractionSubstType(),
                                    getReabstractionLoweredResultType(), C);

  case OrigToSubst:
    return SGF.emitOrigToSubstValue(loc, value,
                                    getReabstractionOrigType(),
                                    getReabstractionSubstType(),
                                    getReabstractionLoweredResultType(), C);
  }
  llvm_unreachable("bad kind");
}

llvm::Optional<Conversion>
Conversion::adjustForInitialOptionalConversions(CanType newSourceType) const {
  switch (getKind()) {
  case SubstToOrig:
  case OrigToSubst:
    // TODO: handle reabstraction conversions here, too.
    return llvm::None;

  case ForceAndBridgeToObjC:
    return llvm::None;

  case AnyErasure:
  case BridgeToObjC:
  case BridgeFromObjC:
  case BridgeResultFromObjC:
    return Conversion::getBridging(getKind(), newSourceType,
                                   getBridgingResultType(),
                                   getBridgingLoweredResultType(),
                                   isBridgingExplicit());
  }
  llvm_unreachable("bad kind");
}

llvm::Optional<Conversion> Conversion::adjustForInitialForceValue() const {
  switch (getKind()) {
  case SubstToOrig:
  case OrigToSubst:
  case AnyErasure:
  case BridgeFromObjC:
  case BridgeResultFromObjC:
  case ForceAndBridgeToObjC:
    return llvm::None;

  case BridgeToObjC: {
    auto sourceOptType =
      OptionalType::get(getBridgingSourceType())->getCanonicalType();
    return Conversion::getBridging(ForceAndBridgeToObjC,
                                   sourceOptType,
                                   getBridgingResultType(),
                                   getBridgingLoweredResultType(),
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
  out << name << "(orig: ";
  conversion.getReabstractionOrigType().print(out);
  out << ", subst: ";
  conversion.getReabstractionSubstType().print(out);
  out << ", loweredResult: ";
  conversion.getReabstractionLoweredResultType().print(out);
  out << ')';
}

static void printBridging(const Conversion &conversion, llvm::raw_ostream &out,
                          StringRef name) {
  out << name << "(from: ";
  conversion.getBridgingSourceType().print(out);
  out << ", to: ";
  conversion.getBridgingResultType().print(out);
  out << ", explicit: " << conversion.isBridgingExplicit() << ')';
}

void Conversion::print(llvm::raw_ostream &out) const {
  switch (getKind()) {
  case SubstToOrig:
    return printReabstraction(*this, out, "SubstToOrig");
  case OrigToSubst:
    return printReabstraction(*this, out, "OrigToSubst");
  case AnyErasure:
    return printBridging(*this, out, "AnyErasure");
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

llvm::Optional<ConversionPeepholeHint>
Lowering::canPeepholeConversions(SILGenFunction &SGF,
                                 const Conversion &outerConversion,
                                 const Conversion &innerConversion) {
  switch (outerConversion.getKind()) {
  case Conversion::OrigToSubst:
  case Conversion::SubstToOrig:
    switch (innerConversion.getKind()) {
    case Conversion::OrigToSubst:
    case Conversion::SubstToOrig:
      if (innerConversion.getKind() == outerConversion.getKind())
        break;

      if (innerConversion.getReabstractionOrigType().getCachingKey() !=
          outerConversion.getReabstractionOrigType().getCachingKey() ||
          innerConversion.getReabstractionSubstType() !=
          outerConversion.getReabstractionSubstType()) {
        break;
      }

      return ConversionPeepholeHint(ConversionPeepholeHint::Identity, false);

    default:
      break;
    }

    return llvm::None;

  case Conversion::AnyErasure:
  case Conversion::BridgeFromObjC:
  case Conversion::BridgeResultFromObjC:
    // TODO: maybe peephole bridging through a Swift type?
    // This isn't actually something that happens in normal code generation.
    return llvm::None;

  case Conversion::ForceAndBridgeToObjC:
  case Conversion::BridgeToObjC:
    switch (innerConversion.getKind()) {
    case Conversion::AnyErasure:
    case Conversion::BridgeFromObjC:
    case Conversion::BridgeResultFromObjC: {
      bool outerExplicit = outerConversion.isBridgingExplicit();
      bool innerExplicit = innerConversion.isBridgingExplicit();

      // Never peephole if both conversions are explicit; there might be
      // something the user's trying to do which we don't understand.
      if (outerExplicit && innerExplicit)
        return llvm::None;

      // Otherwise, we can peephole if we understand the resulting conversion
      // and applying the peephole doesn't change semantics.

      CanType sourceType = innerConversion.getBridgingSourceType();
      CanType intermediateType = innerConversion.getBridgingResultType();
      assert(intermediateType == outerConversion.getBridgingSourceType());

      // If we're doing a peephole involving a force, we want to propagate
      // the force to the source value.  If it's not in fact optional, that
      // won't work.
      bool forced =
        outerConversion.getKind() == Conversion::ForceAndBridgeToObjC;
      if (forced) {
        sourceType = sourceType.getOptionalObjectType();
        if (!sourceType)
          return llvm::None;
        intermediateType = intermediateType.getOptionalObjectType();
        assert(intermediateType);
      }

      CanType resultType = outerConversion.getBridgingResultType();
      SILType loweredSourceTy = SGF.getLoweredType(sourceType);
      SILType loweredResultTy = outerConversion.getBridgingLoweredResultType();

      auto applyPeephole = [&](ConversionPeepholeHint::Kind kind) {
        return ConversionPeepholeHint(kind, forced);
      };

      // Converting to Any doesn't do anything semantically special, so we
      // can apply the peephole unconditionally.
      if (isMatchedAnyToAnyObjectConversion(intermediateType, resultType)) {
        if (loweredSourceTy == loweredResultTy) {
          return applyPeephole(ConversionPeepholeHint::Identity);
        } else if (isValueToAnyConversion(sourceType, intermediateType)) {
          return applyPeephole(ConversionPeepholeHint::BridgeToAnyObject);
        } else {
          return applyPeephole(ConversionPeepholeHint::Subtype);
        }
      }

      // Otherwise, undoing a bridging conversions can change semantics by
      // e.g. removing a copy, so we shouldn't do it unless the special
      // syntactic bridging peephole applies.  That requires one of the
      // conversions to be explicit.
      // TODO: use special SILGen to preserve semantics in this case,
      // e.g. by making a copy.
      if (!outerExplicit && !innerExplicit) {
        return llvm::None;
      }

      // Okay, now we're in the domain of the bridging peephole: an
      // explicit bridging conversion can cancel out an implicit bridge
      // between related types.

      // If the source and destination types have exactly the same
      // representation, then (1) they're related and (2) we can directly
      // emit into the context.
      if (loweredSourceTy.getObjectType() == loweredResultTy.getObjectType()) {
        return applyPeephole(ConversionPeepholeHint::Identity);
      }

      // Look for a subtype relationship between the source and destination.
      if (areRelatedTypesForBridgingPeephole(sourceType, resultType)) {
        return applyPeephole(ConversionPeepholeHint::Subtype);
      }

      // If the inner conversion is a result conversion that removes
      // optionality, and the non-optional source type is a subtype of the
      // value type, this is just an implicit force.
      if (!forced &&
          innerConversion.getKind() == Conversion::BridgeResultFromObjC) {
        if (auto sourceValueType = sourceType.getOptionalObjectType()) {
          if (!intermediateType.getOptionalObjectType() &&
              areRelatedTypesForBridgingPeephole(sourceValueType, resultType)) {
            forced = true;
            return applyPeephole(ConversionPeepholeHint::Subtype);
          }
        }
      }

      return llvm::None;
    }

    default:
      return llvm::None;
    }
  }
  llvm_unreachable("bad kind");
}
