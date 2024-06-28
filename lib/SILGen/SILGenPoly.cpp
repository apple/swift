//===--- SILGenPoly.cpp - Function Type Thunks ----------------------------===//
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
// Swift function types can be equivalent or have a subtyping relationship even
// if the SIL-level lowering of the calling convention is different. The
// routines in this file implement thunking between lowered function types.
//
//
// Re-abstraction thunks
// =====================
// After SIL type lowering, generic substitutions become explicit, for example
// the AST type (Int) -> Int passes the Ints directly, whereas (T) -> T with Int
// substituted for T will pass the Ints like a T, as an address-only value with
// opaque type metadata. Such a thunk is called a "re-abstraction thunk" -- the
// AST-level type of the function value does not change, only the manner in
// which parameters and results are passed. See the comment in
// AbstractionPattern.h for details.
//
// Function conversion thunks
// ==========================
// In Swift's AST-level type system, certain types have a subtype relation
// involving a representation change. For example, a concrete type is always
// a subtype of any protocol it conforms to. The upcast from the concrete
// type to an existential type for the protocol requires packaging the
// payload together with type metadata and witness tables.
//
// Between function types, the type (A) -> B is defined to be a subtype of
// (A') -> B' iff A' is a subtype of A, and B is a subtype of B' -- parameters
// are contravariant, and results are covariant.
//
// A subtype conversion of a function value (A) -> B is performed by wrapping
// the function value in a thunk of type (A') -> B'. The thunk takes an A' and
// converts it into an A, calls the inner function value, and converts the
// result from B to B'.
//
// VTable thunks
// =============
//
// If a base class is generic and a derived class substitutes some generic
// parameter of the base with a concrete type, the derived class can override
// methods in the base that involved generic types. In the derived class, a
// method override that involves substituted types will have a different
// SIL lowering than the base method. In this case, the overridden vtable entry
// will point to a thunk which transforms parameters and results and invokes
// the derived method.
//
// Some limited forms of subtyping are also supported for method overrides;
// namely, a derived method's parameter can be a superclass of, or more
// optional than, a parameter of the base, and result can be a subclass of,
// or less optional than, the result of the base.
//
// Witness thunks
// ==============
//
// Protocol witness methods are called with an additional generic parameter
// bound to the Self type, and thus always require a thunk. Thunks are also
// required for conditional conformances, since the extra requirements aren't
// part of the protocol and so any witness tables need to be loaded from the
// original protocol's witness table and passed into the real witness method.
//
// Thunks for class method witnesses dispatch through the vtable allowing
// inherited witnesses to be overridden in subclasses. Hence a witness thunk
// might require two levels of abstraction difference -- the method might
// override a base class method with more generic types, and the protocol
// requirement may involve associated types which are always concrete in the
// conforming class.
//
// Other thunks
// ============
//
// Foreign-to-native, native-to-foreign thunks for declarations and function
// values are implemented in SILGenBridging.cpp.
//
//===----------------------------------------------------------------------===//

#define DEBUG_TYPE "silgen-poly"
#include "ExecutorBreadcrumb.h"
#include "FunctionInputGenerator.h"
#include "Initialization.h"
#include "LValue.h"
#include "RValue.h"
#include "SILGen.h"
#include "SILGenFunction.h"
#include "SILGenFunctionBuilder.h"
#include "Scope.h"
#include "TupleGenerators.h"
#include "swift/Basic/Assertions.h"
#include "swift/Basic/Generators.h"
#include "swift/AST/ASTMangler.h"
#include "swift/AST/Decl.h"
#include "swift/AST/DiagnosticsSIL.h"
#include "swift/AST/ExistentialLayout.h"
#include "swift/AST/GenericEnvironment.h"
#include "swift/AST/Module.h"
#include "swift/AST/ProtocolConformance.h"
#include "swift/AST/TypeCheckRequests.h"
#include "swift/AST/Types.h"
#include "swift/SIL/PrettyStackTrace.h"
#include "swift/SIL/AbstractionPatternGenerators.h"
#include "swift/SIL/SILArgument.h"
#include "swift/SIL/TypeLowering.h"
#include "llvm/Support/Compiler.h"

using namespace swift;
using namespace Lowering;

static ParameterConvention
getScalarConventionForPackConvention(ParameterConvention conv) {
  switch (conv) {
  case ParameterConvention::Pack_Owned:
    return ParameterConvention::Indirect_In;
  case ParameterConvention::Pack_Guaranteed:
    return ParameterConvention::Indirect_In_Guaranteed;
  case ParameterConvention::Pack_Inout:
    return ParameterConvention::Indirect_Inout;
  default:
    llvm_unreachable("not a pack convention");
  }
  llvm_unreachable("bad convention");
}

namespace {

class IndirectSlot {
  llvm::PointerUnion<SILValue, SILType> value;
public:
  explicit IndirectSlot(SILType type) : value(type) {}
  IndirectSlot(SILValue address) : value(address) {
    assert(address->getType().isAddress());
  }

  SILType getType() const {
    if (value.is<SILValue>()) {
      return value.get<SILValue>()->getType();
    } else {
      return value.get<SILType>();
    }
  }

  bool hasAddress() const { return value.is<SILValue>(); }

  SILValue getAddress() const {
    return value.get<SILValue>();
  }

  SILValue allocate(SILGenFunction &SGF, SILLocation loc) const {
    if (hasAddress()) return getAddress();
    return SGF.emitTemporaryAllocation(loc, getType());
  }
};

} // end anonymous namespace

static bool hasAbstractionDifference(SILType resultType1,
                                     SILType resultType2) {
  return resultType1.getASTType() != resultType2.getASTType();
}

static bool hasAbstractionDifference(IndirectSlot resultSlot,
                                     SILValue resultAddr) {
  return hasAbstractionDifference(resultSlot.getType(),
                                  resultAddr->getType());
}

static bool hasAbstractionDifference(IndirectSlot resultSlot,
                                     SILResultInfo resultInfo) {
  return resultSlot.getType().getASTType()
      != resultInfo.getInterfaceType();
}

/// A helper function that pulls an element off the front of an array.
template <class T>
static const T &claimNext(ArrayRef<T> &array) {
  assert(!array.empty() && "claiming next from empty array!");
  const T &result = array.front();
  array = array.slice(1);
  return result;
}

namespace {
  /// An abstract class for transforming first-class SIL values.
  class Transform {
  private:
    SILGenFunction &SGF;
    SILLocation Loc;

  public:
    Transform(SILGenFunction &SGF, SILLocation loc) : SGF(SGF), Loc(loc) {}
    virtual ~Transform() = default;

    /// Transform an arbitrary value.
    RValue transform(RValue &&input,
                     AbstractionPattern inputOrigType,
                     CanType inputSubstType,
                     AbstractionPattern outputOrigType,
                     CanType outputSubstType,
                     SILType outputLoweredTy,
                     SGFContext ctxt);

    /// Transform an arbitrary value.
    ManagedValue transform(ManagedValue input,
                           AbstractionPattern inputOrigType,
                           CanType inputSubstType,
                           AbstractionPattern outputOrigType,
                           CanType outputSubstType,
                           SILType outputLoweredTy,
                           SGFContext ctxt);

    /// Transform a metatype value.
    ManagedValue transformMetatype(ManagedValue fn,
                                   AbstractionPattern inputOrigType,
                                   CanMetatypeType inputSubstType,
                                   AbstractionPattern outputOrigType,
                                   CanMetatypeType outputSubstType,
                                   SILType outputLoweredTy);

    /// Transform a tuple value.
    ManagedValue transformTuple(ManagedValue input,
                                AbstractionPattern inputOrigType,
                                CanTupleType inputSubstType,
                                AbstractionPattern outputOrigType,
                                CanTupleType outputSubstType,
                                SILType outputLoweredTy,
                                SGFContext ctxt);

    /// Transform a function value.
    ManagedValue transformFunction(ManagedValue fn,
                                   AbstractionPattern inputOrigType,
                                   CanAnyFunctionType inputSubstType,
                                   AbstractionPattern outputOrigType,
                                   CanAnyFunctionType outputSubstType,
                                   const TypeLowering &expectedTL);
  };
} // end anonymous namespace

ManagedValue
SILGenFunction::emitTransformExistential(SILLocation loc,
                                         ManagedValue input,
                                         CanType inputType,
                                         CanType outputType,
                                         SGFContext ctxt) {
  assert(inputType != outputType);

  FormalEvaluationScope scope(*this);

  if (inputType->isAnyExistentialType()) {
    CanType openedType = OpenedArchetypeType::getAny(inputType,
                                                     F.getGenericSignature());
    SILType loweredOpenedType = getLoweredType(openedType);

    input = emitOpenExistential(loc, input,
                                loweredOpenedType, AccessKind::Read);
    inputType = openedType;
  }

  // Build conformance table
  CanType fromInstanceType = inputType;
  CanType toInstanceType = outputType;
  
  // Look through metatypes
  while (isa<MetatypeType>(fromInstanceType) &&
         isa<ExistentialMetatypeType>(toInstanceType)) {
    fromInstanceType = cast<MetatypeType>(fromInstanceType)
      .getInstanceType();
    toInstanceType = cast<ExistentialMetatypeType>(toInstanceType)
      ->getExistentialInstanceType()->getCanonicalType();
  }

  assert(!fromInstanceType.isAnyExistentialType());
  ArrayRef<ProtocolConformanceRef> conformances =
      SGM.M.getSwiftModule()->collectExistentialConformances(
                                     fromInstanceType,
                                     toInstanceType,
                                     /*allowMissing=*/true);

  // Build result existential
  AbstractionPattern opaque = AbstractionPattern::getOpaque();
  const TypeLowering &concreteTL = getTypeLowering(opaque, inputType);
  const TypeLowering &expectedTL = getTypeLowering(outputType);
  return emitExistentialErasure(
                   loc, inputType, concreteTL, expectedTL,
                   conformances, ctxt,
                   [&](SGFContext C) -> ManagedValue {
                     return manageOpaqueValue(input, loc, C);
                   });
}

/// Apply this transformation to an arbitrary value.
RValue Transform::transform(RValue &&input,
                            AbstractionPattern inputOrigType,
                            CanType inputSubstType,
                            AbstractionPattern outputOrigType,
                            CanType outputSubstType,
                            SILType outputLoweredTy,
                            SGFContext ctxt) {
  // Fast path: we don't have a tuple.
  auto inputTupleType = dyn_cast<TupleType>(inputSubstType);
  if (!inputTupleType) {
    assert(!isa<TupleType>(outputSubstType) &&
           "transformation introduced a tuple?");
    auto result = transform(std::move(input).getScalarValue(),
                            inputOrigType, inputSubstType,
                            outputOrigType, outputSubstType,
                            outputLoweredTy, ctxt);
    return RValue(SGF, Loc, outputSubstType, result);
  }

  // Okay, we have a tuple.  The output type will also be a tuple unless
  // there's a subtyping conversion that erases tuples, but that's currently
  // not allowed by the typechecker, which considers existential erasure to
  // be a conversion relation, not a subtyping one.  Anyway, it would be
  // possible to support that here, but since it's not currently required...
  assert(isa<TupleType>(outputSubstType) &&
         "subtype constraint erasing tuple is not currently implemented");
  auto outputTupleType = cast<TupleType>(outputSubstType);
  assert(inputTupleType->getNumElements() == outputTupleType->getNumElements());
  assert(outputLoweredTy.is<TupleType>() &&
         "expected lowered output type wasn't a tuple when formal type was");
  assert(outputLoweredTy.castTo<TupleType>()->getNumElements() ==
           outputTupleType->getNumElements());

  // Pull the r-value apart.
  SmallVector<RValue, 8> inputElts;
  std::move(input).extractElements(inputElts);

  // Emit into the context initialization if it's present and possible
  // to split.
  SmallVector<InitializationPtr, 4> eltInitsBuffer;
  MutableArrayRef<InitializationPtr> eltInits;
  auto tupleInit = ctxt.getEmitInto();
  if (!ctxt.getEmitInto()
      || !ctxt.getEmitInto()->canSplitIntoTupleElements()) {
    tupleInit = nullptr;
  } else {
    eltInits = tupleInit->splitIntoTupleElements(SGF, Loc, outputTupleType,
                                                 eltInitsBuffer);
  }

  // At this point, if tupleInit is non-null, we must emit all of the
  // elements into their corresponding contexts.
  assert(tupleInit == nullptr ||
         eltInits.size() == inputTupleType->getNumElements());

  SmallVector<ManagedValue, 8> outputExpansion;
  for (auto eltIndex : indices(inputTupleType->getElementTypes())) {
    // Determine the appropriate context for the element.
    SGFContext eltCtxt;
    if (tupleInit) eltCtxt = SGFContext(eltInits[eltIndex].get());

    // Recurse.
    RValue outputElt = transform(std::move(inputElts[eltIndex]),
                                 inputOrigType.getTupleElementType(eltIndex),
                                 inputTupleType.getElementType(eltIndex),
                                 outputOrigType.getTupleElementType(eltIndex),
                                 outputTupleType.getElementType(eltIndex),
                                 outputLoweredTy.getTupleElementType(eltIndex),
                                 eltCtxt);

    // Force the r-value into its context if necessary.
    assert(!outputElt.isInContext() || tupleInit != nullptr);
    if (tupleInit && !outputElt.isInContext()) {
      std::move(outputElt).forwardInto(SGF, Loc, eltInits[eltIndex].get());
    } else {
      std::move(outputElt).getAll(outputExpansion);
    }
  }

  // If we emitted into context, be sure to finish the overall initialization.
  if (tupleInit) {
    tupleInit->finishInitialization(SGF);
    return RValue::forInContext();
  }

  return RValue(SGF, outputExpansion, outputTupleType);
}

// Single @objc protocol value metatypes can be converted to the ObjC
// Protocol class type.
static bool isProtocolClass(Type t) {
  auto classDecl = t->getClassOrBoundGenericClass();
  if (!classDecl)
    return false;

  ASTContext &ctx = classDecl->getASTContext();
  return (classDecl->getName() == ctx.Id_Protocol &&
          classDecl->getModuleContext()->getName() == ctx.Id_ObjectiveC);
}

static ManagedValue emitManagedLoad(SILGenFunction &SGF, SILLocation loc,
                                    ManagedValue addr,
                                    const TypeLowering &addrTL) {
  // SEMANTIC ARC TODO: When the verifier is finished, revisit this.
  if (!addr.hasCleanup())
    return SGF.B.createLoadBorrow(loc, addr);

  auto loadedValue = addrTL.emitLoad(SGF.B, loc, addr.forward(SGF),
                                     LoadOwnershipQualifier::Take);
  return SGF.emitManagedRValueWithCleanup(loadedValue, addrTL);
}

/// Apply this transformation to an arbitrary value.
ManagedValue Transform::transform(ManagedValue v,
                                  AbstractionPattern inputOrigType,
                                  CanType inputSubstType,
                                  AbstractionPattern outputOrigType,
                                  CanType outputSubstType,
                                  SILType loweredResultTy,
                                  SGFContext ctxt) {
  // Load if the result isn't address-only.  All the translation routines
  // expect this.
  if (v.getType().isAddress()) {
    auto &inputTL = SGF.getTypeLowering(v.getType());
    if (!inputTL.isAddressOnly() || !SGF.silConv.useLoweredAddresses()) {
      v = emitManagedLoad(SGF, Loc, v, inputTL);
    }
  }

  // Downstream code expects the lowered result type to be an object if
  // it's loadable, so make sure that's satisfied.
  auto &expectedTL = SGF.getTypeLowering(loweredResultTy);
  loweredResultTy = expectedTL.getLoweredType();

  // Nothing to convert
  if (v.getType() == loweredResultTy)
    return v;

  CanType inputObjectType = inputSubstType.getOptionalObjectType();
  bool inputIsOptional = (bool) inputObjectType;

  CanType outputObjectType = outputSubstType.getOptionalObjectType();
  bool outputIsOptional = (bool) outputObjectType;

  // If the value is less optional than the desired formal type, wrap in
  // an optional.
  if (outputIsOptional && !inputIsOptional) {
    return SGF.emitInjectOptional(
        Loc, expectedTL, ctxt, [&](SGFContext objectCtxt) {
          return transform(v, inputOrigType, inputSubstType,
                           outputOrigType.getOptionalObjectType(),
                           outputObjectType,
                           loweredResultTy.getOptionalObjectType(),
                           objectCtxt);
        });
  }

  // If the value is an optional, but the desired formal type isn't an
  // optional or Any, force it.
  if (inputIsOptional && !outputIsOptional &&
      !outputSubstType->isExistentialType()) {
    // isImplicitUnwrap is hardcoded true because the looseness in types of
    // @objc witnesses/overrides that we're handling here only allows IUOs,
    // not explicit Optionals.
    v = SGF.emitCheckedGetOptionalValueFrom(Loc, v,
                                            /*isImplicitUnwrap*/ true, 
                                            SGF.getTypeLowering(v.getType()),
                                            SGFContext());

    // Check if we have any more conversions remaining.
    if (v.getType() == loweredResultTy)
      return v;

    inputIsOptional = false;
  }

  // Optional-to-optional conversion.
  if (inputIsOptional && outputIsOptional) {
    // If the conversion is trivial, just cast.
    if (SGF.SGM.Types.checkForABIDifferences(SGF.SGM.M,
                                             v.getType(), loweredResultTy)
          == TypeConverter::ABIDifference::CompatibleRepresentation) {
      if (v.getType().isAddress())
        return SGF.B.createUncheckedAddrCast(Loc, v, loweredResultTy);
      return SGF.B.createUncheckedBitCast(Loc, v, loweredResultTy);
    }

    auto transformOptionalPayload =
        [&](SILGenFunction &SGF, SILLocation loc, ManagedValue input,
            SILType loweredResultTy, SGFContext context) -> ManagedValue {
      return transform(input, inputOrigType.getOptionalObjectType(),
                       inputObjectType, outputOrigType.getOptionalObjectType(),
                       outputObjectType, loweredResultTy, context);
    };

    return SGF.emitOptionalToOptional(Loc, v, loweredResultTy,
                                      transformOptionalPayload);
  }
  
  // Abstraction changes:

  //  - functions
  if (auto outputFnType = dyn_cast<AnyFunctionType>(outputSubstType)) {
    auto inputFnType = cast<AnyFunctionType>(inputSubstType);
    return transformFunction(v,
                             inputOrigType, inputFnType,
                             outputOrigType, outputFnType,
                             expectedTL);
  }

  //  - tuples of transformable values
  if (auto outputTupleType = dyn_cast<TupleType>(outputSubstType)) {
    auto inputTupleType = cast<TupleType>(inputSubstType);
    return transformTuple(v,
                          inputOrigType, inputTupleType,
                          outputOrigType, outputTupleType,
                          loweredResultTy, ctxt);
  }

  //  - metatypes
  if (auto outputMetaType = dyn_cast<MetatypeType>(outputSubstType)) {
    if (auto inputMetaType = dyn_cast<MetatypeType>(inputSubstType)) {
      return transformMetatype(v,
                               inputOrigType, inputMetaType,
                               outputOrigType, outputMetaType,
                               loweredResultTy);
    }
  }

  // Subtype conversions:

  // A base class method returning Self can be used in place of a derived
  // class method returning Self.
  if (auto inputSelfType = dyn_cast<DynamicSelfType>(inputSubstType)) {
    inputSubstType = inputSelfType.getSelfType();
  }

  //  - casts for classes
  if (outputSubstType->getClassOrBoundGenericClass() &&
      inputSubstType->getClassOrBoundGenericClass()) {
    auto class1 = inputSubstType->getClassOrBoundGenericClass();
    auto class2 = outputSubstType->getClassOrBoundGenericClass();

    // CF <-> Objective-C via toll-free bridging.
    if ((class1->getForeignClassKind() == ClassDecl::ForeignKind::CFType) ^
        (class2->getForeignClassKind() == ClassDecl::ForeignKind::CFType)) {
      return SGF.B.createUncheckedRefCast(Loc, v, loweredResultTy);
    }

    if (outputSubstType->isExactSuperclassOf(inputSubstType)) {
      // Upcast to a superclass.
      return SGF.B.createUpcast(Loc, v, loweredResultTy);
    } else {
      // FIXME: Should only happen with the DynamicSelfType case above,
      // except that convenience inits return the static self and not
      // DynamicSelfType.
      assert(inputSubstType->isExactSuperclassOf(outputSubstType)
             && "should be inheritance relationship between input and output");
      return SGF.B.createUncheckedRefCast(Loc, v, loweredResultTy);
    }
  }

  // - upcasts for collections
  if (outputSubstType->getStructOrBoundGenericStruct() &&
      inputSubstType->getStructOrBoundGenericStruct()) {
    auto *inputStruct = inputSubstType->getStructOrBoundGenericStruct();
    auto *outputStruct = outputSubstType->getStructOrBoundGenericStruct();

    // Attempt collection upcast only if input and output declarations match.
    if (inputStruct == outputStruct) {
      FuncDecl *fn = nullptr;
      if (inputSubstType->isArray()) {
        fn = SGF.SGM.getArrayForceCast(Loc);
      } else if (inputSubstType->isDictionary()) {
        fn = SGF.SGM.getDictionaryUpCast(Loc);
      } else if (inputSubstType->isSet()) {
        fn = SGF.SGM.getSetUpCast(Loc);
      } else {
        llvm::report_fatal_error("unsupported collection upcast kind");
      }

      return SGF.emitCollectionConversion(Loc, fn, inputSubstType,
                                          outputSubstType, v, ctxt)
                .getScalarValue();
    }
  }

  //  - upcasts from an archetype
  if (outputSubstType->getClassOrBoundGenericClass()) {
    if (auto archetypeType = dyn_cast<ArchetypeType>(inputSubstType)) {
      if (archetypeType->getSuperclass()) {
        // Replace the cleanup with a new one on the superclass value so we
        // always use concrete retain/release operations.
        return SGF.B.createUpcast(Loc, v, loweredResultTy);
      }
    }
  }

  // - metatype to Protocol conversion
  if (isProtocolClass(outputSubstType)) {
    if (auto metatypeTy = dyn_cast<MetatypeType>(inputSubstType)) {
      return SGF.emitProtocolMetatypeToObject(Loc, metatypeTy,
                                   SGF.getLoweredLoadableType(outputSubstType));
    }
  }

  // - metatype to AnyObject conversion
  if (outputSubstType->isAnyObject() &&
      isa<MetatypeType>(inputSubstType)) {
    return SGF.emitClassMetatypeToObject(Loc, v,
                                   SGF.getLoweredLoadableType(outputSubstType));
  }
  
  // - existential metatype to AnyObject conversion
  if (outputSubstType->isAnyObject() &&
      isa<ExistentialMetatypeType>(inputSubstType)) {
    return SGF.emitExistentialMetatypeToObject(Loc, v,
                                   SGF.getLoweredLoadableType(outputSubstType));
  }

  // - block to AnyObject conversion (under ObjC interop)
  if (outputSubstType->isAnyObject() &&
      SGF.getASTContext().LangOpts.EnableObjCInterop) {
    if (auto inputFnType = dyn_cast<AnyFunctionType>(inputSubstType)) {
      if (inputFnType->getRepresentation() == FunctionTypeRepresentation::Block)
        return SGF.B.createBlockToAnyObject(Loc, v, loweredResultTy);
    }
  }

  //  - existentials
  if (outputSubstType->isAnyExistentialType()) {
    // We have to re-abstract payload if its a metatype or a function
    v = SGF.emitSubstToOrigValue(Loc, v, AbstractionPattern::getOpaque(),
                                 inputSubstType);
    return SGF.emitTransformExistential(Loc, v,
                                        inputSubstType, outputSubstType,
                                        ctxt);
  }

  // - upcasting class-constrained existentials or metatypes thereof
  if (inputSubstType->isAnyExistentialType()) {
    auto instanceType = inputSubstType;
    while (auto metatypeType = dyn_cast<ExistentialMetatypeType>(instanceType))
      instanceType = metatypeType.getInstanceType();

    auto layout = instanceType.getExistentialLayout();
    if (layout.getSuperclass()) {
      CanType openedType =
          OpenedArchetypeType::getAny(inputSubstType,
                                      SGF.F.getGenericSignature());
      SILType loweredOpenedType = SGF.getLoweredType(openedType);

      FormalEvaluationScope scope(SGF);

      auto payload = SGF.emitOpenExistential(Loc, v,
                                             loweredOpenedType,
                                             AccessKind::Read);
      payload = payload.ensurePlusOne(SGF, Loc);
      return transform(payload,
                       AbstractionPattern::getOpaque(),
                       openedType,
                       outputOrigType,
                       outputSubstType,
                       loweredResultTy,
                       ctxt);
    }
  }

  // - T : Hashable to AnyHashable
  if (outputSubstType->isAnyHashable()) {
    auto *protocol = SGF.getASTContext().getProtocol(
        KnownProtocolKind::Hashable);
    auto conformance = SGF.SGM.M.getSwiftModule()->lookupConformance(
        inputSubstType, protocol);
    auto addr = v.getType().isAddress() ? v : v.materialize(SGF, Loc);
    auto result = SGF.emitAnyHashableErasure(Loc, addr, inputSubstType,
                                             conformance, ctxt);
    if (result.isInContext())
      return ManagedValue::forInContext();
    return std::move(result).getAsSingleValue(SGF, Loc);
  }

  // Should have handled the conversion in one of the cases above.
  v.dump();
  llvm_unreachable("Unhandled transform?");
}

ManagedValue Transform::transformMetatype(ManagedValue meta,
                                          AbstractionPattern inputOrigType,
                                          CanMetatypeType inputSubstType,
                                          AbstractionPattern outputOrigType,
                                          CanMetatypeType outputSubstType,
                                          SILType expectedType) {
  assert(!meta.hasCleanup() && "metatype with cleanup?!");

  auto wasRepr = meta.getType().castTo<MetatypeType>()->getRepresentation();
  auto willBeRepr = expectedType.castTo<MetatypeType>()->getRepresentation();
  
  SILValue result;

  if ((wasRepr == MetatypeRepresentation::Thick &&
       willBeRepr == MetatypeRepresentation::Thin) ||
      (wasRepr == MetatypeRepresentation::Thin &&
       willBeRepr == MetatypeRepresentation::Thick)) {
    // If we have a thin-to-thick abstraction change, cook up new a metatype
    // value out of nothing -- thin metatypes carry no runtime state.
    result = SGF.B.createMetatype(Loc, expectedType);
  } else {
    // Otherwise, we have a metatype subtype conversion of thick metatypes.
    assert(wasRepr == willBeRepr && "Unhandled metatype conversion");
    result = SGF.B.createUpcast(Loc, meta.getUnmanagedValue(), expectedType);
  }

  return ManagedValue::forObjectRValueWithoutOwnership(result);
}

/// Explode a managed tuple into a bunch of managed elements.
///
/// If the tuple is in memory, the result elements will also be in
/// memory.
static void explodeTuple(SILGenFunction &SGF, SILLocation loc,
                         ManagedValue managedTuple,
                         SmallVectorImpl<ManagedValue> &out) {
  // If the tuple is empty, there's nothing to do.
  if (managedTuple.getType().castTo<TupleType>()->getNumElements() == 0)
    return;

  SmallVector<SILValue, 16> elements;
  bool isPlusOne = managedTuple.hasCleanup();

  if (managedTuple.getType().isAddress()) {
    SGF.B.emitDestructureAddressOperation(loc, managedTuple.forward(SGF),
                                          elements);
  } else {
    SGF.B.emitDestructureValueOperation(loc, managedTuple.forward(SGF),
                                        elements);
  }

  for (auto element : elements) {
    if (element->getType().isTrivial(SGF.F)) {
      out.push_back(ManagedValue::forRValueWithoutOwnership(element));
      continue;
    }

    if (!isPlusOne) {
      out.push_back(ManagedValue::forBorrowedRValue(element));
      continue;
    }

    if (element->getType().isAddress()) {
      out.push_back(SGF.emitManagedBufferWithCleanup(element));
      continue;
    }

    out.push_back(SGF.emitManagedRValueWithCleanup(element));
  }
}

/// Apply this transformation to all the elements of a tuple value,
/// which just entails mapping over each of its component elements.
ManagedValue Transform::transformTuple(ManagedValue inputTuple,
                                       AbstractionPattern inputOrigType,
                                       CanTupleType inputSubstType,
                                       AbstractionPattern outputOrigType,
                                       CanTupleType outputSubstType,
                                       SILType outputLoweredTy,
                                       SGFContext ctxt) {
  const TypeLowering &outputTL =
    SGF.getTypeLowering(outputLoweredTy);
  assert((outputTL.isAddressOnly() == inputTuple.getType().isAddress() ||
          !SGF.silConv.useLoweredAddresses()) &&
         "expected loadable inputs to have been loaded");

  // If there's no representation difference, we're done.
  if (outputLoweredTy == inputTuple.getType().copyCategory(outputLoweredTy))
    return inputTuple;

  assert(inputOrigType.matchesTuple(outputSubstType));
  assert(outputOrigType.matchesTuple(outputSubstType));

  auto inputType = inputTuple.getType().castTo<TupleType>();
  assert(outputSubstType->getNumElements() == inputType->getNumElements());

  // If the tuple is address only, we need to do the operation in memory.
  SILValue outputAddr;
  if (outputTL.isAddressOnly() && SGF.silConv.useLoweredAddresses())
    outputAddr = SGF.getBufferForExprResult(Loc, outputLoweredTy, ctxt);

  // Explode the tuple into individual managed values.
  SmallVector<ManagedValue, 4> inputElts;
  explodeTuple(SGF, Loc, inputTuple, inputElts);

  // Track all the managed elements whether or not we're actually
  // emitting to an address, just so that we can disable them after.
  SmallVector<ManagedValue, 4> outputElts;

  for (auto index : indices(inputType->getElementTypes())) {
    auto &inputEltTL = SGF.getTypeLowering(inputElts[index].getType());
    ManagedValue inputElt = inputElts[index];
    if (inputElt.getType().isAddress() && !inputEltTL.isAddressOnly()) {
      inputElt = emitManagedLoad(SGF, Loc, inputElt, inputEltTL);
    }

    auto inputEltOrigType = inputOrigType.getTupleElementType(index);
    auto inputEltSubstType = inputSubstType.getElementType(index);
    auto outputEltOrigType = outputOrigType.getTupleElementType(index);
    auto outputEltSubstType = outputSubstType.getElementType(index);
    auto outputEltLoweredTy = outputLoweredTy.getTupleElementType(index);

    // If we're emitting to memory, project out this element in the
    // destination buffer, then wrap that in an Initialization to
    // track the cleanup.
    std::optional<TemporaryInitialization> outputEltTemp;
    if (outputAddr) {
      SILValue outputEltAddr =
        SGF.B.createTupleElementAddr(Loc, outputAddr, index);
      auto &outputEltTL = SGF.getTypeLowering(outputEltLoweredTy);
      assert(outputEltTL.isAddressOnly() == inputEltTL.isAddressOnly());
      auto cleanup =
        SGF.enterDormantTemporaryCleanup(outputEltAddr, outputEltTL);
      outputEltTemp.emplace(outputEltAddr, cleanup);
    }

    SGFContext eltCtxt =
      (outputEltTemp ? SGFContext(&outputEltTemp.value()) : SGFContext());
    auto outputElt = transform(inputElt,
                               inputEltOrigType, inputEltSubstType,
                               outputEltOrigType, outputEltSubstType,
                               outputEltLoweredTy, eltCtxt);

    // If we're not emitting to memory, remember this element for
    // later assembly into a tuple.
    if (!outputEltTemp) {
      assert(outputElt);
      assert(!inputEltTL.isAddressOnly() || !SGF.silConv.useLoweredAddresses());
      outputElts.push_back(outputElt);
      continue;
    }

    // Otherwise, make sure we emit into the slot.
    auto &temp = outputEltTemp.value();
    auto outputEltAddr = temp.getManagedAddress();

    // That might involve storing directly.
    if (!outputElt.isInContext()) {
      outputElt.forwardInto(SGF, Loc, outputEltAddr.getValue());
      temp.finishInitialization(SGF);
    }

    outputElts.push_back(outputEltAddr);
  }

  // Okay, disable all the individual element cleanups and collect
  // the values for a potential tuple aggregate.
  SmallVector<SILValue, 4> outputEltValues;
  for (auto outputElt : outputElts) {
    SILValue value = outputElt.forward(SGF);
    if (!outputAddr) outputEltValues.push_back(value);
  }

  // If we're emitting to an address, just manage that.
  if (outputAddr)
    return SGF.manageBufferForExprResult(outputAddr, outputTL, ctxt);

  // Otherwise, assemble the tuple value and manage that.
  auto outputTuple =
    SGF.B.createTuple(Loc, outputLoweredTy, outputEltValues);
  return SGF.emitManagedRValueWithCleanup(outputTuple, outputTL);
}

void SILGenFunction::collectThunkParams(
    SILLocation loc, SmallVectorImpl<ManagedValue> &params,
    SmallVectorImpl<ManagedValue> *indirectResults,
    SmallVectorImpl<ManagedValue> *indirectErrors) {
  // Add the indirect results.
  for (auto resultTy : F.getConventions().getIndirectSILResultTypes(
           getTypeExpansionContext())) {
    auto paramTy = F.mapTypeIntoContext(resultTy);
    // Lower result parameters in the context of the function: opaque result
    // types will be lowered to their underlying type if allowed by resilience.
    auto inContextParamTy = F.getLoweredType(paramTy.getASTType())
                                .getCategoryType(paramTy.getCategory());
    SILArgument *arg = F.begin()->createFunctionArgument(inContextParamTy);
    if (indirectResults)
      indirectResults->push_back(ManagedValue::forLValue(arg));
  }

  if (F.getConventions().hasIndirectSILErrorResults()) {
    assert(F.getConventions().getNumIndirectSILErrorResults() == 1);
    auto paramTy = F.mapTypeIntoContext(
                       F.getConventions().getSILErrorType(getTypeExpansionContext()));
    auto inContextParamTy = F.getLoweredType(paramTy.getASTType())
                                .getCategoryType(paramTy.getCategory());
    SILArgument *arg = F.begin()->createFunctionArgument(inContextParamTy);
    if (indirectErrors)
      indirectErrors->push_back(ManagedValue::forLValue(arg));

    IndirectErrorResult = arg;
  }

  // Add the parameters.
  auto paramTypes = F.getLoweredFunctionType()->getParameters();
  for (auto param : paramTypes) {
    auto paramTy = F.mapTypeIntoContext(
        F.getConventions().getSILType(param, getTypeExpansionContext()));
    // Lower parameters in the context of the function: opaque result types will
    // be lowered to their underlying type if allowed by resilience.
    auto inContextParamTy = F.getLoweredType(paramTy.getASTType())
                                .getCategoryType(paramTy.getCategory());
    params.push_back(B.createInputFunctionArgument(inContextParamTy, loc));
  }
}

/// If the inner function we are calling (with type \c fnType) from the thunk
/// created by \c SGF requires an indirect error argument, returns that
/// argument.
static std::optional<SILValue>
emitThunkIndirectErrorArgument(SILGenFunction &SGF, SILLocation loc,
                               CanSILFunctionType fnType) {
  // If the function we're calling has an indirect error result, create an
  // argument for it.
  auto innerError = fnType->getOptionalErrorResult();
  if (!innerError || innerError->getConvention() != ResultConvention::Indirect)
    return std::nullopt;

  // If the type of the indirect error is the same for both the inner
  // function and the thunk, so we can re-use the indirect error slot.
  auto loweredErrorResultType = SGF.getSILType(*innerError, fnType);
  if (SGF.IndirectErrorResult &&
      SGF.IndirectErrorResult->getType().getObjectType()
          == loweredErrorResultType) {
    return SGF.IndirectErrorResult;
  }

  // The type of the indirect error in the inner function differs from
  // that of the thunk, or the thunk has a direct error, so allocate a
  // stack location for the inner indirect error.
  SILValue innerIndirectErrorAddr =
      SGF.B.createAllocStack(loc, loweredErrorResultType);
  SGF.enterDeallocStackCleanup(innerIndirectErrorAddr);

  return innerIndirectErrorAddr;
}

namespace {

class TranslateIndirect : public Cleanup {
  AbstractionPattern InputOrigType, OutputOrigType;
  CanType InputSubstType, OutputSubstType;
  SILValue Input, Output;

public:
  TranslateIndirect(AbstractionPattern inputOrigType, CanType inputSubstType,
                    AbstractionPattern outputOrigType, CanType outputSubstType,
                    SILValue input, SILValue output)
    : InputOrigType(inputOrigType), OutputOrigType(outputOrigType),
      InputSubstType(inputSubstType), OutputSubstType(outputSubstType),
      Input(input), Output(output) {
    assert(input->getType().isAddress());
    assert(output->getType().isAddress());
  }

  void emit(SILGenFunction &SGF, CleanupLocation loc,
            ForUnwind_t forUnwind) override {
    FullExpr scope(SGF.Cleanups, loc);

    // Re-assert ownership of the input value.
    auto inputMV = SGF.emitManagedBufferWithCleanup(Input);

    // Set up an initialization of the output buffer.
    auto &outputTL = SGF.getTypeLowering(Output->getType());
    auto outputInit = SGF.useBufferAsTemporary(Output, outputTL);

    // Transform into the output buffer.
    auto mv = SGF.emitTransformedValue(loc, inputMV,
                                       InputOrigType, InputSubstType,
                                       OutputOrigType, OutputSubstType,
                                       Output->getType().getObjectType(),
                                       SGFContext(outputInit.get()));
    if (!mv.isInContext())
      mv.ensurePlusOne(SGF, loc).forwardInto(SGF, loc, outputInit.get());

    // Disable the cleanup; we've kept our promise to leave the inout
    // initialized.
    outputInit->getManagedAddress().forward(SGF);
  }

  void dump(SILGenFunction &SGF) const override {
    llvm::errs() << "TranslateIndirect("
      << InputOrigType << ", " << InputSubstType << ", "
      << OutputOrigType << ", " << OutputSubstType << ", "
      << Output << ", " << Input << ")\n";
  }
};

template <class Expander>
class OuterPackArgGenerator {
  Expander &TheExpander;
public:
  using reference = ManagedValue;

  OuterPackArgGenerator(Expander &expander) : TheExpander(expander) {}

  reference claimNext() {
    return TheExpander.claimNextOuterPackArg();
  }
  SILArgumentConvention getCurrentConvention() {
    return SILArgumentConvention(TheExpander.getOuterPackConvention());
  }
  void finishCurrent(ManagedValue packAddr) {
    // Ignore: we don't care about tracking *outer* pack cleanups
  }
};

/// A CRTP helper class for classes that supervise a translation between
/// inner and outer signatures.
template <class Impl, class InnerSlotType>
class ExpanderBase {
protected:
  Impl &asImpl() { return static_cast<Impl&>(*this); }

  SILGenFunction &SGF;
  SILLocation Loc;
  ArrayRefGenerator<ArrayRef<ManagedValue>> OuterArgs;
  OuterPackArgGenerator<Impl> OuterPackArgs;

public:
  ExpanderBase(SILGenFunction &SGF, SILLocation loc,
               ArrayRef<ManagedValue> outerArgs)
    : SGF(SGF), Loc(loc), OuterArgs(outerArgs), OuterPackArgs(asImpl()) {}

  void expand(AbstractionPattern innerOrigType,
              CanType innerSubstType,
              AbstractionPattern outerOrigType,
              CanType outerSubstType);

  void expandInnerVanishingTuple(AbstractionPattern innerOrigType,
                                 CanType innerSubstType,
    llvm::function_ref<void(AbstractionPattern innerOrigEltType,
                            CanType innerSubstEltType)> handleSingle,
    llvm::function_ref<ManagedValue(AbstractionPattern innerOrigEltType,
                                    CanType innerSubstEltType,
                                    SILType innerEltTy)> handlePackElement);

  void expandOuterVanishingTuple(AbstractionPattern outerOrigType,
                                 CanType outerSubstType,
    llvm::function_ref<void(AbstractionPattern outerOrigEltType,
                            CanType outerSubstEltType)> handleSingle,
    llvm::function_ref<void(AbstractionPattern outerOrigEltType,
                            CanType outerSubstEltType,
                            ManagedValue outerEltAddr)> handlePackElement);

  void expandParallelTuples(AbstractionPattern innerOrigType,
                            CanType innerSubstType,
                            AbstractionPattern outerOrigType,
                            CanType outerSubstType);

  ManagedValue
  expandParallelTuplesInnerIndirect(AbstractionPattern innerOrigType,
                                    CanTupleType innerSubstType,
                                    AbstractionPattern outerOrigType,
                                    CanTupleType outerSubstType,
                                    InnerSlotType innerAddr);
  void expandParallelTuplesOuterIndirect(AbstractionPattern innerOrigType,
                                         CanTupleType innerSubstType,
                                         AbstractionPattern outerOrigType,
                                         CanTupleType outerSubstType,
                                         ManagedValue outerAddr);

  ManagedValue expandInnerIndirect(AbstractionPattern innerOrigType,
                                   CanType innerSubstType,
                                   AbstractionPattern outerOrigType,
                                   CanType outerSubstType,
                                   InnerSlotType innerSlot);

  void expandOuterIndirect(AbstractionPattern innerOrigType,
                           CanType innerSubstType,
                           AbstractionPattern outerOrigType,
                           CanType outerSubstType,
                           ManagedValue outerAddr);
};

class ParamInfo {
  IndirectSlot slot;
  ParameterConvention convention;

  bool temporaryShouldBorrow(SILGenFunction &SGF, bool forceAllocation) {
    if (slot.hasAddress() && !forceAllocation) {
      // An address has already been allocated via some projection.  It is not
      // currently supported to store_borrow to such projections.
      return false;
    }
    auto &tl = getTypeLowering(SGF);
    if (tl.isAddressOnly() && SGF.silConv.useLoweredAddresses()) {
      // In address-lowered mode, address-only types can't be loaded in the
      // first place before being store_borrow'd.
      return false;
    }
    if (convention != ParameterConvention::Indirect_In_Guaranteed) {
      // Can only store_borrow into a temporary allocation for @in_guaranteed.
      return false;
    }
    if (tl.isTrivial()) {
      // Can't store_borrow a trivial type.
      return false;
    }
    return true;
  }

  TypeLowering const &getTypeLowering(SILGenFunction &SGF) {
    return SGF.getTypeLowering(getType());
  }

public:
  ParamInfo(IndirectSlot slot, ParameterConvention convention)
    : slot(slot), convention(convention) {}

  SILValue allocate(SILGenFunction &SGF, SILLocation loc) const {
    return slot.allocate(SGF, loc);
  }

  std::unique_ptr<AnyTemporaryInitialization>
  allocateForInitialization(SILGenFunction &SGF, SILLocation loc,
                            bool forceAllocation = false) {
    auto &lowering = getTypeLowering(SGF);
    SILValue address;
    if (forceAllocation) {
      address = SGF.emitTemporaryAllocation(loc, lowering.getLoweredType());
    } else {
      address = allocate(SGF, loc);
    }
    if (address->getType().isMoveOnly())
      address = SGF.B.createMarkUnresolvedNonCopyableValueInst(
          loc, address,
          MarkUnresolvedNonCopyableValueInst::CheckKind::
              ConsumableAndAssignable);
    if (temporaryShouldBorrow(SGF, forceAllocation)) {
      return std::make_unique<StoreBorrowInitialization>(address);
    }
    auto innerTemp = SGF.useBufferAsTemporary(address, lowering);
    return innerTemp;
  }

  SILType getType() const {
    return slot.getType();
  }

  ParameterConvention getConvention() const {
    return convention;
  }

  /// Are we expected to generate into a fixed address?
  bool hasAddress() const {
    return slot.hasAddress();
  }

  /// Return the fixed address we're expected to generate into.
  SILValue getAddress() const {
    return slot.getAddress();
  }

  /// Are we expected to produce an address?
  bool shouldProduceAddress(SILGenFunction &SGF) const {
    return hasAddress() ||
           (isIndirectFormalParameter(convention) &&
            SGF.silConv.useLoweredAddresses());
  }
};

/// Given a list of inputs that are suited to the parameters of one
/// function, translate them into a list of outputs that are suited
/// for the parameters of another function, given that the two
/// functions differ only by abstraction differences and/or a small
/// a small set of subtyping-esque conversions tolerated by the
/// type checker.
///
/// This is a conceptually rich transformation, and there are several
/// different concepts of expansion and transformation going on at
/// once here.  We will briefly review these concepts in order to
/// explain what has to happen here.
///
/// Swift functions have *formal* parameters.  These are represented here
/// as the list of input and and output `CanParam` structures.  The
/// type-checker requires function types to be formally related to each
/// in specific ways in order for a conversion to be accepted; this
/// includes the parameter lists being the same length (other than
/// the exception of SE-0110's tuple-splat behavior).
///
/// SIL functions have *lowered* parameters.  These correspond to the
/// SIL values we receive in OuterArgs and must generate in InnerArgs.
///
/// A single formal parameter can correspond to any number of
/// lowered parameters because SIL function type lowering recursively
/// expands tuples that aren't being passed inout.
///
/// The lowering of generic Swift function types must be independent
/// of the actual types substituted for generic parameters, which means
/// that decisions about when to expand must be made according to the
/// orig (unsubstituted) function type, not the substituted types.
/// But the type checker only cares that function types are related
/// as substituted types, so we may need to combine or expand tuples
/// because of the differences between abstraction.
///
/// This translation therefore recursively walks the orig parameter
/// types of the input and output function type and expects the
/// corresponding lowered parameter lists to match with the structure
/// we see there.  When the walk reaches a non-tuple orig type on the
/// input side, we know that the input function receives a lowered
/// parameter and therefore there is a corresponding value in OuterArgs.
/// When the walk reaches a non-tuple orig type on the output side,
/// we know that the output function receives a lowered parameter
/// and therefore there is a corresponding type in OutputTypes (and
/// a need to produce an argument value in InnerArgs).
///
/// Variadic generics complicate this because both tuple element
/// lists and function formal parameter lists can contain pack
/// expansions.  Again, the relevant question is where expansions
/// appear in the orig type; the substituted parameters / tuple
/// elements are still required to be related by the type-checker.
/// Like any other non-tuple pattern, an expansion always corresponds
/// to a single lowered parameter, which may then include multiple
/// formal parameters or tuple elements from the substituted type's
/// perspective.  The orig type should carry precise substitutions
/// that will tell us how many components the expansion expands to,
/// which we must use to inform our recursive walk.  These components
/// may or may not still contain pack expansions in the substituted
/// types; if they do, they will have the same shape.
///
/// An example might help.  Suppose that we have a generic function
/// that returns a function value:
///
///   func produceFunction<T_0, each T_1>() ->
///     (Optional<T_0>, (B, C), repeat Array<each T_1>) -> ()
///
/// And suppose we call this with generic arguments <A, Pack{D, E}>.
/// Then formally we now have a function of type:
///
///     (Optional<A>, (B, C), Array<D>, Array<E>) -> ()
///
/// These are the orig and subst formal parameter sequences.  The
/// lowered parameter sequence of this type (assuming all the concrete
/// types A,B,... are non-trivial but loadable) is:
///
///     (@in_guaranteed Optional<A>,
///      @guaranteed B,
///      @guaranteed C,
///      @pack_guaranteed Pack{Array<D>, Array<E>})
///
/// Just for edification, if we had written this function type in a
/// non-generic context, it would have this lowered parameter sequence:
///
///     (@guaranteed Optional<A>,
///      @guaranteed B,
///      @guaranteed C,
///      @guaranteed Array<D>,
///      @guaranteed Array<E>)
///
/// Now suppose we also have a function that takes a function value:
///
///   func consumeFunction<T_out_0, each T_out_1>(
///      _ function: (A, T_out_0, repeat each T_out_1, Array<E>) -> ()
///   )
///
/// If we call this with the generic arguments <(B, C), Pack{Array<D>}>,
/// then it will expect a value of type:
///
///     (A, (B, C), Array<D>, Array<E>) -> ()
///
/// This is a supertype of the function type above (because of the
/// contravariance of function parameters), and so the type-checker will
/// permit the result of one call to be passed to the other.  The
/// lowered parameter sequence of this type will be:
///
///     (@guaranteed A,
///      @in_guaranteed (B, C),
///      @in_guaranteed Array<D>,
///      @guaranteeed Array<E>)
///
/// We will then end up in this code with the second type as the input
/// type and the first type as the output type.  (The second type is
/// the input because of contravariance again: we do this conversion
/// by capturing a value of the first function type in a closure which
/// presents the interface of the second function type.  In this code,
/// we are emitting the body of that closure, and therefore the inputs
/// we receive are the parameters of that closure, matching the lowered
/// signature of the second function type.)
class TranslateArguments : public ExpanderBase<TranslateArguments, ParamInfo> {
  class InnerPackArgGenerator {
    TranslateArguments &translator;
  public:
    InnerPackArgGenerator(TranslateArguments &translator)
      : translator(translator) {}

    ManagedValue claimNext() {
      return translator.createInnerIndirectPackArg();
    }
    SILArgumentConvention getCurrentConvention() {
      return SILArgumentConvention(translator.getInnerPackConvention());
    }
    void finishCurrent(ManagedValue packAddr) {
      translator.finishInnerIndirectPackArg(packAddr);
    }
  };

  struct IndirectTupleExpansionCombiner {
    SmallVector<CleanupHandle, 4> eltCleanups;
    TranslateArguments &translator;

    IndirectTupleExpansionCombiner(TranslateArguments &translator)
      : translator(translator) {}

    ParamInfo getElementSlot(SILValue eltAddr) {
      return ParamInfo(eltAddr, ParameterConvention::Indirect_In);
    }
    void collectElement(ManagedValue eltAddr) {
      if (eltAddr.hasCleanup())
        eltCleanups.push_back(eltAddr.getCleanup());
    }
    ManagedValue finish(SILValue tupleAddr, ParamInfo tupleSlot) {
      // We generated an owned tuple, so forward all the element cleanups
      // and enter a single cleanup for the entire tuple.
      for (auto cleanup : eltCleanups) {
        translator.SGF.Cleanups.forwardCleanup(cleanup);
      }
      auto tupleMV = translator.SGF.emitManagedBufferWithCleanup(tupleAddr);

      // We then may need to borrow that.
      return translator.maybeBorrowTemporary(tupleMV, tupleSlot);
    }
  };

  SmallVectorImpl<ManagedValue> &InnerArgs;
  CanSILFunctionType InnerTypesFuncTy;
  ArrayRef<SILParameterInfo> InnerTypes;
  InnerPackArgGenerator InnerPacks;
public:
  TranslateArguments(SILGenFunction &SGF, SILLocation loc,
                     ArrayRef<ManagedValue> outerArgs,
                     SmallVectorImpl<ManagedValue> &innerArgs,
                     CanSILFunctionType innerTypesFuncTy,
                     ArrayRef<SILParameterInfo> innerTypes)
    : ExpanderBase(SGF, loc, outerArgs), InnerArgs(innerArgs),
      InnerTypesFuncTy(innerTypesFuncTy), InnerTypes(innerTypes),
      InnerPacks(*this) {}

  void process(AbstractionPattern innerOrigFunctionType,
               AnyFunctionType::CanParamArrayRef innerSubstTypes,
               AbstractionPattern outerOrigFunctionType,
               AnyFunctionType::CanParamArrayRef outerSubstTypes,
               bool ignoreFinalOuterOrigParam = false) {
    if (outerSubstTypes.size() == 1 &&
        innerSubstTypes.size() != 1) {
      // SE-0110 tuple splat. Turn the inner into a single value of tuple
      // type, and process.
      auto outerOrigType = outerOrigFunctionType.getFunctionParamType(0);
      auto outerSubstType = outerSubstTypes[0].getPlainType();

      // Build an abstraction pattern for the inner.
      SmallVector<AbstractionPattern, 8> innerOrigTypes;
      for (unsigned i = 0, e = innerOrigFunctionType.getNumFunctionParams();
           i < e; ++i) {
        innerOrigTypes.push_back(
          innerOrigFunctionType.getFunctionParamType(i));
      }
      auto innerOrigType = AbstractionPattern::getTuple(innerOrigTypes);

      // Build the substituted inner tuple type. Note that we deliberately
      // don't use composeTuple() because we want to drop ownership
      // qualifiers.
      SmallVector<TupleTypeElt, 8> elts;
      for (auto param : innerSubstTypes) {
        assert(!param.isVariadic());
        assert(!param.isInOut());
        elts.emplace_back(param.getParameterType());
      }
      auto innerSubstType = CanTupleType(
        TupleType::get(elts, SGF.getASTContext()));

      // Translate the outer tuple value into the inner tuple value. Note
      // that the inner abstraction pattern is a tuple, and we explode tuples
      // into multiple parameters, so if the outer abstraction pattern is
      // opaque, this will explode the outer value. Otherwise, the outer
      // parameters will be mapped one-to-one to the inner parameters.
      expand(innerOrigType, innerSubstType,
             outerOrigType, outerSubstType);
      OuterArgs.finish();
      return;
    }

    // Otherwise, formal parameters are always reabstracted one-to-one,
    // at least in substituted types.  (They can differ in the orig types
    // because of pack expansions in the parameters.)  As a result, if we
    // generate the substituted parameters for both the inner and
    // outer function types, and we pull a substituted formal parameter
    // off of one whenever we pull a substituted formal parameter off the
    // other, we should end up exhausting both sequences.
    assert(outerSubstTypes.size() == innerSubstTypes.size());

    // It's more straightforward for generating packs if we allow
    // ourselves to be driven by the inner sequence.  This requires us
    // to invert control for the outer sequence, pulling off one
    // component at a time while looping over the inner sequence.

    FunctionInputGenerator outerParams(SGF.getASTContext(), OuterArgs,
                                       outerOrigFunctionType,
                                       outerSubstTypes,
                                       ignoreFinalOuterOrigParam);

    FunctionParamGenerator innerParams(innerOrigFunctionType,
                                       innerSubstTypes,
                                       /*drop self*/ false);

    // Loop over the *orig* formal parameters.  We'll pull off
    // *substituted* formal parameters in parallel for both outers
    // and inners.
    for (; !innerParams.isFinished(); innerParams.advance()) {
      // If we have a pack expansion formal parameter in the orig
      // inner type, it corresponds to N formal parameters in the
      // substituted inner type.  This will pull off N substituted
      // formal parameters from the outer type.
      if (innerParams.isOrigPackExpansion()) {
        auto innerPackParam = claimNextInnerParam();
        auto innerPackArg =
          expandPackInnerParam(innerParams.getOrigType(),
                               innerParams.getSubstParams(),
                               innerPackParam,
                               outerParams);
        InnerArgs.push_back(innerPackArg);

      // Otherwise, we have a single, non-pack formal parameter
      // in the inner type.  This will pull off a single substiuted
      // formal parameter from outerParams.
      } else {
        expandOuterSingleInnerParam(innerParams.getOrigType(),
                                    innerParams.getSubstParams()[0],
                                    outerParams);
      }
    }
    innerParams.finish();
    outerParams.finish();

    OuterArgs.finish();
  }

  /// This is used for translating yields.
  void process(ArrayRef<AbstractionPattern> innerOrigTypes,
               AnyFunctionType::CanParamArrayRef innerSubstTypes,
               ArrayRef<AbstractionPattern> outerOrigTypes,
               AnyFunctionType::CanParamArrayRef outerSubstTypes) {
    assert(outerOrigTypes.size() == outerSubstTypes.size());
    assert(innerOrigTypes.size() == innerSubstTypes.size());
    assert(outerOrigTypes.size() == innerOrigTypes.size());

    for (auto i : indices(outerOrigTypes)) {
      expandParam(innerOrigTypes[i], innerSubstTypes[i],
                  outerOrigTypes[i], outerSubstTypes[i]);
    }

    OuterArgs.finish();
  }

private:
  friend ExpanderBase;
  using ExpanderBase::expand;

  void expandParam(AbstractionPattern innerOrigType,
                   AnyFunctionType::CanParam innerSubstType,
                   AbstractionPattern outerOrigType,
                   AnyFunctionType::CanParam outerSubstType) {
    // Note that it's OK for the outer to be inout but not the inner;
    // this means we're just going to load the inout and pass it on as a
    // scalar.
    if (innerSubstType.isInOut()) {
      assert(outerSubstType.isInOut());
      auto outerValue = claimNextOuterArg();
      auto innerParam = claimNextInnerParam();

      ManagedValue inner =
        processInOut(innerOrigType, innerSubstType.getParameterType(),
                     outerOrigType, outerSubstType.getParameterType(),
                     outerValue, innerParam);
      InnerArgs.push_back(inner);
    } else {
      expand(innerOrigType, innerSubstType.getParameterType(),
             outerOrigType, outerSubstType.getParameterType());
    }
  }

  void expandOuterSingleInnerParam(AbstractionPattern innerOrigType,
                            AnyFunctionType::CanParam innerSubstParam,
                            FunctionInputGenerator &outerParam);

  ManagedValue expandPackExpansion(
                      AbstractionPattern innerOrigType,
                      CanPackExpansionType innerSubstType,
                      AbstractionPattern outerOrigType,
                      CanPackExpansionType outerSubstType,
                      CanPackType innerFormalPackType,
                      ParamInfo innerTupleOrPackSlot,
                      unsigned innerComponentIndex,
                      CanPackType outerFormalPackType,
                      ManagedValue outerTupleOrPackAddr,
                      unsigned outerComponentIndex);

  ManagedValue expandPackInnerParam(
                       AbstractionPattern innerOrigExpansionType,
                       AnyFunctionType::CanParamArrayRef innerSubstParams,
                       ParamInfo innerParam,
                       FunctionInputGenerator &outerParams);

  ManagedValue expandSingleInnerIndirect(AbstractionPattern innerOrigType,
                                         CanType innerSubstType,
                                         AbstractionPattern outerOrigType,
                                         CanType outerSubstType,
                                         ParamInfo innerParam) {
    assert(!outerOrigType.isTuple());
    auto outerArg = claimNextOuterArg();
    return processSingle(innerOrigType, innerSubstType,
                         outerOrigType, outerSubstType,
                         outerArg, innerParam);
  }

  void expandOuterTuple(AbstractionPattern innerOrigType,
                        CanType innerSubstType,
                        AbstractionPattern outerOrigType,
                        CanTupleType outerSubstType) {
    assert(!innerOrigType.isTuple());
    assert(outerOrigType.isTuple());
    assert(!outerOrigType.doesTupleVanish());

    auto innerParam = claimNextInnerParam();
    auto innerArg = expandOuterTupleInnerSingle(innerOrigType, innerSubstType,
                                                outerOrigType, outerSubstType,
                                                innerParam);
    InnerArgs.push_back(innerArg);
  }

  ManagedValue expandOuterTupleInnerIndirect(AbstractionPattern innerOrigType,
                                             CanType innerSubstType,
                                             AbstractionPattern outerOrigType,
                                             CanTupleType outerSubstType,
                                             ParamInfo innerParam) {
    return expandOuterTupleInnerSingle(innerOrigType, innerSubstType,
                                       outerOrigType, outerSubstType,
                                       innerParam);
  }

  ManagedValue expandOuterTupleInnerSingle(AbstractionPattern innerOrigType,
                                           CanType innerSubstType,
                                           AbstractionPattern outerOrigType,
                                           CanTupleType outerSubstType,
                                           ParamInfo innerParam) {
    assert(outerOrigType.isTuple());
    assert(!outerOrigType.doesTupleVanish());

    // Tuple types can be subtypes of optionals.
    if (innerSubstType.getOptionalObjectType()) {
      return expandOuterTupleInnerSingleOptional(innerOrigType,
                                                 innerSubstType,
                                                 outerOrigType,
                                                 outerSubstType,
                                                 innerParam);
    }

    // Tuple types can be subtypes of existentials.
    if (innerSubstType->isExistentialType()) {
      return expandOuterTupleInnerSingleExistential(innerOrigType,
                                                    innerSubstType,
                                                    outerOrigType,
                                                    outerSubstType,
                                                    innerParam);
    }

    // Otherwise, the inner type had better be a tuple.
    auto innerTupleType = cast<TupleType>(innerSubstType);
    if (innerParam.shouldProduceAddress(SGF)) {
      return expandParallelTuplesInnerIndirect(innerOrigType, innerTupleType,
                                               outerOrigType, outerSubstType,
                                               innerParam);
    } else {
      auto innerArg =
        expandParallelTuplesInnerDirect(innerOrigType, innerTupleType,
                                        outerOrigType, outerSubstType,
                                        innerParam.getType());
      return maybeBorrowTemporary(innerArg, innerParam);
    }
  }

  void expandSingleOuterParam(AbstractionPattern innerOrigType,
                              AnyFunctionType::CanParam innerSubstParam,
                              AbstractionPattern outerOrigType,
                              AnyFunctionType::CanParam outerSubstParam,
                              ManagedValue outerArg) {
    CanType outerSubstType = outerSubstParam.getParameterType();
    CanType innerSubstType = innerSubstParam.getParameterType();

    if (innerSubstParam.isInOut()) {
      assert(outerSubstParam.isInOut());
      auto innerLoweredTy = claimNextInnerParam();

      ManagedValue innerArg = processInOut(innerOrigType, innerSubstType,
                                           outerOrigType, outerSubstType,
                                           outerArg, innerLoweredTy);
      InnerArgs.push_back(innerArg);
    } else {
      expandSingleOuter(innerOrigType, innerSubstType,
                        outerOrigType, outerSubstType,
                        outerArg);
    }
  }

  void expandSingleOuterIndirect(AbstractionPattern innerOrigType,
                                 CanType innerSubstType,
                                 AbstractionPattern outerOrigType,
                                 CanType outerSubstType,
                                 ManagedValue outerResultAddr) {
    expandSingleOuter(innerOrigType, innerSubstType,
                      outerOrigType, outerSubstType,
                      outerResultAddr);
  }

  void expandSingleOuter(AbstractionPattern innerOrigType,
                         CanType innerSubstType,
                         AbstractionPattern outerOrigType,
                         CanType outerSubstType,
                         ManagedValue outerArg) {
    if (!innerOrigType.isTuple()) {
      auto innerParam = claimNextInnerParam();
      ManagedValue innerArg = processSingle(innerOrigType, innerSubstType,
                                            outerOrigType, outerSubstType,
                                            outerArg, innerParam);
      InnerArgs.push_back(innerArg);
    } else if (innerOrigType.doesTupleVanish()) {
      expandSingleOuterInnerVanishing(outerOrigType,
                                      outerSubstType,
                                      innerOrigType,
                                      innerSubstType,
                                      outerArg);
    } else {
      expandSingleOuterInnerTuple(outerOrigType,
                                  cast<TupleType>(outerSubstType),
                                  innerOrigType,
                                  cast<TupleType>(innerSubstType),
                                  outerArg);
    }
  }

  void expandSingleOuterInnerVanishing(AbstractionPattern innerOrigType,
                                       CanType innerSubstType,
                                       AbstractionPattern outerOrigType,
                                       CanType outerSubstType,
                                       ManagedValue outerArg) {
    assert(innerOrigType.isTuple());
    assert(innerOrigType.doesTupleVanish());

    expandInnerVanishingTuple(innerOrigType, innerSubstType,
       [&](AbstractionPattern innerOrigEltType, CanType innerSubstEltType) {
      expandSingleOuter(innerOrigEltType, innerSubstEltType,
                        outerOrigType, outerSubstType,
                        outerArg);
    }, [&](AbstractionPattern innerOrigEltType, CanType innerSubstEltType,
           SILType innerEltTy) {
      auto innerParam = getInnerPackElementSlot(innerEltTy);
      return processSingle(innerOrigEltType, innerSubstEltType,
                           outerOrigType, outerSubstType,
                           outerArg, innerParam);
    });
  }

  /// Take a tuple that has been expanded in the outer and turn it into
  /// a scalar tuple value in the inner.
  ManagedValue
  expandParallelTuplesInnerDirect(AbstractionPattern innerOrigType,
                                  CanTupleType innerSubstType,
                                  AbstractionPattern outerOrigType,
                                  CanTupleType outerSubstType,
                                  SILType loweredInnerTy) {
    assert(outerOrigType.isTuple());
    assert(!outerOrigType.doesTupleVanish());

    // We have to use an indirect pattern if the substituted types contain
    // pack expansions.
    if (innerSubstType.containsPackExpansionType()) {
      auto innerTupleBuffer = SGF.emitTemporaryAllocation(Loc, loweredInnerTy);
      ParamInfo innerSlot(innerTupleBuffer, ParameterConvention::Indirect_In);
      auto innerTupleAddr =
        expandParallelTuplesInnerIndirect(innerOrigType, innerSubstType,
                                          outerOrigType, outerSubstType,
                                          innerSlot);
      return SGF.B.createLoadTake(Loc, innerTupleAddr);
    }

    // Otherwise, expand the outer tuple in parallel with the elements of
    // the inner tuple, generate the inner elements, and then form them into
    // a scalar tuple.
    assert(!outerSubstType.containsPackExpansionType());

    SmallVector<ManagedValue, 4> innerEltMVs;
    TupleSubstElementGenerator innerElt(SGF.getASTContext(),
                                        innerOrigType, innerSubstType);
    ExpandedTupleInputGenerator outerElt(SGF.getASTContext(), OuterPackArgs,
                                         outerOrigType, outerSubstType);

    for (; !innerElt.isFinished(); innerElt.advance(), outerElt.advance()) {
      assert(!outerElt.isFinished() && "elements not parallel");
      assert(!outerElt.isSubstPackExpansion() && !innerElt.isSubstPackExpansion());
      AbstractionPattern outerOrigEltType = outerElt.getOrigType();
      AbstractionPattern innerOrigEltType = innerElt.getOrigType();
      CanType outerSubstEltType = outerElt.getSubstType();
      CanType innerSubstEltType = innerElt.getSubstType();
      SILType loweredInnerEltTy =
        loweredInnerTy.getTupleElementType(innerElt.getSubstElementIndex());
      ParamInfo innerEltSlot =
        getInnerParamInfo(loweredInnerEltTy, ParameterConvention::Direct_Owned);

      ManagedValue innerEltMV = expandInnerSingle(innerOrigEltType,
                                                  innerSubstEltType,
                                                  outerOrigEltType,
                                                  outerSubstEltType,
                                                  innerEltSlot);
      innerEltMVs.push_back(innerEltMV);
    }

    assert(outerElt.isFinished() && "elements not parallel");
    outerElt.finish();
    innerElt.finish();

    SmallVector<SILValue, 4> innerEltValues;
    for (auto &elt : innerEltMVs)
      innerEltValues.push_back(elt.forward(SGF));

    auto tuple = SGF.B.createTuple(Loc, loweredInnerTy, innerEltValues);
    if (tuple->getOwnershipKind() == OwnershipKind::Owned)
      return SGF.emitManagedRValueWithCleanup(tuple);
    if (tuple->getType().isTrivial(SGF.F))
      return ManagedValue::forRValueWithoutOwnership(tuple);
    return ManagedValue::forBorrowedRValue(tuple);
  }

  /// Handle a tuple that has been exploded in the outer but wrapped in
  /// an optional in the inner.
  ManagedValue
  expandOuterTupleInnerSingleOptional(AbstractionPattern innerOrigType,
                                      CanType innerSubstType,
                                      AbstractionPattern outerOrigType,
                                      CanTupleType outerSubstType,
                                      ParamInfo innerParam) {
    auto innerOptionalTy = innerParam.getType();
    auto innerObjectTy = innerOptionalTy.getOptionalObjectType();
    auto someDecl = SGF.getASTContext().getOptionalSomeDecl();

    // Use the scalar pattern unless we need to emit into a slot.
    bool produceAddress = innerParam.shouldProduceAddress(SGF);
    if (!produceAddress) {
      // 'enum' can construct payloads of borrowed values, at least in
      // some cases, but let's not try to rely on that here.
      ParamInfo innerObjectParam =
        getInnerParamInfo(innerObjectTy, ParameterConvention::Direct_Owned);
      auto payload =
        expandOuterTupleInnerSingle(innerOrigType.getOptionalObjectType(),
                                    innerSubstType.getOptionalObjectType(),
                                    outerOrigType, outerSubstType,
                                    innerObjectParam);
      auto innerOptionalMV =
        SGF.B.createEnum(Loc, payload, someDecl, innerOptionalTy);
      return maybeBorrowTemporary(innerOptionalMV, innerParam);
    }

    // Otherwise, set up the optional object slot.
    auto innerOptionalAddr = innerParam.allocate(SGF, Loc);
    auto innerObjectAddr =
      SGF.B.createInitEnumDataAddr(Loc, innerOptionalAddr, someDecl,
                                   innerObjectTy);
    ParamInfo innerObjectParam(IndirectSlot(innerObjectAddr),
                               ParameterConvention::Indirect_In);

    // Recurse to fill in the optional object.  We always produce an owned
    // value here.
    ManagedValue innerPayload =
      expandOuterTupleInnerSingle(innerOrigType.getOptionalObjectType(),
                                  innerSubstType.getOptionalObjectType(),
                                  outerOrigType, outerSubstType,
                                  innerObjectParam);

    // Finish the optional and take ownership of it.
    SGF.B.createInjectEnumAddr(Loc, innerOptionalAddr, someDecl);

    innerPayload.forward(SGF);
    ManagedValue innerOptionalMV =
      SGF.emitManagedBufferWithCleanup(innerOptionalAddr);

    // Return a value with the right ownership.
    return maybeBorrowTemporary(innerOptionalMV, innerParam);
  }

  /// Handle a tuple that has been exploded in the outer but wrapped
  /// in an existential in the inner.
  ManagedValue
  expandOuterTupleInnerSingleExistential(AbstractionPattern innerOrigType,
                                         CanType innerSubstType,
                                         AbstractionPattern outerOrigType,
                                         CanTupleType outerSubstType,
                                         ParamInfo innerAnySlot) {
    auto existentialBuf = innerAnySlot.allocate(SGF, Loc);

    auto opaque = AbstractionPattern::getOpaque();
    auto &concreteTL = SGF.getTypeLowering(opaque, outerSubstType);

    auto conformances = SGF.SGM.M.getSwiftModule()->collectExistentialConformances(
        outerSubstType, innerSubstType);

    auto innerTupleAddr =
      SGF.B.createInitExistentialAddr(Loc, existentialBuf,
                                      outerSubstType,
                                      concreteTL.getLoweredType(),
                                      conformances);
    ParamInfo innerTupleSlot(IndirectSlot(innerTupleAddr),
                             ParameterConvention::Indirect_In);

    ManagedValue innerPayload =
      expandParallelTuplesInnerIndirect(opaque, outerSubstType,
                                        outerOrigType, outerSubstType,
                                        innerTupleSlot);

    ManagedValue anyMV;
    if (SGF.silConv.useLoweredAddresses()) {
      // We always need to return the existential buf with a cleanup even if
      // we stored trivial values, since SILGen maintains the invariant that
      // forwarding a non-trivial value (i.e. an Any) into memory must be done
      // at +1.
      innerPayload.forward(SGF);
      anyMV = SGF.emitManagedBufferWithCleanup(existentialBuf);
    } else {
      // We are under opaque value(s) mode - load the any and init an opaque
      // TODO: just emit the tuple as a scalar
      auto loadedPayload = SGF.B.createLoadCopy(Loc, innerPayload);
      auto &anyTL = SGF.getTypeLowering(opaque, innerSubstType);
      anyMV = SGF.B.createInitExistentialValue(
        Loc, anyTL.getLoweredType(), outerSubstType, loadedPayload,
        conformances);
    }
    return maybeBorrowTemporary(anyMV, innerAnySlot);
  }

  void expandInnerTupleOuterIndirect(AbstractionPattern innerOrigType,
                                     CanTupleType innerSubstType,
                                     AbstractionPattern outerOrigType,
                                     CanType outerSubstType,
                                     ManagedValue outerArg) {
    assert(innerOrigType.isTuple());
    assert(!innerOrigType.doesTupleVanish());

    // The outer subst type must be convertible to the inner subst type,
    // which is a tuple, so the outer subst type must also be a tuple.
    auto outerSubstTupleType = cast<TupleType>(outerSubstType);

    expandSingleOuterInnerTuple(innerOrigType, innerSubstType,
                                outerOrigType, outerSubstTupleType,
                                outerArg);
  }

  void expandInnerTuple(AbstractionPattern innerOrigType,
                        CanTupleType innerSubstType,
                        AbstractionPattern outerOrigType,
                        CanType outerSubstType) {
    assert(!outerOrigType.isTuple());
    assert(innerOrigType.isTuple());
    assert(!innerOrigType.doesTupleVanish());

    // The outer subst type must be convertible to the inner subst type,
    // which is a tuple, so the outer subst type must also be a tuple.
    // But we know here that the outer type doesn't have a tuple pattern,
    // so it must be passed as a single argument.
    auto outerSubstTupleType = cast<TupleType>(outerSubstType);
    auto outerArg = claimNextOuterArg();

    expandSingleOuterInnerTuple(innerOrigType, innerSubstType,
                                outerOrigType, outerSubstTupleType,
                                outerArg);
  }

  /// Given that a tuple value is being passed as an aggregate in
  /// the outer signature, but not the inner signature, expand the
  /// tuple.
  void expandSingleOuterInnerTuple(AbstractionPattern innerOrigType,
                                   CanTupleType innerSubstType,
                                   AbstractionPattern outerOrigType,
                                   CanTupleType outerSubstType,
                                   ManagedValue outerTuple) {
    assert(innerOrigType.isTuple());
    assert(!innerOrigType.doesTupleVanish());

    assert(outerSubstType->getNumElements() ==
           innerSubstType->getNumElements());

    // The tuple can be a scalar when opaque values are enabled.
    // TODO: handle this by breaking apart the tuple directly when
    // it doesn't contain pack expansions.
    if (!outerTuple.getType().isAddress()) {
      outerTuple = outerTuple.materialize(SGF, Loc);
    }

    expandParallelTuplesOuterIndirect(innerOrigType, innerSubstType,
                                      outerOrigType, outerSubstType,
                                      outerTuple);
  }

  ManagedValue expandInnerSingle(AbstractionPattern innerOrigType,
                                 CanType innerSubstType,
                                 AbstractionPattern outerOrigType,
                                 CanType outerSubstType,
                                 ParamInfo innerSlot) {
    if (!outerOrigType.isTuple()) {
      auto outerArg = claimNextOuterArg();
      return processSingle(innerOrigType, innerSubstType,
                           outerOrigType, outerSubstType,
                           outerArg, innerSlot);
    }

    if (!outerOrigType.doesTupleVanish()) {
      return expandOuterTupleInnerSingle(innerOrigType,
                                         innerSubstType,
                                         outerOrigType,
                                         cast<TupleType>(outerSubstType),
                                         innerSlot);
    }

    ManagedValue innerArg;
    expandOuterVanishingTuple(outerOrigType, outerSubstType,
       [&](AbstractionPattern outerOrigEltType, CanType outerSubstEltType) {
      innerArg = expandInnerSingle(innerOrigType,
                                   innerSubstType,
                                   outerOrigEltType,
                                   outerSubstEltType,
                                   innerSlot);
    }, [&](AbstractionPattern outerOrigEltType, CanType outerSubstEltType,
           ManagedValue outerAddr) {
      innerArg = processIndirect(innerOrigType, innerSubstType,
                                 outerOrigEltType, outerSubstEltType,
                                 outerAddr, innerSlot);
    });
    return innerArg;
  }

  // process into a temporary.
  ManagedValue processIndirect(AbstractionPattern innerOrigType,
                               CanType innerSubstType,
                               AbstractionPattern outerOrigType,
                               CanType outerSubstType,
                               ManagedValue outerArg,
                               ParamInfo innerSlot) {
    auto innerResultTy = innerSlot.getType();
    auto innerTemp =
        innerSlot.allocateForInitialization(SGF, Loc, /*forceAllocation=*/true);
    processSingleInto(innerOrigType, innerSubstType,
                      outerOrigType, outerSubstType,
                      outerArg, innerResultTy.getAddressType(),
                      *innerTemp);
    return maybeBorrowTemporary(innerTemp->getManagedAddress(), innerSlot);
  }

  // process into an owned argument.
  ManagedValue processIntoOwned(AbstractionPattern innerOrigType,
                                CanType innerSubstType,
                                AbstractionPattern outerOrigType,
                                CanType outerSubstType,
                                ManagedValue outerArg,
                                SILType innerLoweredTy) {
    auto inner = processPrimitive(innerOrigType, innerSubstType,
                                  outerOrigType, outerSubstType,
                                  outerArg, innerLoweredTy);

    // If our inner is guaranteed or unowned, we need to create a copy here.
    if (inner.getOwnershipKind() != OwnershipKind::Owned)
      inner = inner.copyUnmanaged(SGF, Loc);

    return inner;
  }

  // process into a guaranteed argument.
  ManagedValue processIntoGuaranteed(AbstractionPattern innerOrigType,
                                     CanType innerSubstType,
                                     AbstractionPattern outerOrigType,
                                     CanType outerSubstType,
                                     ManagedValue outer,
                                     SILType innerLoweredTy) {
    auto inner = processPrimitive(innerOrigType, innerSubstType,
                                  outerOrigType, outerSubstType,
                                  outer, innerLoweredTy);

    // If our inner value is not guaranteed, we need to:
    //
    // 1. Unowned - Copy + Borrow.
    // 2. Owned - Borrow.
    // 3. Trivial - do nothing.
    //
    // This means we can first transition unowned => owned and then handle
    // the new owned value using the same code path as values that are
    // initially owned.
    if (inner.getOwnershipKind() == OwnershipKind::Unowned) {
      assert(!inner.hasCleanup());
      inner = SGF.emitManagedCopy(Loc, inner.getValue());
    }

    // If the inner is unowned or owned, create a borrow.
    if (inner.getOwnershipKind() != OwnershipKind::Guaranteed) {
      inner = SGF.emitManagedBeginBorrow(Loc, inner.getValue());
    }

    return inner;
  }

  ManagedValue maybeBorrowTemporary(ManagedValue innerValue, ParamInfo innerSlot) {
    auto convention = innerSlot.getConvention();
    assert(!isPackParameter(convention));
    assert(innerSlot.shouldProduceAddress(SGF) == innerValue.getType().isAddress());
    if (innerValue.hasCleanup() && !isConsumedParameterInCaller(convention)) {
      return innerValue.borrow(SGF, Loc);
    }
    return innerValue;
  }

  void expandSingle(AbstractionPattern innerOrigType,
                    CanType innerSubstType,
                    AbstractionPattern outerOrigType,
                    CanType outerSubstType) {
    auto outerArg = claimNextOuterArg();
    auto innerParam = claimNextInnerParam();
    auto innerArg = processSingle(innerOrigType, innerSubstType,
                                  outerOrigType, outerSubstType,
                                  outerArg, innerParam);
    InnerArgs.push_back(innerArg);
  }

  /// process a single value and add it as an inner.
  ManagedValue processSingle(AbstractionPattern innerOrigType,
                             CanType innerSubstType,
                             AbstractionPattern outerOrigType,
                             CanType outerSubstType,
                             ManagedValue outer,
                             ParamInfo innerParam) {
    if (innerParam.hasAddress()) {
      auto innerTemp = innerParam.allocateForInitialization(SGF, Loc);
      processSingleInto(innerOrigType, innerSubstType,
                        outerOrigType, outerSubstType,
                        outer, innerParam.getType(), *innerTemp);
      return maybeBorrowTemporary(innerTemp->getManagedAddress(), innerParam);
    }

    auto innerTy = innerParam.getType();

    // Easy case: we want to pass exactly this value.
    if (outer.getType() == innerParam.getType()) {
      if (isConsumedParameterInCaller(innerParam.getConvention()) &&
          !outer.isPlusOne(SGF)) {
        outer = outer.copyUnmanaged(SGF, Loc);
      }

      return outer;
    }

    switch (innerParam.getConvention()) {
    // Direct translation is relatively easy.
    case ParameterConvention::Direct_Owned:
    case ParameterConvention::Direct_Unowned:
      return processIntoOwned(innerOrigType, innerSubstType,
                              outerOrigType, outerSubstType,
                              outer, innerTy);
    case ParameterConvention::Direct_Guaranteed:
      return processIntoGuaranteed(innerOrigType, innerSubstType,
                                   outerOrigType, outerSubstType,
                                   outer, innerTy);
    case ParameterConvention::Indirect_In_CXX:
    case ParameterConvention::Indirect_In: {
      if (SGF.silConv.useLoweredAddresses()) {
        return processIndirect(innerOrigType, innerSubstType,
                               outerOrigType, outerSubstType,
                               outer, innerParam);
      }
      return processIntoOwned(innerOrigType, innerSubstType,
                              outerOrigType, outerSubstType,
                              outer, innerTy);
    }
    case ParameterConvention::Indirect_In_Guaranteed: {
      if (SGF.silConv.useLoweredAddresses()) {
        return processIndirect(innerOrigType, innerSubstType,
                               outerOrigType, outerSubstType,
                               outer, innerParam);
      }
      return processIntoGuaranteed(innerOrigType, innerSubstType,
                                   outerOrigType, outerSubstType,
                                   outer, innerTy);
    }
    case ParameterConvention::Pack_Guaranteed:
    case ParameterConvention::Pack_Owned:
      SGF.SGM.diagnose(Loc, diag::not_implemented,
                       "reabstraction of pack values");
      return SGF.emitUndef(innerTy);
    case ParameterConvention::Indirect_Inout:
    case ParameterConvention::Pack_Inout:
      llvm_unreachable("inout reabstraction handled elsewhere");
    case ParameterConvention::Indirect_InoutAliasable:
      llvm_unreachable("abstraction difference in aliasable argument not "
                       "allowed");
    }

    llvm_unreachable("Covered switch isn't covered?!");
  }

  ManagedValue processInOut(AbstractionPattern innerOrigType,
                            CanType innerSubstType,
                            AbstractionPattern outerOrigType,
                            CanType outerSubstType,
                            ManagedValue outer,
                            ParamInfo innerParam) {
    auto resultTy = innerParam.getType();
    assert(outer.isLValue());
    if (outer.getType() == resultTy) {
      return outer;
    }

    // Create a temporary of the right type.
    auto &temporaryTL = SGF.getTypeLowering(resultTy);
    auto temporary = SGF.emitTemporary(Loc, temporaryTL);

    // Take ownership of the outer value.  This leaves the outer l-value
    // effectively uninitialized, but we'll push a cleanup that will put
    // a value back into it.
    FullExpr scope(SGF.Cleanups, CleanupLocation(Loc));
    auto ownedOuter =
      SGF.emitManagedBufferWithCleanup(outer.getLValueAddress());

    // process the outer value into the temporary.
    processSingleInto(innerOrigType, innerSubstType,
                      outerOrigType, outerSubstType,
                      ownedOuter, resultTy, *temporary);

    // Forward the cleanup on the temporary.  We're about to push a new
    // cleanup that will re-assert ownership of this value.
    auto temporaryAddr = temporary->getManagedAddress().forward(SGF);

    // Leave the scope in which we did the forward translation.  This
    // ensures that the value in the outer buffer is destroyed
    // immediately rather than (potentially) arbitrarily later
    // at a point where we want to put new values in the outer buffer.
    scope.pop();

    // Push the cleanup to perform the reverse translation.  This cleanup
    // asserts ownership of the value of the temporary.
    SGF.Cleanups.pushCleanup<TranslateIndirect>(innerOrigType,
                                                innerSubstType,
                                                outerOrigType,
                                                outerSubstType,
                                                temporaryAddr,
                                                outer.getLValueAddress());

    // Use the temporary as the argument.
    return ManagedValue::forLValue(temporaryAddr);
  }

  ManagedValue
  expandSingleIndirect(AbstractionPattern innerOrigType,
                       CanType innerSubstType,
                       AbstractionPattern outerOrigType,
                       CanType outerSubstType,
                       ParamInfo innerSlot,
                       ManagedValue outerArg) {
    auto innerInit = innerSlot.allocateForInitialization(SGF, Loc);
    processSingleInto(innerOrigType, innerSubstType,
                      outerOrigType, outerSubstType,
                      outerArg, innerSlot.getType(), *innerInit);
    return maybeBorrowTemporary(innerInit->getManagedAddress(), innerSlot);
  }

  /// process a single value and initialize the given temporary with it.
  void processSingleInto(AbstractionPattern innerOrigType,
                         CanType innerSubstType,
                         AbstractionPattern outerOrigType,
                         CanType outerSubstType,
                         ManagedValue outer,
                         SILType innerTy,
                         Initialization &init) {
    assert(innerTy.isAddress());
    auto innerArg = processPrimitive(innerOrigType, innerSubstType,
                                     outerOrigType, outerSubstType,
                                     outer, innerTy,
                                     SGFContext(&init));
    if (!innerArg.isInContext()) {
      if (innerArg.isPlusOneOrTrivial(SGF) || init.isBorrow()) {
        innerArg.forwardInto(SGF, Loc, &init);
      } else {
        innerArg.copyInto(SGF, Loc, &init);
      }
    }
  }

  /// Apply primitive translation to the given value.
  ManagedValue processPrimitive(AbstractionPattern innerOrigType,
                                CanType innerSubstType,
                                AbstractionPattern outerOrigType,
                                CanType outerSubstType,
                                ManagedValue outer,
                                SILType loweredinnerTy,
                                SGFContext context = SGFContext()) {
    return SGF.emitTransformedValue(Loc, outer,
                                    outerOrigType, outerSubstType,
                                    innerOrigType, innerSubstType,
                                    loweredinnerTy, context);
  }

  ManagedValue claimNextOuterArg() {
    return OuterArgs.claimNext();
  }

  friend OuterPackArgGenerator<TranslateArguments>;
  ManagedValue claimNextOuterPackArg() {
    return claimNextOuterArg();
  }
  ParameterConvention getOuterPackConvention() {
    llvm_unreachable("don't have this information");
  }

  /// Claim the next lowered parameter in the inner.  The conventions in
  /// this class are set up such that the place that claims an inner type
  /// is also responsible for adding the inner to inners.  This allows
  /// readers to easily verify that this is done on all paths.  (It'd
  /// sure be nice if we had better language mode for that, though.)
  ParamInfo claimNextInnerParam() {
    return getInnerParamInfo(claimNext(InnerTypes));
  }

  ParamInfo getInnerParamInfo(SILParameterInfo innerParam) {
    auto innerTy = SGF.getSILType(innerParam, InnerTypesFuncTy);
    return ParamInfo(IndirectSlot(innerTy), innerParam.getConvention());
  }

  ParamInfo getInnerParamInfo(SILType paramTy, ParameterConvention convention) {
    return getInnerParamInfo(SILParameterInfo(paramTy.getASTType(), convention));
  }

  PackGeneratorRef getInnerPackGenerator() {
    return InnerPacks;
  }

  ManagedValue createInnerIndirectPackArg() {
    auto innerPackParam = InnerTypes.front();
    assert(innerPackParam.isPack());
    auto innerTy = SGF.getSILType(innerPackParam, InnerTypesFuncTy);
    auto packAddr =
      SGF.emitTemporaryPackAllocation(Loc, innerTy.getObjectType());

    // Seed the managed pack argument with the right ownership.
    // As we emit things into it, we'll update the cleanup if the pack
    // owns the values.
    switch (innerPackParam.getConvention()) {
    case ParameterConvention::Pack_Inout:
      return ManagedValue::forLValue(packAddr);
    case ParameterConvention::Pack_Guaranteed:
      return ManagedValue::forBorrowedAddressRValue(packAddr);
    case ParameterConvention::Pack_Owned:
      return ManagedValue::forOwnedAddressRValue(packAddr,
                                                 CleanupHandle::invalid());
    case ParameterConvention::Direct_Guaranteed:
    case ParameterConvention::Direct_Owned:
    case ParameterConvention::Direct_Unowned:
    case ParameterConvention::Indirect_Inout:
    case ParameterConvention::Indirect_InoutAliasable:
    case ParameterConvention::Indirect_In_CXX:
    case ParameterConvention::Indirect_In:
    case ParameterConvention::Indirect_In_Guaranteed:
      llvm_unreachable("not a pack convention");
    }
    llvm_unreachable("bad convention");
  }

  ParameterConvention getInnerPackConvention() {
    auto innerPackParam = InnerTypes.front();
    assert(innerPackParam.isPack());
    return innerPackParam.getConvention();
  }

  ParamInfo getInnerPackExpansionSlot(SILValue packAddr) {
    return ParamInfo(IndirectSlot(packAddr), getInnerPackConvention());
  }

  /// Given an element of an inner pack that we're emitting into,
  /// return a fake ParamInfo for it.
  ParamInfo getInnerPackElementSlot(SILType elementTy) {
    auto convention =
      getScalarConventionForPackConvention(getInnerPackConvention());
    return ParamInfo(IndirectSlot(elementTy), convention);
  }

  void finishInnerIndirectPackArg(ManagedValue packAddr) {
    auto innerPackParam = claimNext(InnerTypes);
    assert(innerPackParam.isPack()); (void) innerPackParam;
    InnerArgs.push_back(packAddr);
  }
};

} // end anonymous namespace

ManagedValue
TupleElementAddressGenerator::projectElementAddress(SILGenFunction &SGF,
                                                    SILLocation loc) {
  unsigned eltIndex = getSubstElementIndex();

  auto tupleValue = this->tupleAddr;

  CleanupCloner cloner(SGF, tupleValue);
  auto tupleAddr = tupleValue.forward(SGF);

  auto eltTy = tupleAddr->getType().getTupleElementType(eltIndex);
  ManagedValue eltValue;
  if (!tupleContainsPackExpansion()) {
    eltValue = cloner.clone(
      SGF.B.createTupleElementAddr(loc, tupleAddr, eltIndex, eltTy));
  } else if (isSubstPackExpansion()) {
    eltValue = cloner.cloneForTuplePackExpansionComponent(tupleAddr,
                                                          getInducedPackType(),
                                                          eltIndex);
  } else {
    auto packIndex =
      SGF.B.createScalarPackIndex(loc, eltIndex, getInducedPackType());
    auto eltAddr =
      SGF.B.createTuplePackElementAddr(loc, packIndex, tupleAddr, eltTy);
    eltValue = cloner.clone(eltAddr);
  }

  tupleValue = cloner.cloneForRemainingTupleComponents(tupleAddr,
                                                       getInducedPackTypeIfPresent(),
                                                       eltIndex + 1);
  this->tupleAddr = tupleValue;

  return eltValue;
}

ManagedValue
ExpandedTupleInputGenerator::projectPackComponent(SILGenFunction &SGF,
                                                  SILLocation loc) {
  assert(isOrigPackExpansion());

  auto formalPackType = getFormalPackType();
  auto componentIndex = getPackComponentIndex();

  auto packValue = getPackValue();
  auto packTy = packValue.getType().castTo<SILPackType>();
  auto componentTy = packTy->getSILElementType(componentIndex);

  auto isComponentExpansion = componentTy.is<PackExpansionType>();
  assert(isComponentExpansion == isa<PackExpansionType>(
            formalPackType.getElementType(componentIndex)));

  // Deactive the cleanup for the outer pack value.
  CleanupCloner cloner(SGF, packValue);
  auto packAddr = packValue.forward(SGF);

  ManagedValue componentValue;
  if (isComponentExpansion) {
    // "Project" the expansion component from the pack.
    // This would be a slice, but we can't currently do pack slices
    // in SIL, so for now we're just returning the original pack.
    componentValue =
      cloner.cloneForPackPackExpansionComponent(packAddr, formalPackType,
                                                componentIndex);

  } else {
    // Project the scalar element from the pack.
    auto packIndex =
      SGF.B.createScalarPackIndex(loc, componentIndex, formalPackType);
    auto eltAddr =
      SGF.B.createPackElementGet(loc, packIndex, packAddr, componentTy);

    componentValue = cloner.clone(eltAddr);
  }

  // Re-enter a cleanup for whatever pack components remain.
  packValue = cloner.cloneForRemainingPackComponents(packAddr,
                                                     formalPackType,
                                                     componentIndex + 1);
  updatePackValue(packValue);

  return componentValue;
}

ManagedValue
ExpandedTupleInputGenerator::createPackComponentTemporary(SILGenFunction &SGF,
                                                          SILLocation loc) {
  assert(isOrigPackExpansion());

  auto formalPackType = getFormalPackType();
  auto componentIndex = getPackComponentIndex();

  auto packValue = getPackValue();
  auto packTy = packValue.getType().castTo<SILPackType>();
  auto componentTy = packTy->getSILElementType(componentIndex);

  auto isComponentExpansion = componentTy.is<PackExpansionType>();
  assert(isComponentExpansion == isa<PackExpansionType>(
            formalPackType.getElementType(componentIndex)));

  auto packAddr = packValue.getLValueAddress();

  // If we don't have a pack-expansion component, we're just making a
  // single element.  We don't handle the expansion case for now, because
  // the reabstraction code generally needs to handle it differently
  // anyway.  We could if it's important, but the caller would still need
  // to handle it differently.
  assert(!isComponentExpansion);

  // Create the temporary.
  auto temporary =
    SGF.emitTemporaryAllocation(loc, componentTy.getObjectType());

  // Write the temporary into the pack at the appropriate position.
  auto packIndex =
    SGF.B.createScalarPackIndex(loc, componentIndex, formalPackType);
  SGF.B.createPackElementSet(loc, temporary, packIndex, packAddr);

  return ManagedValue::forLValue(temporary);
}

void ExpandedTupleInputGenerator::setPackComponent(SILGenFunction &SGF,
                                                   SILLocation loc,
                                                   ManagedValue eltValue) {
  assert(isOrigPackExpansion());

  auto formalPackType = getFormalPackType();
  auto componentIndex = getPackComponentIndex();

  auto packValue = getPackValue();

  // If this isn't a substituted pack expansion, write the value into
  // the pack at this element.
  if (!isSubstPackExpansion()) {
    assert(packValue.getType().getPackElementType(componentIndex)
             == eltValue.getType());

    // Write the address into the pack.
    auto packIndex =
      SGF.B.createScalarPackIndex(loc, componentIndex, formalPackType);
    SGF.B.createPackElementSet(loc, eltValue.getValue(), packIndex,
                               packValue.getValue());

  // Otherwise, we assume the caller will have generated a pack loop that
  // sets up the pack appropriately, and we just need to manage cleanups.
  } else {
    assert(eltValue.getValue() == packValue.getValue());
  }

  // Update the cleanup on the pack value if we're building a managed
  // pack value and the element has a cleanup.
  assert(packValue.isLValue() == eltValue.isLValue());

#ifndef NDEBUG
  auto convention = packInputs.getCurrentConvention();
  switch (convention.Value) {
  case SILArgumentConvention::Pack_Out:
  case SILArgumentConvention::Pack_Inout:
    assert(packValue.isLValue());
    break;
  case SILArgumentConvention::Pack_Guaranteed:
    assert(!packValue.isLValue());
    assert(!packValue.hasCleanup());
    assert(!eltValue.hasCleanup() && "putting owned value in guaranteed pack");
    break;
  case SILArgumentConvention::Pack_Owned:
    assert(!packValue.isLValue());
    assert(!packValue.hasCleanup());
    assert(eltValue.isPlusOneOrTrivial(SGF) &&
           "putting borrowed value in owned pack");
    break;
  default:
    llvm_unreachable("not a pack kind");
  }
#endif

  if (!eltValue.hasCleanup())
    return;

  // Forward the old cleanup on the pack and value and enter a new cleanup
  // to cover both.
  // The assumption here is that we're building a +1 pack overall.
  eltValue.forward(SGF);
  auto packAddr = packValue.forward(SGF);
  auto packCleanup =
    SGF.enterDestroyPrecedingPackComponentsCleanup(packAddr, formalPackType,
                                                   componentIndex + 1);
  updatePackValue(ManagedValue::forOwnedAddressRValue(packAddr, packCleanup));
}

ManagedValue
FunctionInputGenerator::projectPackComponent(SILGenFunction &SGF,
                                             SILLocation loc) {
  assert(isOrigPackExpansion());

  auto formalPackType = getFormalPackType();
  auto componentIndex = getPackComponentIndex();

  auto packValue = getPackValue();
  auto packTy = packValue.getType().castTo<SILPackType>();
  auto componentTy = packTy->getSILElementType(componentIndex);

  auto isComponentExpansion = componentTy.is<PackExpansionType>();
  assert(isComponentExpansion == isa<PackExpansionType>(
            formalPackType.getElementType(componentIndex)));

  // Deactivate the cleanup for the outer pack value.
  CleanupCloner cloner(SGF, packValue);
  auto packAddr = packValue.forward(SGF);

  ManagedValue componentValue;
  if (isComponentExpansion) {
    // "Project" the expansion component from the pack.
    // This would be a slice, but we can't currently do pack slices
    // in SIL, so for now we're just managing cleanups.
    componentValue =
      cloner.cloneForPackPackExpansionComponent(packAddr, formalPackType,
                                                componentIndex);

  } else {
    // Project the scalar element from the pack.
    auto packIndex =
      SGF.B.createScalarPackIndex(loc, componentIndex, formalPackType);
    auto eltAddr =
      SGF.B.createPackElementGet(loc, packIndex, packAddr, componentTy);

    componentValue = cloner.clone(eltAddr);
  }

  // Re-enter a cleanup for whatever pack components remain.
  packValue = cloner.cloneForRemainingPackComponents(packAddr,
                                                     formalPackType,
                                                     componentIndex + 1);
  updatePackValue(packValue);

  return componentValue;
}

void TranslateArguments::expandOuterSingleInnerParam(
                            AbstractionPattern innerOrigType,
                            AnyFunctionType::CanParam innerSubstParam,
                            FunctionInputGenerator &outerParam) {
  // If we're not processing a pack expansion, do a normal translation.
  if (!outerParam.isOrigPackExpansion()) {
    expandParam(innerOrigType, innerSubstParam,
                outerParam.getOrigType(), outerParam.getSubstParam());

  // Pull out a scalar component from the pack and process that.
  } else {
    auto outerValue = outerParam.projectPackComponent(SGF, Loc);
    expandSingleOuterParam(innerOrigType, innerSubstParam,
                           outerParam.getOrigType(), outerParam.getSubstParam(),
                           outerValue);
  }

  outerParam.advance();
}

ManagedValue TranslateArguments::expandPackInnerParam(
                       AbstractionPattern innerOrigExpansionType,
                       AnyFunctionType::CanParamArrayRef innerSubstParams,
                       ParamInfo innerPackParam,
                       FunctionInputGenerator &outerParam) {
  assert(isPackParameter(innerPackParam.getConvention()));
  assert(!innerPackParam.hasAddress());
  auto innerTy = innerPackParam.getType();
  auto innerPackTy = innerTy.castTo<SILPackType>();
  assert(innerPackTy->getNumElements() == innerSubstParams.size());

  // TODO: try to just forward the entire pack, or a slice of it.

  // Allocate a pack of the expected type.
  auto innerPackAddr =
    SGF.emitTemporaryPackAllocation(Loc, innerTy.getObjectType());
  auto innerFormalPackType =
    CanPackType::get(SGF.getASTContext(), innerSubstParams);
  auto innerOrigPatternType =
    innerOrigExpansionType.getPackExpansionPatternType();

  SmallVector<CleanupHandle, 4> innerComponentCleanups;

  for (auto innerComponentIndex : indices(innerSubstParams)) {
    SILType innerComponentTy =
      innerPackTy->getSILElementType(innerComponentIndex);
    auto innerSubstType =
      innerSubstParams[innerComponentIndex].getParameterType();

    auto outerSubstType = outerParam.getSubstParam().getParameterType();
    auto outerOrigType = outerParam.getOrigType();

    auto insertScalarIntoPack = [&](ManagedValue inner) {
      assert(!innerComponentTy.is<PackExpansionType>());
      auto innerPackIndex =
        SGF.B.createScalarPackIndex(Loc, innerComponentIndex,
                                    innerFormalPackType);
      SGF.B.createPackElementSet(Loc, inner.getValue(),
                                 innerPackIndex, innerPackAddr);
      if (inner.hasCleanup())
        innerComponentCleanups.push_back(inner.getCleanup());
    };

    // If we're not translating from a pack expansion, process into a
    // single component.  This may claim any number of outer values.
    if (!outerParam.isOrigPackExpansion()) {
      // Fake up a lowered parameter as if we could pass just this
      // component.
      ParamInfo innerComponentParam(IndirectSlot(innerComponentTy),
        getScalarConventionForPackConvention(innerPackParam.getConvention()));

      ManagedValue innerArg = expandSingleInnerIndirect(innerOrigPatternType,
                                                        innerSubstType,
                                                        outerOrigType,
                                                        outerSubstType,
                                                        innerComponentParam);
      insertScalarIntoPack(innerArg);

    // Otherwise, we're starting with the outer value from the pack.
    } else {
      auto outerOrigPatternType = outerOrigType.getPackExpansionPatternType();
      auto outerComponentIndex = outerParam.getPackComponentIndex();
      auto outerPackValue = outerParam.getPackValue();
      auto outerPackTy = outerPackValue.getType().castTo<SILPackType>();
      auto outerComponentTy =
        outerPackTy->getSILElementType(outerComponentIndex);

      // If we have a pack expansion component, emit a pack loop.
      if (auto innerExpansionTy =
            innerComponentTy.getAs<PackExpansionType>()) {
        auto outerExpansionTy = outerComponentTy.castTo<PackExpansionType>();

        // Claim the pack-expansion component and set up to clone its
        // cleanup onto the elements.
        auto outerComponent = outerParam.projectPackComponent(SGF, Loc);

        // We can only do direct forwarding of of the pack elements in
        // one very specific case right now.  That isn't great, but we
        // have to live with it.
        bool forwardouterToinner =
          (outerExpansionTy.getPatternType()
             == innerExpansionTy.getPatternType());

        // The result of the transformation will be +1 unless we do that.
        bool innerIsPlusOne = !forwardouterToinner;

        ManagedValue inner =
          SGF.emitPackTransform(Loc, outerComponent,
                                outerParam.getFormalPackType(),
                                outerComponentIndex,
                                innerPackAddr,
                                innerFormalPackType,
                                innerComponentIndex,
                                /*is trivial*/ forwardouterToinner,
                                innerIsPlusOne,
            [&](ManagedValue outerEltAddr, SILType innerEltTy,
                SGFContext ctxt) {
          // If we decided to just forward, we can do that now.
          if (forwardouterToinner)
            return outerEltAddr;

          // Otherwise, map the subst pattern types into element context.
          CanType innerSubstEltType =
            cast<PackExpansionType>(innerSubstType).getPatternType();
          CanType outerSubstEltType =
            cast<PackExpansionType>(outerSubstType).getPatternType();
          if (auto openedEnv =
                SGF.getInnermostPackExpansion()->OpenedElementEnv) {
            outerSubstEltType = openedEnv
              ->mapContextualPackTypeIntoElementContext(outerSubstEltType);
            innerSubstEltType = openedEnv
              ->mapContextualPackTypeIntoElementContext(innerSubstEltType);
          }

          auto init = ctxt.getEmitInto();
          assert(init);
          processSingleInto(innerOrigPatternType, innerSubstEltType,
                            outerOrigPatternType, outerSubstEltType,
                            outerEltAddr, innerEltTy, *init);
          return ManagedValue::forInContext();
        });

        if (inner.hasCleanup())
          innerComponentCleanups.push_back(inner.getCleanup());

      // Otherwise, claim the next pack component and process it.
      } else {
        ParamInfo innerComponentParam(IndirectSlot(innerComponentTy),
          getScalarConventionForPackConvention(innerPackParam.getConvention()));

        ManagedValue outer = outerParam.projectPackComponent(SGF, Loc);
        ManagedValue inner =
          processSingle(innerOrigPatternType, innerSubstType,
                        outerOrigPatternType, outerSubstType,
                        outer, innerComponentParam);
        insertScalarIntoPack(inner);
      }
    }

    outerParam.advance();
  }

  // Wrap up the value.
  switch (innerPackParam.getConvention()) {
  case ParameterConvention::Indirect_In:
  case ParameterConvention::Indirect_In_Guaranteed:
  case ParameterConvention::Indirect_Inout:
  case ParameterConvention::Indirect_InoutAliasable:
  case ParameterConvention::Indirect_In_CXX:
  case ParameterConvention::Direct_Owned:
  case ParameterConvention::Direct_Unowned:
  case ParameterConvention::Direct_Guaranteed:
    llvm_unreachable("not a pack parameter convention");

  case ParameterConvention::Pack_Inout:
    llvm_unreachable("pack inout reabstraction not supported");

  // For guaranteed, leave the cleanups in place and produce a
  // borrowed value.
  case ParameterConvention::Pack_Guaranteed:
    return ManagedValue::forBorrowedAddressRValue(innerPackAddr);

  // For owned, forward all the cleanups and enter a new cleanup
  // for the entire pack.
  case ParameterConvention::Pack_Owned: {
    if (innerComponentCleanups.empty())
      return ManagedValue::forTrivialAddressRValue(innerPackAddr);

    for (auto cleanup : innerComponentCleanups) {
      SGF.Cleanups.forwardCleanup(cleanup);
    }

    auto packCleanup =
      SGF.enterDestroyPackCleanup(innerPackAddr, innerFormalPackType);
    return ManagedValue::forOwnedAddressRValue(innerPackAddr, packCleanup);
  }
  }
  llvm_unreachable("bad convention");
}

/// Apply trivial conversions to a value to handle differences between the inner
/// and outer types of a function conversion thunk.
static ManagedValue applyTrivialConversions(SILGenFunction &SGF,
                                            SILLocation loc,
                                            ManagedValue innerValue,
                                            SILType outerType) {
  auto innerASTTy = innerValue.getType().getASTType();
  auto outerASTTy = outerType.getASTType();
  if (innerASTTy->hasArchetype())
    innerASTTy = innerASTTy->mapTypeOutOfContext()->getCanonicalType();
  if (outerASTTy->hasArchetype())
    outerASTTy = outerASTTy->mapTypeOutOfContext()->getCanonicalType();

  if (innerASTTy == outerASTTy) {
    return innerValue;
  }
  if (innerASTTy->getClassOrBoundGenericClass()
      && outerASTTy->getClassOrBoundGenericClass()) {
    if (outerASTTy->isExactSuperclassOf(innerASTTy)) {
      return SGF.B.createUpcast(loc, innerValue, outerType);
    } else if (innerASTTy->isExactSuperclassOf(outerASTTy)) {
      return SGF.B.createUncheckedRefCast(loc, innerValue, outerType);
    }
  } else if (auto innerFnTy = dyn_cast<SILFunctionType>(innerASTTy)) {
    if (auto outerFnTy = dyn_cast<SILFunctionType>(outerASTTy)) {
      auto abiDiffA =
        SGF.SGM.Types.checkFunctionForABIDifferences(SGF.SGM.M,
                                                     innerFnTy,
                                                     outerFnTy);
      auto abiDiffB =
        SGF.SGM.Types.checkFunctionForABIDifferences(SGF.SGM.M,
                                                     outerFnTy,
                                                     innerFnTy);
      
      if (abiDiffA == TypeConverter::ABIDifference::CompatibleRepresentation
        || abiDiffA == TypeConverter::ABIDifference::CompatibleCallingConvention
        || abiDiffB == TypeConverter::ABIDifference::CompatibleRepresentation
        || abiDiffB == TypeConverter::ABIDifference::CompatibleCallingConvention) {
        return SGF.B.createConvertFunction(loc, innerValue, outerType);
      }
    }
  }

  llvm_unreachable("unhandled reabstraction type mismatch");
}

/// Forward arguments according to a function type's ownership conventions.
static void forwardFunctionArguments(SILGenFunction &SGF,
                                     SILLocation loc,
                                     CanSILFunctionType fTy,
                                     ArrayRef<ManagedValue> managedArgs,
                                     SmallVectorImpl<SILValue> &forwardedArgs) {
  auto argTypes = fTy->getParameters();
  for (auto index : indices(managedArgs)) {
    auto arg = managedArgs[index];
    auto argTy = argTypes[index];
    auto argSubstTy =
        argTy.getArgumentType(SGF.SGM.M, fTy, SGF.getTypeExpansionContext());

    arg = applyTrivialConversions(SGF, loc, arg,
                                  SILType::getPrimitiveObjectType(argSubstTy));

    if (argTy.isConsumedInCaller()) {
      forwardedArgs.push_back(arg.ensurePlusOne(SGF, loc).forward(SGF));
      continue;
    }

    if (isGuaranteedParameterInCallee(argTy.getConvention())) {
      forwardedArgs.push_back(
          SGF.emitManagedBeginBorrow(loc, arg.getValue()).getValue());
      continue;
    }

    forwardedArgs.push_back(arg.getValue());
  }
}

namespace {
  class YieldInfo {
    SmallVector<AbstractionPattern, 1> OrigTypes;
    SmallVector<AnyFunctionType::Param, 1> Yields;
    ArrayRef<SILYieldInfo> LoweredInfos;
  public:
    YieldInfo(SILGenModule &SGM, SILDeclRef function,
              CanSILFunctionType loweredType, SubstitutionMap subs) {
      LoweredInfos = loweredType->getUnsubstitutedType(SGM.M)->getYields();

      auto accessor = cast<AccessorDecl>(function.getDecl());
      auto storage = accessor->getStorage();

      OrigTypes.push_back(
        SGM.Types.getAbstractionPattern(storage, /*nonobjc*/ true));

      SmallVector<AnyFunctionType::Yield, 1> yieldsBuffer;
      auto yields = AnyFunctionRef(accessor).getYieldResults(yieldsBuffer);
      assert(yields.size() == 1);
      Yields.push_back(yields[0].getCanonical().subst(subs).asParam());
    }

    ArrayRef<AbstractionPattern> getOrigTypes() const { return OrigTypes; }
    AnyFunctionType::CanParamArrayRef getSubstTypes() const {
      return AnyFunctionType::CanParamArrayRef(Yields);
    }
    ArrayRef<SILYieldInfo> getLoweredTypes() const { return LoweredInfos; }
  };
}

static ManagedValue manageYield(SILGenFunction &SGF, SILValue value,
                                SILYieldInfo info) {
  switch (info.getConvention()) {
  case ParameterConvention::Indirect_Inout:
  case ParameterConvention::Indirect_InoutAliasable:
  case ParameterConvention::Pack_Inout:
    return ManagedValue::forLValue(value);
  case ParameterConvention::Direct_Owned:
  case ParameterConvention::Indirect_In_CXX:
  case ParameterConvention::Indirect_In:
  case ParameterConvention::Pack_Owned:
    return SGF.emitManagedRValueWithCleanup(value);
  case ParameterConvention::Direct_Guaranteed:
  case ParameterConvention::Direct_Unowned:
  case ParameterConvention::Pack_Guaranteed:
    if (value->getOwnershipKind() == OwnershipKind::None)
      return ManagedValue::forObjectRValueWithoutOwnership(value);
    return ManagedValue::forBorrowedObjectRValue(value);
  case ParameterConvention::Indirect_In_Guaranteed: {
    if (SGF.silConv.useLoweredAddresses()) {
      return ManagedValue::forBorrowedAddressRValue(value);
    }
    if (value->getType().isTrivial(SGF.F)) {
      return ManagedValue::forObjectRValueWithoutOwnership(value);
    }
    return ManagedValue::forBorrowedObjectRValue(value);
  }
  }
  llvm_unreachable("bad kind");
}

static void manageYields(SILGenFunction &SGF, ArrayRef<SILValue> yields,
                         ArrayRef<SILYieldInfo> yieldInfos,
                         SmallVectorImpl<ManagedValue> &yieldMVs) {
  assert(yields.size() == yieldInfos.size());
  for (auto i : indices(yields)) {
    yieldMVs.push_back(manageYield(SGF, yields[i], yieldInfos[i]));
  }
}

/// Translate the values yielded to us back out to the caller.
static void translateYields(SILGenFunction &SGF, SILLocation loc,
                            ArrayRef<SILValue> innerYields,
                            const YieldInfo &innerInfos,
                            const YieldInfo &outerInfos) {
  assert(innerInfos.getOrigTypes().size() == innerInfos.getSubstTypes().size());
  assert(outerInfos.getOrigTypes().size() == outerInfos.getSubstTypes().size());
  assert(innerInfos.getOrigTypes().size() == outerInfos.getOrigTypes().size());

  SmallVector<ManagedValue, 4> innerMVs;
  manageYields(SGF, innerYields, innerInfos.getLoweredTypes(), innerMVs);

  FullExpr scope(SGF.Cleanups, CleanupLocation(loc));

  // Map the SILYieldInfos into the local context and incidentally turn
  // them into SILParameterInfos.
  SmallVector<SILParameterInfo, 4> outerLoweredTypesAsParameters;
  for (auto unmappedInfo : outerInfos.getLoweredTypes()) {
    auto mappedTy = SGF.F.mapTypeIntoContext(
                                     unmappedInfo.getSILStorageInterfaceType());
    outerLoweredTypesAsParameters.push_back({mappedTy.getASTType(),
                                             unmappedInfo.getConvention()});
  }

  // Translate the yields as if they were arguments.
  SmallVector<ManagedValue, 4> outerMVs;
  TranslateArguments translator(SGF, loc, innerMVs, outerMVs,
                                CanSILFunctionType(),
                                outerLoweredTypesAsParameters);

  // Note that we intentionally reverse the outer and inner types here.
  translator.process(outerInfos.getOrigTypes(), outerInfos.getSubstTypes(),
                     innerInfos.getOrigTypes(), innerInfos.getSubstTypes());

  // Prepare a destination for the unwind; use the current cleanup stack
  // as the depth so that we branch right to it.
  SILBasicBlock *unwindBB = SGF.createBasicBlock(FunctionSection::Postmatter);
  JumpDest unwindDest(unwindBB, SGF.Cleanups.getCleanupsDepth(),
                      CleanupLocation(loc));

  // Emit the yield.
  SGF.emitRawYield(loc, outerMVs, unwindDest, /*unique*/ true);

  // Emit the unwind block.
  {
    SILGenSavedInsertionPoint savedIP(SGF, unwindBB,
                                      FunctionSection::Postmatter);

    // Emit all active cleanups.
    SGF.Cleanups.emitCleanupsForReturn(CleanupLocation(loc), IsForUnwind);
    SGF.B.createUnwind(loc);
  }
}

namespace {

/// A helper class to translate the inner results to the outer results.
///
/// Creating a result-translation plan involves three basic things:
///   - building SILArguments for each of the outer indirect results
///   - building a list of SILValues for each of the inner indirect results
///   - building a list of Operations to perform which will reabstract
///     the inner results to match the outer.
class ResultPlanner : public ExpanderBase<ResultPlanner, IndirectSlot> {
  /// A single result-translation operation.
  struct Operation {
    enum Kind {
      /// Take the last N direct outer results, tuple them, and make that a
      /// new direct outer result.
      ///
      /// Valid: NumElements, OuterResult
      TupleDirect,

      /// Take the last direct inner result, which must be a tuple, and
      /// split it into its elements.
      DestructureDirectInnerTuple,

      /// Take the last direct outer result, inject it into an optional
      /// type, and make that a new direct outer result.
      ///
      /// Valid: SomeDecl, OuterResult
      InjectOptionalDirect,

      /// Finish building an optional Some in the given address.
      ///
      /// Valid: SomeDecl, OuterResultAddr
      InjectOptionalIndirect,

      /// Take the next direct inner result and just make it a direct
      /// outer result.
      ///
      /// Valid: InnerResult, OuterResult.
      DirectToDirect,

      /// Take the next direct inner result and store it into an
      /// outer result address.
      ///
      /// Valid: InnerDirect, OuterResultAddr.
      DirectToIndirect,

      /// Take from an indirect inner result and make it the next outer
      /// direct result.
      ///
      /// Valid: InnerResultAddr, OuterResult.
      IndirectToDirect,

      /// Take from an indirect inner result into an outer indirect result.
      ///
      /// Valid: InnerResultAddr, OuterResultAddr.
      IndirectToIndirect,

      /// Take a value out of the source inner result address, reabstract
      /// it, and initialize the destination outer result address.
      ///
      /// Valid: reabstraction info, InnerAddress, OuterAddress.
      ReabstractIndirectToIndirect,

      /// Take a value out of the source inner result address, reabstract
      /// it, and add it as the next direct outer result.
      ///
      /// Valid: reabstraction info, InnerAddress, OuterResult.
      ReabstractIndirectToDirect,

      /// Take the next direct inner result, reabstract it, and initialize
      /// the destination outer result address.
      ///
      /// Valid: reabstraction info, InnerResult, OuterAddress.
      ReabstractDirectToIndirect,

      /// Take the next direct inner result, reabstract it, and add it as
      /// the next direct outer result.
      ///
      /// Valid: reabstraction info, InnerResult, OuterResult.
      ReabstractDirectToDirect,

      /// Reabstract the elements of the inner result address (a tuple)
      /// into the elements of the given component of the outer result
      /// address.
      ///
      /// Valid: reabstraction info, InnerResultAddr, OuterResultAddr,
      /// PackExpansion.
      ReabstractTupleIntoPackExpansion,
    };

    Operation(Kind kind) : TheKind(kind) {}

    Kind TheKind;

    // Reabstraction information.  Only valid for reabstraction kinds.
    AbstractionPattern InnerOrigType = AbstractionPattern::getInvalid();
    AbstractionPattern OuterOrigType = AbstractionPattern::getInvalid();
    CanType InnerSubstType, OuterSubstType;

    union {
      SILValue InnerResultAddr;
      SILResultInfo InnerResult;
      unsigned NumElements;
      EnumElementDecl *SomeDecl;
    };

    union {
      SILValue OuterResultAddr;
      SILResultInfo OuterResult;
    };

    union {
      struct {
        CanPackType OuterFormalPackType;
        CanPackType InnerFormalPackType;
        unsigned OuterComponentIndex;
        unsigned InnerComponentIndex;
      } PackExpansion;
    };

    void emitReabstractTupleIntoPackExpansion(SILGenFunction &SGF,
                                              SILLocation loc);
  };

  class InnerPackResultGenerator {
    ResultPlanner &planner;
  public:
    InnerPackResultGenerator(ResultPlanner &planner) : planner(planner) {}

    ManagedValue claimNext() {
      SILResultInfo resultInfo = planner.claimNextInnerResult();
      return ManagedValue::forLValue(
        planner.addInnerIndirectPackResultTemporary(resultInfo));
    }
    SILArgumentConvention getCurrentConvention() const {
      return SILArgumentConvention::Pack_Out;
    }
    void finishCurrent(ManagedValue packAddr) {
      // ignore this
    }
  };

  struct IndirectTupleExpansionCombiner {
    IndirectTupleExpansionCombiner(ResultPlanner &planner) {}

    IndirectSlot getElementSlot(SILValue eltAddr) {
      return IndirectSlot(eltAddr);
    }
    void collectElement(ManagedValue eltAddr) {
      assert(eltAddr.isLValue());
    }
    ManagedValue finish(SILValue tupleAddr, IndirectSlot tupleSlot) {
      return ManagedValue::forLValue(tupleAddr);
    }
  };

  SmallVector<Operation, 8> Operations;
  ArrayRef<SILResultInfo> AllOuterResults;
  ArrayRef<SILResultInfo> AllInnerResults;
  SmallVectorImpl<SILValue> &InnerArgs;
  InnerPackResultGenerator InnerPacks;

public:
  ResultPlanner(SILGenFunction &SGF, SILLocation loc,
                ArrayRef<ManagedValue> outerIndirectArgs,
                SmallVectorImpl<SILValue> &innerArgs)
    : ExpanderBase(SGF, loc, outerIndirectArgs),
      InnerArgs(innerArgs), InnerPacks(*this) {}

  void plan(AbstractionPattern innerOrigType, CanType innerSubstType,
            AbstractionPattern outerOrigType, CanType outerSubstType,
            CanSILFunctionType innerFnType, CanSILFunctionType outerFnType) {
    // Assert that the indirect results are set up like we expect.
    assert(InnerArgs.empty());
    assert(SGF.F.begin()->args_size()
           >= SILFunctionConventions(outerFnType, SGF.SGM.M)
                  .getNumIndirectSILResults());

    InnerArgs.reserve(
        SILFunctionConventions(innerFnType, SGF.SGM.M)
            .getNumIndirectSILResults());

    AllOuterResults = outerFnType->getUnsubstitutedType(SGF.SGM.M)->getResults();
    AllInnerResults = innerFnType->getUnsubstitutedType(SGF.SGM.M)->getResults();

    // Recursively walk the result types.
    expand(innerOrigType, innerSubstType, outerOrigType, outerSubstType);

    // Assert that we consumed and produced all the indirect result
    // information we needed.
    assert(AllOuterResults.empty());
    assert(AllInnerResults.empty());
    assert(InnerArgs.size() ==
           SILFunctionConventions(innerFnType, SGF.SGM.M)
               .getNumIndirectSILResults());
    OuterArgs.finish();
  }

  SILValue execute(SILValue innerResult, CanSILFunctionType innerFuncTy);

private:
  friend ExpanderBase;

  void execute(SmallVectorImpl<SILValue> &innerDirectResults,
               SmallVectorImpl<SILValue> &outerDirectResults);
  void executeInnerTuple(SILValue innerElement,
                         SmallVector<SILValue, 4> &innerDirectResults);

  void expandInnerTuple(AbstractionPattern innerOrigType,
                        CanTupleType innerSubstType,
                        AbstractionPattern outerOrigType,
                        CanType outerSubstType);
  void expandOuterTuple(AbstractionPattern innerOrigType,
                        CanType innerSubstType,
                        AbstractionPattern outerOrigType,
                        CanTupleType outerSubstType);

  void expandSingle(AbstractionPattern innerOrigType,
                    CanType innerSubstType,
                    AbstractionPattern outerOrigType,
                    CanType outerSubstType);
  void planSingle(AbstractionPattern innerOrigType,
                  CanType innerSubstType,
                  AbstractionPattern outerOrigType,
                  CanType outerSubstType,
                  SILResultInfo innerResult,
                  SILResultInfo outerResult,
                  SILValue optOuterResultAddr);

  void expandInnerTupleOuterIndirect(AbstractionPattern innerOrigType,
                                     CanTupleType innerSubstType,
                                     AbstractionPattern outerOrigType,
                                     CanType outerSubstType,
                                     ManagedValue outerAddr);

  void expandSingleOuterIndirect(AbstractionPattern innerOrigType,
                                 CanType innerSubstType,
                                 AbstractionPattern outerOrigType,
                                 CanType outerSubstType,
                                 ManagedValue outerAddr);
  void planSingleIntoIndirect(AbstractionPattern innerOrigType,
                              CanType innerSubstType,
                              AbstractionPattern outerOrigType,
                              CanType outerSubstType,
                              SILResultInfo innerResult,
                              SILValue outerResultAddr);

  void planIntoDirect(AbstractionPattern innerOrigType,
                      CanType innerSubstType,
                      AbstractionPattern outerOrigType,
                      CanType outerSubstType,
                      SILResultInfo outerResult);
  void planSingleIntoDirect(AbstractionPattern innerOrigType,
                            CanType innerSubstType,
                            AbstractionPattern outerOrigType,
                            CanType outerSubstType,
                            SILResultInfo innerResult,
                            SILResultInfo outerResult);
  void planExpandedIntoDirect(AbstractionPattern innerOrigType,
                              CanTupleType innerSubstType,
                              AbstractionPattern outerOrigType,
                              CanType outerSubstType,
                              SILResultInfo outerResult);

  void planFromDirect(AbstractionPattern innerOrigType,
                      CanType innerSubstType,
                      AbstractionPattern outerOrigType,
                      CanType outerSubstType,
                      SILResultInfo innerResult);

  ManagedValue
  expandOuterTupleInnerIndirect(AbstractionPattern innerOrigType,
                                CanType innerSubstType,
                                AbstractionPattern outerOrigType,
                                CanTupleType outerSubstType,
                                IndirectSlot innerResultAddr);
  void planExpandedFromDirect(AbstractionPattern innerOrigType,
                              CanTupleType innerSubstType,
                              AbstractionPattern outerOrigType,
                              CanTupleType outerSubstType,
                              SILResultInfo innerResult);
  ManagedValue
  expandSingleInnerIndirect(AbstractionPattern innerOrigType,
                            CanType innerSubstType,
                            AbstractionPattern outerOrigType,
                            CanType outerSubstType,
                            IndirectSlot innerResultAddr);
  SILValue planSingleFromIndirect(AbstractionPattern innerOrigType,
                                  CanType innerSubstType,
                                  AbstractionPattern outerOrigType,
                                  CanType outerSubstType,
                                  IndirectSlot innerResultSlot,
                                  SILResultInfo outerResult,
                                  SILValue optOuterResultAddr);

  ManagedValue expandSingleIndirect(AbstractionPattern innerOrigType,
                                    CanType innerSubstType,
                                    AbstractionPattern outerOrigType,
                                    CanType outerSubstType,
                                    IndirectSlot innerResultTy,
                                    ManagedValue outerResultAddr);
  SILValue planIndirectIntoIndirect(AbstractionPattern innerOrigType,
                                    CanType innerSubstType,
                                    AbstractionPattern outerOrigType,
                                    CanType outerSubstType,
                                    IndirectSlot innerResultAddr,
                                    SILValue outerResultAddr);

  ManagedValue expandPackExpansion(AbstractionPattern innerOrigType,
                                   CanPackExpansionType innerSubstType,
                                   AbstractionPattern outerOrigType,
                                   CanPackExpansionType outerSubstType,
                                   CanPackType innerFormalPackType,
                                   IndirectSlot innerTupleOrPackSlot,
                                   unsigned innerPackComponentIndex,
                                   CanPackType outerFormalPackType,
                                   ManagedValue outerTupleOrPackAddr,
                                   unsigned outerPackComponentIndex);

  /// Claim the next inner result from the plan data.
  SILResultInfo claimNextInnerResult() {
    return claimNext(AllInnerResults);
  }

  /// Claim the next outer result from the plan data.  If it's indirect,
  /// grab its SILArgument.
  std::pair<SILResultInfo, SILValue> claimNextOuterResult() {
    SILResultInfo result = claimNext(AllOuterResults);

    SILValue resultAddr;
    if (SGF.silConv.isSILIndirect(result)) {
      resultAddr = OuterArgs.claimNext().getLValueAddress();
    }

    return { result, resultAddr };
  }

  friend OuterPackArgGenerator<ResultPlanner>;
  ManagedValue claimNextOuterPackArg() {
    SILResultInfo result = claimNext(AllOuterResults);
    assert(result.isPack()); (void) result;

    return OuterArgs.claimNext();
  }
  SILArgumentConvention getOuterPackConvention() {
    return SILArgumentConvention::Pack_Out;
  }

  /// Create a temporary address suitable for passing to the given inner
  /// indirect result and add it as an inner indirect result.
  SILValue addInnerIndirectResultTemporary(SILResultInfo innerResult) {
    assert(SGF.silConv.isSILIndirect(innerResult) ||
           !SGF.silConv.useLoweredAddresses());
    auto temporary =
        SGF.emitTemporaryAllocation(Loc,
                            SGF.getSILType(innerResult, CanSILFunctionType()));
    InnerArgs.push_back(temporary);
    return temporary;
  }

  SILValue addInnerIndirectPackResultTemporary(SILResultInfo innerResult) {
    assert(innerResult.isPack());
    assert(SGF.silConv.isSILIndirect(innerResult));
    auto temporary =
        SGF.emitTemporaryPackAllocation(Loc,
                            SGF.getSILType(innerResult, CanSILFunctionType()));
    InnerArgs.push_back(temporary);
    return temporary;
  }

  IndirectSlot getInnerPackExpansionSlot(SILValue packAddr) {
    return IndirectSlot(packAddr);
  }

  IndirectSlot getInnerPackElementSlot(SILType elementTy) {
    return IndirectSlot(elementTy);
  }

  PackGeneratorRef getInnerPackGenerator() {
    return InnerPacks;
  }

  /// Cause the next inner indirect result to be emitted directly into
  /// the given outer result address.
  void addInPlace(SILValue outerResultAddr) {
    InnerArgs.push_back(outerResultAddr);
    // Does not require an Operation.
  }

  Operation &addOperation(Operation::Kind kind) {
    Operations.emplace_back(kind);
    return Operations.back();
  }

  void addDirectToDirect(SILResultInfo innerResult, SILResultInfo outerResult) {
    auto &op = addOperation(Operation::DirectToDirect);
    op.InnerResult = innerResult;
    op.OuterResult = outerResult;
  }

  void addDirectToIndirect(SILResultInfo innerResult,
                           SILValue outerResultAddr) {
    auto &op = addOperation(Operation::DirectToIndirect);
    op.InnerResult = innerResult;
    op.OuterResultAddr = outerResultAddr;
  }

  void addIndirectToDirect(SILValue innerResultAddr,
                           SILResultInfo outerResult) {
    auto &op = addOperation(Operation::IndirectToDirect);
    op.InnerResultAddr = innerResultAddr;
    op.OuterResult = outerResult;
  }

  void addIndirectToIndirect(SILValue innerResultAddr,
                             SILValue outerResultAddr) {
    auto &op = addOperation(Operation::IndirectToIndirect);
    op.InnerResultAddr = innerResultAddr;
    op.OuterResultAddr = outerResultAddr;
  }

  void addTupleDirect(unsigned numElements, SILResultInfo outerResult) {
    auto &op = addOperation(Operation::TupleDirect);
    op.NumElements = numElements;
    op.OuterResult = outerResult;
  }

  void addDestructureDirectInnerTuple(SILResultInfo innerResult) {
    auto &op = addOperation(Operation::DestructureDirectInnerTuple);
    op.InnerResult = innerResult;
  }

  void addInjectOptionalDirect(EnumElementDecl *someDecl,
                               SILResultInfo outerResult) {
    auto &op = addOperation(Operation::InjectOptionalDirect);
    op.SomeDecl = someDecl;
    op.OuterResult = outerResult;
  }

  void addInjectOptionalIndirect(EnumElementDecl *someDecl,
                                 SILValue outerResultAddr) {
    auto &op = addOperation(Operation::InjectOptionalIndirect);
    op.SomeDecl = someDecl;
    op.OuterResultAddr = outerResultAddr;
  }

  void addReabstractDirectToDirect(AbstractionPattern innerOrigType,
                                   CanType innerSubstType,
                                   AbstractionPattern outerOrigType,
                                   CanType outerSubstType,
                                   SILResultInfo innerResult,
                                   SILResultInfo outerResult) {
    auto &op = addOperation(Operation::ReabstractDirectToDirect);
    op.InnerResult = innerResult;
    op.OuterResult = outerResult;
    op.InnerOrigType = innerOrigType;
    op.InnerSubstType = innerSubstType;
    op.OuterOrigType = outerOrigType;
    op.OuterSubstType = outerSubstType;
  }

  void addReabstractDirectToIndirect(AbstractionPattern innerOrigType,
                                     CanType innerSubstType,
                                     AbstractionPattern outerOrigType,
                                     CanType outerSubstType,
                                     SILResultInfo innerResult,
                                     SILValue outerResultAddr) {
    auto &op = addOperation(Operation::ReabstractDirectToIndirect);
    op.InnerResult = innerResult;
    op.OuterResultAddr = outerResultAddr;
    op.InnerOrigType = innerOrigType;
    op.InnerSubstType = innerSubstType;
    op.OuterOrigType = outerOrigType;
    op.OuterSubstType = outerSubstType;
  }

  void addReabstractIndirectToDirect(AbstractionPattern innerOrigType,
                                     CanType innerSubstType,
                                     AbstractionPattern outerOrigType,
                                     CanType outerSubstType,
                                     SILValue innerResultAddr,
                                     SILResultInfo outerResult) {
    auto &op = addOperation(Operation::ReabstractIndirectToDirect);
    op.InnerResultAddr = innerResultAddr;
    op.OuterResult = outerResult;
    op.InnerOrigType = innerOrigType;
    op.InnerSubstType = innerSubstType;
    op.OuterOrigType = outerOrigType;
    op.OuterSubstType = outerSubstType;
  }

  void addReabstractIndirectToIndirect(AbstractionPattern innerOrigType,
                                       CanType innerSubstType,
                                       AbstractionPattern outerOrigType,
                                       CanType outerSubstType,
                                       SILValue innerResultAddr,
                                       SILValue outerResultAddr) {
    auto &op = addOperation(Operation::ReabstractIndirectToIndirect);
    op.InnerResultAddr = innerResultAddr;
    op.OuterResultAddr = outerResultAddr;
    op.InnerOrigType = innerOrigType;
    op.InnerSubstType = innerSubstType;
    op.OuterOrigType = outerOrigType;
    op.OuterSubstType = outerSubstType;
  }

  void addReabstractTupleIntoPackExpansion(AbstractionPattern innerOrigType,
                                           CanPackExpansionType innerSubstType,
                                           AbstractionPattern outerOrigType,
                                           CanPackExpansionType outerSubstType,
                                           CanPackType innerFormalPackType,
                                           SILValue innerResultAddr,
                                           unsigned innerComponentIndex,
                                           CanPackType outerFormalPackType,
                                           SILValue outerResultAddr,
                                           unsigned outerComponentIndex) {
    auto &op = addOperation(Operation::ReabstractTupleIntoPackExpansion);
    op.InnerResultAddr = innerResultAddr;
    op.OuterResultAddr = outerResultAddr;
    op.InnerOrigType = innerOrigType;
    op.InnerSubstType = innerSubstType;
    op.OuterOrigType = outerOrigType;
    op.OuterSubstType = outerSubstType;
    op.PackExpansion.InnerFormalPackType = innerFormalPackType;
    op.PackExpansion.InnerComponentIndex = innerComponentIndex;
    op.PackExpansion.OuterFormalPackType = outerFormalPackType;
    op.PackExpansion.OuterComponentIndex = outerComponentIndex;
  }
};

} // end anonymous namespace

/// The general case of translation, where we may need to
/// expand tuples.
template <class Impl, class InnerSlotType>
void ExpanderBase<Impl, InnerSlotType>::expand(
                                        AbstractionPattern innerOrigType,
                                        CanType innerSubstType,
                                        AbstractionPattern outerOrigType,
                                        CanType outerSubstType) {
  // The substituted types must match up in tuple-ness and arity,
  // *except* that we allow abstraction into any/optional in the direction
  // of the conversion.
#ifndef NDEBUG
  {
    auto innerTupleType = dyn_cast<TupleType>(innerSubstType);
    auto outerTupleType = dyn_cast<TupleType>(outerSubstType);
    if (innerTupleType) {
      if (outerTupleType) {
        assert(innerTupleType->getNumElements() ==
               outerTupleType->getNumElements());
      } else {
        // FIXME: only allowed for ResultPlanner
        assert(outerSubstType->isAny() || outerSubstType->getOptionalObjectType());
      }
    } else {
      if (outerTupleType) {
        // FIXME: only allowed for TranslateArguments
        assert(innerSubstType->isAny() || innerSubstType->getOptionalObjectType());
      }
    }
  }
#endif

  // Tuples in the abstraction pattern are expanded.
  // If we have a vanishing tuple on one side or the other,
  // we should look through that structure immediately and recurse.
  // Otherwise, if we have non-vanishing tuple structure on both sides,
  // we need to walk it in parallel.
  // Otherwise, we should only have non-vanishing tuple structure on
  // the input side, and the output side must be Any or optional.

  // If the outer abstraction pattern is a vanishing tuple, look
  // through that and recurse.
  bool outerIsExpanded = outerOrigType.isTuple();
  if (outerIsExpanded && outerOrigType.doesTupleVanish()) {
    asImpl().expandOuterVanishingTuple(outerOrigType, outerSubstType,
       [&](AbstractionPattern outerOrigEltType, CanType outerSubstEltType) {
      asImpl().expand(innerOrigType, innerSubstType,
                      outerOrigEltType, outerSubstEltType);
    }, [&](AbstractionPattern outerOrigEltType, CanType outerSubstEltType,
           ManagedValue outerAddr) {
      return asImpl().expandOuterIndirect(innerOrigType, innerSubstType,
                                          outerOrigEltType, outerSubstEltType,
                                          outerAddr);
    });
    return;
  }

  // If the inner abstraction pattern is a vanishing tuple, look
  // through that and recurse.
  bool innerIsExpanded = innerOrigType.isTuple();
  if (innerIsExpanded && innerOrigType.doesTupleVanish()) {
    expandInnerVanishingTuple(innerOrigType, innerSubstType,
       [&](AbstractionPattern innerOrigEltType, CanType innerSubstEltType) {
      asImpl().expand(innerOrigEltType, innerSubstEltType,
                      outerOrigType, outerSubstType);
    }, [&](AbstractionPattern innerOrigEltType, CanType innerSubstEltType,
           SILType innerEltTy) {
      auto innerSlot = asImpl().getInnerPackElementSlot(innerEltTy);
      return asImpl().expandInnerIndirect(innerOrigEltType,
                                          innerSubstEltType,
                                          outerOrigType,
                                          outerSubstType,
                                          innerSlot);
    });
    return;
  }

  if (innerIsExpanded && outerIsExpanded) {
    asImpl().expandParallelTuples(innerOrigType, innerSubstType,
                                  outerOrigType, outerSubstType);
  } else if (outerIsExpanded) {
    asImpl().expandOuterTuple(innerOrigType, innerSubstType,
                              outerOrigType, cast<TupleType>(outerSubstType));
  } else if (innerIsExpanded) {
    asImpl().expandInnerTuple(innerOrigType, cast<TupleType>(innerSubstType),
                              outerOrigType, outerSubstType);
  } else {
    asImpl().expandSingle(innerOrigType, innerSubstType,
                          outerOrigType, outerSubstType);
  }
}

template <class Impl, class InnerSlotType>
void ExpanderBase<Impl, InnerSlotType>::expandOuterVanishingTuple(
                          AbstractionPattern outerOrigType,
                          CanType outerSubstType,
    llvm::function_ref<void(AbstractionPattern outerOrigEltType,
                            CanType outerSubstEltType)> handleSingle,
    llvm::function_ref<void(AbstractionPattern outerOrigEltType,
                            CanType outerOrigSubstType,
                            ManagedValue outerEltAddr)> handlePackElement) {
  assert(outerOrigType.isTuple());
  assert(outerOrigType.doesTupleVanish());

  bool foundSurvivor = false;

  ExpandedTupleInputGenerator elt(SGF.getASTContext(), OuterPackArgs,
                                  outerOrigType, outerSubstType);
  for (; !elt.isFinished(); elt.advance()) {
    assert(!foundSurvivor);
    foundSurvivor = true;

    if (!elt.isOrigPackExpansion()) {
      handleSingle(elt.getOrigType(), outerSubstType);
    } else {
      assert(elt.getPackComponentIndex() == 0);
      assert(!isa<PackExpansionType>(elt.getSubstType()));
      ManagedValue eltAddr = elt.projectPackComponent(SGF, Loc);
      handlePackElement(elt.getOrigType().getPackExpansionPatternType(),
                        outerSubstType, eltAddr);
    }
  }
  elt.finish();

  assert(foundSurvivor && "vanishing tuple had no surviving element?");
}

template <class Impl, class InnerSlotType>
void ExpanderBase<Impl, InnerSlotType>::expandInnerVanishingTuple(
                          AbstractionPattern innerOrigType,
                          CanType innerSubstType,
    llvm::function_ref<void(AbstractionPattern innerOrigEltType,
                            CanType innerSubstEltType)> handleSingle,
    llvm::function_ref<ManagedValue(AbstractionPattern innerOrigEltType,
                                    CanType innerOrigSubstType,
                                    SILType innerEltTy)> handlePackElement) {
  assert(innerOrigType.isTuple());
  assert(innerOrigType.doesTupleVanish());

  bool foundSurvivor = false;

  ExpandedTupleInputGenerator elt(SGF.getASTContext(),
                                  asImpl().getInnerPackGenerator(),
                                  innerOrigType, innerSubstType);
  for (; !elt.isFinished(); elt.advance()) {
    assert(!foundSurvivor);
    foundSurvivor = true;

    if (!elt.isOrigPackExpansion()) {
      handleSingle(elt.getOrigType(), innerSubstType);
    } else {
      assert(elt.getPackComponentIndex() == 0);
      assert(!isa<PackExpansionType>(elt.getSubstType()));
      ManagedValue eltAddr =
        handlePackElement(elt.getOrigType().getPackExpansionPatternType(),
                          innerSubstType, elt.getPackComponentType());
      elt.setPackComponent(SGF, Loc, eltAddr);
    }
  }
  elt.finish();

  assert(foundSurvivor && "vanishing tuple had no surviving element?");
}

template <class Impl, class InnerSlotType>
void ExpanderBase<Impl, InnerSlotType>::expandParallelTuples(
                                         AbstractionPattern innerOrigType,
                                         CanType innerSubstType,
                                         AbstractionPattern outerOrigType,
                                         CanType outerSubstType) {
  assert(innerOrigType.isTuple());
  assert(!innerOrigType.doesTupleVanish());
  assert(outerOrigType.isTuple());
  assert(!outerOrigType.doesTupleVanish());

  auto &ctx = SGF.getASTContext();
  auto innerPacks = asImpl().getInnerPackGenerator();
  ExpandedTupleInputGenerator innerElt(ctx, innerPacks,
                                       innerOrigType, innerSubstType);
  ExpandedTupleInputGenerator outerElt(ctx, OuterPackArgs,
                                       outerOrigType, outerSubstType);

  for (; !innerElt.isFinished(); innerElt.advance(), outerElt.advance()) {
    assert(!outerElt.isFinished() && "elements not parallel");
    assert(outerElt.isSubstPackExpansion() == innerElt.isSubstPackExpansion());

    // If this substituted component is a pack expansion, handle that
    // immediately.
    if (auto outerSubstExpansionType =
          dyn_cast<PackExpansionType>(outerElt.getSubstType())) {
      ManagedValue outerPackComponent =
        outerElt.projectPackComponent(SGF, Loc);

      auto innerEltSlot = asImpl().getInnerPackExpansionSlot(
        innerElt.getPackValue().getLValueAddress());
      ManagedValue innerPackComponent = asImpl().expandPackExpansion(
                                innerElt.getOrigType(),
        cast<PackExpansionType>(innerElt.getSubstType()),
                                outerElt.getOrigType(),
                                outerSubstExpansionType,
                                innerElt.getFormalPackType(),
                                innerEltSlot,
                                innerElt.getPackComponentIndex(),
                                outerElt.getFormalPackType(),
                                outerPackComponent,
                                outerElt.getPackComponentIndex());

      // Update the cleanups on the inner element.
      innerElt.setPackComponent(SGF, Loc, innerPackComponent);
      continue;
    }

    AbstractionPattern outerOrigEltType = outerElt.getOrigType();
    AbstractionPattern innerOrigEltType = innerElt.getOrigType();
    CanType outerSubstEltType = outerElt.getSubstType();
    CanType innerSubstEltType = innerElt.getSubstType();

    if (outerElt.isOrigPackExpansion()) {
      auto outerEltAddr = outerElt.projectPackComponent(SGF, Loc);
      if (innerElt.isOrigPackExpansion()) {
        auto innerSlot =
          asImpl().getInnerPackElementSlot(innerElt.getPackComponentType());
        auto innerEltAddr =
          asImpl().expandSingleIndirect(innerOrigEltType, innerSubstEltType,
                                        outerOrigEltType, outerSubstEltType,
                                        innerSlot, outerEltAddr);
        innerElt.setPackComponent(SGF, Loc, innerEltAddr);
      } else {
        asImpl().expandOuterIndirect(innerOrigEltType, innerSubstEltType,
                                     outerOrigEltType, outerSubstEltType,
                                     outerEltAddr);
      }
    } else {
      if (innerElt.isOrigPackExpansion()) {
        auto innerSlot =
          asImpl().getInnerPackElementSlot(innerElt.getPackComponentType());
        auto innerEltAddr =
          asImpl().expandInnerIndirect(innerOrigEltType, innerSubstEltType,
                                       outerOrigEltType, outerSubstEltType,
                                       innerSlot);
        innerElt.setPackComponent(SGF, Loc, innerEltAddr);
      } else {
        asImpl().expand(innerOrigEltType, innerSubstEltType,
                        outerOrigEltType, outerSubstEltType);
      }
    }
  }
  assert(outerElt.isFinished() && "elements not parallel");
  outerElt.finish();
  innerElt.finish();
}

static SILType getTupleOrPackElementType(SILType valueType,
                                         unsigned componentIndex) {
  if (auto packType = valueType.getAs<SILPackType>()) {
    return packType->getSILElementType(componentIndex);
  } else {
    return valueType.getTupleElementType(componentIndex);
  }
}

static SILValue emitTupleOrPackElementAddr(SILGenFunction &SGF,
                                           SILLocation loc,
                                           SILValue packIndex,
                                           SILValue tupleOrPackAddr,
                                           SILType eltTy) {
  if (tupleOrPackAddr->getType().is<SILPackType>()) {
    return SGF.B.createPackElementGet(loc, packIndex, tupleOrPackAddr, eltTy);
  } else {
    return SGF.B.createTuplePackElementAddr(loc, packIndex, tupleOrPackAddr,
                                            eltTy);
  }
}

static CleanupHandle
enterPartialDestroyRemainingTupleOrPackCleanup(SILGenFunction &SGF,
                                               SILValue tupleOrPackAddr,
                                               CanPackType formalPackType,
                                               unsigned componentIndex,
                                               SILValue afterIndexWithinComponent) {
  if (tupleOrPackAddr->getType().is<SILPackType>()) {
    return SGF.enterPartialDestroyRemainingPackCleanup(tupleOrPackAddr,
                                                       formalPackType,
                                                       componentIndex,
                                                       afterIndexWithinComponent);
  } else {
    return SGF.enterPartialDestroyRemainingTupleCleanup(tupleOrPackAddr,
                                                        formalPackType,
                                                        componentIndex,
                                                        afterIndexWithinComponent);
  }
}

static CleanupHandle
enterPartialDestroyTupleOrPackCleanup(SILGenFunction &SGF,
                                      SILValue tupleOrPackAddr,
                                      CanPackType formalPackType,
                                      unsigned componentIndex,
                                      SILValue beforeIndexWithinComponent) {
  if (tupleOrPackAddr->getType().is<SILPackType>()) {
    return SGF.enterPartialDestroyPackCleanup(tupleOrPackAddr,
                                              formalPackType,
                                              componentIndex,
                                              beforeIndexWithinComponent);
  } else {
    return SGF.enterPartialDestroyTupleCleanup(tupleOrPackAddr,
                                               formalPackType,
                                               componentIndex,
                                               beforeIndexWithinComponent);
  }
}

/// We have a pack expansion in a substituted result type.  The inner result
/// type is either expanded (in which case the inner slot will have pack type)
/// or not (in which case it will have tuple type).  The inner slot always
/// has an address.
ManagedValue ResultPlanner::expandPackExpansion(
                      AbstractionPattern innerOrigType,
                      CanPackExpansionType innerSubstType,
                      AbstractionPattern outerOrigType,
                      CanPackExpansionType outerSubstType,
                      CanPackType innerFormalPackType,
                      IndirectSlot innerTupleOrPackSlot,
                      unsigned innerComponentIndex,
                      CanPackType outerFormalPackType,
                      ManagedValue outerTupleOrPackAddr,
                      unsigned outerComponentIndex) {
  assert(innerTupleOrPackSlot.hasAddress());
  // The orig and subst types are the pattern types, not the expansion types.

  // If the inner slot is a tuple, we're going to get the whole tuple back;
  // set up an operation to translate it back into the outer type.
  if (innerTupleOrPackSlot.getType().is<TupleType>()) {
    auto innerTupleAddr = innerTupleOrPackSlot.getAddress();
    addReabstractTupleIntoPackExpansion(innerOrigType, innerSubstType,
                                        outerOrigType, outerSubstType,
                                        innerFormalPackType,
                                        innerTupleAddr,
                                        innerComponentIndex,
                                        outerFormalPackType,
                                        outerTupleOrPackAddr.getLValueAddress(),
                                        outerComponentIndex);
    return ManagedValue::forLValue(innerTupleAddr);
  }

  // Otherwise, we need to emit a pack loop to set up the indirect result pack
  // for the callee, then add an operation to reabstract the result back if
  // necessary.
  SILValue innerPackAddr = innerTupleOrPackSlot.getAddress();
  SILType innerPackExpansionTy =
    innerPackAddr->getType().getPackElementType(innerComponentIndex);
  SILType outerPackExpansionTy =
    getTupleOrPackElementType(outerTupleOrPackAddr.getType(),
                              outerComponentIndex);

  SILType innerEltTy, outerEltTy;
  auto openedEnv = SGF.createOpenedElementValueEnvironment(
                   { innerPackExpansionTy, outerPackExpansionTy },
                   { &innerEltTy, &outerEltTy });

  // If the pack elements need reabstraction, we need to collect results
  // into the elements of a temporary tuple and then reabstract after the
  // call.
  SILValue innerTemporaryAddr;
  bool reabstract = hasAbstractionDifference(innerEltTy, outerEltTy);
  if (reabstract) {
    auto innerTemporaryTy =
      SILType::getPrimitiveObjectType(CanType(
        TupleType::get({innerPackExpansionTy.getASTType()},
                       SGF.getASTContext())));
    innerTemporaryAddr = SGF.emitTemporaryAllocation(Loc, innerTemporaryTy);
  }

  // Perform a pack loop to set the element addresses for this pack
  // expansion in the inner pack.
  SGF.emitDynamicPackLoop(Loc, innerFormalPackType, innerComponentIndex,
                          openedEnv,
                          [&](SILValue indexWithinComponent,
                              SILValue packExpansionIndex,
                              SILValue innerPackIndex) {
    SILValue innerEltAddr;
    if (reabstract) {
      innerEltAddr = SGF.B.createTuplePackElementAddr(Loc, packExpansionIndex,
                                                      innerTemporaryAddr,
                                                      innerEltTy);
    } else {
      SILValue outerPackIndex = packExpansionIndex;
      if (outerFormalPackType->getNumElements() != 1) {
        outerPackIndex =
          SGF.B.createPackPackIndex(Loc, outerComponentIndex,
                                    outerPackIndex, outerFormalPackType);
      }

      // Since we're not reabstracting, we can use the outer address
      // directly as the inner address.
      innerEltAddr = emitTupleOrPackElementAddr(SGF, Loc, outerPackIndex,
                                    outerTupleOrPackAddr.getLValueAddress(),
                                                outerEltTy);
    }

    SGF.B.createPackElementSet(Loc, innerEltAddr, innerPackIndex, innerPackAddr);
  });

  if (reabstract) {
    auto innerFormalPackType =
      innerTemporaryAddr->getType().castTo<TupleType>().getInducedPackType();
    unsigned innerComponentIndex = 0;

    addReabstractTupleIntoPackExpansion(innerOrigType, innerSubstType,
                                        outerOrigType, outerSubstType,
                                        innerFormalPackType,
                                        innerTemporaryAddr,
                                        innerComponentIndex,
                                        outerFormalPackType,
                                        outerTupleOrPackAddr.getLValueAddress(),
                                        outerComponentIndex);
  }

  return ManagedValue::forLValue(innerPackAddr);
}

ManagedValue TranslateArguments::expandPackExpansion(
                      AbstractionPattern innerOrigType,
                      CanPackExpansionType innerSubstType,
                      AbstractionPattern outerOrigType,
                      CanPackExpansionType outerSubstType,
                      CanPackType innerFormalPackType,
                      ParamInfo innerTupleOrPackSlot,
                      unsigned innerComponentIndex,
                      CanPackType outerFormalPackType,
                      ManagedValue outerTupleOrPackMV,
                      unsigned outerComponentIndex) {
  assert(innerTupleOrPackSlot.hasAddress());

  bool innerIsTuple = innerTupleOrPackSlot.getType().is<TupleType>();

  SILType innerPackExpansionTy =
    getTupleOrPackElementType(innerTupleOrPackSlot.getType(), innerComponentIndex);
  SILType outerPackExpansionTy =
    getTupleOrPackElementType(outerTupleOrPackMV.getType(), outerComponentIndex);

  SILType innerEltTy, outerEltTy;
  CanType innerSubstEltType, outerSubstEltType;
  auto openedEnv = SGF.createOpenedElementValueEnvironment(
                   { innerPackExpansionTy, outerPackExpansionTy },
                   { &innerEltTy, &outerEltTy },
                   { innerSubstType, outerSubstType },
                   { &innerSubstEltType, &outerSubstEltType });

  auto innerConvention = innerTupleOrPackSlot.getConvention();
  auto innerTupleOrPackAddr = innerTupleOrPackSlot.getAddress();

  // If we're translating into a pack, and the expansion elements need
  // reabstraction, we need to do that into a temporary tuple so that
  // they're in a location that will survive the loop.  Note that we
  // *also* need to do this if we have to copy the elements.  But we never
  // need to do this if we're translating into a tuple because we always
  // copy the elements.
  SILValue innerTemporaryAddr;
  bool needsInnerTemporary;
  if (innerIsTuple) {
    needsInnerTemporary = false;
  } else if (hasAbstractionDifference(innerEltTy, outerEltTy)) {
    needsInnerTemporary = true;
  } else {
    // If the inner parameter is @pack_owned, we can only forward the
    // outer parameter if it's also @pack_owned.
    // Note that we need to force *trivial* packs/tuples to be copied, in
    // case the inner context wants to mutate the memory, even though we might
    // have ownership of that memory (e.g. if it's a consuming parameter).
    needsInnerTemporary = (isConsumedParameterInCaller(innerConvention) &&
                           !outerTupleOrPackMV.isPlusOne(SGF));
  }

  // If we have to reabstract, we need a temporary to hold the
  // reabstracted values.
  if (needsInnerTemporary) {
    auto innerTemporaryTy =
      SILType::getPrimitiveObjectType(CanType(
        TupleType::get({innerPackExpansionTy.getASTType()},
                       SGF.getASTContext())));
    innerTemporaryAddr = SGF.emitTemporaryAllocation(Loc, innerTemporaryTy);

  }

  // outerTupleOrPackMV represents our ownership of this pack-expansion
  // component of the outer pack/tuple.  If we do have ownership of it,
  // and we don't need to consume that ownership, it's fine to leave
  // the cleanup around.  The only case where that's going to be true,
  // though, is when we're not reabstracting and we're generating a
  // borrowed component.  Otherwise we need to claim ownership of the
  // entire component.
  //
  // If we're in that borrowed case, pretend we don't have ownership.
  //
  // This doesn't apply if we're translating into a tuple because we
  // always need to copy/move into the tuple.
  if (!innerIsTuple && !needsInnerTemporary &&
      !isConsumedParameterInCaller(innerConvention)) {
    outerTupleOrPackMV =
      ManagedValue::forBorrowedAddressRValue(outerTupleOrPackMV.getValue());
  }

  // Forward our ownership of the outer component if we still have it.
  CleanupCloner outerCleanupCloner(SGF, outerTupleOrPackMV);
  SILValue outerTupleOrPackAddr = outerTupleOrPackMV.forward(SGF);

  bool innerIsOwned = (innerIsTuple || needsInnerTemporary ||
                       isConsumedParameterInCaller(innerConvention));

  // Perform a pack loop to translate the components and set the element
  // addresses for this pack expansion in the inner pack (if it's a pack).
  //
  // Invariant: if outerTupleOrPackMV.hasCleanup(), we've consumed the
  //   value of the outer pack expansion component for all indices prior
  //   to the current index.  ("Consumption" here might include the trivial
  //   consumption of putting its address in the corresponding inner pack.)
  // Invariant: if innerIsOwned, the inner pack expansion component contains
  //   the address of an owned value for all indices prior to the current
  //   index.
  SGF.emitDynamicPackLoop(Loc, innerFormalPackType, innerComponentIndex,
                          openedEnv,
                          [&](SILValue indexWithinComponent,
                              SILValue packExpansionIndex,
                              SILValue innerPackIndex) {
    // Generate the outer element value.
    SILValue outerPackIndex = packExpansionIndex;
    if (outerFormalPackType->getNumElements() != 1) {
      outerPackIndex =
        SGF.B.createPackPackIndex(Loc, outerComponentIndex,
                                  outerPackIndex, outerFormalPackType);
    }
    SILValue outerEltAddr = emitTupleOrPackElementAddr(SGF, Loc, outerPackIndex,
                                                       outerTupleOrPackAddr,
                                                       outerEltTy);

    // If we're claiming ownership of the elements of the outer pack
    // expansion, we've already done that for all preceding outer elements,
    // but we still have ownership of the remaining elements:

    // Enter a cleanup for the current outer element.
    ManagedValue outerEltMV = outerCleanupCloner.clone(outerEltAddr);

    // Enter a cleanup for the remaining outer elements past the current.
    CleanupHandle outerRemainingEltsCleanup = CleanupHandle::invalid();
    if (outerTupleOrPackMV.hasCleanup()) {
      outerRemainingEltsCleanup =
        enterPartialDestroyRemainingTupleOrPackCleanup(SGF, outerTupleOrPackAddr,
                                                       outerFormalPackType,
                                                       outerComponentIndex,
                                                       indexWithinComponent);
    }

    // If we're generating owned values, enter a cleanup for the elements
    // we generated in previous loop iterations.
    CleanupHandle innerPreviousEltsCleanup = CleanupHandle::invalid();
    if (innerIsOwned) {
      innerPreviousEltsCleanup =
        enterPartialDestroyTupleOrPackCleanup(SGF, innerTupleOrPackAddr,
                                              innerFormalPackType,
                                              innerComponentIndex,
                                              indexWithinComponent);
    }

    // Project out the destination address.
    SILValue innerEltAddr;
    if (innerIsTuple) {
      innerEltAddr = SGF.B.createTuplePackElementAddr(Loc, packExpansionIndex,
                                                      innerTupleOrPackAddr,
                                                      innerEltTy);
    } else if (needsInnerTemporary) {
      innerEltAddr = SGF.B.createTuplePackElementAddr(Loc, packExpansionIndex,
                                                      innerTemporaryAddr,
                                                      innerEltTy);
    } else {
      innerEltAddr = outerEltAddr;
    }

    // Translate the outer into the destination address.
    if (innerIsTuple || needsInnerTemporary) {
      auto innerEltSlot =
        ParamInfo(innerEltAddr, ParameterConvention::Indirect_In);
      ManagedValue innerEltMV =
        expandSingleIndirect(innerOrigType.getPackExpansionPatternType(),
                             innerSubstEltType,
                             outerOrigType.getPackExpansionPatternType(),
                             outerSubstEltType,
                             innerEltSlot, outerEltMV);
      assert(innerEltMV.getValue() == innerEltAddr);
      assert(innerEltMV.isPlusOneOrTrivial(SGF));

      // Deactivate the cleanup for the inner element.
      (void) innerEltMV.forward(SGF);
    }

    // Set the destination address into the inner pack, if applicable.
    if (!innerIsTuple) {
      SGF.B.createPackElementSet(Loc, innerEltAddr, innerPackIndex,
                                 innerTupleOrPackAddr);
    }

    // Deactivate the previous-inner and remaining-outer cleanups that
    // we set up above.
    if (innerPreviousEltsCleanup.isValid())
      SGF.Cleanups.forwardCleanup(innerPreviousEltsCleanup);
    if (outerRemainingEltsCleanup.isValid())
      SGF.Cleanups.forwardCleanup(outerRemainingEltsCleanup);

    // Note that we leave the current outer cleanup alive if the translation
    // didn't consume it; emitDynamicPackLoop will emit it when it loops back.
  });

  // If the inner elements are owned, we need to enter a cleanup for them.
  if (innerIsOwned) {
    auto innerExpansionCleanup =
      enterPartialDestroyTupleOrPackCleanup(SGF, innerTupleOrPackAddr,
                                            innerFormalPackType,
                                            innerComponentIndex,
                                            /*entire component*/ SILValue());

    // We only associate this cleanup with what we return from this function
    // if we're generating an owned value; otherwise we just leave it active
    // so that we destroy the values later.
    if (isConsumedParameterInCaller(innerConvention)) {
      return ManagedValue::forOwnedAddressRValue(innerTupleOrPackAddr,
                                                 innerExpansionCleanup);
    }
  }

  return ManagedValue::forBorrowedAddressRValue(innerTupleOrPackAddr);
}

void ResultPlanner::Operation::emitReabstractTupleIntoPackExpansion(
       SILGenFunction &SGF, SILLocation loc) {
  SILValue innerTupleAddr = InnerResultAddr;
  SILValue outerTupleOrPackAddr = OuterResultAddr;
  unsigned innerComponentIndex = PackExpansion.InnerComponentIndex;
  unsigned outerComponentIndex = PackExpansion.OuterComponentIndex;

  SILType innerPackExpansionTy =
    innerTupleAddr->getType().getTupleElementType(innerComponentIndex);
  SILType outerPackExpansionTy =
    getTupleOrPackElementType(outerTupleOrPackAddr->getType(),
                              outerComponentIndex);

  SILType innerEltTy, outerEltTy;
  CanType innerSubstEltType, outerSubstEltType;
  auto openedEnv = SGF.createOpenedElementValueEnvironment(
                   { innerPackExpansionTy, outerPackExpansionTy },
                   { &innerEltTy, &outerEltTy },
                   { InnerSubstType, OuterSubstType },
                   { &innerSubstEltType, &outerSubstEltType });

  auto innerFormalPackType = PackExpansion.InnerFormalPackType;
  auto outerFormalPackType = PackExpansion.OuterFormalPackType;

  SGF.emitDynamicPackLoop(loc, innerFormalPackType, innerComponentIndex,
                          openedEnv,
                          [&](SILValue indexWithinComponent,
                              SILValue packExpansionIndex,
                              SILValue innerPackIndex) {
    // Construct a managed value for the inner element, loading it
    // if appropriate.  We assume the result is owned.
    auto innerEltAddr =
      SGF.B.createTuplePackElementAddr(loc, innerPackIndex, innerTupleAddr,
                                       innerEltTy);
    auto innerEltValue =
      SGF.emitManagedBufferWithCleanup(innerEltAddr);
    innerEltValue = SGF.B.createLoadIfLoadable(loc, innerEltValue);

    // Project the address of the outer element.
    auto outerPackIndex = packExpansionIndex;
    if (outerFormalPackType->getNumElements() != 1) {
      outerPackIndex = SGF.B.createPackPackIndex(loc, outerComponentIndex,
                                                 outerPackIndex,
                                                 outerFormalPackType);
    }
    auto outerEltAddr =
      emitTupleOrPackElementAddr(SGF, loc, outerPackIndex,
                                 outerTupleOrPackAddr, outerEltTy);

    // Set up to perform the reabstraction into the outer result.
    TemporaryInitialization outerEltInit(outerEltAddr,
                                         CleanupHandle::invalid());
    auto outerResultCtxt = SGFContext(&outerEltInit);

    // Reabstract.
    auto outerEltValue =
      SGF.emitTransformedValue(loc, innerEltValue,
                               InnerOrigType, innerSubstEltType,
                               OuterOrigType, outerSubstEltType,
                               outerEltTy, outerResultCtxt);

    // Force the value into the outer result address if necessary.
    if (!outerEltValue.isInContext()) {
      outerEltValue.forwardInto(SGF, loc, outerEltAddr);
    }
  });
}

void ResultPlanner::expandOuterTuple(AbstractionPattern innerOrigType,
                                     CanType innerSubstType,
                                     AbstractionPattern outerOrigType,
                                     CanTupleType outerSubstType) {
  assert(outerOrigType.isTuple());
  assert(!outerOrigType.doesTupleVanish());
  assert(!innerOrigType.isTuple());

  // We know that the outer tuple is not vanishing (because the top-level
  // plan function filters that out), so the outer subst type must be a
  // tuple.  The inner subst type must also be a tuple because only tuples
  // convert to tuples.
  auto innerSubstTupleType = cast<TupleType>(innerSubstType);

  // The next inner result is not expanded, so there's a single result.
  SILResultInfo innerResult = claimNextInnerResult();

  if (SGF.silConv.isSILIndirect(innerResult)) {
    SILValue innerResultAddr = addInnerIndirectResultTemporary(innerResult);
    auto innerResultMV =
      expandParallelTuplesInnerIndirect(innerOrigType, innerSubstTupleType,
                                        outerOrigType, outerSubstType,
                                        innerResultAddr);
    assert(innerResultMV.getValue() == innerResultAddr);
    (void) innerResultMV;
  } else {
    assert(!SGF.silConv.useLoweredAddresses() &&
           "Formal Indirect Results that are not SIL Indirect are only "
           "allowed in opaque values mode");
    planExpandedFromDirect(innerOrigType, innerSubstTupleType,
                           outerOrigType, outerSubstType,
                           innerResult);
  }
}

void ResultPlanner::expandInnerTuple(AbstractionPattern innerOrigType,
                                     CanTupleType innerSubstType,
                                     AbstractionPattern outerOrigType,
                                     CanType outerSubstType) {
  assert(innerOrigType.isTuple());
  assert(!innerOrigType.doesTupleVanish());
  assert(!outerOrigType.isTuple());

  // The outer subst type might not be a tuple if it's e.g. Any.

  // The next outer result is not expanded, so there's a single result.
  auto outerResultPair = claimNextOuterResult();
  SILResultInfo outerResult = outerResultPair.first;
  SILValue outerResultAddr = outerResultPair.second;

  // Base the plan on whether the single result is direct or indirect.
  if (SGF.silConv.isSILIndirect(outerResult)) {
    assert(outerResultAddr);
    expandInnerTupleOuterIndirect(innerOrigType, innerSubstType,
                                  outerOrigType, outerSubstType,
                                  ManagedValue::forLValue(outerResultAddr));
  } else {
    assert(!outerResultAddr);
    planExpandedIntoDirect(innerOrigType, innerSubstType,
                           outerOrigType, outerSubstType,
                           outerResult);
  }
}

void ResultPlanner::expandSingle(AbstractionPattern innerOrigType,
                                 CanType innerSubstType,
                                 AbstractionPattern outerOrigType,
                                 CanType outerSubstType) {
  SILResultInfo innerResult = claimNextInnerResult();
  auto outerResultPair = claimNextOuterResult();
  SILResultInfo outerResult = outerResultPair.first;
  SILValue outerResultAddr = outerResultPair.second;
  planSingle(innerOrigType, innerSubstType,
             outerOrigType, outerSubstType,
             innerResult, outerResult, outerResultAddr);
}

void ResultPlanner::planSingle(AbstractionPattern innerOrigType,
                               CanType innerSubstType,
                               AbstractionPattern outerOrigType,
                               CanType outerSubstType,
                               SILResultInfo innerResult,
                               SILResultInfo outerResult,
                               SILValue optOuterResultAddr) {
  // If the outer result is indirect, plan to emit into that.
  if (SGF.silConv.isSILIndirect(outerResult)) {
    assert(optOuterResultAddr);
    planSingleIntoIndirect(innerOrigType, innerSubstType,
                           outerOrigType, outerSubstType,
                           innerResult, optOuterResultAddr);

  } else {
    assert(!optOuterResultAddr);
    planSingleIntoDirect(innerOrigType, innerSubstType,
                         outerOrigType, outerSubstType,
                         innerResult, outerResult);
  }
}

/// Plan the emission of a call result into an outer result address.
template <class Impl, class InnerSlotType>
void ExpanderBase<Impl, InnerSlotType>::expandOuterIndirect(
                                        AbstractionPattern innerOrigType,
                                        CanType innerSubstType,
                                        AbstractionPattern outerOrigType,
                                        CanType outerSubstType,
                                        ManagedValue outerAddr) {
  // outerOrigType can be a tuple if we're e.g. injecting into an optional;
  // we just know we're not expanding it.

  // If the inner pattern is not a tuple, there's no more recursive
  // expansion; translate the single value.
  if (!innerOrigType.isTuple()) {
    asImpl().expandSingleOuterIndirect(innerOrigType, innerSubstType,
                                       outerOrigType, outerSubstType,
                                       outerAddr);
    return;
  }

  // If the inner pattern is not vanishing, expand the tuple.
  if (!innerOrigType.doesTupleVanish()) {
    asImpl().expandInnerTupleOuterIndirect(innerOrigType,
                                           cast<TupleType>(innerSubstType),
                                           outerOrigType, outerSubstType,
                                           outerAddr);
    return;
  }

  // Otherwise, we have a vanishing tuple.  Expand it, find the surviving
  // element, and recurse.
  asImpl().expandInnerVanishingTuple(innerOrigType, innerSubstType,
     [&](AbstractionPattern innerOrigEltType, CanType innerSubstEltType) {
    asImpl().expandOuterIndirect(innerOrigEltType, innerSubstEltType,
                                 outerOrigType, outerSubstType,
                                 outerAddr);
  }, [&](AbstractionPattern innerOrigEltType,
         CanType innerSubstEltType, SILType innerEltTy) {
    auto innerSlot = asImpl().getInnerPackElementSlot(innerEltTy);
    return asImpl().expandSingleIndirect(innerOrigEltType,
                                         innerSubstEltType,
                                         outerOrigType,
                                         outerSubstType,
                                         innerSlot,
                                         outerAddr);
  });
}

void ResultPlanner::expandSingleOuterIndirect(
                                        AbstractionPattern innerOrigType,
                                        CanType innerSubstType,
                                        AbstractionPattern outerOrigType,
                                        CanType outerSubstType,
                                        ManagedValue outerResultAddr) {
  // Claim the next inner result.
  SILResultInfo innerResult = claimNextInnerResult();

  planSingleIntoIndirect(innerOrigType, innerSubstType,
                         outerOrigType, outerSubstType,
                         innerResult, outerResultAddr.getLValueAddress());
}

/// Plan the emission of a call result into an outer result address,
/// given that the inner abstraction pattern is a tuple.
void
ResultPlanner::expandInnerTupleOuterIndirect(AbstractionPattern innerOrigType,
                                             CanTupleType innerSubstType,
                                             AbstractionPattern outerOrigType,
                                             CanType outerSubstType,
                                             ManagedValue outerResultAddrMV) {
  assert(innerOrigType.isTuple());
  assert(!innerOrigType.doesTupleVanish());
  // outerOrigType can be a tuple if we're doing something like
  // injecting into an optional tuple.

  auto outerSubstTupleType = dyn_cast<TupleType>(outerSubstType);

  // If the outer type is not a tuple, it must be optional.
  if (!outerSubstTupleType) {
    auto outerResultAddr = outerResultAddrMV.getLValueAddress();

    // Figure out what kind of optional it is.
    CanType outerSubstObjectType = outerSubstType.getOptionalObjectType();
    if (outerSubstObjectType) {
      auto someDecl = SGF.getASTContext().getOptionalSomeDecl();

      // Prepare the value slot in the optional value.
      SILType outerObjectType =
          outerResultAddr->getType().getOptionalObjectType();
      SILValue outerObjectResultAddr
        = SGF.B.createInitEnumDataAddr(Loc, outerResultAddr,
                                       someDecl, outerObjectType);

      // Emit into that address.
      expandInnerTupleOuterIndirect(
          innerOrigType, innerSubstType, outerOrigType.getOptionalObjectType(),
          outerSubstObjectType, ManagedValue::forLValue(outerObjectResultAddr));

      // Add an operation to finish the enum initialization.
      addInjectOptionalIndirect(someDecl, outerResultAddr);
      return;
    }

    auto conformances = SGF.SGM.M.getSwiftModule()->collectExistentialConformances(
        innerSubstType, outerSubstType);

    // Prepare the value slot in the existential.
    auto opaque = AbstractionPattern::getOpaque();
    SILValue outerConcreteResultAddr
      = SGF.B.createInitExistentialAddr(Loc, outerResultAddr, innerSubstType,
                                        SGF.getLoweredType(opaque, innerSubstType),
                                        conformances);

    // Emit into that address.
    expandInnerTupleOuterIndirect(innerOrigType, innerSubstType,
                                  innerOrigType, innerSubstType,
                                  ManagedValue::forLValue(outerConcreteResultAddr));
    return;
  }

  // Otherwise, we're doing a tuple-to-tuple conversion, which is a
  // parallel walk.
  expandParallelTuplesOuterIndirect(innerOrigType, innerSubstType,
                                    outerOrigType, outerSubstTupleType,
                                    outerResultAddrMV);
}

template <class Impl, class InnerSlotType>
void ExpanderBase<Impl, InnerSlotType>::expandParallelTuplesOuterIndirect(
                                             AbstractionPattern innerOrigType,
                                             CanTupleType innerSubstType,
                                             AbstractionPattern outerOrigType,
                                             CanTupleType outerSubstType,
                                             ManagedValue outerTupleAddr) {
  assert(innerOrigType.isTuple());
  assert(!innerOrigType.doesTupleVanish());

  auto &ctx = SGF.getASTContext();
  auto innerPacks = asImpl().getInnerPackGenerator();
  ExpandedTupleInputGenerator innerElt(ctx, innerPacks,
                                       innerOrigType, innerSubstType);
  TupleElementAddressGenerator outerElt(ctx,
                                        outerTupleAddr,
                                        outerOrigType,
                                        outerSubstType);

  for (; !outerElt.isFinished(); outerElt.advance(), innerElt.advance()) {
    assert(!innerElt.isFinished());
    assert(innerElt.isSubstPackExpansion() == outerElt.isSubstPackExpansion());

    ManagedValue outerEltAddr = outerElt.projectElementAddress(SGF, Loc);

    if (!innerElt.isOrigPackExpansion()) {
      asImpl().expandOuterIndirect(innerElt.getOrigType(),
                                   innerElt.getSubstType(),
                                   outerElt.getOrigType(),
                                   outerElt.getSubstType(),
                                   outerEltAddr);
      continue;
    }

    if (!innerElt.isSubstPackExpansion()) {
      auto innerSlot =
        asImpl().getInnerPackElementSlot(innerElt.getPackComponentType());
      ManagedValue innerEltAddr =
        asImpl().expandSingleIndirect(innerElt.getOrigType(),
                                      innerElt.getSubstType(),
                                      outerElt.getOrigType(),
                                      outerElt.getSubstType(),
                                      innerSlot,
                                      outerEltAddr);
      innerElt.setPackComponent(SGF, Loc, innerEltAddr);
      continue;
    }

    auto innerExpansionSlot = asImpl().getInnerPackExpansionSlot(
      innerElt.getPackValue().getLValueAddress());
    asImpl().expandPackExpansion(innerElt.getOrigType(),
         cast<PackExpansionType>(innerElt.getSubstType()),
                                 outerElt.getOrigType(),
         cast<PackExpansionType>(outerElt.getSubstType()),
                                 innerElt.getFormalPackType(),
                                 innerExpansionSlot,
                                 innerElt.getPackComponentIndex(),
                                 outerElt.getInducedPackType(),
                                 outerEltAddr,
                                 outerElt.getSubstElementIndex());
  }
  innerElt.finish();
  outerElt.finish();
}

/// Plan the emission of a call result as a single outer direct result.
void ResultPlanner::planIntoDirect(AbstractionPattern innerOrigType,
                                   CanType innerSubstType,
                                   AbstractionPattern outerOrigType,
                                   CanType outerSubstType,
                                   SILResultInfo outerResult) {
  assert(!outerOrigType.isTuple() || !SGF.silConv.useLoweredAddresses());

  // If the inner pattern is scalar, claim the next inner result.
  if (!innerOrigType.isTuple()) {
    SILResultInfo innerResult = claimNextInnerResult();

    planSingleIntoDirect(innerOrigType, innerSubstType,
                         outerOrigType, outerSubstType,
                         innerResult, outerResult);
    return;
  }

  // If the inner pattern doesn't vanish, expand it.
  if (!innerOrigType.doesTupleVanish()) {
    planExpandedIntoDirect(innerOrigType, cast<TupleType>(innerSubstType),
                           outerOrigType, outerSubstType,
                           outerResult);
    return;
  }

  // Otherwise, the inner tuple vanishes.  Expand it, find the surviving
  // element, and recurse.
  expandInnerVanishingTuple(innerOrigType, innerSubstType,
     [&](AbstractionPattern innerOrigEltType, CanType innerSubstEltType) {
    planIntoDirect(innerOrigEltType, innerSubstEltType,
                   outerOrigType, outerSubstType,
                   outerResult);
  }, [&](AbstractionPattern innerOrigEltType,
         CanType innerSubstEltType, SILType innerEltTy) {
    auto innerTemp =
      planSingleFromIndirect(innerOrigEltType, innerSubstEltType,
                             outerOrigType, outerSubstType,
                             IndirectSlot(innerEltTy),
                             outerResult, SILValue());
    return ManagedValue::forLValue(innerTemp);
  });
}

/// Plan the emission of a call result as a single outer direct result,
/// given that the inner abstraction pattern is a tuple.
void ResultPlanner::planExpandedIntoDirect(AbstractionPattern innerOrigType,
                                           CanTupleType innerSubstType,
                                           AbstractionPattern outerOrigType,
                                           CanType outerSubstType,
                                           SILResultInfo outerResult) {
  assert(innerOrigType.isTuple());
  assert(!innerOrigType.doesTupleVanish());

  auto outerSubstTupleType = dyn_cast<TupleType>(outerSubstType);

  // If the outer type is not a tuple, it must be optional or we are under
  // opaque value mode
  if (!outerSubstTupleType) {
    CanType outerSubstObjectType = outerSubstType.getOptionalObjectType();

    if (outerSubstObjectType) {
      auto someDecl = SGF.getASTContext().getOptionalSomeDecl();
      SILType outerObjectType =
          SGF.getSILType(outerResult, CanSILFunctionType())
             .getOptionalObjectType();
      SILResultInfo outerObjectResult(outerObjectType.getASTType(),
                                      outerResult.getConvention());

      // Plan to leave the tuple elements as a single direct outer result.
      planExpandedIntoDirect(
          innerOrigType, innerSubstType, outerOrigType.getOptionalObjectType(),
          outerSubstObjectType, outerObjectResult);

      // Take that result and inject it into an optional.
      addInjectOptionalDirect(someDecl, outerResult);
      return;
    } else {
      assert(!SGF.silConv.useLoweredAddresses() &&
             "inner type was a tuple but outer type was neither a tuple nor "
             "optional nor are we under opaque value mode");
      assert(outerSubstType->isAny());

      auto *module = SGF.getModule().getSwiftModule();

      auto opaque = AbstractionPattern::getOpaque();
      auto anyType = SGF.getLoweredType(opaque, outerSubstType);
      auto outerResultAddr = SGF.emitTemporaryAllocation(Loc, anyType);
      auto conformances =
        module->collectExistentialConformances(innerSubstType, outerSubstType);

      SILValue outerConcreteResultAddr = SGF.B.createInitExistentialAddr(
          Loc, outerResultAddr, innerSubstType,
          SGF.getLoweredType(opaque, innerSubstType), conformances);

      expandInnerTupleOuterIndirect(innerOrigType, innerSubstType,
                                    innerOrigType, innerSubstType,
                                    ManagedValue::forLValue(
                                      outerConcreteResultAddr));

      addReabstractIndirectToDirect(innerOrigType, innerSubstType,
                                    outerOrigType, outerSubstType,
                                    outerConcreteResultAddr, outerResult);
      return;
    }
  }

  // Otherwise, the outer type is a tuple with parallel structure
  // to the inner type.
  assert(innerSubstType->getNumElements()
           == outerSubstTupleType->getNumElements());

  auto outerTupleTy = SGF.getSILType(outerResult, CanSILFunctionType());

  // If the substituted tuples contain pack expansions, we need to
  // use the indirect path for the tuple and then add a load operation,
  // because we can't do pack loops on scalar tuples in SIL.
  if (innerSubstType->containsPackExpansionType()) {
    auto temporary =
      SGF.emitTemporaryAllocation(Loc, outerTupleTy.getObjectType());
    expandInnerTupleOuterIndirect(innerOrigType, innerSubstType,
                                  outerOrigType, outerSubstType,
                                  ManagedValue::forLValue(temporary));
    addIndirectToDirect(temporary, outerResult);
    return;
  }

  // Expand the inner tuple, producing direct outer results for each
  // of the elements.
  InnerPackResultGenerator innerPacks(*this);
  ExpandedTupleInputGenerator innerElt(SGF.getASTContext(), innerPacks,
                                       innerOrigType, innerSubstType);
  outerOrigType.forEachExpandedTupleElement(outerSubstType,
    [&](AbstractionPattern outerOrigEltType,
        CanType outerSubstEltType,
        const TupleTypeElt &outerOrigTupleElt) {
    assert(!innerElt.isFinished());
    auto eltIndex = innerElt.getSubstElementIndex();
    auto outerEltTy = outerTupleTy.getTupleElementType(eltIndex);
    SILResultInfo outerEltResult(outerEltTy.getASTType(),
                                 outerResult.getConvention());

    // If the inner element is part of an orig pack expansion, it's
    // indirect; create a temporary for it and reabstract that back to
    // a direct result.
    if (innerElt.isOrigPackExpansion()) {
      auto innerEltAddr =
        planSingleFromIndirect(innerElt.getOrigType(), innerElt.getSubstType(),
                               outerOrigEltType, outerSubstEltType,
                               IndirectSlot(innerElt.getPackComponentType()),
                               outerEltResult, SILValue());
      innerElt.setPackComponent(SGF, Loc,
                                ManagedValue::forLValue(innerEltAddr));

    // Otherwise, recurse normally.
    } else {
      planIntoDirect(innerElt.getOrigType(), innerElt.getSubstType(),
                     outerOrigEltType, outerSubstEltType,
                     outerEltResult);
    }
    innerElt.advance();
  });
  innerElt.finish();

  // Bind those direct results together into a single tuple.
  addTupleDirect(innerSubstType->getNumElements(), outerResult);
}

/// Plan the emission of a call result as a single outer direct result,
/// given that the inner abstraction pattern is not a tuple.
void ResultPlanner::planSingleIntoDirect(AbstractionPattern innerOrigType,
                                         CanType innerSubstType,
                                         AbstractionPattern outerOrigType,
                                         CanType outerSubstType,
                                         SILResultInfo innerResult,
                                         SILResultInfo outerResult) {
  // If the inner result is indirect, plan to emit from that.
  if (SGF.silConv.isSILIndirect(innerResult)) {
    SILValue innerResultAddr =
      addInnerIndirectResultTemporary(innerResult);
    auto innerResultValue =
      planSingleFromIndirect(innerOrigType, innerSubstType,
                             outerOrigType, outerSubstType,
                             innerResultAddr, outerResult, SILValue());
    assert(innerResultValue == innerResultAddr); (void) innerResultValue;
    return;
  }

  // Otherwise, we have two direct results.

  // If there's no abstraction difference, it's just returned directly.
  if (SGF.getSILType(innerResult, CanSILFunctionType())
          == SGF.getSILType(outerResult, CanSILFunctionType())) {
    addDirectToDirect(innerResult, outerResult);

  // Otherwise, we need to reabstract.
  } else {
    addReabstractDirectToDirect(innerOrigType, innerSubstType,
                                outerOrigType, outerSubstType,
                                innerResult, outerResult);
  }
}

/// Plan the emission of a call result into an outer result address,
/// given that the inner abstraction pattern is not a tuple.
void
ResultPlanner::planSingleIntoIndirect(AbstractionPattern innerOrigType,
                                      CanType innerSubstType,
                                      AbstractionPattern outerOrigType,
                                      CanType outerSubstType,
                                      SILResultInfo innerResult,
                                      SILValue outerResultAddr) {
  // innerOrigType and outerOrigType can be tuple patterns; we just
  // know they're not expanded in this position.

  // If the inner result is indirect, we need some memory to emit it into.
  if (SGF.silConv.isSILIndirect(innerResult)) {
    // If there's no abstraction difference, that can just be
    // in-place into the outer result address.
    if (!hasAbstractionDifference(outerResultAddr, innerResult)) {
      addInPlace(outerResultAddr);

    // Otherwise, we'll need a temporary.
    } else {
      SILValue innerResultAddr =
        addInnerIndirectResultTemporary(innerResult);
      addReabstractIndirectToIndirect(innerOrigType, innerSubstType,
                                      outerOrigType, outerSubstType,
                                      innerResultAddr, outerResultAddr);
    }

  // Otherwise, the inner result is direct.
  } else {
    // If there's no abstraction difference, we just need to store.
    if (!hasAbstractionDifference(outerResultAddr, innerResult)) {
      addDirectToIndirect(innerResult, outerResultAddr);

    // Otherwise, we need to reabstract and store.
    } else {
      addReabstractDirectToIndirect(innerOrigType, innerSubstType,
                                    outerOrigType, outerSubstType,
                                    innerResult, outerResultAddr);      
    }
  }
}

/// Plan the emission of a call result from an inner result address.
template <class Impl, class InnerSlotType>
ManagedValue ExpanderBase<Impl, InnerSlotType>::expandInnerIndirect(
                                        AbstractionPattern innerOrigType,
                                        CanType innerSubstType,
                                        AbstractionPattern outerOrigType,
                                        CanType outerSubstType,
                                        InnerSlotType innerSlot) {
  // If the outer pattern is scalar, stop expansion and delegate to the
  // impl class.
  if (!outerOrigType.isTuple()) {
    return asImpl().expandSingleInnerIndirect(innerOrigType, innerSubstType,
                                              outerOrigType, outerSubstType,
                                              innerSlot);
  }

  // If the outer pattern is a non-vanishing tuple, we have to expand it.
  if (!outerOrigType.doesTupleVanish()) {
    return asImpl().expandOuterTupleInnerIndirect(innerOrigType,
                                                  innerSubstType,
                                                  outerOrigType,
                                  cast<TupleType>(outerSubstType),
                                                  innerSlot);
  }

  // Otherwise, the outer pattern is a vanishing tuple.  Expand it,
  // find the surviving element, and recurse.
  ManagedValue innerAddr;
  asImpl().expandOuterVanishingTuple(outerOrigType, outerSubstType,
     [&](AbstractionPattern outerOrigEltType, CanType outerSubstEltType) {
    innerAddr =
      asImpl().expandInnerIndirect(innerOrigType, innerSubstType,
                                   outerOrigEltType, outerSubstEltType,
                                   innerSlot);
  }, [&](AbstractionPattern outerOrigEltType, CanType outerSubstEltType,
         ManagedValue outerAddr) {
    innerAddr =
      asImpl().expandSingleIndirect(innerOrigType, innerSubstType,
                                    outerOrigEltType, outerSubstEltType,
                                    innerSlot, outerAddr);
  });
  return innerAddr;
}

ManagedValue ResultPlanner::expandOuterTupleInnerIndirect(
                                            AbstractionPattern innerOrigType,
                                            CanType innerSubstType,
                                            AbstractionPattern outerOrigType,
                                            CanTupleType outerSubstType,
                                            IndirectSlot innerResultSlot) {
  assert(outerOrigType.isTuple());
  assert(!outerOrigType.doesTupleVanish());

  // In results, if the outer substituted type is a tuple, the inner
  // type must be a tuple, so we must have parallel tuple structure.
  return expandParallelTuplesInnerIndirect(innerOrigType,
                                           cast<TupleType>(innerSubstType),
                                           outerOrigType,
                                           outerSubstType,
                                           innerResultSlot);
}

/// Expand outer tuple structure, given that the inner substituted type
/// is a tuple with parallel structured that's passed indirectly.
template <class Impl, class InnerSlotType>
ManagedValue
ExpanderBase<Impl, InnerSlotType>::expandParallelTuplesInnerIndirect(
                                             AbstractionPattern innerOrigType,
                                             CanTupleType innerSubstType,
                                             AbstractionPattern outerOrigType,
                                             CanTupleType outerSubstType,
                                             InnerSlotType innerTupleSlot) {
  assert(innerSubstType->getNumElements() == outerSubstType->getNumElements());
  assert(outerOrigType.isTuple());
  assert(!outerOrigType.doesTupleVanish());

  SILValue innerTupleAddr = innerTupleSlot.allocate(SGF, Loc);

  auto &ctx = SGF.getASTContext();
  ExpandedTupleInputGenerator
    outerElt(ctx, OuterPackArgs, outerOrigType, outerSubstType);
  TupleElementAddressGenerator
    innerElt(ctx, ManagedValue::forLValue(innerTupleAddr),
             innerOrigType, innerSubstType);
  typename Impl::IndirectTupleExpansionCombiner eltExpansion(asImpl());
  for (; !innerElt.isFinished(); innerElt.advance(), outerElt.advance()) {
    assert(!outerElt.isFinished());

    // Project the address of the element.
    SILValue innerEltAddr =
      innerElt.projectElementAddress(SGF, Loc).getLValueAddress();
    InnerSlotType innerEltSlot = eltExpansion.getElementSlot(innerEltAddr);

    // If the outer element does not come from a pack, we just recurse.
    if (!outerElt.isOrigPackExpansion()) {
      auto innerEltMV =
        asImpl().expandInnerIndirect(innerElt.getOrigType(),
                                     innerElt.getSubstType(),
                                     outerElt.getOrigType(),
                                     outerElt.getSubstType(),
                                     innerEltSlot);

      assert(innerEltMV.getValue() == innerEltAddr);
      eltExpansion.collectElement(innerEltMV);
      continue;
    }

    // Otherwise, we're going to have an indirect outer element value.
    ManagedValue outerEltAddr = outerElt.projectPackComponent(SGF, Loc);

    if (auto outerSubstExpansionType =
          dyn_cast<PackExpansionType>(outerElt.getSubstType())) {
      auto innerExpansionMV =
        asImpl().expandPackExpansion(innerElt.getOrigType(),
             cast<PackExpansionType>(innerElt.getSubstType()),
                                     outerElt.getOrigType(),
                                     outerSubstExpansionType,
                                     innerElt.getInducedPackType(),
                                     innerEltSlot,
                                     innerElt.getSubstElementIndex(),
                                     outerElt.getFormalPackType(),
                                     outerEltAddr,
                                     outerElt.getPackComponentIndex());
      assert(innerExpansionMV.getValue() == innerEltAddr);
      eltExpansion.collectElement(innerExpansionMV);
      continue;
    }

    auto innerEltMV =
      asImpl().expandSingleIndirect(innerElt.getOrigType(),
                                    innerElt.getSubstType(),
                                    outerElt.getOrigType(),
                                    outerElt.getSubstType(),
                                    innerEltSlot,
                                    outerEltAddr);

    assert(innerEltMV.getValue() == innerEltAddr);
    eltExpansion.collectElement(innerEltMV);
  }
  innerElt.finish();
  outerElt.finish();

  // Construct a managed value for the whole tuple.
  return eltExpansion.finish(innerTupleAddr, innerTupleSlot);
}

void ResultPlanner::planExpandedFromDirect(AbstractionPattern innerOrigType,
                                           CanTupleType innerSubstType,
                                           AbstractionPattern outerOrigType,
                                           CanTupleType outerSubstType,
                                           SILResultInfo innerResult) {

  assert(outerOrigType.isTuple());
  assert(!outerOrigType.doesTupleVanish());
  assert(innerSubstType->getNumElements() == outerSubstType->getNumElements());

  SILType innerResultTy = SGF.getSILType(innerResult, CanSILFunctionType());

  // If the substituted tuples contain pack expansions, we need to
  // store the direct type to a temporary and then plan as if the
  // result was indirect, because we can't do pack loops in SIL on
  // scalar tuples.
  assert(innerSubstType->containsPackExpansionType() ==
           outerSubstType->containsPackExpansionType());
  if (innerSubstType->containsPackExpansionType()) {
    auto innerResultAddr =
      expandParallelTuplesInnerIndirect(innerOrigType, innerSubstType,
                                        outerOrigType, outerSubstType,
                                        IndirectSlot(innerResultTy));
    addDirectToIndirect(innerResult, innerResultAddr.getLValueAddress());
    return;
  }

  // Split the inner tuple value into its elements.
  addDestructureDirectInnerTuple(innerResult);

  // Expand the outer tuple and recurse.
  ExpandedTupleInputGenerator
    outerElt(SGF.getASTContext(), OuterPackArgs, outerOrigType, outerSubstType);
  innerOrigType.forEachExpandedTupleElement(innerSubstType,
      [&](AbstractionPattern innerOrigEltType,
          CanType innerSubstEltType,
          const TupleTypeElt &innerOrigTupleElt) {
    SILType innerEltTy =
      innerResultTy.getTupleElementType(outerElt.getSubstElementIndex());
    SILResultInfo innerEltResult(innerEltTy.getASTType(),
                                 innerResult.getConvention());

    // If the outer element comes from an orig pack expansion, it's
    // always indirect.
    if (outerElt.isOrigPackExpansion()) {
      auto outerEltAddr =
        outerElt.projectPackComponent(SGF, Loc).getLValueAddress();
      planSingleIntoIndirect(innerOrigEltType, innerSubstEltType,
                             outerElt.getOrigType(), outerElt.getSubstType(),
                             innerEltResult, outerEltAddr);

    // Otherwise, we need to recurse.
    } else {
      planFromDirect(innerOrigEltType, innerSubstEltType,
                     outerElt.getOrigType(), outerElt.getSubstType(),
                     innerEltResult);
    }
    outerElt.advance();
  });
  outerElt.finish();
}

void ResultPlanner::planFromDirect(AbstractionPattern innerOrigType,
                                   CanType innerSubstType,
                                   AbstractionPattern outerOrigType,
                                   CanType outerSubstType,
                                   SILResultInfo innerResult) {
  // If the outer type isn't a tuple, it's a single result.
  if (!outerOrigType.isTuple()) {
    auto outerResult = claimNextOuterResult();
    planSingle(innerOrigType, innerSubstType,
               outerOrigType, outerSubstType,
               innerResult, outerResult.first, outerResult.second);
    return;
  }

  // If the outer tuple doesn't vanish, then the inner substituted type
  // must have parallel tuple structure.
  if (!outerOrigType.doesTupleVanish()) {
    planExpandedFromDirect(innerOrigType, cast<TupleType>(innerSubstType),
                           outerOrigType, cast<TupleType>(outerSubstType),
                           innerResult);
    return;
  }

  // Otherwise, expand the outer tuple and recurse for the surviving
  // element.
  expandOuterVanishingTuple(outerOrigType, outerSubstType,
     [&](AbstractionPattern outerOrigEltType, CanType outerSubstEltType) {
    planFromDirect(innerOrigType, innerSubstType,
                   outerOrigEltType, outerSubstEltType,
                   innerResult);
  }, [&](AbstractionPattern outerOrigEltType, CanType outerSubstEltType,
         ManagedValue outerResultAddr) {
    planSingleIntoIndirect(innerOrigType, innerSubstType,
                           outerOrigEltType, outerSubstEltType,
                           innerResult, outerResultAddr.getLValueAddress());
  });
}

ManagedValue
ResultPlanner::expandSingleInnerIndirect(AbstractionPattern innerOrigType,
                                         CanType innerSubstType,
                                         AbstractionPattern outerOrigType,
                                         CanType outerSubstType,
                                         IndirectSlot innerResultSlot) {
  auto outerResult = claimNextOuterResult();
  auto innerResultAddr =
    planSingleFromIndirect(innerOrigType, innerSubstType,
                           outerOrigType, outerSubstType,
                           innerResultSlot,
                           outerResult.first, outerResult.second);
  return ManagedValue::forLValue(innerResultAddr);
}

/// Plan the emission of a call result from an inner result address,
/// given that the outer abstraction pattern is not a tuple.
SILValue
ResultPlanner::planSingleFromIndirect(AbstractionPattern innerOrigType,
                                      CanType innerSubstType,
                                      AbstractionPattern outerOrigType,
                                      CanType outerSubstType,
                                      IndirectSlot innerResultSlot,
                                      SILResultInfo outerResult,
                                      SILValue optOuterResultAddr) {
  assert(SGF.silConv.isSILIndirect(outerResult) == bool(optOuterResultAddr));

  // The outer result can be indirect, and it doesn't necessarily have an
  // abstraction difference.  Note that we should only end up in this path
  // in cases where simply forwarding the outer result address wasn't possible.

  if (SGF.silConv.isSILIndirect(outerResult)) {
    assert(optOuterResultAddr);
    return planIndirectIntoIndirect(innerOrigType, innerSubstType,
                                    outerOrigType, outerSubstType,
                                    innerResultSlot, optOuterResultAddr);
  } else {
    auto innerResultAddr = innerResultSlot.allocate(SGF, Loc);
    if (!hasAbstractionDifference(innerResultSlot, outerResult)) {
      addIndirectToDirect(innerResultAddr, outerResult);
    } else {
      addReabstractIndirectToDirect(innerOrigType, innerSubstType,
                                    outerOrigType, outerSubstType,
                                    innerResultAddr, outerResult);
    }
    return innerResultAddr;
  }
}

ManagedValue
ResultPlanner::expandSingleIndirect(AbstractionPattern innerOrigType,
                                    CanType innerSubstType,
                                    AbstractionPattern outerOrigType,
                                    CanType outerSubstType,
                                    IndirectSlot innerResultSlot,
                                    ManagedValue outerResultAddr) {
  auto innerResultAddr =
    planIndirectIntoIndirect(innerOrigType, innerSubstType,
                             outerOrigType, outerSubstType,
                             innerResultSlot,
                             outerResultAddr.getLValueAddress());
  return ManagedValue::forLValue(innerResultAddr);
}

SILValue
ResultPlanner::planIndirectIntoIndirect(AbstractionPattern innerOrigType,
                                        CanType innerSubstType,
                                        AbstractionPattern outerOrigType,
                                        CanType outerSubstType,
                                        IndirectSlot innerResultSlot,
                                        SILValue outerResultAddr) {
  if (!hasAbstractionDifference(innerResultSlot, outerResultAddr)) {
    // If there's no abstraction difference and no fixed address for
    // the inner result, just forward the outer address.
    if (!innerResultSlot.hasAddress())
      return outerResultAddr;

    // Otherwise, emit into the fixed inner address.
    auto innerResultAddr = innerResultSlot.getAddress();
    addIndirectToIndirect(innerResultAddr, outerResultAddr);
    return innerResultAddr;
  } else {
    auto innerResultAddr = innerResultSlot.allocate(SGF, Loc);
    addReabstractIndirectToIndirect(innerOrigType, innerSubstType,
                                    outerOrigType, outerSubstType,
                                    innerResultAddr, outerResultAddr);
    return innerResultAddr;
  }
}

static size_t getIsolatedParamIndex(CanAnyFunctionType fnType) {
  auto params = fnType->getParams();
  for (auto i : indices(params)) {
    if (params[i].isIsolated())
      return i;
  }
  llvm_unreachable("function does not have parameter isolation?");
}

/// Destructure a tuple and push its elements in reverse onto
/// the given stack, so that popping them off will visit them in
/// forward order.
static void destructureAndReverseTuple(SILGenFunction &SGF,
                                       SILLocation loc,
                                       SILValue tupleValue,
                                    SmallVectorImpl<SILValue> &values) {
  auto tupleTy = tupleValue->getType().castTo<TupleType>();
  assert(!tupleTy->containsPackExpansionType() &&
         "cannot destructure a tuple with pack expansions in it");

  SGF.B.emitDestructureValueOperation(loc, tupleValue, values);
  std::reverse(values.end() - tupleTy->getNumElements(), values.end());
}

SILValue ResultPlanner::execute(SILValue innerResult,
                                CanSILFunctionType innerFnType) {
  // The code emission here assumes that we don't need to have
  // active cleanups for all the result values we're not actively
  // transforming.  In other words, it's not "exception-safe".

  // Explode the first level of tuple for the direct inner results
  // (the one that's introduced implicitly when there are multiple
  // results).
  SmallVector<SILValue, 4> innerDirectResultStack;
  unsigned numInnerDirectResults =
    SILFunctionConventions(innerFnType, SGF.SGM.M)
        .getNumDirectSILResults();
  if (numInnerDirectResults == 0) {
    // silently ignore the result
  } else if (numInnerDirectResults > 1) {
    destructureAndReverseTuple(SGF, Loc, innerResult,
                               innerDirectResultStack);
  } else {
    innerDirectResultStack.push_back(innerResult);
  }

  // Translate the result values.
  SmallVector<SILValue, 4> outerDirectResults;
  execute(innerDirectResultStack, outerDirectResults);

  // Implode the outer direct results.
  SILValue outerResult;
  if (outerDirectResults.size() == 1) {
    outerResult = outerDirectResults[0];
  } else {
    outerResult = SGF.B.createTuple(Loc, outerDirectResults);
  }

  return outerResult;
}

/// innerDirectResults is a stack: we expect to pull the next result
/// off the end.
void ResultPlanner::execute(SmallVectorImpl<SILValue> &innerDirectResultStack,
                            SmallVectorImpl<SILValue> &outerDirectResults) {
  // A helper function to claim an inner direct result.
  auto claimNextInnerDirectResult = [&](SILResultInfo result) -> ManagedValue {
    auto resultValue = innerDirectResultStack.pop_back_val();
    assert(resultValue->getType() == SGF.getSILType(result, CanSILFunctionType()));
    auto &resultTL = SGF.getTypeLowering(result.getInterfaceType());
    switch (result.getConvention()) {
    case ResultConvention::Indirect:
      assert(!SGF.silConv.isSILIndirect(result)
             && "claiming indirect result as direct!");
      LLVM_FALLTHROUGH;
    case ResultConvention::Owned:
    case ResultConvention::Autoreleased:
      return SGF.emitManagedRValueWithCleanup(resultValue, resultTL);
    case ResultConvention::Pack:
      llvm_unreachable("shouldn't have direct result with pack results");
    case ResultConvention::UnownedInnerPointer:
      // FIXME: We can't reasonably lifetime-extend an inner-pointer result
      // through a thunk. We don't know which parameter to the thunk was
      // originally 'self'.
      SGF.SGM.diagnose(Loc.getSourceLoc(), diag::not_implemented,
                       "reabstraction of returns_inner_pointer function");
      LLVM_FALLTHROUGH;
    case ResultConvention::Unowned:
      return SGF.emitManagedCopy(Loc, resultValue, resultTL);
    }
    llvm_unreachable("bad result convention!");
  };

  // A helper function to add an outer direct result.
  auto addOuterDirectResult = [&](ManagedValue resultValue,
                                  SILResultInfo result) {
    resultValue = applyTrivialConversions(SGF, Loc, resultValue,
                        SGF.getSILTypeInContext(result, CanSILFunctionType()));
    outerDirectResults.push_back(resultValue.forward(SGF));
  };

  auto emitReabstract =
      [&](Operation &op, bool innerIsIndirect, bool outerIsIndirect) {
    // Set up the inner result.
    ManagedValue innerResult;
    if (innerIsIndirect) {
      innerResult = SGF.emitManagedBufferWithCleanup(op.InnerResultAddr);
    } else {
      innerResult = claimNextInnerDirectResult(op.InnerResult);
    }

    // Set up the context into which to emit the outer result.
    SGFContext outerResultCtxt;
    std::optional<TemporaryInitialization> outerResultInit;
    SILType outerResultTy;
    if (outerIsIndirect) {
      outerResultTy = op.OuterResultAddr->getType();
      outerResultInit.emplace(op.OuterResultAddr, CleanupHandle::invalid());
      outerResultCtxt = SGFContext(&*outerResultInit);
    } else  {
      outerResultTy =
        SGF.F.mapTypeIntoContext(
          SGF.getSILType(op.OuterResult, CanSILFunctionType()));
    }

    // Perform the translation.
    auto translated =
      SGF.emitTransformedValue(Loc, innerResult,
                               op.InnerOrigType, op.InnerSubstType,
                               op.OuterOrigType, op.OuterSubstType,
                               outerResultTy, outerResultCtxt);

    // If the outer is indirect, force it into the context.
    if (outerIsIndirect) {
      if (!translated.isInContext()) {
        translated.forwardInto(SGF, Loc, op.OuterResultAddr);
      }

    // Otherwise, it's a direct result.
    } else {
      addOuterDirectResult(translated, op.OuterResult);
    }
  };

  // Execute each operation.
  for (auto &op : Operations) {
    switch (op.TheKind) {
    case Operation::DirectToDirect: {
      auto result = claimNextInnerDirectResult(op.InnerResult);
      addOuterDirectResult(result, op.OuterResult);
      continue;
    }

    case Operation::DirectToIndirect: {
      auto result = claimNextInnerDirectResult(op.InnerResult);
      SGF.B.emitStoreValueOperation(Loc, result.forward(SGF),
                                    op.OuterResultAddr,
                                    StoreOwnershipQualifier::Init);
      continue;
    }

    case Operation::IndirectToDirect: {
      auto resultAddr = op.InnerResultAddr;
      auto &resultTL = SGF.getTypeLowering(resultAddr->getType());
      auto result = SGF.emitManagedRValueWithCleanup(
          resultTL.emitLoad(SGF.B, Loc, resultAddr,
                            LoadOwnershipQualifier::Take),
          resultTL);
      addOuterDirectResult(result, op.OuterResult);
      continue;
    }

    case Operation::IndirectToIndirect: {
      // The type could be address-only; just take.
      SGF.B.createCopyAddr(Loc, op.InnerResultAddr, op.OuterResultAddr,
                           IsTake, IsInitialization);
      continue;
    }

    case Operation::ReabstractIndirectToIndirect:
      emitReabstract(op, /*indirect source*/ true, /*indirect dest*/ true);
      continue;
    case Operation::ReabstractIndirectToDirect:
      emitReabstract(op, /*indirect source*/ true, /*indirect dest*/ false);
      continue;
    case Operation::ReabstractDirectToIndirect:
      emitReabstract(op, /*indirect source*/ false, /*indirect dest*/ true);
      continue;
    case Operation::ReabstractDirectToDirect:
      emitReabstract(op, /*indirect source*/ false, /*indirect dest*/ false);
      continue;

    case Operation::ReabstractTupleIntoPackExpansion:
      op.emitReabstractTupleIntoPackExpansion(SGF, Loc);
      continue;

    case Operation::TupleDirect: {
      auto firstEltIndex = outerDirectResults.size() - op.NumElements;
      auto elts = llvm::ArrayRef(outerDirectResults).slice(firstEltIndex);
      auto tupleType = SGF.F.mapTypeIntoContext(
                          SGF.getSILType(op.OuterResult, CanSILFunctionType()));
      auto tuple = SGF.B.createTuple(Loc, tupleType, elts);
      outerDirectResults.resize(firstEltIndex);
      outerDirectResults.push_back(tuple);
      continue;
    }

    case Operation::DestructureDirectInnerTuple: {
      auto result = claimNextInnerDirectResult(op.InnerResult);
      assert(result.isPlusOne(SGF));
      destructureAndReverseTuple(SGF, Loc, result.forward(SGF),
                                 innerDirectResultStack);
      continue;
    }

    case Operation::InjectOptionalDirect: {
      SILValue value = outerDirectResults.pop_back_val();
      auto tupleType = SGF.F.mapTypeIntoContext(
                          SGF.getSILType(op.OuterResult, CanSILFunctionType()));
      SILValue optValue = SGF.B.createEnum(Loc, value, op.SomeDecl, tupleType);
      outerDirectResults.push_back(optValue);
      continue;
    }

    case Operation::InjectOptionalIndirect:
      SGF.B.createInjectEnumAddr(Loc, op.OuterResultAddr, op.SomeDecl);
      continue;
    }
    llvm_unreachable("bad operation kind");
  }

  assert(innerDirectResultStack.empty() && "didn't consume all inner results?");
}

/// Build the body of a transformation thunk.
///
/// \param inputOrigType Abstraction pattern of function value being thunked
/// \param inputSubstType Formal AST type of function value being thunked
/// \param outputOrigType Abstraction pattern of the thunk
/// \param outputSubstType Formal AST type of the thunk
/// \param dynamicSelfType If true, the last parameter is a dummy used to pass
/// DynamicSelfType metadata
static void buildThunkBody(SILGenFunction &SGF, SILLocation loc,
                           AbstractionPattern inputOrigType,
                           CanAnyFunctionType inputSubstType,
                           AbstractionPattern outputOrigType,
                           CanAnyFunctionType outputSubstType,
                           CanSILFunctionType expectedType,
                           CanType dynamicSelfType,
                           llvm::function_ref<void(SILGenFunction &)> emitProlog
                               = [](SILGenFunction &){}) {
  PrettyStackTraceSILFunction stackTrace("emitting reabstraction thunk in",
                                         &SGF.F);
  auto thunkType = SGF.F.getLoweredFunctionType();

  FullExpr scope(SGF.Cleanups, CleanupLocation(loc));

  SmallVector<ManagedValue, 8> params;
  SmallVector<ManagedValue, 4> indirectResultParams;
  SGF.collectThunkParams(loc, params, &indirectResultParams);

  // Ignore the self parameter at the SIL level. IRGen will use it to
  // recover type metadata.
  if (dynamicSelfType)
    params.pop_back();

  ManagedValue fnValue = params.pop_back_val();
  auto fnType = fnValue.getType().castTo<SILFunctionType>();
  assert(!fnType->isPolymorphic());
  auto argTypes = fnType->getParameters();

  // If the destination type is @isolated(any), pop off that argument as well.
  ManagedValue outputErasedIsolation;
  if (expectedType->hasErasedIsolation()) {
    outputErasedIsolation = params.pop_back_val();
  }

  // We may need to establish the right executor for the input function.
  // If both function types are synchronous, whoever calls this thunk is
  // responsible for establishing the executor properly, so we don't need
  // to do anything.  If both function types are asynchronous, the input
  // function is responsible for establishing the executor, so again we
  // don't need to do anything.  But if the input is synchronous and the
  // executor is asynchronous, we need to treat this like any other call
  // to a synchronous function from an asynchronous context.
  bool hopToIsolatedParameter = false;
  if (outputSubstType->isAsync() && !inputSubstType->isAsync()) {
    auto inputIsolation = inputSubstType->getIsolation();
    switch (inputIsolation.getKind()) {
    // Synchronous nonisolated functions are called on the current executor.
    case FunctionTypeIsolation::Kind::NonIsolated:
      break;

    // For a function with parameter isolation, we'll have to dig the
    // argument out after translation but before making the call.
    case FunctionTypeIsolation::Kind::Parameter:
      hopToIsolatedParameter = true;
      break;

    // For a function with global-actor isolation, hop to the appropriate
    // global actor.
    case FunctionTypeIsolation::Kind::GlobalActor:
      // If the thunk is erasing to @isolated(any), the output erased
      // isolation should already be the global actor.  But it's probably
      // more optimizable to ignore this.
      SGF.emitPrologGlobalActorHop(loc, inputIsolation.getGlobalActorType());
      break;

    // If the input is @isolated(any), dig out its isolation.
    case FunctionTypeIsolation::Kind::Erased:
      // If we're converting between @isolated(any) types, the isolation
      // value we captured for the output will be what we dug out of the
      // input function.
      ManagedValue inputErasedIsolation;
      if (outputErasedIsolation) {
        inputErasedIsolation = outputErasedIsolation;

      // Otherwise, if we're statically erasing `@isolated(any)`, we'll need
      // to dig the input isolation out.
      } else {
        inputErasedIsolation = SGF.emitLoadErasedIsolation(loc, fnValue);
      }
      SGF.B.createHopToExecutor(loc, inputErasedIsolation.getValue(),
                                /*mandatory*/false);
      break;
    }
  }

  emitProlog(SGF);

  // Translate the argument values.  Function parameters are
  // contravariant: we want to switch the direction of transformation
  // on them by flipping inputOrigType and outputOrigType.
  //
  // For example, a transformation of (Int,Int)->Int to (T,T)->T is
  // one that should take an (Int,Int)->Int value and make it be
  // abstracted like a (T,T)->T value.  This must be done with a thunk.
  // Within the thunk body, the result of calling the inner function
  // needs to be translated from Int to T (we receive a normal Int
  // and return it like a T), but the parameters are translated in the
  // other direction (the thunk receives an Int like a T, and passes it
  // like a normal Int when calling the inner function).
  SmallVector<ManagedValue, 8> args;
  TranslateArguments(SGF, loc, params, args, fnType, argTypes)
    .process(inputOrigType,
             inputSubstType.getParams(),
             outputOrigType,
             outputSubstType.getParams());

  SmallVector<SILValue, 8> argValues;

  // Plan the results.  This builds argument values for all the
  // inner indirect results.
  ResultPlanner resultPlanner(SGF, loc, indirectResultParams, argValues);
  resultPlanner.plan(inputOrigType.getFunctionResultType(),
                     inputSubstType.getResult(),
                     outputOrigType.getFunctionResultType(),
                     outputSubstType.getResult(),
                     fnType, thunkType);

  // If the function we're calling has an indirect error result, create an
  // argument for it.
  if (auto innerIndirectErrorAddr =
          emitThunkIndirectErrorArgument(SGF, loc, fnType)) {
    argValues.push_back(*innerIndirectErrorAddr);
  }

  // If we need to jump to an isolated parameter, do so before the call.
  if (hopToIsolatedParameter) {
    auto formalIsolatedIndex = getIsolatedParamIndex(inputSubstType);
    auto isolatedIndex = inputOrigType.getLoweredParamIndex(formalIsolatedIndex);
    SGF.B.createHopToExecutor(loc, args[isolatedIndex].getValue(),
                              /*mandatory*/false);
  }

  // Add the rest of the arguments.
  forwardFunctionArguments(SGF, loc, fnType, args, argValues);

  auto fun = fnType->isCalleeGuaranteed() ? fnValue.borrow(SGF, loc).getValue()
                                          : fnValue.forward(SGF);
  SILValue innerResult =
      SGF.emitApplyWithRethrow(loc, fun,
                               /*substFnType*/ fnValue.getType(),
                               /*substitutions*/ {}, argValues);

  // Reabstract the result.
  SILValue outerResult = resultPlanner.execute(innerResult, fnType);

  scope.pop();
  SGF.B.createReturn(loc, outerResult);
}

/// Build the type of a function transformation thunk.
CanSILFunctionType SILGenFunction::buildThunkType(
    CanSILFunctionType &sourceType,
    CanSILFunctionType &expectedType,
    CanType &inputSubstType,
    CanType &outputSubstType,
    GenericEnvironment *&genericEnv,
    SubstitutionMap &interfaceSubs,
    CanType &dynamicSelfType,
    bool withoutActuallyEscaping) {
  return buildSILFunctionThunkType(&F, sourceType, expectedType, inputSubstType, outputSubstType, genericEnv, interfaceSubs, dynamicSelfType, withoutActuallyEscaping);
}

static ManagedValue createPartialApplyOfThunk(SILGenFunction &SGF,
                                              SILLocation loc,
                                              SILFunction *thunk,
                                              SubstitutionMap interfaceSubs,
                                              CanType dynamicSelfType,
                                              CanSILFunctionType toType,
                                              ManagedValue fn,
                                              ManagedValue isolation) {
  auto thunkValue = SGF.B.createFunctionRefFor(loc, thunk);

  // This parallels the logic in buildSILFunctionThunkType.
  SmallVector<ManagedValue, 3> thunkArgs;

  // The isolation of an @isolated(any) closure is always the first capture.
  assert(toType->hasErasedIsolation() == isolation.isValid());
  if (isolation) {
    thunkArgs.push_back(isolation);
  }

  thunkArgs.push_back(fn);
  if (dynamicSelfType) {
    SILType dynamicSILType = SGF.getLoweredType(dynamicSelfType);
    SILValue value = SGF.B.createMetatype(loc, dynamicSILType);
    thunkArgs.push_back(ManagedValue::forObjectRValueWithoutOwnership(value));
  }

  return
    SGF.B.createPartialApply(loc, thunkValue,
                             interfaceSubs, thunkArgs,
                             toType->getCalleeConvention(),
                             toType->getIsolation());
}

static ManagedValue createDifferentiableFunctionThunk(
    SILGenFunction &SGF, SILLocation loc, ManagedValue fn,
    AbstractionPattern inputOrigType, CanAnyFunctionType inputSubstType,
    AbstractionPattern outputOrigType, CanAnyFunctionType outputSubstType);

/// Create a reabstraction thunk.
static ManagedValue createThunk(SILGenFunction &SGF,
                                SILLocation loc,
                                ManagedValue fn,
                                AbstractionPattern inputOrigType,
                                CanAnyFunctionType inputSubstType,
                                AbstractionPattern outputOrigType,
                                CanAnyFunctionType outputSubstType,
                                const TypeLowering &expectedTL) {
  auto substSourceType = fn.getType().castTo<SILFunctionType>();
  auto substExpectedType = expectedTL.getLoweredType().castTo<SILFunctionType>();
  
  LLVM_DEBUG(llvm::dbgs() << "=== Generating reabstraction thunk from:\n";
             substSourceType.dump(llvm::dbgs());
             llvm::dbgs() << "\n    to:\n";
             substExpectedType.dump(llvm::dbgs());
             llvm::dbgs() << "\n    for source location:\n";
             if (auto d = loc.getAsASTNode<Decl>()) {
               d->dump(llvm::dbgs());
             } else if (auto e = loc.getAsASTNode<Expr>()) {
               e->dump(llvm::dbgs());
             } else if (auto s = loc.getAsASTNode<Stmt>()) {
               s->dump(llvm::dbgs());
             } else if (auto p = loc.getAsASTNode<Pattern>()) {
               p->dump(llvm::dbgs());
             } else {
               loc.dump();
             }
             llvm::dbgs() << "\n");
  
  // Apply substitutions in the source and destination types, since the thunk
  // doesn't change because of different function representations.
  CanSILFunctionType sourceType;
  if (substSourceType->getPatternSubstitutions()) {
    sourceType = substSourceType->getUnsubstitutedType(SGF.SGM.M);
    fn = SGF.B.createConvertFunction(loc, fn,
                                   SILType::getPrimitiveObjectType(sourceType));
  } else {
    sourceType = substSourceType;
  }
  
  auto expectedType = substExpectedType
    ->getUnsubstitutedType(SGF.SGM.M);

  assert(expectedType->hasErasedIsolation()
           == outputSubstType->getIsolation().isErased());

  assert(sourceType->isDifferentiable() == expectedType->isDifferentiable() &&
         "thunks can't change differentiability");
  if (sourceType->isDifferentiable()) {
    return createDifferentiableFunctionThunk(SGF, loc, fn, inputOrigType,
                                             inputSubstType, outputOrigType,
                                             outputSubstType);
  }

  // We can't do bridging here.
  assert(expectedType->getLanguage() ==
         fn.getType().castTo<SILFunctionType>()->getLanguage() &&
         "bridging in re-abstraction thunk?");

  // Declare the thunk.
  SubstitutionMap interfaceSubs;
  GenericEnvironment *genericEnv = nullptr;
  auto toType = expectedType->getWithExtInfo(
      expectedType->getExtInfo().withNoEscape(false));
  CanType dynamicSelfType;
  auto thunkType = SGF.buildThunkType(sourceType, toType,
                                      inputSubstType,
                                      outputSubstType,
                                      genericEnv,
                                      interfaceSubs,
                                      dynamicSelfType);
  // An actor-isolated non-async function can be converted to an async function
  // by inserting a hop to the global actor.
  CanType globalActorForThunk;
  if (outputSubstType->isAsync()
      && !inputSubstType->isAsync()) {
    globalActorForThunk = CanType(inputSubstType->getGlobalActor());
  }

  auto thunk = SGF.SGM.getOrCreateReabstractionThunk(
                                       thunkType,
                                       sourceType,
                                       toType,
                                       dynamicSelfType,
                                       globalActorForThunk);

  // Build it if necessary.
  if (thunk->empty()) {
    thunk->setGenericEnvironment(genericEnv);
    SILGenFunction thunkSGF(SGF.SGM, *thunk, SGF.FunctionDC);
    auto loc = RegularLocation::getAutoGeneratedLocation();
    buildThunkBody(thunkSGF, loc,
                   inputOrigType,
                   inputSubstType,
                   outputOrigType,
                   outputSubstType,
                   expectedType,
                   dynamicSelfType);
    SGF.SGM.emitLazyConformancesForFunction(thunk);
  }

  // If we're generating a function with erased isolation, compute the
  // isolation of the function value we're converting.  This is purely
  // based on the isolation in the function type, so it's critical that
  // we not use this path for function values where we've statically
  // erased the isolation.
  ManagedValue erasedIsolation;
  if (toType->hasErasedIsolation()) {
    auto inputIsolation = inputSubstType->getIsolation();
    erasedIsolation = SGF.emitFunctionTypeIsolation(loc, inputIsolation, fn);
  }

  auto thunkedFn =
    createPartialApplyOfThunk(SGF, loc, thunk, interfaceSubs, dynamicSelfType,
                              toType, fn.ensurePlusOne(SGF, loc),
                              erasedIsolation);

  // Convert to the substituted result type.
  if (expectedType != substExpectedType) {
    auto substEscapingExpectedType = substExpectedType
      ->getWithExtInfo(substExpectedType->getExtInfo().withNoEscape(false));
    thunkedFn = SGF.B.createConvertFunction(loc, thunkedFn,
                    SILType::getPrimitiveObjectType(substEscapingExpectedType));
  }
  
  if (!substExpectedType->isNoEscape()) {
    return thunkedFn;
  }

  // Handle the escaping to noescape conversion.
  assert(substExpectedType->isNoEscape());
  return SGF.B.createConvertEscapeToNoEscape(
      loc, thunkedFn, SILType::getPrimitiveObjectType(substExpectedType));
}

/// Create a reabstraction thunk for a @differentiable function.
static ManagedValue createDifferentiableFunctionThunk(
    SILGenFunction &SGF, SILLocation loc, ManagedValue fn,
    AbstractionPattern inputOrigType, CanAnyFunctionType inputSubstType,
    AbstractionPattern outputOrigType, CanAnyFunctionType outputSubstType) {
  // Applies a thunk to all the components by extracting them, applying thunks
  // to all of them, and then putting them back together.
  auto sourceType = fn.getType().castTo<SILFunctionType>();

  auto withoutDifferentiablePattern =
      [](AbstractionPattern pattern) -> AbstractionPattern {
    auto patternType = pattern.getAs<AnyFunctionType>();
    // If pattern does not store an `AnyFunctionType`, return original pattern.
    // This logic handles opaque abstraction patterns.
    if (!patternType)
      return pattern;
    pattern.rewriteType(
        pattern.getGenericSignature(),
        patternType->getWithoutDifferentiability()->getCanonicalType());
    return pattern;
  };

  auto inputOrigTypeNotDiff = withoutDifferentiablePattern(inputOrigType);
  CanAnyFunctionType inputSubstTypeNotDiff(
      inputSubstType->getWithoutDifferentiability());
  auto outputOrigTypeNotDiff = withoutDifferentiablePattern(outputOrigType);
  CanAnyFunctionType outputSubstTypeNotDiff(
      outputSubstType->getWithoutDifferentiability());
  auto &expectedTLNotDiff =
      SGF.getTypeLowering(outputOrigTypeNotDiff, outputSubstTypeNotDiff);
  // `differentiable_function_extract` takes `@guaranteed` values.
  auto borrowedFnValue = fn.borrow(SGF, loc);
  SILValue original = SGF.B.createDifferentiableFunctionExtractOriginal(
      loc, borrowedFnValue.getValue());
  original = SGF.B.emitCopyValueOperation(loc, original);
  auto managedOriginal = SGF.emitManagedRValueWithCleanup(original);

  ManagedValue originalThunk = createThunk(
      SGF, loc, managedOriginal, inputOrigTypeNotDiff, inputSubstTypeNotDiff,
      outputOrigTypeNotDiff, outputSubstTypeNotDiff, expectedTLNotDiff);

  auto numUncurriedParams = inputSubstType->getNumParams();
  if (auto *resultFnType =
          inputSubstType->getResult()->getAs<AnyFunctionType>()) {
    numUncurriedParams += resultFnType->getNumParams();
  }
  llvm::SmallBitVector parameterBits(numUncurriedParams);
  for (auto i : range(inputSubstType->getNumParams()))
    if (!inputSubstType->getParams()[i].isNoDerivative())
      parameterBits.set(i);
  auto *parameterIndices = IndexSubset::get(SGF.getASTContext(), parameterBits);

  auto getDerivativeFnTy =
      [&](CanAnyFunctionType fnTy,
          AutoDiffDerivativeFunctionKind kind) -> CanAnyFunctionType {
    auto assocTy = fnTy->getAutoDiffDerivativeFunctionType(
        parameterIndices, kind,
        LookUpConformanceInModule(SGF.SGM.M.getSwiftModule()));
    return cast<AnyFunctionType>(assocTy->getCanonicalType());
  };
  auto getDerivativeFnPattern =
      [&](AbstractionPattern pattern,
          AutoDiffDerivativeFunctionKind kind) -> AbstractionPattern {
    return pattern.getAutoDiffDerivativeFunctionType(
        parameterIndices, kind,
        LookUpConformanceInModule(SGF.SGM.M.getSwiftModule()));
  };
  auto createDerivativeFnThunk =
      [&](AutoDiffDerivativeFunctionKind kind) -> ManagedValue {
    auto derivativeFnInputOrigType =
        getDerivativeFnPattern(inputOrigTypeNotDiff, kind);
    auto derivativeFnInputSubstType =
        getDerivativeFnTy(inputSubstTypeNotDiff, kind);
    auto derivativeFnOutputOrigType =
        getDerivativeFnPattern(outputOrigTypeNotDiff, kind);
    auto derivativeFnOutputSubstType =
        getDerivativeFnTy(outputSubstTypeNotDiff, kind);
    auto &derivativeFnExpectedTL = SGF.getTypeLowering(
        derivativeFnOutputOrigType, derivativeFnOutputSubstType);
    SILValue derivativeFn = SGF.B.createDifferentiableFunctionExtract(
        loc, kind, borrowedFnValue.getValue());
    derivativeFn = SGF.B.emitCopyValueOperation(loc, derivativeFn);
    auto managedDerivativeFn = SGF.emitManagedRValueWithCleanup(derivativeFn);
    return createThunk(SGF, loc, managedDerivativeFn, derivativeFnInputOrigType,
                       derivativeFnInputSubstType, derivativeFnOutputOrigType,
                       derivativeFnOutputSubstType, derivativeFnExpectedTL);
  };

  auto jvpThunk = createDerivativeFnThunk(AutoDiffDerivativeFunctionKind::JVP);
  auto vjpThunk = createDerivativeFnThunk(AutoDiffDerivativeFunctionKind::VJP);

  SILValue convertedBundle = SGF.B.createDifferentiableFunction(
      loc, sourceType->getDifferentiabilityParameterIndices(),
      sourceType->getDifferentiabilityResultIndices(),
      originalThunk.forward(SGF),
      std::make_pair(jvpThunk.forward(SGF), vjpThunk.forward(SGF)));
  return SGF.emitManagedRValueWithCleanup(convertedBundle);
}

static CanSILFunctionType buildWithoutActuallyEscapingThunkType(
    SILGenFunction &SGF, CanSILFunctionType &noEscapingType,
    CanSILFunctionType &escapingType, GenericEnvironment *&genericEnv,
    SubstitutionMap &interfaceSubs, CanType &dynamicSelfType) {

  assert(escapingType->getExtInfo().isEqualTo(
      noEscapingType->getExtInfo().withNoEscape(false),
      useClangTypes(escapingType)));

  CanType inputSubstType, outputSubstType;
  auto type = SGF.buildThunkType(noEscapingType, escapingType,
                                 inputSubstType, outputSubstType,
                                 genericEnv, interfaceSubs,
                                 dynamicSelfType,
                                 /*withoutActuallyEscaping=*/true);
  return type;
}

static void buildWithoutActuallyEscapingThunkBody(SILGenFunction &SGF,
                                                  CanType dynamicSelfType) {
  PrettyStackTraceSILFunction stackTrace(
      "emitting withoutActuallyEscaping thunk in", &SGF.F);

  auto loc = RegularLocation::getAutoGeneratedLocation();

  FullExpr scope(SGF.Cleanups, CleanupLocation(loc));

  SmallVector<ManagedValue, 8> params;
  SmallVector<ManagedValue, 8> indirectResults;
  SmallVector<ManagedValue, 1> indirectErrorResults;
  SGF.collectThunkParams(loc, params, &indirectResults, &indirectErrorResults);

  // Ignore the self parameter at the SIL level. IRGen will use it to
  // recover type metadata.
  if (dynamicSelfType)
    params.pop_back();

  ManagedValue fnValue = params.pop_back_val();
  auto fnType = fnValue.getType().castTo<SILFunctionType>();

  // Forward indirect result arguments.
  SmallVector<SILValue, 8> argValues;
  if (!indirectResults.empty()) {
    for (auto result : indirectResults)
      argValues.push_back(result.getLValueAddress());
  }

  // Forward indirect error arguments.
  for (auto indirectError : indirectErrorResults)
    argValues.push_back(indirectError.getLValueAddress());

   // Add the rest of the arguments.
  forwardFunctionArguments(SGF, loc, fnType, params, argValues);

  auto fun = fnType->isCalleeGuaranteed() ? fnValue.borrow(SGF, loc).getValue()
                                          : fnValue.forward(SGF);
  SILValue result =
      SGF.emitApplyWithRethrow(loc, fun,
                               /*substFnType*/ fnValue.getType(),
                               /*substitutions*/ {}, argValues);

  scope.pop();
  SGF.B.createReturn(loc, result);
}

ManagedValue
SILGenFunction::createWithoutActuallyEscapingClosure(
    SILLocation loc, ManagedValue noEscapingFunctionValue, SILType escapingTy) {

  auto escapingFnSubstTy = escapingTy.castTo<SILFunctionType>();
  auto noEscapingFnSubstTy = noEscapingFunctionValue.getType()
    .castTo<SILFunctionType>();
  // TODO: maybe this should use a more explicit instruction.
  assert(escapingFnSubstTy->getExtInfo().isEqualTo(
      noEscapingFnSubstTy->getExtInfo().withNoEscape(false),
      useClangTypes(escapingFnSubstTy)));

  // Apply function type substitutions, since the code sequence for a thunk
  // doesn't vary with function representation.
  auto escapingFnTy = escapingFnSubstTy->getUnsubstitutedType(SGM.M);
  auto noEscapingFnTy = noEscapingFnSubstTy->getUnsubstitutedType(SGM.M);

  SubstitutionMap interfaceSubs;
  GenericEnvironment *genericEnv = nullptr;

  CanType dynamicSelfType;
  auto thunkType = buildWithoutActuallyEscapingThunkType(
      *this, noEscapingFnTy, escapingFnTy, genericEnv, interfaceSubs,
      dynamicSelfType);

  auto *thunk = SGM.getOrCreateReabstractionThunk(
      thunkType, noEscapingFnTy, escapingFnTy, dynamicSelfType, CanType());

  if (thunk->empty()) {
    thunk->setWithoutActuallyEscapingThunk();
    thunk->setGenericEnvironment(genericEnv);
    SILGenFunction thunkSGF(SGM, *thunk, FunctionDC);
    buildWithoutActuallyEscapingThunkBody(thunkSGF, dynamicSelfType);
    SGM.emitLazyConformancesForFunction(thunk);
  }
  assert(thunk->isWithoutActuallyEscapingThunk());

  // Create a copy for the noescape value, so we can mark_dependence upon the
  // original value.
  auto noEscapeValue = noEscapingFunctionValue.copy(*this, loc);
  // Convert away function type substitutions.
  if (noEscapingFnTy != noEscapingFnSubstTy) {
    noEscapeValue = B.createConvertFunction(loc, noEscapeValue,
                              SILType::getPrimitiveObjectType(noEscapingFnTy));
  }

  // FIXME: implement this
  ManagedValue erasedIsolation;
  if (escapingFnTy->hasErasedIsolation()) {
    SGM.diagnose(loc, diag::without_actually_escaping_on_isolated_any);
    erasedIsolation = emitUndef(SILType::getOpaqueIsolationType(getASTContext()));
  }

  auto thunkedFn =
    createPartialApplyOfThunk(*this, loc, thunk, interfaceSubs, dynamicSelfType,
                              escapingFnTy, noEscapeValue, erasedIsolation);

  // Convert to the substituted escaping type.
  if (escapingFnTy != escapingFnSubstTy) {
    thunkedFn = B.createConvertFunction(loc, thunkedFn,
                            SILType::getPrimitiveObjectType(escapingFnSubstTy));
  }
  
  // We need to ensure the 'lifetime' of the trivial values context captures. As
  // long as we represent these captures by the same value the following works.
  thunkedFn = emitManagedRValueWithCleanup(
    B.createMarkDependence(loc, thunkedFn.forward(*this),
                           noEscapingFunctionValue.getValue(),
                           MarkDependenceKind::Escaping));

  return thunkedFn;
}

/// Given a value, extracts all elements to `result` from this value if it's a
/// tuple. Otherwise, add this value directly to `result`.
static void extractAllElements(SILValue val, SILLocation loc,
                               SILBuilder &builder,
                               SmallVectorImpl<SILValue> &result) {
  auto &fn = builder.getFunction();
  auto tupleType = val->getType().getAs<TupleType>();
  if (!tupleType) {
    result.push_back(val);
    return;
  }
  if (!fn.hasOwnership()) {
    for (auto i : range(tupleType->getNumElements()))
      result.push_back(builder.createTupleExtract(loc, val, i));
    return;
  }
  if (tupleType->getNumElements() == 0)
    return;
  builder.emitDestructureValueOperation(loc, val, result);
}

/// Given a range of elements, joins these into a single value. If there's
/// exactly one element, returns that element. Otherwise, creates a tuple using
/// a `tuple` instruction.
static SILValue joinElements(ArrayRef<SILValue> elements, SILBuilder &builder,
                             SILLocation loc) {
  if (elements.size() == 1)
    return elements.front();
  return builder.createTuple(loc, elements);
}

/// Adapted from `SILGenModule::getOrCreateReabstractionThunk`.
ManagedValue SILGenFunction::getThunkedAutoDiffLinearMap(
    ManagedValue linearMap, AutoDiffLinearMapKind linearMapKind,
    CanSILFunctionType fromType, CanSILFunctionType toType, bool reorderSelf) {
  // Compute the thunk type.
  SubstitutionMap interfaceSubs;
  GenericEnvironment *genericEnv = nullptr;
  // Ignore subst types.
  CanType inputSubstType, outputSubstType;
  CanType dynamicSelfType;
  fromType = fromType->getUnsubstitutedType(getModule());
  toType = toType->getUnsubstitutedType(getModule());
  auto thunkType =
      buildThunkType(fromType, toType, inputSubstType, outputSubstType,
                     genericEnv, interfaceSubs, dynamicSelfType);
  assert(!dynamicSelfType && "Dynamic self type not handled");
  auto thunkDeclType =
      thunkType->getWithExtInfo(thunkType->getExtInfo().withNoEscape(false));

  // Get the thunk name.
  auto fromInterfaceType = fromType->mapTypeOutOfContext()->getCanonicalType();
  auto toInterfaceType = toType->mapTypeOutOfContext()->getCanonicalType();
  Mangle::ASTMangler mangler;
  std::string name;
  // If `self` is being reordered, it is an AD-specific self-reordering
  // reabstraction thunk.
  if (reorderSelf) {
    name = mangler.mangleAutoDiffSelfReorderingReabstractionThunk(
        toInterfaceType, fromInterfaceType,
        thunkType->getInvocationGenericSignature(), linearMapKind);
  }
  // Otherwise, it is just a normal reabstraction thunk.
  else {
    name = mangler.mangleReabstractionThunkHelper(
        thunkType, fromInterfaceType, toInterfaceType, Type(), Type(),
        getModule().getSwiftModule());
  }

  // Create the thunk.
  auto loc = F.getLocation();
  SILGenFunctionBuilder fb(SGM);
  auto *thunk = fb.getOrCreateSharedFunction(
      loc, name, thunkDeclType, IsBare, IsTransparent, IsSerialized,
      ProfileCounter(), IsReabstractionThunk, IsNotDynamic, IsNotDistributed,
      IsNotRuntimeAccessible);

  // Partially-apply the thunk to `linearMap` and return the thunked value.
  auto getThunkedResult = [&]() {
    auto linearMapFnType = linearMap.getType().castTo<SILFunctionType>();
    auto linearMapUnsubstFnType =
        linearMapFnType->getUnsubstitutedType(getModule());
    if (linearMapFnType != linearMapUnsubstFnType) {
      auto unsubstType =
          SILType::getPrimitiveObjectType(linearMapUnsubstFnType);
      linearMap = B.createConvertFunction(loc, linearMap, unsubstType,
                                          /*withoutActuallyEscaping*/ false);
    }
    auto thunkedFn = createPartialApplyOfThunk(
        *this, loc, thunk, interfaceSubs, dynamicSelfType, toType, linearMap,
        ManagedValue());
    if (!toType->isNoEscape())
      return thunkedFn;
    // Handle escaping to noescape conversion.
    return B.createConvertEscapeToNoEscape(
        loc, thunkedFn, SILType::getPrimitiveObjectType(toType));
  };

  if (!thunk->empty())
    return getThunkedResult();
  thunk->setGenericEnvironment(genericEnv);

  SILGenFunction thunkSGF(SGM, *thunk, FunctionDC);
  SmallVector<ManagedValue, 4> params;
  SmallVector<ManagedValue, 4> thunkIndirectResults;
  SmallVector<ManagedValue, 4> thunkIndirectErrorResults;
  thunkSGF.collectThunkParams(
      loc, params, &thunkIndirectResults, &thunkIndirectErrorResults);

  SILFunctionConventions fromConv(fromType, getModule());
  SILFunctionConventions toConv(toType, getModule());
  if (!toConv.useLoweredAddresses()) {
    SmallVector<ManagedValue, 4> thunkArguments;
    for (auto indRes : thunkIndirectResults)
      thunkArguments.push_back(indRes);
    for (auto indErrRes : thunkIndirectErrorResults)
      thunkArguments.push_back(indErrRes);
    thunkArguments.append(params.begin(), params.end());
    SmallVector<SILParameterInfo, 4> toParameters(
        toConv.getParameters().begin(), toConv.getParameters().end());
    SmallVector<SILResultInfo, 4> toResults(toConv.getResults().begin(),
                                            toConv.getResults().end());
    // Handle self reordering.
    // - For pullbacks: reorder result infos.
    // - For differentials: reorder parameter infos and arguments.
    auto numIndirectResults =
        thunkIndirectResults.size() + thunkIndirectErrorResults.size();
    if (reorderSelf && linearMapKind == AutoDiffLinearMapKind::Pullback &&
        toResults.size() > 1) {
      std::rotate(toResults.begin(), toResults.end() - 1, toResults.end());
    }
    if (reorderSelf && linearMapKind == AutoDiffLinearMapKind::Differential &&
        thunkArguments.size() > 1) {
      // Before: [arg1, arg2, ..., arg_self, df]
      //  After: [arg_self, arg1, arg2, ..., df]
      std::rotate(thunkArguments.begin() + numIndirectResults,
                  thunkArguments.end() - 2, thunkArguments.end() - 1);
      // Before: [arg1, arg2, ..., arg_self]
      //  After: [arg_self, arg1, arg2, ...]
      std::rotate(toParameters.begin(), toParameters.end() - 1,
                  toParameters.end());
    }

    // Correctness assertions.
#ifndef NDEBUG
    assert(toType->getNumParameters() == fromType->getNumParameters());
    for (unsigned paramIdx : range(toType->getNumParameters())) {
      auto fromParam = fromConv.getParameters()[paramIdx];
      auto toParam = toParameters[paramIdx];
      assert(fromParam.getInterfaceType() == toParam.getInterfaceType());
    }
    assert(fromType->getNumResults() == toType->getNumResults());
    for (unsigned resIdx : range(toType->getNumResults())) {
      auto fromRes = fromConv.getResults()[resIdx];
      auto toRes = toResults[resIdx];
      assert(fromRes.getInterfaceType() == toRes.getInterfaceType());
    }
#endif // NDEBUG

    auto *linearMapArg = thunk->getArguments().back();
    SmallVector<SILValue, 4> arguments;
    for (unsigned paramIdx : range(toType->getNumParameters())) {
      arguments.push_back(thunkArguments[paramIdx].getValue());
    }
    auto *apply =
        thunkSGF.B.createApply(loc, linearMapArg, SubstitutionMap(), arguments);

    // Get return elements.
    SmallVector<SILValue, 4> results;
    extractAllElements(apply, loc, thunkSGF.B, results);

    // Handle self reordering.
    // For pullbacks: rotate direct results if self is direct.
    if (reorderSelf && linearMapKind == AutoDiffLinearMapKind::Pullback) {
      auto fromSelfResult = fromConv.getResults().front();
      auto toSelfResult = toConv.getResults().back();
      assert(fromSelfResult.getInterfaceType() ==
             toSelfResult.getInterfaceType());
      // Before: [dir_res_self, dir_res1, dir_res2, ...]
      //  After: [dir_res1, dir_res2, ..., dir_res_self]
      if (results.size() > 1) {
        std::rotate(results.begin(), results.begin() + 1, results.end());
      }
    }
    auto retVal = joinElements(results, thunkSGF.B, loc);

    // Emit cleanups.
    thunkSGF.Cleanups.emitCleanupsForReturn(CleanupLocation(loc), NotForUnwind);

    // Create return.
    thunkSGF.B.createReturn(loc, retVal);

    return getThunkedResult();
  }

  SmallVector<ManagedValue, 4> thunkArguments;
  thunkArguments.append(thunkIndirectResults.begin(),
                        thunkIndirectResults.end());
  thunkArguments.append(thunkIndirectErrorResults.begin(),
                        thunkIndirectErrorResults.end());
  thunkArguments.append(params.begin(), params.end());
  SmallVector<SILParameterInfo, 4> toParameters(toConv.getParameters().begin(),
                                                toConv.getParameters().end());
  SmallVector<SILResultInfo, 4> toResults(toConv.getResults().begin(),
                                          toConv.getResults().end());
  // Handle self reordering.
  // - For pullbacks: reorder result infos.
  //   - If self is indirect, reorder indirect results.
  //   - If self is direct, reorder direct results after `apply` is generated.
  // - For differentials: reorder parameter infos and arguments.
  auto numIndirectResults = thunkIndirectResults.size();
  if (reorderSelf && linearMapKind == AutoDiffLinearMapKind::Pullback &&
      toResults.size() > 1) {
    auto toSelfResult = toResults.back();
    if (toSelfResult.isFormalIndirect() && numIndirectResults > 1) {
      // Before: [ind_res1, ind_res2, ..., ind_res_self, arg1, arg2, ..., pb]
      //  After: [ind_res_self, ind_res1, ind_res2, ..., arg1, arg2, ..., pb]
      std::rotate(thunkArguments.begin(),
                  thunkArguments.begin() + numIndirectResults - 1,
                  thunkArguments.begin() + numIndirectResults);
      // Before: [ind_res1, ind_res2, ..., ind_res_self]
      //  After: [ind_res_self, ind_res1, ind_res2, ...]
      std::rotate(thunkIndirectResults.begin(), thunkIndirectResults.end() - 1,
                  thunkIndirectResults.end());
    }
    std::rotate(toResults.begin(), toResults.end() - 1, toResults.end());
  }
  if (reorderSelf && linearMapKind == AutoDiffLinearMapKind::Differential &&
      thunkArguments.size() > 1) {
    // Before: [ind_res1, ind_res2, ..., arg1, arg2, ..., arg_self, df]
    //  After: [ind_res1, ind_res2, ..., arg_self, arg1, arg2, ..., df]
    std::rotate(thunkArguments.begin() + numIndirectResults,
                thunkArguments.end() - 2, thunkArguments.end() - 1);
    // Before: [arg1, arg2, ..., arg_self]
    //  After: [arg_self, arg1, arg2, ...]
    std::rotate(toParameters.begin(), toParameters.end() - 1,
                toParameters.end());
  }

  // Correctness assertions.
#ifndef NDEBUG
  assert(toType->getNumParameters() == fromType->getNumParameters());
  for (unsigned paramIdx : range(toType->getNumParameters())) {
    auto fromParam = fromConv.getParameters()[paramIdx];
    auto toParam = toParameters[paramIdx];
    assert(fromParam.getInterfaceType() == toParam.getInterfaceType());
  }
  assert(fromType->getNumResults() == toType->getNumResults());
  for (unsigned resIdx : range(toType->getNumResults())) {
    auto fromRes = fromConv.getResults()[resIdx];
    auto toRes = toResults[resIdx];
    assert(fromRes.getInterfaceType() == toRes.getInterfaceType());
  }
#endif // NDEBUG

  // Gather arguments.
  SmallVector<SILValue, 4> arguments;
  auto toArgIter = thunkArguments.begin();
  auto useNextArgument = [&]() {
    auto nextArgument = *toArgIter++;
    arguments.push_back(nextArgument.getValue());
  };

  SmallVector<AllocStackInst *, 4> localAllocations;
  auto createAllocStack = [&](SILType type) {
    auto *alloc = thunkSGF.B.createAllocStack(loc, type);
    localAllocations.push_back(alloc);
    return alloc;
  };

  // Handle indirect results.
  for (unsigned resIdx : range(toType->getNumResults())) {
    auto fromRes = fromConv.getResults()[resIdx];
    auto toRes = toResults[resIdx];
    // No abstraction mismatch.
    if (fromRes.isFormalIndirect() == toRes.isFormalIndirect()) {
      // If result types are indirect, directly pass as next argument.
      if (toRes.isFormalIndirect())
        useNextArgument();
      continue;
    }
    // Convert indirect result to direct result.
    if (fromRes.isFormalIndirect()) {
      SILType resultTy =
          fromConv.getSILType(fromRes, thunkSGF.getTypeExpansionContext());
      assert(resultTy.isAddress());
      auto *indRes = createAllocStack(resultTy);
      arguments.push_back(indRes);
      continue;
    }
    // Convert direct result to indirect result.
    // Increment thunk argument iterator; reabstraction handled later.
    ++toArgIter;
  }

  // Reabstract parameters.
  for (unsigned paramIdx : range(toType->getNumParameters())) {
    auto fromParam = fromConv.getParameters()[paramIdx];
    auto toParam = toParameters[paramIdx];
    // No abstraction mismatch. Directly use next argument.
    if (fromParam.isFormalIndirect() == toParam.isFormalIndirect()) {
      useNextArgument();
      continue;
    }
    // Convert indirect parameter to direct parameter.
    if (fromParam.isFormalIndirect()) {
      auto paramTy = fromConv.getSILType(fromType->getParameters()[paramIdx],
                                         thunkSGF.getTypeExpansionContext());
      if (!paramTy.hasArchetype())
        paramTy = thunk->mapTypeIntoContext(paramTy);
      assert(paramTy.isAddress());
      auto toArg = (*toArgIter++).getValue();
      auto *buf = createAllocStack(toArg->getType());
      thunkSGF.B.createStore(loc, toArg, buf, StoreOwnershipQualifier::Init);
      arguments.push_back(buf);
      continue;
    }
    // Convert direct parameter to indirect parameter.
    assert(toParam.isFormalIndirect());
    auto toArg = (*toArgIter++).getValue();
    auto load = thunkSGF.emitManagedLoadBorrow(loc, toArg);
    arguments.push_back(load.getValue());
  }

  auto *linearMapArg = thunk->getArgumentsWithoutIndirectResults().back();
  auto *apply = thunkSGF.B.createApply(loc, linearMapArg, SubstitutionMap(),
                                       arguments);

  // Get return elements.
  SmallVector<SILValue, 4> results;
  // Extract all direct results.
  SmallVector<SILValue, 4> directResults;
  extractAllElements(apply, loc, thunkSGF.B, directResults);

  // Handle self reordering.
  // For pullbacks: rotate direct results if self is direct.
  if (reorderSelf && linearMapKind == AutoDiffLinearMapKind::Pullback) {
    auto fromSelfResult = fromConv.getResults().front();
    auto toSelfResult = toConv.getResults().back();
    assert(fromSelfResult.getInterfaceType() ==
           toSelfResult.getInterfaceType());
    // Before: [dir_res_self, dir_res1, dir_res2, ...]
    //  After: [dir_res1, dir_res2, ..., dir_res_self]
    if (toSelfResult.isFormalDirect() && fromSelfResult.isFormalDirect() &&
        directResults.size() > 1) {
      std::rotate(directResults.begin(), directResults.begin() + 1,
                  directResults.end());
    }
  }

  auto fromDirResultsIter = directResults.begin();
  auto fromIndResultsIter = apply->getIndirectSILResults().begin();
  auto toIndResultsIter = thunkIndirectResults.begin();
  // Reabstract results.
  for (unsigned resIdx : range(toType->getNumResults())) {
    auto fromRes = fromConv.getResults()[resIdx];
    auto toRes = toResults[resIdx];
    // No abstraction mismatch.
    if (fromRes.isFormalIndirect() == toRes.isFormalIndirect()) {
      // If result types are direct, add call result as direct thunk result.
      if (toRes.isFormalDirect())
        results.push_back(*fromDirResultsIter++);
      // If result types are indirect, increment indirect result iterators.
      else {
        ++fromIndResultsIter;
        ++toIndResultsIter;
      }
      continue;
    }
    // Load direct results from indirect results.
    if (fromRes.isFormalIndirect()) {
      auto indRes = *fromIndResultsIter++;
      auto *load = thunkSGF.B.createLoad(loc, indRes,
                                         LoadOwnershipQualifier::Unqualified);
      results.push_back(load);
      continue;
    }
    // Store direct results to indirect results.
    assert(toRes.isFormalIndirect());
    SILType resultTy =
        toConv.getSILType(toRes, thunkSGF.getTypeExpansionContext());
    assert(resultTy.isAddress());
    auto indRes = *toIndResultsIter++;
    thunkSGF.emitSemanticStore(loc, *fromDirResultsIter++,
                               indRes.getLValueAddress(),
                               thunkSGF.getTypeLowering(resultTy),
                               IsInitialization);
  }
  auto retVal = joinElements(results, thunkSGF.B, loc);

  // Emit cleanups.
  thunkSGF.Cleanups.emitCleanupsForReturn(CleanupLocation(loc),
                                          NotForUnwind);

  // Deallocate local allocations.
  for (auto *alloc : llvm::reverse(localAllocations))
    thunkSGF.B.createDeallocStack(loc, alloc);

  // Create return.
  thunkSGF.B.createReturn(loc, retVal);
  return getThunkedResult();
}

SILFunction *SILGenModule::getOrCreateCustomDerivativeThunk(
    AbstractFunctionDecl *originalAFD, SILFunction *originalFn,
    SILFunction *customDerivativeFn, const AutoDiffConfig &config,
    AutoDiffDerivativeFunctionKind kind) {
  auto customDerivativeFnTy = customDerivativeFn->getLoweredFunctionType();
  auto *thunkGenericEnv = customDerivativeFnTy->getSubstGenericSignature().getGenericEnvironment();

  auto origFnTy = originalFn->getLoweredFunctionType();
  auto derivativeCanGenSig = config.derivativeGenericSignature.getCanonicalSignature();
  auto thunkFnTy = origFnTy->getAutoDiffDerivativeFunctionType(
      config.parameterIndices, config.resultIndices, kind, Types,
      LookUpConformanceInModule(M.getSwiftModule()), derivativeCanGenSig);
  assert(!thunkFnTy->getExtInfo().hasContext());

  Mangle::ASTMangler mangler;
  auto name = getASTContext()
      .getIdentifier(
          mangler.mangleAutoDiffDerivativeFunction(originalAFD, kind, config))
      .str();

  auto loc = customDerivativeFn->getLocation();
  SILGenFunctionBuilder fb(*this);
  // Derivative thunks have the same linkage as the original function, stripping
  // external.
  auto linkage = stripExternalFromLinkage(originalFn->getLinkage());
  auto *thunk = fb.getOrCreateFunction(
      loc, name, linkage, thunkFnTy, IsBare, IsNotTransparent,
      customDerivativeFn->getSerializedKind(),
      customDerivativeFn->isDynamicallyReplaceable(),
      customDerivativeFn->isDistributed(),
      customDerivativeFn->isRuntimeAccessible(),
      customDerivativeFn->getEntryCount(), IsThunk,
      customDerivativeFn->getClassSubclassScope());
  // This thunk may be publicly exposed and cannot be transparent.
  // Instead, mark it as "always inline" for optimization.
  thunk->setInlineStrategy(AlwaysInline);
  if (!thunk->empty())
    return thunk;
  thunk->setGenericEnvironment(thunkGenericEnv);

  SILGenFunction thunkSGF(*this, *thunk, customDerivativeFn->getDeclContext());
  SmallVector<ManagedValue, 4> params;
  SmallVector<ManagedValue, 4> indirectResults;
  SmallVector<ManagedValue, 1> indirectErrorResults;
  thunkSGF.collectThunkParams(
      loc, params, &indirectResults, &indirectErrorResults);

  auto *fnRef = thunkSGF.B.createFunctionRef(loc, customDerivativeFn);
  auto fnRefType =
      thunkSGF.F.mapTypeIntoContext(fnRef->getType().mapTypeOutOfContext())
          .castTo<SILFunctionType>()
          ->getUnsubstitutedType(M);

  // Special support for thunking class initializer derivatives.
  //
  // User-defined custom derivatives take a metatype as the last parameter:
  // - `$(Param0, Param1, ..., @thick Class.Type) -> (...)`
  // But class initializers take an allocated instance as the last parameter:
  // - `$(Param0, Param1, ..., @owned Class) -> (...)`
  //
  // Adjust forwarded arguments:
  // - Pop the last `@owned Class` argument.
  // - Create a `@thick Class.Type` value and pass it as the last argument.
  auto *origAFD =
      cast<AbstractFunctionDecl>(originalFn->getDeclContext()->getAsDecl());
  bool isClassInitializer =
      isa<ConstructorDecl>(origAFD) &&
      origAFD->getDeclContext()->getSelfClassDecl() &&
      SILDeclRef(origAFD, SILDeclRef::Kind::Initializer).mangle() ==
          originalFn->getName();
  if (isClassInitializer) {
    params.pop_back();
    auto *classDecl = thunkFnTy->getParameters()
                          .back()
                          .getInterfaceType()
                          ->getClassOrBoundGenericClass();
    assert(classDecl && "Expected last argument to have class type");
    auto classMetatype = MetatypeType::get(
        classDecl->getDeclaredInterfaceType(), MetatypeRepresentation::Thick);
    auto canClassMetatype = classMetatype->getCanonicalType();
    auto *metatype = thunkSGF.B.createMetatype(
        loc, SILType::getPrimitiveObjectType(canClassMetatype));
    params.push_back(ManagedValue::forObjectRValueWithoutOwnership(metatype));
  }

  // Collect thunk arguments, converting ownership.
  SmallVector<SILValue, 8> arguments;
  for (auto indRes : indirectResults)
    arguments.push_back(indRes.getLValueAddress());
  for (auto indErrorRes : indirectErrorResults)
    arguments.push_back(indErrorRes.getLValueAddress());
  forwardFunctionArguments(thunkSGF, loc, fnRefType, params, arguments);

  SubstitutionMap subs = thunk->getForwardingSubstitutionMap();
  SILType substFnType = fnRef->getType().substGenericArgs(
      M, subs, thunk->getTypeExpansionContext());

  // Apply function argument.
  auto apply =
      thunkSGF.emitApplyWithRethrow(loc, fnRef, substFnType, subs, arguments);

  // Self reordering thunk is necessary if wrt at least two parameters,
  // including self.
  auto shouldReorderSelf = [&]() {
    if (!originalFn->hasSelfParam())
      return false;
    auto selfParamIndex = origFnTy->getNumParameters() - 1;
    if (!config.parameterIndices->contains(selfParamIndex))
      return false;
    return config.parameterIndices->getNumIndices() > 1;
  };
  bool reorderSelf = shouldReorderSelf();

  // If self ordering is not necessary and linear map types are unchanged,
  // return the `apply` instruction.
  auto linearMapFnType = cast<SILFunctionType>(
      thunk
          ->mapTypeIntoContext(
              fnRefType->getResults().back().getInterfaceType())
          ->getCanonicalType());
  auto targetLinearMapFnType =
      thunk
          ->mapTypeIntoContext(
              thunkFnTy->getResults().back().getSILStorageInterfaceType())
          .castTo<SILFunctionType>();
  SILFunctionConventions conv(thunkFnTy, thunkSGF.getModule());

  // Create return instruction in the thunk, first deallocating local
  // allocations and freeing arguments-to-free.
  auto createReturn = [&](SILValue retValue) {
    // Emit cleanups.
    thunkSGF.Cleanups.emitCleanupsForReturn(CleanupLocation(loc),
                                            NotForUnwind);
    // Create return.
    thunkSGF.B.createReturn(loc, retValue);
  };

  if (!reorderSelf && linearMapFnType == targetLinearMapFnType) {
    SmallVector<SILValue, 8> results;
    extractAllElements(apply, loc, thunkSGF.B, results);
    auto result = joinElements(results, thunkSGF.B, apply.getLoc());
    createReturn(result);
    return thunk;
  }

  // Otherwise, apply reabstraction/self reordering thunk to linear map.
  SmallVector<SILValue, 8> directResults;
  extractAllElements(apply, loc, thunkSGF.B, directResults);
  auto linearMap = thunkSGF.emitManagedRValueWithCleanup(directResults.back());
  assert(linearMap.getType().castTo<SILFunctionType>() == linearMapFnType);
  auto linearMapKind = kind.getLinearMapKind();
  linearMap = thunkSGF.getThunkedAutoDiffLinearMap(
      linearMap, linearMapKind, linearMapFnType, targetLinearMapFnType,
      reorderSelf);
  auto typeExpansionContext = thunkSGF.getTypeExpansionContext();
  SILType linearMapResultType =
      thunk
          ->getLoweredType(thunk
                               ->mapTypeIntoContext(
                                   conv.getSILResultType(typeExpansionContext))
                               .getASTType())
          .getCategoryType(
              conv.getSILResultType(typeExpansionContext).getCategory());
  if (auto tupleType = linearMapResultType.getAs<TupleType>()) {
    linearMapResultType = SILType::getPrimitiveType(
        tupleType->getElementTypes().back()->getCanonicalType(),
        conv.getSILResultType(typeExpansionContext).getCategory());
  }

  auto targetLinearMapUnsubstFnType =
      SILType::getPrimitiveObjectType(targetLinearMapFnType);
  if (linearMap.getType() != targetLinearMapUnsubstFnType) {
    linearMap = thunkSGF.B.createConvertFunction(
        loc, linearMap, targetLinearMapUnsubstFnType,
        /*withoutActuallyEscaping*/ false);
  }

  // Return original results and thunked differential/pullback.
  if (directResults.size() > 1) {
    auto originalDirectResults = ArrayRef<SILValue>(directResults).drop_back(1);
    auto originalDirectResult =
        joinElements(originalDirectResults, thunkSGF.B, apply.getLoc());
    auto thunkResult = joinElements(
        {originalDirectResult, linearMap.forward(thunkSGF)}, thunkSGF.B, loc);
    createReturn(thunkResult);
  } else {
    createReturn(linearMap.forward(thunkSGF));
  }
  return thunk;
}

static bool isUnimplementableVariadicFunctionAbstraction(AbstractionPattern origType,
                                                         CanAnyFunctionType substType) {
  return origType.isTypeParameterOrOpaqueArchetype() &&
         substType->containsPackExpansionParam();
}

ManagedValue Transform::transformFunction(ManagedValue fn,
                                          AbstractionPattern inputOrigType,
                                          CanAnyFunctionType inputSubstType,
                                          AbstractionPattern outputOrigType,
                                          CanAnyFunctionType outputSubstType,
                                          const TypeLowering &expectedTL) {
  assert(fn.getType().isObject() &&
         "expected input to emitTransformedFunctionValue to be loaded");

  auto expectedFnType = expectedTL.getLoweredType().castTo<SILFunctionType>();

  auto fnType = fn.getType().castTo<SILFunctionType>();
  assert(expectedFnType->getExtInfo().hasContext()
         || !fnType->getExtInfo().hasContext());

  // If there's no abstraction difference, we're done.
  if (fnType == expectedFnType) {
    return fn;
  }

  // Check for unimplementable functions.
  if (fnType->isUnimplementable() || expectedFnType->isUnimplementable()) {
    if (isUnimplementableVariadicFunctionAbstraction(inputOrigType,
                                                     inputSubstType)) {
      SGF.SGM.diagnose(Loc, diag::unsupported_variadic_function_abstraction,
                       inputSubstType);
    } else {
      assert(isUnimplementableVariadicFunctionAbstraction(outputOrigType,
                                                          outputSubstType));
      SGF.SGM.diagnose(Loc, diag::unsupported_variadic_function_abstraction,
                       outputSubstType);
    }

    return SGF.emitUndef(expectedFnType);
  }

  // Check if we require a re-abstraction thunk.
  if (SGF.SGM.Types.checkForABIDifferences(SGF.SGM.M,
                              SILType::getPrimitiveObjectType(fnType),
                              SILType::getPrimitiveObjectType(expectedFnType))
        == TypeConverter::ABIDifference::NeedsThunk) {
    assert(expectedFnType->getExtInfo().hasContext()
           && "conversion thunk will not be thin!");
    return createThunk(SGF, Loc, fn,
                       inputOrigType, inputSubstType,
                       outputOrigType, outputSubstType,
                       expectedTL);
  }

  // We do not, conversion is trivial.
  auto expectedEI = expectedFnType->getExtInfo().intoBuilder();
  auto newEI = expectedEI.withRepresentation(fnType->getRepresentation())
                   .withNoEscape(fnType->getRepresentation() ==
                                         SILFunctionType::Representation::Thick
                                     ? fnType->isNoEscape()
                                     : expectedFnType->isNoEscape())
                   .build();
  auto newFnType =
      adjustFunctionType(expectedFnType, newEI, fnType->getCalleeConvention(),
                         fnType->getWitnessMethodConformanceOrInvalid());

  // Apply any ABI-compatible conversions before doing thin-to-thick or
  // escaping->noescape conversion.
  if (fnType != newFnType) {
    SILType resTy = SILType::getPrimitiveObjectType(newFnType);
    fn = SGF.B.createConvertFunction(Loc, fn.ensurePlusOne(SGF, Loc), resTy);
  }

  // Now do thin-to-thick if necessary.
  if (newFnType != expectedFnType &&
      fnType->getRepresentation() == SILFunctionTypeRepresentation::Thin) {
    assert(expectedEI.getRepresentation() ==
           SILFunctionTypeRepresentation::Thick &&
           "all other conversions should have been handled by "
           "FunctionConversionExpr");
    SILType resTy = SILType::getPrimitiveObjectType(expectedFnType);
    fn = SGF.emitManagedRValueWithCleanup(
        SGF.B.createThinToThickFunction(Loc, fn.forward(SGF), resTy));

  } else if (newFnType != expectedFnType) {
    // Escaping to noescape conversion.
    SILType resTy = SILType::getPrimitiveObjectType(expectedFnType);
    fn = SGF.B.createConvertEscapeToNoEscape(Loc, fn, resTy);
  }

  return fn;
}

/// Given a value with the abstraction patterns of the original formal
/// type, give it the abstraction patterns of the substituted formal type.
ManagedValue
SILGenFunction::emitOrigToSubstValue(SILLocation loc, ManagedValue v,
                                     AbstractionPattern origType,
                                     CanType substType,
                                     SGFContext ctxt) {
  return emitOrigToSubstValue(loc, v, origType, substType,
                              getLoweredType(substType), ctxt);
}
ManagedValue
SILGenFunction::emitOrigToSubstValue(SILLocation loc, ManagedValue v,
                                     AbstractionPattern origType,
                                     CanType substType,
                                     SILType loweredResultTy,
                                     SGFContext ctxt) {
  return emitTransformedValue(loc, v,
                              origType, substType,
                              AbstractionPattern(substType), substType,
                              loweredResultTy, ctxt);
}

/// Given a value with the abstraction patterns of the original formal
/// type, give it the abstraction patterns of the substituted formal type.
RValue SILGenFunction::emitOrigToSubstValue(SILLocation loc, RValue &&v,
                                            AbstractionPattern origType,
                                            CanType substType,
                                            SGFContext ctxt) {
  return emitOrigToSubstValue(loc, std::move(v), origType, substType,
                              getLoweredType(substType), ctxt);
}
RValue SILGenFunction::emitOrigToSubstValue(SILLocation loc, RValue &&v,
                                            AbstractionPattern origType,
                                            CanType substType,
                                            SILType loweredResultTy,
                                            SGFContext ctxt) {
  return emitTransformedValue(loc, std::move(v),
                              origType, substType,
                              AbstractionPattern(substType), substType,
                              loweredResultTy,  ctxt);
}

/// Given a value with the abstraction patterns of the substituted
/// formal type, give it the abstraction patterns of the original
/// formal type.
ManagedValue
SILGenFunction::emitSubstToOrigValue(SILLocation loc, ManagedValue v,
                                     AbstractionPattern origType,
                                     CanType substType,
                                     SGFContext ctxt) {
  return emitSubstToOrigValue(loc, v, origType, substType,
                              getLoweredType(origType, substType), ctxt);
}

ManagedValue
SILGenFunction::emitSubstToOrigValue(SILLocation loc, ManagedValue v,
                                     AbstractionPattern origType,
                                     CanType substType,
                                     SILType loweredResultTy,
                                     SGFContext ctxt) {
  return emitTransformedValue(loc, v,
                              AbstractionPattern(substType), substType,
                              origType, substType,
                              loweredResultTy, ctxt);
}

/// Given a value with the abstraction patterns of the substituted
/// formal type, give it the abstraction patterns of the original
/// formal type.
RValue SILGenFunction::emitSubstToOrigValue(SILLocation loc, RValue &&v,
                                            AbstractionPattern origType,
                                            CanType substType,
                                            SGFContext ctxt) {
  return emitSubstToOrigValue(loc, std::move(v), origType, substType,
                              getLoweredType(origType, substType), ctxt);
}

RValue SILGenFunction::emitSubstToOrigValue(SILLocation loc, RValue &&v,
                                            AbstractionPattern origType,
                                            CanType substType,
                                            SILType loweredResultTy,
                                            SGFContext ctxt) {
  return emitTransformedValue(loc, std::move(v),
                              AbstractionPattern(substType), substType,
                              origType, substType,
                              loweredResultTy, ctxt);
}

ManagedValue
SILGenFunction::emitMaterializedRValueAsOrig(Expr *expr,
                                             AbstractionPattern origType) {  
  // Create a temporary.
  auto &origTL = getTypeLowering(origType, expr->getType());
  auto temporary = emitTemporary(expr, origTL);

  // Emit the reabstracted r-value.
  auto result =
    emitRValueAsOrig(expr, origType, origTL, SGFContext(temporary.get()));

  // Force the result into the temporary.
  if (!result.isInContext()) {
    temporary->copyOrInitValueInto(*this, expr, result, /*init*/ true);
    temporary->finishInitialization(*this);
  }

  return temporary->getManagedAddress();
}

ManagedValue
SILGenFunction::emitRValueAsOrig(Expr *expr, AbstractionPattern origPattern,
                                 const TypeLowering &origTL, SGFContext ctxt) {
  auto outputSubstType = expr->getType()->getCanonicalType();
  auto &substTL = getTypeLowering(outputSubstType);
  if (substTL.getLoweredType() == origTL.getLoweredType())
    return emitRValueAsSingleValue(expr, ctxt);

  ManagedValue temp = emitRValueAsSingleValue(expr);
  return emitSubstToOrigValue(expr, temp, origPattern,
                              outputSubstType, ctxt);
}

ManagedValue
SILGenFunction::emitTransformedValue(SILLocation loc, ManagedValue v,
                                     CanType inputType,
                                     CanType outputType,
                                     SGFContext ctxt) {
  return emitTransformedValue(loc, v,
                              AbstractionPattern(inputType), inputType,
                              AbstractionPattern(outputType), outputType,
                              getLoweredType(outputType),
                              ctxt);
}

ManagedValue
SILGenFunction::emitTransformedValue(SILLocation loc, ManagedValue v,
                                     AbstractionPattern inputOrigType,
                                     CanType inputSubstType,
                                     AbstractionPattern outputOrigType,
                                     CanType outputSubstType,
                                     SILType outputLoweredTy,
                                     SGFContext ctxt) {
  return Transform(*this, loc).transform(v,
                                         inputOrigType,
                                         inputSubstType,
                                         outputOrigType,
                                         outputSubstType,
                                         outputLoweredTy, ctxt);
}

RValue
SILGenFunction::emitTransformedValue(SILLocation loc, RValue &&v,
                                     AbstractionPattern inputOrigType,
                                     CanType inputSubstType,
                                     AbstractionPattern outputOrigType,
                                     CanType outputSubstType,
                                     SILType outputLoweredTy,
                                     SGFContext ctxt) {
  return Transform(*this, loc).transform(std::move(v),
                                         inputOrigType,
                                         inputSubstType,
                                         outputOrigType,
                                         outputSubstType, 
                                         outputLoweredTy, ctxt);
}

//===----------------------------------------------------------------------===//
// vtable thunks
//===----------------------------------------------------------------------===//

void
SILGenFunction::emitVTableThunk(SILDeclRef base,
                                SILDeclRef derived,
                                SILFunction *implFn,
                                AbstractionPattern inputOrigType,
                                CanAnyFunctionType inputSubstType,
                                CanAnyFunctionType outputSubstType,
                                bool baseLessVisibleThanDerived) {
  auto fd = cast<AbstractFunctionDecl>(derived.getDecl());

  SILLocation loc(fd);
  loc.markAutoGenerated();
  CleanupLocation cleanupLoc(fd);
  cleanupLoc.markAutoGenerated();
  Scope scope(Cleanups, cleanupLoc);

  SmallVector<ManagedValue, 8> thunkArgs;
  SmallVector<ManagedValue, 8> thunkIndirectResults;
  collectThunkParams(loc, thunkArgs, &thunkIndirectResults);

  CanSILFunctionType derivedFTy;
  if (baseLessVisibleThanDerived) {
    derivedFTy =
        SGM.Types.getConstantOverrideType(getTypeExpansionContext(), derived);
  } else {
    derivedFTy =
        SGM.Types.getConstantInfo(getTypeExpansionContext(), derived).SILFnType;
  }

  auto subs = getForwardingSubstitutionMap();
  if (auto genericSig = derivedFTy->getInvocationGenericSignature()) {
    subs = SubstitutionMap::get(genericSig, subs);

    derivedFTy =
        derivedFTy->substGenericArgs(SGM.M, subs, getTypeExpansionContext());

    inputSubstType = cast<FunctionType>(
        cast<GenericFunctionType>(inputSubstType)
            ->substGenericArgs(subs)->getCanonicalType());
    outputSubstType = cast<FunctionType>(
        cast<GenericFunctionType>(outputSubstType)
            ->substGenericArgs(subs)->getCanonicalType());
  }

  // Emit the indirect return and arguments.
  auto thunkTy = F.getLoweredFunctionType();

  SmallVector<ManagedValue, 8> substArgs;

  AbstractionPattern outputOrigType(outputSubstType);

  // Reabstract the arguments.
  TranslateArguments(*this, loc, thunkArgs, substArgs,
                     derivedFTy, derivedFTy->getParameters())
    .process(outputOrigType,
             outputSubstType.getParams(),
             inputOrigType,
             inputSubstType.getParams());
  
  auto coroutineKind = F.getLoweredFunctionType()->getCoroutineKind();

  // Collect the arguments to the implementation.
  SmallVector<SILValue, 8> args;

  std::optional<ResultPlanner> resultPlanner;

  if (coroutineKind == SILCoroutineKind::None) {
    // First, indirect results.
    resultPlanner.emplace(*this, loc, thunkIndirectResults, args);
    resultPlanner->plan(outputOrigType.getFunctionResultType(),
                        outputSubstType.getResult(),
                        inputOrigType.getFunctionResultType(),
                        inputSubstType.getResult(),
                        derivedFTy, thunkTy);

    // If the function we're calling has an indirect error result, create an
    // argument for it.
    if (auto innerIndirectErrorAddr =
            emitThunkIndirectErrorArgument(*this, loc, derivedFTy)) {
      args.push_back(*innerIndirectErrorAddr);
    }
  }

  // Then, the arguments.
  forwardFunctionArguments(*this, loc, derivedFTy, substArgs, args);

  // Create the call.
  SILValue derivedRef;
  if (baseLessVisibleThanDerived) {
    // See the comment in SILVTableVisitor.h under maybeAddMethod().
    auto selfValue = thunkArgs.back().getValue();
    auto derivedTy =
        SGM.Types.getConstantOverrideType(getTypeExpansionContext(), derived);
    derivedRef = emitClassMethodRef(loc, selfValue, derived, derivedTy);
  } else {
    derivedRef = B.createFunctionRefFor(loc, implFn);
  }

  SILValue result;

  switch (coroutineKind) {
  case SILCoroutineKind::None: {
    auto implResult =
      emitApplyWithRethrow(loc, derivedRef,
                            SILType::getPrimitiveObjectType(derivedFTy),
                            subs, args);

    // Reabstract the return.
    result = resultPlanner->execute(implResult, derivedFTy);
    break;
  }

  case SILCoroutineKind::YieldOnce: {
    SmallVector<SILValue, 4> derivedYields;
    auto tokenAndCleanup =
        emitBeginApplyWithRethrow(loc, derivedRef,
                                  SILType::getPrimitiveObjectType(derivedFTy),
                                  subs, args, derivedYields);
    auto overrideSubs = SubstitutionMap::getOverrideSubstitutions(
        base.getDecl(), derived.getDecl()).subst(subs);

    YieldInfo derivedYieldInfo(SGM, derived, derivedFTy, subs);
    YieldInfo baseYieldInfo(SGM, base, thunkTy, overrideSubs);

    translateYields(*this, loc, derivedYields, derivedYieldInfo, baseYieldInfo);

    // Kill the abort cleanup without emitting it.
    Cleanups.setCleanupState(tokenAndCleanup.second, CleanupState::Dead);

    // End the inner coroutine normally.
    emitEndApplyWithRethrow(loc, tokenAndCleanup.first);

    result = B.createTuple(loc, {});
    break;
  }

  case SILCoroutineKind::YieldMany:
    SGM.diagnose(loc, diag::unimplemented_generator_witnesses);
    result = B.createTuple(loc, {});
    break;
  }
  
  scope.pop();
  B.createReturn(loc, result);

  // Now that the thunk body has been completely emitted, verify the
  // body early.
  F.verify();
}

//===----------------------------------------------------------------------===//
// Protocol witnesses
//===----------------------------------------------------------------------===//

enum class WitnessDispatchKind {
  Static,
  Dynamic,
  Class,
  Witness
};

static WitnessDispatchKind getWitnessDispatchKind(SILDeclRef witness,
                                                  bool isSelfConformance) {
  auto *decl = witness.getDecl();

  if (isSelfConformance) {
    assert(isa<ProtocolDecl>(decl->getDeclContext()));
    return WitnessDispatchKind::Witness;
  }

  ClassDecl *C = decl->getDeclContext()->getSelfClassDecl();
  if (!C) {
    return WitnessDispatchKind::Static;
  }

  // If the witness is dynamic, go through dynamic dispatch.
  if (decl->shouldUseObjCDispatch()) {
    // For initializers we still emit a static allocating thunk around
    // the dynamic initializing entry point.
    if (witness.kind == SILDeclRef::Kind::Allocator)
      return WitnessDispatchKind::Static;
    return WitnessDispatchKind::Dynamic;
  }

  bool isFinal = (decl->isFinal() || C->isFinal());
  if (auto fnDecl = dyn_cast<AbstractFunctionDecl>(witness.getDecl()))
    isFinal |= fnDecl->hasForcedStaticDispatch();

  bool isExtension = isa<ExtensionDecl>(decl->getDeclContext());

  // If we have a final method or a method from an extension that is not
  // Objective-C, emit a static reference.
  // A natively ObjC method witness referenced this way will end up going
  // through its native thunk, which will redispatch the method after doing
  // bridging just like we want.
  if (isFinal || isExtension || witness.isForeignToNativeThunk())
    return WitnessDispatchKind::Static;

  if (witness.kind == SILDeclRef::Kind::Allocator) {
    // Non-required initializers can witness a protocol requirement if the class
    // is final, so we can statically dispatch to them.
    if (!cast<ConstructorDecl>(decl)->isRequired())
      return WitnessDispatchKind::Static;

    // We emit a static thunk for ObjC allocating constructors.
    if (decl->hasClangNode())
      return WitnessDispatchKind::Static;
  }

  // Otherwise emit a class method.
  return WitnessDispatchKind::Class;
}

static CanSILFunctionType
getWitnessFunctionType(TypeExpansionContext context, SILGenModule &SGM,
                       SILDeclRef witness, WitnessDispatchKind witnessKind) {
  switch (witnessKind) {
  case WitnessDispatchKind::Static:
  case WitnessDispatchKind::Dynamic:
  case WitnessDispatchKind::Witness:
    return SGM.Types.getConstantInfo(context, witness).SILFnType;
  case WitnessDispatchKind::Class:
    return SGM.Types.getConstantOverrideType(context, witness);
  }

  llvm_unreachable("Unhandled WitnessDispatchKind in switch.");
}

static std::pair<CanType, ProtocolConformanceRef>
getSelfTypeAndConformanceForWitness(SILDeclRef witness, SubstitutionMap subs) {
  auto protocol = cast<ProtocolDecl>(witness.getDecl()->getDeclContext());
  auto selfParam = protocol->getSelfInterfaceType()->getCanonicalType();
  auto type = subs.getReplacementTypes()[0];
  auto conf = subs.lookupConformance(selfParam, protocol);
  return {type->getCanonicalType(), conf};
}

static SILValue
getWitnessFunctionRef(SILGenFunction &SGF,
                      SILDeclRef witness,
                      CanSILFunctionType witnessFTy,
                      WitnessDispatchKind witnessKind,
                      SubstitutionMap witnessSubs,
                      SmallVectorImpl<ManagedValue> &witnessParams,
                      SILLocation loc) {
  switch (witnessKind) {
  case WitnessDispatchKind::Static:
    if (auto *derivativeId = witness.getDerivativeFunctionIdentifier()) { // TODO Maybe we need check here too
      auto originalFn =
          SGF.emitGlobalFunctionRef(loc, witness.asAutoDiffOriginalFunction());
      auto *loweredParamIndices = autodiff::getLoweredParameterIndices(
          derivativeId->getParameterIndices(),
          witness.getDecl()->getInterfaceType()->castTo<AnyFunctionType>());
      // FIXME: is this correct in the presence of curried types?
      auto *resultIndices = autodiff::getFunctionSemanticResultIndices(
        witness.getDecl()->getInterfaceType()->castTo<AnyFunctionType>(),
        derivativeId->getParameterIndices());
      auto diffFn = SGF.B.createDifferentiableFunction(
          loc, loweredParamIndices, resultIndices, originalFn);
      return SGF.B.createDifferentiableFunctionExtract(
          loc,
          NormalDifferentiableFunctionTypeComponent(derivativeId->getKind()),
          diffFn);
    }
    return SGF.emitGlobalFunctionRef(loc, witness);
  case WitnessDispatchKind::Dynamic:
    assert(!witness.getDerivativeFunctionIdentifier());
    return SGF.emitDynamicMethodRef(loc, witness, witnessFTy).getValue();
  case WitnessDispatchKind::Witness: {
    auto typeAndConf =
      getSelfTypeAndConformanceForWitness(witness, witnessSubs);
    return SGF.B.createWitnessMethod(loc, typeAndConf.first,
                                     typeAndConf.second,
                                     witness,
                            SILType::getPrimitiveObjectType(witnessFTy));
  }
  case WitnessDispatchKind::Class: {
    SILValue selfPtr = witnessParams.back().getValue();
    // If `witness` is a derivative function `SILDeclRef`, replace the
    // derivative function identifier's generic signature with the witness thunk
    // substitution map's generic signature.
    if (auto *derivativeId = witness.getDerivativeFunctionIdentifier()) {
      auto *newDerivativeId = AutoDiffDerivativeFunctionIdentifier::get(
          derivativeId->getKind(), derivativeId->getParameterIndices(),
          witnessSubs.getGenericSignature(), SGF.getASTContext());
      return SGF.emitClassMethodRef(
          loc, selfPtr, witness.asAutoDiffDerivativeFunction(newDerivativeId),
          witnessFTy);
    }
    return SGF.emitClassMethodRef(loc, selfPtr, witness, witnessFTy);
  }
  }

  llvm_unreachable("Unhandled WitnessDispatchKind in switch.");
}

static ManagedValue
emitOpenExistentialInSelfConformance(SILGenFunction &SGF, SILLocation loc,
                                     SILDeclRef witness,
                                     SubstitutionMap subs, ManagedValue value,
                                     SILParameterInfo destParameter) {
  auto openedTy = destParameter.getSILStorageInterfaceType();
  return SGF.emitOpenExistential(loc, value, openedTy,
                                 destParameter.isIndirectMutating()
                                   ? AccessKind::ReadWrite
                                   : AccessKind::Read);
}

void SILGenFunction::emitProtocolWitness(
    AbstractionPattern reqtOrigTy, CanAnyFunctionType reqtSubstTy,
    SILDeclRef requirement, SubstitutionMap reqtSubs, SILDeclRef witness,
    SubstitutionMap witnessSubs, IsFreeFunctionWitness_t isFree,
    bool isSelfConformance, bool isPreconcurrency,
    std::optional<ActorIsolation> enterIsolation) {
  // FIXME: Disable checks that the protocol witness carries debug info.
  // Should we carry debug info for witnesses?
  F.setBare(IsBare);

  SILLocation loc(witness.getDecl());
  loc.markAutoGenerated();

  CleanupLocation cleanupLoc(witness.getDecl());
  cleanupLoc.markAutoGenerated();

  FullExpr scope(Cleanups, cleanupLoc);
  FormalEvaluationScope formalEvalScope(*this);

  auto thunkTy = F.getLoweredFunctionType();

  SmallVector<ManagedValue, 8> origParams;
  SmallVector<ManagedValue, 8> thunkIndirectResults;
  collectThunkParams(loc, origParams, &thunkIndirectResults);

  if (witness.getDecl()->requiresUnavailableDeclABICompatibilityStubs())
    emitApplyOfUnavailableCodeReached();

  if (enterIsolation) {
    // If we are supposed to enter the actor, do so now by hopping to the
    // actor.
    std::optional<ManagedValue> actorSelf;

    // For an instance actor, get the actor 'self'.
    if (*enterIsolation == ActorIsolation::ActorInstance) {
      assert(enterIsolation->getActorInstanceParameter() == 0 && "Not self?");
      auto actorSelfVal = origParams.back();

      if (actorSelfVal.getType().isAddress()) {
        auto &actorSelfTL = getTypeLowering(actorSelfVal.getType());
        if (!actorSelfTL.isAddressOnly()) {
          actorSelfVal = emitManagedLoad(
              *this, loc, actorSelfVal, actorSelfTL);
        }
      }

      actorSelf = actorSelfVal;
    }

    if (!F.isAsync()) {
      assert(isPreconcurrency);

      if (getASTContext().LangOpts.isDynamicActorIsolationCheckingEnabled()) {
        emitPreconditionCheckExpectedExecutor(loc, *enterIsolation, actorSelf);
      }
    } else {
      emitHopToTargetActor(loc, enterIsolation, actorSelf);
    }
  }

  // Get the type of the witness.
  auto witnessKind = getWitnessDispatchKind(witness, isSelfConformance);
  auto witnessInfo = getConstantInfo(getTypeExpansionContext(), witness);
  CanAnyFunctionType witnessSubstTy = witnessInfo.LoweredType;
  if (auto genericFnType = dyn_cast<GenericFunctionType>(witnessSubstTy)) {
    witnessSubstTy = cast<FunctionType>(genericFnType
                                          ->substGenericArgs(witnessSubs)
                                          ->getCanonicalType());
  }

  assert(!witnessSubstTy->hasError());

  if (auto genericFnType = dyn_cast<GenericFunctionType>(reqtSubstTy)) {
    auto forwardingSubs = F.getForwardingSubstitutionMap();
    reqtSubstTy = cast<FunctionType>(genericFnType
                                          ->substGenericArgs(forwardingSubs)
                                          ->getCanonicalType());
  } else {
    reqtSubstTy = cast<FunctionType>(F.mapTypeIntoContext(reqtSubstTy)
                                          ->getCanonicalType());
  }

  assert(!reqtSubstTy->hasError());

  // Get the lowered type of the witness.
  auto origWitnessFTy = getWitnessFunctionType(getTypeExpansionContext(), SGM,
                                               witness, witnessKind);
  auto witnessFTy = origWitnessFTy;
  if (!witnessSubs.empty()) {
    witnessFTy = origWitnessFTy->substGenericArgs(SGM.M, witnessSubs,
                                                  getTypeExpansionContext());
  }
  auto witnessUnsubstTy = witnessFTy->getUnsubstitutedType(SGM.M);

  auto reqtSubstParams = reqtSubstTy.getParams();
  auto witnessSubstParams = witnessSubstTy.getParams();

  // For a self-conformance, open the self parameter.
  if (isSelfConformance) {
    assert(!isFree && "shouldn't have a free witness for a self-conformance");
    origParams.back() =
      emitOpenExistentialInSelfConformance(*this, loc, witness, witnessSubs,
                                         origParams.back(),
                                         witnessUnsubstTy->getSelfParameter());
  }

  bool ignoreFinalInputOrigParam = false;

  // For static C++ methods and constructors, we need to drop the (metatype)
  // "self" param. The "native" SIL representation will look like this:
  //    @convention(method) (@thin Foo.Type) -> () but the "actual" SIL function
  // looks like this:
  //    @convention(c) () -> ()
  // . We do this by simply omitting the last params.
  // TODO: fix this for static C++ methods.
  if (witness.getDecl()->getClangDecl() &&
      isa<clang::CXXConstructorDecl>(witness.getDecl()->getClangDecl())) {
    origParams.pop_back();
    reqtSubstParams = reqtSubstParams.drop_back();
    ignoreFinalInputOrigParam = true;
  }

  // For a free function witness, discard the 'self' parameter of the
  // requirement.  We'll also have to tell the traversal to ignore the
  // final orig parameter.
  if (isFree) {
    origParams.pop_back();
    reqtSubstParams = reqtSubstParams.drop_back();
    ignoreFinalInputOrigParam = true;
  }

  // Translate the argument values from the requirement abstraction level to
  // the substituted signature of the witness.
  SmallVector<ManagedValue, 8> witnessParams;
  AbstractionPattern witnessOrigTy(witnessInfo.LoweredType);
  TranslateArguments(*this, loc,
                     origParams, witnessParams,
                     witnessUnsubstTy, witnessUnsubstTy->getParameters())
    .process(witnessOrigTy,
             witnessSubstParams,
             reqtOrigTy,
             reqtSubstParams,
             ignoreFinalInputOrigParam);

  SILValue witnessFnRef = getWitnessFunctionRef(*this, witness,
                                                origWitnessFTy,
                                                witnessKind, witnessSubs,
                                                witnessParams, loc);

  auto coroutineKind =
    witnessFnRef->getType().castTo<SILFunctionType>()->getCoroutineKind();
  assert(coroutineKind == F.getLoweredFunctionType()->getCoroutineKind() &&
         "coroutine-ness mismatch between requirement and witness");

  // Collect the arguments.
  SmallVector<SILValue, 8> args;

  std::optional<ResultPlanner> resultPlanner;
  if (coroutineKind == SILCoroutineKind::None) {
    //   - indirect results
    resultPlanner.emplace(*this, loc, thunkIndirectResults, args);
    resultPlanner->plan(witnessOrigTy.getFunctionResultType(),
                        witnessSubstTy.getResult(),
                        reqtOrigTy.getFunctionResultType(),
                        reqtSubstTy.getResult(),
                        witnessFTy, thunkTy);

    // If the function we're calling has an indirect error result, create an
    // argument for it.
    if (auto innerIndirectErrorAddr =
            emitThunkIndirectErrorArgument(*this, loc, witnessFTy)) {
      args.push_back(*innerIndirectErrorAddr);
    }
  }

  //   - the rest of the arguments
  forwardFunctionArguments(*this, loc, witnessFTy, witnessParams, args);

  // Perform the call.
  SILType witnessSILTy = SILType::getPrimitiveObjectType(witnessFTy);

  SILValue reqtResultValue;
  switch (coroutineKind) {
  case SILCoroutineKind::None: {
    SILValue witnessResultValue =
      emitApplyWithRethrow(loc, witnessFnRef, witnessSILTy, witnessSubs, args);

    // Reabstract the result value.
    reqtResultValue = resultPlanner->execute(witnessResultValue, witnessFTy);
    break;
  }

  case SILCoroutineKind::YieldOnce: {
    SmallVector<SILValue, 4> witnessYields;
    auto tokenAndCleanup =
      emitBeginApplyWithRethrow(loc, witnessFnRef, witnessSILTy, witnessSubs,
                                args, witnessYields);

    YieldInfo witnessYieldInfo(SGM, witness, witnessFTy, witnessSubs);
    YieldInfo reqtYieldInfo(SGM, requirement, thunkTy,
                            reqtSubs.subst(getForwardingSubstitutionMap()));

    translateYields(*this, loc, witnessYields, witnessYieldInfo, reqtYieldInfo);

    // Kill the abort cleanup without emitting it.
    Cleanups.setCleanupState(tokenAndCleanup.second, CleanupState::Dead);

    // End the inner coroutine normally.
    emitEndApplyWithRethrow(loc, tokenAndCleanup.first);

    reqtResultValue = B.createTuple(loc, {});
    break;
  }

  case SILCoroutineKind::YieldMany:
    SGM.diagnose(loc, diag::unimplemented_generator_witnesses);
    reqtResultValue = B.createTuple(loc, {});
    break;
  }

  formalEvalScope.pop();
  scope.pop();
  B.createReturn(loc, reqtResultValue);

  // Now that we have finished emitting the function, verify it!
  F.verify();
}

ManagedValue SILGenFunction::emitActorIsolationErasureThunk(
    SILLocation loc, ManagedValue func,
    CanAnyFunctionType isolatedType, CanAnyFunctionType nonIsolatedType) {
  auto globalActor = isolatedType->getGlobalActor();

  assert(globalActor);
  assert(!nonIsolatedType->getGlobalActor());

  CanSILFunctionType loweredIsolatedType =
      func.getType().castTo<SILFunctionType>();
  CanSILFunctionType loweredNonIsolatedType =
      getLoweredType(nonIsolatedType).castTo<SILFunctionType>();

  LLVM_DEBUG(
      llvm::dbgs() << "=== Generating actor isolation erasure thunk for:";
      loweredIsolatedType.dump(llvm::dbgs()); llvm::dbgs() << "\n");

  if (loweredIsolatedType->getPatternSubstitutions()) {
    loweredIsolatedType = loweredIsolatedType->getUnsubstitutedType(SGM.M);
    func = B.createConvertFunction(
        loc, func, SILType::getPrimitiveObjectType(loweredIsolatedType));
  }

  auto expectedType = loweredNonIsolatedType->getUnsubstitutedType(SGM.M);

  // This thunk is for complete dynamic erasure, i.e. to `nonisolated`,
  // not to @isolated(any).
  assert(!expectedType->hasErasedIsolation());

  SubstitutionMap interfaceSubs;
  GenericEnvironment *genericEnv = nullptr;
  CanType dynamicSelfType;

  auto thunkType = buildThunkType(loweredIsolatedType,
                                  expectedType,
                                  isolatedType,
                                  nonIsolatedType,
                                  genericEnv,
                                  interfaceSubs,
                                  dynamicSelfType);

  auto *thunk = SGM.getOrCreateReabstractionThunk(
      thunkType, loweredIsolatedType, expectedType, dynamicSelfType,
      globalActor->getCanonicalType());

  if (thunk->empty()) {
    thunk->setGenericEnvironment(genericEnv);
    SILGenFunction thunkSGF(SGM, *thunk, FunctionDC);

    buildThunkBody(
        thunkSGF, loc, AbstractionPattern(isolatedType), isolatedType,
        AbstractionPattern(nonIsolatedType), nonIsolatedType, expectedType,
        dynamicSelfType, [&loc, &globalActor](SILGenFunction &thunkSGF) {
          thunkSGF.emitPreconditionCheckExpectedExecutor(
              loc, ActorIsolation::forGlobalActor(globalActor), std::nullopt);
        });

    SGM.emitLazyConformancesForFunction(thunk);
  }

  // Create it in the current function.
  ManagedValue thunkedFn = createPartialApplyOfThunk(
      *this, loc, thunk, interfaceSubs, dynamicSelfType, loweredNonIsolatedType,
      func.ensurePlusOne(*this, loc), ManagedValue());

  if (expectedType != loweredNonIsolatedType) {
    auto escapingExpectedType = loweredNonIsolatedType->getWithExtInfo(
        loweredNonIsolatedType->getExtInfo().withNoEscape(false));
    thunkedFn = B.createConvertFunction(
        loc, thunkedFn, SILType::getPrimitiveObjectType(escapingExpectedType));
  }

  if (loweredIsolatedType->isNoEscape()) {
    thunkedFn = B.createConvertEscapeToNoEscape(
        loc, thunkedFn,
        SILType::getPrimitiveObjectType(loweredNonIsolatedType));
  }

  return thunkedFn;
}
