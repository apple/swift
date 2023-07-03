//===--- AbstractionPattern.cpp - Abstraction patterns --------------------===//
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
// This file defines routines relating to abstraction patterns.
// working in concert with the Clang importer.
//
//===----------------------------------------------------------------------===//

#define DEBUG_TYPE "libsil"
#include "swift/AST/ASTContext.h"
#include "swift/AST/Decl.h"
#include "swift/AST/ForeignAsyncConvention.h"
#include "swift/AST/ForeignErrorConvention.h"
#include "swift/AST/GenericEnvironment.h"
#include "swift/AST/GenericSignature.h"
#include "swift/AST/ModuleLoader.h"
#include "swift/AST/TypeCheckRequests.h"
#include "swift/AST/CanTypeVisitor.h"
#include "swift/Basic/Defer.h"
#include "swift/SIL/TypeLowering.h"
#include "swift/SIL/AbstractionPatternGenerators.h"
#include "clang/AST/ASTContext.h"
#include "clang/AST/Attr.h"
#include "clang/AST/DeclCXX.h"
#include "clang/AST/DeclObjC.h"
#include "clang/AST/PrettyPrinter.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/ErrorHandling.h"

using namespace swift;
using namespace swift::Lowering;

AbstractionPattern
TypeConverter::getAbstractionPattern(AbstractStorageDecl *decl,
                                     bool isNonObjC) {
  if (auto var = dyn_cast<VarDecl>(decl)) {
    return getAbstractionPattern(var, isNonObjC);
  } else {
    return getAbstractionPattern(cast<SubscriptDecl>(decl), isNonObjC);
  }
}

AbstractionPattern
TypeConverter::getAbstractionPattern(SubscriptDecl *decl, bool isNonObjC) {
  auto sig = decl->getGenericSignatureOfContext().getCanonicalSignature();
  auto type = sig.getReducedType(decl->getElementInterfaceType());
  return AbstractionPattern(sig, type);
}

static const clang::Type *getClangType(const clang::Decl *decl) {
  if (auto valueDecl = dyn_cast<clang::ValueDecl>(decl)) {
    return valueDecl->getType().getTypePtr();
  }

  // This should *really* be a ValueDecl.
  return cast<clang::ObjCPropertyDecl>(decl)->getType().getTypePtr();
}

static Bridgeability getClangDeclBridgeability(const clang::Decl *decl) {
  // These declarations are always imported without bridging (for now).
  if (isa<clang::VarDecl>(decl) ||
      isa<clang::FieldDecl>(decl) ||
      isa<clang::IndirectFieldDecl>(decl))
    return Bridgeability::None;

  // Functions and methods always use normal bridging.
  return Bridgeability::Full;
}

AbstractionPattern
TypeConverter::getAbstractionPattern(VarDecl *var, bool isNonObjC) {
  auto sig = var->getDeclContext()
                 ->getGenericSignatureOfContext()
                 .getCanonicalSignature();

  auto interfaceType = var->getInterfaceType();
  if (auto *packExpansionType = interfaceType->getAs<PackExpansionType>())
    interfaceType = packExpansionType->getPatternType();

  auto swiftType = sig.getReducedType(interfaceType);

  if (isNonObjC)
    return AbstractionPattern(sig, swiftType);

  if (auto clangDecl = var->getClangDecl()) {
    auto clangType = getClangType(clangDecl);
    auto contextType = var->getDeclContext()->mapTypeIntoContext(swiftType);
    swiftType =
        getLoweredBridgedType(AbstractionPattern(sig, swiftType, clangType),
                              contextType, getClangDeclBridgeability(clangDecl),
                              SILFunctionTypeRepresentation::CFunctionPointer,
                              TypeConverter::ForMemory)
            ->getCanonicalType();
    return AbstractionPattern(sig, swiftType, clangType);
  }

  return AbstractionPattern(sig, swiftType);
}

AbstractionPattern TypeConverter::getAbstractionPattern(EnumElementDecl *decl) {
  assert(decl->hasAssociatedValues());
  assert(!decl->hasClangNode());

  // This cannot be implemented correctly for Optional.Some.
  assert(!decl->getParentEnum()->isOptionalDecl() &&
         "Optional.Some does not have a unique abstraction pattern because "
         "optionals are re-abstracted");

  auto sig = decl->getParentEnum()
                 ->getGenericSignatureOfContext()
                 .getCanonicalSignature();
  auto type = sig.getReducedType(decl->getArgumentInterfaceType());

  return AbstractionPattern(sig, type);
}

AbstractionPattern::EncodedForeignInfo
AbstractionPattern::EncodedForeignInfo::encode(
    const llvm::Optional<ForeignErrorConvention> &foreignError,
    const llvm::Optional<ForeignAsyncConvention> &foreignAsync) {
  // Foreign async convention takes precedence.
  if (foreignAsync.has_value()) {
    return EncodedForeignInfo(EncodedForeignInfo::Async,
                              foreignAsync->completionHandlerParamIndex(),
                              foreignAsync->completionHandlerErrorParamIndex(),
                              foreignAsync->completionHandlerFlagParamIndex(),
                              foreignAsync->completionHandlerFlagIsErrorOnZero());
  } else if (foreignError.has_value()) {
    return EncodedForeignInfo(EncodedForeignInfo::Error,
                              foreignError->getErrorParameterIndex(),
                              foreignError->isErrorParameterReplacedWithVoid(),
                              foreignError->stripsResultOptionality());
  } else {
    return {};
  }
}

AbstractionPattern AbstractionPattern::getObjCMethod(
    CanType origType, const clang::ObjCMethodDecl *method,
    const llvm::Optional<ForeignErrorConvention> &foreignError,
    const llvm::Optional<ForeignAsyncConvention> &foreignAsync) {
  auto errorInfo = EncodedForeignInfo::encode(foreignError, foreignAsync);
  return getObjCMethod(origType, method, errorInfo);
}

AbstractionPattern AbstractionPattern::getCurriedObjCMethod(
    CanType origType, const clang::ObjCMethodDecl *method,
    const llvm::Optional<ForeignErrorConvention> &foreignError,
    const llvm::Optional<ForeignAsyncConvention> &foreignAsync) {
  auto errorInfo = EncodedForeignInfo::encode(foreignError, foreignAsync);
  return getCurriedObjCMethod(origType, method, errorInfo);
}

AbstractionPattern
AbstractionPattern::getCurriedCFunctionAsMethod(CanType origType,
                                         const AbstractFunctionDecl *function) {
  auto clangFn = cast<clang::ValueDecl>(function->getClangDecl());
  return getCurriedCFunctionAsMethod(origType,
                     clangFn->getType().getTypePtr(),
                     function->getImportAsMemberStatus());
}

AbstractionPattern
AbstractionPattern::getCurriedCXXMethod(CanType origType,
                                        const AbstractFunctionDecl *function) {
  auto clangMethod = cast<clang::CXXMethodDecl>(function->getClangDecl());
  return getCurriedCXXMethod(origType, clangMethod, function->getImportAsMemberStatus());
}

AbstractionPattern
AbstractionPattern::getOptional(AbstractionPattern object) {
  switch (object.getKind()) {
  case Kind::Invalid:
    llvm_unreachable("querying invalid abstraction pattern!");
  case Kind::Tuple:
  case Kind::PartialCurriedObjCMethodType:
  case Kind::CurriedObjCMethodType:
  case Kind::CFunctionAsMethodType:
  case Kind::PartialCurriedCFunctionAsMethodType:
  case Kind::CurriedCFunctionAsMethodType:
  case Kind::ObjCMethodType:
  case Kind::CXXMethodType:
  case Kind::CurriedCXXMethodType:
  case Kind::PartialCurriedCXXMethodType:
  case Kind::OpaqueFunction:
  case Kind::OpaqueDerivativeFunction:
  case Kind::ObjCCompletionHandlerArgumentsType:
    llvm_unreachable("cannot add optionality to non-type abstraction");
  case Kind::Opaque:
    return AbstractionPattern::getOpaque();
  case Kind::ClangType:
    return AbstractionPattern(object.getGenericSubstitutions(),
                              object.getGenericSignature(),
                              OptionalType::get(object.getType())
                                ->getCanonicalType(),
                              object.getClangType());
  case Kind::Type:
    return AbstractionPattern(object.getGenericSubstitutions(),
                              object.getGenericSignature(),
                              OptionalType::get(object.getType())
                                ->getCanonicalType());
  case Kind::Discard:
    return AbstractionPattern::getDiscard(
                              object.getGenericSubstitutions(),
                              object.getGenericSignature(),
                              OptionalType::get(object.getType())
                                ->getCanonicalType());
  }
  llvm_unreachable("bad kind");
}

bool AbstractionPattern::isConcreteType() const {
  assert(isTypeParameter());
  return (getKind() != Kind::Opaque &&
          GenericSig != nullptr &&
          GenericSig->isConcreteType(getType()));
}

bool AbstractionPattern::requiresClass() const {
  switch (getKind()) {
  case Kind::Opaque:
    return false;
  case Kind::Type:
  case Kind::Discard:
  case Kind::ClangType: {
    auto type = getType();
    if (auto element = dyn_cast<PackElementType>(type))
      type = element.getPackType();
    if (auto archetype = dyn_cast<ArchetypeType>(type))
      return archetype->requiresClass();
    if (type->isTypeParameter()) {
      if (getKind() == Kind::ClangType) {
        // ObjC generics are always class constrained.
        return true;
      }

      assert(GenericSig &&
             "Dependent type in pattern without generic signature?");
      return GenericSig->requiresClass(type);
    }
    return false;
  }
    
  default:
    return false;
  }
}

LayoutConstraint AbstractionPattern::getLayoutConstraint() const {
  switch (getKind()) {
  case Kind::Opaque:
    return LayoutConstraint();
  case Kind::Type:
  case Kind::Discard:
  case Kind::ClangType: {
    auto type = getType();
    if (auto archetype = dyn_cast<ArchetypeType>(type)) {
      return archetype->getLayoutConstraint();
    } else if (isa<DependentMemberType>(type) ||
               isa<GenericTypeParamType>(type)) {
      if (getKind() == Kind::ClangType) {
        // ObjC generics are always class constrained.
        return LayoutConstraint::getLayoutConstraint(
          LayoutConstraintKind::Class);
      }

      assert(GenericSig &&
             "Dependent type in pattern without generic signature?");
      return GenericSig->getLayoutConstraint(type);
    }
    return LayoutConstraint();
  }
  default:
    return LayoutConstraint();
  }
}

bool AbstractionPattern::matchesTuple(CanType substType) const {
  switch (getKind()) {
  case Kind::Invalid:
    llvm_unreachable("querying invalid abstraction pattern!");
  case Kind::PartialCurriedObjCMethodType:
  case Kind::CurriedObjCMethodType:
  case Kind::PartialCurriedCFunctionAsMethodType:
  case Kind::CurriedCFunctionAsMethodType:
  case Kind::CFunctionAsMethodType:
  case Kind::ObjCMethodType:
  case Kind::CXXMethodType:
  case Kind::CurriedCXXMethodType:
  case Kind::PartialCurriedCXXMethodType:
  case Kind::OpaqueFunction:
  case Kind::OpaqueDerivativeFunction:
    return false;
  case Kind::Opaque:
    return true;
  case Kind::ObjCCompletionHandlerArgumentsType:
  case Kind::ClangType:
  case Kind::Type:
  case Kind::Discard:
    if (isTypeParameterOrOpaqueArchetype())
      return true;
    if (!isa<TupleType>(getType()))
      return false;
    LLVM_FALLTHROUGH;
  case Kind::Tuple: {
    if (doesTupleVanish()) {
      // TODO: recurse into elements.
      return true;
    }

    auto substTupleType = dyn_cast<TupleType>(substType);
    if (!substTupleType) return false;

    size_t nextSubstIndex = 0;
    auto nextComponentIsAcceptable = [&](bool isPackExpansion) -> bool {
      if (nextSubstIndex == substTupleType->getNumElements())
        return false;
      auto substComponentType = substTupleType.getElementType(nextSubstIndex++);
      return (isPackExpansion == isa<PackExpansionType>(substComponentType));
    };
    for (auto elt : getTupleElementTypes()) {
      bool isPackExpansion = elt.isPackExpansion();
      if (isPackExpansion && elt.GenericSubs) {
        auto origExpansion = cast<PackExpansionType>(elt.getType());
        auto substShape = cast<PackType>(
          origExpansion.getCountType().subst(elt.GenericSubs)
            ->getCanonicalType());
        for (auto shapeElt : substShape.getElementTypes()) {
          if (!nextComponentIsAcceptable(isa<PackExpansionType>(shapeElt)))
            return false;
        }
      } else if (!nextComponentIsAcceptable(isPackExpansion)) {
        return false;
      }
    }
    return nextSubstIndex == substTupleType->getNumElements();
  }
  }
  llvm_unreachable("bad kind");
}

static const clang::FunctionType *
getClangFunctionType(const clang::Type *clangType) {
  if (auto ptrTy = clangType->getAs<clang::PointerType>()) {
    clangType = ptrTy->getPointeeType().getTypePtr();
  } else if (auto blockTy = clangType->getAs<clang::BlockPointerType>()) {
    clangType = blockTy->getPointeeType().getTypePtr();
  } else if (auto refTy = clangType->getAs<clang::ReferenceType>()) {
    clangType = refTy->getPointeeType().getTypePtr();
  }
  return clangType->castAs<clang::FunctionType>();
}

static
const clang::Type *getClangFunctionParameterType(const clang::Type *ty,
                                                 unsigned index) {
  // TODO: adjust for error type parameter.

  // If we're asking about parameters, we'd better have a FunctionProtoType.
  auto fnType = getClangFunctionType(ty)->castAs<clang::FunctionProtoType>();
  assert(index < fnType->getNumParams());
  return fnType->getParamType(index).getTypePtr();
}

static
const clang::Type *getClangArrayElementType(const clang::Type *ty,
                                            unsigned index) {
  return ty->castAsArrayTypeUnsafe()->getElementType().getTypePtr();
}

static CanType getCanTupleElementType(CanType type, unsigned index) {
  if (auto tupleTy = dyn_cast<TupleType>(type))
    return tupleTy.getElementType(index);

  assert(index == 0);
  return type;
}

AbstractionPattern
AbstractionPattern::getTupleElementType(unsigned index) const {
  switch (getKind()) {
  case Kind::Invalid:
    llvm_unreachable("querying invalid abstraction pattern!");
  case Kind::PartialCurriedObjCMethodType:
  case Kind::CurriedObjCMethodType:
  case Kind::PartialCurriedCFunctionAsMethodType:
  case Kind::CurriedCFunctionAsMethodType:
  case Kind::CFunctionAsMethodType:
  case Kind::ObjCMethodType:
  case Kind::CXXMethodType:
  case Kind::CurriedCXXMethodType:
  case Kind::PartialCurriedCXXMethodType:
  case Kind::OpaqueFunction:
  case Kind::OpaqueDerivativeFunction:
    llvm_unreachable("function types are not tuples");
  case Kind::Opaque:
    return *this;
  case Kind::Tuple:
    assert(index < getNumTupleElements_Stored());
    return OrigTupleElements[index];
  case Kind::ClangType:
    return AbstractionPattern(getGenericSubstitutions(),
                              getGenericSignature(),
                              getCanTupleElementType(getType(), index),
                              getClangArrayElementType(getClangType(), index));
  case Kind::Discard:
    llvm_unreachable("operation not needed on discarded abstractions yet");
  case Kind::Type:
    if (isTypeParameterOrOpaqueArchetype())
      return AbstractionPattern::getOpaque();
    return AbstractionPattern(getGenericSubstitutions(),
                              getGenericSignature(),
                              getCanTupleElementType(getType(), index));
      
  case Kind::ObjCCompletionHandlerArgumentsType: {
    // Match up the tuple element with the parameter from the Clang block type,
    // skipping the error parameter and flag indexes if any.
    auto callback = cast<clang::FunctionProtoType>(getClangType());
    auto errorIndex = getEncodedForeignInfo()
      .getAsyncCompletionHandlerErrorParamIndex();
    auto flagIndex = getEncodedForeignInfo()
      .getAsyncCompletionHandlerErrorFlagParamIndex();
    unsigned paramIndex = index;
    if (errorIndex && paramIndex >= *errorIndex)
      ++paramIndex;
    if (flagIndex && paramIndex >= *flagIndex)
      ++paramIndex;
    return AbstractionPattern(getGenericSubstitutions(),
                              getGenericSignature(),
                              getCanTupleElementType(getType(), index),
                              callback->getParamType(paramIndex).getTypePtr());
  }
    
  }
  llvm_unreachable("bad kind");
}

bool AbstractionPattern::doesTupleContainPackExpansionType() const {
  switch (getKind()) {
  case Kind::Invalid:
    llvm_unreachable("querying invalid abstraction pattern!");
  case Kind::Opaque:
  case Kind::PartialCurriedObjCMethodType:
  case Kind::CurriedObjCMethodType:
  case Kind::CFunctionAsMethodType:
  case Kind::CurriedCFunctionAsMethodType:
  case Kind::PartialCurriedCFunctionAsMethodType:
  case Kind::ObjCMethodType:
  case Kind::CXXMethodType:
  case Kind::CurriedCXXMethodType:
  case Kind::PartialCurriedCXXMethodType:
  case Kind::OpaqueFunction:
  case Kind::OpaqueDerivativeFunction:
    llvm_unreachable("pattern is not a tuple");
  case Kind::Tuple: {
    for (auto &elt : llvm::makeArrayRef(OrigTupleElements,
                                        getNumTupleElements_Stored())) {
      if (elt.isPackExpansion())
        return true;
    }
    return true;
  }
  case Kind::ObjCCompletionHandlerArgumentsType:
  case Kind::Type:
  case Kind::Discard:
  case Kind::ClangType:
    return cast<TupleType>(getType()).containsPackExpansionType();
  }
  llvm_unreachable("bad kind");
}

bool AbstractionPattern::doesTupleVanish() const {
  assert(isTuple());
  return getVanishingTupleElementPatternType().has_value();
}

llvm::Optional<AbstractionPattern>
AbstractionPattern::getVanishingTupleElementPatternType() const {
  if (!isTuple())
    return llvm::None;
  if (!GenericSubs)
    return llvm::None;

  // Substitution causes tuples to vanish when substituting the elements
  // produces a singleton tuple and it didn't start that way.

  auto numOrigElts = getNumTupleElements();

  // Track whether we've found a single element.
  llvm::Optional<AbstractionPattern> singletonEltType;
  bool hadOrigExpansion = false;
  for (auto index : range(numOrigElts)) {
    auto eltType = getTupleElementType(index);

    // If this pattern isn't a pack expansion, we've got a new candidate
    // singleton.  If this is the second such candidate, of course, it's
    // not a singleton.
    if (!eltType.isPackExpansion()) {
      if (singletonEltType)
        return llvm::None;
      singletonEltType = eltType;

    // Otherwise, check what the expansion shape expands to.
    } else {
      hadOrigExpansion = true;

      auto expansionType = cast<PackExpansionType>(eltType.getType());
      auto substShape = cast<PackType>(
        expansionType.getCountType().subst(GenericSubs)->getCanonicalType());
      auto expansionCount = substShape->getNumElements();

      // If it expands to multiple elements or to a single expansion, we
      // won't have a singleton tuple.  If it expands to a single scalar
      // element, this is a singleton candidate.
      if (expansionCount > 1) {
        return llvm::None;
      } else if (expansionCount == 1) {
        auto substExpansion =
          dyn_cast<PackExpansionType>(substShape.getElementType(0));
        if (substExpansion)
          return llvm::None;
        if (singletonEltType)
          return llvm::None;
        singletonEltType = eltType.getPackExpansionPatternType();
      }
    }
  }

  // If we found a singleton scalar element, and we didn't start with
  // a singleton element, that's the index we want to return.
  if (singletonEltType && !(numOrigElts == 1 && !hadOrigExpansion))
    return singletonEltType;
  return llvm::None;
}

void AbstractionPattern::forEachTupleElement(CanType substType,
      llvm::function_ref<void(TupleElementGenerator &)> handleElement) const {
  TupleElementGenerator elt(*this, substType);
  for (; !elt.isFinished(); elt.advance()) {
    handleElement(elt);
  }
  elt.finish();
}

TupleElementGenerator::TupleElementGenerator(
                              AbstractionPattern origTupleType,
                              CanType substType)
    : origTupleType(origTupleType), substType(substType) {
  assert(origTupleType.isTuple());
  assert(origTupleType.matchesTuple(substType));

  origTupleVanishes = origTupleType.doesTupleVanish();
  origTupleTypeIsOpaque = origTupleType.isOpaqueTuple();
  numOrigElts = origTupleType.getNumTupleElements();

  if (!isFinished()) loadElement();
}

void AbstractionPattern::forEachExpandedTupleElement(CanType substType,
                      llvm::function_ref<void(AbstractionPattern origEltType,
                                              CanType substEltType,
                                              const TupleTypeElt &elt)>
                        handleElement) const {
  assert(matchesTuple(substType));

  // Handle opaque patterns by just iterating the substituted components.
  if (!isTuple()) {
    auto substTupleType = cast<TupleType>(substType);
    auto substEltTypes = substTupleType.getElementTypes();
    for (auto i : indices(substEltTypes)) {
      handleElement(getTupleElementType(i), substEltTypes[i],
                    substTupleType->getElement(i));
    }
    return;
  }

  // For vanishing tuples, just call the callback once.
  if (auto origEltType = getVanishingTupleElementPatternType()) {
    handleElement(*origEltType, substType, TupleTypeElt(substType));
    return;
  }

  auto substTupleType = cast<TupleType>(substType);
  auto substEltTypes = substTupleType.getElementTypes();

  // For non-opaque patterns, we have to iterate the original components
  // in order to match things up properly, but we'll still end up calling
  // once per substituted element.
  size_t substEltIndex = 0;
  for (size_t origEltIndex : range(getNumTupleElements())) {
    auto origEltType = getTupleElementType(origEltIndex);
    if (!origEltType.isPackExpansion()) {
      handleElement(origEltType, substEltTypes[substEltIndex],
                    substTupleType->getElement(substEltIndex));
      substEltIndex++;
    } else {
      auto origPatternType = origEltType.getPackExpansionPatternType();
      for (auto i : range(origEltType.getNumPackExpandedComponents())) {
        (void) i;
        auto substEltType = substEltTypes[substEltIndex];
        // When the substituted type is a pack expansion, pass down
        // the original element type so that it's *also* a pack expansion.
        // Clients expect to look through this structure in parallel on
        // both types.  The count is misleading, but normal usage won't
        // access it, and there's nothing we could provide that *wouldn't*
        // be misleading in one way or another.
        handleElement(isa<PackExpansionType>(substEltType)
                        ? origEltType : origPatternType,
                      substEltType,
                      substTupleType->getElement(substEltIndex));
        substEltIndex++;
      }
    }
  }
  assert(substEltIndex == substEltTypes.size());
}

AbstractionPattern
AbstractionPattern::getPackElementPackType() const {
  switch (getKind()) {
  case Kind::Invalid:
    llvm_unreachable("querying invalid abstraction pattern!");
  case Kind::PartialCurriedObjCMethodType:
  case Kind::CurriedObjCMethodType:
  case Kind::PartialCurriedCFunctionAsMethodType:
  case Kind::CurriedCFunctionAsMethodType:
  case Kind::CFunctionAsMethodType:
  case Kind::ObjCMethodType:
  case Kind::CXXMethodType:
  case Kind::CurriedCXXMethodType:
  case Kind::PartialCurriedCXXMethodType:
  case Kind::OpaqueFunction:
  case Kind::OpaqueDerivativeFunction:
  case Kind::ClangType:
  case Kind::Tuple:
  case Kind::ObjCCompletionHandlerArgumentsType:
    llvm_unreachable("not a pack type");
  case Kind::Opaque:
    return *this;
  case Kind::Discard:
    llvm_unreachable("operation not needed on discarded abstractions yet");
  case Kind::Type:
    if (isTypeParameterOrOpaqueArchetype())
      return AbstractionPattern::getOpaque();
    return AbstractionPattern(getGenericSubstitutions(),
                              getGenericSignature(),
                              cast<PackElementType>(getType()).getPackType());
  }
  llvm_unreachable("bad kind");
}

static CanType getCanPackElementType(CanType type, unsigned index) {
  return cast<PackType>(type).getElementType(index);
}

static CanType getCanSILPackElementType(CanType type, unsigned index) {
  return cast<SILPackType>(type).getElementType(index);
}

static CanType getAnyCanPackElementType(CanType type, unsigned index) {
  if (isa<PackType>(type)) {
    return getCanPackElementType(type, index);
  }
  return getCanSILPackElementType(type, index);
}

AbstractionPattern
AbstractionPattern::getPackElementType(unsigned index) const {
  switch (getKind()) {
  case Kind::Invalid:
    llvm_unreachable("querying invalid abstraction pattern!");
  case Kind::PartialCurriedObjCMethodType:
  case Kind::CurriedObjCMethodType:
  case Kind::PartialCurriedCFunctionAsMethodType:
  case Kind::CurriedCFunctionAsMethodType:
  case Kind::CFunctionAsMethodType:
  case Kind::ObjCMethodType:
  case Kind::CXXMethodType:
  case Kind::CurriedCXXMethodType:
  case Kind::PartialCurriedCXXMethodType:
  case Kind::OpaqueFunction:
  case Kind::OpaqueDerivativeFunction:
  case Kind::ClangType:
  case Kind::Tuple:
  case Kind::ObjCCompletionHandlerArgumentsType:
    llvm_unreachable("not a pack type");
  case Kind::Opaque:
    return *this;
  case Kind::Discard:
    llvm_unreachable("operation not needed on discarded abstractions yet");
  case Kind::Type:
    if (isTypeParameterOrOpaqueArchetype())
      return AbstractionPattern::getOpaque();
    return AbstractionPattern(getGenericSubstitutions(),
                              getGenericSignature(),
                              getAnyCanPackElementType(getType(), index)); 
  }
  llvm_unreachable("bad kind");
}

bool AbstractionPattern::matchesPack(CanPackType substType) {
  switch (getKind()) {
  case Kind::Invalid:
    llvm_unreachable("querying invalid abstraction pattern!");
  case Kind::PartialCurriedObjCMethodType:
  case Kind::CurriedObjCMethodType:
  case Kind::PartialCurriedCFunctionAsMethodType:
  case Kind::CurriedCFunctionAsMethodType:
  case Kind::CFunctionAsMethodType:
  case Kind::ObjCMethodType:
  case Kind::CXXMethodType:
  case Kind::CurriedCXXMethodType:
  case Kind::PartialCurriedCXXMethodType:
  case Kind::OpaqueFunction:
  case Kind::OpaqueDerivativeFunction:
  case Kind::Tuple:
  case Kind::ObjCCompletionHandlerArgumentsType:
  case Kind::ClangType:
    return false;
  case Kind::Opaque:
    return true;
  case Kind::Type:
  case Kind::Discard: {
    if (isTypeParameterOrOpaqueArchetype())
      return true;
    auto type = getType();
    if (auto pack = dyn_cast<PackType>(type))
      return (pack->getNumElements() == substType->getNumElements());
    return false;
  }
  }
  llvm_unreachable("bad kind");
}

AbstractionPattern
AbstractionPattern::getPackExpansionComponentType(CanType substType) const {
  return getPackExpansionComponentType(isa<PackExpansionType>(substType));
}

AbstractionPattern
AbstractionPattern::getPackExpansionComponentType(bool isExpansion) const {
  assert(isPackExpansion());
  return isExpansion ? *this : getPackExpansionPatternType();
}

static CanType getPackExpansionPatternType(CanType type) {
  return cast<PackExpansionType>(type).getPatternType();
}

AbstractionPattern AbstractionPattern::getPackExpansionPatternType() const {
  switch (getKind()) {
  case Kind::Invalid:
    llvm_unreachable("querying invalid abstraction pattern!");
  case Kind::ObjCMethodType:
  case Kind::CurriedObjCMethodType:
  case Kind::PartialCurriedObjCMethodType:
  case Kind::CFunctionAsMethodType:
  case Kind::CurriedCFunctionAsMethodType:
  case Kind::PartialCurriedCFunctionAsMethodType:
  case Kind::CXXMethodType:
  case Kind::CurriedCXXMethodType:
  case Kind::PartialCurriedCXXMethodType:
  case Kind::Tuple:
  case Kind::OpaqueFunction:
  case Kind::OpaqueDerivativeFunction:
  case Kind::ObjCCompletionHandlerArgumentsType:
  case Kind::ClangType:
    llvm_unreachable("pattern for function or tuple cannot be for "
                     "pack expansion type");

  case Kind::Opaque:
    return *this;

  case Kind::Type:
    if (isTypeParameterOrOpaqueArchetype())
      return AbstractionPattern::getOpaque();
    return AbstractionPattern(getGenericSubstitutions(),
                              getGenericSignature(),
                              ::getPackExpansionPatternType(getType()));

  case Kind::Discard:
    return AbstractionPattern::getDiscard(
        getGenericSubstitutions(), getGenericSignature(),
        ::getPackExpansionPatternType(getType()));
  }
  llvm_unreachable("bad kind");
}

size_t AbstractionPattern::getNumPackExpandedComponents() const {
  assert(isPackExpansion());
  assert(getKind() == Kind::Type || getKind() == Kind::Discard);

  // If we don't have substitutions, we should be walking parallel
  // structure; take a single element.
  if (!GenericSubs) return 1;

  // Otherwise, substitute the expansion shape.
  auto origExpansion = cast<PackExpansionType>(getType());
  auto substShape = cast<PackType>(
    origExpansion.getCountType().subst(GenericSubs)->getCanonicalType());
  return substShape->getNumElements();
}

AbstractionPattern AbstractionPattern::getMetatypeInstanceType() const {
  assert(getKind() == Kind::Type);
  return AbstractionPattern(getGenericSubstitutions(),
                            getGenericSignature(),
                       cast<AnyMetatypeType>(getType()).getInstanceType());
}

AbstractionPattern AbstractionPattern::getDynamicSelfSelfType() const {
  assert(getKind() == Kind::Type);
  return AbstractionPattern(getGenericSubstitutions(),
                            getGenericSignature(),
                            cast<DynamicSelfType>(getType()).getSelfType());
}

AbstractionPattern
AbstractionPattern::getParameterizedProtocolArgType(unsigned argIndex) const {
  assert(getKind() == Kind::Type);
  return AbstractionPattern(getGenericSubstitutions(),
                            getGenericSignature(),
            cast<ParameterizedProtocolType>(getType()).getArgs()[argIndex]);
}

AbstractionPattern AbstractionPattern::removingMoveOnlyWrapper() const {
  switch (getKind()) {
  case Kind::Invalid:
    llvm_unreachable("querying invalid abstraction pattern!");
  case Kind::PartialCurriedObjCMethodType:
  case Kind::CurriedObjCMethodType:
  case Kind::PartialCurriedCFunctionAsMethodType:
  case Kind::CurriedCFunctionAsMethodType:
  case Kind::CFunctionAsMethodType:
  case Kind::ObjCMethodType:
  case Kind::CXXMethodType:
  case Kind::CurriedCXXMethodType:
  case Kind::PartialCurriedCXXMethodType:
  case Kind::OpaqueFunction:
  case Kind::OpaqueDerivativeFunction:
    llvm_unreachable("function types can not be move only");
  case Kind::ClangType:
    llvm_unreachable("clang types can not be move only yet");
  case Kind::ObjCCompletionHandlerArgumentsType:
    llvm_unreachable("not handled yet");
  case Kind::Discard:
    llvm_unreachable("operation not needed on discarded abstractions yet");
  case Kind::Tuple:
    llvm_unreachable("cannot apply move-only wrappers to open-coded patterns");
  case Kind::Opaque:
    // Opaque is opaque. We do not remove anything.
    return *this;
  case Kind::Type:
    if (auto mvi = dyn_cast<SILMoveOnlyWrappedType>(getType())) {
      return AbstractionPattern(getGenericSubstitutions(),
                                getGenericSignature(), mvi->getInnerType());
    }
    return *this;
  }

  llvm_unreachable("bad kind");
}

AbstractionPattern AbstractionPattern::addingMoveOnlyWrapper() const {
  switch (getKind()) {
  case Kind::Invalid:
    llvm_unreachable("querying invalid abstraction pattern!");
  case Kind::PartialCurriedObjCMethodType:
  case Kind::CurriedObjCMethodType:
  case Kind::PartialCurriedCFunctionAsMethodType:
  case Kind::CurriedCFunctionAsMethodType:
  case Kind::CFunctionAsMethodType:
  case Kind::ObjCMethodType:
  case Kind::CXXMethodType:
  case Kind::CurriedCXXMethodType:
  case Kind::PartialCurriedCXXMethodType:
  case Kind::OpaqueFunction:
  case Kind::OpaqueDerivativeFunction:
    llvm_unreachable("function types can not be move only");
  case Kind::ClangType:
    llvm_unreachable("clang types can not be move only yet");
  case Kind::ObjCCompletionHandlerArgumentsType:
    llvm_unreachable("not handled yet");
  case Kind::Discard:
    llvm_unreachable("operation not needed on discarded abstractions yet");
  case Kind::Tuple:
    llvm_unreachable("cannot add move only wrapper to open-coded pattern");
  case Kind::Opaque:
  case Kind::Type:
    if (isa<SILMoveOnlyWrappedType>(getType()))
      return *this;
    return AbstractionPattern(getGenericSubstitutions(),
                              getGenericSignature(),
                              SILMoveOnlyWrappedType::get(getType()));
  }

  llvm_unreachable("bad kind");
}

/// Return a pattern corresponding to the 'self' parameter of the given
/// Objective-C method.
AbstractionPattern
AbstractionPattern::getObjCMethodSelfPattern(CanType selfType) const {
  // Just use id for the receiver type.  If this is ever
  // insufficient --- if we have interesting bridging to do to
  // 'self' --- we have the right information to be more exact.
  auto clangSelfType =
    getObjCMethod()->getASTContext().getObjCIdType().getTypePtr();

  return AbstractionPattern(getGenericSubstitutions(),
                            getGenericSignatureForFunctionComponent(),
                            selfType, clangSelfType);
}

/// Return a pattern corresponding to the 'self' parameter of the given
/// C function imported as a method.
AbstractionPattern
AbstractionPattern::getCFunctionAsMethodSelfPattern(CanType selfType) const {
  auto memberStatus = getImportAsMemberStatus();
  if (memberStatus.isInstance()) {
    // Use the clang type for the receiver type.  If this is ever
    // insufficient --- if we have interesting bridging to do to
    // 'self' --- we have the right information to be more exact.
    auto clangSelfType =
      getClangFunctionParameterType(getClangType(),memberStatus.getSelfIndex());

    return AbstractionPattern(getGenericSubstitutions(),
                              getGenericSignatureForFunctionComponent(),
                              selfType, clangSelfType);
  }
  // The formal metatype parameter to a C function imported as a static method
  // is dropped on the floor. Leave it untransformed.
  return AbstractionPattern::getDiscard(
                           getGenericSubstitutions(),
                           getGenericSignatureForFunctionComponent(), selfType);
}

AbstractionPattern
AbstractionPattern::getCXXMethodSelfPattern(CanType selfType) const {
  assert(hasStoredCXXMethod());
  auto CXXMethod = getCXXMethod();
  if (CXXMethod->isInstance()) {
    // Use the clang type for the receiver type.  If this is ever
    // insufficient --- if we have interesting bridging to do to
    // 'self' --- we have the right information to be more exact.
    auto clangSelfType =
        CXXMethod->getThisType().getTypePtr();
    return AbstractionPattern(getGenericSubstitutions(),
                              getGenericSignatureForFunctionComponent(),
                              selfType, clangSelfType);
  }
  // The formal metatype parameter to a C++ function imported as a static method
  // is dropped on the floor. Leave it untransformed.
  return AbstractionPattern::getDiscard(
      getGenericSubstitutions(),
      getGenericSignatureForFunctionComponent(), selfType);
}

static CanType getResultType(CanType type) {
  return cast<AnyFunctionType>(type).getResult();
}

AbstractionPattern AbstractionPattern::getFunctionResultType() const {
  switch (getKind()) {
  case Kind::Invalid:
    llvm_unreachable("querying invalid abstraction pattern!");
  case Kind::ObjCCompletionHandlerArgumentsType:
  case Kind::Tuple:
    llvm_unreachable("abstraction pattern for tuple cannot be function");
  case Kind::Opaque:
    return *this;
  case Kind::Type:
    if (isTypeParameterOrOpaqueArchetype())
      return AbstractionPattern::getOpaque();
    return AbstractionPattern(getGenericSubstitutions(),
                              getGenericSignatureForFunctionComponent(),
                              getResultType(getType()));
  case Kind::Discard:
    llvm_unreachable("don't need to discard function abstractions yet");
  case Kind::ClangType:
  case Kind::CFunctionAsMethodType:
  case Kind::PartialCurriedCFunctionAsMethodType: {
    auto clangFunctionType = getClangFunctionType(getClangType());
    return AbstractionPattern(getGenericSubstitutions(),
                              getGenericSignatureForFunctionComponent(),
                              getResultType(getType()),
                              clangFunctionType->getReturnType().getTypePtr());    
  }
  case Kind::CXXMethodType:
  case Kind::PartialCurriedCXXMethodType:
    return AbstractionPattern(getGenericSubstitutions(),
                              getGenericSignatureForFunctionComponent(),
                              getResultType(getType()),
                              getCXXMethod()->getReturnType().getTypePtr());
  case Kind::CurriedObjCMethodType:
    return getPartialCurriedObjCMethod(
                              getGenericSubstitutions(),
                              getGenericSignatureForFunctionComponent(),
                              getResultType(getType()),
                              getObjCMethod(),
                              getEncodedForeignInfo());
  case Kind::CurriedCFunctionAsMethodType:
    return getPartialCurriedCFunctionAsMethod(
                                      getGenericSubstitutions(),
                                      getGenericSignatureForFunctionComponent(),
                                      getResultType(getType()),
                                      getClangType(),
                                      getImportAsMemberStatus());
  case Kind::CurriedCXXMethodType:
    return getPartialCurriedCXXMethod(getGenericSubstitutions(),
                                      getGenericSignatureForFunctionComponent(),
                                      getResultType(getType()), getCXXMethod(),
                                      getImportAsMemberStatus());
  case Kind::PartialCurriedObjCMethodType:
  case Kind::ObjCMethodType: {
    // If this is a foreign async function, the result type comes from the
    // completion callback argument to the original method. Line up the
    // result abstraction pattern with that callback argument.
    if (getEncodedForeignInfo().getKind() == EncodedForeignInfo::IsAsync) {
      auto paramIndex
        = getEncodedForeignInfo().getAsyncCompletionHandlerParamIndex();
      
      auto callbackParamTy = getObjCMethod()->parameters()[paramIndex]
                                            ->getType()
                                            ->getPointeeType()
                                            ->getAs<clang::FunctionProtoType>();
      
      // The result comprises the non-error argument(s) to the callback, if
      // any.
      
      auto callbackErrorIndex = getEncodedForeignInfo()
                                    .getAsyncCompletionHandlerErrorParamIndex();
      auto callbackErrorFlagIndex = getEncodedForeignInfo()
                                .getAsyncCompletionHandlerErrorFlagParamIndex();
      assert((!callbackErrorIndex.has_value()
              || callbackParamTy->getNumParams() > *callbackErrorIndex)
             && "completion handler has invalid error param index?!");
      assert((!callbackErrorFlagIndex.has_value()
              || callbackParamTy->getNumParams() > *callbackErrorFlagIndex)
             && "completion handler has invalid error param index?!");
      unsigned numNonErrorParams
        = callbackParamTy->getNumParams() - callbackErrorIndex.has_value()
                                          - callbackErrorFlagIndex.has_value();
            
      switch (numNonErrorParams) {
      case 0:
        // If there are no result arguments, then the imported result type is
        // Void, with no interesting abstraction properties.
        return AbstractionPattern(TupleType::getEmpty(getType()->getASTContext()));
          
      case 1: {
        // If there's a single argument, abstract it according to its formal type
        // in the ObjC signature.
        unsigned callbackResultIndex = 0;
        for (auto index : indices(callbackParamTy->getParamTypes())) {
          if (callbackErrorIndex && index == *callbackErrorIndex)
            continue;
          if (callbackErrorFlagIndex && index == *callbackErrorFlagIndex)
            continue;
          callbackResultIndex = index;
          break;
        }
        auto clangResultType = callbackParamTy
          ->getParamType(callbackResultIndex)
          .getTypePtr();
        
        return AbstractionPattern(getGenericSubstitutions(),
                                  getGenericSignatureForFunctionComponent(),
                                  getResultType(getType()), clangResultType);
      }
          
      default:
        // If there are multiple results, we have a special abstraction pattern
        // form to represent the mapping from block parameters to tuple elements
        // in the return type.
        return AbstractionPattern::getObjCCompletionHandlerArgumentsType(
                      getGenericSubstitutions(),
                      getGenericSignatureForFunctionComponent(),
                      getResultType(getType()), callbackParamTy,
                      getEncodedForeignInfo());
      }
    }
    
    return AbstractionPattern(getGenericSubstitutions(),
                              getGenericSignatureForFunctionComponent(),
                              getResultType(getType()),
                              getObjCMethod()->getReturnType().getTypePtr());
  }
  case Kind::OpaqueFunction:
    return getOpaque();
  case Kind::OpaqueDerivativeFunction:
    static SmallVector<AbstractionPattern, 2> elements{getOpaque(),
                                                       getOpaqueFunction()};
    return getTuple(elements);
  }
  llvm_unreachable("bad kind");
}

AbstractionPattern
AbstractionPattern::getObjCMethodAsyncCompletionHandlerType(
                                     CanType swiftCompletionHandlerType) const {
  switch (getKind()) {
  case Kind::PartialCurriedObjCMethodType:
  case Kind::ObjCMethodType: {
    // Create an abstraction pattern using the original ObjC type of the
    // completion handler.
    assert(getEncodedForeignInfo().getKind() == EncodedForeignInfo::IsAsync);
    auto paramIndex = getEncodedForeignInfo().getAsyncCompletionHandlerParamIndex();
    auto callbackParamTy = getObjCMethod()->parameters()[paramIndex]
                                          ->getType().getTypePtr();
    
    CanGenericSignature patternSig;
    if (auto origSig = getGenericSignature()) {
      patternSig = origSig;
    } else if (auto genFnTy = dyn_cast<GenericFunctionType>(getType())) {
      patternSig = genFnTy->getGenericSignature().getCanonicalSignature();
    }
    
    return AbstractionPattern(patternSig,
                              swiftCompletionHandlerType, callbackParamTy);
  }
  case Kind::Opaque:
  case Kind::OpaqueFunction:
  case Kind::OpaqueDerivativeFunction:
  case Kind::Type:
    return AbstractionPattern(getGenericSignature(),
                              swiftCompletionHandlerType);
  case Kind::Invalid:
  case Kind::Tuple:
  case Kind::Discard:
  case Kind::ClangType:
  case Kind::CFunctionAsMethodType:
  case Kind::PartialCurriedCFunctionAsMethodType:
  case Kind::CXXMethodType:
  case Kind::PartialCurriedCXXMethodType:
  case Kind::CurriedObjCMethodType:
  case Kind::CurriedCFunctionAsMethodType:
  case Kind::CurriedCXXMethodType:
  case Kind::ObjCCompletionHandlerArgumentsType:
    swift_unreachable("not appropriate for this kind");
  }
  llvm_unreachable("covered switch");
}


CanType AbstractionPattern::getObjCMethodAsyncCompletionHandlerForeignType(
    ForeignAsyncConvention convention,
    Lowering::TypeConverter &TC
) const {
  auto nativeCHTy = convention.completionHandlerType();

  // Use the abstraction pattern we're lowering against in order to lower
  // the completion handler type, so we can preserve C/ObjC distinctions that
  // normally get abstracted away by the importer.
  auto completionHandlerNativeOrigTy = getObjCMethodAsyncCompletionHandlerType(nativeCHTy);
  
  // Bridge the Swift completion handler type back to its
  // foreign representation.
  auto foreignCHTy = TC.getLoweredBridgedType(completionHandlerNativeOrigTy,
                                    nativeCHTy,
                                    Bridgeability::Full,
                                    SILFunctionTypeRepresentation::ObjCMethod,
                                    TypeConverter::ForArgument)
    ->getCanonicalType();

  return foreignCHTy;
}

AbstractionPattern
AbstractionPattern::getFunctionParamType(unsigned index) const {
  switch (getKind()) {
  case Kind::Opaque:
    return *this;
  case Kind::Type: {
    if (isTypeParameterOrOpaqueArchetype())
      return AbstractionPattern::getOpaque();
    auto params = cast<AnyFunctionType>(getType()).getParams();
    return AbstractionPattern(getGenericSubstitutions(),
                              getGenericSignatureForFunctionComponent(),
                              params[index].getParameterType());
  }
  case Kind::CurriedCFunctionAsMethodType: {
    auto params = cast<AnyFunctionType>(getType()).getParams();
    assert(params.size() == 1);
    return getCFunctionAsMethodSelfPattern(params[0].getParameterType());
  }
  case Kind::CurriedCXXMethodType: {
    auto params = cast<AnyFunctionType>(getType()).getParams();
    assert(params.size() == 1);
    return getCXXMethodSelfPattern(params[0].getParameterType());
  }
  case Kind::CFunctionAsMethodType:
  case Kind::PartialCurriedCFunctionAsMethodType: {
    auto params = cast<AnyFunctionType>(getType()).getParams();

    // Only the full method type has a 'self' parameter.
    if (getKind() == Kind::CFunctionAsMethodType) {
      assert(params.size() > 0);

      // The last parameter is 'self'.
      if (index == params.size() - 1) {
        return getCFunctionAsMethodSelfPattern(params.back().getParameterType());
      }
    }

    // A parameter of type () does not correspond to a Clang parameter.
    auto paramType = params[index].getParameterType();
    if (paramType->isVoid())
      return AbstractionPattern(paramType);

    // Otherwise, we're talking about the formal parameter clause.
    // Jump over the self parameter in the Clang type.
    unsigned clangIndex = index;
    auto memberStatus = getImportAsMemberStatus();
    if (memberStatus.isInstance() && clangIndex >= memberStatus.getSelfIndex())
      ++clangIndex;
    return AbstractionPattern(getGenericSignatureForFunctionComponent(),
                              paramType,
                     getClangFunctionParameterType(getClangType(), clangIndex));
  }
  case Kind::CXXMethodType:
  case Kind::PartialCurriedCXXMethodType: {
    auto params = cast<AnyFunctionType>(getType()).getParams();

    // Only the full method type has a 'self' parameter.
    if (getKind() == Kind::CXXMethodType) {
      assert(params.size() > 0);

      // The last parameter is 'self'.
      if (index == params.size() - 1) {
        return getCXXMethodSelfPattern(params.back().getParameterType());
      }
    }

    // A parameter of type () does not correspond to a Clang parameter.
    auto paramType = params[index].getParameterType();
    if (paramType->isVoid())
      return AbstractionPattern(paramType);

    // Otherwise, we're talking about the formal parameter clause.
    auto methodType = getCXXMethod()->getType().getTypePtr();
    return AbstractionPattern(getGenericSignatureForFunctionComponent(),
                              paramType,
                              getClangFunctionParameterType(methodType, index));
  }
  case Kind::CurriedObjCMethodType: {
    auto params = cast<AnyFunctionType>(getType()).getParams();
    assert(params.size() == 1);
    return getObjCMethodSelfPattern(params[0].getParameterType());
  }
  case Kind::ObjCMethodType:
  case Kind::PartialCurriedObjCMethodType: {
    auto params = cast<AnyFunctionType>(getType()).getParams();

    // Only the full method type has a 'self' parameter.
    if (getKind() == Kind::ObjCMethodType) {
      assert(params.size() > 0);

      // The last parameter is 'self'.
      if (index == params.size() - 1) {
        return getObjCMethodSelfPattern(params.back().getParameterType());
      }
    }

    // A parameter of type () does not correspond to a Clang parameter.
    auto paramType = params[index].getParameterType();
    if (paramType->isVoid())
      return AbstractionPattern(paramType);

    // Otherwise, we're talking about the formal parameter clause.
    auto method = getObjCMethod();
    auto errorInfo = getEncodedForeignInfo();

    unsigned paramIndex = index;
    if (errorInfo.hasValue()) {
      auto errorParamIndex = errorInfo.getForeignParamIndex();

      if (!errorInfo.hasErrorParameterReplacedWithVoid()) {
        if (paramIndex >= errorParamIndex) {
          ++paramIndex;
        }
      }
    }

    return AbstractionPattern(getGenericSubstitutions(),
                              getGenericSignatureForFunctionComponent(),
                              paramType,
                      method->parameters()[paramIndex]->getType().getTypePtr());
  }
  case Kind::ClangType: {
    auto params = cast<AnyFunctionType>(getType()).getParams();
    return AbstractionPattern(getGenericSubstitutions(),
                              getGenericSignatureForFunctionComponent(),
                              params[index].getParameterType(),
                          getClangFunctionParameterType(getClangType(), index));
  }
  case Kind::OpaqueFunction:
    return getOpaque();
  case Kind::OpaqueDerivativeFunction:
    return getOpaque();
  default:
    llvm_unreachable("does not have function parameters");
  }
}

ParameterTypeFlags
AbstractionPattern::getFunctionParamFlags(unsigned index) const {
  return cast<AnyFunctionType>(getType()).getParams()[index]
           .getParameterFlags();
}

unsigned AbstractionPattern::getNumFunctionParams() const {
  return cast<AnyFunctionType>(getType()).getParams().size();
}

void AbstractionPattern::
forEachFunctionParam(AnyFunctionType::CanParamArrayRef substParams,
                     bool ignoreFinalOrigParam,
    llvm::function_ref<void(FunctionParamGenerator &param)> function) const {
  FunctionParamGenerator generator(*this, substParams, ignoreFinalOrigParam);
  for (; !generator.isFinished(); generator.advance()) {
    function(generator);
  }
  generator.finish();
}

FunctionParamGenerator::FunctionParamGenerator(
                              AbstractionPattern origFunctionType,
                              AnyFunctionType::CanParamArrayRef substParams,
                              bool ignoreFinalOrigParam)
    : origFunctionType(origFunctionType), allSubstParams(substParams) {
  origFunctionTypeIsOpaque =
    (origFunctionType.isTypeParameterOrOpaqueArchetype() ||
     origFunctionType.isOpaqueFunctionOrOpaqueDerivativeFunction());

  if (origFunctionTypeIsOpaque) {
    numOrigParams = allSubstParams.size();
  } else {
    numOrigParams = origFunctionType.getNumFunctionParams();
    if (ignoreFinalOrigParam)
      numOrigParams--;
  }

  if (!isFinished()) loadParameter();
}

static CanType getOptionalObjectType(CanType type) {
  auto objectType = type.getOptionalObjectType();
  assert(objectType && "type was not optional");
  return objectType;
}

AbstractionPattern AbstractionPattern::getOptionalObjectType() const {
  switch (getKind()) {
  case Kind::Invalid:
    llvm_unreachable("querying invalid abstraction pattern!");
  case Kind::ObjCMethodType:
  case Kind::CurriedObjCMethodType:
  case Kind::PartialCurriedObjCMethodType:
  case Kind::CFunctionAsMethodType:
  case Kind::CurriedCFunctionAsMethodType:
  case Kind::PartialCurriedCFunctionAsMethodType:
  case Kind::CXXMethodType:
  case Kind::CurriedCXXMethodType:
  case Kind::PartialCurriedCXXMethodType:
  case Kind::Tuple:
  case Kind::OpaqueFunction:
  case Kind::OpaqueDerivativeFunction:
  case Kind::ObjCCompletionHandlerArgumentsType:
    llvm_unreachable("pattern for function or tuple cannot be for optional");

  case Kind::Opaque:
    return *this;

  case Kind::Type:
    if (isTypeParameterOrOpaqueArchetype())
      return AbstractionPattern::getOpaque();
    return AbstractionPattern(getGenericSubstitutions(),
                              getGenericSignature(),
                              ::getOptionalObjectType(getType()));

  case Kind::Discard:
    return AbstractionPattern::getDiscard(getGenericSubstitutions(),
                                          getGenericSignature(),
                                          ::getOptionalObjectType(getType()));

  case Kind::ClangType:
    // This is not reflected in clang types.
    return AbstractionPattern(getGenericSubstitutions(),
                              getGenericSignature(),
                              ::getOptionalObjectType(getType()),
                              getClangType());
  }
  llvm_unreachable("bad kind");
}

AbstractionPattern AbstractionPattern::getReferenceStorageReferentType() const {
  switch (getKind()) {
  case Kind::Invalid:
    llvm_unreachable("querying invalid abstraction pattern!");
  case Kind::Opaque:
  case Kind::ObjCMethodType:
  case Kind::CurriedObjCMethodType:
  case Kind::PartialCurriedObjCMethodType:
  case Kind::CurriedCFunctionAsMethodType:
  case Kind::PartialCurriedCFunctionAsMethodType:
  case Kind::CFunctionAsMethodType:
  case Kind::CXXMethodType:
  case Kind::CurriedCXXMethodType:
  case Kind::PartialCurriedCXXMethodType:
  case Kind::Tuple:
  case Kind::OpaqueFunction:
  case Kind::OpaqueDerivativeFunction:
  case Kind::ObjCCompletionHandlerArgumentsType:
    return *this;
  case Kind::Type:
    return AbstractionPattern(getGenericSubstitutions(),
                              getGenericSignature(),
                              getType().getReferenceStorageReferent());
  case Kind::Discard:
    return AbstractionPattern::getDiscard(getGenericSubstitutions(),
                                          getGenericSignature(),
                                       getType().getReferenceStorageReferent());
  case Kind::ClangType:
    // This is not reflected in clang types.
    return AbstractionPattern(getGenericSubstitutions(),
                              getGenericSignature(),
                              getType().getReferenceStorageReferent(),
                              getClangType());
  }
  llvm_unreachable("bad kind");
}

static CanType getExistentialConstraintType(CanType type) {
  assert(type.isExistentialType());
  if (auto *ET = type->getAs<ExistentialType>()) {
    return CanType(ET->getConstraintType());
  }
  return type;
}

AbstractionPattern AbstractionPattern::getExistentialConstraintType() const {
  switch (getKind()) {
  case Kind::Invalid:
    llvm_unreachable("querying invalid abstraction pattern!");
  case Kind::ObjCMethodType:
  case Kind::CurriedObjCMethodType:
  case Kind::PartialCurriedObjCMethodType:
  case Kind::CFunctionAsMethodType:
  case Kind::CurriedCFunctionAsMethodType:
  case Kind::PartialCurriedCFunctionAsMethodType:
  case Kind::CXXMethodType:
  case Kind::CurriedCXXMethodType:
  case Kind::PartialCurriedCXXMethodType:
  case Kind::Tuple:
  case Kind::OpaqueFunction:
  case Kind::OpaqueDerivativeFunction:
  case Kind::ObjCCompletionHandlerArgumentsType:
    llvm_unreachable("pattern for function or tuple cannot be for optional");

  case Kind::Opaque:
    return *this;

  case Kind::Type:
    if (isTypeParameterOrOpaqueArchetype())
      return AbstractionPattern::getOpaque();
    return AbstractionPattern(getGenericSubstitutions(),
                              getGenericSignature(),
                              ::getExistentialConstraintType(getType()));

  case Kind::Discard:
    return AbstractionPattern::getDiscard(
        getGenericSubstitutions(), getGenericSignature(),
        ::getExistentialConstraintType(getType()));

  case Kind::ClangType:
    // This is not reflected in clang types.
    return AbstractionPattern(getGenericSubstitutions(),
                              getGenericSignature(),
                              ::getExistentialConstraintType(getType()),
                              getClangType());
  }
  llvm_unreachable("bad kind");
}

void AbstractionPattern::dump() const {
  print(llvm::errs());
  llvm::errs() << "\n";
}

static void printGenerics(raw_ostream &out, const AbstractionPattern &pattern) {
  if (auto sig = pattern.getGenericSignature()) {
    sig->print(out);
  }
  // It'd be really nice if we could get these interleaved with the types.
  if (auto subs = pattern.getGenericSubstitutions()) {
    out << "@<";
    bool first = true;
    for (auto sub : subs.getReplacementTypes()) {
      if (!first) {
        out << ",";
      } else {
        first = false;
      }
      out << sub;
    }
    out << ">";
  }
}

void AbstractionPattern::print(raw_ostream &out) const {
  switch (getKind()) {
  case Kind::Invalid:
    out << "AP::Invalid";
    return;
  case Kind::Opaque:
    out << "AP::Opaque";
    return;
  case Kind::OpaqueFunction:
    out << "AP::OpaqueFunction";
    return;
  case Kind::OpaqueDerivativeFunction:
    out << "AP::OpaqueDerivativeFunction";
    return;
  case Kind::Type:
  case Kind::Discard:
    out << (getKind() == Kind::Type
              ? "AP::Type" :
            getKind() == Kind::Discard
              ? "AP::Discard" : "<<UNHANDLED CASE>>");
    printGenerics(out, *this);
    out << '(';
    getType().dump(out);
    out << ')';
    return;
  case Kind::Tuple:
    out << "AP::Tuple(";
    for (unsigned i = 0, e = getNumTupleElements(); i != e; ++i) {
      if (i != 0) out << ", ";
      getTupleElementType(i).print(out);
    }
    out << ")";
    return;
  case Kind::ClangType:
  case Kind::CurriedCFunctionAsMethodType:
  case Kind::PartialCurriedCFunctionAsMethodType:
  case Kind::CFunctionAsMethodType:
  case Kind::ObjCCompletionHandlerArgumentsType:
    out << (getKind() == Kind::ClangType
              ? "AP::ClangType(" :
            getKind() == Kind::CurriedCFunctionAsMethodType
              ? "AP::CurriedCFunctionAsMethodType(" :
            getKind() == Kind::PartialCurriedCFunctionAsMethodType
              ? "AP::PartialCurriedCFunctionAsMethodType(" :
            getKind() == Kind::ObjCCompletionHandlerArgumentsType
              ? "AP::ObjCCompletionHandlerArgumentsType("
              : "AP::CFunctionAsMethodType(");
    printGenerics(out, *this);
    getType().dump(out);
    out << ", ";
    // [TODO: Improve-Clang-type-printing]
    // It would be better to use print, but we need a PrintingPolicy
    // for that, for which we need a clang LangOptions, and... ugh.
    clang::QualType(getClangType(), 0).dump();
    if (hasImportAsMemberStatus()) {
      out << ", member=";
      auto status = getImportAsMemberStatus();
      if (status.isInstance()) {
        out << "instance, self=" << status.getSelfIndex();
      } else if (status.isStatic()) {
        out << "static";
      }
    }
    if (hasStoredForeignInfo()) {
      if (auto errorIndex
          = getEncodedForeignInfo().getAsyncCompletionHandlerErrorParamIndex()){
        out << ", errorParamIndex=" << *errorIndex;
      }
    }
    out << ")";
    return;
  case Kind::CXXMethodType:
  case Kind::CurriedCXXMethodType:
  case Kind::PartialCurriedCXXMethodType:
    out << (getKind() == Kind::CXXMethodType
              ? "AP::CXXMethodType(" :
            getKind() == Kind::CurriedCXXMethodType
              ? "AP::CurriedCXXMethodType("
              : "AP::PartialCurriedCXXMethodType");
    printGenerics(out, *this);
    getType().dump(out);
    out << ", ";
    getCXXMethod()->dump();
    assert(!hasImportAsMemberStatus());
    out << ")";
    return;
  case Kind::CurriedObjCMethodType:
  case Kind::PartialCurriedObjCMethodType:
  case Kind::ObjCMethodType:
    out << (getKind() == Kind::ObjCMethodType
              ? "AP::ObjCMethodType(" :
            getKind() == Kind::CurriedObjCMethodType
              ? "AP::CurriedObjCMethodType("
              : "AP::PartialCurriedObjCMethodType(");
    getType().dump(out);
    auto errorInfo = getEncodedForeignInfo();
    switch (errorInfo.getKind()) {
    case EncodedForeignInfo::IsNotForeign:
      break;
    
    case EncodedForeignInfo::IsError:
      out << ", errorParameter=" << errorInfo.getErrorParamIndex();
      if (errorInfo.hasErrorParameterReplacedWithVoid())
        out << ", replacedWithVoid";
      if (errorInfo.errorStripsResultOptionality())
        out << ", stripsResultOptionality";
      break;
        
    case EncodedForeignInfo::IsAsync:
      out << ", completionHandlerParameter=" << errorInfo.getAsyncCompletionHandlerParamIndex();
      if (auto errorParam = errorInfo.getAsyncCompletionHandlerErrorParamIndex()) {
        out << " (errorParam=" << *errorParam;
        if (auto errorFlag = errorInfo.getAsyncCompletionHandlerErrorFlagParamIndex()) {
          out << ", errorFlagParam=" << *errorFlag
              << (errorInfo.isCompletionErrorFlagZeroOnError()
                    ? ", zeroOnError"
                    : ", nonzeroOnError");
        }
        out << ')';
      }
    }
    out << ", ";
    getObjCMethod()->dump(out);
    out << ")";
    return;
  }
  llvm_unreachable("bad kind");
}

bool AbstractionPattern::hasSameBasicTypeStructure(CanType l, CanType r) {
  if (l == r) return true;

  // Tuples must match.
  auto lTuple = dyn_cast<TupleType>(l);
  auto rTuple = dyn_cast<TupleType>(r);
  if (lTuple && rTuple) {
    auto lElts = lTuple.getElementTypes();
    auto rElts = rTuple.getElementTypes();
    if (lElts.size() != rElts.size())
      return false;
    for (auto i : indices(lElts)) {
      if (!hasSameBasicTypeStructure(lElts[i], rElts[i]))
        return false;
    }
    return true;
  } else if (lTuple || rTuple) {
    return false;
  }

  // Functions must match.
  auto lFunction = dyn_cast<AnyFunctionType>(l);
  auto rFunction = dyn_cast<AnyFunctionType>(r);
  if (lFunction && rFunction) {
    auto lParam = lFunction.getParams();
    auto rParam = rFunction.getParams();
    if (lParam.size() != rParam.size())
      return false;

    for (unsigned i : indices(lParam)) {
      if (!hasSameBasicTypeStructure(lParam[i].getPlainType(),
                                     rParam[i].getPlainType()))
        return false;
    }

    return hasSameBasicTypeStructure(lFunction.getResult(),
                                     rFunction.getResult());
  } else if (lFunction || rFunction) {
    return false;
  }

  // Optionals must match, sortof.
  auto lObject = l.getOptionalObjectType();
  auto rObject = r.getOptionalObjectType();
  if (lObject && rObject) {
    return hasSameBasicTypeStructure(lObject, rObject);
  } else if (lObject || rObject) {
    // Allow optionality mis-matches, but require the underlying types to match.
    return hasSameBasicTypeStructure(lObject ? lObject : l,
                                     rObject ? rObject : r);
  }

  // Otherwise, the structure is similar enough.
  return true;
}

AbstractionPattern
AbstractionPattern::unsafeGetSubstFieldType(ValueDecl *member,
                                            CanType origMemberInterfaceType,
                                            SubstitutionMap subMap)
const {
  assert(origMemberInterfaceType);
  if (isTypeParameterOrOpaqueArchetype()) {
    // Fall back to the generic abstraction pattern for the member.
    auto sig = member->getDeclContext()->getGenericSignatureOfContext();
    return AbstractionPattern(subMap, sig.getCanonicalSignature(),
                              origMemberInterfaceType);
  }

  switch (getKind()) {
  case Kind::Opaque:
    llvm_unreachable("should be handled by isTypeParameter");
  case Kind::Invalid:
    llvm_unreachable("called on invalid abstraction pattern");
  case Kind::Tuple:
    llvm_unreachable("should not have a tuple pattern matching a struct/enum "
                     "type");
  case Kind::OpaqueFunction:
    llvm_unreachable("should not have an opaque function pattern matching a "
                     "struct/enum type");
  case Kind::OpaqueDerivativeFunction:
    llvm_unreachable("should not have an opaque derivative function pattern "
                     "matching a struct/enum type");
  case Kind::ObjCCompletionHandlerArgumentsType:
    llvm_unreachable("should not have a completion handler argument pattern "
                     "matching a struct/enum type");
  case Kind::PartialCurriedObjCMethodType:
  case Kind::CurriedObjCMethodType:
  case Kind::PartialCurriedCFunctionAsMethodType:
  case Kind::CurriedCFunctionAsMethodType:
  case Kind::CFunctionAsMethodType:
  case Kind::ObjCMethodType:
  case Kind::CXXMethodType:
  case Kind::CurriedCXXMethodType:
  case Kind::PartialCurriedCXXMethodType:
  case Kind::ClangType:
  case Kind::Type:
  case Kind::Discard:
    auto memberTy = getType()->getTypeOfMember(member->getModuleContext(),
                                      member, origMemberInterfaceType)
                             ->getReducedType(getGenericSignature());
      
    return AbstractionPattern(getGenericSubstitutions(),
                              getGenericSignature(),
                              memberTy);
  }
  llvm_unreachable("invalid abstraction pattern kind");
}

AbstractionPattern AbstractionPattern::getAutoDiffDerivativeFunctionType(
    IndexSubset *parameterIndices, AutoDiffDerivativeFunctionKind kind,
    LookupConformanceFn lookupConformance,
    GenericSignature derivativeGenericSignature, bool makeSelfParamFirst) {
  switch (getKind()) {
  case Kind::Type: {
    auto fnTy = dyn_cast<AnyFunctionType>(getType());
    if (!fnTy)
      return getOpaqueDerivativeFunction();
    auto derivativeFnTy = fnTy->getAutoDiffDerivativeFunctionType(
        parameterIndices, kind, lookupConformance, derivativeGenericSignature,
        makeSelfParamFirst);
    assert(derivativeFnTy);
    return AbstractionPattern(
        getGenericSignature(),
        derivativeFnTy->getReducedType(getGenericSignature()));
  }
  case Kind::Opaque:
    return getOpaqueDerivativeFunction();
  default:
    llvm_unreachable("called on unsupported abstraction pattern kind");
  }
}

AbstractionPattern::CallingConventionKind
AbstractionPattern::getResultConvention(TypeConverter &TC) const {
  // Tuples should be destructured.
  if (isTuple()) {
    return Destructured;
  }
  switch (getKind()) {
  case Kind::Opaque:
    // Maximally abstracted values are always passed indirectly.
    return Indirect;
  
  case Kind::OpaqueFunction:
  case Kind::OpaqueDerivativeFunction:
  case Kind::PartialCurriedObjCMethodType:
  case Kind::CurriedObjCMethodType:
  case Kind::PartialCurriedCFunctionAsMethodType:
  case Kind::CurriedCFunctionAsMethodType:
  case Kind::CFunctionAsMethodType:
  case Kind::ObjCMethodType:
  case Kind::CXXMethodType:
  case Kind::CurriedCXXMethodType:
  case Kind::PartialCurriedCXXMethodType:
    // Function types are always passed directly
    return Direct;
      
  case Kind::ClangType:
  case Kind::Type:
  case Kind::Discard:
    // Pass according to the formal type.
    return SILType::isFormallyReturnedIndirectly(getType(),
                                                 TC,
                                                 getGenericSignatureOrNull())
      ? Indirect : Direct;
  
  case Kind::Invalid:
  case Kind::Tuple:
  case Kind::ObjCCompletionHandlerArgumentsType:
    llvm_unreachable("should not get here");
  }
}

AbstractionPattern::CallingConventionKind
AbstractionPattern::getParameterConvention(TypeConverter &TC) const {
  // Tuples should be destructured.
  if (isTuple()) {
    return Destructured;
  }
  switch (getKind()) {
  case Kind::Opaque:
    // Maximally abstracted values are always passed indirectly.
    return Indirect;
  
  case Kind::OpaqueFunction:
  case Kind::OpaqueDerivativeFunction:
  case Kind::PartialCurriedObjCMethodType:
  case Kind::CurriedObjCMethodType:
  case Kind::PartialCurriedCFunctionAsMethodType:
  case Kind::CurriedCFunctionAsMethodType:
  case Kind::CFunctionAsMethodType:
  case Kind::ObjCMethodType:
  case Kind::CXXMethodType:
  case Kind::CurriedCXXMethodType:
  case Kind::PartialCurriedCXXMethodType:
    // Function types are always passed directly
    return Direct;
      
  case Kind::ClangType:
  case Kind::Type:
  case Kind::Discard:
    // Pass according to the formal type.
    return SILType::isFormallyPassedIndirectly(getType(),
                                                 TC,
                                                 getGenericSignatureOrNull())
      ? Indirect : Direct;
  
  case Kind::Invalid:
  case Kind::Tuple:
  case Kind::ObjCCompletionHandlerArgumentsType:
    llvm_unreachable("should not get here");
  }
}

bool AbstractionPattern::arePackElementsPassedIndirectly(TypeConverter &TC) const {
  assert(getKind() == Kind::Type && isa<PackExpansionType>(getType()));
  // It makes sense to pass classes, metatypes, and similar sorts of
  // types using direct packs.  At the other end of the spectrum, we
  // definitely shouldn't pass types directly in packs if it's
  // address-only and we'd need to do a non-trivial operation to move
  // the value in and out of the pack.  There's also a size component
  // to this analysis --- we shouldn't pass packs of enormous address-only
  // structs directly --- and unfortunately we don't have that
  // information at this point.
  //
  // The simplest thing to do is to just not use direct packs for now,
  // but we should revisit that before locking down the ABI.
  return true;
}

bool
AbstractionPattern::operator==(const AbstractionPattern &other) const {
  if (TheKind != other.TheKind)
    return false;
  
  switch (getKind()) {
  case Kind::Opaque:
  case Kind::Invalid:
  case Kind::OpaqueFunction:
  case Kind::OpaqueDerivativeFunction:
    // No additional info to compare.
    return true;

  case Kind::Tuple:
    if (getNumTupleElements() != other.getNumTupleElements()) {
      return false;
    }
    for (unsigned i = 0; i < getNumTupleElements(); ++i) {
      if (getTupleElementType(i) != other.getTupleElementType(i)) {
        return false;
      }
    }
    return true;

  case Kind::Type:
  case Kind::Discard:
    return OrigType == other.OrigType
      && GenericSig == other.GenericSig;
      
  case Kind::ClangType:
    return OrigType == other.OrigType
      && GenericSig == other.GenericSig
      && ClangType == other.ClangType;

  case Kind::ObjCCompletionHandlerArgumentsType:
  case Kind::CFunctionAsMethodType:
  case Kind::CurriedCFunctionAsMethodType:
  case Kind::PartialCurriedCFunctionAsMethodType:
    return OrigType == other.OrigType
      && GenericSig == other.GenericSig
      && ClangType == other.ClangType
      && OtherData == other.OtherData;

  case Kind::ObjCMethodType:
  case Kind::CurriedObjCMethodType:
  case Kind::PartialCurriedObjCMethodType:
    return OrigType == other.OrigType
      && GenericSig == other.GenericSig
      && ObjCMethod == other.ObjCMethod
      && OtherData == other.OtherData;
      
  case Kind::CXXMethodType:
  case Kind::CurriedCXXMethodType:
  case Kind::PartialCurriedCXXMethodType:
    return OrigType == other.OrigType
      && GenericSig == other.GenericSig
      && CXXMethod == other.CXXMethod
      && OtherData == other.OtherData;
  }
}

namespace {
class SubstFunctionTypePatternVisitor
  : public CanTypeVisitor<SubstFunctionTypePatternVisitor, CanType,
                          AbstractionPattern>
{
public:
  TypeConverter &TC;
  SmallVector<GenericTypeParamType *, 2> substGenericParams;
  SmallVector<Requirement, 2> substRequirements;
  SmallVector<Type, 2> substReplacementTypes;
  CanType substYieldType;
  unsigned packExpansionLevel;
  
  SubstFunctionTypePatternVisitor(TypeConverter &TC)
    : TC(TC), packExpansionLevel(0) {}

  // Creates and returns a fresh type parameter in the substituted generic
  // signature if `pattern` is a type parameter or opaque archetype. Returns
  // null otherwise.
  CanType handleTypeParameter(AbstractionPattern pattern, CanType substTy,
                              unsigned level) {
    if (!pattern.isTypeParameterOrOpaqueArchetype())
      return CanType();

    unsigned paramIndex = substGenericParams.size();

    bool withinExpansion = (level < packExpansionLevel);

    // Pack parameters that aren't within expansions should just be
    // abstracted as scalars.
    bool isParameterPack = (withinExpansion && pattern.isTypeParameterPack());

    auto gp = GenericTypeParamType::get(isParameterPack, 0, paramIndex,
                                        TC.Context);
    substGenericParams.push_back(gp);

    CanType replacement;

    if (withinExpansion) {
      // If we're within an expansion, and there are substitutions in the
      // abstraction pattern, use those instead of substTy.  substTy is not
      // contextually meaningful in this case; see handlePackExpansion.
      if (auto subs = pattern.getGenericSubstitutions()) {
        replacement = pattern.getType().subst(subs)->getCanonicalType();

      // If we don't have substitutions, but we're abstracting a pack
      // parameter, assume that we're lowering a function type using
      // itself as its pattern or something like.  The substituted type
      // should be `each T` for some pack reference; wrap that in a pack.
      } else if (isParameterPack) {
        replacement = CanPackType::getSingletonPackExpansion(substTy);

      // Otherwise, just use substTy.
      } else {
        replacement = substTy;
      }

    // Otherwise, we can just use substTy.
    } else {
      assert(!isParameterPack);
      assert(!isa<PackType>(substTy));
      replacement = substTy;
    }
    substReplacementTypes.push_back(replacement);

    if (auto layout = pattern.getLayoutConstraint()) {
      // Look at the layout constraint on this position in the abstraction pattern
      // and carry it over, with some generalization to the point it affects
      // calling convention.
      // TODO: We should do this once we surface more interesting layout
      // constraints in the language. There are several places in type lowering
      // that need to be changed to allow for this and generate correct calling
      // convention lowering.
#if WE_MAKE_LAYOUT_CONSTRAINTS_AVAILABLE_IN_THE_SURFACE_LANGUAGE
      switch (layout->getKind()) {
      // Keep these layout constraints as is.
      case LayoutConstraintKind::RefCountedObject:
      case LayoutConstraintKind::TrivialOfAtMostSize:
        break;
      
      case LayoutConstraintKind::UnknownLayout:
      case LayoutConstraintKind::Trivial:
        // These constraints don't really constrain the ABI, so we can
        // eliminate them.
        layout = LayoutConstraint();
        break;
    
      // Replace these specific constraints with one of the more general
      // constraints above.
      case LayoutConstraintKind::NativeClass:
      case LayoutConstraintKind::Class:
      case LayoutConstraintKind::NativeRefCountedObject:
        // These can all be generalized to RefCountedObject.
        layout = LayoutConstraint::getLayoutConstraint(
                                   LayoutConstraintKind::RefCountedObject);
        break;
          
      case LayoutConstraintKind::TrivialOfExactSize:
        // Generalize to TrivialOfAtMostSize.
        layout = LayoutConstraint::getLayoutConstraint(
           LayoutConstraintKind::TrivialOfAtMostSize,
           layout->getTrivialSizeInBits(),
           layout->getAlignmentInBits(),
           C);
        break;
      }
#endif
      
      if (layout) {
        substRequirements.push_back(
                      Requirement(RequirementKind::Layout, gp, layout));
      }
    }
    
    if (level > 0)
      return CanType(PackElementType::get(gp, level));

    return CanType(gp);
  }

  CanType visit(CanType t, AbstractionPattern pattern) {
    unsigned level = 0;
    if (auto elementType = dyn_cast<PackElementType>(t)) {
      level = elementType->getLevel();
      t = elementType.getPackType();
      pattern = pattern.getPackElementPackType();
    }

    if (auto gp = handleTypeParameter(pattern, t, level))
      return gp;

    if (pattern.isTuple())
      return visitTuplePattern(t, pattern);

    return CanTypeVisitor::visit(t, pattern);
  }

  CanType visitType(CanType t, AbstractionPattern pattern) {
    assert(pattern.getType()->isExistentialType() ||
           (!pattern.getType()->hasTypeParameter()
           && !pattern.getType()->hasArchetype()
           && !pattern.getType()->hasOpaqueArchetype()));
    return pattern.getType();
  }
  
  CanType visitDynamicSelfType(CanDynamicSelfType dst,
                               AbstractionPattern pattern) {
    // A "dynamic self" type can be bound to another dynamic self type, or the
    // non-dynamic base class type.
    if (isa<DynamicSelfType>(pattern.getType())) {
      auto origSelf = pattern.getDynamicSelfSelfType();
      auto newBase = visit(dst.getSelfType(), origSelf);
      return DynamicSelfType::get(newBase, TC.Context)
        ->getCanonicalType();
    }
    
    return visit(dst.getSelfType(), pattern);
  }
  
  CanType visitAnyMetatypeType(CanAnyMetatypeType mt, AbstractionPattern pattern){
    auto substInstance = visit(mt.getInstanceType(),
                               pattern.getMetatypeInstanceType());
    
    // The CanType cast is required for this to type-check because
    // C++'s ?: operator doesn't look for common superclasses.
    return isa<ExistentialMetatypeType>(mt)
      ? CanExistentialMetatypeType::get(substInstance)
      : CanType(CanMetatypeType::get(substInstance));
  }
  
  CanType handleGenericNominalType(AbstractionPattern origPattern, CanType subst) {
    CanType orig = origPattern.getType();
    CanGenericSignature origSig = origPattern.getGenericSignatureOrNull();
    auto origPatternSubs = origPattern.getGenericSubstitutions();

    // If there are no loose type parameters in the pattern here, we don't need
    // to do a recursive visit at all.
    if (!orig->hasTypeParameter()
        && !orig->hasArchetype()
        && !orig->hasOpaqueArchetype()) {
      return subst;
    }

    // If the substituted type is a subclass of the abstraction pattern
    // type, use the substituted type for the abstraction pattern. This only
    // comes up when lowering override types for vtable entries.
    auto getDifferentBaseClass = [](Type substInstance, Type origInstance) -> ClassDecl* {
      if (auto dynA = substInstance->getAs<DynamicSelfType>()) {
        substInstance = dynA->getSelfType();
      }
      if (auto dynB = origInstance->getAs<DynamicSelfType>()) {
        origInstance = dynB->getSelfType();
      }
      if (auto aClass = substInstance->getClassOrBoundGenericClass()) {
        if (auto bClass = origInstance->getClassOrBoundGenericClass()) {
          if (aClass != bClass) {
            return bClass;
          }
        }
      }
      
      return nullptr;
    };
    
    // Both instance and class methods can be overridden; check for metatype
    // subtyping too.
    ClassDecl *differentOrigClass = getDifferentBaseClass(subst, orig);
    if (!differentOrigClass) {
      if (auto substMeta = subst->getAs<MetatypeType>()) {
        if (auto origMeta = dyn_cast<MetatypeType>(orig)) {
          differentOrigClass = getDifferentBaseClass(substMeta->getInstanceType(),
                                                     origMeta->getInstanceType());
        }
      }
    }
    
    if (differentOrigClass) {
      orig = subst;
      origSig = TC.getCurGenericSignature();
      origPatternSubs = SubstitutionMap();
      assert((!subst->hasTypeParameter() || origSig) &&
             "lowering mismatched interface types in a context without "
             "a generic signature");
    }
    
    auto decl = orig->getAnyNominal();

    auto moduleDecl = decl->getParentModule();
    auto origSubMap = orig->getContextSubstitutionMap(moduleDecl, decl);
    auto substSubMap = subst->getContextSubstitutionMap(moduleDecl, decl);
    
    auto nomGenericSig = decl->getGenericSignature();
    
    TypeSubstitutionMap replacementTypes;
    for (auto gp : nomGenericSig.getGenericParams()) {
      auto origParamTy = Type(gp).subst(origSubMap)
        ->getCanonicalType();
      auto substParamTy = Type(gp).subst(substSubMap)
        ->getCanonicalType();

      replacementTypes[gp->getCanonicalType()->castTo<SubstitutableType>()]
          = visit(substParamTy,
                  AbstractionPattern(origPatternSubs, origSig, origParamTy));
    }

    auto newSubMap = SubstitutionMap::get(nomGenericSig,
      QueryTypeSubstitutionMap{replacementTypes},
      LookUpConformanceInModule(moduleDecl));
    
    for (auto reqt : nomGenericSig.getRequirements()) {
      substRequirements.push_back(reqt.subst(newSubMap));
    }
    
    return decl->getDeclaredInterfaceType().subst(newSubMap)->getCanonicalType();
  }
  
  CanType visitNominalType(CanNominalType nom, AbstractionPattern pattern) {
    auto nomDecl = nom->getDecl();
    
    // If the type is generic (because it's a nested type in a generic context),
    // process the generic type bindings.
    if (!isa<ProtocolDecl>(nomDecl) && nomDecl->isGenericContext()) {
      return handleGenericNominalType(pattern, nom);
    }
    
    // Otherwise, there are no structural type parameters to visit.
    return nom;
  }
  
  CanType visitBoundGenericType(CanBoundGenericType bgt,
                                AbstractionPattern pattern) {
    return handleGenericNominalType(pattern, bgt);
  }

  CanType visitPackType(CanPackType pack, AbstractionPattern pattern) {
    // Break down the pack.
    SmallVector<CanType, 4> packElts;
    for (auto i : range(pack->getNumElements())) {
      packElts.push_back(visit(pack.getElementType(i),
                               pattern.getPackElementType(i)));
    }

    return CanPackType::get(TC.Context, packElts);
  }

  CanType visitPackExpansionType(CanPackExpansionType pack,
                                 AbstractionPattern pattern) {
    llvm_unreachable("shouldn't encounter pack expansion by itself");
  }

  CanType visitPackElementType(CanPackElementType packElement,
                               AbstractionPattern pattern) {
    llvm_unreachable("shouldn't encounter pack element by itself");
  }

  CanType handlePackExpansion(AbstractionPattern origExpansion,
                              CanType candidateSubstType) {
    // When we're within a pack expansion, pack references matching that
    // expansion should be abstracted as packs.  The substitution will be
    // the pack substitution for that parameter recorded in the pattern.

    // Remember that we're within an expansion.
    ++packExpansionLevel;

    SWIFT_DEFER {
      --packExpansionLevel;
    };

    auto origPatternType = origExpansion.getPackExpansionPatternType();

    // We only really need a subst type here if we don't have
    // substitutions in the pattern, because handleTypeParameter
    // will always those substitutions within an expansion if
    // they're available.  And if we don't have substitutions in the
    // pattern, we can't map the pack expansion to a concrete set
    // of expanded components, so we should have exactly one subst
    // type.
    CanType substPatternType;
    if (origExpansion.getGenericSubstitutions()) {
      substPatternType = origPatternType.getType();
    } else {
      assert(candidateSubstType);
      substPatternType =
        cast<PackExpansionType>(candidateSubstType).getPatternType();
    }

    // Recursively visit the pattern type.
    auto patternTy = visit(substPatternType, origPatternType);

    // Find a pack parameter from the pattern to expand over.
    CanType countParam;
    patternTy->walkPackReferences([&](Type t) {
      if (t->isTypeParameter()) {
        auto param = t->getRootGenericParam();
        if (param->isParameterPack()) {
          countParam = CanType(param);
          return true;
        }
      }

      return false;
    });

    // If that didn't work, we should be able to find an expansion
    // to use from either the substituted type or the subs.  At worst,
    // we can make one.
    assert(countParam && "implementable but lazy");

    return CanPackExpansionType::get(patternTy, countParam);
  }

  CanType visitExistentialType(CanExistentialType exist,
                               AbstractionPattern pattern) {
    // Avoid walking into the constraint type if we can help it.
    if (!exist->hasTypeParameter() && !exist->hasArchetype() &&
        !exist->hasOpaqueArchetype()) {
      return CanType(exist);
    }

    return CanExistentialType::get(visit(
        exist.getConstraintType(), pattern.getExistentialConstraintType()));
  }

  CanType visitParameterizedProtocolType(CanParameterizedProtocolType ppt,
                                         AbstractionPattern pattern) {
    // Recurse into the arguments of the parameterized protocol.
    SmallVector<Type, 4> substArgs;
    auto origPPT = pattern.getAs<ParameterizedProtocolType>();
    if (!origPPT)
      return ppt;
    
    for (unsigned i = 0; i < ppt->getArgs().size(); ++i) {
      auto argTy = ppt.getArgs()[i];
      auto origArgTy = pattern.getParameterizedProtocolArgType(i);
      auto substEltTy = visit(argTy, origArgTy);
      substArgs.push_back(substEltTy);
    }

    return CanType(ParameterizedProtocolType::get(
        TC.Context, ppt->getBaseType(), substArgs));
  }

  /// Visit a tuple pattern.  Note that, because of vanishing tuples,
  /// we can't handle this case by matching a tuple type in the
  /// substituted type; we have to check for a tuple pattern in the
  /// top-level visit routine.
  CanType visitTuplePattern(CanType substType, AbstractionPattern pattern) {
    assert(pattern.isTuple());

    SmallVector<TupleTypeElt, 4> tupleElts;
    pattern.forEachTupleElement(substType, [&](TupleElementGenerator &elt) {
      auto substEltTypes = elt.getSubstTypes();
      CanType eltTy;
      if (!elt.isOrigPackExpansion()) {
        eltTy = visit(substEltTypes[0], elt.getOrigType());
      } else {
        CanType candidateSubstType;
        if (!substEltTypes.empty())
          candidateSubstType = substEltTypes[0];
        eltTy = handlePackExpansion(elt.getOrigType(), candidateSubstType);
      }
      tupleElts.push_back(elt.getOrigElement().getWithType(eltTy));
    });
    
    return CanType(TupleType::get(tupleElts, TC.Context));
  }
  
  CanType handleUnabstractedFunctionType(CanAnyFunctionType func,
                                         AbstractionPattern pattern,
                                         CanType yieldType,
                                         AbstractionPattern yieldPattern) {
    SmallVector<FunctionType::Param, 4> newParams;
    auto addParam = [&](ParameterTypeFlags oldFlags, CanType newType) {
      newParams.push_back(FunctionType::Param(
          newType, /*label*/ Identifier(), oldFlags.withVariadic(false),
          /*internal label*/ Identifier()));
    };

    pattern.forEachFunctionParam(func.getParams(), /*ignore self*/ false,
                                 [&](FunctionParamGenerator &param) {
      if (!param.isOrigPackExpansion()) {
        auto newParamTy = visit(param.getSubstParams()[0].getParameterType(),
                                param.getOrigType());
        addParam(param.getOrigFlags(), newParamTy);
      } else {
        auto substParams = param.getSubstParams();
        CanType candidateSubstType;
        if (!substParams.empty())
          candidateSubstType = substParams[0].getParameterType();
        auto expansionType =
          handlePackExpansion(param.getOrigType(), candidateSubstType);
        addParam(param.getOrigFlags(), expansionType);
      }
    });
    
    if (yieldType) {
      substYieldType = visit(yieldType, yieldPattern);
    }
    
    auto newResultTy = visit(func.getResult(),
                             pattern.getFunctionResultType());

    llvm::Optional<FunctionType::ExtInfo> extInfo;
    if (func->hasExtInfo())
      extInfo = func->getExtInfo();
    
    return CanFunctionType::get(FunctionType::CanParamArrayRef(newParams),
                                newResultTy, extInfo);
  }
  
  CanType visitFunctionType(CanFunctionType func,
                            AbstractionPattern pattern) {
    return handleUnabstractedFunctionType(func, pattern,
                                          CanType(),
                                          AbstractionPattern::getInvalid());
  }
};
}

std::tuple<AbstractionPattern, SubstitutionMap, AbstractionPattern>
AbstractionPattern::getSubstFunctionTypePattern(CanAnyFunctionType substType,
                                                TypeConverter &TC,
                                                AbstractionPattern origYieldType,
                                                CanType substYieldType)
const {
  // If this abstraction pattern isn't meaningfully generic, then we don't
  // need to do any transformation.
  if (!isTypeParameterOrOpaqueArchetype()
      && !isOpaqueFunctionOrOpaqueDerivativeFunction()
      && !getType()->hasArchetype()
      && !getType()->hasOpaqueArchetype()
      && !getType()->hasTypeParameter()
      && !isa<GenericFunctionType>(getType())) {
    return std::make_tuple(
            AbstractionPattern(TC.getCurGenericSignature(), substType),
            SubstitutionMap(),
            substYieldType
              ? AbstractionPattern(TC.getCurGenericSignature(), substYieldType)
              : AbstractionPattern::getInvalid());
  }

  SubstFunctionTypePatternVisitor visitor(TC);
  auto substTy = visitor.handleUnabstractedFunctionType(substType, *this,
                                                        substYieldType,
                                                        origYieldType);
  
  auto substSig = buildGenericSignature(TC.Context, GenericSignature(),
                                        std::move(visitor.substGenericParams),
                                        std::move(visitor.substRequirements))
    .getCanonicalSignature();
  
  auto subMap = SubstitutionMap::get(substSig,
    [&](SubstitutableType *dependentType) -> Type {
      auto index = cast<GenericTypeParamType>(dependentType)->getIndex();
      return visitor.substReplacementTypes[index];
    }, [&](CanType dependentType,
           Type conformingReplacementType,
           ProtocolDecl *conformedProtocol) -> ProtocolConformanceRef {
      // TODO: Should have collected the conformances used in the original
      // type.
      if (conformingReplacementType->isTypeParameter())
        return ProtocolConformanceRef(conformedProtocol);
    
      return TC.M.lookupConformance(conformingReplacementType, conformedProtocol,
                                    /*allowMissing*/ true);
    });

  auto yieldType = visitor.substYieldType;
  if (yieldType)
    yieldType = yieldType->getReducedType(substSig);
  
  // Note that we specifically do not want to put subMap in the
  // abstraction patterns here, because the types we will be lowering
  // against them will not be substituted.
  return std::make_tuple(
          AbstractionPattern(substSig, substTy->getReducedType(substSig)),
          subMap,
          yieldType
            ? AbstractionPattern(substSig, yieldType)
            : AbstractionPattern::getInvalid());
}
