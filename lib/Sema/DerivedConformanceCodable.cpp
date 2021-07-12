//===--- DerivedConformanceCodable.cpp - Derived Codable ------------------===//
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
// This file implements explicit derivation of the Encodable and Decodable
// protocols for a struct or class.
//
//===----------------------------------------------------------------------===//

#include "TypeChecker.h"
#include "llvm/ADT/STLExtras.h"
#include "swift/AST/Decl.h"
#include "swift/AST/Expr.h"
#include "swift/AST/Module.h"
#include "swift/AST/ParameterList.h"
#include "swift/AST/Pattern.h"
#include "swift/AST/Stmt.h"
#include "swift/AST/Types.h"
#include "swift/Basic/StringExtras.h"
#include "DerivedConformances.h"

using namespace swift;

/// Returns whether the type represented by the given ClassDecl inherits from a
/// type which conforms to the given protocol.
static bool superclassConformsTo(ClassDecl *target, KnownProtocolKind kpk) {
  if (!target) {
    return false;
  }

  auto superclass = target->getSuperclassDecl();
  if (!superclass)
    return false;

  return !superclass
              ->getModuleContext()
              ->lookupConformance(target->getSuperclass(),
                                  target->getASTContext().getProtocol(kpk))
              .isInvalid();
}

/// Retrieve the variable name for the purposes of encoding/decoding.
static Identifier getVarNameForCoding(VarDecl *var) {
  if (auto originalVar = var->getOriginalWrappedProperty())
    return originalVar->getName();

  return var->getName();
}

/// Compute the Identifier for the CodingKey of an enum case
static Identifier caseCodingKeysIdentifier(const ASTContext &C,
                                         EnumElementDecl *elt) {
  llvm::SmallString<16> scratch;
  camel_case::appendSentenceCase(scratch, elt->getBaseIdentifier().str());
  llvm::StringRef result =
      camel_case::appendSentenceCase(scratch, C.Id_CodingKeys.str());
  return C.getIdentifier(result);
}

/// Fetches the \c CodingKeys enum nested in \c target, potentially reaching
/// through a typealias if the "CodingKeys" entity is a typealias.
///
/// This is only useful once a \c CodingKeys enum has been validated (via \c
/// hasValidCodingKeysEnum) or synthesized (via \c synthesizeCodingKeysEnum).
///
/// \param C The \c ASTContext to perform the lookup in.
///
/// \param target The target type to look in.
///
/// \return A retrieved canonical \c CodingKeys enum if \c target has a valid
/// one; \c nullptr otherwise.
static EnumDecl *lookupEvaluatedCodingKeysEnum(ASTContext &C,
                                               NominalTypeDecl *target,
                                               Identifier identifier) {
  auto codingKeyDecls = target->lookupDirect(DeclName(identifier));
  if (codingKeyDecls.empty())
    return nullptr;

  auto *codingKeysDecl = codingKeyDecls.front();
  if (auto *typealiasDecl = dyn_cast<TypeAliasDecl>(codingKeysDecl))
    codingKeysDecl = typealiasDecl->getDeclaredInterfaceType()->getAnyNominal();

  return dyn_cast<EnumDecl>(codingKeysDecl);
}

static EnumDecl *lookupEvaluatedCodingKeysEnum(ASTContext &C,
                                               NominalTypeDecl *target) {
  return lookupEvaluatedCodingKeysEnum(C, target, C.Id_CodingKeys);
}

static EnumElementDecl *lookupEnumCase(ASTContext &C, NominalTypeDecl *target,
                                       Identifier identifier) {
  auto elementDecls = target->lookupDirect(DeclName(identifier));
  if (elementDecls.empty())
    return nullptr;

  auto *elementDecl = elementDecls.front();

  return dyn_cast<EnumElementDecl>(elementDecl);
}

static NominalTypeDecl *lookupErrorContext(ASTContext &C,
                                           NominalTypeDecl *errorDecl) {
  auto elementDecls = errorDecl->lookupDirect(C.Id_Context);
  if (elementDecls.empty())
    return nullptr;

  auto *decl = elementDecls.front();

  return dyn_cast<NominalTypeDecl>(decl);
}

static EnumDecl *addImplicitCodingKeys_enum(EnumDecl *target) {
  auto &C = target->getASTContext();

  // We want to look through all the case declarations of this enum to create
  // enum cases based on those case names.
  auto *codingKeyProto = C.getProtocol(KnownProtocolKind::CodingKey);
  auto codingKeyType = codingKeyProto->getDeclaredInterfaceType();
  InheritedEntry protoTypeLoc[1] = {
    InheritedEntry(TypeLoc::withoutLoc(codingKeyType))};
  ArrayRef<InheritedEntry> inherited = C.AllocateCopy(protoTypeLoc);

  llvm::SmallVector<EnumDecl *, 4> codingKeys;

  auto *enumDecl = new (C) EnumDecl(SourceLoc(), C.Id_CodingKeys, SourceLoc(),
                                    inherited, nullptr, target);
  enumDecl->setImplicit();
  enumDecl->setAccess(AccessLevel::Private);

  for (auto *elementDecl : target->getAllElements()) {
    auto *elt =
        new (C) EnumElementDecl(SourceLoc(), elementDecl->getBaseName(),
                                nullptr, SourceLoc(), nullptr, enumDecl);
    elt->setImplicit();
    enumDecl->addMember(elt);
  }
  // Forcibly derive conformance to CodingKey.
  TypeChecker::checkConformancesInContext(enumDecl);

  target->addMember(enumDecl);

  return enumDecl;
}

static EnumDecl *addImplicitCaseCodingKeys(EnumDecl *target,
                                      EnumElementDecl *elementDecl,
                                      EnumDecl *codingKeysEnum) {
  auto &C = target->getASTContext();

  auto enumIdentifier = caseCodingKeysIdentifier(C, elementDecl);

  auto *codingKeyProto = C.getProtocol(KnownProtocolKind::CodingKey);
  auto codingKeyType = codingKeyProto->getDeclaredInterfaceType();
  InheritedEntry protoTypeLoc[1] = {InheritedEntry(TypeLoc::withoutLoc(codingKeyType))};
  ArrayRef<InheritedEntry> inherited = C.AllocateCopy(protoTypeLoc);

  // Only derive if this case exist in the CodingKeys enum
  auto *codingKeyCase =
      lookupEnumCase(C, codingKeysEnum, elementDecl->getBaseIdentifier());
  if (!codingKeyCase)
    return nullptr;

  auto *caseEnum = new (C) EnumDecl(SourceLoc(), enumIdentifier, SourceLoc(),
                                    inherited, nullptr, target);
  caseEnum->setImplicit();
  caseEnum->setAccess(AccessLevel::Private);

  if (elementDecl->hasAssociatedValues()) {
    for (auto entry : llvm::enumerate(*elementDecl->getParameterList())) {
      auto *paramDecl = entry.value();

      // if the type conforms to {En,De}codable, add it to the enum.
      Identifier paramIdentifier = getVarNameForCoding(paramDecl);
      bool generatedName = false;
      if (paramIdentifier.empty()) {
        paramIdentifier = C.getIdentifier("_" + std::to_string(entry.index()));
        generatedName = true;
      }
      
      auto *elt =
          new (C) EnumElementDecl(SourceLoc(), paramIdentifier, nullptr,
                                  SourceLoc(), nullptr, caseEnum);
      elt->setImplicit();
      caseEnum->addMember(elt);
    }
  }

  // Forcibly derive conformance to CodingKey.
  TypeChecker::checkConformancesInContext(caseEnum);
  target->addMember(caseEnum);

  return caseEnum;
}

// Create CodingKeys in the parent type always, because both
// Encodable and Decodable might want to use it, and they may have
// different conditional bounds. CodingKeys is simple and can't
// depend on those bounds.
//
// FIXME: Eventually we should find a way to expose this function to the lookup
// machinery so it no longer costs two protocol conformance lookups to retrieve
// CodingKeys. It will also help in our quest to separate semantic and parsed
// members.
static EnumDecl *addImplicitCodingKeys(NominalTypeDecl *target) {
  if (auto *enumDecl = dyn_cast<EnumDecl>(target)) {
    return addImplicitCodingKeys_enum(enumDecl);
  }

  auto &C = target->getASTContext();
  assert(target->lookupDirect(DeclName(C.Id_CodingKeys)).empty());

  // We want to look through all the var declarations of this type to create
  // enum cases based on those var names.
  auto *codingKeyProto = C.getProtocol(KnownProtocolKind::CodingKey);
  auto codingKeyType = codingKeyProto->getDeclaredInterfaceType();
  InheritedEntry protoTypeLoc[1] = {InheritedEntry(TypeLoc::withoutLoc(codingKeyType))};
  ArrayRef<InheritedEntry> inherited = C.AllocateCopy(protoTypeLoc);

  auto *enumDecl = new (C) EnumDecl(SourceLoc(), C.Id_CodingKeys, SourceLoc(),
                                    inherited, nullptr, target);
  enumDecl->setImplicit();
  enumDecl->setSynthesized();
  enumDecl->setAccess(AccessLevel::Private);

  // For classes which inherit from something Encodable or Decodable, we
  // provide case `super` as the first key (to be used in encoding super).
  auto *classDecl = dyn_cast<ClassDecl>(target);
  if (superclassConformsTo(classDecl, KnownProtocolKind::Encodable) ||
      superclassConformsTo(classDecl, KnownProtocolKind::Decodable)) {
    // TODO: Ensure the class doesn't already have or inherit a variable named
    // "`super`"; otherwise we will generate an invalid enum. In that case,
    // diagnose and bail.
    auto *super = new (C) EnumElementDecl(SourceLoc(), C.Id_super, nullptr,
                                          SourceLoc(), nullptr, enumDecl);
    super->setImplicit();
    enumDecl->addMember(super);
  }

  for (auto *varDecl : target->getStoredProperties()) {
    if (!varDecl->isUserAccessible()) {
      continue;
    }

    auto *elt =
        new (C) EnumElementDecl(SourceLoc(), getVarNameForCoding(varDecl),
                                nullptr, SourceLoc(), nullptr, enumDecl);
    elt->setImplicit();
    enumDecl->addMember(elt);
  }

  // Forcibly derive conformance to CodingKey.
  TypeChecker::checkConformancesInContext(enumDecl);

  // Add to the type.
  target->addMember(enumDecl);

  return enumDecl;
}

static EnumDecl *validateCodingKeysType(const DerivedConformance &derived,
                                        TypeDecl *_codingKeysTypeDecl) {
  auto &C = derived.Context;
  auto *codingKeysTypeDecl = _codingKeysTypeDecl;
  // CodingKeys may be a typealias. If so, follow the alias to its canonical
  // type.
  auto codingKeysType = codingKeysTypeDecl->getDeclaredInterfaceType();
  if (isa<TypeAliasDecl>(codingKeysTypeDecl))
    codingKeysTypeDecl = codingKeysType->getAnyNominal();

  // Ensure that the type we found conforms to the CodingKey protocol.
  auto *codingKeyProto = C.getProtocol(KnownProtocolKind::CodingKey);
  if (!TypeChecker::conformsToProtocol(codingKeysType, codingKeyProto,
                                       derived.getConformanceContext())) {
    // If CodingKeys is a typealias which doesn't point to a valid nominal type,
    // codingKeysTypeDecl will be nullptr here. In that case, we need to warn on
    // the location of the usage, since there isn't an underlying type to
    // diagnose on.
    SourceLoc loc = codingKeysTypeDecl ? codingKeysTypeDecl->getLoc()
                                       : cast<TypeDecl>(_codingKeysTypeDecl)->getLoc();

    C.Diags.diagnose(loc, diag::codable_codingkeys_type_does_not_conform_here,
                     derived.getProtocolType());
    return nullptr;
  }

  auto *codingKeysDecl =
      dyn_cast_or_null<EnumDecl>(codingKeysType->getAnyNominal());
  if (!codingKeysDecl) {
    codingKeysTypeDecl->diagnose(
        diag::codable_codingkeys_type_is_not_an_enum_here,
        derived.getProtocolType());
    return nullptr;
  }

  return codingKeysDecl;
}

/// Validates the given CodingKeys enum decl by ensuring its cases are a 1-to-1
/// match with the the given VarDecls.
///
/// \param varDecls The \c var decls to validate against.
/// \param codingKeysTypeDecl The \c CodingKeys enum decl to validate.
static bool validateCodingKeysEnum(const DerivedConformance &derived,
                               llvm::SmallMapVector<Identifier, VarDecl *, 8> varDecls,
                               TypeDecl *codingKeysTypeDecl) {
  auto *codingKeysDecl = validateCodingKeysType(derived, codingKeysTypeDecl);
  if (!codingKeysDecl)
    return false;

  // Look through all var decls.
  //
  // If any of the entries in the CodingKeys decl are not present in the type
  // by name, then this decl doesn't match.
  // If there are any vars left in the type which don't have a default value
  // (for Decodable), then this decl doesn't match.
  bool varDeclsAreValid = true;
  for (auto elt : codingKeysDecl->getAllElements()) {
    auto it = varDecls.find(elt->getBaseIdentifier());
    if (it == varDecls.end()) {
      elt->diagnose(diag::codable_extraneous_codingkey_case_here,
                    elt->getBaseIdentifier());
      // TODO: Investigate typo-correction here; perhaps the case name was
      //       misspelled and we can provide a fix-it.
      varDeclsAreValid = false;
      continue;
    }

    // We have a property to map to. Ensure it's {En,De}codable.
    auto target = derived.getConformanceContext()->mapTypeIntoContext(
         it->second->getValueInterfaceType());
    if (TypeChecker::conformsToProtocol(target, derived.Protocol,
                                        derived.getConformanceContext())
            .isInvalid()) {
      TypeLoc typeLoc = {
          it->second->getTypeReprOrParentPatternTypeRepr(),
          it->second->getType(),
      };
      it->second->diagnose(diag::codable_non_conforming_property_here,
                           derived.getProtocolType(), typeLoc);
      varDeclsAreValid = false;
    } else {
      // The property was valid. Remove it from the list.
      varDecls.erase(it);
    }
  }

  if (!varDeclsAreValid)
    return false;

  // If there are any remaining var decls which the CodingKeys did not cover,
  // we can skip them on encode. On decode, though, we can only skip them if
  // they have a default value.
  if (derived.Protocol->isSpecificProtocol(KnownProtocolKind::Decodable)) {
    for (auto &entry : varDecls) {
      const auto *pbd = entry.second->getParentPatternBinding();
      if (pbd && pbd->isDefaultInitializable()) {
        continue;
      }

      if (entry.second->isParentInitialized()) {
        continue;
      }

      if (auto *paramDecl = dyn_cast<ParamDecl>(entry.second)) {
        if (paramDecl->hasDefaultExpr()) {
          continue;
        }
      }

      // The var was not default initializable, and did not have an explicit
      // initial value.
      varDeclsAreValid = false;
      entry.second->diagnose(diag::codable_non_decoded_property_here,
                             derived.getProtocolType(), entry.first);
    }
  }

  return varDeclsAreValid;
}

static bool validateCodingKeysEnum_enum(const DerivedConformance &derived,
                                        TypeDecl *codingKeysTypeDecl) {
  auto *enumDecl = dyn_cast<EnumDecl>(derived.Nominal);
  if (!enumDecl) {
    return false;
  }
  llvm::SmallSetVector<Identifier, 4> caseNames;
  for (auto *elt : enumDecl->getAllElements()) {
    caseNames.insert(elt->getBaseIdentifier());
  }

  auto *codingKeysDecl = validateCodingKeysType(derived,
                                                codingKeysTypeDecl);
  if (!codingKeysDecl)
    return false;

  bool casesAreValid = true;
  for (auto *elt : codingKeysDecl->getAllElements()) {
    if (!caseNames.contains(elt->getBaseIdentifier())) {
      elt->diagnose(diag::codable_extraneous_codingkey_case_here,
                    elt->getBaseIdentifier());
      casesAreValid = false;
    }
  }

  return casesAreValid;
}

/// Looks up and validates a CodingKeys enum for the given DerivedConformance.
/// If a CodingKeys enum does not exist, one will be derived.
static bool validateCodingKeysEnum(const DerivedConformance &derived) {
  auto &C = derived.Context;

  auto codingKeysDecls =
       derived.Nominal->lookupDirect(DeclName(C.Id_CodingKeys));

  if (codingKeysDecls.size() > 1) {
    return false;
  }

  ValueDecl *result = codingKeysDecls.empty()
                          ? addImplicitCodingKeys(derived.Nominal)
                          : codingKeysDecls.front();
  auto *codingKeysTypeDecl = dyn_cast<TypeDecl>(result);
  if (!codingKeysTypeDecl) {
    result->diagnose(diag::codable_codingkeys_type_is_not_an_enum_here,
                     derived.getProtocolType());
    return false;
  }

  if (dyn_cast<EnumDecl>(derived.Nominal)) {
    return validateCodingKeysEnum_enum(derived, codingKeysTypeDecl);
  } else {

    // Look through all var decls in the given type.
    // * Filter out lazy/computed vars.
    // * Filter out ones which are present in the given decl (by name).

    // Here we'll hold on to properties by name -- when we've validated a property
    // against its CodingKey entry, it will get removed.
    llvm::SmallMapVector<Identifier, VarDecl *, 8> properties;
    for (auto *varDecl : derived.Nominal->getStoredProperties()) {
      if (!varDecl->isUserAccessible())
        continue;

      properties[getVarNameForCoding(varDecl)] = varDecl;
    }

    return validateCodingKeysEnum(derived, properties, codingKeysTypeDecl);
  }
}

/// Looks up and validates a CaseCodingKeys enum for the given elementDecl.
/// If a CaseCodingKeys enum does not exist, one will be derived.
///
/// \param elementDecl The \c EnumElementDecl to validate against.
static bool validateCaseCodingKeysEnum(const DerivedConformance &derived,
                                       EnumElementDecl *elementDecl) {
  auto &C = derived.Context;
  auto *enumDecl = dyn_cast<EnumDecl>(derived.Nominal);
  if (!enumDecl) {
    return false;
  }

  auto *codingKeysEnum = lookupEvaluatedCodingKeysEnum(C, enumDecl);

  // At this point we ran validation for this and should have
  // a CodingKeys decl.
  assert(codingKeysEnum && "Missing CodingKeys decl.");

  auto cckIdentifier = caseCodingKeysIdentifier(C, elementDecl);
  auto caseCodingKeysDecls =
       enumDecl->lookupDirect(DeclName(cckIdentifier));

  if (caseCodingKeysDecls.size() > 1) {
    return false;
  }

  ValueDecl *result = caseCodingKeysDecls.empty()
                          ? addImplicitCaseCodingKeys(
                              enumDecl, elementDecl, codingKeysEnum)
                          : caseCodingKeysDecls.front();

  if (!result) {
    // There is no coding key defined for this element,
    // which is okay, because not all elements have to
    // be considered for serialization. Attempts to
    // en-/decode them will be handled at runtime.
    return true;
  }

  auto *codingKeysTypeDecl = dyn_cast<TypeDecl>(result);
  if (!codingKeysTypeDecl) {
    result->diagnose(diag::codable_codingkeys_type_is_not_an_enum_here,
                     derived.getProtocolType());
    return false;
  }

  // Here we'll hold on to parameters by name -- when we've validated a parameter
  // against its CodingKey entry, it will get removed.
  llvm::SmallMapVector<Identifier, VarDecl *, 8> properties;
  if (elementDecl->hasAssociatedValues()) {
    for (auto entry : llvm::enumerate(*elementDecl->getParameterList())) {
      auto paramDecl = entry.value();
      if (!paramDecl->isUserAccessible())
        continue;

      auto identifier = getVarNameForCoding(paramDecl);
      if (identifier.empty()) {
        identifier = C.getIdentifier("_" + std::to_string(entry.index()));
      }

      properties[identifier] = paramDecl;
    }
  }

  return validateCodingKeysEnum(derived, properties, codingKeysTypeDecl);
}

/// Creates a new var decl representing
///
///   var/let identifier : containerBase<keyType>
///
/// \c containerBase is the name of the type to use as the base (either
/// \c KeyedEncodingContainer or \c KeyedDecodingContainer).
///
/// \param C The AST context to create the decl in.
///
/// \param DC The \c DeclContext to create the decl in.
///
/// \param keyedContainerDecl The generic type to bind the key type in.
///
/// \param keyType The key type to bind to the container type.
///
/// \param introducer Whether to declare the variable as immutable.
///
/// \param identifier Identifier of the variable.
static VarDecl *createKeyedContainer(ASTContext &C, DeclContext *DC,
                                     NominalTypeDecl *keyedContainerDecl,
                                     Type keyType,
                                     VarDecl::Introducer introducer,
                                     Identifier identifier) {
  // Bind Keyed*Container to Keyed*Container<KeyType>
  Type boundType[1] = {keyType};
  auto containerType = BoundGenericType::get(keyedContainerDecl, Type(),
                                             C.AllocateCopy(boundType));

  // let container : Keyed*Container<KeyType>
  auto *containerDecl = new (C) VarDecl(/*IsStatic=*/false, introducer,
                                        SourceLoc(), identifier, DC);
  containerDecl->setImplicit();
  containerDecl->setSynthesized();
  containerDecl->setInterfaceType(containerType);
  return containerDecl;
}

/// Creates a new var decl representing
///
///   var/let container : containerBase<keyType>
///
/// \c containerBase is the name of the type to use as the base (either
/// \c KeyedEncodingContainer or \c KeyedDecodingContainer).
///
/// \param C The AST context to create the decl in.
///
/// \param DC The \c DeclContext to create the decl in.
///
/// \param keyedContainerDecl The generic type to bind the key type in.
///
/// \param keyType The key type to bind to the container type.
///
/// \param introducer Whether to declare the variable as immutable.
static VarDecl *createKeyedContainer(ASTContext &C, DeclContext *DC,
                                     NominalTypeDecl *keyedContainerDecl,
                                     Type keyType,
                                     VarDecl::Introducer introducer) {
  return createKeyedContainer(C, DC, keyedContainerDecl, keyType,
                              introducer, C.Id_container);
}

/// Creates a new \c CallExpr representing
///
///   base.container(keyedBy: CodingKeys.self)
///
/// \param C The AST context to create the expression in.
///
/// \param DC The \c DeclContext to create any decls in.
///
/// \param base The base expression to make the call on.
///
/// \param returnType The return type of the call.
///
/// \param param The parameter to the call.
static CallExpr *createContainerKeyedByCall(ASTContext &C, DeclContext *DC,
                                            Expr *base, Type returnType,
                                            NominalTypeDecl *param) {
  // (keyedBy:)
  auto *keyedByDecl = new (C)
      ParamDecl(SourceLoc(), SourceLoc(),
                C.Id_keyedBy, SourceLoc(), C.Id_keyedBy, DC);
  keyedByDecl->setImplicit();
  keyedByDecl->setSpecifier(ParamSpecifier::Default);
  keyedByDecl->setInterfaceType(returnType);

  // base.container(keyedBy:) expr
  auto *paramList = ParameterList::createWithoutLoc(keyedByDecl);
  auto *unboundCall = UnresolvedDotExpr::createImplicit(C, base, C.Id_container,
                                                        paramList);

  // CodingKeys.self expr
  auto *codingKeysExpr = TypeExpr::createImplicitForDecl(
      DeclNameLoc(), param, param->getDeclContext(),
      DC->mapTypeIntoContext(param->getInterfaceType()));
  auto *codingKeysMetaTypeExpr = new (C) DotSelfExpr(codingKeysExpr,
                                                     SourceLoc(), SourceLoc());

  // Full bound base.container(keyedBy: CodingKeys.self) call
  Expr *args[1] = {codingKeysMetaTypeExpr};
  Identifier argLabels[1] = {C.Id_keyedBy};
  return CallExpr::createImplicit(C, unboundCall, C.AllocateCopy(args),
                                  C.AllocateCopy(argLabels));
}

static CallExpr *createNestedContainerKeyedByForKeyCall(
    ASTContext &C, DeclContext *DC, Expr *base, NominalTypeDecl *codingKeysType,
    EnumElementDecl *key) {
  SmallVector<Identifier, 2> argNames{C.Id_keyedBy, C.Id_forKey};

  // base.nestedContainer(keyedBy:, forKey:) expr
  auto *unboundCall = UnresolvedDotExpr::createImplicit(
      C, base, C.Id_nestedContainer, argNames);

  // CodingKeys.self expr
  auto *codingKeysExpr = TypeExpr::createImplicitForDecl(
      DeclNameLoc(), codingKeysType, codingKeysType->getDeclContext(),
      DC->mapTypeIntoContext(codingKeysType->getInterfaceType()));
  auto *codingKeysMetaTypeExpr =
      new (C) DotSelfExpr(codingKeysExpr, SourceLoc(), SourceLoc());

  // key expr
  auto *metaTyRef = TypeExpr::createImplicit(
      DC->mapTypeIntoContext(key->getParentEnum()->getDeclaredInterfaceType()),
      C);
  auto *keyExpr = new (C) MemberRefExpr(metaTyRef, SourceLoc(), key,
                                        DeclNameLoc(), /*Implicit=*/true);

  // Full bound base.nestedContainer(keyedBy: CodingKeys.self, forKey: key) call
  Expr *args[2] = {codingKeysMetaTypeExpr, keyExpr};
  return CallExpr::createImplicit(C, unboundCall, C.AllocateCopy(args),
                                  argNames);
}

static ThrowStmt *createThrowDecodingErrorTypeMismatchStmt(
    ASTContext &C, DeclContext *DC, NominalTypeDecl *targetDecl,
    Expr *containerExpr, Expr *debugMessage) {
  auto *errorDecl = C.getDecodingErrorDecl();
  auto *contextDecl = lookupErrorContext(C, errorDecl);
  assert(contextDecl && "Missing Context decl.");

  auto *contextTypeExpr =
      TypeExpr::createImplicit(contextDecl->getDeclaredType(), C);

  // Context.init(codingPath:, debugDescription:)
  auto *contextInitCall = UnresolvedDotExpr::createImplicit(
      C, contextTypeExpr, DeclBaseName::createConstructor(),
      {C.Id_codingPath, C.Id_debugDescription, C.Id_underlyingError});

  auto *codingPathExpr =
      UnresolvedDotExpr::createImplicit(C, containerExpr, C.Id_codingPath);

  auto *contextInitCallExpr = CallExpr::createImplicit(
      C, contextInitCall,
      {codingPathExpr, debugMessage,
       new (C) NilLiteralExpr(SourceLoc(), /* Implicit */ true)},
      {C.Id_codingPath, C.Id_debugDescription, C.Id_underlyingError});

  auto *decodingErrorTypeExpr =
      TypeExpr::createImplicit(errorDecl->getDeclaredType(), C);
  auto *decodingErrorCall = UnresolvedDotExpr::createImplicit(
      C, decodingErrorTypeExpr, C.Id_typeMismatch,
      {Identifier(), Identifier()});
  auto *targetType = TypeExpr::createImplicit(
      DC->mapTypeIntoContext(targetDecl->getDeclaredInterfaceType()), C);
  auto *targetTypeExpr =
      new (C) DotSelfExpr(targetType, SourceLoc(), SourceLoc());

  auto *decodingErrorCallExpr = CallExpr::createImplicit(
      C, decodingErrorCall, {targetTypeExpr, contextInitCallExpr},
      {Identifier(), Identifier()});
  return new (C) ThrowStmt(SourceLoc(), decodingErrorCallExpr);
}

static ThrowStmt *createThrowEncodingErrorInvalidValueStmt(ASTContext &C,
                                                           DeclContext *DC,
                                                           Expr *valueExpr,
                                                           Expr *containerExpr,
                                                           Expr *debugMessage) {
  auto *errorDecl = C.getEncodingErrorDecl();
  auto *contextDecl = lookupErrorContext(C, errorDecl);
  assert(contextDecl && "Missing Context decl.");

  auto *contextTypeExpr =
      TypeExpr::createImplicit(contextDecl->getDeclaredType(), C);

  // Context.init(codingPath:, debugDescription:)
  auto *contextInitCall = UnresolvedDotExpr::createImplicit(
      C, contextTypeExpr, DeclBaseName::createConstructor(),
      {C.Id_codingPath, C.Id_debugDescription, C.Id_underlyingError});

  auto *codingPathExpr =
      UnresolvedDotExpr::createImplicit(C, containerExpr, C.Id_codingPath);

  auto *contextInitCallExpr = CallExpr::createImplicit(
      C, contextInitCall,
      {codingPathExpr, debugMessage,
       new (C) NilLiteralExpr(SourceLoc(), /* Implicit */ true)},
      {C.Id_codingPath, C.Id_debugDescription, C.Id_underlyingError});

  auto *decodingErrorTypeExpr =
      TypeExpr::createImplicit(errorDecl->getDeclaredType(), C);
  auto *decodingErrorCall = UnresolvedDotExpr::createImplicit(
      C, decodingErrorTypeExpr, C.Id_invalidValue,
      {Identifier(), Identifier()});

  auto *decodingErrorCallExpr = CallExpr::createImplicit(
      C, decodingErrorCall, {valueExpr, contextInitCallExpr},
      {Identifier(), Identifier()});
  return new (C) ThrowStmt(SourceLoc(), decodingErrorCallExpr);
}

/// Looks up the property corresponding to the indicated coding key.
///
/// \param conformanceDC The DeclContext we're generating code within.
/// \param elt The CodingKeys enum case.
/// \param targetDecl The type to look up properties in.
///
/// \return A tuple containing the \c VarDecl for the property, the type that
/// should be passed when decoding it, and a boolean which is true if
/// \c encodeIfPresent/\c decodeIfPresent should be used for this property.
static std::tuple<VarDecl *, Type, bool>
lookupVarDeclForCodingKeysCase(DeclContext *conformanceDC,
                               EnumElementDecl *elt,
                               NominalTypeDecl *targetDecl) {
  for (auto decl : targetDecl->lookupDirect(
                                   DeclName(elt->getBaseIdentifier()))) {
    if (auto *vd = dyn_cast<VarDecl>(decl)) {
      // If we found a property with an attached wrapper, retrieve the
      // backing property.
      if (auto backingVar = vd->getPropertyWrapperBackingProperty())
        vd = backingVar;

      if (!vd->isStatic()) {
        // This is the VarDecl we're looking for.

        auto varType =
            conformanceDC->mapTypeIntoContext(vd->getValueInterfaceType());

        bool useIfPresentVariant = false;

        if (auto objType = varType->getOptionalObjectType()) {
          varType = objType;
          useIfPresentVariant = true;
        }

        return std::make_tuple(vd, varType, useIfPresentVariant);
      }
    }
  }

  llvm_unreachable("Should have found at least 1 var decl");
}

/// Synthesizes the body for `func encode(to encoder: Encoder) throws`.
///
/// \param encodeDecl The function decl whose body to synthesize.
static std::pair<BraceStmt *, bool>
deriveBodyEncodable_encode(AbstractFunctionDecl *encodeDecl, void *) {
  // struct Foo : Codable {
  //   var x: Int
  //   var y: String
  //
  //   // Already derived by this point if possible.
  //   @derived enum CodingKeys : CodingKey {
  //     case x
  //     case y
  //   }
  //
  //   @derived func encode(to encoder: Encoder) throws {
  //     var container = encoder.container(keyedBy: CodingKeys.self)
  //     try container.encode(x, forKey: .x)
  //     try container.encode(y, forKey: .y)
  //   }
  // }

  // The enclosing type decl.
  auto conformanceDC = encodeDecl->getDeclContext();
  auto *targetDecl = conformanceDC->getSelfNominalTypeDecl();

  auto *funcDC = cast<DeclContext>(encodeDecl);
  auto &C = funcDC->getASTContext();

  // We'll want the CodingKeys enum for this type, potentially looking through
  // a typealias.
  auto *codingKeysEnum = lookupEvaluatedCodingKeysEnum(C, targetDecl);
  // We should have bailed already if:
  // a) The type does not have CodingKeys
  // b) The type is not an enum
  assert(codingKeysEnum && "Missing CodingKeys decl.");

  SmallVector<ASTNode, 5> statements;

  // Generate a reference to containerExpr ahead of time in case there are no
  // properties to encode or decode, but the type is a class which inherits from
  // something Codable and needs to encode super.

  // let container : KeyedEncodingContainer<CodingKeys>
  auto codingKeysType = codingKeysEnum->getDeclaredType();
  auto *containerDecl = createKeyedContainer(C, funcDC,
                                             C.getKeyedEncodingContainerDecl(),
                                             codingKeysEnum->getDeclaredInterfaceType(),
                                             VarDecl::Introducer::Var);

  auto *containerExpr = new (C) DeclRefExpr(ConcreteDeclRef(containerDecl),
                                            DeclNameLoc(), /*Implicit=*/true,
                                            AccessSemantics::DirectToStorage);

  // Need to generate
  //   `let container = encoder.container(keyedBy: CodingKeys.self)`
  // This is unconditional because a type with no properties should encode as an
  // empty container.
  //
  // `let container` (containerExpr) is generated above.

  // encoder
  auto encoderParam = encodeDecl->getParameters()->get(0);
  auto *encoderExpr = new (C) DeclRefExpr(ConcreteDeclRef(encoderParam),
                                          DeclNameLoc(), /*Implicit=*/true);

  // Bound encoder.container(keyedBy: CodingKeys.self) call
  auto containerType = containerDecl->getInterfaceType();
  auto *callExpr = createContainerKeyedByCall(C, funcDC, encoderExpr,
                                              containerType, codingKeysEnum);

  // Full `let container = encoder.container(keyedBy: CodingKeys.self)`
  // binding.
  auto *containerPattern = NamedPattern::createImplicit(C, containerDecl);
  auto *bindingDecl = PatternBindingDecl::createImplicit(
      C, StaticSpellingKind::None, containerPattern, callExpr, funcDC);
  statements.push_back(bindingDecl);
  statements.push_back(containerDecl);

  // Now need to generate `try container.encode(x, forKey: .x)` for all
  // existing properties. Optional properties get `encodeIfPresent`.
  for (auto *elt : codingKeysEnum->getAllElements()) {
    VarDecl *varDecl;
    Type varType;                // not used in Encodable synthesis
    bool useIfPresentVariant;

    std::tie(varDecl, varType, useIfPresentVariant) =
        lookupVarDeclForCodingKeysCase(conformanceDC, elt, targetDecl);

    // self.x
    auto *selfRef = DerivedConformance::createSelfDeclRef(encodeDecl);
    auto *varExpr = new (C) MemberRefExpr(selfRef, SourceLoc(),
                                          ConcreteDeclRef(varDecl),
                                          DeclNameLoc(), /*Implicit=*/true);

    // CodingKeys.x
    auto *metaTyRef = TypeExpr::createImplicit(codingKeysType, C);
    auto *keyExpr = new (C) MemberRefExpr(metaTyRef, SourceLoc(), elt,
                                          DeclNameLoc(), /*Implicit=*/true);

    // encode(_:forKey:)/encodeIfPresent(_:forKey:)
    auto methodName = useIfPresentVariant ? C.Id_encodeIfPresent : C.Id_encode;
    SmallVector<Identifier, 2> argNames{Identifier(), C.Id_forKey};

    auto *encodeCall = UnresolvedDotExpr::createImplicit(C, containerExpr,
                                                         methodName, argNames);

    // container.encode(self.x, forKey: CodingKeys.x)
    Expr *args[2] = {varExpr, keyExpr};
    auto *callExpr = CallExpr::createImplicit(C, encodeCall,
                                              C.AllocateCopy(args),
                                              C.AllocateCopy(argNames));

    // try container.encode(self.x, forKey: CodingKeys.x)
    auto *tryExpr = new (C) TryExpr(SourceLoc(), callExpr, Type(),
                                    /*Implicit=*/true);
    statements.push_back(tryExpr);
  }

  // Classes which inherit from something Codable should encode super as well.
  if (superclassConformsTo(dyn_cast<ClassDecl>(targetDecl),
                           KnownProtocolKind::Encodable)) {
    // Need to generate `try super.encode(to: container.superEncoder())`

    // superEncoder()
    auto *method = UnresolvedDeclRefExpr::createImplicit(C, C.Id_superEncoder);

    // container.superEncoder()
    auto *superEncoderRef = new (C) DotSyntaxCallExpr(containerExpr,
                                                      SourceLoc(), method);

    // encode(to:) expr
    auto *encodeDeclRef = new (C) DeclRefExpr(ConcreteDeclRef(encodeDecl),
                                              DeclNameLoc(), /*Implicit=*/true);

    // super
    auto *superRef = new (C) SuperRefExpr(encodeDecl->getImplicitSelfDecl(),
                                          SourceLoc(), /*Implicit=*/true);

    // super.encode(to:)
    auto *encodeCall = new (C) DotSyntaxCallExpr(superRef, SourceLoc(),
                                                 encodeDeclRef);

    // super.encode(to: container.superEncoder())
    Expr *args[1] = {superEncoderRef};
    Identifier argLabels[1] = {C.Id_to};
    auto *callExpr = CallExpr::createImplicit(C, encodeCall,
                                              C.AllocateCopy(args),
                                              C.AllocateCopy(argLabels));

    // try super.encode(to: container.superEncoder())
    auto *tryExpr = new (C) TryExpr(SourceLoc(), callExpr, Type(),
                                    /*Implicit=*/true);
    statements.push_back(tryExpr);
  }

  auto *body = BraceStmt::create(C, SourceLoc(), statements, SourceLoc(),
                                 /*implicit=*/true);
  return { body, /*isTypeChecked=*/false };
}

static std::pair<BraceStmt *, bool>
deriveBodyEncodable_enum_encode(AbstractFunctionDecl *encodeDecl, void *) {
  // enum Foo : Codable {
  //   case bar(x: Int)
  //   case baz(y: String)
  //
  //   // Already derived by this point if possible.
  //   @derived enum CodingKeys : CodingKey {
  //     case bar
  //     case baz
  //
  //     @derived enum BarCodingKeys : CodingKey {
  //       case x
  //     }
  //
  //     @derived enum BazCodingKeys : CodingKey {
  //       case y
  //     }
  //   }
  //
  //   @derived func encode(to encoder: Encoder) throws {
  //     var container = encoder.container(keyedBy: CodingKeys.self)
  //     switch self {
  //     case bar(let x):
  //       let nestedContainer = try container.nestedContainer(keyedBy:
  //       BarCodingKeys.self, forKey: .bar) try nestedContainer.encode(x,
  //       forKey: .x)
  //     case baz(let y):
  //       let nestedContainer = try container.nestedContainer(keyedBy:
  //       BazCodingKeys.self, forKey: .baz) try nestedContainer.encode(y,
  //       forKey: .y)
  //     }
  //   }
  // }

  // The enclosing type decl.
  auto conformanceDC = encodeDecl->getDeclContext();
  auto *enumDecl = conformanceDC->getSelfEnumDecl();

  auto *funcDC = cast<DeclContext>(encodeDecl);
  auto &C = funcDC->getASTContext();

  // We'll want the CodingKeys enum for this type, potentially looking through
  // a typealias.
  auto *codingKeysEnum = lookupEvaluatedCodingKeysEnum(C, enumDecl);
  // We should have bailed already if:
  // a) The type does not have CodingKeys
  // b) The type is not an enum
  assert(codingKeysEnum && "Missing CodingKeys decl.");

  SmallVector<ASTNode, 5> statements;

  // Generate a reference to containerExpr ahead of time in case there are no
  // properties to encode or decode, but the type is a class which inherits from
  // something Codable and needs to encode super.

  // let container : KeyedEncodingContainer<CodingKeys>
  auto *containerDecl =
      createKeyedContainer(C, funcDC, C.getKeyedEncodingContainerDecl(),
                           codingKeysEnum->getDeclaredInterfaceType(),
                           VarDecl::Introducer::Var);

  auto *containerExpr =
      new (C) DeclRefExpr(ConcreteDeclRef(containerDecl), DeclNameLoc(),
                          /*Implicit=*/true, AccessSemantics::DirectToStorage);

  // Need to generate
  //   `let container = encoder.container(keyedBy: CodingKeys.self)`
  // This is unconditional because a type with no properties should encode as an
  // empty container.
  //
  // `let container` (containerExpr) is generated above.

  // encoder
  auto encoderParam = encodeDecl->getParameters()->get(0);
  auto *encoderExpr = new (C) DeclRefExpr(ConcreteDeclRef(encoderParam),
                                          DeclNameLoc(), /*Implicit=*/true);

  // Bound encoder.container(keyedBy: CodingKeys.self) call
  auto containerType = containerDecl->getInterfaceType();
  auto *callExpr = createContainerKeyedByCall(C, funcDC, encoderExpr,
                                              containerType, codingKeysEnum);

  // Full `let container = encoder.container(keyedBy: CodingKeys.self)`
  // binding.
  auto *containerPattern = NamedPattern::createImplicit(C, containerDecl);
  auto *bindingDecl = PatternBindingDecl::createImplicit(
      C, StaticSpellingKind::None, containerPattern, callExpr, funcDC);
  statements.push_back(bindingDecl);
  statements.push_back(containerDecl);

  auto *selfRef = encodeDecl->getImplicitSelfDecl();

  SmallVector<ASTNode, 4> cases;
  for (auto elt : enumDecl->getAllElements()) {
    // CodingKeys.x
    auto *codingKeyCase =
        lookupEnumCase(C, codingKeysEnum, elt->getName().getBaseIdentifier());

    SmallVector<ASTNode, 3> caseStatements;

    // .<elt>(let a0, let a1, ...)
    SmallVector<VarDecl *, 3> payloadVars;
    auto subpattern = DerivedConformance::enumElementPayloadSubpattern(
        elt, 'a', encodeDecl, payloadVars, /* useLabels */ true);

    auto hasBoundDecls = !payloadVars.empty();
    Optional<MutableArrayRef<VarDecl *>> caseBodyVarDecls;
    if (hasBoundDecls) {
      // We allocated a direct copy of our var decls for the case
      // body.
      auto copy = C.Allocate<VarDecl *>(payloadVars.size());
      for (unsigned i : indices(payloadVars)) {
        auto *vOld = payloadVars[i];
        auto *vNew = new (C) VarDecl(
            /*IsStatic*/ false, vOld->getIntroducer(), vOld->getNameLoc(),
            vOld->getName(), vOld->getDeclContext());
        vNew->setImplicit();
        copy[i] = vNew;
      }
      caseBodyVarDecls.emplace(copy);
    }

    if (!codingKeyCase) {
      // This case should not be encodable, so throw an error if an attempt is
      // made to encode it
      llvm::SmallString<128> buffer;
      buffer.append("Case '");
      buffer.append(elt->getBaseIdentifier().str());
      buffer.append(
          "' cannot be decoded because it is not defined in CodingKeys.");
      auto *debugMessage = new (C) StringLiteralExpr(
          C.AllocateCopy(buffer.str()), SourceRange(), /* Implicit */ true);
      auto *selfRefExpr = new (C) DeclRefExpr(
          ConcreteDeclRef(selfRef), DeclNameLoc(), /* Implicit */ true);
      auto *throwStmt = createThrowEncodingErrorInvalidValueStmt(
          C, funcDC, selfRefExpr, containerExpr, debugMessage);
      caseStatements.push_back(throwStmt);
    } else {
      auto caseIdentifier = caseCodingKeysIdentifier(C, elt);
      auto *caseCodingKeys =
          lookupEvaluatedCodingKeysEnum(C, enumDecl, caseIdentifier);

      auto *nestedContainerDecl = createKeyedContainer(
          C, funcDC, C.getKeyedEncodingContainerDecl(),
          caseCodingKeys->getDeclaredInterfaceType(), VarDecl::Introducer::Var,
          C.Id_nestedContainer);

      auto *nestedContainerCall = createNestedContainerKeyedByForKeyCall(
          C, funcDC, containerExpr, caseCodingKeys, codingKeyCase);

      auto *containerPattern =
          NamedPattern::createImplicit(C, nestedContainerDecl);
      auto *bindingDecl = PatternBindingDecl::createImplicit(
          C, StaticSpellingKind::None, containerPattern, nestedContainerCall,
          funcDC);
      caseStatements.push_back(bindingDecl);
      caseStatements.push_back(nestedContainerDecl);

      // TODO: use param decls to get names
      for (auto entry : llvm::enumerate(payloadVars)) {
        auto *payloadVar = entry.value();
        auto *nestedContainerExpr = new (C)
            DeclRefExpr(ConcreteDeclRef(nestedContainerDecl), DeclNameLoc(),
                        /*Implicit=*/true, AccessSemantics::DirectToStorage);
        auto payloadVarRef = new (C) DeclRefExpr(payloadVar, DeclNameLoc(),
                                                 /*implicit*/ true);
        auto *paramDecl = elt->getParameterList()->get(entry.index());
        auto caseCodingKeysIdentifier = getVarNameForCoding(paramDecl);
        if (caseCodingKeysIdentifier.empty()) {
          caseCodingKeysIdentifier = C.getIdentifier("_" + std::to_string(entry.index()));
        }
        auto *caseCodingKey =
            lookupEnumCase(C, caseCodingKeys, caseCodingKeysIdentifier);

        // If there is no key defined for this parameter, skip it.
        if (!caseCodingKey)
          continue;

        auto varType = conformanceDC->mapTypeIntoContext(
            payloadVar->getValueInterfaceType());

        bool useIfPresentVariant = false;
        if (auto objType = varType->getOptionalObjectType()) {
          varType = objType;
          useIfPresentVariant = true;
        }

        // BarCodingKeys.x
        auto *metaTyRef =
            TypeExpr::createImplicit(caseCodingKeys->getDeclaredType(), C);
        auto *keyExpr =
            new (C) MemberRefExpr(metaTyRef, SourceLoc(), caseCodingKey,
                                  DeclNameLoc(), /*Implicit=*/true);

        // encode(_:forKey:)/encodeIfPresent(_:forKey:)
        auto methodName =
            useIfPresentVariant ? C.Id_encodeIfPresent : C.Id_encode;
        SmallVector<Identifier, 2> argNames{Identifier(), C.Id_forKey};

        auto *encodeCall = UnresolvedDotExpr::createImplicit(
            C, nestedContainerExpr, methodName, argNames);

        // nestedContainer.encode(x, forKey: CodingKeys.x)
        Expr *args[2] = {payloadVarRef, keyExpr};
        auto *callExpr = CallExpr::createImplicit(
            C, encodeCall, C.AllocateCopy(args), C.AllocateCopy(argNames));

        // try nestedContainer.encode(x, forKey: CodingKeys.x)
        auto *tryExpr = new (C) TryExpr(SourceLoc(), callExpr, Type(),
                                        /*Implicit=*/true);
        caseStatements.push_back(tryExpr);
      }
    }

    // generate: case .<Case>:
    auto enumType = funcDC->mapTypeIntoContext(enumDecl->getDeclaredInterfaceType());
    auto pat = new (C) EnumElementPattern(
        TypeExpr::createImplicit(enumType, C), SourceLoc(),
        DeclNameLoc(), DeclNameRef(), elt, subpattern);
    pat->setImplicit();

    auto labelItem = CaseLabelItem(pat);
    auto body = BraceStmt::create(C, SourceLoc(), caseStatements, SourceLoc());
    cases.push_back(CaseStmt::create(C, CaseParentKind::Switch, SourceLoc(),
                                     labelItem, SourceLoc(), SourceLoc(), body,
                                     /*case body vardecls*/ caseBodyVarDecls));
  }

  // generate: switch self { }
  auto enumRef =
      new (C) DeclRefExpr(ConcreteDeclRef(selfRef), DeclNameLoc(),
                          /*implicit*/ true, AccessSemantics::Ordinary);

  auto switchStmt =
      SwitchStmt::createImplicit(LabeledStmtInfo(), enumRef, cases, C);
  statements.push_back(switchStmt);

  auto *body = BraceStmt::create(C, SourceLoc(), statements, SourceLoc(),
                                 /*implicit=*/true);
  return {body, /*isTypeChecked=*/false};
}

/// Synthesizes a function declaration for `encode(to: Encoder) throws` with a
/// lazily synthesized body for the given type.
///
/// Adds the function declaration to the given type before returning it.
static FuncDecl *deriveEncodable_encode(DerivedConformance &derived) {
  auto &C = derived.Context;
  auto conformanceDC = derived.getConformanceContext();

  // Expected type: (Self) -> (Encoder) throws -> ()
  // Constructed as: func type
  //                 input: Self
  //                 throws
  //                 output: function type
  //                         input: Encoder
  //                         output: ()
  // Create from the inside out:

  auto encoderType = C.getEncoderDecl()->getDeclaredInterfaceType();
  auto returnType = TupleType::getEmpty(C);

  // Params: (Encoder)
  auto *encoderParam = new (C)
      ParamDecl(SourceLoc(), SourceLoc(), C.Id_to,
                SourceLoc(), C.Id_encoder, conformanceDC);
  encoderParam->setSpecifier(ParamSpecifier::Default);
  encoderParam->setInterfaceType(encoderType);
  encoderParam->setImplicit();

  ParameterList *params = ParameterList::createWithoutLoc(encoderParam);

  // Func name: encode(to: Encoder)
  DeclName name(C, C.Id_encode, params);
  auto *const encodeDecl = FuncDecl::createImplicit(
      C, StaticSpellingKind::None, name, /*NameLoc=*/SourceLoc(),
      /*Async=*/false,
      /*Throws=*/true, /*GenericParams=*/nullptr, params, returnType,
      conformanceDC);
  encodeDecl->setSynthesized();

  if (dyn_cast<EnumDecl>(derived.Nominal)) {
    encodeDecl->setBodySynthesizer(deriveBodyEncodable_enum_encode);
  } else {
    encodeDecl->setBodySynthesizer(deriveBodyEncodable_encode);
  }

  // This method should be marked as 'override' for classes inheriting Encodable
  // conformance from a parent class.
  if (superclassConformsTo(dyn_cast<ClassDecl>(derived.Nominal),
                           KnownProtocolKind::Encodable)) {
    auto *attr = new (C) OverrideAttr(/*IsImplicit=*/true);
    encodeDecl->getAttrs().add(attr);
  }

  encodeDecl->copyFormalAccessFrom(derived.Nominal,
                                   /*sourceIsParentContext*/ true);

  derived.addMembersToConformanceContext({encodeDecl});

  return encodeDecl;
}

/// Synthesizes the body for `init(from decoder: Decoder) throws`.
///
/// \param initDecl The function decl whose body to synthesize.
static std::pair<BraceStmt *, bool>
deriveBodyDecodable_init(AbstractFunctionDecl *initDecl, void *) {
  // struct Foo : Codable {
  //   var x: Int
  //   var y: String
  //
  //   // Already derived by this point if possible.
  //   @derived enum CodingKeys : CodingKey {
  //     case x
  //     case y
  //   }
  //
  //   @derived init(from decoder: Decoder) throws {
  //     let container = try decoder.container(keyedBy: CodingKeys.self)
  //     x = try container.decode(Type.self, forKey: .x)
  //     y = try container.decode(Type.self, forKey: .y)
  //   }
  // }

  // The enclosing type decl.
  auto conformanceDC = initDecl->getDeclContext();
  auto *targetDecl = conformanceDC->getSelfNominalTypeDecl();

  auto *funcDC = cast<DeclContext>(initDecl);
  auto &C = funcDC->getASTContext();

  // We'll want the CodingKeys enum for this type, potentially looking through
  // a typealias.
  auto *codingKeysEnum = lookupEvaluatedCodingKeysEnum(C, targetDecl);
  // We should have bailed already if:
  // a) The type does not have CodingKeys
  // b) The type is not an enum
  assert(codingKeysEnum && "Missing CodingKeys decl.");

  // Generate a reference to containerExpr ahead of time in case there are no
  // properties to encode or decode, but the type is a class which inherits from
  // something Codable and needs to decode super.

  // let container : KeyedDecodingContainer<CodingKeys>
  auto codingKeysType = codingKeysEnum->getDeclaredType();
  auto *containerDecl = createKeyedContainer(C, funcDC,
                                             C.getKeyedDecodingContainerDecl(),
                                             codingKeysEnum->getDeclaredInterfaceType(),
                                             VarDecl::Introducer::Let);

  auto *containerExpr = new (C) DeclRefExpr(ConcreteDeclRef(containerDecl),
                                            DeclNameLoc(), /*Implicit=*/true,
                                            AccessSemantics::DirectToStorage);

  SmallVector<ASTNode, 5> statements;
  auto enumElements = codingKeysEnum->getAllElements();
  if (!enumElements.empty()) {
    // Need to generate
    //   `let container = try decoder.container(keyedBy: CodingKeys.self)`
    // `let container` (containerExpr) is generated above.

    // decoder
    auto decoderParam = initDecl->getParameters()->get(0);
    auto *decoderExpr = new (C) DeclRefExpr(ConcreteDeclRef(decoderParam),
                                            DeclNameLoc(), /*Implicit=*/true);

    // Bound decoder.container(keyedBy: CodingKeys.self) call
    auto containerType = containerDecl->getInterfaceType();
    auto *callExpr = createContainerKeyedByCall(C, funcDC, decoderExpr,
                                                containerType, codingKeysEnum);

    // try decoder.container(keyedBy: CodingKeys.self)
    auto *tryExpr = new (C) TryExpr(SourceLoc(), callExpr, Type(),
                                    /*implicit=*/true);

    // Full `let container = decoder.container(keyedBy: CodingKeys.self)`
    // binding.
    auto *containerPattern = NamedPattern::createImplicit(C, containerDecl);
    auto *bindingDecl = PatternBindingDecl::createImplicit(
        C, StaticSpellingKind::None, containerPattern, tryExpr, funcDC);
    statements.push_back(bindingDecl);
    statements.push_back(containerDecl);

    // Now need to generate `x = try container.decode(Type.self, forKey: .x)`
    // for all existing properties. Optional properties get `decodeIfPresent`.
    for (auto *elt : enumElements) {
      VarDecl *varDecl;
      Type varType;
      bool useIfPresentVariant;

      std::tie(varDecl, varType, useIfPresentVariant) =
          lookupVarDeclForCodingKeysCase(conformanceDC, elt, targetDecl);

      // Don't output a decode statement for a let with an initial value.
      if (varDecl->isLet() && varDecl->isParentInitialized()) {
        // But emit a warning to let the user know that it won't be decoded.
        auto lookupResult =
            codingKeysEnum->lookupDirect(varDecl->getBaseName());
        auto keyExistsInCodingKeys =
            llvm::any_of(lookupResult, [&](ValueDecl *VD) {
              if (isa<EnumElementDecl>(VD)) {
                return VD->getBaseName() == varDecl->getBaseName();
              }
              return false;
            });
        auto *encodableProto = C.getProtocol(KnownProtocolKind::Encodable);
        bool conformsToEncodable =
            conformanceDC->getParentModule()->lookupConformance(
                targetDecl->getDeclaredInterfaceType(), encodableProto) != nullptr;

        // Strategy to use for CodingKeys enum diagnostic part - this is to
        // make the behaviour more explicit:
        //
        // 1. If we have an *implicit* CodingKeys enum:
        // (a) If the type is Decodable only, explicitly define the enum and
        //     remove the key from it. This makes it explicit that the key
        //     will not be decoded.
        // (b) If the type is Codable, explicitly define the enum and keep the
        //     key in it. This is because removing the key will break encoding
        //     which is mostly likely not what the user expects.
        //
        // 2. If we have an *explicit* CodingKeys enum:
        // (a) If the type is Decodable only and the key exists in the enum,
        //     then explicitly remove the key from the enum. This makes it
        //     explicit that the key will not be decoded.
        // (b) If the type is Decodable only and the key does not exist in
        //     the enum, do nothing. This is because the user has explicitly
        //     made it clear that that they don't want the key to be decoded.
        // (c) If the type is Codable, do nothing. This is because removing
        //     the key will break encoding which is most likely not what the
        //     user expects.
        if (!codingKeysEnum->isImplicit()) {
          if (conformsToEncodable || !keyExistsInCodingKeys) {
            continue;
          }
        }

        varDecl->diagnose(diag::decodable_property_will_not_be_decoded);
        if (codingKeysEnum->isImplicit()) {
          varDecl->diagnose(
              diag::decodable_property_init_or_codingkeys_implicit,
              conformsToEncodable ? 0 : 1, varDecl->getName());
        } else {
          varDecl->diagnose(
              diag::decodable_property_init_or_codingkeys_explicit,
              varDecl->getName());
        }
        if (auto *PBD = varDecl->getParentPatternBinding()) {
          varDecl->diagnose(diag::decodable_make_property_mutable)
              .fixItReplace(PBD->getLoc(), "var");
        }

        continue;
      }

      auto methodName =
          useIfPresentVariant ? C.Id_decodeIfPresent : C.Id_decode;

      // Type.self (where Type === type(of: x))
      // Calculating the metatype needs to happen after potential Optional
      // unwrapping in lookupVarDeclForCodingKeysCase().
      auto *metaTyRef = TypeExpr::createImplicit(varType, C);
      auto *targetExpr = new (C) DotSelfExpr(metaTyRef, SourceLoc(),
                                             SourceLoc(), varType);

      // CodingKeys.x
      metaTyRef = TypeExpr::createImplicit(codingKeysType, C);
      auto *keyExpr = new (C) MemberRefExpr(metaTyRef, SourceLoc(),
                                            elt, DeclNameLoc(), /*Implicit=*/true);

      // decode(_:forKey:)/decodeIfPresent(_:forKey:)
      SmallVector<Identifier, 2> argNames{Identifier(), C.Id_forKey};
      auto *decodeCall = UnresolvedDotExpr::createImplicit(
          C, containerExpr, methodName, argNames);

      // container.decode(Type.self, forKey: CodingKeys.x)
      Expr *args[2] = {targetExpr, keyExpr};
      auto *callExpr = CallExpr::createImplicit(C, decodeCall,
                                                C.AllocateCopy(args),
                                                C.AllocateCopy(argNames));

      // try container.decode(Type.self, forKey: CodingKeys.x)
      auto *tryExpr = new (C) TryExpr(SourceLoc(), callExpr, Type(),
                                      /*Implicit=*/true);

      auto *selfRef = DerivedConformance::createSelfDeclRef(initDecl);
      auto *varExpr = UnresolvedDotExpr::createImplicit(C, selfRef,
                                                        varDecl->getName());
      auto *assignExpr = new (C) AssignExpr(varExpr, SourceLoc(), tryExpr,
                                            /*Implicit=*/true);
      statements.push_back(assignExpr);
    }
  }

  // Classes which have a superclass must call super.init(from:) if the
  // superclass is Decodable, or super.init() if it is not.
  if (auto *classDecl = dyn_cast<ClassDecl>(targetDecl)) {
    if (auto *superclassDecl = classDecl->getSuperclassDecl()) {
      if (superclassConformsTo(classDecl, KnownProtocolKind::Decodable)) {
        // Need to generate `try super.init(from: container.superDecoder())`

        // container.superDecoder
        auto *superDecoderRef =
          UnresolvedDotExpr::createImplicit(C, containerExpr,
                                            C.Id_superDecoder);

        // container.superDecoder()
        auto *superDecoderCall =
          CallExpr::createImplicit(C, superDecoderRef, ArrayRef<Expr *>(),
                                   ArrayRef<Identifier>());

        // super
        auto *superRef = new (C) SuperRefExpr(initDecl->getImplicitSelfDecl(),
                                              SourceLoc(), /*Implicit=*/true);

        // super.init(from:)
        auto *initCall = UnresolvedDotExpr::createImplicit(
            C, superRef, DeclBaseName::createConstructor(), {C.Id_from});

        // super.decode(from: container.superDecoder())
        Expr *args[1] = {superDecoderCall};
        Identifier argLabels[1] = {C.Id_from};
        auto *callExpr = CallExpr::createImplicit(C, initCall,
                                                  C.AllocateCopy(args),
                                                  C.AllocateCopy(argLabels));

        // try super.init(from: container.superDecoder())
        auto *tryExpr = new (C) TryExpr(SourceLoc(), callExpr, Type(),
                                        /*Implicit=*/true);
        statements.push_back(tryExpr);
      } else {
        // The explicit constructor name is a compound name taking no arguments.
        DeclName initName(C, DeclBaseName::createConstructor(),
                          ArrayRef<Identifier>());

        // We need to look this up in the superclass to see if it throws.
        auto result = superclassDecl->lookupDirect(initName);

        // We should have bailed one level up if this were not available.
        assert(!result.empty());

        // If the init is failable, we should have already bailed one level
        // above.
        ConstructorDecl *superInitDecl = cast<ConstructorDecl>(result.front());
        assert(!superInitDecl->isFailable());

        // super
        auto *superRef = new (C) SuperRefExpr(initDecl->getImplicitSelfDecl(),
                                              SourceLoc(), /*Implicit=*/true);

        // super.init()
        auto *superInitRef = UnresolvedDotExpr::createImplicit(C, superRef,
                                                               initName);
        // super.init() call
        Expr *callExpr = CallExpr::createImplicit(C, superInitRef,
                                                  ArrayRef<Expr *>(),
                                                  ArrayRef<Identifier>());

        // If super.init throws, try super.init()
        if (superInitDecl->hasThrows())
          callExpr = new (C) TryExpr(SourceLoc(), callExpr, Type(),
                                     /*Implicit=*/true);

        statements.push_back(callExpr);
      }
    }
  }

  auto *body = BraceStmt::create(C, SourceLoc(), statements, SourceLoc(),
                                 /*implicit=*/true);
  return { body, /*isTypeChecked=*/false };
}

/// Synthesizes the body for `init(from decoder: Decoder) throws`.
///
/// \param initDecl The function decl whose body to synthesize.
static std::pair<BraceStmt *, bool>
deriveBodyDecodable_enum_init(AbstractFunctionDecl *initDecl, void *) {
  // enum Foo : Codable {
  //   case bar(x: Int)
  //   case baz(y: String)
  //
  //   // Already derived by this point if possible.
  //   @derived enum CodingKeys : CodingKey {
  //     case bar
  //     case baz
  //
  //     @derived enum BarCodingKeys : CodingKey {
  //       case x
  //     }
  //
  //     @derived enum BazCodingKeys : CodingKey {
  //       case y
  //     }
  //   }
  //
  //   @derived init(from decoder: Decoder) throws {
  //     let container = try decoder.container(keyedBy: CodingKeys.self)
  //     if container.allKeys.count != 1 {
  //       let context = DecodingError.Context(
  //           codingPath: container.codingPath,
  //           debugDescription: "Invalid number of keys found, expected one.")
  //       throw DecodingError.typeMismatch(Foo.self, context)
  //     }
  //     switch container.allKeys.first {
  //     case .bar:
  //       let nestedContainer = try container.nestedContainer(keyedBy:
  //       BarCodingKeys.self, forKey: .bar) let x = try
  //       nestedContainer.decode(Int.self, forKey: .x) self = .bar(x: x)
  //     case .baz:
  //       let nestedContainer = try container.nestedContainer(keyedBy:
  //       BarCodingKeys.self, forKey: .baz) let y = try
  //       nestedContainer.decode(String.self, forKey: .y) self = .baz(y: y)
  //     }
  //   }

  // The enclosing type decl.
  auto conformanceDC = initDecl->getDeclContext();
  auto *targetEnum = conformanceDC->getSelfEnumDecl();

  auto *funcDC = cast<DeclContext>(initDecl);
  auto &C = funcDC->getASTContext();

  // We'll want the CodingKeys enum for this type, potentially looking through
  // a typealias.
  auto *codingKeysEnum = lookupEvaluatedCodingKeysEnum(C, targetEnum);
  // We should have bailed already if:
  // a) The type does not have CodingKeys
  // b) The type is not an enum
  assert(codingKeysEnum && "Missing CodingKeys decl.");

  // Generate a reference to containerExpr ahead of time in case there are no
  // properties to encode or decode, but the type is a class which inherits from
  // something Codable and needs to decode super.

  // let container : KeyedDecodingContainer<CodingKeys>
  auto codingKeysType = codingKeysEnum->getDeclaredInterfaceType();
  auto *containerDecl =
      createKeyedContainer(C, funcDC, C.getKeyedDecodingContainerDecl(),
                           codingKeysEnum->getDeclaredInterfaceType(),
                           VarDecl::Introducer::Let);

  auto *containerExpr =
      new (C) DeclRefExpr(ConcreteDeclRef(containerDecl), DeclNameLoc(),
                          /*Implicit=*/true, AccessSemantics::DirectToStorage);

  SmallVector<ASTNode, 5> statements;
  if (codingKeysEnum->hasCases()) {
    // Need to generate
    //   `let container = try decoder.container(keyedBy: CodingKeys.self)`
    // `let container` (containerExpr) is generated above.

    // decoder
    auto decoderParam = initDecl->getParameters()->get(0);
    auto *decoderExpr = new (C) DeclRefExpr(ConcreteDeclRef(decoderParam),
                                            DeclNameLoc(), /*Implicit=*/true);

    // Bound decoder.container(keyedBy: CodingKeys.self) call
    auto containerType = containerDecl->getInterfaceType();
    auto *callExpr = createContainerKeyedByCall(C, funcDC, decoderExpr,
                                                containerType, codingKeysEnum);

    // try decoder.container(keyedBy: CodingKeys.self)
    auto *tryExpr = new (C) TryExpr(SourceLoc(), callExpr, Type(),
                                    /*implicit=*/true);

    // Full `let container = decoder.container(keyedBy: CodingKeys.self)`
    // binding.
    auto *containerPattern = NamedPattern::createImplicit(C, containerDecl);
    auto *bindingDecl = PatternBindingDecl::createImplicit(
        C, StaticSpellingKind::None, containerPattern, tryExpr, funcDC);
    statements.push_back(bindingDecl);
    statements.push_back(containerDecl);

    SmallVector<ASTNode, 3> cases;

    for (auto *elt : targetEnum->getAllElements()) {
      auto *codingKeyCase =
          lookupEnumCase(C, codingKeysEnum, elt->getName().getBaseIdentifier());

      // Skip this case if it's not defined in the CodingKeys
      if (!codingKeyCase)
        continue;

      // generate: case .<Case>:
      auto pat = new (C) EnumElementPattern(
          TypeExpr::createImplicit(funcDC->mapTypeIntoContext(codingKeysType),
                                   C),
          SourceLoc(), DeclNameLoc(), DeclNameRef(), codingKeyCase, nullptr);
      pat->setImplicit();
      pat->setType(codingKeysType);

      auto labelItem =
          CaseLabelItem(new (C) OptionalSomePattern(pat, SourceLoc()));

      llvm::SmallVector<ASTNode, 3> caseStatements;

      auto caseIdentifier = caseCodingKeysIdentifier(C, elt);
      auto *caseCodingKeys =
          lookupEvaluatedCodingKeysEnum(C, targetEnum, caseIdentifier);

      auto *nestedContainerDecl = createKeyedContainer(
          C, funcDC, C.getKeyedDecodingContainerDecl(),
          caseCodingKeys->getDeclaredInterfaceType(), VarDecl::Introducer::Var,
          C.Id_nestedContainer);

      auto *nestedContainerCall = createNestedContainerKeyedByForKeyCall(
          C, funcDC, containerExpr, caseCodingKeys, codingKeyCase);

      auto *tryNestedContainerCall = new (C) TryExpr(
          SourceLoc(), nestedContainerCall, Type(), /* Implicit */ true);

      auto *containerPattern =
          NamedPattern::createImplicit(C, nestedContainerDecl);
      auto *bindingDecl = PatternBindingDecl::createImplicit(
          C, StaticSpellingKind::None, containerPattern, tryNestedContainerCall,
          funcDC);
      caseStatements.push_back(bindingDecl);
      caseStatements.push_back(nestedContainerDecl);

      llvm::SmallVector<Expr *, 3> decodeCalls;
      llvm::SmallVector<Identifier, 3> params;
      if (elt->hasAssociatedValues()) {
        for (auto entry : llvm::enumerate(*elt->getParameterList())) {
          auto *paramDecl = entry.value();
          Identifier identifier = getVarNameForCoding(paramDecl);
          if (identifier.empty()) {
            identifier = C.getIdentifier("_" + std::to_string(entry.index()));
          }
          auto *caseCodingKey = lookupEnumCase(C, caseCodingKeys, identifier);

          params.push_back(getVarNameForCoding(paramDecl));

          // If no key is defined for this parameter, use the default value
          if (!caseCodingKey) {
            // This should have been verified to have a default expr in the
            // CodingKey synthesis
            assert(paramDecl->hasDefaultExpr());
            decodeCalls.push_back(paramDecl->getTypeCheckedDefaultExpr());
            continue;
          }

          // Type.self
          auto varType = funcDC->mapTypeIntoContext(
                  paramDecl->getValueInterfaceType());

          bool useIfPresentVariant = false;
          if (auto objType = varType->getOptionalObjectType()) {
            varType = objType;
            useIfPresentVariant = true;
          }

          auto *parameterTypeExpr = TypeExpr::createImplicit(varType, C);
          auto *parameterMetaTypeExpr =
              new (C) DotSelfExpr(parameterTypeExpr, SourceLoc(), SourceLoc());
          // BarCodingKeys.x
          auto *metaTyRef =
              TypeExpr::createImplicit(caseCodingKeys->getDeclaredType(), C);
          auto *keyExpr =
              new (C) MemberRefExpr(metaTyRef, SourceLoc(), caseCodingKey,
                                    DeclNameLoc(), /*Implicit=*/true);

          auto *nestedContainerExpr = new (C)
              DeclRefExpr(ConcreteDeclRef(nestedContainerDecl), DeclNameLoc(),
                          /*Implicit=*/true, AccessSemantics::DirectToStorage);

          // decode(_:, forKey:)
          auto methodName = useIfPresentVariant ? C.Id_decodeIfPresent
                                                : C.Id_decode;
          auto *decodeCall = UnresolvedDotExpr::createImplicit(
              C, nestedContainerExpr, methodName, {Identifier(), C.Id_forKey});

          // nestedContainer.decode(Type.self, forKey: BarCodingKeys.x)
          auto *callExpr = CallExpr::createImplicit(
              C, decodeCall, {parameterMetaTypeExpr, keyExpr},
              {Identifier(), C.Id_forKey});

          // try nestedContainer.decode(Type.self, forKey: BarCodingKeys.x)
          auto *tryExpr = new (C) TryExpr(SourceLoc(), callExpr, Type(),
                                          /*Implicit=*/true);

          decodeCalls.push_back(tryExpr);
        }
      }

      auto *selfRef = DerivedConformance::createSelfDeclRef(initDecl);

      // Foo.bar
      auto *selfTypeExpr =
          TypeExpr::createImplicit(targetEnum->getDeclaredType(), C);

      if (params.empty()) {
        auto *selfCaseExpr = new (C) MemberRefExpr(
            selfTypeExpr, SourceLoc(), elt, DeclNameLoc(), /*Implicit=*/true);

        auto *selfRef = DerivedConformance::createSelfDeclRef(initDecl);

        auto *assignExpr =
            new (C) AssignExpr(selfRef, SourceLoc(), selfCaseExpr,
                               /*Implicit=*/true);

        caseStatements.push_back(assignExpr);
      } else {
        // Foo.bar(x:)
        auto *selfCaseExpr = UnresolvedDotExpr::createImplicit(
            C, selfTypeExpr, elt->getBaseIdentifier(), C.AllocateCopy(params));

        // Foo.bar(x: try nestedContainer.decode(Int.self, forKey: .x))
        auto *caseCallExpr = CallExpr::createImplicit(
            C, selfCaseExpr, C.AllocateCopy(decodeCalls),
            C.AllocateCopy(params));

        // self = Foo.bar(x: try nestedContainer.decode(Int.self))
        auto *assignExpr =
            new (C) AssignExpr(selfRef, SourceLoc(), caseCallExpr,
                               /*Implicit=*/true);

        caseStatements.push_back(assignExpr);
      }

      auto body =
          BraceStmt::create(C, SourceLoc(), caseStatements, SourceLoc());

      cases.push_back(CaseStmt::create(C, CaseParentKind::Switch, SourceLoc(),
                                       labelItem, SourceLoc(), SourceLoc(),
                                       body,
                                       /*case body vardecls*/ None));
    }

    // generate:
    //
    //  if container.allKeys.count != 1 {
    //    let context = DecodingError.Context(
    //            codingPath: container.codingPath,
    //            debugDescription: "Invalid number of keys found, expected
    //            one.")
    //    throw DecodingError.typeMismatch(Foo.self, context)
    //  }
    auto *debugMessage = new (C) StringLiteralExpr(
        StringRef("Invalid number of keys found, expected one."), SourceRange(),
        /* Implicit */ true);
    auto *throwStmt = createThrowDecodingErrorTypeMismatchStmt(
        C, funcDC, targetEnum, containerExpr, debugMessage);

    // container.allKeys
    auto *allKeysExpr =
        UnresolvedDotExpr::createImplicit(C, containerExpr, C.Id_allKeys);

    // container.allKeys.count
    auto *keysCountExpr =
        UnresolvedDotExpr::createImplicit(C, allKeysExpr, C.Id_count);

    // container.allKeys.count == 1
    auto *cmpFunc = C.getEqualIntDecl();
    auto *fnType = cmpFunc->getInterfaceType()->castTo<FunctionType>();
    auto *cmpFuncExpr = new (C)
        DeclRefExpr(cmpFunc, DeclNameLoc(),
                    /*implicit*/ true, AccessSemantics::Ordinary, fnType);
    auto *oneExpr = IntegerLiteralExpr::createFromUnsigned(C, 1);

    auto *tupleExpr = TupleExpr::createImplicit(C, {keysCountExpr, oneExpr},
                                                {Identifier(), Identifier()});

    auto *cmpExpr =
        new (C) BinaryExpr(cmpFuncExpr, tupleExpr, /*implicit*/ true);
    cmpExpr->setThrows(false);

    auto *guardBody = BraceStmt::create(C, SourceLoc(), {throwStmt},
                                        SourceLoc(), /* Implicit */ true);

    auto *guardStmt = new (C)
        GuardStmt(SourceLoc(), cmpExpr, guardBody, /* Implicit */ true, C);

    statements.push_back(guardStmt);

    // generate: switch container.allKeys.first { }
    auto *firstExpr =
        UnresolvedDotExpr::createImplicit(C, allKeysExpr, C.Id_first);

    auto switchStmt =
        SwitchStmt::createImplicit(LabeledStmtInfo(), firstExpr, cases, C);

    statements.push_back(switchStmt);
  }

  auto *body = BraceStmt::create(C, SourceLoc(), statements, SourceLoc(),
                                 /*implicit=*/true);
  return {body, /*isTypeChecked=*/false};
}

/// Synthesizes a function declaration for `init(from: Decoder) throws` with a
/// lazily synthesized body for the given type.
///
/// Adds the function declaration to the given type before returning it.
static ValueDecl *deriveDecodable_init(DerivedConformance &derived) {
  auto &C = derived.Context;

  auto classDecl = dyn_cast<ClassDecl>(derived.Nominal);
  auto conformanceDC = derived.getConformanceContext();

  // Expected type: (Self) -> (Decoder) throws -> (Self)
  // Constructed as: func type
  //                 input: Self
  //                 throws
  //                 output: function type
  //                         input: Encoder
  //                         output: Self
  // Compute from the inside out:

  // Params: (Decoder)
  auto decoderType = C.getDecoderDecl()->getDeclaredInterfaceType();
  auto *decoderParamDecl = new (C) ParamDecl(
      SourceLoc(), SourceLoc(), C.Id_from,
      SourceLoc(), C.Id_decoder, conformanceDC);
  decoderParamDecl->setImplicit();
  decoderParamDecl->setSpecifier(ParamSpecifier::Default);
  decoderParamDecl->setInterfaceType(decoderType);

  auto *paramList = ParameterList::createWithoutLoc(decoderParamDecl);

  // Func name: init(from: Decoder)
  DeclName name(C, DeclBaseName::createConstructor(), paramList);

  auto *initDecl =
      new (C) ConstructorDecl(name, SourceLoc(),
                              /*Failable=*/false,SourceLoc(),
                              /*Async=*/false, /*AsyncLoc=*/SourceLoc(),
                              /*Throws=*/true, SourceLoc(), paramList,
                              /*GenericParams=*/nullptr, conformanceDC);
  initDecl->setImplicit();
  initDecl->setSynthesized();

  if (dyn_cast<EnumDecl>(derived.Nominal)) {
    initDecl->setBodySynthesizer(&deriveBodyDecodable_enum_init);
  } else {
    initDecl->setBodySynthesizer(&deriveBodyDecodable_init);
  }

  // This constructor should be marked as `required` for non-final classes.
  if (classDecl && !classDecl->isSemanticallyFinal()) {
    auto *reqAttr = new (C) RequiredAttr(/*IsImplicit=*/true);
    initDecl->getAttrs().add(reqAttr);
  }

  initDecl->copyFormalAccessFrom(derived.Nominal,
                                 /*sourceIsParentContext*/ true);

  derived.addMembersToConformanceContext({initDecl});

  return initDecl;
}

/// Returns whether the given type is valid for synthesizing {En,De}codable.
///
/// Checks to see whether the given type has a valid \c CodingKeys enum, and if
/// not, attempts to synthesize one for it.
///
/// \param requirement The requirement we want to synthesize.
static bool canSynthesize(DerivedConformance &derived, ValueDecl *requirement) {
  // Before we attempt to look up (or more importantly, synthesize) a CodingKeys
  // entity on target, we need to make sure the type is otherwise valid.
  //
  // If we are synthesizing Decodable and the target is a class with a
  // superclass, our synthesized init(from:) will need to call either
  // super.init(from:) or super.init() depending on whether the superclass is
  // Decodable itself.
  //
  // If the required initializer is not available, we shouldn't attempt to
  // synthesize CodingKeys.
  auto proto = derived.Protocol;
  auto *classDecl = dyn_cast<ClassDecl>(derived.Nominal);
  if (proto->isSpecificProtocol(KnownProtocolKind::Decodable) && classDecl) {
    if (auto *superclassDecl = classDecl->getSuperclassDecl()) {
      DeclName memberName;
      auto superType = superclassDecl->getDeclaredInterfaceType();
      if (TypeChecker::conformsToProtocol(superType, proto, superclassDecl)) {
        // super.init(from:) must be accessible.
        memberName = cast<ConstructorDecl>(requirement)->getName();
      } else {
        // super.init() must be accessible.
        // Passing an empty params array constructs a compound name with no
        // arguments (as opposed to a simple name when omitted).
        memberName =
            DeclName(derived.Context, DeclBaseName::createConstructor(),
                     ArrayRef<Identifier>());
      }

      auto result =
          TypeChecker::lookupMember(superclassDecl, superType,
                                    DeclNameRef(memberName));

      if (result.empty()) {
        // No super initializer for us to call.
        superclassDecl->diagnose(diag::decodable_no_super_init_here,
                                 requirement->getName(), memberName);
        return false;
      } else if (result.size() > 1) {
        // There are multiple results for this lookup. We'll end up producing a
        // diagnostic later complaining about duplicate methods (if we haven't
        // already), so just bail with a general error.
        return false;
      } else {
        auto *initializer =
          cast<ConstructorDecl>(result.front().getValueDecl());
        auto conformanceDC = derived.getConformanceContext();
        if (!initializer->isDesignatedInit()) {
          // We must call a superclass's designated initializer.
          initializer->diagnose(diag::decodable_super_init_not_designated_here,
                                requirement->getName(), memberName);
          return false;
        } else if (!initializer->isAccessibleFrom(conformanceDC)) {
          // Cannot call an inaccessible method.
          auto accessScope = initializer->getFormalAccessScope(conformanceDC);
          initializer->diagnose(diag::decodable_inaccessible_super_init_here,
                                requirement->getName(), memberName,
                                accessScope.accessLevelForDiagnostics());
          return false;
        } else if (initializer->isFailable()) {
          // We can't call super.init() if it's failable, since init(from:)
          // isn't failable.
          initializer->diagnose(diag::decodable_super_init_is_failable_here,
                                requirement->getName(), memberName);
          return false;
        }
      }
    }
  }

  if (!validateCodingKeysEnum(derived)) {
    return false;
  }

  bool allValid = true;
  if (auto *enumDecl = dyn_cast<EnumDecl>(derived.Nominal)) {
    llvm::SmallSetVector<Identifier, 4> caseNames;
    for (auto *elementDecl : enumDecl->getAllElements()) {
      bool duplicate = false;
      if (!caseNames.insert(elementDecl->getBaseIdentifier())) {
        elementDecl->diagnose(diag::codable_enum_duplicate_case_name_here,
                             derived.getProtocolType(),
                             derived.Nominal->getDeclaredType(),
                             elementDecl->getBaseIdentifier());
        allValid = false;
        duplicate = true;
      }

      if (elementDecl->hasAssociatedValues()) {
        llvm::SmallMapVector<Identifier, ParamDecl *, 4> params;
        for (auto entry : llvm::enumerate(*elementDecl->getParameterList())) {
          auto *paramDecl = entry.value();
          Identifier paramIdentifier = getVarNameForCoding(paramDecl);
          bool generatedName = false;
          if (paramIdentifier.empty()) {
            paramIdentifier = derived.Context.getIdentifier("_" + std::to_string(entry.index()));
            generatedName = true;
          }
          auto inserted = params.insert(std::make_pair(paramIdentifier, paramDecl));
          if (!inserted.second) {
            // duplicate identifier found
            auto userDefinedParam = paramDecl;
            if (generatedName) {
              // at most we have one user defined and one generated identifier
              // with this name, so if this is the generated, the other one
              // must be the user defined
              userDefinedParam = inserted.first->second;
            }

            userDefinedParam->diagnose(diag::codable_enum_duplicate_parameter_name_here,
                                  derived.getProtocolType(),
                                  derived.Nominal->getDeclaredType(),
                                  paramIdentifier,
                                  elementDecl->getBaseIdentifier());
            allValid = false;
          }
        }
      }

      if (!duplicate && !validateCaseCodingKeysEnum(derived, elementDecl)) {
        allValid = false;
      }
    }
  }

  return allValid;
}

static bool canDeriveCodable(NominalTypeDecl *NTD,
                             KnownProtocolKind Kind) {
  assert(Kind == KnownProtocolKind::Encodable ||
         Kind == KnownProtocolKind::Decodable);

  // Structs, classes and enums can explicitly derive Encodable and Decodable
  // conformance (explicitly meaning we can synthesize an implementation if
  // a type conforms manually).
  if (!isa<StructDecl>(NTD) && !isa<ClassDecl>(NTD) && !isa<EnumDecl>(NTD)) {
    return false;
  }

  auto *PD = NTD->getASTContext().getProtocol(Kind);
  if (!PD) {
    return false;
  }

  return true;
}

bool DerivedConformance::canDeriveDecodable(NominalTypeDecl *NTD) {
  return canDeriveCodable(NTD, KnownProtocolKind::Decodable);
}

bool DerivedConformance::canDeriveEncodable(NominalTypeDecl *NTD) {
  return canDeriveCodable(NTD, KnownProtocolKind::Encodable);
}

ValueDecl *DerivedConformance::deriveEncodable(ValueDecl *requirement) {
  // We can only synthesize Encodable for structs and classes.
  if (!isa<StructDecl>(Nominal) && !isa<ClassDecl>(Nominal) &&
      !isa<EnumDecl>(Nominal))
    return nullptr;

  if (requirement->getBaseName() != Context.Id_encode) {
    // Unknown requirement.
    requirement->diagnose(diag::broken_encodable_requirement);
    return nullptr;
  }

  if (checkAndDiagnoseDisallowedContext(requirement))
    return nullptr;

  // Check other preconditions for synthesized conformance.
  // This synthesizes a CodingKeys enum if possible.
  if (!canSynthesize(*this, requirement)) {
    ConformanceDecl->diagnose(diag::type_does_not_conform,
                              Nominal->getDeclaredType(), getProtocolType());
    requirement->diagnose(diag::no_witnesses, diag::RequirementKind::Func,
                          requirement->getName(), getProtocolType(),
                          /*AddFixIt=*/false);

    return nullptr;
  }

  return deriveEncodable_encode(*this);
}

ValueDecl *DerivedConformance::deriveDecodable(ValueDecl *requirement) {
  // We can only synthesize Encodable for structs and classes.
  if (!isa<StructDecl>(Nominal) && !isa<ClassDecl>(Nominal) &&
      !isa<EnumDecl>(Nominal))
    return nullptr;

  if (requirement->getBaseName() != DeclBaseName::createConstructor()) {
    // Unknown requirement.
    requirement->diagnose(diag::broken_decodable_requirement);
    return nullptr;
  }

  if (checkAndDiagnoseDisallowedContext(requirement))
    return nullptr;

  // Check other preconditions for synthesized conformance.
  // This synthesizes a CodingKeys enum if possible.
  if (!canSynthesize(*this, requirement)) {
    ConformanceDecl->diagnose(diag::type_does_not_conform,
                              Nominal->getDeclaredType(), getProtocolType());
    requirement->diagnose(diag::no_witnesses, diag::RequirementKind::Constructor,
                          requirement->getName(), getProtocolType(),
                          /*AddFixIt=*/false);

    return nullptr;
  }

  return deriveDecodable_init(*this);
}
