//===--- DerivedConformances.h - Derived protocol conformance ---*- C++ -*-===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2020 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//
//
//  This file defines entry points to synthesize compiler-derived conformances
//  to certain known protocols.
//
//===----------------------------------------------------------------------===//

#ifndef SWIFT_SEMA_DERIVEDCONFORMANCES_H
#define SWIFT_SEMA_DERIVEDCONFORMANCES_H

#include "swift/Basic/LLVM.h"
#include "swift/AST/Builtins.h"
#include <utility>

namespace swift {
class AbstractFunctionDecl;
class AccessorDecl;
class AssociatedTypeDecl;
class ASTContext;
struct ASTNode;
class CallExpr;
class CaseStmt;
class Decl;
class DeclContext;
class DeclRefExpr;
class EnumDecl;
class EnumElementDecl;
class Expr;
class GuardStmt;
class Identifier;
class NominalTypeDecl;
class ParamDecl;
class Pattern;
class PatternBindingDecl;
class ProtocolDecl;
class StructDecl;
class Type;
class TypeDecl;
class ValueDecl;
class VarDecl;

class DerivedConformance {
public:
  enum class SynthesizedIntroducer : bool { Let, Var };

public:
  ASTContext &Context;
  Decl *ConformanceDecl;
  NominalTypeDecl *Nominal;
  ProtocolDecl *Protocol;

  DerivedConformance(ASTContext &ctx, Decl *conformanceDecl,
                     NominalTypeDecl *nominal, ProtocolDecl *protocol);

  /// Retrieve the context in which the conformance is declared (either the
  /// nominal type, or an extension of it) as a \c DeclContext.
  DeclContext *getConformanceContext() const;

  /// Retrieve the module in which the conformance is declared.
  ModuleDecl *getParentModule() const;

  /// Add \c children as members of the context that declares the conformance.
  void addMembersToConformanceContext(ArrayRef<Decl *> children);
  /// Add \c member right after the \c hint member which may be the tail
  void addMemberToConformanceContext(Decl *member, Decl* hint);
  /// Add \c member in front of any other existing members
  void addMemberToConformanceContext(Decl *member, bool insertAtHead);

  /// Get the declared type of the protocol that this is conformance is for.
  Type getProtocolType() const;

  /// Returns the VarDecl of each stored property in the given struct whose type
  /// does not conform to a protocol.
  /// \p theStruct The struct whose stored properties should be checked.
  /// \p protocol The protocol being requested.
  /// \return The VarDecl of each stored property whose type does not conform.
  static SmallVector<VarDecl *, 3> storedPropertiesNotConformingToProtocol(
      DeclContext *DC, StructDecl *theStruct, ProtocolDecl *protocol);

  /// True if the type can implicitly derive a conformance for the given
  /// protocol.
  ///
  /// If true, explicit conformance checking will synthesize implicit
  /// declarations for requirements of the protocol that are not satisfied by
  /// the type's explicit members.
  ///
  /// \param nominal The nominal type for which we are determining whether to
  /// derive a witness.
  ///
  /// \param protocol The protocol whose requirements are being derived.
  ///
  /// \return True if the type can implicitly derive a conformance for the
  /// given protocol.
  static bool derivesProtocolConformance(DeclContext *DC,
                                         NominalTypeDecl *nominal,
                                         ProtocolDecl *protocol);

  /// Diagnose problems, if any, preventing automatic derivation of protocol
  /// requirements
  ///
  /// \param nominal The nominal type for which we would like to diagnose
  /// derivation failures
  ///
  /// \param protocol The protocol with requirements we would like to diagnose
  /// derivation failures for
  static void tryDiagnoseFailedDerivation(DeclContext *DC,
                                          NominalTypeDecl *nominal,
                                          ProtocolDecl *protocol);

  /// Diagnose any members which do not conform to the protocol for which
  /// we were trying to synthesize the conformance to.
  ///
  /// \param nominal The nominal type for which we would like to diagnose
  /// derivation failures
  ///
  /// \param protocol The protocol with requirements we would like to diagnose
  /// derivation failures for
  static void diagnoseAnyNonConformingMemberTypes(DeclContext *DC,
                                                  NominalTypeDecl *nominal,
                                                  ProtocolDecl *protocol);

  /// Diagnose the declaration for which we were trying to synthesize
  /// the conformance for, if the synthesis is not supported for that
  /// declaration.
  ///
  /// \param nominal The nominal type for which we would like to diagnose
  /// derivation failures
  ///
  /// \param protocol The protocol with requirements we would like to diagnose
  /// derivation failures for
  static void diagnoseIfSynthesisUnsupportedForDecl(NominalTypeDecl *nominal,
                                                    ProtocolDecl *protocol);

  /// Determine the derivable requirement that would satisfy the given
  /// requirement, if there is one.
  ///
  /// \param nominal The nominal type for which we are determining whether to
  /// derive a witness.
  ///
  /// \param requirement The requirement for which we are checking for a
  /// derivation. This requirement need not be within a derivable protocol,
  /// because derivable requirements can get restated in inherited unrelated
  /// or unrelated protocols.
  ///
  /// \returns The requirement whose witness could be derived to potentially
  /// satisfy this given requirement, or NULL if there is no such requirement.
  static ValueDecl *getDerivableRequirement(NominalTypeDecl *nominal,
                                            ValueDecl *requirement);

  /// Determine if an AdditiveArithmetic requirement can be derived for a type.
  ///
  /// \returns True if the requirement can be derived.
  static bool canDeriveAdditiveArithmetic(NominalTypeDecl *type,
                                          DeclContext *DC);

  /// Derive an AdditiveArithmetic requirement for a nominal type.
  ///
  /// \returns the derived member, which will also be added to the type.
  ValueDecl *deriveAdditiveArithmetic(ValueDecl *requirement);

  /// Determine if a Differentiable requirement can be derived for a nominal
  /// type.
  ///
  /// \returns True if the requirement can be derived.
  static bool canDeriveDifferentiable(NominalTypeDecl *type, DeclContext *DC,
                                      ValueDecl *requirement);

  /// Derive a Differentiable requirement for a nominal type.
  ///
  /// \returns the derived member, which will also be added to the type.
  ValueDecl *deriveDifferentiable(ValueDecl *requirement);

  /// Derive a Differentiable type witness for a nominal type.
  ///
  /// \returns the derived member, which will also be added to the type.
  std::pair<Type, TypeDecl *>
  deriveDifferentiable(AssociatedTypeDecl *assocType);

  /// Derive a CaseIterable requirement for an enum if it has no associated
  /// values for any of its cases.
  ///
  /// \returns the derived member, which will also be added to the type.
  ValueDecl *deriveCaseIterable(ValueDecl *requirement);

  /// Derive a CaseIterable type witness for an enum if it has no associated
  /// values for any of its cases.
  ///
  /// \returns the derived member, which will also be added to the type.
  Type deriveCaseIterable(AssociatedTypeDecl *assocType);

  /// Determine if a RawRepresentable requirement can be derived for a type.
  ///
  /// This is implemented for non-empty enums without associated values,
  /// that declare a raw type in the inheritance clause.
  static bool canDeriveRawRepresentable(DeclContext *DC, NominalTypeDecl *type);

  /// Derive a RawRepresentable requirement for an enum, if it has a valid
  /// raw type and raw values for all of its cases.
  ///
  /// \returns the derived member, which will also be added to the type.
  ValueDecl *deriveRawRepresentable(ValueDecl *requirement);

  /// Derive a RawRepresentable type witness for an enum, if it has a valid
  /// raw type and raw values for all of its cases.
  ///
  /// \returns the derived member, which will also be added to the type.
  Type deriveRawRepresentable(AssociatedTypeDecl *assocType);

  /// Determine if a Comparable requirement can be derived for a type.
  ///
  /// This is implemented for enums without associated or raw values.
  ///
  /// \returns True if the requirement can be derived.
  static bool canDeriveComparable(DeclContext *DC, EnumDecl *enumeration);

  /// Derive an Equatable requirement for a type.
  ///
  /// This is implemented for enums without associated or raw values.
  ///
  /// \returns the derived member, which will also be added to the type.
  ValueDecl *deriveComparable(ValueDecl *requirement);

  /// Diagnose problems, if any, preventing automatic derivation of Comparable
  /// requirements
  ///
  /// \param nominal The nominal type for which we would like to diagnose
  /// derivation failures
  static void tryDiagnoseFailedComparableDerivation(DeclContext *DC,
                                                    NominalTypeDecl *nominal);

  /// Diagnose problems, if any, preventing automatic derivation of
  /// DistributedActor requirements
  ///
  /// \param nominal The nominal type for which we would like to diagnose
  /// derivation failures
  static void tryDiagnoseFailedDistributedActorDerivation(DeclContext *DC,
                                                    NominalTypeDecl *nominal);

  /// Diagnose problems, if any, preventing automatic derivation of
  /// DistributedActor requirements
  ///
  /// \param nominal The nominal type for which we would like to diagnose
  /// derivation failures
  static void
  tryDiagnoseFailedDistributedActorSystemDerivation(DeclContext *DC,
                                                    NominalTypeDecl *nominal);

  /// Determine if an Equatable requirement can be derived for a type.
  ///
  /// This is implemented for enums without associated values or all-Equatable
  /// associated values, and for structs with all-Equatable stored properties.
  ///
  /// \returns True if the requirement can be derived.
  static bool canDeriveEquatable(DeclContext *DC, NominalTypeDecl *type);

  /// Derive an Equatable requirement for a type.
  ///
  /// This is implemented for enums without associated values or all-Equatable
  /// associated values, and for structs with all-Equatable stored properties.
  ///
  /// \returns the derived member, which will also be added to the type.
  ValueDecl *deriveEquatable(ValueDecl *requirement);

  /// Diagnose problems, if any, preventing automatic derivation of Equatable
  /// requirements
  ///
  /// \param nominal The nominal type for which we would like to diagnose
  /// derivation failures
  static void tryDiagnoseFailedEquatableDerivation(DeclContext *DC,
                                                   NominalTypeDecl *nominal);

  /// Determine if a Hashable requirement can be derived for a type.
  ///
  /// This is implemented for enums without associated values or all-Hashable
  /// associated values, and for structs with all-Hashable stored properties.
  ///
  /// \returns True if the requirement can be derived.
  static bool canDeriveHashable(NominalTypeDecl *type);

  /// Derive a Hashable requirement for a type.
  ///
  /// This is implemented for enums without associated values or all-Hashable
  /// associated values, and for structs with all-Hashable stored properties.
  ///
  /// \returns the derived member, which will also be added to the type.
  ValueDecl *deriveHashable(ValueDecl *requirement);

  /// Diagnose problems, if any, preventing automatic derivation of Hashable
  /// requirements
  ///
  /// \param nominal The nominal type for which we would like to diagnose
  /// derivation failures
  static void tryDiagnoseFailedHashableDerivation(DeclContext *DC,
                                                  NominalTypeDecl *nominal);

  /// Derive a _BridgedNSError requirement for an @objc enum type.
  ///
  /// \returns the derived member, which will also be added to the type.
  ValueDecl *deriveBridgedNSError(ValueDecl *requirement);

  /// Determine if \c Encodable can be derived for the given type.
  static bool canDeriveEncodable(NominalTypeDecl *NTD);
  /// Determine if \c Decodable can be derived for the given type.
  static bool canDeriveDecodable(NominalTypeDecl *NTD);

  /// Derive a CodingKey requirement for an enum type.
  ///
  /// \returns the derived member, which will also be added to the type.
  ValueDecl *deriveCodingKey(ValueDecl *requirement);

  /// Derive an Encodable requirement for a struct type.
  ///
  /// \returns the derived member, which will also be added to the type.
  ValueDecl *deriveEncodable(ValueDecl *requirement);

  /// Derive a Decodable requirement for a struct type.
  ///
  /// \returns the derived member, which will also be added to the type.
  ValueDecl *deriveDecodable(ValueDecl *requirement);  

  /// Identifiable may need to have the `ID` type witness synthesized explicitly
  static bool canDeriveIdentifiable(NominalTypeDecl *nominal,
                                    DeclContext *dc);

  /// Whether we can derive the given DistributedActor requirement in the given context.
  static bool canDeriveDistributedActor(NominalTypeDecl *nominal,
                                        DeclContext *dc);

  /// Whether we can derive the given DistributedActorSystem requirements.
  static bool canDeriveDistributedActorSystem(NominalTypeDecl *nominal,
                                              DeclContext *dc);

  /// Derive a 'DistributedActor' requirement for an distributed actor.
  ///
  /// \returns the derived member, which will also be added to the type.
  ValueDecl *deriveDistributedActor(ValueDecl *requirement);

  /// Derive a 'DistributedActorSystem' requirement..
  ///
  /// \returns the derived member, which will also be added to the type.
  ValueDecl *deriveDistributedActorSystem(ValueDecl *requirement);

  /// Derive a DistributedActor associated type for a distributed actor.
  ///
  /// \returns the derived type member, which will also be added to the type.
  std::pair<Type, TypeDecl *> deriveDistributedActor(
      AssociatedTypeDecl *assocType);

  /// Determine if \c Actor can be derived for the given type.
  static bool canDeriveActor(DeclContext *DC, NominalTypeDecl *NTD);

  /// Derive an Actor witness for an actor type.
  ///
  /// \returns the derived member, which will also be added to the type.
  ValueDecl *deriveActor(ValueDecl *requirement);

  /// Declare a read-only property.
  std::pair<VarDecl *, PatternBindingDecl *>
  declareDerivedProperty(SynthesizedIntroducer intro, Identifier name,
                         Type propertyInterfaceType, Type propertyContextType,
                         bool isStatic, bool isFinal);

  /// Add a getter to a derived property.  The property becomes read-only.
  static AccessorDecl *
  addGetterToReadOnlyDerivedProperty(VarDecl *property,
                                     Type propertyContextType);

  /// Declare a getter for a derived property.
  /// The getter will not be added to the property yet.
  static AccessorDecl *declareDerivedPropertyGetter(VarDecl *property,
                                                    Type propertyContextType);

  /// Build a reference to the 'self' decl of a derived function.
  static DeclRefExpr *createSelfDeclRef(AbstractFunctionDecl *fn);

  /// Build a builtin call.  By default, the call is assumed not to throw.
  static CallExpr *createBuiltinCall(ASTContext &ctx,
                                     BuiltinValueKind builtin,
                                     ArrayRef<Type> typeArgs,
                              ArrayRef<ProtocolConformanceRef> conformances,
                                     ArrayRef<Expr *> args);

  /// Build a call to the stdlib function that should be called when unavailable
  /// code is reached unexpectedly.
  static CallExpr *
  createDiagnoseUnavailableCodeReachedCallExpr(ASTContext &ctx);

  /// Returns true if this derivation is trying to use a context that isn't
  /// appropriate for deriving.
  ///
  /// \param synthesizing The decl that is being synthesized.
  bool checkAndDiagnoseDisallowedContext(ValueDecl *synthesizing) const;

  /// Returns a generated guard statement that checks whether the given lhs and
  /// rhs expressions are equal. If not equal, the else block for the guard
  /// returns `guardReturnValue`.
  /// \p C The AST context.
  /// \p lhsExpr The first expression to compare for equality.
  /// \p rhsExpr The second expression to compare for equality.
  /// \p guardReturnValue The expression to return if the two sides are not
  /// equal
  static GuardStmt *returnIfNotEqualGuard(ASTContext &C, Expr *lhsExpr,
                                          Expr *rhsExpr,
                                          Expr *guardReturnValue);
  // return false
  static GuardStmt *returnFalseIfNotEqualGuard(ASTContext &C, Expr *lhsExpr,
                                               Expr *rhsExpr);

  // Return `nil` is the `testExp` is `false`.
  static GuardStmt *returnNilIfFalseGuardTypeChecked(ASTContext &C,
                                                     Expr *testExpr,
                                                     Type optionalWrappedType);

  // return lhs < rhs
  static GuardStmt *
  returnComparisonIfNotEqualGuard(ASTContext &C, Expr *lhsExpr, Expr *rhsExpr);

  /// Returns the ParamDecl for each associated value of the given enum whose
  /// type does not conform to a protocol \p theEnum The enum whose elements and
  /// associated values should be checked. \p protocol The protocol being
  /// requested. \return The ParamDecl of each associated value whose type does
  /// not conform.
  static SmallVector<ParamDecl *, 4>
  associatedValuesNotConformingToProtocol(DeclContext *DC, EnumDecl *theEnum,
                                          ProtocolDecl *protocol);

  /// Returns true if, for every element of the given enum, it either has no
  /// associated values or all of them conform to a protocol.
  /// \p theEnum The enum whose elements and associated values should be
  /// checked. \p protocol The protocol being requested. \return True if all
  /// associated values of all elements of the enum conform.
  static bool allAssociatedValuesConformToProtocol(DeclContext *DC,
                                                   EnumDecl *theEnum,
                                                   ProtocolDecl *protocol);
  /// Create AST statements which convert from an enum to an Int with a switch.
  /// \p stmts The generated statements are appended to this vector.
  /// \p parentDC Either an extension or the enum itself.
  /// \p enumDecl The enum declaration.
  /// \p enumVarDecl The enum input variable.
  /// \p funcDecl The parent function.
  /// \p indexName The name of the output variable.
  /// \return A DeclRefExpr of the output variable (of type Int).
  static DeclRefExpr *
  convertEnumToIndex(SmallVectorImpl<ASTNode> &stmts, DeclContext *parentDC,
                     EnumDecl *enumDecl, VarDecl *enumVarDecl,
                     AbstractFunctionDecl *funcDecl, const char *indexName);

  static Pattern *enumElementPayloadSubpattern(
      EnumElementDecl *enumElementDecl, char varPrefix, DeclContext *varContext,
      SmallVectorImpl<VarDecl *> &boundVars, bool useLabels = false);

  /// Creates a synthesized case statement that has the following structure:
  ///
  ///     case .<elt>, ..., .<elt>:
  ///       _diagnoseUnavailableCodeReached()
  ///
  /// The number of \c .<elt> matches is equal to \p subPatternCount.
  static CaseStmt *unavailableEnumElementCaseStmt(
      Type enumType, EnumElementDecl *enumElementDecl, DeclContext *parentDC,
      unsigned subPatternCount = 1);

  static VarDecl *indexedVarDecl(char prefixChar, int index, Type type,
                                 DeclContext *varContext);
};

/// Determine whether any "memberwise" accessors, which walk through the
/// stored properties of the given nominal type, require actor isolation
/// because they involve mutable state.
bool memberwiseAccessorsRequireActorIsolation(NominalTypeDecl *nominal);

} // namespace swift

#endif
