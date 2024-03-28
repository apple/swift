//===-- TypeCheckDistributed.h - Distributed actor typechecking -*- C++ -*-===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2021 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//
//
// This file provides type checking support for Swift's distributed actor model.
//
//===----------------------------------------------------------------------===//

#ifndef SWIFT_SEMA_TYPECHECKDISTRIBUTED_H
#define SWIFT_SEMA_TYPECHECKDISTRIBUTED_H

#include "swift/AST/ConcreteDeclRef.h"
#include "swift/AST/DiagnosticEngine.h"
#include "swift/AST/Type.h"

namespace swift {

class ClassDecl;
class ConstructorDecl;
class Decl;
class DeclContext;
class FuncDecl;
class NominalTypeDecl;

/******************************************************************************/
/********************* Distributed Actor Type Checking ************************/
/******************************************************************************/

// Diagnose an error if the Distributed module is not loaded.
bool ensureDistributedModuleLoaded(Decl *decl);

/// Check for illegal property declarations (e.g. re-declaring transport or id)
void checkDistributedActorProperties(const NominalTypeDecl *decl);

/// Type-check additional ad-hoc protocol requirements.
/// Ad-hoc requirements are protocol requirements currently not expressible
/// in the Swift type-system.
bool checkDistributedActorSystemAdHocProtocolRequirements(
    ASTContext &Context,
    ProtocolDecl *Proto,
    NormalProtocolConformance *Conformance,
    Type Adoptee,
    bool diagnose);

/// Check 'DistributedActorSystem' implementations for additional restrictions.
bool checkDistributedActorSystem(const NominalTypeDecl *system);

/// Typecheck a distributed method declaration
bool checkDistributedFunction(AbstractFunctionDecl *decl,
                              ClassDecl *inSpecificActor);

inline bool checkDistributedFunction(AbstractFunctionDecl *decl) {
  return checkDistributedFunction(decl, /*inSpecificActor=*/nullptr);
}

/// Typecheck a distributed computed (get-only) property declaration.
/// They are effectively checked the same way as argument-less methods.
bool checkDistributedActorProperty(VarDecl *decl, bool diagnose);

/// Diagnose a distributed func declaration in a not-distributed actor protocol.
void diagnoseDistributedFunctionInNonDistributedActorProtocol(
  const ProtocolDecl *proto, InFlightDiagnostic &diag);

/// Emit a FixIt suggesting to add Codable to the nominal type.
void addCodableFixIt(const NominalTypeDecl *nominal, InFlightDiagnostic &diag);

}

#endif /* SWIFT_SEMA_TYPECHECKDISTRIBUTED_H */
