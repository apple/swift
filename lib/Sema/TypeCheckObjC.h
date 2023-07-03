//===--- TypeCheckObjC.h - Type Checking for ObjC interop -------*- C++ -*-===//
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
// This file provides utilities for type-checking interoperability with
// Objective-C.
//
//===----------------------------------------------------------------------===//
#ifndef SWIFT_SEMA_TYPE_CHECK_OBJC_H
#define SWIFT_SEMA_TYPE_CHECK_OBJC_H

#include "swift/AST/DiagnosticEngine.h"
#include "swift/AST/ForeignAsyncConvention.h"
#include "swift/AST/ForeignErrorConvention.h"
#include "llvm/ADT/Optional.h"
#include "llvm/ADT/PointerUnion.h"

namespace swift {

class AbstractFunctionDecl;
class ASTContext;
class ObjCAttr;
class SubscriptDecl;
class ValueDecl;
class VarDecl;
class InFlightDiagnostic;

/// Describes the reason why are we trying to apply @objc to a declaration.
///
/// Should only affect diagnostics. If you change this enum, also change
/// the OBJC_ATTR_SELECT macro in DiagnosticsSema.def.
class ObjCReason {
public:
  // The kind of reason.
  enum Kind {
    /// Has the '@cdecl' attribute.
    ExplicitlyCDecl,
    /// Has the 'dynamic' modifier.
    ExplicitlyDynamic,
    /// Has an explicit '@objc' attribute.
    ExplicitlyObjC,
    /// Has an explicit '@objcmembers' attribute.
    ExplicitlyObjCMembers,
    /// Has an explicit '@IBOutlet' attribute.
    ExplicitlyIBOutlet,
    /// Has an explicit '@IBAction' attribute.
    ExplicitlyIBAction,
    /// Has an explicit '@IBSegueAction' attribute.
    ExplicitlyIBSegueAction,
    /// Has an explicit '@NSManaged' attribute.
    ExplicitlyNSManaged,
    /// Is a member of an @objc protocol.
    MemberOfObjCProtocol,
    /// Implicitly-introduced @objc.
    ImplicitlyObjC,
    /// Is an override of an @objc member.
    OverridesObjC,
    /// Is a witness to an @objc protocol requirement.
    WitnessToObjC,
    /// Has an explicit '@IBInspectable' attribute.
    ExplicitlyIBInspectable,
    /// Has an explicit '@GKInspectable' attribute.
    ExplicitlyGKInspectable,
    /// Is it a member of an @objc extension of a class.
    MemberOfObjCExtension,
    /// Has an explicit '@objc' attribute added by an access note, rather than
    /// written in source code.
    ExplicitlyObjCByAccessNote,

    // These kinds do not appear in diagnostics.

    /// Is it a member of an @objcMembers class.
    MemberOfObjCMembersClass,
    /// A member of an Objective-C-defined class or subclass.
    MemberOfObjCSubclass,
    /// Is a member of an @objc enum.
    ElementOfObjCEnum,
    /// An accessor to a property.
    Accessor,
  };

private:
  Kind kind;

  /// When the kind is \c WitnessToObjC, the requirement being witnessed.
  llvm::PointerUnion<ValueDecl *, DeclAttribute *> declOrAttr =
      static_cast<DeclAttribute *>(nullptr);

  ObjCReason(Kind kind, ValueDecl *decl) : kind(kind), declOrAttr(decl) { }

  static bool requiresAttr(Kind kind) {
    switch (kind) {
    case ExplicitlyCDecl:
    case ExplicitlyDynamic:
    case ExplicitlyObjC:
    case ExplicitlyObjCMembers:
    case ExplicitlyIBOutlet:
    case ExplicitlyIBAction:
    case ExplicitlyIBSegueAction:
    case ExplicitlyNSManaged:
    case ExplicitlyIBInspectable:
    case ExplicitlyGKInspectable:
    case ExplicitlyObjCByAccessNote:
      return true;

    case MemberOfObjCProtocol:
    case ImplicitlyObjC:
    case OverridesObjC:
    case WitnessToObjC:
    case MemberOfObjCExtension:
    case MemberOfObjCMembersClass:
    case MemberOfObjCSubclass:
    case ElementOfObjCEnum:
    case Accessor:
      return false;
    }
  }

public:
  /// Implicit conversion from the trivial reason kinds.
  ObjCReason(Kind kind) : kind(kind) {
    assert(kind != WitnessToObjC && "Use ObjCReason::witnessToObjC()");
    assert(!requiresAttr(kind) && "Use ObjCReason(Kind, DeclAttribute*)");
  }

  ObjCReason(Kind kind, const DeclAttribute *attr)
      : kind(kind), declOrAttr(const_cast<DeclAttribute *>(attr)) {
    // const_cast above because it's difficult to get a non-const DeclAttribute.
    assert(requiresAttr(kind) && "Use ObjCReason(Kind)");
  }

  /// Retrieve the kind of requirement.
  operator Kind() const { return kind; }

  /// Form a reason specifying that we have a witness to the given @objc
  /// requirement.
  static ObjCReason witnessToObjC(ValueDecl *requirement) {
    return ObjCReason(WitnessToObjC, requirement);
  }

  /// When the entity should be @objc because it is a witness to an @objc
  /// requirement, retrieve the requirement.
  ValueDecl *getObjCRequirement() const {
    assert(kind == WitnessToObjC);
    return declOrAttr.get<ValueDecl *>();
  }

  DeclAttribute *getAttr() const {
    if (!requiresAttr(kind))
      return nullptr;
    return declOrAttr.get<DeclAttribute *>();
  }

  void setAttrInvalid() const;

  /// Emit an additional diagnostic describing why we are applying @objc to the
  /// decl, if this is not obvious from the decl itself.
  void describe(const Decl *VD) const;
};

/// Determine how to diagnose conflicts due to inferring @objc with this
/// particular reason.
DiagnosticBehavior
behaviorLimitForObjCReason(ObjCReason reason, ASTContext &ctx);

/// Returns the ObjCReason for this ObjCAttr to be attached to the declaration.
ObjCReason objCReasonForObjCAttr(const ObjCAttr *attr);

/// Return the %select discriminator for the OBJC_ATTR_SELECT macro used to
/// complain about the correct attribute during @objc inference.
unsigned getObjCDiagnosticAttrKind(ObjCReason reason);

/// Determine whether the given function can be represented in Objective-C,
/// and figure out its foreign error convention (if any).
bool isRepresentableInObjC(
    const AbstractFunctionDecl *AFD, ObjCReason Reason,
    llvm::Optional<ForeignAsyncConvention> &asyncConvention,
    llvm::Optional<ForeignErrorConvention> &errorConvention);

/// Determine whether the given variable can be represented in Objective-C.
bool isRepresentableInObjC(const VarDecl *VD, ObjCReason Reason);

/// Determine whether the given subscript can be represented in Objective-C.
bool isRepresentableInObjC(const SubscriptDecl *SD, ObjCReason Reason);

/// Check whether the given declaration can be represented in Objective-C.
bool canBeRepresentedInObjC(const ValueDecl *decl);

/// Attach Fix-Its to the given diagnostic that updates the name of the
/// given declaration to the desired target name.
///
/// \returns false if the name could not be fixed.
bool fixDeclarationName(InFlightDiagnostic &diag, const ValueDecl *decl,
                        DeclName targetName);

/// Fix the Objective-C name of the given declaration to match the provided
/// Objective-C selector.
///
/// \param ignoreImpliedName When true, ignore the implied name of the
/// given declaration, because it no longer applies.
///
/// For properties, the selector should be a zero-parameter selector of the
/// given property's name.
bool fixDeclarationObjCName(InFlightDiagnostic &diag, const ValueDecl *decl,
                            llvm::Optional<ObjCSelector> nameOpt,
                            llvm::Optional<ObjCSelector> targetNameOpt,
                            bool ignoreImpliedName = false);

} // end namespace swift

#endif // SWIFT_SEMA_TYPE_CHECK_OBJC_H
