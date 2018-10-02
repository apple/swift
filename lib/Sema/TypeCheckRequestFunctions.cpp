//===--- TypeCheckRequests.cpp - Type Checking Requests ------------------===//
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
#include "GenericTypeResolver.h"
#include "TypeChecker.h"
#include "swift/AST/TypeCheckRequests.h"
#include "swift/AST/Decl.h"
#include "swift/AST/ExistentialLayout.h"
#include "swift/AST/TypeLoc.h"
#include "swift/AST/Types.h"
#include "swift/Subsystems.h"

using namespace swift;

llvm::Expected<Type>
InheritedTypeRequest::evaluate(
  Evaluator &evaluator, llvm::PointerUnion<TypeDecl *, ExtensionDecl *> decl,
  unsigned index) const {
  // Figure out how to resolve types.
  TypeResolutionOptions options = None;
  DeclContext *dc;
  if (auto typeDecl = decl.dyn_cast<TypeDecl *>()) {
    if (auto nominal = dyn_cast<NominalTypeDecl>(typeDecl)) {
      dc = nominal;
      options |= TypeResolutionFlags::AllowUnavailableProtocol;
    } else {
      dc = typeDecl->getDeclContext();
    }
  } else {
    auto ext = decl.get<ExtensionDecl *>();
    dc = ext;
    options |= TypeResolutionFlags::AllowUnavailableProtocol;
  }

  DependentGenericTypeResolver protoResolver;
  GenericTypeToArchetypeResolver archetypeResolver(dc);
  GenericTypeResolver *resolver;
  if (isa<ProtocolDecl>(dc)) {
    resolver = &protoResolver;
  } else {
    resolver = &archetypeResolver;
  }

  auto lazyResolver = dc->getASTContext().getLazyResolver();
  assert(lazyResolver && "Cannot resolve inherited type at this point");

  TypeChecker &tc = *static_cast<TypeChecker *>(lazyResolver);
  TypeLoc &typeLoc = getTypeLoc(decl, index);

  Type inheritedType =
    tc.resolveType(typeLoc.getTypeRepr(), dc, options, resolver);
  if (inheritedType && !isa<ProtocolDecl>(dc))
    inheritedType = inheritedType->mapTypeOutOfContext();
  return inheritedType ? inheritedType : ErrorType::get(tc.Context);
}

llvm::Expected<Type>
SuperclassTypeRequest::evaluate(Evaluator &evaluator,
                                NominalTypeDecl *nominalDecl) const {
  assert(isa<ClassDecl>(nominalDecl) || isa<ProtocolDecl>(nominalDecl));

  for (unsigned int idx : indices(nominalDecl->getInherited())) {
    auto result = evaluator(InheritedTypeRequest{nominalDecl, idx});

    if (auto err = result.takeError()) {
      // FIXME: Should this just return once a cycle is detected?
      llvm::handleAllErrors(std::move(err),
        [](const CyclicalRequestError<InheritedTypeRequest> &E) {
          /* cycle detected */
        });
      continue;
    }

    Type inheritedType = *result;
    if (!inheritedType) continue;

    // If we found a class, return it.
    if (inheritedType->getClassOrBoundGenericClass()) {
      if (inheritedType->hasArchetype())
        return inheritedType->mapTypeOutOfContext();

      return inheritedType;
    }

    // If we found an existential with a superclass bound, return it.
    if (inheritedType->isExistentialType()) {
      if (auto superclassType =
            inheritedType->getExistentialLayout().explicitSuperclass) {
        if (superclassType->getClassOrBoundGenericClass()) {
          if (superclassType->hasArchetype())
            return superclassType->mapTypeOutOfContext();

          return superclassType;
        }
      }
    }
  }

  // No superclass.
  return Type();
}

llvm::Expected<Type>
EnumRawTypeRequest::evaluate(Evaluator &evaluator, EnumDecl *enumDecl) const {
  for (unsigned int idx : indices(enumDecl->getInherited())) {
    auto inheritedTypeResult = evaluator(InheritedTypeRequest{enumDecl, idx});
    
    if (auto err = inheritedTypeResult.takeError()) {
      llvm::handleAllErrors(std::move(err),
        [](const CyclicalRequestError<InheritedTypeRequest> &E) {
          // cycle detected
        });
      continue;
    }

    auto &inheritedType = *inheritedTypeResult;
    if (!inheritedType) continue;

    // Skip existential types.
    if (inheritedType->isExistentialType()) continue;

    // We found a raw type; return it.
    if (inheritedType->hasArchetype())
      return inheritedType->mapTypeOutOfContext();

    return inheritedType;
  }

  // No raw type.
  return Type();
}

// Define request evaluation functions for each of the type checker requests.
static AbstractRequestFunction *typeCheckerRequestFunctions[] = {
#define SWIFT_TYPEID(Name)                                    \
  reinterpret_cast<AbstractRequestFunction *>(&Name::evaluateRequest),
#include "swift/AST/TypeCheckerTypeIDZone.def"
#undef SWIFT_TYPEID
};

void swift::registerTypeCheckerRequestFunctions(Evaluator &evaluator) {
  evaluator.registerRequestFunctions(SWIFT_TYPE_CHECKER_REQUESTS_TYPEID_ZONE,
                                     typeCheckerRequestFunctions);
}
