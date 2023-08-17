//===--- TypeMatcher.h - Recursive Type Matcher -----------------*- C++ -*-===//
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
// This file defines TypeMatcher, which performs a structural match between
// two types, calling out differences for subclasses to process specifically.
//
//===----------------------------------------------------------------------===//

#ifndef SWIFT_AST_TYPE_MATCHER_H
#define SWIFT_AST_TYPE_MATCHER_H

#include "swift/AST/CanTypeVisitor.h"

namespace swift {

/// Recursively match the structure of two types, calling out those places
/// where the types are not structurally identical and allowing the
/// implementing subclass to take specific actions in response to such
/// mismatches.
///
/// Subclasses must implement the 'mismatch' method, which will be called when
/// a mismatch between the two given types is detected. The most general form
/// of 'mismatch', which all subclasses will likely implement as a catch-all,
/// takes two \c TypeBase pointers:
///
/// \code
/// bool mismatch(TypeBase *firstType, TypeBase *secondType,
///               Type sugaredFirstType);
/// /\endcode
///
/// \c mismatch will be called with the most specific types. Thus, a client that
/// wants to (say) bind type variables in the first type to types in the second
/// type could isolate that case with a 'mismatch' overload:
///
/// \code
/// bool mismatch(TypeVariableType *firstTypeVar, TypeBase *secondType,
///               Type sugaredFirstType);
/// \endcode
///
/// While having other mismatches fall to the original case. The 'mismatch'
/// function should return true to indicate that matching should continue,
/// or false to indicate that matching should exit early.
template<typename ImplClass>
class TypeMatcher {
public:
  enum class Position : uint8_t {
    Type,
    Shape
  };

private:
  class MatchVisitor : public CanTypeVisitor<MatchVisitor, bool, Type, Type> {
    TypeMatcher &Matcher;

    /// Dispatch to the derived class when we've destructured the second type.
    template<typename FirstType, typename SecondType>
    bool mismatch(FirstType *firstType, SecondType *secondType,
                  Type sugaredFirstType) {
      return Matcher.asDerived().mismatch(firstType, secondType,
                                          sugaredFirstType);
    }

    /// Dispatch to the most-derived class when we have not yet destructured the
    /// second type.
    template<typename FirstType>
    bool mismatch(FirstType *firstType, Type secondType,
                  Type sugaredFirstType) {
      switch (secondType->getKind()) {
#define TYPE(CLASS, PARENT)                                         \
      case TypeKind::CLASS:                                         \
        return mismatch(firstType,                                  \
                        cast<CLASS##Type>(secondType.getPointer()), \
                        sugaredFirstType);
#include "swift/AST/TypeNodes.def"
      }

      llvm_unreachable("Unhandled TypeKind in switch.");
    }

    /// Honeypot to catch cases where we should redispatch the second type, but
    /// we have a stray ".getPointer()" in the dispatch call.
    template<typename FirstType>
    bool mismatch(FirstType *firstType, TypeBase *secondType,
                  Type sugaredFirstType) = delete;

  public:
    MatchVisitor(TypeMatcher &matcher) : Matcher(matcher) { }

#define TRIVIAL_CASE(TheType)                                              \
      bool visit##TheType(Can##TheType firstType, Type secondType,         \
                          Type sugaredFirstType) {                         \
        /* If the types match, continue. */                                \
        if (firstType->isEqual(secondType))                                \
          return true;                                                     \
                                                                           \
        /* Otherwise, let the derived class deal with the mismatch. */     \
        return mismatch(firstType.getPointer(), secondType,                \
                        sugaredFirstType);                                 \
      }

    TRIVIAL_CASE(ErrorType)
    TRIVIAL_CASE(BuiltinIntegerType)
    TRIVIAL_CASE(BuiltinFloatType)
    TRIVIAL_CASE(BuiltinVectorType)
#define SINGLETON_TYPE(SHORT_ID, ID) TRIVIAL_CASE(ID##Type)
#include "swift/AST/TypeNodes.def"

    bool visitUnresolvedType(CanUnresolvedType firstType, Type secondType,
                             Type sugaredFirstType) {
      // Unresolved types never match.
      return mismatch(firstType.getPointer(), secondType, sugaredFirstType);
    }

    bool visitTupleType(CanTupleType firstTuple, Type secondType,
                        Type sugaredFirstType) {
      if (auto secondTuple = secondType->getAs<TupleType>()) {
        auto sugaredFirstTuple = sugaredFirstType->getAs<TupleType>();
        if (firstTuple->getNumElements() != secondTuple->getNumElements())
          return mismatch(firstTuple.getPointer(), secondTuple,
                          sugaredFirstType);

        for (unsigned i = 0, n = firstTuple->getNumElements(); i != n; ++i) {
          const auto &firstElt = firstTuple->getElements()[i];
          const auto &secondElt = secondTuple->getElements()[i];

          if (firstElt.getName() != secondElt.getName()) {
            return mismatch(firstTuple.getPointer(), secondTuple,
                            sugaredFirstType);
          }

          // Recurse on the tuple elements.
          if (!this->visit(firstTuple.getElementType(i), secondElt.getType(),
                           sugaredFirstTuple->getElementType(i)))
            return false;
        }

        return true;
      }

      // Tuple/non-tuple mismatch.
      return mismatch(firstTuple.getPointer(), secondType, sugaredFirstType);
    }

    bool visitSILPackType(CanSILPackType firstPack, Type secondType,
                          Type sugaredFirstType) {
      if (auto secondPack = secondType->getAs<SILPackType>()) {
        if (firstPack->getNumElements() != secondPack->getNumElements())
          return mismatch(firstPack.getPointer(), secondPack,
                          sugaredFirstType);

        for (unsigned i = 0, n = firstPack->getNumElements(); i != n; ++i) {
          // Recurse on the pack elements.  There's no need to preserve
          // sugar for SIL types.
          if (!this->visit(firstPack->getElementType(i),
                           secondPack->getElementType(i),
                           firstPack->getElementType(i)))
            return false;
        }

        return true;
      }

      // Pack/non-pack mismatch.
      return mismatch(firstPack.getPointer(), secondType, sugaredFirstType);
    }

    bool visitPackType(CanPackType firstTuple, Type secondType,
                       Type sugaredFirstType) {
      if (auto secondTuple = secondType->getAs<PackType>()) {
        auto sugaredFirstTuple = sugaredFirstType->getAs<PackType>();
        if (firstTuple->getNumElements() != secondTuple->getNumElements())
          return mismatch(firstTuple.getPointer(), secondTuple,
                          sugaredFirstType);

        for (unsigned i = 0, n = firstTuple->getNumElements(); i != n; ++i) {
          Type secondElt = secondTuple->getElementType(i);

          // Recurse on the pack elements.
          if (!this->visit(firstTuple.getElementType(i), secondElt,
                           sugaredFirstTuple->getElementType(i)))
            return false;
        }

        return true;
      }

      // Pack/non-pack mismatch.
      return mismatch(firstTuple.getPointer(), secondType, sugaredFirstType);
    }

    bool visitPackExpansionType(CanPackExpansionType firstPE, Type secondType,
                                Type sugaredFirstType) {
      if (auto secondExpansion = secondType->getAs<PackExpansionType>()) {
        if (!this->visit(firstPE.getPatternType(),
                         secondExpansion->getPatternType(),
                         sugaredFirstType->castTo<PackExpansionType>()
                           ->getPatternType())) {
          return false;
        }

        Matcher.asDerived().pushPosition(Position::Shape);
        if (!this->visit(firstPE.getCountType(),
                         secondExpansion->getCountType(),
                         sugaredFirstType->castTo<PackExpansionType>()
                           ->getCountType())) {
          return false;
        }
        Matcher.asDerived().popPosition(Position::Shape);

        return true;
      }

      return mismatch(firstPE.getPointer(), secondType, sugaredFirstType);
    }

    bool visitPackElementType(CanPackElementType firstElement, Type secondType,
                              Type sugaredFirstType) {
      if (auto secondElement = secondType->getAs<PackElementType>()) {
        return this->visit(firstElement.getPackType(),
                           secondElement->getPackType(),
                           sugaredFirstType->castTo<PackElementType>()
                             ->getPackType());
      }

      return mismatch(firstElement.getPointer(), secondType, sugaredFirstType);
    }

    bool visitReferenceStorageType(CanReferenceStorageType firstStorage,
                                   Type secondType, Type sugaredFirstType) {
      if (firstStorage->getKind() == secondType->getDesugaredType()->getKind()) {
        auto secondStorage = secondType->castTo<ReferenceStorageType>();
        return this->visit(firstStorage.getReferentType(),
                           secondStorage->getReferentType(),
                           sugaredFirstType->castTo<ReferenceStorageType>()
                             ->getReferentType());
      }

      return mismatch(firstStorage.getPointer(), secondType, sugaredFirstType);
    }

    bool visitNominalType(CanNominalType firstNominal,
                          Type secondType, Type sugaredFirstType) {
      if (firstNominal->getKind() == secondType->getDesugaredType()->getKind()) {
        auto secondNominal = secondType->castTo<NominalType>();
        if (firstNominal->getDecl() != secondNominal->getDecl())
          return mismatch(firstNominal.getPointer(), secondNominal,
                          sugaredFirstType);

        if (firstNominal.getParent())
          return this->visit(firstNominal.getParent(),
                             secondNominal->getParent(),
                             sugaredFirstType->castTo<NominalType>()
                               ->getParent());

        return true;
      }

      return mismatch(firstNominal.getPointer(), secondType, sugaredFirstType);
    }

    bool visitAnyMetatypeType(CanAnyMetatypeType firstMeta,
                              Type secondType, Type sugaredFirstType) {
      if (firstMeta->getKind() == secondType->getDesugaredType()->getKind()) {
        auto secondMeta = secondType->castTo<AnyMetatypeType>();
        return this->visit(firstMeta.getInstanceType(),
                           secondMeta->getInstanceType(),
                           sugaredFirstType->castTo<AnyMetatypeType>()
                             ->getInstanceType());
      }

      return mismatch(firstMeta.getPointer(), secondType, sugaredFirstType);
    }

    TRIVIAL_CASE(ModuleType)

    bool visitArchetypeType(CanArchetypeType firstArchetype,
                            Type secondType,
                            Type sugaredFirstType) {
      if (auto firstOpaqueArchetype = dyn_cast<OpaqueTypeArchetypeType>(firstArchetype)) {
        if (auto secondOpaqueArchetype = secondType->getAs<OpaqueTypeArchetypeType>()) {
          if (firstOpaqueArchetype->getDecl() == secondOpaqueArchetype->getDecl()) {
            auto firstSubMap = firstOpaqueArchetype->getSubstitutions();
            auto secondSubMap = secondOpaqueArchetype->getSubstitutions();
            assert(firstSubMap.getReplacementTypes().size() ==
                   secondSubMap.getReplacementTypes().size());

            for (unsigned i : indices(firstSubMap.getReplacementTypes())) {
              auto firstSubstType = firstSubMap.getReplacementTypes()[i];
              auto secondSubstType = secondSubMap.getReplacementTypes()[i];

              if (!this->visit(firstSubstType->getCanonicalType(),
                               secondSubstType, firstSubstType))
                return false;
            }

            return true;
          }
        }
      }

      // FIXME: Once OpenedArchetypeType stores substitutions, do something
      // similar to the above.

      if (firstArchetype->isEqual(secondType))
        return true;


      return mismatch(firstArchetype.getPointer(), secondType, sugaredFirstType);
    }

    bool visitDynamicSelfType(CanDynamicSelfType firstDynamicSelf,
                              Type secondType,
                              Type sugaredFirstType) {
      if (auto secondDynamicSelf = secondType->getAs<DynamicSelfType>()) {
        auto firstBase = firstDynamicSelf->getSelfType();
        auto secondBase = secondDynamicSelf->getSelfType();
        auto firstSugaredBase = sugaredFirstType->getAs<DynamicSelfType>()
            ->getSelfType();

        if (!this->visit(CanType(firstBase), secondBase, firstSugaredBase))
          return false;

        return true;
      }

      return mismatch(firstDynamicSelf.getPointer(), secondType,
                      sugaredFirstType);
    }

    bool visitDependentMemberType(CanDependentMemberType firstType,
                                   Type secondType,
                                   Type sugaredFirstType) {
      /* If the types match, continue. */
      if (!Matcher.asDerived().alwaysMismatchTypeParameters() &&
          firstType->isEqual(secondType))
        return true;

      /* Otherwise, let the derived class deal with the mismatch. */
      return mismatch(firstType.getPointer(), secondType,
                      sugaredFirstType);
    }

    bool visitGenericTypeParamType(CanGenericTypeParamType firstType,
                                   Type secondType,
                                   Type sugaredFirstType) {
      /* If the types match, continue. */
      if (!Matcher.asDerived().alwaysMismatchTypeParameters() &&
          firstType->isEqual(secondType))
        return true;

      /* Otherwise, let the derived class deal with the mismatch. */
      return mismatch(firstType.getPointer(), secondType,
                      sugaredFirstType);
    }

    /// FIXME: Split this out into cases?
    bool visitAnyFunctionType(CanAnyFunctionType firstFunc, Type secondType,
                              Type sugaredFirstType) {
      if (auto secondFunc = secondType->getAs<AnyFunctionType>()) {
        // FIXME: Compare throws()? Both existing subclasses would prefer
        // to mismatch on (!firstFunc->throws() && secondFunc->throws()), but
        // embedding that non-commutativity in this general matcher is icky.
        if (firstFunc->isNoEscape() != secondFunc->isNoEscape())
          return mismatch(firstFunc.getPointer(), secondFunc, sugaredFirstType);

        if (firstFunc->isSendable() != secondFunc->isSendable())
          return mismatch(firstFunc.getPointer(), secondFunc, sugaredFirstType);

        auto sugaredFirstFunc = sugaredFirstType->castTo<AnyFunctionType>();
        if (firstFunc->getParams().size() != secondFunc->getParams().size())
          return mismatch(firstFunc.getPointer(), secondFunc, sugaredFirstFunc);

        for (unsigned i = 0, n = firstFunc->getParams().size(); i != n; ++i) {
          const auto &firstElt = firstFunc->getParams()[i];
          const auto &secondElt = secondFunc->getParams()[i];

          if (firstElt.getLabel() != secondElt.getLabel() ||
              firstElt.isVariadic() != secondElt.isVariadic() ||
              firstElt.isInOut() != secondElt.isInOut())
            return mismatch(firstElt.getOldType().getPointer(),
                            secondElt.getOldType(),
                            sugaredFirstFunc->getParams()[i].getOldType());

          // Recurse on parameter components.
          if (!this->visit(firstElt.getOldType()->getCanonicalType(),
                           secondElt.getOldType(),
                           sugaredFirstFunc->getParams()[i].getOldType()))
            return false;
        }

        return this->visit(firstFunc.getResult(), secondFunc->getResult(),
                           sugaredFirstFunc->getResult());
      }

      return mismatch(firstFunc.getPointer(), secondType, sugaredFirstType);
    }

    TRIVIAL_CASE(SILFunctionType)
    TRIVIAL_CASE(SILBlockStorageType)
    TRIVIAL_CASE(SILBoxType)
    TRIVIAL_CASE(SILMoveOnlyWrappedType)

    bool visitProtocolCompositionType(CanProtocolCompositionType firstProtocolComposition,
                                      Type secondType,
                                      Type sugaredFirstType) {
      if (auto secondProtocolComposition = secondType->getAs<ProtocolCompositionType>()) {
        if (firstProtocolComposition->hasExplicitAnyObject() !=
            secondProtocolComposition->hasExplicitAnyObject()) {
          return mismatch(firstProtocolComposition.getPointer(), secondType,
                          sugaredFirstType);
        }

        auto firstMembers = firstProtocolComposition->getMembers();
        auto secondMembers = secondProtocolComposition->getMembers();

        if (firstMembers.size() == secondMembers.size()) {
          for (unsigned i : indices(firstMembers)) {
            auto firstMember = firstMembers[i];
            auto secondMember = secondMembers[i];

            // FIXME: We lose sugar here, because the sugared type might have a different
            // number of members, or the members might appear in a different order.
            if (!this->visit(CanType(firstMember), secondMember, firstMember)) {
              return false;
            }
          }

          return true;
        }
      }

      return mismatch(firstProtocolComposition.getPointer(), secondType,
                      sugaredFirstType);
    }

    bool visitParameterizedProtocolType(CanParameterizedProtocolType firstParametrizedProto,
                                        Type secondType,
                                        Type sugaredFirstType) {
      if (auto secondParametrizedProto = secondType->getAs<ParameterizedProtocolType>()) {
        if (!this->visit(firstParametrizedProto.getBaseType(),
                         secondParametrizedProto->getBaseType(),
                         sugaredFirstType->castTo<ParameterizedProtocolType>()
                             ->getBaseType())) {
          return false;
        }

        auto firstArgs = firstParametrizedProto->getArgs();
        auto secondArgs = secondParametrizedProto->getArgs();

        if (firstArgs.size() == secondArgs.size()) {
          for (unsigned i : indices(firstArgs)) {
            if (!this->visit(CanType(firstArgs[i]),
                             secondArgs[i],
                             sugaredFirstType->castTo<ParameterizedProtocolType>()
                                 ->getArgs()[i])) {
              return false;
            }
          }

          return true;
        }
      }

      return mismatch(firstParametrizedProto.getPointer(), secondType,
                      sugaredFirstType);
    }

    bool visitExistentialType(CanExistentialType firstExistential,
                              Type secondType,
                              Type sugaredFirstType) {
      if (auto secondExistential = secondType->getAs<ExistentialType>()) {
        return this->visit(firstExistential.getConstraintType(),
                           secondExistential->getConstraintType(),
                           sugaredFirstType->castTo<ExistentialType>()
                               ->getConstraintType());
      }

      return mismatch(firstExistential.getPointer(), secondType,
                      sugaredFirstType);
    }

    bool visitLValueType(CanLValueType firstLValue, Type secondType,
                         Type sugaredFirstType) {
      if (auto secondLValue = secondType->getAs<LValueType>()) {
        return this->visit(firstLValue.getObjectType(),
                           secondLValue->getObjectType(),
                           sugaredFirstType->castTo<LValueType>()
                            ->getObjectType());
      }

      return mismatch(firstLValue.getPointer(), secondType, sugaredFirstType);
    }

    bool visitInOutType(CanInOutType firstInOut, Type secondType,
                        Type sugaredFirstType) {
      if (auto secondInOut = secondType->getAs<InOutType>()) {
        return this->visit(firstInOut.getObjectType(),
                           secondInOut->getObjectType(),
                           sugaredFirstType->castTo<InOutType>()
                             ->getObjectType());
      }

      return mismatch(firstInOut.getPointer(), secondType, sugaredFirstType);
    }

    bool visitBoundGenericType(CanBoundGenericType firstBGT,
                               Type secondType, Type sugaredFirstType) {
      if (firstBGT->getKind() == secondType->getDesugaredType()->getKind()) {
        auto secondBGT = secondType->castTo<BoundGenericType>();
        if (firstBGT->getDecl() != secondBGT->getDecl())
          return mismatch(firstBGT.getPointer(), secondBGT, sugaredFirstType);

        auto sugaredFirstBGT = sugaredFirstType->castTo<BoundGenericType>();
        if (firstBGT->getParent() &&
            !this->visit(firstBGT.getParent(), secondBGT->getParent(),
                         sugaredFirstBGT->getParent()))
          return false;

        for (unsigned i = 0, n = firstBGT->getGenericArgs().size();
             i != n; ++i) {
          if (!this->visit(firstBGT.getGenericArgs()[i],
                           secondBGT->getGenericArgs()[i],
                           sugaredFirstBGT->getGenericArgs()[i]))
            return false;
        }

        return true;
      }

      return mismatch(firstBGT.getPointer(), secondType, sugaredFirstType);
    }

#undef TRIVIAL_CASE
  };

  bool alwaysMismatchTypeParameters() const { return false; }

  void pushPosition(Position pos) {}
  void popPosition(Position pos) {}

  ImplClass &asDerived() { return static_cast<ImplClass &>(*this); }

  const ImplClass &asDerived() const {
    return static_cast<const ImplClass &>(*this);
  }

public:
  bool match(Type first, Type second) {
    return MatchVisitor(*this).visit(first->getCanonicalType(), second,
                                     first);
  }
};

} // end namespace swift

#endif // SWIFT_AST_TYPE_MATCHER_H
