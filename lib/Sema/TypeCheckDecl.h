//===--- TypeCheckDecl.h ----------------------------------------*- C++ -*-===//
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
//  This file defines a typechecker-internal interface to a bunch of
//  routines for semantic checking of declaration.
//
//===----------------------------------------------------------------------===//

#ifndef SWIFT_TYPECHECKING_TYPECHECKDECL_H
#define SWIFT_TYPECHECKING_TYPECHECKDECL_H

#include "llvm/ADT/Optional.h"

namespace swift {

class ASTContext;
class DeclContext;
class ValueDecl;
class Pattern;
class ConstructorDecl;
class EnumDecl;
class SourceFile;
class PrecedenceGroupDecl;
class ParameterList;

/// Walks up the override chain for \p CD until it finds an initializer that is
/// required and non-implicit. If no such initializer exists, returns the
/// declaration where \c required was introduced (i.e. closest to the root
/// class).
const ConstructorDecl *findNonImplicitRequiredInit(const ConstructorDecl *CD);

// Implemented in TypeCheckDeclOverride.cpp
bool checkOverrides(ValueDecl *decl);
void checkImplementationOnlyOverride(const ValueDecl *VD);

// Implemented in TypeCheckStorage.cpp
void setBoundVarsTypeError(Pattern *pattern, ASTContext &ctx);


/// How to generate the raw value for each element of an enum that doesn't
/// have one explicitly specified.
enum class AutomaticEnumValueKind {
  /// Raw values cannot be automatically generated.
  None,
  /// The raw value is the enum element's name.
  String,
  /// The raw value is the previous element's raw value, incremented.
  ///
  /// For the first element in the enum, the raw value is 0.
  Integer,
};

llvm::Optional<AutomaticEnumValueKind>
computeAutomaticEnumValueKind(EnumDecl *ED);

void validatePrecedenceGroup(PrecedenceGroupDecl *PGD);

void diagnoseAttrsAddedByAccessNote(SourceFile &SF);

void checkVariadicParameters(ParameterList *params, DeclContext *dc);

}

#endif
