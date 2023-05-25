//===--- PackConformance.h - Variadic Protocol Conformance ------*- C++ -*-===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2022 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//
//
// This file defines the PackConformance structure, which describes the
// conformance of a type pack parameter to a protocol.
//
//===----------------------------------------------------------------------===//
#ifndef SWIFT_AST_PACKCONFORMANCE_H
#define SWIFT_AST_PACKCONFORMANCE_H

#include "swift/AST/ASTAllocated.h"
#include "swift/AST/ProtocolConformanceRef.h"
#include "swift/AST/Type.h"
#include "swift/AST/TypeAlignments.h"
#include "swift/Basic/Compiler.h"
#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/FoldingSet.h"
#include "llvm/Support/TrailingObjects.h"

namespace swift {

class PackType;

class alignas(1 << DeclAlignInBits) PackConformance final
  : public ASTAllocated<PackConformance>,
    public llvm::FoldingSetNode,
    private llvm::TrailingObjects<PackConformance, ProtocolConformanceRef> {
  friend class ASTContext;
  friend TrailingObjects;

  /// The pack type conforming to the protocol.
  PackType *ConformingType;

  /// The conformed-to protocol.
  ProtocolDecl *Protocol;

public:
  void Profile(llvm::FoldingSetNodeID &ID) const;
  static void Profile(llvm::FoldingSetNodeID &ID,
                      PackType *conformingType,
                      ProtocolDecl *protocol,
                      ArrayRef<ProtocolConformanceRef> conformances);

private:
  PackConformance(PackType *conformingType,
                  ProtocolDecl *protocol,
                  ArrayRef<ProtocolConformanceRef> conformances);

  size_t numTrailingObjects(OverloadToken<ProtocolConformanceRef>) const;

public:
  static PackConformance *get(PackType *conformingType,
                              ProtocolDecl *protocol,
                              ArrayRef<ProtocolConformanceRef> conformances);

  PackType *getType() const { return ConformingType; }

  ProtocolDecl *getProtocol() const { return Protocol; }

  ArrayRef<ProtocolConformanceRef> getPatternConformances() const;

  bool isInvalid() const;

  bool isCanonical() const;

  PackConformance *getCanonicalConformance() const;

  PackType *getAssociatedType(Type assocType) const;

  PackConformance *
  getAssociatedConformance(Type assocType, ProtocolDecl *protocol) const;

  /// The ProtocolConformanceRef either stores a pack conformance, or
  /// it is invalid in the case of substitution failure.
  ProtocolConformanceRef subst(SubstitutionMap subMap,
                               SubstOptions options=None) const;

  /// The ProtocolConformanceRef either stores a pack conformance, or
  /// it is invalid in the case of substitution failure.
  ProtocolConformanceRef subst(TypeSubstitutionFn subs,
                               LookupConformanceFn conformances,
                               SubstOptions options=None) const;

  /// Apply an in-flight substitution to the conformances in this
  /// protocol conformance ref.
  ///
  /// This function should generally not be used outside of the
  /// substitution subsystem.
  ProtocolConformanceRef subst(InFlightSubstitution &IFS) const;

  SWIFT_DEBUG_DUMP;
  void dump(llvm::raw_ostream &out, unsigned indent = 0) const;
};

void simple_display(llvm::raw_ostream &out, PackConformance *conformance);

} // end namespace swift

#endif // SWIFT_AST_PACKCONFORMANCE_H
