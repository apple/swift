//===--- NameLookup.cpp - Name lookup utilities ---------------------------===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2021 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//

#include "NameLookup.h"
#include "swift/AST/Decl.h"
#include "swift/AST/GenericEnvironment.h"
#include "swift/AST/Module.h"
#include "swift/AST/Types.h"
#include "llvm/ADT/SmallVector.h"
#include <algorithm>

using namespace swift;
using namespace rewriting;

void
swift::rewriting::lookupConcreteNestedType(
    Type baseType,
    Identifier name,
    SmallVectorImpl<TypeDecl *> &concreteDecls) {
  if (auto *decl = baseType->getAnyNominal())
    lookupConcreteNestedType(decl, name, concreteDecls);
  else if (auto *archetype = baseType->getAs<OpaqueTypeArchetypeType>()) {
    // If our concrete type is an opaque result archetype, look into its
    // generic environment recursively.
    auto *genericEnv = archetype->getGenericEnvironment();
    auto genericSig = genericEnv->getGenericSignature();

    auto *typeDecl =
        genericSig->lookupNestedType(archetype->getInterfaceType(), name);
    if (typeDecl != nullptr)
      concreteDecls.push_back(typeDecl);
  }
}

void
swift::rewriting::lookupConcreteNestedType(
    NominalTypeDecl *decl,
    Identifier name,
    SmallVectorImpl<TypeDecl *> &concreteDecls) {
  SmallVector<ValueDecl *, 2> foundMembers;
  decl->getParentModule()->lookupQualified(
      decl, DeclNameRef(name), decl->getLoc(),
      NL_QualifiedDefault | NL_OnlyTypes | NL_ProtocolMembers,
      foundMembers);
  for (auto member : foundMembers)
    concreteDecls.push_back(cast<TypeDecl>(member));
}

TypeDecl *
swift::rewriting::findBestConcreteNestedType(
    SmallVectorImpl<TypeDecl *> &concreteDecls) {
  if (concreteDecls.empty())
    return nullptr;

  return *std::min_element(concreteDecls.begin(), concreteDecls.end(),
                           [](TypeDecl *type1, TypeDecl *type2) {
                             return TypeDecl::compare(type1, type2) < 0;
                           });
}
