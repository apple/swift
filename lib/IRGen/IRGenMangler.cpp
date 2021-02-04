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
#include "GenClass.h"
#include "swift/AST/ExistentialLayout.h"
#include "swift/AST/IRGenOptions.h"
#include "swift/AST/ProtocolAssociations.h"
#include "swift/AST/ProtocolConformance.h"
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
  appendType(type);

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
  OptimizeProtocolNames = false;
  UseObjCRuntimeNames = true;

  llvm::SaveAndRestore<bool>
    AllowSymbolicReferencesLocally(AllowSymbolicReferences);
  llvm::SaveAndRestore<std::function<bool (SymbolicReferent)>>
    CanSymbolicReferenceLocally(CanSymbolicReference);

  AllowSymbolicReferences = true;
  bool SupportsProtocolConformanceSymbolicReferences
    = IGM.getAvailabilityContext()
        .isContainedIn(IGM.Context.getSwiftFutureAvailability());
  CanSymbolicReference = [&](SymbolicReferent s) -> bool {
    if (auto type = s.dyn_cast<const NominalTypeDecl *>()) {
      // The short-substitution types in the standard library have compact
      // manglings already, and the runtime ought to have a lookup table for
      // them. Symbolic referencing would be wasteful.
      if (type->getModuleContext()->isStdlibModule()
          && Mangle::getStandardTypeSubst(type->getName().str())) {
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
      if (auto clas = dyn_cast<ClassDecl>(type)) {
        // Swift-defined classes can be symbolically referenced.
        if (hasKnownSwiftMetadata(IGM, const_cast<ClassDecl*>(clas)))
          return true;

        // Foreign class types can be symbolically referenced.
        if (clas->getForeignClassKind() == ClassDecl::ForeignKind::CFType)
          return true;

        // Otherwise no.
        return false;
      }

      return true;
    } else if (s.is<const OpaqueTypeDecl *>()) {
      // Always symbolically reference opaque types.
      return true;
    } else if (s.is<const ProtocolConformance *>()) {
      // Always symbolically reference protocol conformances, if we
      // have a runtime that supports it.
      return SupportsProtocolConformanceSymbolicReferences;
    } else {
      llvm_unreachable("symbolic referent not handled");
    }
  };

  SymbolicReferences.clear();
  
  body();
  
  return {finalize(), std::move(SymbolicReferences)};
}

SymbolicMangling
IRGenMangler::mangleTypeForReflection(IRGenModule &IGM,
                                      Type Ty) {
  return withSymbolicReferences(IGM, [&]{
    appendType(Ty);
  });
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

SymbolicMangling
IRGenMangler::mangleProtocolConformanceForReflection(IRGenModule &IGM,
                                  Type ty, ProtocolConformanceRef conformance) {
  return withSymbolicReferences(IGM, [&]{
    if (conformance.isConcrete()) {
      appendProtocolConformance(conformance.getConcrete());
    } else {
      // Use a special mangling for abstract conformances.
      appendType(ty);
      appendProtocolName(conformance.getAbstract());
    }
  });
}

std::string IRGenMangler::mangleTypeForLLVMTypeName(CanType Ty) {
  // To make LLVM IR more readable we always add a 'T' prefix so that type names
  // don't start with a digit and don't need to be quoted.
  Buffer << 'T';
  if (auto P = dyn_cast<ProtocolType>(Ty)) {
    appendProtocolName(P->getDecl(), /*allowStandardSubstitution=*/false);
    appendOperator("P");
  } else {
    appendType(Ty);
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
      appendProtocolName(protocols[i]->getDecl());
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

      appendType(CanType(superclass));
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

  case MangledTypeRefRole::Metadata:
  case MangledTypeRefRole::Reflection:
    prefix = "symbolic ";
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
    if (auto ty = referent.dyn_cast<const NominalTypeDecl*>())
      appendContext(ty, ty->getAlternateModuleName());
    else if (auto opaque = referent.dyn_cast<const OpaqueTypeDecl*>())
      appendOpaqueDeclName(opaque);
    else if (auto conformance = referent.dyn_cast<const ProtocolConformance *>())
      appendProtocolConformance(conformance);
    else
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
    appendType(type);
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

std::string IRGenMangler::mangleSymbolNameForGenericEnvironment(
                                              CanGenericSignature genericSig) {
  beginManglingWithoutPrefix();
  Buffer << "generic environment ";
  appendGenericSignature(genericSig);
  return finalize();
}
