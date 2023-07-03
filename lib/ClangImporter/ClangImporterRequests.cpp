//===--- ClangImporterRequests.cpp - Clang Importer Requests --------------------===//
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

#include "swift/AST/NameLookupRequests.h"
#include "swift/AST/ASTContext.h"
#include "swift/AST/Decl.h"
#include "swift/AST/Evaluator.h"
#include "swift/AST/Module.h"
#include "swift/AST/SourceFile.h"
#include "swift/ClangImporter/ClangImporterRequests.h"
#include "swift/Subsystems.h"

using namespace swift;

llvm::Optional<ObjCInterfaceAndImplementation>
ObjCInterfaceAndImplementationRequest::getCachedResult() const {
  auto passedDecl = std::get<0>(getStorage());
  if (!passedDecl)
    return {};

  auto cachedDecl = passedDecl->getCachedObjCImplementationDecl();

  // !cachedDecl means that no decl has been cached and we need to evaluate the
  // request.
  if (!cachedDecl)
    return llvm::None;

  // nullptr cachedDecl means that the lack of a decl was cached.
  else if (!*cachedDecl)
    return {};

  // A decl was cached! Arbitrarily guess that we looked up the implementation
  // from the interface.
  ObjCInterfaceAndImplementation result{passedDecl, *cachedDecl};

  // An imported decl can only be an interface; a native decl can only be an
  // implementation. If `implementationDecl` has a Clang node, we must have
  // looked up the interface from the implementation.
  if (result.implementationDecl->hasClangNode())
    std::swap(result.interfaceDecl, result.implementationDecl);

  return result;
}

void ObjCInterfaceAndImplementationRequest::
cacheResult(ObjCInterfaceAndImplementation value) const {
  Decl *decl = std::get<0>(getStorage());

  if (!value) {
    // `decl` is neither an interface nor an implementation.
    decl->setCachedObjCImplementationDecl(nullptr);
    return;
  }

  // Cache computed pointers from implementations to interfaces.
  value.interfaceDecl->
      setCachedObjCImplementationDecl(value.implementationDecl);
  value.implementationDecl->
      setCachedObjCImplementationDecl(value.interfaceDecl);

  // If this was a duplicate implementation, cache a null so we don't recompute.
  if (decl != value.interfaceDecl && decl != value.implementationDecl)
    decl->setCachedObjCImplementationDecl(nullptr);
}

// Define request evaluation functions for each of the name lookup requests.
static AbstractRequestFunction *clangImporterRequestFunctions[] = {
#define SWIFT_REQUEST(Zone, Name, Sig, Caching, LocOptions)                    \
  reinterpret_cast<AbstractRequestFunction *>(&Name::evaluateRequest),
#include "swift/ClangImporter/ClangImporterTypeIDZone.def"
#undef SWIFT_REQUEST
};

void swift::registerClangImporterRequestFunctions(Evaluator &evaluator) {
  evaluator.registerRequestFunctions(Zone::ClangImporter,
                                     clangImporterRequestFunctions);
}

