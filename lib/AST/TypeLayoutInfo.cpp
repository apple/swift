#include "swift/AST/ASTContext.h"
#include "swift/AST/CanTypeVisitor.h"
#include "swift/AST/TypeCheckRequests.h"
#include "swift/AST/TypeLayoutInfo.h"
#include "swift/SIL/AbstractionPattern.h"

namespace swift {
namespace Lowering {
/// A CRTP helper class for doing things that depends on type
/// classification.
template <class Impl, class RetTy>
class TypeClassifierBase
    : public CanTypeVisitor<Impl, RetTy, AbstractionPattern> {
  using super =
      CanTypeVisitor<Impl, RetTy, AbstractionPattern>;
  Impl &asImpl() { return *static_cast<Impl *>(this); }
protected:
  TypeClassifierBase() {}
public:

  RetTy visit(CanType substType, AbstractionPattern origType) {
    if (auto origEltType = origType.getVanishingTupleElementPatternType()) {
      return visit(substType, *origEltType);
    }

    return super::visit(substType, origType);
  }
};

class ComputeLayout : public TypeClassifierBase<ComputeLayout, TypeLayoutInfo> {
  TypeExpansionContext Expansion;
  IsTypeExpansionSensitive_t isSensitive;
public:
  ComputeLayout(TypeExpansionContext Expansion, IsTypeExpansionSensitive_t isSensitive)
    : TypeClassifierBase(),
                   Expansion(Expansion),
                   isSensitive(isSensitive)
                   {}

#define NAMED_INFO(TYPE, LOWERING)                                                 \
    TypeLayoutInfo visit##TYPE##Type(Can##TYPE##Type type,                   \
                                     AbstractionPattern orig) {              \
      return TypeLayoutInfo::for##LOWERING()            \
                .withTypeExpansionSensitive(isSensitive); \
    }
  NAMED_INFO(BuiltinInteger, Trivial)
  NAMED_INFO(BuiltinIntegerLiteral, Trivial)
  NAMED_INFO(BuiltinFloat, Trivial)
  NAMED_INFO(BuiltinRawUnsafeContinuation, Trivial)
  NAMED_INFO(BuiltinJob, Trivial)
  NAMED_INFO(BuiltinExecutor, Trivial)
  NAMED_INFO(BuiltinPackIndex, Trivial)
  NAMED_INFO(BuiltinNativeObject, Reference)
  NAMED_INFO(BuiltinBridgeObject, Reference)
  NAMED_INFO(BuiltinVector, Trivial)
  NAMED_INFO(BuiltinRawPointer, RawPointer)
  NAMED_INFO(SILToken, Trivial)
  NAMED_INFO(AnyMetatype, Trivial)
  NAMED_INFO(Module, Trivial)
#undef NAMED_INFO

#define MANUAL_INFO(TYPE, ...)                                               \
    TypeLayoutInfo visit##TYPE##Type(Can##TYPE##Type type,                   \
                                     AbstractionPattern orig) {              \
      return TypeLayoutInfo(__VA_ARGS__);                                                          \
    }
  MANUAL_INFO(BuiltinDefaultActorStorage,
              IsNotTrivial, IsFixedABI,
              IsAddressOnly, IsNotResilient,
              isSensitive,
              DoesNotHaveRawPointer,
              IsLexical)

  MANUAL_INFO(BuiltinUnsafeValueBuffer,
              IsNotTrivial, IsFixedABI,
              IsAddressOnly, IsNotResilient,
              isSensitive,
              DoesNotHaveRawPointer,
              IsLexical)

  MANUAL_INFO(BuiltinNonDefaultDistributedActorStorage,
              IsNotTrivial, IsFixedABI,
              IsAddressOnly, IsNotResilient,
              isSensitive,
              DoesNotHaveRawPointer,
              IsLexical)

  MANUAL_INFO(Pack,
              IsNotTrivial, IsFixedABI,
              IsAddressOnly, IsNotResilient,
              isSensitive,
              DoesNotHaveRawPointer,
              IsLexical)

  MANUAL_INFO(SILPack,
              IsNotTrivial, IsFixedABI,
              IsAddressOnly, IsNotResilient,
              isSensitive,
              DoesNotHaveRawPointer,
              IsLexical)
#undef MANUAL_INFO


#define NEVER_LOADABLE_CHECKED_REF_STORAGE(Name, ...) \
    TypeLayoutInfo visit##Name##StorageType(Can##Name##StorageType type, \
                                            AbstractionPattern origType) { \
      return { IsNotTrivial, \
               IsFixedABI, \
               IsAddressOnly, \
               IsNotResilient, \
               isSensitive, \
               DoesNotHaveRawPointer, \
               IsLexical}; \
    }
#define ALWAYS_LOADABLE_CHECKED_REF_STORAGE(Name, ...) \
    TypeLayoutInfo visit##Name##StorageType(Can##Name##StorageType type, \
                                            AbstractionPattern origType) { \
      return TypeLayoutInfo::forReference()            \
                .withTypeExpansionSensitive(isSensitive); \
    }
#define SOMETIMES_LOADABLE_CHECKED_REF_STORAGE(Name, ...) \
    TypeLayoutInfo visitLoadable##Name##StorageType(\
                                                Can##Name##StorageType type, \
                                                AbstractionPattern origType) { \
      return TypeLayoutInfo::forReference()            \
                .withTypeExpansionSensitive(isSensitive); \
    } \
    TypeLayoutInfo visitAddressOnly##Name##StorageType(\
                                                Can##Name##StorageType type, \
                                                AbstractionPattern origType) { \
      return { IsNotTrivial, \
               IsFixedABI, \
               IsAddressOnly, \
               IsNotResilient, \
               isSensitive, \
               DoesNotHaveRawPointer, \
               IsLexical}; \
    } \
    TypeLayoutInfo visit##Name##StorageType(Can##Name##StorageType type, \
                                            AbstractionPattern origType) { \
      auto referentType = \
        type->getReferentType()->lookThroughSingleOptionalType(); \
      auto concreteType = getConcreteReferenceStorageReferent(referentType, origType); \
      if (Name##StorageType::get(concreteType, TC.Context) \
            ->isLoadable(Expansion.getResilienceExpansion())) { \
        return asImpl().visitLoadable##Name##StorageType(type, origType, \
                                                         isSensitive); \
      } else { \
        return asImpl().visitAddressOnly##Name##StorageType(type, origType, \
                                                            isSensitive); \
      } \
    }
#define UNCHECKED_REF_STORAGE(Name, ...) \
    TypeLayoutInfo visit##Name##StorageType(Can##Name##StorageType type, \
                                            AbstractionPattern origType) { \
      return TypeLayoutInfo::forTrivial() \
                .withTypeExpansionSensitive(isSensitive); \
    }
#include "swift/AST/ReferenceStorage.def"

  // Tuples depend on their elements.
  TypeLayoutInfo visitTupleType(CanTupleType type,
                                AbstractionPattern origType) {
    TypeLayoutInfo props;
    props.setTypeExpansionSensitive(isSensitive);
    origType.forEachExpandedTupleElement(type,
                                         [&](AbstractionPattern origEltType,
                                             CanType substEltType,
                                             const TupleTypeElt &elt) {
                                           props.addSubobject(visit(substEltType,
                                                                    origEltType));
                                         });
    return props;
  }

  TypeLayoutInfo visitPackExpansionType(CanPackExpansionType type,
                                        AbstractionPattern origType) {
    TypeLayoutInfo props;
    props.setTypeExpansionSensitive(isSensitive);
    props.setAddressOnly();
    props.addSubobject(visit(type.getPatternType(),
                             origType.getPackExpansionPatternType()));
    return props;
  }

#define NOT_IMPLEMENTED(TYPE)                                                  \
    TypeLayoutInfo visit##TYPE##Type(Can##TYPE##Type type,                     \
                                     AbstractionPattern orig) {                \
      llvm_unreachable("unknown layout for TYPE");                                                          \
    }
#undef NOT_IMPLEMENTED

};

TypeLayoutInfo TypeLayoutInfo::get(ASTContext &ctx,
                                   CanType type,
                                   CanGenericSignature sig,
                                   TypeExpansionContext expansion) {
  return evaluateOrDefault(ctx.evaluator,
                           TypeLayoutInfoRequest{type},
                           TypeLayoutInfo());
}

} // Lowering


Lowering::TypeLayoutInfo TypeLayoutInfoRequest::evaluate(Evaluator &evaluator,
                                                         CanType type) const {
  Lowering::ComputeLayout computeLayout(TypeExpansionContext::minimal(),
                                        Lowering::IsTypeExpansionSensitive);

  // FIXME:: might be the wrong abstraction pattern and need it as an input here
  auto info = computeLayout.visit(type, Lowering::AbstractionPattern(type));

  info.setTypeExpansionSensitive(Lowering::IsTypeExpansionSensitive);
  return info;
}


} // swift

