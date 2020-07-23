//===--- SILWitnessVisitor.h - Witness method table visitor -----*- C++ -*-===//
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
// This file defines the SILWitnessVisitor class, which is used to generate and
// perform lookups in witness method tables for protocols and protocol
// conformances.
//
//===----------------------------------------------------------------------===//

#ifndef SWIFT_SIL_SILWITNESSVISITOR_H
#define SWIFT_SIL_SILWITNESSVISITOR_H

#include "swift/AST/ASTVisitor.h"
#include "swift/AST/Decl.h"
#include "swift/AST/ProtocolAssociations.h"
#include "swift/AST/Types.h"
#include "swift/SIL/TypeLowering.h"
#include "llvm/Support/ErrorHandling.h"

namespace swift {

/// A CRTP class for visiting the witnesses of a protocol.
///
/// The design here is that each entry (or small group of entries)
/// gets turned into a call to the implementation class describing
/// the exact variant of witness.  For example, for member
/// variables, there should be separate callbacks for adding a
/// getter/setter pair, for just adding a getter, and for adding a
/// physical projection (if we decide to support that).
///
/// You must override the following methods:
/// - addProtocolConformanceDescriptor()
/// - addOutOfLineBaseProtocol()
/// - addAssociatedType()
/// - addAssociatedConformance()
/// - addMethod()
/// - addPlaceholder()

template <class T> class SILWitnessVisitor : public ASTVisitor<T> {
  T &asDerived() { return *static_cast<T*>(this); }

public:
  void visitProtocolDecl(ProtocolDecl *protocol) {
    // This chicanery is to move conformances arising from
    // protocol extensions to the end of the witness table.
    unsigned moduleNumber = 0;
    while (visitProtocolDecl(protocol, moduleNumber++)) {}
  }

  bool visitProtocolDecl(ProtocolDecl *protocol, unsigned moduleNumber) {
    bool emittedConformance = false;
    llvm::DenseMap<ProtocolDecl *,bool> seen;

    // The protocol conformance descriptor gets added first.
    if (moduleNumber == 0)
      asDerived().addProtocolConformanceDescriptor();

    for (const auto &reqt : protocol->getRequirementSignature()) {
      switch (reqt.getKind()) {
      // These requirements don't show up in the witness table.
      case RequirementKind::Superclass:
      case RequirementKind::SameType:
      case RequirementKind::Layout:
        continue;

      case RequirementKind::Conformance: {
        auto type = reqt.getFirstType()->getCanonicalType();
        assert(type->isTypeParameter());
        auto requirement =
          cast<ProtocolType>(reqt.getSecondType()->getCanonicalType())
            ->getDecl();

        // ObjC protocols do not have witnesses.
        if (!Lowering::TypeConverter::protocolRequiresWitnessTable(requirement))
          continue;

        if (reqt.getModuleNumber() != moduleNumber)
          continue;

        // If the type parameter is 'self', consider this to be protocol
        // inheritance.  In the canonical signature, these should all
        // come before any protocol requirements on associated types.
        if (auto parameter = dyn_cast<GenericTypeParamType>(type)) {
          assert(type->isEqual(protocol->getSelfInterfaceType()));
          assert(parameter->getDepth() == 0 && parameter->getIndex() == 0 &&
                 "non-self type parameter in protocol");
          asDerived().addOutOfLineBaseProtocol(requirement);
          emittedConformance = true;
          continue;
        }

        // Otherwise, add an associated requirement.
        AssociatedConformance assocConf(protocol, type, requirement);
        asDerived().addAssociatedConformance(assocConf);
        emittedConformance = true;
        continue;
      }
      }
      llvm_unreachable("bad requirement kind");
    }

    if (moduleNumber)
      return emittedConformance;

    // Add the associated types.
    for (auto *associatedType : protocol->getAssociatedTypeMembers()) {
      // If this is a new associated type (which does not override an
      // existing associated type), add it.
      if (associatedType->getOverriddenDecls().empty())
        asDerived().addAssociatedType(AssociatedType(associatedType));
    }

//    if (asDerived().shouldVisitRequirementSignatureOnly())
//      return true;

    // Visit the witnesses for the direct members of a protocol.
    for (Decl *member : protocol->getMembers()) {
      ASTVisitor<T>::visit(member);
    }

    return true;
  }

  /// If true, only the base protocols and associated types will be visited.
  /// The base implementation returns false.
  bool shouldVisitRequirementSignatureOnly() const {
    return false;
  }

  /// Fallback for unexpected protocol requirements.
  void visitDecl(Decl *d) {
    llvm_unreachable("unhandled protocol requirement");
  }

  void visitAbstractStorageDecl(AbstractStorageDecl *sd) {
    sd->visitOpaqueAccessors([&](AccessorDecl *accessor) {
      if (SILDeclRef::requiresNewWitnessTableEntry(accessor)) {
        asDerived().addMethod(SILDeclRef(accessor, SILDeclRef::Kind::Func));
        addAutoDiffDerivativeMethodsIfRequired(accessor,
                                               SILDeclRef::Kind::Func);
      }
    });
  }

  void visitConstructorDecl(ConstructorDecl *cd) {
    if (SILDeclRef::requiresNewWitnessTableEntry(cd)) {
      asDerived().addMethod(SILDeclRef(cd, SILDeclRef::Kind::Allocator));
      addAutoDiffDerivativeMethodsIfRequired(cd, SILDeclRef::Kind::Allocator);
    }
  }

  void visitAccessorDecl(AccessorDecl *func) {
    // Accessors are emitted by visitAbstractStorageDecl, above.
  }

  void visitFuncDecl(FuncDecl *func) {
    assert(!isa<AccessorDecl>(func));
    if (SILDeclRef::requiresNewWitnessTableEntry(func)) {
      asDerived().addMethod(SILDeclRef(func, SILDeclRef::Kind::Func));
      addAutoDiffDerivativeMethodsIfRequired(func, SILDeclRef::Kind::Func);
    }
  }

  void visitMissingMemberDecl(MissingMemberDecl *placeholder) {
    asDerived().addPlaceholder(placeholder);
  }

  void visitAssociatedTypeDecl(AssociatedTypeDecl *td) {
    // We already visited these in the first pass.
  }
    
  void visitTypeAliasDecl(TypeAliasDecl *tad) {
    // We don't care about these by themselves for witnesses.
  }

  void visitPatternBindingDecl(PatternBindingDecl *pbd) {
    // We only care about the contained VarDecls.
  }

  void visitIfConfigDecl(IfConfigDecl *icd) {
    // We only care about the active members, which were already subsumed by the
    // enclosing type.
  }

  void visitPoundDiagnosticDecl(PoundDiagnosticDecl *pdd) {
    // We don't care about diagnostics at this stage.
  }

private:
  void addAutoDiffDerivativeMethodsIfRequired(AbstractFunctionDecl *AFD,
                                              SILDeclRef::Kind kind) {
    SILDeclRef declRef(AFD, kind);
    for (auto *diffAttr : AFD->getAttrs().getAttributes<DifferentiableAttr>()) {
      asDerived().addMethod(declRef.asAutoDiffDerivativeFunction(
          AutoDiffDerivativeFunctionIdentifier::get(
              AutoDiffDerivativeFunctionKind::JVP,
              diffAttr->getParameterIndices(),
              diffAttr->getDerivativeGenericSignature(),
              AFD->getASTContext())));
      asDerived().addMethod(declRef.asAutoDiffDerivativeFunction(
          AutoDiffDerivativeFunctionIdentifier::get(
              AutoDiffDerivativeFunctionKind::VJP,
              diffAttr->getParameterIndices(),
              diffAttr->getDerivativeGenericSignature(),
              AFD->getASTContext())));
    }
  }
};

} // end namespace swift

#endif
