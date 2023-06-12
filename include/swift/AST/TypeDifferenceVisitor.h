//===--- TypeDifferenceVisitor.h - Visitor for pairs of types ---*- C++ -*-===//
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
// This file defines TypeDifferenceVisitor, a visitor which finds
// differences between canonical types.
//
//===----------------------------------------------------------------------===//

#ifndef SWIFT_AST_TYPEDIFFERENCEVISITOR_H
#define SWIFT_AST_TYPEDIFFERENCEVISITOR_H

#include "swift/AST/SILLayout.h"
#include "swift/AST/Types.h"

namespace swift {

// TODO: maybe have a version of this that works on non-canonical types

template <class Impl, class RetTy, class... Args>
class CanTypePairVisitor {
public:
  // Provide default implementations that chain to the base class.
#define ABSTRACT_TYPE(CLASS, PARENT)                                    \
  RetTy visit##CLASS##Type(Can##CLASS##Type type1,                      \
                           Can##CLASS##Type type2,                      \
                           Args... args) {                              \
    return static_cast<Impl&>(*this)                                    \
             .visit##PARENT(type1, type2, std::forward<Args>(args)...); \
  }
#define TYPE(CLASS, PARENT) ABSTRACT_TYPE(CLASS, PARENT)
#define ABSTRACT_SUGARED_TYPE(CLASS, PARENT)
#define SUGARED_TYPE(CLASS, PARENT)
  // Don't allow unchecked types by default, but allow visitors to opt-in to
  // handling them.
#define UNCHECKED_TYPE(CLASS, PARENT)                          \
  RetTy visit##CLASS##Type(Can##CLASS##Type type1,             \
                           Can##CLASS##Type type2,             \
                           Args... args) {                     \
     llvm_unreachable("unchecked type");                       \
  }
#include "swift/AST/TypeNodes.def"
};

/// A CRTP class for finding differences between types.
///
/// The visitors all short-circuit as soon as one returns true.
///
/// visitDifferentTypes()
template <class Impl>
class CanTypeDifferenceVisitor : public CanTypePairVisitor<Impl, bool> {
protected:
  Impl &asImpl() { return static_cast<Impl&>(*this); }
public:
  /// Two component types differ.
  bool visitDifferentComponentTypes(CanType type1, CanType type2) {
    asImpl().visitDifferentTypes(type1, type2);

    // Short-circuit by default.
    return true;
  }

  /// Two types differ in non-type structure, like a convention or a label.
  /// Generally, you can't usefully recover when this is called; it always
  /// needs to return true.
  bool visitDifferentTypeStructure(CanType type1, CanType type2) {
    asImpl().visitDifferentTypes(type1, type2);
    return true;
  }

  /// Inform the subclass that a difference was detected.
  void visitDifferentTypes(CanType type1, CanType type2) {}

  bool visit(CanType type1, CanType type2) {
    if (type1 == type2)
      return false;

    if (type1->getKind() != type2->getKind())
      return asImpl().visitDifferentComponentTypes(type1, type2);

    switch (type1->getKind()) {
#define SUGARED_TYPE(CLASS, PARENT) \
    case TypeKind::CLASS:
#define TYPE(CLASS, PARENT)
#include "swift/AST/TypeNodes.def"
      llvm_unreachable("non-canonical type");

#define SUGARED_TYPE(CLASS, PARENT)
#define TYPE(CLASS, PARENT)                                        \
    case TypeKind::CLASS:                                          \
      return asImpl().visit##CLASS##Type(cast<CLASS##Type>(type1), \
                                         cast<CLASS##Type>(type2));
#include "swift/AST/TypeNodes.def"
    }
    llvm_unreachable("Not reachable, all cases handled");
  }

  // In the type-specific visitors, we know that we have
  // non-identical types.

  // These types are singleton and can't actually differ.
#define SINGLETON_TYPE(SHORT_ID, ID)                               \
  bool visit##ID##Type(Can##ID##Type type1, Can##ID##Type type2) {\
    llvm_unreachable("singleton type that wasn't identical"); \
  }
#include "swift/AST/TypeNodes.def"

  bool visitBuiltinIntegerType(CanBuiltinIntegerType type1,
                               CanBuiltinIntegerType type2) {
    return asImpl().visitDifferentTypeStructure(type1, type2);
  }

  bool visitBuiltinFloatType(CanBuiltinFloatType type1,
                             CanBuiltinFloatType type2) {
    return asImpl().visitDifferentTypeStructure(type1, type2);
  }

  bool visitBuiltinVectorType(CanBuiltinVectorType type1,
                              CanBuiltinVectorType type2) {
    if (type1->getNumElements() != type2->getNumElements())
      return asImpl().visitDifferentTypeStructure(type1, type2);
    return asImpl().visit(type1.getElementType(), type2.getElementType());
  }

  bool visitPackType(CanPackType type1, CanPackType type2) {
    return visitComponentArray(type1, type2,
                               type1->getElementTypes(),
                               type2->getElementTypes());
  }

  bool visitSILPackType(CanSILPackType type1, CanSILPackType type2) {
    if (type1->isElementAddress() != type2->isElementAddress())
      return asImpl().visitDifferentTypeStructure(type1, type2);
    return visitComponentArray(type1, type2,
                               type1->getElementTypes(),
                               type2->getElementTypes());
  }

  bool visitPackExpansionType(CanPackExpansionType type1,
                              CanPackExpansionType type2) {
    return asImpl().visit(type1.getPatternType(), type2.getPatternType());
  }

  bool visitPackElementType(CanPackElementType type1,
                            CanPackElementType type2) {
    return asImpl().visit(type1.getPackType(), type2.getPackType());
  }

  bool visitTupleType(CanTupleType type1, CanTupleType type2) {
    return visitComponentArray(type1, type2,
                               type1->getElements(), type2->getElements());
  }

  bool visitComponent(CanType type1, CanType type2,
                      const TupleTypeElt &elt1, const TupleTypeElt &elt2) {
    if (elt1.getName() != elt2.getName())
      return asImpl().visitDifferentTypeStructure(type1, type2);
    return asImpl().visit(CanType(elt1.getType()), CanType(elt2.getType()));
  }

  bool visitReferenceStorageType(CanReferenceStorageType type1,
                                 CanReferenceStorageType type2) {
    return asImpl().visit(type1.getReferentType(), type2.getReferentType());
  }

  bool visitUnboundGenericType(CanUnboundGenericType type1,
                               CanUnboundGenericType type2) {
    assert(type1->getDecl() != type2->getDecl());
    return asImpl().visitDifferentTypeStructure(type1, type2);
  }

  bool visitNominalType(CanNominalType type1, CanNominalType type2) {
    assert(type1->getDecl() != type2->getDecl());
    return asImpl().visitDifferentTypeStructure(type1, type2);
  }

  bool visitBoundGenericType(CanBoundGenericType type1,
                             CanBoundGenericType type2) {
    if (type1->getDecl() != type2->getDecl())
      return asImpl().visitDifferentTypeStructure(type1, type2);

    return visitComponentArray(type1, type2,
                               type1.getGenericArgs(), type2.getGenericArgs());
  }

  bool visitAnyMetatypeType(CanAnyMetatypeType type1,
                            CanAnyMetatypeType type2) {
    if (type1->hasRepresentation() != type2->hasRepresentation() ||
        (type1->hasRepresentation() && 
         type1->getRepresentation() != type2->getRepresentation()))
      return asImpl().visitDifferentTypeStructure(type1, type2);

    return asImpl().visit(type1.getInstanceType(), type2.getInstanceType());
  }

  bool visitModuleType(CanModuleType type1, CanModuleType type2) {
    return asImpl().visitDifferentTypeStructure(type1, type2);
  }

  bool visitDynamicSelfType(CanDynamicSelfType type1,
                            CanDynamicSelfType type2) {
    return asImpl().visit(type1.getSelfType(), type2.getSelfType());
  }

  bool visitSubstitutableType(CanSubstitutableType type1,
                              CanSubstitutableType type2) {
    return asImpl().visitDifferentComponentTypes(type1, type2);
  }

  bool visitDependentMemberType(CanDependentMemberType type1,
                                CanDependentMemberType type2) {
    if (type1->getName() != type2->getName())
      return asImpl().visitDifferentTypeStructure(type1, type2);
    return asImpl().visit(type1.getBase(), type2.getBase());
  }

  bool visitGenericFunctionType(CanGenericFunctionType type1,
                                CanGenericFunctionType type2) {
    if (type1.getGenericSignature() != type2.getGenericSignature())
      return asImpl().visitDifferentTypeStructure(type1, type2);

    return asImpl().visitAnyFunctionType(type1, type2);
  }

  bool visitAnyFunctionType(CanAnyFunctionType type1,
                            CanAnyFunctionType type2) {
    if (!type1->hasSameExtInfoAs(type2))
      return asImpl().visitDifferentTypeStructure(type1, type2);

    if (asImpl().visit(type1.getResult(), type2.getResult()))
      return true;

    return visitComponentArray(type1, type2,
                               type1.getParams(), type2.getParams());
  }

  bool visitComponent(CanType type1, CanType type2,
                      AnyFunctionType::CanParam param1,
                      AnyFunctionType::CanParam param2) {
    if (param1.getLabel() != param2.getLabel() ||
        param1.getParameterFlags() != param2.getParameterFlags())
      return asImpl().visitDifferentTypeStructure(type1, type2);
    return asImpl().visit(param1.getPlainType(), param2.getPlainType());
  }

  bool visitSILFunctionType(CanSILFunctionType type1,
                            CanSILFunctionType type2) {
    return (asImpl().visitSILFunctionTypeStructure(type1, type2) ||
            asImpl().visitSILFunctionTypeSubstitutions(type1, type2) ||
            asImpl().visitSILFunctionTypeComponents(type1, type2));
  }

  bool visitSILFunctionTypeStructure(CanSILFunctionType type1,
                                     CanSILFunctionType type2) {
    if (!type1->hasSameExtInfoAs(type2) ||
        type1->getCoroutineKind() != type2->getCoroutineKind() ||
        type1->getInvocationGenericSignature() !=
            type2->getInvocationGenericSignature())
      return asImpl().visitDifferentTypeStructure(type1, type2);
    return false;
  }

  bool visitSILFunctionTypeSubstitutions(CanSILFunctionType type1,
                                         CanSILFunctionType type2) {
    return asImpl().visitOptSubstitutionMap(type1, type2,
                                            type1->getPatternSubstitutions(),
                                            type2->getPatternSubstitutions())
        || asImpl().visitOptSubstitutionMap(type1, type2,
                                            type1->getInvocationSubstitutions(),
                                            type2->getInvocationSubstitutions());
  }

  bool visitSILFunctionTypeComponents(CanSILFunctionType type1,
                                      CanSILFunctionType type2) {
    return visitComponentArray(type1, type2,
                               type1->getParameters(), type2->getParameters())
        || visitComponentArray(type1, type2,
                               type1->getResults(), type2->getResults())
        || visitComponentArray(type1, type2,
                               type1->getYields(), type2->getYields());
  }

  bool visitComponent(CanType type1, CanType type2,
                      SILParameterInfo param1, SILParameterInfo param2) {
    if (param1.getConvention() != param2.getConvention() ||
        param1.getDifferentiability() != param2.getDifferentiability())
      return asImpl().visitDifferentTypeStructure(type1, type2);

    return asImpl().visit(param1.getInterfaceType(),
                          param2.getInterfaceType());
  }

  bool visitComponent(CanType type1, CanType type2,
                      SILResultInfo result1, SILResultInfo result2) {
    if (result1.getConvention() != result2.getConvention())
      return asImpl().visitDifferentTypeStructure(type1, type2);

    return asImpl().visit(result1.getInterfaceType(),
                          result2.getInterfaceType());
  }

  bool visitComponent(CanType type1, CanType type2,
                      SILYieldInfo yield1, SILYieldInfo yield2) {
    if (yield1.getConvention() != yield2.getConvention())
      return asImpl().visitDifferentTypeStructure(type1, type2);

    return asImpl().visit(yield1.getInterfaceType(),
                          yield2.getInterfaceType());
  }

  bool visitSILBoxType(CanSILBoxType type1, CanSILBoxType type2) {
    return (asImpl().visitSILLayout(type1, type2,
                                    type1->getLayout(), type2->getLayout()) ||
            asImpl().visitOptSubstitutionMap(type1, type2,
                                             type1->getSubstitutions(),
                                             type2->getSubstitutions()));
  }

  bool visitSILLayout(CanType type1, CanType type2,
                      SILLayout *layout1, SILLayout *layout2) {
    if (layout1->getGenericSignature() != layout2->getGenericSignature() ||
        layout1->isMutable() != layout2->isMutable())
      return asImpl().visitDifferentTypeStructure(type1, type2);

    return visitComponentArray(type1, type2,
                               layout1->getFields(), layout2->getFields());
  }

  bool visitComponent(CanType type1, CanType type2,
                      const SILField &field1, const SILField &field2) {
    if (field1.isMutable() != field2.isMutable())
      return asImpl().visitDifferentTypeStructure(type1, type2);
    return asImpl().visit(field1.getLoweredType(), field2.getLoweredType());
  }

  bool visitSILBlockStorageType(CanSILBlockStorageType type1,
                                CanSILBlockStorageType type2) {
    return asImpl().visit(type1->getCaptureType(), type2->getCaptureType());
  }

  bool visitSILMoveOnlyWrappedType(CanSILMoveOnlyWrappedType type1,
                                   CanSILMoveOnlyWrappedType type2) {
    return asImpl().visit(type1->getInnerType(), type2->getInnerType());
  }

  bool visitProtocolCompositionType(CanProtocolCompositionType type1,
                                    CanProtocolCompositionType type2) {
    return visitComponentArray(type1, type2,
                               type1->getMembers(), type2->getMembers());
  }

  bool visitParameterizedProtocolType(CanParameterizedProtocolType type1,
                                      CanParameterizedProtocolType type2) {
    if (asImpl().visit(type1.getBaseType(), type2.getBaseType()))
      return true;

    return visitComponentArray(type1, type2,
                               type1->getArgs(), type2->getArgs());
  }

  bool visitExistentialType(CanExistentialType type1,
                            CanExistentialType type2) {
    return asImpl().visit(type1.getConstraintType(),
                          type2.getConstraintType());
  }

  bool visitLValueType(CanLValueType type1, CanLValueType type2) {
    return asImpl().visit(type1.getObjectType(), type2.getObjectType());
  }

  bool visitInOutType(CanInOutType type1, CanInOutType type2) {
    return asImpl().visit(type1.getObjectType(), type2.getObjectType());
  }

  bool visitErrorType(CanErrorType type1, CanErrorType type2) {
    return false;
  }

  bool visitOptSubstitutionMap(CanType type1, CanType type2,
                               SubstitutionMap subs1, SubstitutionMap subs2) {
    if ((bool) subs1 != (bool) subs2)
      return asImpl().visitDifferentTypeStructure(type1, type2);
    if (subs1)
      return asImpl().visitSubstitutionMap(type1, type2, subs1, subs2);
    return false;
  }

  bool visitSubstitutionMap(CanType type1, CanType type2,
                            SubstitutionMap subs1, SubstitutionMap subs2) {
    if (CanGenericSignature(subs1.getGenericSignature())
          != CanGenericSignature(subs2.getGenericSignature()))
      return asImpl().visitDifferentTypeStructure(type1, type2);

    return visitComponentArray(type1, type2,
                               subs1.getReplacementTypes(),
                               subs2.getReplacementTypes());
  }

private:
  bool visitComponent(CanType type1, CanType type2,
                      Type componentType1, Type componentType2) {
    return asImpl().visit(CanType(componentType1), CanType(componentType2));
  }

  template <class T>
  bool visitComponentArray(CanType type1, CanType type2, T array1, T array2) {
    if (array1.size() != array2.size())
      return asImpl().visitDifferentTypeStructure(type1, type2);

    for (auto i : indices(array1)) {
      if (asImpl().visitComponent(type1, type2, array1[i], array2[i]))
        return true;
    }

    return false;
  }
};

}

#endif
