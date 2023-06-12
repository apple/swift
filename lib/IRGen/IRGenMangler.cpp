//===--- IRGenMangler.cpp - mangling of IRGen symbols ---------------------===//
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

#include "IRGenMangler.h"
#include "ExtendedExistential.h"
#include "GenClass.h"
#include "swift/AST/ExistentialLayout.h"
#include "swift/AST/IRGenOptions.h"
#include "swift/AST/ProtocolAssociations.h"
#include "swift/AST/ProtocolConformance.h"
#include "swift/Basic/Platform.h"
#include "swift/Demangling/ManglingMacros.h"
#include "swift/Demangling/Demangle.h"
#include "swift/ABI/MetadataValues.h"
#include "swift/ClangImporter/ClangModule.h"
#include "llvm/Support/SaveAndRestore.h"

using namespace swift;
using namespace irgen;

const char *getManglingForWitness(swift::Demangle::ValueWitnessKind kind) {
  switch (kind) {
#define VALUE_WITNESS(MANGLING, NAME) \
  case swift::Demangle::ValueWitnessKind::NAME: return #MANGLING;
#include "swift/Demangling/ValueWitnessMangling.def"
  }
  llvm_unreachable("not a function witness");
}

std::string IRGenMangler::mangleValueWitness(Type type, ValueWitness witness) {
  beginMangling();
  appendType(type, nullptr);

  const char *Code = nullptr;
  switch (witness) {
#define GET_MANGLING(ID) \
    case ValueWitness::ID: Code = getManglingForWitness(swift::Demangle::ValueWitnessKind::ID); break;
    GET_MANGLING(InitializeBufferWithCopyOfBuffer) \
    GET_MANGLING(Destroy) \
    GET_MANGLING(InitializeWithCopy) \
    GET_MANGLING(AssignWithCopy) \
    GET_MANGLING(InitializeWithTake) \
    GET_MANGLING(AssignWithTake) \
    GET_MANGLING(GetEnumTagSinglePayload) \
    GET_MANGLING(StoreEnumTagSinglePayload) \
    GET_MANGLING(GetEnumTag) \
    GET_MANGLING(DestructiveProjectEnumData) \
    GET_MANGLING(DestructiveInjectEnumTag)
#undef GET_MANGLING
    case ValueWitness::Size:
    case ValueWitness::Flags:
    case ValueWitness::ExtraInhabitantCount:
    case ValueWitness::Stride:
      llvm_unreachable("not a function witness");
  }
  appendOperator("w", Code);
  return finalize();
}

std::string IRGenMangler::manglePartialApplyForwarder(StringRef FuncName) {
  if (FuncName.empty()) {
    beginMangling();
  } else {
    if (FuncName.startswith(MANGLING_PREFIX_STR)) {
      Buffer << FuncName;
    } else {
      beginMangling();
      appendIdentifier(FuncName);
    }
  }
  appendOperator("TA");
  return finalize();
}

SymbolicMangling
IRGenMangler::withSymbolicReferences(IRGenModule &IGM,
                                  llvm::function_ref<void ()> body) {
  Mod = IGM.getSwiftModule();
  configureForSymbolicMangling();

  llvm::SaveAndRestore<bool>
    AllowSymbolicReferencesLocally(AllowSymbolicReferences);
  llvm::SaveAndRestore<std::function<bool (SymbolicReferent)>>
    CanSymbolicReferenceLocally(CanSymbolicReference);

  AllowSymbolicReferences = true;
  CanSymbolicReference = [&](SymbolicReferent s) -> bool {
    switch (s.getKind()) {
    case SymbolicReferent::NominalType: {
      auto type = s.getNominalType();
      // The short-substitution types in the standard library have compact
      // manglings already, and the runtime ought to have a lookup table for
      // them. Symbolic referencing would be wasteful.
      if (AllowStandardSubstitutions
          && type->getModuleContext()->hasStandardSubstitutions()
          && Mangle::getStandardTypeSubst(
               type->getName().str(), AllowConcurrencyStandardSubstitutions)) {
        return false;
      }
      
      // TODO: We could assign a symbolic reference discriminator to refer
      // to objc protocol refs.
      if (auto proto = dyn_cast<ProtocolDecl>(type)) {
        if (proto->isObjC()) {
          return false;
        }
      }

      // Classes defined in Objective-C don't have descriptors.
      // TODO: We could assign a symbolic reference discriminator to refer
      // to objc class refs.
      if (auto clazz = dyn_cast<ClassDecl>(type)) {
        // Swift-defined classes can be symbolically referenced.
        if (hasKnownSwiftMetadata(IGM, const_cast<ClassDecl*>(clazz)))
          return true;

        // Foreign class types can be symbolically referenced.
        if (clazz->getForeignClassKind() == ClassDecl::ForeignKind::CFType ||
            const_cast<ClassDecl *>(clazz)->isForeignReferenceType())
          return true;

        // Otherwise no.
        return false;
      }

      return true;
    }
    case SymbolicReferent::OpaqueType:
      // Always symbolically reference opaque types.
      return true;
    case SymbolicReferent::ExtendedExistentialTypeShape:
      // Always symbolically reference extended existential type shapes.
      return true;
    }
    llvm_unreachable("symbolic referent not handled");
  };

  SymbolicReferences.clear();
  
  body();
  
  return {finalize(), std::move(SymbolicReferences)};
}

SymbolicMangling
IRGenMangler::mangleTypeForReflection(IRGenModule &IGM,
                                      CanGenericSignature Sig,
                                      CanType Ty) {
  // If our target predates Swift 5.5, we cannot apply the standard
  // substitutions for types defined in the Concurrency module.
  ASTContext &ctx = Ty->getASTContext();
  llvm::SaveAndRestore<bool> savedConcurrencyStandardSubstitutions(
      AllowConcurrencyStandardSubstitutions);
  if (auto runtimeCompatVersion = getSwiftRuntimeCompatibilityVersionForTarget(
          ctx.LangOpts.Target)) {
    if (*runtimeCompatVersion < llvm::VersionTuple(5, 5))
      AllowConcurrencyStandardSubstitutions = false;
  }

  llvm::SaveAndRestore<bool> savedAllowStandardSubstitutions(
      AllowStandardSubstitutions);
  if (IGM.getOptions().DisableStandardSubstitutionsInReflectionMangling)
    AllowStandardSubstitutions = false;

  llvm::SaveAndRestore<bool> savedAllowMarkerProtocols(
      AllowMarkerProtocols, false);
  return withSymbolicReferences(IGM, [&]{
    appendType(Ty, Sig);
  });
}

SymbolicMangling
IRGenMangler::mangleTypeForFlatUniqueTypeRef(CanGenericSignature sig,
                                             CanType type) {
  // Use runtime information like we would for a symbolic mangling,
  // just don't allow actual symbolic references anywhere in the
  // mangled name.
  configureForSymbolicMangling();

  // We don't make the substitution adjustments above because they're
  // target-specific and so would break the goal of getting a unique
  // string.
  appendType(type, sig);

  assert(SymbolicReferences.empty());
  return {finalize(), {}};
}

std::string IRGenMangler::mangleProtocolConformanceDescriptor(
                                 const RootProtocolConformance *conformance) {
  beginMangling();
  if (isa<NormalProtocolConformance>(conformance)) {
    appendProtocolConformance(conformance);
    appendOperator("Mc");
  } else {
    auto protocol = cast<SelfProtocolConformance>(conformance)->getProtocol();
    appendProtocolName(protocol);
    appendOperator("MS");
  }
  return finalize();
}

std::string IRGenMangler::mangleProtocolConformanceDescriptorRecord(
                                 const RootProtocolConformance *conformance) {
  beginMangling();
  appendProtocolConformance(conformance);
  appendOperator("Hc");
  return finalize();
}

std::string IRGenMangler::mangleProtocolConformanceInstantiationCache(
                                 const RootProtocolConformance *conformance) {
  beginMangling();
  if (isa<NormalProtocolConformance>(conformance)) {
    appendProtocolConformance(conformance);
    appendOperator("Mc");
  } else {
    auto protocol = cast<SelfProtocolConformance>(conformance)->getProtocol();
    appendProtocolName(protocol);
    appendOperator("MS");
  }
  appendOperator("MK");
  return finalize();
}

std::string IRGenMangler::mangleTypeForLLVMTypeName(CanType Ty) {
  // To make LLVM IR more readable we always add a 'T' prefix so that type names
  // don't start with a digit and don't need to be quoted.
  Buffer << 'T';
  if (Ty->is<ExistentialType>() && Ty->hasParameterizedExistential()) {
    appendType(Ty, nullptr);
  } else {
    if (auto existential = Ty->getAs<ExistentialType>())
      Ty = existential->getConstraintType()->getCanonicalType();
    if (auto P = dyn_cast<ProtocolType>(Ty)) {
      appendProtocolName(P->getDecl(), /*allowStandardSubstitution=*/false);
      appendOperator("P");
    } else {
      appendType(Ty, nullptr);
    }
  }
  return finalize();
}

std::string IRGenMangler::
mangleProtocolForLLVMTypeName(ProtocolCompositionType *type) {
  ExistentialLayout layout = type->getExistentialLayout();

  if (type->isAny()) {
    Buffer << "Any";
  } else if (layout.isAnyObject()) {
    Buffer << "AnyObject";
  } else {
    // To make LLVM IR more readable we always add a 'T' prefix so that type names
    // don't start with a digit and don't need to be quoted.
    Buffer << 'T';
    auto protocols = layout.getProtocols();
    for (unsigned i = 0, e = protocols.size(); i != e; ++i) {
      appendProtocolName(protocols[i]);
      if (i == 0)
        appendOperator("_");
    }
    if (auto superclass = layout.explicitSuperclass) {
      // We share type infos for different instantiations of a generic type
      // when the archetypes have the same exemplars.  We cannot mangle
      // archetypes, and the mangling does not have to be unique, so we just
      // mangle the unbound generic form of the type.
      if (superclass->hasArchetype()) {
        superclass = superclass->getClassOrBoundGenericClass()
          ->getDeclaredType();
      }

      appendType(CanType(superclass), nullptr);
      appendOperator("Xc");
    } else if (layout.getLayoutConstraint()) {
      appendOperator("Xl");
    } else {
      appendOperator("p");
    }
  }
  return finalize();
}

std::string IRGenMangler::
mangleSymbolNameForSymbolicMangling(const SymbolicMangling &mangling,
                                    MangledTypeRefRole role) {
  beginManglingWithoutPrefix();
  const char *prefix;
  switch (role) {
  case MangledTypeRefRole::DefaultAssociatedTypeWitness:
    prefix = "default assoc type ";
    break;

  case MangledTypeRefRole::FieldMetadata:
  case MangledTypeRefRole::Metadata:
  case MangledTypeRefRole::Reflection:
    prefix = "symbolic ";
    break;

  case MangledTypeRefRole::FlatUnique:
    prefix = "flat unique ";
    assert(mangling.SymbolicReferences.empty());
    break;
  }
  auto prefixLen = strlen(prefix);

  Buffer << prefix << mangling.String;

  for (auto &symbol : mangling.SymbolicReferences) {
    // Fill in the placeholder space with something printable.
    auto referent = symbol.first;
    auto offset = symbol.second;
    Storage[prefixLen + offset]
      = Storage[prefixLen + offset+1]
      = Storage[prefixLen + offset+2]
      = Storage[prefixLen + offset+3]
      = Storage[prefixLen + offset+4]
      = '_';
    Buffer << ' ';
    switch (referent.getKind()) {
    case SymbolicReferent::NominalType: {
      auto ty = referent.getNominalType();
      appendContext(ty, ty->getAlternateModuleName());
      continue;
    }
    case SymbolicReferent::OpaqueType: {
      appendOpaqueDeclName(referent.getOpaqueType());
      continue;
    }
    case SymbolicReferent::ExtendedExistentialTypeShape: {
      auto existentialType = referent.getType()->getCanonicalType();
      auto shapeInfo =
        ExtendedExistentialTypeShapeInfo::get(existentialType);
      appendExtendedExistentialTypeShapeSymbol(shapeInfo.genSig,
                                               shapeInfo.shapeType,
                                               shapeInfo.isUnique());
      continue;
    }
    }
    llvm_unreachable("unhandled referent");
  }
  
  return finalize();
}

std::string IRGenMangler::mangleSymbolNameForAssociatedConformanceWitness(
                                  const NormalProtocolConformance *conformance,
                                  CanType associatedType,
                                  const ProtocolDecl *proto) {
  beginManglingWithoutPrefix();
  if (conformance) {
    Buffer << "associated conformance ";
    appendProtocolConformance(conformance);
  } else {
    Buffer << "default associated conformance";
  }

  bool isFirstAssociatedTypeIdentifier = true;
  appendAssociatedTypePath(associatedType, isFirstAssociatedTypeIdentifier);
  appendProtocolName(proto);
  return finalize();
}

std::string IRGenMangler::mangleSymbolNameForMangledMetadataAccessorString(
                                           const char *kind,
                                           CanGenericSignature genericSig,
                                           CanType type) {
  beginManglingWithoutPrefix();
  Buffer << kind << " ";

  if (genericSig)
    appendGenericSignature(genericSig);

  if (type)
    appendType(type, genericSig);
  return finalize();
}

std::string IRGenMangler::mangleSymbolNameForMangledConformanceAccessorString(
                                           const char *kind,
                                           CanGenericSignature genericSig,
                                           CanType type,
                                           ProtocolConformanceRef conformance) {
  beginManglingWithoutPrefix();
  Buffer << kind << " ";

  if (genericSig)
    appendGenericSignature(genericSig);

  appendAnyProtocolConformance(genericSig, type, conformance);
  return finalize();
}

std::string IRGenMangler::mangleSymbolNameForMangledGetEnumTagForLayoutString(
    CanType type) {
  beginManglingWithoutPrefix();
  Buffer << "get_enum_tag_for_layout_string"
         << " ";

  appendType(type, nullptr);
  return finalize();
}

std::string IRGenMangler::mangleSymbolNameForUnderlyingTypeAccessorString(
    OpaqueTypeDecl *opaque, unsigned index) {
  beginManglingWithoutPrefix();
  Buffer << "get_underlying_type_ref ";

  appendContextOf(opaque);
  appendOpaqueDeclName(opaque);

  if (index == 0) {
    appendOperator("Qr");
  } else {
    appendOperator("QR", Index(index));
  }

  return finalize();
}

std::string
IRGenMangler::mangleSymbolNameForUnderlyingWitnessTableAccessorString(
    OpaqueTypeDecl *opaque, const Requirement &req, ProtocolDecl *protocol) {
  beginManglingWithoutPrefix();
  Buffer << "get_underlying_witness ";

  appendContextOf(opaque);
  appendOpaqueDeclName(opaque);

  appendType(req.getFirstType()->getCanonicalType(), opaque->getGenericSignature());

  appendProtocolName(protocol);

  appendOperator("HC");

  return finalize();
}

std::string IRGenMangler::mangleSymbolNameForGenericEnvironment(
                                              CanGenericSignature genericSig) {
  beginManglingWithoutPrefix();
  Buffer << "generic environment ";
  appendGenericSignature(genericSig);
  return finalize();
}

std::string
IRGenMangler::mangleExtendedExistentialTypeShapeSymbol(
                                                CanGenericSignature genSig,
                                                CanType shapeType,
                                                bool isUnique) {
  beginMangling();
  appendExtendedExistentialTypeShapeSymbol(genSig, shapeType, isUnique);
  return finalize();
}

void
IRGenMangler::appendExtendedExistentialTypeShapeSymbol(
                                                CanGenericSignature genSig,
                                                CanType shapeType,
                                                bool isUnique) {
  appendExtendedExistentialTypeShape(genSig, shapeType);

  // If this is non-unique, add a suffix to avoid accidental misuse
  // (and to make it easier to analyze in an image).
  if (!isUnique)
    appendOperator("Mq");
}

void
IRGenMangler::appendExtendedExistentialTypeShape(CanGenericSignature genSig,
                                                 CanType shapeType) {
  // Append the generalization signature.
  if (genSig) appendGenericSignature(genSig);

  // Append the existential type.
  appendType(shapeType, genSig);

  // Append the shape operator.
  appendOperator(genSig ? "XG" : "Xg");
}

