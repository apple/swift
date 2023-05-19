//===--- CodeSynthesis.h - Typechecker code synthesis -----------*- C++ -*-===//
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
//  routines for synthesizing various declarations.
//
//===----------------------------------------------------------------------===//

#ifndef SWIFT_TYPECHECKING_CODESYNTHESIS_H
#define SWIFT_TYPECHECKING_CODESYNTHESIS_H

#include "swift/AST/ASTWalker.h"
#include "swift/AST/ForeignErrorConvention.h"
#include "swift/Basic/ExternalUnion.h"
#include "swift/Basic/LLVM.h"
#include "llvm/ADT/Optional.h"

namespace swift {

class AbstractFunctionDecl;
class AbstractStorageDecl;
class Argument;
class ArgumentList;
class ASTContext;
class ClassDecl;
class ConstructorDecl;
class FuncDecl;
class GenericParamList;
class NominalTypeDecl;
class ObjCReason;
class ParamDecl;
class Type;
class ValueDecl;
class VarDecl;

enum class SelfAccessorKind {
  /// We're building a derived accessor on top of whatever this
  /// class provides.
  Peer,

  /// We're building a setter or something around an underlying
  /// implementation, which might be storage or inherited from a
  /// superclass.
  Super,
};

/// Builds a reference to the \c self decl in a function.
///
/// \param selfDecl The self decl to reference.
/// \param selfAccessorKind The kind of access being performed.
/// \param isLValue Whether the resulting expression is an lvalue.
/// \param convertTy The type of the resulting expression. For a reference to
/// super, this can be a superclass type to upcast to.
Expr *buildSelfReference(VarDecl *selfDecl, SelfAccessorKind selfAccessorKind,
                         bool isLValue, Type convertTy = Type());

/// Builds a reference to the \c self decl in a function, for use as an argument
/// to a function.
///
/// \param selfDecl The self decl to reference.
/// \param selfAccessorKind The kind of access being performed.
/// \param isMutable Whether the resulting argument is for a mutable self
/// argument. Such an argument is passed 'inout'.
Argument buildSelfArgument(VarDecl *selfDecl, SelfAccessorKind selfAccessorKind,
                           bool isMutable);

/// Build an argument list that forwards references to the specified parameter
/// list.
ArgumentList *buildForwardingArgumentList(ArrayRef<ParamDecl *> params,
                                          ASTContext &ctx);

/// Returns the protocol requirement with the specified name.
ValueDecl *getProtocolRequirement(ProtocolDecl *protocol, Identifier name);

// Returns true if given nominal type declaration has a `let` stored property
// with an initial value.
bool hasLetStoredPropertyWithInitialValue(NominalTypeDecl *nominal);

/// Add 'nonisolated' to the synthesized declaration when needed. Returns true
/// if an attribute was added.
bool addNonIsolatedToSynthesized(NominalTypeDecl *nominal, ValueDecl *value);

/// Adds the `@_spi` groups from \p inferredFromDecl to \p decl.
void applyInferredSPIAccessControlAttr(Decl *decl, const Decl *inferredFromDecl,
                                       ASTContext &ctx);

} // end namespace swift

#endif
