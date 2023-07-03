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
#include "swift/Basic/Generators.h"
#include "swift/AST/ASTMangler.h"
#include "swift/AST/Decl.h"
#include "swift/AST/DiagnosticsSIL.h"
#include "swift/AST/ExistentialLayout.h"
#include "swift/AST/GenericEnvironment.h"
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


static ArrayRef<ProtocolConformanceRef>
collectExistentialConformances(ModuleDecl *M, CanType fromType, CanType toType) {
  assert(!fromType.isAnyExistentialType());

  auto layout = toType.getExistentialLayout();
  auto protocols = layout.getProtocols();
  
  SmallVector<ProtocolConformanceRef, 4> conformances;
  for (auto proto : protocols) {
    auto conformance =
      M->lookupConformance(fromType, proto, /*allowMissing=*/true);
    assert(conformance);
    conformances.push_back(conformance);
  }
  
  return M->getASTContext().AllocateCopy(conformances);
}

static ManagedValue emitTransformExistential(SILGenFunction &SGF,
                                             SILLocation loc,
                                             ManagedValue input,
                                             CanType inputType,
                                             CanType outputType,
                                             SGFContext ctxt) {
  assert(inputType != outputType);

  FormalEvaluationScope scope(SGF);

  if (inputType->isAnyExistentialType()) {
    CanType openedType = OpenedArchetypeType::getAny(inputType,
                                                     SGF.F.getGenericSignature());
    SILType loweredOpenedType = SGF.getLoweredType(openedType);

    input = SGF.emitOpenExistential(loc, input,
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

  ArrayRef<ProtocolConformanceRef> conformances =
      collectExistentialConformances(SGF.SGM.M.getSwiftModule(),
                                     fromInstanceType,
                                     toInstanceType);

  // Build result existential
  AbstractionPattern opaque = AbstractionPattern::getOpaque();
  const TypeLowering &concreteTL = SGF.getTypeLowering(opaque, inputType);
  const TypeLowering &expectedTL = SGF.getTypeLowering(outputType);
  return SGF.emitExistentialErasure(
                   loc, inputType, concreteTL, expectedTL,
                   conformances, ctxt,
                   [&](SGFContext C) -> ManagedValue {
                     return SGF.manageOpaqueValue(input, loc, C);
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
    if (!inputTL.isAddressOnly()) {
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
    return emitTransformExistential(SGF, Loc, v,
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

  return ManagedValue::forUnmanaged(result);
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
    if (!isPlusOne)
      out.push_back(ManagedValue::forUnmanaged(element));
    else if (element->getType().isAddress())
      out.push_back(SGF.emitManagedBufferWithCleanup(element));
    else
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
    llvm::Optional<TemporaryInitialization> outputEltTemp;
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
    SmallVectorImpl<SILArgument *> *indirectResults) {
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
      indirectResults->push_back(arg);
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
/// SIL values we receive in Inputs and must generate in Outputs.
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
/// parameter and therefore there is a corresponding value in Inputs.
/// When the walk reaches a non-tuple orig type on the output side,
/// we know that the output function receives a lowered parameter
/// and therefore there is a corresponding type in OutputTypes (and
/// a need to produce an argument value in Outputs).
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
class TranslateArguments {
  SILGenFunction &SGF;
  SILLocation Loc;
  ArrayRefGenerator<ArrayRef<ManagedValue>> Inputs;
  SmallVectorImpl<ManagedValue> &Outputs;
  CanSILFunctionType OutputTypesFuncTy;
  ArrayRef<SILParameterInfo> OutputTypes;
public:
  TranslateArguments(SILGenFunction &SGF, SILLocation loc,
                     ArrayRef<ManagedValue> inputs,
                     SmallVectorImpl<ManagedValue> &outputs,
                     CanSILFunctionType outputTypesFuncTy,
                     ArrayRef<SILParameterInfo> outputTypes)
    : SGF(SGF), Loc(loc), Inputs(inputs), Outputs(outputs),
      OutputTypesFuncTy(outputTypesFuncTy), OutputTypes(outputTypes) {}

  void translate(AbstractionPattern inputOrigFunctionType,
                 AnyFunctionType::CanParamArrayRef inputSubstTypes,
                 AbstractionPattern outputOrigFunctionType,
                 AnyFunctionType::CanParamArrayRef outputSubstTypes,
                 bool ignoreFinalInputOrigParam = false) {
    if (inputSubstTypes.size() == 1 &&
        outputSubstTypes.size() != 1) {
      // SE-0110 tuple splat. Turn the output into a single value of tuple
      // type, and translate.
      auto inputOrigType = inputOrigFunctionType.getFunctionParamType(0);
      auto inputSubstType = inputSubstTypes[0].getPlainType();

      // Build an abstraction pattern for the output.
      SmallVector<AbstractionPattern, 8> outputOrigTypes;
      for (unsigned i = 0, e = outputOrigFunctionType.getNumFunctionParams();
           i < e; ++i) {
        outputOrigTypes.push_back(
          outputOrigFunctionType.getFunctionParamType(i));
      }
      auto outputOrigType = AbstractionPattern::getTuple(outputOrigTypes);

      // Build the substituted output tuple type. Note that we deliberately
      // don't use composeTuple() because we want to drop ownership
      // qualifiers.
      SmallVector<TupleTypeElt, 8> elts;
      for (auto param : outputSubstTypes) {
        assert(!param.isVariadic());
        assert(!param.isInOut());
        elts.emplace_back(param.getParameterType());
      }
      auto outputSubstType = CanTupleType(
        TupleType::get(elts, SGF.getASTContext()));

      // Translate the input tuple value into the output tuple value. Note
      // that the output abstraction pattern is a tuple, and we explode tuples
      // into multiple parameters, so if the input abstraction pattern is
      // opaque, this will explode the input value. Otherwise, the input
      // parameters will be mapped one-to-one to the output parameters.
      translate(inputOrigType, inputSubstType,
                outputOrigType, outputSubstType);
      Inputs.finish();
      return;
    }

    // Otherwise, formal parameters are always reabstracted one-to-one,
    // at least in substituted types.  (They can differ in the orig types
    // because of pack expansions in the parameters.)  As a result, if we
    // generate the substituted parameters for both the input and
    // output function types, and we pull a substituted formal parameter
    // off of one whenever we pull a substituted formal parameter off the
    // other, we should end up exhausting both sequences.
    assert(inputSubstTypes.size() == outputSubstTypes.size());

    // It's more straightforward for generating packs if we allow
    // ourselves to be driven by the output sequence.  This requires us
    // to invert control for the input sequence, pulling off one
    // component at a time while looping over the output sequence.

    FunctionInputGenerator inputParams(SGF.getASTContext(), Inputs,
                                       inputOrigFunctionType,
                                       inputSubstTypes,
                                       ignoreFinalInputOrigParam);

    FunctionParamGenerator outputParams(outputOrigFunctionType,
                                        outputSubstTypes,
                                        /*drop self*/ false);

    // Loop over the *orig* formal parameters.  We'll pull off
    // *substituted* formal parameters in parallel for both inputs
    // and outputs.
    for (; !outputParams.isFinished(); outputParams.advance()) {
      // If we have a pack expansion formal parameter in the orig
      // output type, it corresponds to N formal parameters in the
      // substituted output type.  translateToPackParam will pull off
      // N substituted formal parameters from the input type.
      if (outputParams.isOrigPackExpansion()) {
        auto outputPackParam = claimNextOutputType();
        auto output =
          translateToPackParam(inputParams,
                               outputParams.getOrigType(),
                               outputParams.getSubstParams(),
                               outputPackParam);
        Outputs.push_back(output);

      // Otherwise, we have a single, non-pack formal parameter
      // in the output type.  translateToSingleParam will pull
      // off a single formal parameter from the input type.
      } else {
        translateToSingleParam(inputParams,
                               outputParams.getOrigType(),
                               outputParams.getSubstParams()[0]);
      }
    }
    outputParams.finish();
    inputParams.finish();

    Inputs.finish();
  }

  /// This is used for translating yields.
  void translate(ArrayRef<AbstractionPattern> inputOrigTypes,
                 AnyFunctionType::CanParamArrayRef inputSubstTypes,
                 ArrayRef<AbstractionPattern> outputOrigTypes,
                 AnyFunctionType::CanParamArrayRef outputSubstTypes) {
    assert(inputOrigTypes.size() == inputSubstTypes.size());
    assert(outputOrigTypes.size() == outputSubstTypes.size());
    assert(inputOrigTypes.size() == outputOrigTypes.size());

    for (auto i : indices(inputOrigTypes)) {
      translate(inputOrigTypes[i], inputSubstTypes[i],
                outputOrigTypes[i], outputSubstTypes[i]);
    }

    Inputs.finish();
  }

private:
  void translate(AbstractionPattern inputOrigType,
                 AnyFunctionType::CanParam inputSubstType,
                 AbstractionPattern outputOrigType,
                 AnyFunctionType::CanParam outputSubstType) {
    // Note that it's OK for the input to be inout but not the output;
    // this means we're just going to load the inout and pass it on as a
    // scalar.
    if (outputSubstType.isInOut()) {
      assert(inputSubstType.isInOut());
      auto inputValue = claimNextInput();
      auto outputLoweredTy = claimNextOutputType();

      ManagedValue output =
        translateInOut(inputOrigType, inputSubstType.getParameterType(),
                       outputOrigType, outputSubstType.getParameterType(),
                       inputValue, outputLoweredTy);
      Outputs.push_back(output);
    } else {
      translate(inputOrigType, inputSubstType.getParameterType(),
                outputOrigType, outputSubstType.getParameterType());
    }
  }

  void translate(AbstractionPattern inputOrigType,
                 CanType inputSubstType,
                 AbstractionPattern outputOrigType,
                 CanType outputSubstType) {
    // Handle the input pattern being a tuple, which will have been
    // expanded in the input parameters.
    if (inputOrigType.isTuple()) {
      auto inputTupleType = cast<TupleType>(inputSubstType);

      // If the output pattern is also a tuple, it needs to be expanded
      // in the output arguments.  Do a parallel translation of the
      // expanded elements.
      if (outputOrigType.isTuple()) {
        auto outputTupleType = cast<TupleType>(outputSubstType);

        // Both input and output are exploded tuples, easy case.
        return translateParallelExploded(inputOrigType,
                                         inputTupleType,
                                         outputOrigType,
                                         outputTupleType);
      }

      auto outputParam = claimNextOutputType();
      auto output = translateTupleIntoSingle(inputOrigType,
                                             inputTupleType,
                                             outputOrigType,
                                             outputSubstType,
                                             outputParam);
      Outputs.push_back(output);
      return;
    }

    // Handle the output pattern being a tuple, which needs to be
    // expanded in the output parameters.
    if (outputOrigType.isTuple()) {
      auto outputTupleType = cast<TupleType>(outputSubstType);

      // The input type must also be a tuple, because there aren't
      // any conversions that *add* tuple-ness.  It's just not
      // expanded in the input parameters.
      // FIXME: IUO<Tuple> to Tuple, maybe?
      assert(inputOrigType.isTypeParameter() &&
             "Input is not a tuple and is not opaque?");
      auto inputTupleType = cast<TupleType>(inputSubstType);

      translateAndExplodeOutOf(inputOrigType, inputTupleType,
                               outputOrigType, outputTupleType,
                               claimNextInput());
      return;
    }

    // Okay, we are now working with a single value turning into a
    // single value.
    auto inputElt = claimNextInput();
    auto outputEltType = claimNextOutputType();
    auto output = translateSingle(inputOrigType, inputSubstType,
                                  outputOrigType, outputSubstType,
                                  inputElt, outputEltType);
    Outputs.push_back(output);
  }

  void translateToSingleParam(FunctionInputGenerator &inputParam,
                              AbstractionPattern outputOrigType,
                              AnyFunctionType::CanParam outputSubstParam);

  ManagedValue translateToPackParam(FunctionInputGenerator &inputParams,
                       AbstractionPattern outputOrigExpansionType,
                       AnyFunctionType::CanParamArrayRef outputSubstParams,
                       SILParameterInfo outputParam);

  ManagedValue translateIntoSingle(AbstractionPattern inputOrigType,
                                   CanType inputSubstType,
                                   AbstractionPattern outputOrigType,
                                   CanType outputSubstType,
                                   SILParameterInfo outputParam) {
    if (inputOrigType.isTuple()) {
      return translateTupleIntoSingle(inputOrigType,
                                      cast<TupleType>(inputSubstType),
                                      outputOrigType,
                                      outputSubstType,
                                      outputParam);
    }

    auto input = claimNextInput();
    return translateSingle(inputOrigType, inputSubstType,
                           outputOrigType, outputSubstType,
                           input, outputParam);
  }

  ManagedValue
  translateTupleIntoSingle(AbstractionPattern inputOrigType,
                           CanTupleType inputSubstType,
                           AbstractionPattern outputOrigType,
                           CanType outputSubstType,
                           SILParameterInfo outputParam) {
    // Tuple types are subtypes of their optionals
    if (auto outputObjectType = outputSubstType.getOptionalObjectType()) {
      auto outputOrigObjectType = outputOrigType.getOptionalObjectType();

      if (auto outputTupleType = dyn_cast<TupleType>(outputObjectType)) {
        // The input is exploded and the output is an optional tuple.
        // Translate values and collect them into a single optional
        // payload.

        auto result =
            translateAndImplodeIntoOptional(inputOrigType,
                                            inputSubstType,
                                            outputOrigObjectType,
                                            outputTupleType,
                                            outputParam);
        return result;
      }

      // Tuple types are subtypes of optionals of Any, too.
      assert(outputObjectType->isAny());

      // First, construct the existential.
      auto result =
          translateAndImplodeIntoAny(inputOrigType,
                                     inputSubstType,
                                     outputOrigObjectType,
                                     outputObjectType);

      // Now, convert it to an optional.
      return translateSingle(outputOrigObjectType, outputObjectType,
                             outputOrigType, outputSubstType,
                             result, outputParam);
    }

    if (outputSubstType->isAny()) {
      // We don't need outputParam on this path.

      return translateAndImplodeIntoAny(inputOrigType,
                                        inputSubstType,
                                        outputOrigType,
                                        outputSubstType);
    }

    if (auto outputTupleType = dyn_cast<TupleType>(outputSubstType)) {
      // The input is exploded and the output is not. Translate values
      // and store them to a result tuple in memory.
      assert(outputOrigType.isTypeParameter() &&
             "Output is not a tuple and is not opaque?");

      auto outputTy = SGF.getSILType(outputParam, OutputTypesFuncTy);
      auto &outputTL = SGF.getTypeLowering(outputTy);
      if (SGF.silConv.useLoweredAddresses()) {
        auto temp = SGF.emitTemporary(Loc, outputTL);
        translateAndImplodeInto(inputOrigType, inputSubstType,
                                outputOrigType, outputTupleType, *temp);

        return temp->getManagedAddress();
      } else {
        auto result = translateAndImplodeIntoValue(
            inputOrigType, inputSubstType, outputOrigType, outputTupleType,
            outputTL.getLoweredType());
        return result;
      }
    }

    llvm_unreachable("Unhandled conversion from exploded tuple");
  }

  void translateFromSingle(AbstractionPattern inputOrigType,
                           AnyFunctionType::CanParam inputSubstParam,
                           AbstractionPattern outputOrigType,
                           AnyFunctionType::CanParam outputSubstParam,
                           ManagedValue inputValue) {
    CanType inputSubstType = inputSubstParam.getParameterType();
    CanType outputSubstType = outputSubstParam.getParameterType();

    if (outputSubstParam.isInOut()) {
      assert(inputSubstParam.isInOut());
      auto outputLoweredTy = claimNextOutputType();

      ManagedValue output = translateInOut(inputOrigType, inputSubstType,
                                           outputOrigType, outputSubstType,
                                           inputValue, outputLoweredTy);
      Outputs.push_back(output);
    } else if (outputOrigType.isTuple()) {
      translateAndExplodeOutOf(inputOrigType,
                               cast<TupleType>(inputSubstType),
                               outputOrigType,
                               cast<TupleType>(outputSubstType),
                               inputValue);
    } else {
      auto outputParam = claimNextOutputType();
      ManagedValue output = translateSingle(inputOrigType, inputSubstType,
                                            outputOrigType, outputSubstType,
                                            inputValue, outputParam);
      Outputs.push_back(output);
    }
  }

  /// Take a tuple that has been exploded in the input and turn it into
  /// a tuple value in the output.
  ManagedValue translateAndImplodeIntoValue(AbstractionPattern inputOrigType,
                                            CanTupleType inputType,
                                            AbstractionPattern outputOrigType,
                                            CanTupleType outputType,
                                            SILType loweredOutputTy) {
    assert(loweredOutputTy.is<TupleType>());

    SmallVector<ManagedValue, 4> elements;
    assert(outputType->getNumElements() == inputType->getNumElements());
    assert(loweredOutputTy.castTo<TupleType>()->getNumElements() ==
           outputType->getNumElements());
    for (unsigned i : indices(outputType->getElementTypes())) {
      auto inputOrigEltType = inputOrigType.getTupleElementType(i);
      auto inputEltType = inputType.getElementType(i);
      auto outputOrigEltType = outputOrigType.getTupleElementType(i);
      auto outputEltType = outputType.getElementType(i);
      SILType loweredOutputEltTy = loweredOutputTy.getTupleElementType(i);

      ManagedValue elt;
      if (auto inputEltTupleType = dyn_cast<TupleType>(inputEltType)) {
        elt = translateAndImplodeIntoValue(inputOrigEltType,
                                           inputEltTupleType,
                                           outputOrigEltType,
                                           cast<TupleType>(outputEltType),
                                           loweredOutputEltTy);
      } else {
        elt = claimNextInput();

        // Load if necessary.
        if (elt.getType().isAddress()) {
          // This code assumes that we have each element at +1. So, if we do
          // not have a cleanup, we emit a load [copy]. This can occur if we
          // are translating in_guaranteed parameters.
          IsTake_t isTakeVal = elt.isPlusZero() ? IsNotTake : IsTake;
          elt = SGF.emitLoad(Loc, elt.forward(SGF),
                             SGF.getTypeLowering(elt.getType()), SGFContext(),
                             isTakeVal);
        }
      }

      if (elt.getType() != loweredOutputEltTy)
        elt = translatePrimitive(inputOrigEltType, inputEltType,
                                 outputOrigEltType, outputEltType,
                                 elt, loweredOutputEltTy);

      // Aggregation of address-only values requires ownership.
      if (loweredOutputTy.isAddressOnly(SGF.F)) {
        elt = elt.ensurePlusOne(SGF, Loc);
      }
      elements.push_back(elt);
    }

    SmallVector<SILValue, 4> forwarded;
    for (auto &elt : elements)
      forwarded.push_back(elt.forward(SGF));

    auto tuple = SGF.B.createTuple(Loc, loweredOutputTy, forwarded);
    if (tuple->getOwnershipKind() == OwnershipKind::Owned)
      return SGF.emitManagedRValueWithCleanup(tuple);

    return ManagedValue::forUnmanaged(tuple);
  }


  /// Handle a tuple that has been exploded in the input but wrapped in
  /// an optional in the output.
  ManagedValue
  translateAndImplodeIntoOptional(AbstractionPattern inputOrigType,
                                  CanTupleType inputTupleType,
                                  AbstractionPattern outputOrigType,
                                  CanTupleType outputTupleType,
                                  SILParameterInfo outputParam) {
    assert(inputTupleType->getNumElements() ==
           outputTupleType->getNumElements());

    // Collect the tuple elements.
    auto &loweredTL = SGF.getTypeLowering(outputOrigType, outputTupleType);
    auto loweredTy = loweredTL.getLoweredType();
    auto optionalTy = SGF.getSILType(outputParam, OutputTypesFuncTy);
    auto someDecl = SGF.getASTContext().getOptionalSomeDecl();
    if (loweredTL.isLoadable() || !SGF.silConv.useLoweredAddresses()) {
      auto payload =
        translateAndImplodeIntoValue(inputOrigType, inputTupleType,
                                     outputOrigType, outputTupleType,
                                     loweredTy);

      return SGF.B.createEnum(Loc, payload, someDecl, optionalTy);
    } else {
      auto optionalBuf = SGF.emitTemporaryAllocation(Loc, optionalTy);
      auto tupleBuf = SGF.B.createInitEnumDataAddr(Loc, optionalBuf, someDecl,
                                                   loweredTy);
      
      auto tupleTemp = SGF.useBufferAsTemporary(tupleBuf, loweredTL);

      translateAndImplodeInto(inputOrigType, inputTupleType,
                              outputOrigType, outputTupleType,
                              *tupleTemp);

      SGF.B.createInjectEnumAddr(Loc, optionalBuf, someDecl);

      auto payload = tupleTemp->getManagedAddress();
      if (payload.hasCleanup()) {
        payload.forward(SGF);
        return SGF.emitManagedBufferWithCleanup(optionalBuf);
      }
      return ManagedValue::forUnmanaged(optionalBuf);
    }
  }

  /// Handle a tuple that has been exploded in the input but wrapped
  /// in an existential in the output.
  ManagedValue
  translateAndImplodeIntoAny(AbstractionPattern inputOrigType,
                             CanTupleType inputTupleType,
                             AbstractionPattern outputOrigType,
                             CanType outputSubstType) {
    auto existentialTy = SGF.getLoweredType(outputOrigType, outputSubstType);
    auto existentialBuf = SGF.emitTemporaryAllocation(Loc, existentialTy);

    auto opaque = AbstractionPattern::getOpaque();
    auto &concreteTL = SGF.getTypeLowering(opaque, inputTupleType);

    auto tupleBuf =
      SGF.B.createInitExistentialAddr(Loc, existentialBuf,
                                      inputTupleType,
                                      concreteTL.getLoweredType(),
                                      /*conformances=*/{});

    auto tupleTemp = SGF.useBufferAsTemporary(tupleBuf, concreteTL);
    translateAndImplodeInto(inputOrigType, inputTupleType,
                            opaque, inputTupleType,
                            *tupleTemp);

    auto payload = tupleTemp->getManagedAddress();
    if (SGF.silConv.useLoweredAddresses()) {
      // We always need to return the existential buf with a cleanup even if
      // we stored trivial values, since SILGen maintains the invariant that
      // forwarding a non-trivial value (i.e. an Any) into memory must be done
      // at +1.
      payload.forward(SGF);
      return SGF.emitManagedBufferWithCleanup(existentialBuf);
    }

    // We are under opaque value(s) mode - load the any and init an opaque
    auto loadedPayload = SGF.B.createLoadCopy(Loc, payload);
    auto &anyTL = SGF.getTypeLowering(opaque, outputSubstType);
    return SGF.B.createInitExistentialValue(
        Loc, anyTL.getLoweredType(), inputTupleType, loadedPayload,
        /*Conformances=*/{});
  }

  /// Handle a tuple that has been exploded in both the input and
  /// the output.
  void translateParallelExploded(AbstractionPattern inputOrigType,
                                 CanTupleType inputSubstType,
                                 AbstractionPattern outputOrigType,
                                 CanTupleType outputSubstType) {
    assert(inputOrigType.matchesTuple(inputSubstType));
    assert(outputOrigType.matchesTuple(outputSubstType));
    assert(inputSubstType->getNumElements() ==
           outputSubstType->getNumElements());

    for (auto index : indices(outputSubstType.getElementTypes())) {
      translate(inputOrigType.getTupleElementType(index),
                inputSubstType.getElementType(index),
                outputOrigType.getTupleElementType(index),
                outputSubstType.getElementType(index));
    }
  }

  /// Given that a tuple value is being passed indirectly in the
  /// input, explode it and translate the elements.
  void translateAndExplodeOutOf(AbstractionPattern inputOrigType,
                                CanTupleType inputSubstType,
                                AbstractionPattern outputOrigType,
                                CanTupleType outputSubstType,
                                ManagedValue inputTupleAddr) {
    assert(inputOrigType.isTypeParameter());
    assert(outputOrigType.matchesTuple(outputSubstType));
    assert(inputSubstType->getNumElements() ==
           outputSubstType->getNumElements());

    SmallVector<ManagedValue, 4> inputEltAddrs;
    explodeTuple(SGF, Loc, inputTupleAddr, inputEltAddrs);
    assert(inputEltAddrs.size() == outputSubstType->getNumElements());

    for (auto index : indices(outputSubstType.getElementTypes())) {
      auto inputEltOrigType = inputOrigType.getTupleElementType(index);
      auto inputEltSubstType = inputSubstType.getElementType(index);
      auto outputEltOrigType = outputOrigType.getTupleElementType(index);
      auto outputEltSubstType = outputSubstType.getElementType(index);
      auto inputEltAddr = inputEltAddrs[index];
      assert(inputEltAddr.getType().isAddress() ||
             !SGF.silConv.useLoweredAddresses());

      if (auto outputEltTupleType = dyn_cast<TupleType>(outputEltSubstType)) {
        assert(outputEltOrigType.isTuple());
        auto inputEltTupleType = cast<TupleType>(inputEltSubstType);
        translateAndExplodeOutOf(inputEltOrigType,
                                 inputEltTupleType,
                                 outputEltOrigType,
                                 outputEltTupleType,
                                 inputEltAddr);
      } else {
        auto outputType = claimNextOutputType();
        auto output = translateSingle(inputEltOrigType,
                                      inputEltSubstType,
                                      outputEltOrigType,
                                      outputEltSubstType,
                                      inputEltAddr,
                                      outputType);
        Outputs.push_back(output);
      }
    }
  }

  /// Given that a tuple value is being passed indirectly in the
  /// output, translate the elements and implode it.
  void translateAndImplodeInto(AbstractionPattern inputOrigType,
                               CanTupleType inputSubstType,
                               AbstractionPattern outputOrigType,
                               CanTupleType outputSubstType,
                               TemporaryInitialization &tupleInit) {
    assert(inputOrigType.matchesTuple(inputSubstType));
    assert(outputOrigType.matchesTuple(outputSubstType));
    assert(inputSubstType->getNumElements() ==
           outputSubstType->getNumElements());

    auto outputLoweredTy = tupleInit.getAddress()->getType();
    assert(outputLoweredTy.castTo<TupleType>()->getNumElements() ==
           outputSubstType->getNumElements());

    SmallVector<CleanupHandle, 4> cleanups;

    for (auto index : indices(outputSubstType.getElementTypes())) {
      auto inputEltOrigType = inputOrigType.getTupleElementType(index);
      auto inputEltSubstType = inputSubstType.getElementType(index);
      auto outputEltOrigType = outputOrigType.getTupleElementType(index);
      auto outputEltSubstType = outputSubstType.getElementType(index);
      auto eltAddr =
        SGF.B.createTupleElementAddr(Loc, tupleInit.getAddress(), index);

      auto &outputEltTL = SGF.getTypeLowering(eltAddr->getType());
      CleanupHandle eltCleanup =
        SGF.enterDormantTemporaryCleanup(eltAddr, outputEltTL);
      if (eltCleanup.isValid()) cleanups.push_back(eltCleanup);

      TemporaryInitialization eltInit(eltAddr, eltCleanup);
      if (auto outputEltTupleType = dyn_cast<TupleType>(outputEltSubstType)) {
        auto inputEltTupleType = cast<TupleType>(inputEltSubstType);
        translateAndImplodeInto(inputEltOrigType, inputEltTupleType,
                                outputEltOrigType, outputEltTupleType,
                                eltInit);
      } else {
        // Otherwise, we come from a single value.
        auto input = claimNextInput();
        translateSingleInto(inputEltOrigType, inputEltSubstType,
                            outputEltOrigType, outputEltSubstType,
                            input, eltAddr->getType(), eltInit);
      }
    }

    // Deactivate all the element cleanups and activate the tuple cleanup.
    for (auto cleanup : cleanups)
      SGF.Cleanups.forwardCleanup(cleanup);
    tupleInit.finishInitialization(SGF);
  }

  // Translate into a temporary.
  ManagedValue translateIndirect(AbstractionPattern inputOrigType,
                                 CanType inputSubstType,
                                 AbstractionPattern outputOrigType,
                                 CanType outputSubstType, ManagedValue input,
                                 SILType resultTy) {
    auto &outputTL = SGF.getTypeLowering(resultTy);
    auto temp = SGF.emitTemporary(Loc, outputTL);
    translateSingleInto(inputOrigType, inputSubstType, outputOrigType,
                        outputSubstType, input, resultTy.getAddressType(),
                        *temp);
    return temp->getManagedAddress();
  }

  // Translate into an owned argument.
  ManagedValue translateIntoOwned(AbstractionPattern inputOrigType,
                                  CanType inputSubstType,
                                  AbstractionPattern outputOrigType,
                                  CanType outputSubstType,
                                  ManagedValue input,
                                  SILType outputLoweredTy) {
    auto output = translatePrimitive(inputOrigType, inputSubstType,
                                     outputOrigType, outputSubstType,
                                     input, outputLoweredTy);

    // If our output is guaranteed or unowned, we need to create a copy here.
    if (output.getOwnershipKind() != OwnershipKind::Owned)
      output = output.copyUnmanaged(SGF, Loc);

    return output;
  }

  // Translate into a guaranteed argument.
  ManagedValue translateIntoGuaranteed(AbstractionPattern inputOrigType,
                                       CanType inputSubstType,
                                       AbstractionPattern outputOrigType,
                                       CanType outputSubstType,
                                       ManagedValue input,
                                       SILType outputLoweredTy) {
    auto output = translatePrimitive(inputOrigType, inputSubstType,
                                     outputOrigType, outputSubstType,
                                     input, outputLoweredTy);

    // If our output value is not guaranteed, we need to:
    //
    // 1. Unowned - Copy + Borrow.
    // 2. Owned - Borrow.
    // 3. Trivial - do nothing.
    //
    // This means we can first transition unowned => owned and then handle
    // the new owned value using the same code path as values that are
    // initially owned.
    if (output.getOwnershipKind() == OwnershipKind::Unowned) {
      assert(!output.hasCleanup());
      output = SGF.emitManagedRetain(Loc, output.getValue());
    }

    // If the output is unowned or owned, create a borrow.
    if (output.getOwnershipKind() != OwnershipKind::Guaranteed) {
      output = SGF.emitManagedBeginBorrow(Loc, output.getValue());
    }

    return output;
  }

  /// Translate a single value and add it as an output.
  ManagedValue translateSingle(AbstractionPattern inputOrigType,
                               CanType inputSubstType,
                               AbstractionPattern outputOrigType,
                               CanType outputSubstType,
                               ManagedValue input,
                               SILParameterInfo result) {
    auto resultTy = SGF.getSILType(result, OutputTypesFuncTy);

    return translateSingle(inputOrigType, inputSubstType,
                           outputOrigType, outputSubstType,
                           input, resultTy, result.getConvention());
  }

  ManagedValue translateSingle(AbstractionPattern inputOrigType,
                               CanType inputSubstType,
                               AbstractionPattern outputOrigType,
                               CanType outputSubstType,
                               ManagedValue input,
                               SILType resultTy,
                               ParameterConvention resultConvention) {
    // Easy case: we want to pass exactly this value.
    if (input.getType() == resultTy) {
      if (isConsumedParameter(resultConvention) && !input.isPlusOne(SGF)) {
        input = input.copyUnmanaged(SGF, Loc);
      }

      return input;
    }

    switch (resultConvention) {
    // Direct translation is relatively easy.
    case ParameterConvention::Direct_Owned:
    case ParameterConvention::Direct_Unowned:
      return translateIntoOwned(inputOrigType, inputSubstType,
                                outputOrigType, outputSubstType,
                                input, resultTy);
    case ParameterConvention::Direct_Guaranteed:
      return translateIntoGuaranteed(inputOrigType, inputSubstType,
                                     outputOrigType, outputSubstType,
                                     input, resultTy);
    case ParameterConvention::Indirect_In: {
      if (SGF.silConv.useLoweredAddresses()) {
        return translateIndirect(inputOrigType, inputSubstType,
                                 outputOrigType, outputSubstType,
                                 input, resultTy);
      }
      return translateIntoOwned(inputOrigType, inputSubstType,
                                outputOrigType, outputSubstType,
                                input, resultTy);
    }
    case ParameterConvention::Indirect_In_Guaranteed: {
      if (SGF.silConv.useLoweredAddresses()) {
        return translateIndirect(inputOrigType, inputSubstType,
                                 outputOrigType, outputSubstType,
                                 input, resultTy);
      }
      return translateIntoGuaranteed(inputOrigType, inputSubstType,
                                     outputOrigType, outputSubstType,
                                     input, resultTy);
    }
    case ParameterConvention::Pack_Guaranteed:
    case ParameterConvention::Pack_Owned:
      SGF.SGM.diagnose(Loc, diag::not_implemented,
                       "reabstraction of pack values");
      return SGF.emitUndef(resultTy);
    case ParameterConvention::Indirect_Inout:
    case ParameterConvention::Pack_Inout:
      llvm_unreachable("inout reabstraction handled elsewhere");
    case ParameterConvention::Indirect_InoutAliasable:
      llvm_unreachable("abstraction difference in aliasable argument not "
                       "allowed");
    }

    llvm_unreachable("Covered switch isn't covered?!");
  }

  ManagedValue translateInOut(AbstractionPattern inputOrigType,
                              CanType inputSubstType,
                              AbstractionPattern outputOrigType,
                              CanType outputSubstType,
                              ManagedValue input,
                              SILParameterInfo result) {
    auto resultTy = SGF.getSILType(result, OutputTypesFuncTy);
    assert(input.isLValue());
    if (input.getType() == resultTy) {
      return input;
    }

    // Create a temporary of the right type.
    auto &temporaryTL = SGF.getTypeLowering(resultTy);
    auto temporary = SGF.emitTemporary(Loc, temporaryTL);

    // Take ownership of the input value.  This leaves the input l-value
    // effectively uninitialized, but we'll push a cleanup that will put
    // a value back into it.
    FullExpr scope(SGF.Cleanups, CleanupLocation(Loc));
    auto ownedInput =
      SGF.emitManagedBufferWithCleanup(input.getLValueAddress());

    // Translate the input value into the temporary.
    translateSingleInto(inputOrigType, inputSubstType,
                        outputOrigType, outputSubstType,
                        ownedInput, resultTy, *temporary);

    // Forward the cleanup on the temporary.  We're about to push a new
    // cleanup that will re-assert ownership of this value.
    auto temporaryAddr = temporary->getManagedAddress().forward(SGF);

    // Leave the scope in which we did the forward translation.  This
    // ensures that the value in the input buffer is destroyed
    // immediately rather than (potentially) arbitrarily later
    // at a point where we want to put new values in the input buffer.
    scope.pop();

    // Push the cleanup to perform the reverse translation.  This cleanup
    // asserts ownership of the value of the temporary.
    SGF.Cleanups.pushCleanup<TranslateIndirect>(outputOrigType,
                                                outputSubstType,
                                                inputOrigType,
                                                inputSubstType,
                                                temporaryAddr,
                                                input.getLValueAddress());

    // Use the temporary as the argument.
    return ManagedValue::forLValue(temporaryAddr);
  }

  /// Translate a single value and initialize the given temporary with it.
  void translateSingleInto(AbstractionPattern inputOrigType,
                           CanType inputSubstType,
                           AbstractionPattern outputOrigType,
                           CanType outputSubstType,
                           ManagedValue input,
                           SILType outputTy,
                           Initialization &init) {
    assert(outputTy.isAddress());
    auto output = translatePrimitive(inputOrigType, inputSubstType,
                                     outputOrigType, outputSubstType,
                                     input, outputTy,
                                     SGFContext(&init));
    if (!output.isInContext())
      output.ensurePlusOne(SGF, Loc).forwardInto(SGF, Loc, &init);
  }

  /// Apply primitive translation to the given value.
  ManagedValue translatePrimitive(AbstractionPattern inputOrigType,
                                  CanType inputSubstType,
                                  AbstractionPattern outputOrigType,
                                  CanType outputSubstType,
                                  ManagedValue input,
                                  SILType loweredOutputTy,
                                  SGFContext context = SGFContext()) {
    return SGF.emitTransformedValue(Loc, input,
                                    inputOrigType, inputSubstType,
                                    outputOrigType, outputSubstType,
                                    loweredOutputTy, context);
  }

  ManagedValue claimNextInput() {
    return Inputs.claimNext();
  }

  /// Claim the next lowered parameter in the output.  The conventions in
  /// this class are set up such that the place that claims an output type
  /// is also responsible for adding the output to Outputs.  This allows
  /// readers to easily verify that this is done on all paths.  (It'd
  /// sure be nice if we had better language mode for that, though.)
  SILParameterInfo claimNextOutputType() {
    return claimNext(OutputTypes);
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
                                                         inducedPackType,
                                                         eltIndex);
  } else {
    auto packIndex =
      SGF.B.createScalarPackIndex(loc, eltIndex, inducedPackType);
    auto eltAddr =
      SGF.B.createTuplePackElementAddr(loc, packIndex, tupleAddr, eltTy);
    eltValue = cloner.clone(eltAddr);
  }

  tupleValue = cloner.cloneForRemainingTupleComponents(tupleAddr,
                                                       inducedPackType,
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

  // Deactive the cleanup for the input pack value.
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

SILValue
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

  return temporary;
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

  // Deactivate the cleanup for the input pack value.
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

void TranslateArguments::translateToSingleParam(
                            FunctionInputGenerator &inputParam,
                            AbstractionPattern outputOrigType,
                            AnyFunctionType::CanParam outputSubstParam) {
  // If we're not processing a pack expansion, do a normal translation.
  if (!inputParam.isOrigPackExpansion()) {
    translate(inputParam.getOrigType(), inputParam.getSubstParam(),
              outputOrigType, outputSubstParam);

  // Pull out a scalar component from the pack and translate that.
  } else {
    auto inputValue = inputParam.projectPackComponent(SGF, Loc);
    translateFromSingle(inputParam.getOrigType(), inputParam.getSubstParam(),
                        outputOrigType, outputSubstParam,
                        inputValue);
  }

  inputParam.advance();
}

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

ManagedValue TranslateArguments::translateToPackParam(
                       FunctionInputGenerator &inputParam,
                       AbstractionPattern outputOrigExpansionType,
                       AnyFunctionType::CanParamArrayRef outputSubstParams,
                       SILParameterInfo outputPackParam) {
  assert(outputPackParam.isPack());
  auto outputTy = SGF.getSILType(outputPackParam, OutputTypesFuncTy);
  auto outputPackTy = outputTy.castTo<SILPackType>();
  assert(outputPackTy->getNumElements() == outputSubstParams.size());

  // TODO: try to just forward the entire pack, or a slice of it.

  // Allocate a pack of the expected type.
  auto outputPackAddr =
    SGF.emitTemporaryPackAllocation(Loc, outputTy.getObjectType());
  auto outputFormalPackType =
    CanPackType::get(SGF.getASTContext(), outputSubstParams);
  auto outputOrigPatternType =
    outputOrigExpansionType.getPackExpansionPatternType();

  SmallVector<CleanupHandle, 4> outputComponentCleanups;

  for (auto outputComponentIndex : indices(outputSubstParams)) {
    SILType outputComponentTy =
      outputPackTy->getSILElementType(outputComponentIndex);
    auto outputSubstType =
      outputSubstParams[outputComponentIndex].getParameterType();

    auto inputSubstType = inputParam.getSubstParam().getParameterType();
    auto inputOrigType = inputParam.getOrigType();

    auto insertScalarIntoPack = [&](ManagedValue output) {
      assert(!outputComponentTy.is<PackExpansionType>());
      auto outputPackIndex =
        SGF.B.createScalarPackIndex(Loc, outputComponentIndex,
                                    outputFormalPackType);
      SGF.B.createPackElementSet(Loc, output.getValue(),
                                 outputPackIndex, outputPackAddr);
      if (output.hasCleanup())
        outputComponentCleanups.push_back(output.getCleanup());
    };

    // If we're not translating from a pack expansion, translate into a
    // single component.  This may claim any number of input values.
    if (!inputParam.isOrigPackExpansion()) {
      // Fake up a lowered parameter as if we could pass just this
      // component.
      SILParameterInfo outputComponentParam(outputComponentTy.getASTType(),
        getScalarConventionForPackConvention(outputPackParam.getConvention()));

      ManagedValue output = translateIntoSingle(inputOrigType,
                                                inputSubstType,
                                                outputOrigPatternType,
                                                outputSubstType,
                                                outputComponentParam);
      insertScalarIntoPack(output);

    // Otherwise, we're starting with the input value from the pack.
    } else {
      auto inputOrigPatternType = inputOrigType.getPackExpansionPatternType();
      auto inputComponentIndex = inputParam.getPackComponentIndex();
      auto inputPackValue = inputParam.getPackValue();
      auto inputPackTy = inputPackValue.getType().castTo<SILPackType>();
      auto inputComponentTy =
        inputPackTy->getSILElementType(inputComponentIndex);

      // If we have a pack expansion component, emit a pack loop.
      if (auto outputExpansionTy =
            outputComponentTy.getAs<PackExpansionType>()) {
        auto inputExpansionTy = inputComponentTy.castTo<PackExpansionType>();

        // Claim the pack-expansion component and set up to clone its
        // cleanup onto the elements.
        auto inputComponent = inputParam.projectPackComponent(SGF, Loc);

        // We can only do direct forwarding of of the pack elements in
        // one very specific case right now.  That isn't great, but we
        // have to live with it.
        bool forwardInputToOutput =
          (inputExpansionTy.getPatternType()
             == outputExpansionTy.getPatternType());

        // The result of the transformation will be +1 unless we do that.
        bool outputIsPlusOne = !forwardInputToOutput;

        ManagedValue output =
          SGF.emitPackTransform(Loc, inputComponent,
                                inputParam.getFormalPackType(),
                                inputComponentIndex,
                                outputPackAddr,
                                outputFormalPackType,
                                outputComponentIndex,
                                /*is trivial*/ forwardInputToOutput,
                                outputIsPlusOne,
            [&](ManagedValue inputEltAddr, SILType outputEltTy,
                SGFContext ctxt) {
          // If we decided to just forward, we can do that now.
          if (forwardInputToOutput)
            return inputEltAddr;

          // Otherwise, map the subst pattern types into element context.
          CanType outputSubstEltType =
            cast<PackExpansionType>(outputSubstType).getPatternType();
          CanType inputSubstEltType =
            cast<PackExpansionType>(inputSubstType).getPatternType();
          if (auto openedEnv =
                SGF.getInnermostPackExpansion()->OpenedElementEnv) {
            inputSubstEltType = openedEnv
              ->mapContextualPackTypeIntoElementContext(inputSubstEltType);
            outputSubstEltType = openedEnv
              ->mapContextualPackTypeIntoElementContext(outputSubstEltType);
          }

          auto init = ctxt.getEmitInto();
          assert(init);
          translateSingleInto(inputOrigPatternType, inputSubstEltType,
                              outputOrigPatternType, outputSubstEltType,
                              inputEltAddr, outputEltTy, *init);
          return ManagedValue::forInContext();
        });

        if (output.hasCleanup())
          outputComponentCleanups.push_back(output.getCleanup());

      // Otherwise, claim the next pack component and translate it.
      } else {
        SILParameterInfo outputComponentParam(outputComponentTy.getASTType(),
          getScalarConventionForPackConvention(outputPackParam.getConvention()));

        ManagedValue input = inputParam.projectPackComponent(SGF, Loc);
        ManagedValue output =
          translateSingle(inputOrigPatternType, inputSubstType,
                          outputOrigPatternType, outputSubstType,
                          input, outputComponentParam);
        insertScalarIntoPack(output);
      }
    }

    inputParam.advance();
  }

  // Wrap up the value.
  switch (outputPackParam.getConvention()) {
  case ParameterConvention::Indirect_In:
  case ParameterConvention::Indirect_In_Guaranteed:
  case ParameterConvention::Indirect_Inout:
  case ParameterConvention::Indirect_InoutAliasable:
  case ParameterConvention::Direct_Owned:
  case ParameterConvention::Direct_Unowned:
  case ParameterConvention::Direct_Guaranteed:
    llvm_unreachable("not a pack parameter convention");

  case ParameterConvention::Pack_Inout:
    llvm_unreachable("pack inout reabstraction not supported");

  // For guaranteed, leave the cleanups in place and produce a
  // borrowed value.
  case ParameterConvention::Pack_Guaranteed:
    return ManagedValue::forBorrowedAddressRValue(outputPackAddr);

  // For owned, forward all the cleanups and enter a new cleanup
  // for the entire pack.
  case ParameterConvention::Pack_Owned: {
    if (outputComponentCleanups.empty())
      return ManagedValue::forTrivialAddressRValue(outputPackAddr);

    for (auto cleanup : outputComponentCleanups) {
      SGF.Cleanups.forwardCleanup(cleanup);
    }

    auto packCleanup =
      SGF.enterDestroyPackCleanup(outputPackAddr, outputFormalPackType);
    return ManagedValue::forOwnedAddressRValue(outputPackAddr, packCleanup);
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

    if (argTy.isConsumed()) {
      forwardedArgs.push_back(arg.ensurePlusOne(SGF, loc).forward(SGF));
      continue;
    }

    if (isGuaranteedParameter(argTy.getConvention())) {
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
  case ParameterConvention::Indirect_In:
  case ParameterConvention::Pack_Owned:
    return SGF.emitManagedRValueWithCleanup(value);
  case ParameterConvention::Direct_Guaranteed:
  case ParameterConvention::Direct_Unowned:
  case ParameterConvention::Pack_Guaranteed:
    if (value->getOwnershipKind() == OwnershipKind::None)
      return ManagedValue::forUnmanaged(value);
    return ManagedValue::forBorrowedObjectRValue(value);
  case ParameterConvention::Indirect_In_Guaranteed: {
    if (SGF.silConv.useLoweredAddresses()) {
      return ManagedValue::forBorrowedAddressRValue(value);
    }
    if (value->getType().isTrivial(SGF.F)) {
      return ManagedValue::forTrivialObjectRValue(value);
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

  translator.translate(innerInfos.getOrigTypes(), innerInfos.getSubstTypes(),
                       outerInfos.getOrigTypes(), outerInfos.getSubstTypes());

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
class ResultPlanner {
  SILGenFunction &SGF;
  SILLocation Loc;

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

  struct PlanData {
    ArrayRef<SILResultInfo> OuterResults;
    ArrayRef<SILResultInfo> InnerResults;
    SmallVectorImpl<SILValue> &InnerIndirectResultAddrs;
    size_t NextOuterIndirectResultIndex;
  };

  SmallVector<Operation, 8> Operations;
public:
  ResultPlanner(SILGenFunction &SGF, SILLocation loc) : SGF(SGF), Loc(loc) {}

  void plan(AbstractionPattern innerOrigType, CanType innerSubstType,
            AbstractionPattern outerOrigType, CanType outerSubstType,
            CanSILFunctionType innerFnType, CanSILFunctionType outerFnType,
            SmallVectorImpl<SILValue> &innerIndirectResultAddrs) {
    // Assert that the indirect results are set up like we expect.
    assert(innerIndirectResultAddrs.empty());
    assert(SGF.F.begin()->args_size()
           >= SILFunctionConventions(outerFnType, SGF.SGM.M)
                  .getNumIndirectSILResults());

    innerIndirectResultAddrs.reserve(
        SILFunctionConventions(innerFnType, SGF.SGM.M)
            .getNumIndirectSILResults());

    PlanData data = {outerFnType->getUnsubstitutedType(SGF.SGM.M)->getResults(),
                     innerFnType->getUnsubstitutedType(SGF.SGM.M)->getResults(),
                     innerIndirectResultAddrs, 0};

    // Recursively walk the result types.
    plan(innerOrigType, innerSubstType, outerOrigType, outerSubstType, data);

    // Assert that we consumed and produced all the indirect result
    // information we needed.
    assert(data.OuterResults.empty());
    assert(data.InnerResults.empty());
    assert(data.InnerIndirectResultAddrs.size() ==
           SILFunctionConventions(innerFnType, SGF.SGM.M)
               .getNumIndirectSILResults());
    assert(data.NextOuterIndirectResultIndex
           == SILFunctionConventions(outerFnType, SGF.SGM.M)
                  .getNumIndirectSILResults());
  }

  SILValue execute(SILValue innerResult, CanSILFunctionType innerFuncTy);

private:
  void execute(SmallVectorImpl<SILValue> &innerDirectResults,
               SmallVectorImpl<SILValue> &outerDirectResults);
  void executeInnerTuple(SILValue innerElement,
                         SmallVector<SILValue, 4> &innerDirectResults);

  void plan(AbstractionPattern innerOrigType, CanType innerSubstType,
            AbstractionPattern outerOrigType, CanType outerSubstType,
            PlanData &planData);

  void planParallelExpanded(AbstractionPattern innerOrigType,
                            CanType innerSubstType,
                            AbstractionPattern outerOrigType,
                            CanType outerSubstType,
                            PlanData &planData);
  void planFromExpanded(AbstractionPattern innerOrigType,
                        CanType innerSubstType,
                        AbstractionPattern outerOrigType,
                        CanType outerSubstType,
                        PlanData &planData);
  void planIntoExpanded(AbstractionPattern innerOrigType,
                        CanType innerSubstType,
                        AbstractionPattern outerOrigType,
                        CanType outerSubstType,
                        PlanData &planData);
  void planSingle(AbstractionPattern innerOrigType,
                  CanType innerSubstType,
                  AbstractionPattern outerOrigType,
                  CanType outerSubstType,
                  PlanData &planData,
                  SILResultInfo innerResult,
                  SILResultInfo outerResult,
                  SILValue optOuterResultAddr);

  void expandVanishingTuple(AbstractionPattern origType, CanType substType,
                            PlanData &planData, bool forInner,
            llvm::function_ref<void(AbstractionPattern origType,
                                    CanType substType)> handleSingle,
            llvm::function_ref<void(AbstractionPattern origType,
                                    CanType substType,
                                    SILValue eltResult)> handlePackElement);

  void planIntoIndirect(AbstractionPattern innerOrigType,
                        CanType innerSubstType,
                        AbstractionPattern outerOrigType,
                        CanType outerSubstType,
                        PlanData &planData,
                        SILValue outerResultAddr);
  void planExpandedIntoIndirect(AbstractionPattern innerOrigType,
                                CanTupleType innerSubstType,
                                AbstractionPattern outerOrigType,
                                CanType outerSubstType,
                                PlanData &planData,
                                SILValue outerResultAddr);
  void planSingleIntoIndirect(AbstractionPattern innerOrigType,
                              CanType innerSubstType,
                              AbstractionPattern outerOrigType,
                              CanType outerSubstType,
                              PlanData &planData,
                              SILResultInfo innerResult,
                              SILValue outerResultAddr);

  void planIntoDirect(AbstractionPattern innerOrigType,
                      CanType innerSubstType,
                      AbstractionPattern outerOrigType,
                      CanType outerSubstType,
                      PlanData &planData,
                      SILResultInfo outerResult);
  void planSingleIntoDirect(AbstractionPattern innerOrigType,
                            CanType innerSubstType,
                            AbstractionPattern outerOrigType,
                            CanType outerSubstType,
                            PlanData &planData,
                            SILResultInfo innerResult,
                            SILResultInfo outerResult);
  void planExpandedIntoDirect(AbstractionPattern innerOrigType,
                              CanTupleType innerSubstType,
                              AbstractionPattern outerOrigType,
                              CanType outerSubstType,
                              PlanData &planData,
                              SILResultInfo outerResult);

  void planFromIndirect(AbstractionPattern innerOrigType,
                        CanType innerSubstType,
                        AbstractionPattern outerOrigType,
                        CanType outerSubstType,
                        PlanData &planData,
                        SILValue innerResultAddr);
  void planFromDirect(AbstractionPattern innerOrigType,
                      CanType innerSubstType,
                      AbstractionPattern outerOrigType,
                      CanType outerSubstType,
                      PlanData &planData,
                      SILResultInfo innerResult);
  void planExpandedFromIndirect(AbstractionPattern innerOrigType,
                                CanTupleType innerSubstType,
                                AbstractionPattern outerOrigType,
                                CanTupleType outerSubstType,
                                PlanData &planData,
                                SILValue innerResultAddr);
  void planExpandedFromDirect(AbstractionPattern innerOrigType,
                              CanTupleType innerSubstType,
                              AbstractionPattern outerOrigType,
                              CanTupleType outerSubstType,
                              PlanData &planData,
                              SILResultInfo innerResult);
  void planSingleFromIndirect(AbstractionPattern innerOrigType,
                              CanType innerSubstType,
                              AbstractionPattern outerOrigType,
                              CanType outerSubstType,
                              SILValue innerResultAddr,
                              SILResultInfo outerResult,
                              SILValue optOuterResultAddr);
  void planIndirectIntoIndirect(AbstractionPattern innerOrigType,
                                CanType innerSubstType,
                                AbstractionPattern outerOrigType,
                                CanType outerSubstType,
                                SILValue innerResultAddr,
                                SILValue outerResultAddr);

  void planPackExpansionFromPack(AbstractionPattern innerOrigType,
                                 CanType innerSubstType,
                                 AbstractionPattern outerOrigType,
                                 CanType outerSubstType,
                                 CanPackType innerFormalPackType,
                                 SILValue innerPackAddr,
                                 unsigned innerPackComponentIndex,
                                 CanPackType outerFormalPackType,
                                 SILValue outerTupleOrPackAddr,
                                 unsigned outerPackComponentIndex);
  void planPackExpansionFromTuple(AbstractionPattern innerOrigType,
                                  CanType innerSubstType,
                                  AbstractionPattern outerOrigType,
                                  CanType outerSubstType,
                                  CanPackType innerFormalPackType,
                                  SILValue innerTupleAddr,
                                  unsigned innerComponentIndex,
                                  CanPackType outerFormalPackType,
                                  SILValue outerTupleOrPackAddr,
                                  unsigned outerComponentIndex);

  /// Claim the next inner result from the plan data.
  SILResultInfo claimNextInnerResult(PlanData &data) {
    return claimNext(data.InnerResults);
  }

  /// Claim the next outer result from the plan data.  If it's indirect,
  /// grab its SILArgument.
  std::pair<SILResultInfo, SILValue> claimNextOuterResult(PlanData &data) {
    SILResultInfo result = claimNext(data.OuterResults);

    SILValue resultAddr;
    if (SGF.silConv.isSILIndirect(result)) {
      resultAddr =
          SGF.F.begin()->getArgument(data.NextOuterIndirectResultIndex++);
    }

    return { result, resultAddr };
  }

  /// Create a temporary address suitable for passing to the given inner
  /// indirect result and add it as an inner indirect result.
  SILValue addInnerIndirectResultTemporary(PlanData &data,
                                           SILResultInfo innerResult) {
    assert(SGF.silConv.isSILIndirect(innerResult) ||
           !SGF.silConv.useLoweredAddresses());
    auto temporary =
        SGF.emitTemporaryAllocation(Loc,
                            SGF.getSILType(innerResult, CanSILFunctionType()));
    data.InnerIndirectResultAddrs.push_back(temporary);
    return temporary;
  }

  SILValue addInnerIndirectPackResultTemporary(PlanData &data,
                                               SILResultInfo innerResult) {
    assert(innerResult.isPack());
    assert(SGF.silConv.isSILIndirect(innerResult));
    auto temporary =
        SGF.emitTemporaryPackAllocation(Loc,
                            SGF.getSILType(innerResult, CanSILFunctionType()));
    data.InnerIndirectResultAddrs.push_back(temporary);
    return temporary;
  }

  class InnerPackResultGenerator {
    ResultPlanner &planner;
    PlanData &data;
  public:
    InnerPackResultGenerator(ResultPlanner &planner, PlanData &data)
      : planner(planner), data(data) {}

    bool isFinished() const {
      return data.InnerResults.empty();
    }
    ManagedValue claimNext() {
      SILResultInfo resultInfo = planner.claimNextInnerResult(data);
      return ManagedValue::forLValue(
        planner.addInnerIndirectPackResultTemporary(data, resultInfo));
    }
    void advance() {
      llvm_unreachable("should always be claimed from");
    }
    void finish() {
      llvm_unreachable("should not be finished directly");
    }
  };

  class OuterPackResultGenerator {
    ResultPlanner &planner;
    PlanData &data;
  public:
    OuterPackResultGenerator(ResultPlanner &planner, PlanData &data)
      : planner(planner), data(data) {}

    bool isFinished() const {
      return data.OuterResults.empty();
    }
    ManagedValue claimNext() {
      auto resultPair = planner.claimNextOuterResult(data);
      assert(resultPair.first.isPack());
      return ManagedValue::forLValue(resultPair.second);
    }
    void advance() {
      llvm_unreachable("should always be claimed from");
    }
    void finish() {
      llvm_unreachable("should not be finished directly");
    }
  };

  bool hasAbstractionDifference(SILValue resultAddr1,
                                SILValue resultAddr2) {
    return hasAbstractionDifference(resultAddr1->getType(),
                                    resultAddr2->getType());
  }

  bool hasAbstractionDifference(SILType resultType1,
                                SILType resultType2) {
    return resultType1.getASTType() != resultType2.getASTType();
  }

  bool hasAbstractionDifference(SILValue resultAddr,
                                SILResultInfo resultInfo) {
    return resultAddr->getType().getASTType()
        != resultInfo.getInterfaceType();
  }

  /// Cause the next inner indirect result to be emitted directly into
  /// the given outer result address.
  void addInPlace(PlanData &data, SILValue outerResultAddr) {
    data.InnerIndirectResultAddrs.push_back(outerResultAddr);
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
                                           CanType innerSubstType,
                                           AbstractionPattern outerOrigType,
                                           CanType outerSubstType,
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

/// Plan the reabstraction of a call result.
void ResultPlanner::plan(AbstractionPattern innerOrigType,
                         CanType innerSubstType,
                         AbstractionPattern outerOrigType,
                         CanType outerSubstType,
                         PlanData &planData) {
  // The substituted types must match up in tuple-ness and arity.
  assert(
      isa<TupleType>(innerSubstType) == isa<TupleType>(outerSubstType) ||
      (isa<TupleType>(innerSubstType) &&
       (outerSubstType->isAny() || outerSubstType->getOptionalObjectType())));
  assert(!isa<TupleType>(outerSubstType) ||
         cast<TupleType>(innerSubstType)->getNumElements() ==
           cast<TupleType>(outerSubstType)->getNumElements());

  // Tuple results in the abstraction pattern are expanded.
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
    expandVanishingTuple(outerOrigType, outerSubstType, planData, /*inner*/ false,
       [&](AbstractionPattern outerOrigEltType, CanType outerSubstEltType) {
      plan(innerOrigType, innerSubstType,
           outerOrigEltType, outerSubstEltType, planData);
    }, [&](AbstractionPattern outerOrigEltType, CanType outerSubstEltType,
           SILValue outerResultAddr) {
      planIntoIndirect(innerOrigType, innerSubstType,
                       outerOrigEltType, outerSubstEltType,
                       planData, outerResultAddr);
    });
    return;
  }

  // If the inner abstraction pattern is a vanishing tuple, look
  // through that and recurse.
  bool innerIsExpanded = innerOrigType.isTuple();
  if (innerIsExpanded && innerOrigType.doesTupleVanish()) {
    expandVanishingTuple(innerOrigType, innerSubstType, planData, /*inner*/ true,
       [&](AbstractionPattern innerOrigEltType, CanType innerSubstEltType) {
      plan(innerOrigEltType, innerSubstEltType,
           outerOrigType, outerSubstType, planData);
    }, [&](AbstractionPattern innerOrigEltType, CanType innerSubstEltType,
           SILValue innerResultAddr) {
      planFromIndirect(innerOrigEltType, innerSubstEltType,
                       outerOrigType, outerSubstType,
                       planData, innerResultAddr);
    });
    return;
  }

  if (innerIsExpanded && outerIsExpanded) {
    planParallelExpanded(innerOrigType, innerSubstType,
                         outerOrigType, outerSubstType, planData);
  } else if (outerIsExpanded) {
    planIntoExpanded(innerOrigType, innerSubstType,
                     outerOrigType, outerSubstType, planData);
  } else if (innerIsExpanded) {
    planFromExpanded(innerOrigType, innerSubstType,
                     outerOrigType, outerSubstType, planData);
  } else {
    SILResultInfo innerResult = claimNextInnerResult(planData);
    auto outerResultPair = claimNextOuterResult(planData);
    SILResultInfo outerResult = outerResultPair.first;
    SILValue outerResultAddr = outerResultPair.second;
    planSingle(innerOrigType, innerSubstType,
               outerOrigType, outerSubstType,
               planData, innerResult, outerResult, outerResultAddr);
  }
}

void ResultPlanner::expandVanishingTuple(AbstractionPattern origType,
                                         CanType substType,
                                         PlanData &planData,
                                         bool forInner,
          llvm::function_ref<void(AbstractionPattern origType,
                                  CanType substType)> handleSingle,
          llvm::function_ref<void(AbstractionPattern origType,
                                  CanType substType,
                                  SILValue eltResult)> handlePackElement) {

  bool foundSurvivor = false;

  llvm::Optional<InnerPackResultGenerator> innerPacks;
  llvm::Optional<OuterPackResultGenerator> outerPacks;
  llvm::Optional<SimpleGeneratorRef<ManagedValue>> packInputs;
  if (forInner) {
    innerPacks.emplace(*this, planData);
    packInputs.emplace(*innerPacks);
  } else {
    outerPacks.emplace(*this, planData);
    packInputs.emplace(*outerPacks);
  }

  ExpandedTupleInputGenerator elt(SGF.getASTContext(), *packInputs,
                                  origType, substType);
  for (; !elt.isFinished(); elt.advance()) {
    assert(!foundSurvivor);
    foundSurvivor = true;

    if (!elt.isOrigPackExpansion()) {
      handleSingle(elt.getOrigType(), elt.getSubstType());
    } else {
      assert(elt.getPackComponentIndex() == 0);
      assert(!isa<PackExpansionType>(elt.getSubstType()));
      SILValue eltAddr = [&] {
        if (forInner)
          return elt.createPackComponentTemporary(SGF, Loc);
        return elt.projectPackComponent(SGF, Loc).getLValueAddress();
      }();
      handlePackElement(elt.getOrigType().getPackExpansionPatternType(),
                        substType, eltAddr);
    }
  }
  elt.finish();

  assert(foundSurvivor && "vanishing tuple had no surviving element?");
}

void ResultPlanner::planParallelExpanded(AbstractionPattern innerOrigType,
                                         CanType innerSubstType,
                                         AbstractionPattern outerOrigType,
                                         CanType outerSubstType,
                                         PlanData &planData) {
  assert(innerOrigType.isTuple());
  assert(!innerOrigType.doesTupleVanish());
  assert(outerOrigType.isTuple());
  assert(!outerOrigType.doesTupleVanish());

  auto &ctx = SGF.getASTContext();
  InnerPackResultGenerator innerPacks(*this, planData);
  OuterPackResultGenerator outerPacks(*this, planData);
  ExpandedTupleInputGenerator innerElt(ctx, innerPacks,
                                       innerOrigType, innerSubstType);
  ExpandedTupleInputGenerator outerElt(ctx, outerPacks,
                                       outerOrigType, outerSubstType);

  for (; !innerElt.isFinished(); innerElt.advance(), outerElt.advance()) {
    assert(!outerElt.isFinished() && "elements not parallel");
    assert(outerElt.isSubstPackExpansion() == innerElt.isSubstPackExpansion());

    // If this substituted component is a pack expansion, handle that
    // immediately.
    if (auto outerSubstExpansionType =
          dyn_cast<PackExpansionType>(outerElt.getSubstType())) {
      planPackExpansionFromPack(innerElt.getOrigType(),
        cast<PackExpansionType>(innerElt.getSubstType()).getPatternType(),
                                outerElt.getOrigType(),
                                outerSubstExpansionType.getPatternType(),
                                innerElt.getFormalPackType(),
                                innerElt.getPackValue().getLValueAddress(),
                                innerElt.getPackComponentIndex(),
                                outerElt.getFormalPackType(),
                                outerElt.getPackValue().getLValueAddress(),
                                outerElt.getPackComponentIndex());
      continue;
    }

    AbstractionPattern outerOrigEltType = outerElt.getOrigType();
    AbstractionPattern innerOrigEltType = innerElt.getOrigType();
    CanType outerSubstEltType = outerElt.getSubstType();
    CanType innerSubstEltType = innerElt.getSubstType();

    if (outerElt.isOrigPackExpansion()) {
      auto outerEltAddr =
        outerElt.projectPackComponent(SGF, Loc).getLValueAddress();
      if (innerElt.isOrigPackExpansion()) {
        auto innerEltAddr =
          innerElt.createPackComponentTemporary(SGF, Loc);
        planIndirectIntoIndirect(innerOrigEltType, innerSubstEltType,
                                 outerOrigEltType, outerSubstEltType,
                                 innerEltAddr, outerEltAddr);
      } else {
        planIntoIndirect(innerOrigEltType, innerSubstEltType,
                         outerOrigEltType, outerSubstEltType,
                         planData, outerEltAddr);
      }
    } else {
      if (innerElt.isOrigPackExpansion()) {
        auto innerEltAddr = innerElt.createPackComponentTemporary(SGF, Loc);
        planFromIndirect(innerOrigEltType, innerSubstEltType,
                         outerOrigEltType, outerSubstEltType,
                         planData, innerEltAddr);
      } else {
        plan(innerOrigEltType, innerSubstEltType,
             outerOrigEltType, outerSubstEltType,
             planData);
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

/// We have a pack expansion in a substituted result type, and the inner
/// result type is expanded.  Emit a pack loop to set up the indirect
/// result pack for the callee, then add an operation to reabstract the
/// result back if necessary.
void ResultPlanner::planPackExpansionFromPack(
                      AbstractionPattern innerOrigType,
                      CanType innerSubstType,
                      AbstractionPattern outerOrigType,
                      CanType outerSubstType,
                      CanPackType innerFormalPackType,
                      SILValue innerPackAddr,
                      unsigned innerPackComponentIndex,
                      CanPackType outerFormalPackType,
                      SILValue outerTupleOrPackAddr,
                      unsigned outerComponentIndex) {
  // The orig and subst types are the pattern types, not the expansion types.

  SILType innerPackExpansionTy =
    innerPackAddr->getType().getPackElementType(innerPackComponentIndex);
  SILType outerPackExpansionTy =
    getTupleOrPackElementType(outerTupleOrPackAddr->getType(),
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
  SGF.emitDynamicPackLoop(Loc, innerFormalPackType, innerPackComponentIndex,
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
                                                outerTupleOrPackAddr,
                                                outerEltTy);
    }

    SGF.B.createPackElementSet(Loc, innerEltAddr, innerPackIndex,
                               innerPackAddr);
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
                                        outerTupleOrPackAddr,
                                        outerComponentIndex);
  }
}

/// We have a pack expansion in a substituted result type, and the inner
/// result type is not expanded.  Add an operation to move the result
/// into the outer pack or tuple, reabstracting if necessary.
void ResultPlanner::planPackExpansionFromTuple(
                      AbstractionPattern innerOrigType,
                      CanType innerSubstType,
                      AbstractionPattern outerOrigType,
                      CanType outerSubstType,
                      CanPackType innerInducedPackType,
                      SILValue innerTupleAddr,
                      unsigned innerComponentIndex,
                      CanPackType outerFormalPackType,
                      SILValue outerPackOrTupleAddr,
                      unsigned outerComponentIndex) {
  // The orig and subst types are the pattern types, not the expansion types.
  addReabstractTupleIntoPackExpansion(innerOrigType, innerSubstType,
                                      outerOrigType, outerSubstType,
                                      innerInducedPackType,
                                      innerTupleAddr,
                                      innerComponentIndex,
                                      outerFormalPackType,
                                      outerPackOrTupleAddr,
                                      outerComponentIndex);
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
  auto openedEnv = SGF.createOpenedElementValueEnvironment(
                   { innerPackExpansionTy, outerPackExpansionTy },
                   { &innerEltTy, &outerEltTy });

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

    CanType innerSubstType = InnerSubstType;
    CanType outerSubstType = OuterSubstType;
    if (openedEnv) {
      innerSubstType =
        openedEnv->mapContextualPackTypeIntoElementContext(innerSubstType);
      outerSubstType =
        openedEnv->mapContextualPackTypeIntoElementContext(outerSubstType);
    }

    // Reabstract.
    auto outerEltValue =
      SGF.emitTransformedValue(loc, innerEltValue,
                               InnerOrigType, innerSubstType,
                               OuterOrigType, outerSubstType,
                               outerEltTy, outerResultCtxt);

    // Force the value into the outer result address if necessary.
    if (!outerEltValue.isInContext()) {
      outerEltValue.forwardInto(SGF, loc, outerEltAddr);
    }
  });
}

void ResultPlanner::planIntoExpanded(AbstractionPattern innerOrigType,
                                     CanType innerSubstType,
                                     AbstractionPattern outerOrigType,
                                     CanType outerSubstType,
                                     PlanData &planData) {
  assert(outerOrigType.isTuple());
  assert(!outerOrigType.doesTupleVanish());
  assert(!innerOrigType.isTuple());

  // We know that the outer tuple is not vanishing (because the top-level
  // plan function filters that out), so the outer subst type must be a
  // tuple.  The inner subst type must also be a tuple because only tuples
  // convert to tuples.
  auto outerSubstTupleType = cast<TupleType>(outerSubstType);
  auto innerSubstTupleType = cast<TupleType>(innerSubstType);

  // The next inner result is not expanded, so there's a single result.
  SILResultInfo innerResult = claimNextInnerResult(planData);

  if (SGF.silConv.isSILIndirect(innerResult)) {
    SILValue innerResultAddr =
        addInnerIndirectResultTemporary(planData, innerResult);
    planExpandedFromIndirect(innerOrigType, innerSubstTupleType,
                             outerOrigType, outerSubstTupleType,
                             planData, innerResultAddr);
  } else {
    assert(!SGF.silConv.useLoweredAddresses() &&
           "Formal Indirect Results that are not SIL Indirect are only "
           "allowed in opaque values mode");
    planExpandedFromDirect(innerOrigType, innerSubstTupleType,
                           outerOrigType, outerSubstTupleType,
                           planData, innerResult);
  }
}

void ResultPlanner::planFromExpanded(AbstractionPattern innerOrigType,
                                     CanType innerSubstType,
                                     AbstractionPattern outerOrigType,
                                     CanType outerSubstType,
                                     PlanData &planData) {
  assert(innerOrigType.isTuple());
  assert(!innerOrigType.doesTupleVanish());
  assert(!outerOrigType.isTuple());

  // We know that the inner tuple is not vanishing (because the top-level
  // plan function filters that out), so the inner subst type must be a
  // tuple.  The outer subst type might not be a tuple if it's e.g. Any.
  auto innerSubstTupleType = cast<TupleType>(innerSubstType);

  // The next outer result is not expanded, so there's a single result.
  auto outerResultPair = claimNextOuterResult(planData);
  SILResultInfo outerResult = outerResultPair.first;
  SILValue outerResultAddr = outerResultPair.second;

  // Base the plan on whether the single result is direct or indirect.
  if (SGF.silConv.isSILIndirect(outerResult)) {
    assert(outerResultAddr);
    planExpandedIntoIndirect(innerOrigType, innerSubstTupleType,
                             outerOrigType, outerSubstType,
                             planData, outerResultAddr);
  } else {
    assert(!outerResultAddr);
    planExpandedIntoDirect(innerOrigType, innerSubstTupleType,
                           outerOrigType, outerSubstType,
                           planData, outerResult);
  }
}

void ResultPlanner::planSingle(AbstractionPattern innerOrigType,
                               CanType innerSubstType,
                               AbstractionPattern outerOrigType,
                               CanType outerSubstType,
                               PlanData &planData,
                               SILResultInfo innerResult,
                               SILResultInfo outerResult,
                               SILValue optOuterResultAddr) {
  // If the outer result is indirect, plan to emit into that.
  if (SGF.silConv.isSILIndirect(outerResult)) {
    assert(optOuterResultAddr);
    planSingleIntoIndirect(innerOrigType, innerSubstType,
                           outerOrigType, outerSubstType,
                           planData, innerResult, optOuterResultAddr);

  } else {
    assert(!optOuterResultAddr);
    planSingleIntoDirect(innerOrigType, innerSubstType,
                         outerOrigType, outerSubstType,
                         planData, innerResult, outerResult);
  }
}

/// Plan the emission of a call result into an outer result address.
void ResultPlanner::planIntoIndirect(AbstractionPattern innerOrigType,
                                     CanType innerSubstType,
                                     AbstractionPattern outerOrigType,
                                     CanType outerSubstType,
                                     PlanData &planData,
                                     SILValue outerResultAddr) {
  // outerOrigType can be a tuple if we're e.g. injecting into an optional;
  // we just know we're not expanding it.

  // If the inner pattern is not a tuple, claim the next inner result
  // and plan from that.
  if (!innerOrigType.isTuple()) {
    // Claim the next inner result.
    SILResultInfo innerResult = claimNextInnerResult(planData);

    planSingleIntoIndirect(innerOrigType, innerSubstType,
                           outerOrigType, outerSubstType,
                           planData, innerResult, outerResultAddr);
    return;
  }

  // If the inner pattern is not vanishing, expand the tuple.
  if (!innerOrigType.doesTupleVanish()) {
    planExpandedIntoIndirect(innerOrigType, cast<TupleType>(innerSubstType),
                             outerOrigType, outerSubstType,
                             planData, outerResultAddr);
    return;
  }

  // Otherwise, we have a vanishing tuple.  Expand it, find the surviving
  // element, and recurse.
  expandVanishingTuple(innerOrigType, innerSubstType, planData, /*inner*/ true,
     [&](AbstractionPattern innerOrigEltType, CanType innerSubstEltType) {
    planIntoIndirect(innerOrigEltType, innerSubstEltType,
                     outerOrigType, outerSubstType, planData,
                     outerResultAddr);
  }, [&](AbstractionPattern innerOrigEltType,
         CanType innerSubstEltType, SILValue innerResultAddr) {
    planIndirectIntoIndirect(innerOrigEltType, innerSubstEltType,
                             outerOrigType, outerSubstType,
                             innerResultAddr, outerResultAddr);
  });
}

/// Plan the emission of a call result into an outer result address,
/// given that the inner abstraction pattern is a tuple.
void
ResultPlanner::planExpandedIntoIndirect(AbstractionPattern innerOrigType,
                                        CanTupleType innerSubstType,
                                        AbstractionPattern outerOrigType,
                                        CanType outerSubstType,
                                        PlanData &planData,
                                        SILValue outerResultAddr) {
  assert(innerOrigType.isTuple());
  assert(!innerOrigType.doesTupleVanish());
  // outerOrigType can be a tuple if we're doing something like
  // injecting into an optional tuple.

  auto outerSubstTupleType = dyn_cast<TupleType>(outerSubstType);

  // If the outer type is not a tuple, it must be optional.
  if (!outerSubstTupleType) {
    // Figure out what kind of optional it is.
    CanType outerSubstObjectType = outerSubstType.getOptionalObjectType();
    if (outerSubstObjectType) {
      auto someDecl = SGF.getASTContext().getOptionalSomeDecl();

      // Prepare the value slot in the optional value.
      SILType outerObjectType =
          outerResultAddr->getType().getOptionalObjectType();
      SILValue outerObjectResultAddr
        = SGF.B.createInitEnumDataAddr(Loc, outerResultAddr, someDecl,
                                       outerObjectType);

      // Emit into that address.
      planExpandedIntoIndirect(
          innerOrigType, innerSubstType, outerOrigType.getOptionalObjectType(),
          outerSubstObjectType, planData, outerObjectResultAddr);

      // Add an operation to finish the enum initialization.
      addInjectOptionalIndirect(someDecl, outerResultAddr);
      return;
    }

    assert(outerSubstType->isAny());

    // Prepare the value slot in the existential.
    auto opaque = AbstractionPattern::getOpaque();
    SILValue outerConcreteResultAddr
      = SGF.B.createInitExistentialAddr(Loc, outerResultAddr, innerSubstType,
                                        SGF.getLoweredType(opaque, innerSubstType),
                                        /*conformances=*/{});

    // Emit into that address.
    planExpandedIntoIndirect(innerOrigType, innerSubstType,
                             innerOrigType, innerSubstType,
                             planData, outerConcreteResultAddr);
    return;
  }

  // Otherwise, we're doing a tuple-to-tuple conversion, which is a
  // parallel walk.

  auto &ctx = SGF.getASTContext();
  InnerPackResultGenerator innerPacks(*this, planData);
  ExpandedTupleInputGenerator innerElt(ctx, innerPacks,
                                       innerOrigType, innerSubstType);
  TupleElementAddressGenerator outerElt(ctx,
                                  ManagedValue::forLValue(outerResultAddr),
                                        outerOrigType,
                                        outerSubstTupleType);

  for (; !outerElt.isFinished(); outerElt.advance(), innerElt.advance()) {
    assert(!innerElt.isFinished());
    assert(innerElt.isSubstPackExpansion() == outerElt.isSubstPackExpansion());

    SILValue outerEltAddr =
      outerElt.projectElementAddress(SGF, Loc).getLValueAddress();

    if (!innerElt.isOrigPackExpansion()) {
      planIntoIndirect(innerElt.getOrigType(), innerElt.getSubstType(),
                       outerElt.getOrigType(), outerElt.getSubstType(),
                       planData, outerEltAddr);
      continue;
    }

    if (!innerElt.isSubstPackExpansion()) {
      SILValue innerEltAddr = innerElt.createPackComponentTemporary(SGF, Loc);
      planIndirectIntoIndirect(innerElt.getOrigType(), innerElt.getSubstType(),
                               outerElt.getOrigType(), outerElt.getSubstType(),
                               innerEltAddr, outerEltAddr);
      continue;
    }

    planPackExpansionFromPack(innerElt.getOrigType(),
      cast<PackExpansionType>(innerElt.getSubstType()).getPatternType(),
                              outerElt.getOrigType(),
      cast<PackExpansionType>(outerElt.getSubstType()).getPatternType(),
                              innerElt.getFormalPackType(),
                              innerElt.getPackValue().getLValueAddress(),
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
                                   PlanData &planData,
                                   SILResultInfo outerResult) {
  assert(!outerOrigType.isTuple() || !SGF.silConv.useLoweredAddresses());

  // If the inner pattern is scalar, claim the next inner result.
  if (!innerOrigType.isTuple()) {
    SILResultInfo innerResult = claimNextInnerResult(planData);

    planSingleIntoDirect(innerOrigType, innerSubstType,
                         outerOrigType, outerSubstType,
                         planData, innerResult, outerResult);
    return;
  }

  // If the inner pattern doesn't vanish, expand it.
  if (!innerOrigType.doesTupleVanish()) {
    planExpandedIntoDirect(innerOrigType, cast<TupleType>(innerSubstType),
                           outerOrigType, outerSubstType,
                           planData, outerResult);
    return;
  }

  // Otherwise, the inner tuple vanishes.  Expand it, find the surviving
  // element, and recurse.
  expandVanishingTuple(innerOrigType, innerSubstType, planData, /*inner*/ true,
     [&](AbstractionPattern innerOrigEltType, CanType innerSubstEltType) {
    planIntoDirect(innerOrigEltType, innerSubstEltType,
                   outerOrigType, outerSubstType, planData,
                   outerResult);
  }, [&](AbstractionPattern innerOrigEltType,
         CanType innerSubstEltType, SILValue innerResultAddr) {
    planSingleFromIndirect(innerOrigEltType, innerSubstEltType,
                           outerOrigType, outerSubstType,
                           innerResultAddr, outerResult, SILValue());
  });
}

/// Plan the emission of a call result as a single outer direct result,
/// given that the inner abstraction pattern is a tuple.
void ResultPlanner::planExpandedIntoDirect(AbstractionPattern innerOrigType,
                                           CanTupleType innerSubstType,
                                           AbstractionPattern outerOrigType,
                                           CanType outerSubstType,
                                           PlanData &planData,
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
          outerSubstObjectType, planData, outerObjectResult);

      // Take that result and inject it into an optional.
      addInjectOptionalDirect(someDecl, outerResult);
      return;
    } else {
      assert(!SGF.silConv.useLoweredAddresses() &&
             "inner type was a tuple but outer type was neither a tuple nor "
             "optional nor are we under opaque value mode");
      assert(outerSubstType->isAny());

      auto opaque = AbstractionPattern::getOpaque();
      auto anyType = SGF.getLoweredType(opaque, outerSubstType);
      auto outerResultAddr = SGF.emitTemporaryAllocation(Loc, anyType);

      SILValue outerConcreteResultAddr = SGF.B.createInitExistentialAddr(
          Loc, outerResultAddr, innerSubstType,
          SGF.getLoweredType(opaque, innerSubstType), /*conformances=*/{});

      planExpandedIntoIndirect(innerOrigType, innerSubstType,
                               innerOrigType, innerSubstType,
                               planData, outerConcreteResultAddr);

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
    planExpandedIntoIndirect(innerOrigType, innerSubstType,
                             outerOrigType, outerSubstType,
                             planData, temporary);
    addIndirectToDirect(temporary, outerResult);
    return;
  }

  // Expand the inner tuple, producing direct outer results for each
  // of the elements.
  InnerPackResultGenerator innerPacks(*this, planData);
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
      auto innerEltAddr = innerElt.createPackComponentTemporary(SGF, Loc);
      planSingleFromIndirect(innerElt.getOrigType(), innerElt.getSubstType(),
                             outerOrigEltType, outerSubstEltType,
                             innerEltAddr, outerEltResult, SILValue());

    // Otherwise, recurse normally.
    } else {
      planIntoDirect(innerElt.getOrigType(), innerElt.getSubstType(),
                     outerOrigEltType, outerSubstEltType,
                     planData, outerEltResult);
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
                                         PlanData &planData,
                                         SILResultInfo innerResult,
                                         SILResultInfo outerResult) {
  assert(!innerOrigType.isTuple());
  assert(!outerOrigType.isTuple());

  // If the inner result is indirect, plan to emit from that.
  if (SGF.silConv.isSILIndirect(innerResult)) {
    SILValue innerResultAddr =
      addInnerIndirectResultTemporary(planData, innerResult);
    planSingleFromIndirect(innerOrigType, innerSubstType,
                           outerOrigType, outerSubstType,
                           innerResultAddr, outerResult, SILValue());
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
                                      PlanData &planData,
                                      SILResultInfo innerResult,
                                      SILValue outerResultAddr) {
  assert(!innerOrigType.isTuple());
  // outerOrigType can be a tuple pattern; we just know it's not expanded
  // in this position.

  // If the inner result is indirect, we need some memory to emit it into.
  if (SGF.silConv.isSILIndirect(innerResult)) {
    // If there's no abstraction difference, that can just be
    // in-place into the outer result address.
    if (!hasAbstractionDifference(outerResultAddr, innerResult)) {
      addInPlace(planData, outerResultAddr);

    // Otherwise, we'll need a temporary.
    } else {
      SILValue innerResultAddr =
        addInnerIndirectResultTemporary(planData, innerResult);
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
void ResultPlanner::planFromIndirect(AbstractionPattern innerOrigType,
                                     CanType innerSubstType,
                                     AbstractionPattern outerOrigType,
                                     CanType outerSubstType,
                                     PlanData &planData,
                                     SILValue innerResultAddr) {
  assert(!innerOrigType.isTuple());

  // If the outer pattern is scalar, claim the next outer result.
  if (!outerOrigType.isTuple()) {
    auto outerResult = claimNextOuterResult(planData);
    planSingleFromIndirect(innerOrigType, innerSubstType,
                           outerOrigType, outerSubstType,
                           innerResultAddr,
                           outerResult.first, outerResult.second);
    return;
  }

  // If the outer pattern is not a vanishing tuple, emit indirect
  // into it.
  if (!outerOrigType.doesTupleVanish()) {
    planExpandedFromIndirect(innerOrigType, cast<TupleType>(innerSubstType),
                             outerOrigType, cast<TupleType>(outerSubstType),
                             planData, innerResultAddr);
    return;
  }

  // Otherwise, the outer pattern is a vanishing tuple.  Expand it,
  // find the surviving element, and recurse.
  expandVanishingTuple(outerOrigType, outerSubstType, planData, /*inner*/ false,
     [&](AbstractionPattern outerOrigEltType, CanType outerSubstEltType) {
    planFromIndirect(innerOrigType, innerSubstType,
                     outerOrigEltType, outerSubstEltType,
                     planData, innerResultAddr);
  }, [&](AbstractionPattern outerOrigEltType, CanType outerSubstEltType,
         SILValue outerResultAddr) {
    planIndirectIntoIndirect(innerOrigType, innerSubstType,
                             outerOrigEltType, outerSubstEltType,
                             innerResultAddr, outerResultAddr);
  });
}

/// Plan the emission of a call result from an inner result address, given
/// that the outer abstraction pattern is a tuple.
void ResultPlanner::planExpandedFromIndirect(AbstractionPattern innerOrigType,
                                             CanTupleType innerSubstType,
                                             AbstractionPattern outerOrigType,
                                             CanTupleType outerSubstType,
                                             PlanData &planData,
                                             SILValue innerResultAddr) {
  assert(!innerOrigType.isTuple());
  assert(innerSubstType->getNumElements() == outerSubstType->getNumElements());
  assert(outerOrigType.isTuple());
  assert(!outerOrigType.doesTupleVanish());

  auto &ctx = SGF.getASTContext();
  OuterPackResultGenerator outerPacks(*this, planData);
  ExpandedTupleInputGenerator
    outerElt(ctx, outerPacks, outerOrigType, outerSubstType);
  TupleElementAddressGenerator
    innerElt(ctx, ManagedValue::forLValue(innerResultAddr),
             innerOrigType, innerSubstType);
  for (; !innerElt.isFinished(); innerElt.advance(), outerElt.advance()) {
    assert(!outerElt.isFinished());

    // Project the address of the element.
    SILValue innerEltResultAddr =
      innerElt.projectElementAddress(SGF, Loc).getLValueAddress();

    // If the outer element does not come from a pack, we just recurse.
    if (!outerElt.isOrigPackExpansion()) {
      planFromIndirect(innerElt.getOrigType(), innerElt.getSubstType(),
                       outerElt.getOrigType(), outerElt.getSubstType(),
                       planData, innerEltResultAddr);
      continue;
    }

    // Otherwise, we're going to have an indirect result for it.
    auto outerEltResultAddr =
      outerElt.projectPackComponent(SGF, Loc).getLValueAddress();

    if (auto outerSubstExpansionType =
          dyn_cast<PackExpansionType>(outerElt.getSubstType())) {
      planPackExpansionFromTuple(innerElt.getOrigType(),
         cast<PackExpansionType>(innerElt.getSubstType()),
                                 outerElt.getOrigType(),
                                 outerSubstExpansionType,
                                 innerElt.getInducedPackType(),
                                 innerEltResultAddr,
                                 innerElt.getSubstElementIndex(),
                                 outerElt.getFormalPackType(),
                                 outerEltResultAddr,
                                 outerElt.getPackComponentIndex());
      continue;
    }

    planIndirectIntoIndirect(innerElt.getOrigType(), innerElt.getSubstType(),
                             outerElt.getOrigType(), outerElt.getSubstType(),
                             innerEltResultAddr, outerEltResultAddr);
  }
  innerElt.finish();
  outerElt.finish();
}

void ResultPlanner::planExpandedFromDirect(AbstractionPattern innerOrigType,
                                           CanTupleType innerSubstType,
                                           AbstractionPattern outerOrigType,
                                           CanTupleType outerSubstType,
                                           PlanData &planData,
                                           SILResultInfo innerResult) {

  assert(!innerOrigType.isTuple());
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
    auto temporary = SGF.emitTemporaryAllocation(Loc, innerResultTy);
    addDirectToIndirect(innerResult, temporary);
    planExpandedFromIndirect(innerOrigType, innerSubstType,
                             outerOrigType, outerSubstType,
                             planData, temporary);
    return;
  }

  // Split the inner tuple value into its elements.
  addDestructureDirectInnerTuple(innerResult);

  // Expand the outer tuple and recurse.
  OuterPackResultGenerator outerPacks(*this, planData);
  ExpandedTupleInputGenerator
    outerElt(SGF.getASTContext(), outerPacks, outerOrigType, outerSubstType);
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
                             planData, innerEltResult, outerEltAddr);

    // Otherwise, we need to recurse.
    } else {
      planFromDirect(innerOrigEltType, innerSubstEltType,
                     outerElt.getOrigType(), outerElt.getSubstType(),
                     planData, innerEltResult);
    }
    outerElt.advance();
  });
  outerElt.finish();
}

void ResultPlanner::planFromDirect(AbstractionPattern innerOrigType,
                                   CanType innerSubstType,
                                   AbstractionPattern outerOrigType,
                                   CanType outerSubstType,
                                   PlanData &planData,
                                   SILResultInfo innerResult) {
  // If the outer type isn't a tuple, it's a single result.
  if (!outerOrigType.isTuple()) {
    auto outerResult = claimNextOuterResult(planData);
    planSingle(innerOrigType, innerSubstType,
               outerOrigType, outerSubstType,
               planData, innerResult, outerResult.first, outerResult.second);
    return;
  }

  // If the outer tuple doesn't vanish, then the inner substituted type
  // must have parallel tuple structure.
  if (!outerOrigType.doesTupleVanish()) {
    planExpandedFromDirect(innerOrigType, cast<TupleType>(innerSubstType),
                           outerOrigType, cast<TupleType>(outerSubstType),
                           planData, innerResult);
    return;
  }

  // Otherwise, expand the outer tuple and recurse for the surviving
  // element.
  expandVanishingTuple(outerOrigType, outerSubstType, planData, /*inner*/ false,
     [&](AbstractionPattern outerOrigEltType, CanType outerSubstEltType) {
    planFromDirect(innerOrigType, innerSubstType,
                   outerOrigEltType, outerSubstEltType,
                   planData, innerResult);
  }, [&](AbstractionPattern outerOrigEltType, CanType outerSubstEltType,
         SILValue outerResultAddr) {
    planSingleIntoIndirect(innerOrigType, innerSubstType,
                           outerOrigEltType, outerSubstEltType,
                           planData, innerResult, outerResultAddr);
  });
}

/// Plan the emission of a call result from an inner result address,
/// given that the outer abstraction pattern is not a tuple.
void ResultPlanner::planSingleFromIndirect(AbstractionPattern innerOrigType,
                                           CanType innerSubstType,
                                           AbstractionPattern outerOrigType,
                                           CanType outerSubstType,
                                           SILValue innerResultAddr,
                                           SILResultInfo outerResult,
                                           SILValue optOuterResultAddr) {
  assert(!innerOrigType.isTuple());
  assert(!outerOrigType.isTuple());
  assert(SGF.silConv.isSILIndirect(outerResult) == bool(optOuterResultAddr));

  // The outer result can be indirect, and it doesn't necessarily have an
  // abstraction difference.  Note that we should only end up in this path
  // in cases where simply forwarding the outer result address wasn't possible.

  if (SGF.silConv.isSILIndirect(outerResult)) {
    assert(optOuterResultAddr);
    planIndirectIntoIndirect(innerOrigType, innerSubstType,
                             outerOrigType, outerSubstType,
                             innerResultAddr, optOuterResultAddr);
  } else {
    if (!hasAbstractionDifference(innerResultAddr, outerResult)) {
      addIndirectToDirect(innerResultAddr, outerResult);
    } else {
      addReabstractIndirectToDirect(innerOrigType, innerSubstType,
                                    outerOrigType, outerSubstType,
                                    innerResultAddr, outerResult);
    }
  }
}

void ResultPlanner::planIndirectIntoIndirect(AbstractionPattern innerOrigType,
                                             CanType innerSubstType,
                                             AbstractionPattern outerOrigType,
                                             CanType outerSubstType,
                                             SILValue innerResultAddr,
                                             SILValue outerResultAddr) {
  if (!hasAbstractionDifference(innerResultAddr, outerResultAddr)) {
    addIndirectToIndirect(innerResultAddr, outerResultAddr);
  } else {
    addReabstractIndirectToIndirect(innerOrigType, innerSubstType,
                                    outerOrigType, outerSubstType,
                                    innerResultAddr, outerResultAddr);
  }
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
      return SGF.emitManagedRetain(Loc, resultValue, resultTL);
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
    llvm::Optional<TemporaryInitialization> outerResultInit;
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
      auto elts = makeArrayRef(outerDirectResults).slice(firstEltIndex);
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
                           CanType dynamicSelfType) {
  PrettyStackTraceSILFunction stackTrace("emitting reabstraction thunk in",
                                         &SGF.F);
  auto thunkType = SGF.F.getLoweredFunctionType();

  FullExpr scope(SGF.Cleanups, CleanupLocation(loc));

  SmallVector<ManagedValue, 8> params;
  SGF.collectThunkParams(loc, params);

  // Ignore the self parameter at the SIL level. IRGen will use it to
  // recover type metadata.
  if (dynamicSelfType)
    params.pop_back();

  ManagedValue fnValue = params.pop_back_val();
  auto fnType = fnValue.getType().castTo<SILFunctionType>();
  assert(!fnType->isPolymorphic());
  auto argTypes = fnType->getParameters();

  // If the input is synchronous and global-actor-qualified, and the
  // output is asynchronous, hop to the executor expected by the input.
  // Treat this thunk as if it were isolated to that global actor.
  if (outputSubstType->isAsync() && !inputSubstType->isAsync()) {
    if (Type globalActor = inputSubstType->getGlobalActor()) {
      SGF.emitPrologGlobalActorHop(loc, globalActor);
    }
  }

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
    .translate(outputOrigType,
               outputSubstType.getParams(),
               inputOrigType,
               inputSubstType.getParams());

  SmallVector<SILValue, 8> argValues;

  // Plan the results.  This builds argument values for all the
  // inner indirect results.
  ResultPlanner resultPlanner(SGF, loc);
  resultPlanner.plan(inputOrigType.getFunctionResultType(),
                     inputSubstType.getResult(),
                     outputOrigType.getFunctionResultType(),
                     outputSubstType.getResult(),
                     fnType, thunkType, argValues);

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
                                              ManagedValue fn) {
  auto thunkValue = SGF.B.createFunctionRefFor(loc, thunk);
  SmallVector<ManagedValue, 2> thunkArgs;
  thunkArgs.push_back(fn);
  if (dynamicSelfType) {
    SILType dynamicSILType = SGF.getLoweredType(dynamicSelfType);
    SILValue value = SGF.B.createMetatype(loc, dynamicSILType);
    thunkArgs.push_back(ManagedValue::forUnmanaged(value));
  }

  return
    SGF.B.createPartialApply(loc, thunkValue,
                             interfaceSubs, thunkArgs,
                             toType->getCalleeConvention());
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
                   dynamicSelfType);
    SGF.SGM.emitLazyConformancesForFunction(thunk);
  }

  auto thunkedFn =
    createPartialApplyOfThunk(SGF, loc, thunk, interfaceSubs, dynamicSelfType,
                              toType, fn.ensurePlusOne(SGF, loc));

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
  SmallVector<SILArgument*, 8> indirectResults;
  SGF.collectThunkParams(loc, params, &indirectResults);

  // Ignore the self parameter at the SIL level. IRGen will use it to
  // recover type metadata.
  if (dynamicSelfType)
    params.pop_back();

  ManagedValue fnValue = params.pop_back_val();
  auto fnType = fnValue.getType().castTo<SILFunctionType>();

  SmallVector<SILValue, 8> argValues;
  if (!indirectResults.empty()) {
    for (auto *result : indirectResults)
      argValues.push_back(SILValue(result));
  }

  // Forward indirect result arguments.

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

  auto thunkedFn =
    createPartialApplyOfThunk(*this, loc, thunk, interfaceSubs, dynamicSelfType,
                              escapingFnTy, noEscapeValue);

  // Convert to the substituted escaping type.
  if (escapingFnTy != escapingFnSubstTy) {
    thunkedFn = B.createConvertFunction(loc, thunkedFn,
                            SILType::getPrimitiveObjectType(escapingFnSubstTy));
  }
  
  // We need to ensure the 'lifetime' of the trivial values context captures. As
  // long as we represent these captures by the same value the following works.
  thunkedFn = emitManagedRValueWithCleanup(
    B.createMarkDependence(loc, thunkedFn.forward(*this),
                           noEscapingFunctionValue.getValue()));

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
        *this, loc, thunk, interfaceSubs, dynamicSelfType, toType, linearMap);
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
  SmallVector<SILArgument *, 4> thunkIndirectResults;
  thunkSGF.collectThunkParams(loc, params, &thunkIndirectResults);

  SILFunctionConventions fromConv(fromType, getModule());
  SILFunctionConventions toConv(toType, getModule());
  assert(toConv.useLoweredAddresses());

  SmallVector<ManagedValue, 4> thunkArguments;
  for (auto *indRes : thunkIndirectResults)
    thunkArguments.push_back(ManagedValue::forLValue(indRes));
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
    thunkSGF.emitSemanticStore(loc, *fromDirResultsIter++, indRes,
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
      customDerivativeFn->isSerialized(),
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
  SmallVector<SILArgument *, 4> indirectResults;
  thunkSGF.collectThunkParams(loc, params, &indirectResults);

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
    params.push_back(ManagedValue::forUnmanaged(metatype));
  }

  // Collect thunk arguments, converting ownership.
  SmallVector<SILValue, 8> arguments;
  for (auto *indRes : indirectResults)
    arguments.push_back(indRes);
  forwardFunctionArguments(thunkSGF, loc, fnRefType, params, arguments);

  // Apply function argument.
  auto apply = thunkSGF.emitApplyWithRethrow(
      loc, fnRef, /*substFnType*/ fnRef->getType(),
      thunk->getForwardingSubstitutionMap(), arguments);

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
    createReturn(apply);
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
  collectThunkParams(loc, thunkArgs);

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
    .translate(inputOrigType,
               inputSubstType.getParams(),
               outputOrigType,
               outputSubstType.getParams());
  
  auto coroutineKind = F.getLoweredFunctionType()->getCoroutineKind();

  // Collect the arguments to the implementation.
  SmallVector<SILValue, 8> args;

  llvm::Optional<ResultPlanner> resultPlanner;

  if (coroutineKind == SILCoroutineKind::None) {
    // First, indirect results.
    resultPlanner.emplace(*this, loc);
    resultPlanner->plan(outputOrigType.getFunctionResultType(),
                        outputSubstType.getResult(),
                        inputOrigType.getFunctionResultType(),
                        inputSubstType.getResult(),
                        derivedFTy, thunkTy, args);
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
  auto selfParam = protocol->getProtocolSelfType()->getCanonicalType();
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
      auto *loweredResultIndices = IndexSubset::get(
          SGF.getASTContext(), 1, {0}); // FIXME, set to all results
      auto diffFn = SGF.B.createDifferentiableFunction(
          loc, loweredParamIndices, loweredResultIndices, originalFn);
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
    bool isSelfConformance, llvm::Optional<ActorIsolation> enterIsolation) {
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
  collectThunkParams(loc, origParams);

  if (shouldLowerToUnavailableCodeStub(witness.getDecl()))
    emitApplyOfUnavailableCodeReached();

  if (enterIsolation) {
    // If we are supposed to enter the actor, do so now by hopping to the
    // actor.
    llvm::Optional<ManagedValue> actorSelf;

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

    emitHopToTargetActor(loc, enterIsolation, actorSelf);
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
    .translate(reqtOrigTy,
               reqtSubstParams,
               witnessOrigTy,
               witnessSubstParams,
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

  llvm::Optional<ResultPlanner> resultPlanner;
  if (coroutineKind == SILCoroutineKind::None) {
    //   - indirect results
    resultPlanner.emplace(*this, loc);
    resultPlanner->plan(witnessOrigTy.getFunctionResultType(),
                        witnessSubstTy.getResult(),
                        reqtOrigTy.getFunctionResultType(),
                        reqtSubstTy.getResult(),
                        witnessFTy, thunkTy, args);
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
