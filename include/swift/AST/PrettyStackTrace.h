//===--- PrettyStackTrace.h - Crash trace information -----------*- C++ -*-===//
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
// This file defines RAII classes that give better diagnostic output
// about when, exactly, a crash is occurring.
//
//===----------------------------------------------------------------------===//

#ifndef SWIFT_PRETTYSTACKTRACE_H
#define SWIFT_PRETTYSTACKTRACE_H

#include "swift/AST/AnyFunctionRef.h"
#include "swift/AST/FreestandingMacroExpansion.h"
#include "swift/AST/Identifier.h"
#include "swift/AST/Type.h"
#include "swift/Basic/SourceLoc.h"
#include "llvm/ADT/None.h"
#include "llvm/ADT/Optional.h"
#include "llvm/Support/PrettyStackTrace.h"

namespace clang {
  class Type;
  class ASTContext;
}

namespace swift {
  class ASTContext;
  class Decl;
  class Expr;
  class GenericSignature;
  class Pattern;
  class Stmt;
  class TypeRepr;

void printSourceLocDescription(llvm::raw_ostream &out, SourceLoc loc,
                               const ASTContext &Context,
                               bool addNewline = true);

/// PrettyStackTraceLocation - Observe that we are doing some
/// processing starting at a fixed location.
class PrettyStackTraceLocation : public llvm::PrettyStackTraceEntry {
  const ASTContext &Context;
  SourceLoc Loc;
  const char *Action;
public:
  PrettyStackTraceLocation(const ASTContext &C, const char *action,
                           SourceLoc loc)
    : Context(C), Loc(loc), Action(action) {}
  virtual void print(llvm::raw_ostream &OS) const override;
};

void printDeclDescription(llvm::raw_ostream &out, const Decl *D,
                          const ASTContext &Context, bool addNewline = true);

/// PrettyStackTraceDecl - Observe that we are processing a specific
/// declaration.
class PrettyStackTraceDecl : public llvm::PrettyStackTraceEntry {
  const Decl *TheDecl;
  const char *Action;
public:
  PrettyStackTraceDecl(const char *action, const Decl *D)
    : TheDecl(D), Action(action) {}
  virtual void print(llvm::raw_ostream &OS) const override;
};

/// PrettyStackTraceDecl - Observe that we are processing a specific
/// declaration with a given substitution map.
class PrettyStackTraceDeclAndSubst : public llvm::PrettyStackTraceEntry {
  const Decl *decl;
  SubstitutionMap subst;
  const char *action;
public:
  PrettyStackTraceDeclAndSubst(const char *action, SubstitutionMap subst,
                       const Decl *decl)
      : decl(decl), subst(subst), action(action) {}
  virtual void print(llvm::raw_ostream &OS) const override;
};

/// PrettyStackTraceAnyFunctionRef - Observe that we are processing a specific
/// function or closure literal.
class PrettyStackTraceAnyFunctionRef : public llvm::PrettyStackTraceEntry {
  AnyFunctionRef TheRef;
  const char *Action;
public:
  PrettyStackTraceAnyFunctionRef(const char *action, AnyFunctionRef ref)
    : TheRef(ref), Action(action) {}
  virtual void print(llvm::raw_ostream &OS) const override;
};

/// PrettyStackTraceFreestandingMacroExpansion -  Observe that we are
/// processing a specific freestanding macro expansion.
class PrettyStackTraceFreestandingMacroExpansion
    : public llvm::PrettyStackTraceEntry {
  const FreestandingMacroExpansion *Expansion;
  const char *Action;

public:
  PrettyStackTraceFreestandingMacroExpansion(
      const char *action, const FreestandingMacroExpansion *expansion)
      : Expansion(expansion), Action(action) {}
  virtual void print(llvm::raw_ostream &OS) const override;
};

void printExprDescription(llvm::raw_ostream &out, const Expr *E,
                          const ASTContext &Context, bool addNewline = true);

/// PrettyStackTraceExpr - Observe that we are processing a specific
/// expression.
class PrettyStackTraceExpr : public llvm::PrettyStackTraceEntry {
  const ASTContext &Context;
  Expr *TheExpr;
  const char *Action;
public:
  PrettyStackTraceExpr(const ASTContext &C, const char *action, Expr *E)
    : Context(C), TheExpr(E), Action(action) {}
  virtual void print(llvm::raw_ostream &OS) const override;
};

void printStmtDescription(llvm::raw_ostream &out, Stmt *S,
                          const ASTContext &Context, bool addNewline = true);

/// PrettyStackTraceStmt - Observe that we are processing a specific
/// statement.
class PrettyStackTraceStmt : public llvm::PrettyStackTraceEntry {
  const ASTContext &Context;
  Stmt *TheStmt;
  const char *Action;
public:
  PrettyStackTraceStmt(const ASTContext &C, const char *action, Stmt *S)
    : Context(C), TheStmt(S), Action(action) {}
  virtual void print(llvm::raw_ostream &OS) const override;
};

void printPatternDescription(llvm::raw_ostream &out, Pattern *P,
                             const ASTContext &Context, bool addNewline = true);

/// PrettyStackTracePattern - Observe that we are processing a
/// specific pattern.
class PrettyStackTracePattern : public llvm::PrettyStackTraceEntry {
  const ASTContext &Context;
  Pattern *ThePattern;
  const char *Action;
public:
  PrettyStackTracePattern(const ASTContext &C, const char *action, Pattern *P)
    : Context(C), ThePattern(P), Action(action) {}
  virtual void print(llvm::raw_ostream &OS) const override;
};

void printTypeDescription(llvm::raw_ostream &out, Type T,
                          const ASTContext &Context, bool addNewline = true);

/// PrettyStackTraceType - Observe that we are processing a specific type.
class PrettyStackTraceType : public llvm::PrettyStackTraceEntry {
  const ASTContext &Context;
  Type TheType;
  const char *Action;
public:
  PrettyStackTraceType(const ASTContext &C, const char *action, Type type)
    : Context(C), TheType(type), Action(action) {}
  virtual void print(llvm::raw_ostream &OS) const override;
};

/// PrettyStackTraceClangType - Observe that we are processing a
/// specific Clang type.
class PrettyStackTraceClangType : public llvm::PrettyStackTraceEntry {
  const clang::ASTContext &Context;
  const clang::Type *TheType;
  const char *Action;
public:
  PrettyStackTraceClangType(clang::ASTContext &ctx,
                            const char *action, const clang::Type *type)
    : Context(ctx), TheType(type), Action(action) {}
  virtual void print(llvm::raw_ostream &OS) const override;
};

/// Observe that we are processing a specific type representation.
class PrettyStackTraceTypeRepr : public llvm::PrettyStackTraceEntry {
  const ASTContext &Context;
  TypeRepr *TheType;
  const char *Action;
public:
  PrettyStackTraceTypeRepr(const ASTContext &C, const char *action,
                           TypeRepr *type)
    : Context(C), TheType(type), Action(action) {}
  virtual void print(llvm::raw_ostream &OS) const override;
};

/// PrettyStackTraceConformance - Observe that we are processing a
/// specific protocol conformance.
class PrettyStackTraceConformance : public llvm::PrettyStackTraceEntry {
  const ProtocolConformance *Conformance;
  const char *Action;
public:
  PrettyStackTraceConformance(const char *action,
                              const ProtocolConformance *conformance)
    : Conformance(conformance), Action(action) {}
  virtual void print(llvm::raw_ostream &OS) const override;
};

void printConformanceDescription(llvm::raw_ostream &out,
                                 const ProtocolConformance *conformance,
                                 const ASTContext &Context,
                                 bool addNewline = true);

class PrettyStackTraceGenericSignature : public llvm::PrettyStackTraceEntry {
  const char *Action;
  GenericSignature GenericSig;
  llvm::Optional<unsigned> Requirement;

public:
  PrettyStackTraceGenericSignature(
      const char *action, GenericSignature genericSig,
      llvm::Optional<unsigned> requirement = llvm::None)
      : Action(action), GenericSig(genericSig), Requirement(requirement) {}

  void setRequirement(llvm::Optional<unsigned> requirement) {
    Requirement = requirement;
  }

  void print(llvm::raw_ostream &out) const override;
};

class PrettyStackTraceSelector : public llvm::PrettyStackTraceEntry {
  ObjCSelector Selector;
  const char *Action;
public:
  PrettyStackTraceSelector(const char *action, ObjCSelector S)
    : Selector(S), Action(action) {}
  void print(llvm::raw_ostream &OS) const override;
};

/// PrettyStackTraceDifferentiabilityWitness - Observe that we are processing a
/// specific differentiability witness.
class PrettyStackTraceDifferentiabilityWitness
    : public llvm::PrettyStackTraceEntry {
  const SILDifferentiabilityWitnessKey Key;
  const char *Action;

public:
  PrettyStackTraceDifferentiabilityWitness(
      const char *action, const SILDifferentiabilityWitnessKey key)
      : Key(key), Action(action) {}
  virtual void print(llvm::raw_ostream &OS) const override;
};

void printDifferentiabilityWitnessDescription(
    llvm::raw_ostream &out, const SILDifferentiabilityWitnessKey key,
    bool addNewline = true);

/// PrettyStackTraceDeclContext - Observe that we are processing a
/// specific decl context.
class PrettyStackTraceDeclContext : public llvm::PrettyStackTraceEntry {
  const DeclContext *DC;
  const char *Action;
public:
  PrettyStackTraceDeclContext(const char *action, const DeclContext *DC)
    : DC(DC), Action(action) {}
  virtual void print(llvm::raw_ostream &OS) const override;
};

} // end namespace swift

#endif
