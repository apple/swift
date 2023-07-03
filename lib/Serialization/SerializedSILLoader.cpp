//===--- SerializedSILLoader.cpp - A loader for SIL sections --------------===//
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

#define DEBUG_TYPE "serialized-sil-loader"

#include "DeserializeSIL.h"
#include "ModuleFile.h"

#include "swift/AST/ASTMangler.h"
#include "swift/SIL/SILModule.h"
#include "swift/SIL/SILMoveOnlyDeinit.h"
#include "swift/Serialization/SerializedModuleLoader.h"
#include "swift/Serialization/SerializedSILLoader.h"
#include "llvm/Support/Debug.h"

using namespace swift;

SerializedSILLoader::SerializedSILLoader(
    ASTContext &Ctx, SILModule *SILMod,
    DeserializationNotificationHandlerSet *callbacks) {

  // Get a list of SerializedModules from ASTContext.
  // FIXME: Iterating over LoadedModules is not a good way to do this.
  for (const auto &Entry : Ctx.getLoadedModules()) {
    for (auto File : Entry.second->getFiles()) {
      if (auto LoadedAST = dyn_cast<SerializedASTFile>(File)) {
        auto Des = new SILDeserializer(&LoadedAST->File, *SILMod, callbacks);
        LoadedSILSections.emplace_back(Des);
      }
    }
  }
}

SerializedSILLoader::~SerializedSILLoader() {}

SILFunction *SerializedSILLoader::lookupSILFunction(SILFunction *Callee,
                                                    bool onlyUpdateLinkage) {
  // It is possible that one module has a declaration of a SILFunction, while
  // another has the full definition.
  SILFunction *retVal = nullptr;
  for (auto &Des : LoadedSILSections) {
    if (auto Func = Des->lookupSILFunction(Callee,
                                      /*declarationOnly*/ onlyUpdateLinkage)) {
      LLVM_DEBUG(llvm::dbgs() << "Deserialized " << Func->getName() << " from "
                 << Des->getModuleIdentifier().str() << "\n");
      if (!Func->empty())
        return Func;
      retVal = Func;
    }
  }
  return retVal;
}

SILFunction *
SerializedSILLoader::lookupSILFunction(StringRef Name,
                                       llvm::Optional<SILLinkage> Linkage) {
  for (auto &Des : LoadedSILSections) {
    if (auto *Func = Des->lookupSILFunction(Name, /*declarationOnly*/ true)) {
      LLVM_DEBUG(llvm::dbgs() << "Deserialized " << Func->getName() << " from "
                 << Des->getModuleIdentifier().str() << "\n");
      if (Linkage) {
        // This is not the linkage we are looking for.
        if (Func->getLinkage() != *Linkage) {
          LLVM_DEBUG(llvm::dbgs()
                     << "Wrong linkage for Function: "
                     << Func->getName() << " : "
                     << (int)Func->getLinkage() << "\n");
          Des->invalidateFunction(Func);
          Func->getModule().eraseFunction(Func);
          continue;
        }
      }
      return Func;
    }
  }
  return nullptr;
}

bool SerializedSILLoader::hasSILFunction(StringRef Name,
                                         llvm::Optional<SILLinkage> Linkage) {
  // It is possible that one module has a declaration of a SILFunction, while
  // another has the full definition.
  SILFunction *retVal = nullptr;
  for (auto &Des : LoadedSILSections) {
    if (Des->hasSILFunction(Name, Linkage))
      return true;
  }
  return retVal;
}

SILVTable *SerializedSILLoader::lookupVTable(const ClassDecl *C) {
  Mangle::ASTMangler mangler;
  std::string mangledClassName = mangler.mangleNominalType(C);

  for (auto &Des : LoadedSILSections) {
    if (auto VT = Des->lookupVTable(mangledClassName))
      return VT;
  }
  return nullptr;
}

SILMoveOnlyDeinit *
SerializedSILLoader::lookupMoveOnlyDeinit(const NominalTypeDecl *nomDecl) {
  Mangle::ASTMangler mangler;
  std::string mangledClassName = mangler.mangleNominalType(nomDecl);

  for (auto &des : LoadedSILSections) {
    if (auto *tbl = des->lookupMoveOnlyDeinit(mangledClassName))
      return tbl;
  }
  return nullptr;
}

SILWitnessTable *SerializedSILLoader::lookupWitnessTable(SILWitnessTable *WT) {
  for (auto &Des : LoadedSILSections)
    if (auto wT = Des->lookupWitnessTable(WT))
      return wT;
  return nullptr;
}

SILDefaultWitnessTable *SerializedSILLoader::
lookupDefaultWitnessTable(SILDefaultWitnessTable *WT) {
  for (auto &Des : LoadedSILSections)
    if (auto wT = Des->lookupDefaultWitnessTable(WT))
      return wT;
  return nullptr;
}

SILDifferentiabilityWitness *
SerializedSILLoader::lookupDifferentiabilityWitness(
    SILDifferentiabilityWitnessKey key) {
  Mangle::ASTMangler mangler;
  auto mangledKey = mangler.mangleSILDifferentiabilityWitness(
     key.originalFunctionName, key.kind, key.config);
  // It is possible that one module has a declaration of a
  // SILDifferentiabilityWitness, while another has the full definition.
  SILDifferentiabilityWitness *dw = nullptr;
  for (auto &Des : LoadedSILSections) {
    dw = Des->lookupDifferentiabilityWitness(mangledKey);
    if (dw && dw->isDefinition())
      return dw;
  }
  return dw;
}

void SerializedSILLoader::invalidateAllCaches() {
  for (auto &des : LoadedSILSections)
    des->invalidateAllCaches();
}

bool SerializedSILLoader::invalidateFunction(SILFunction *fn) {
  for (auto &des : LoadedSILSections)
    if (des->invalidateFunction(fn))
      return true;
  return false;
}

bool SerializedSILLoader::invalidateGlobalVariable(SILGlobalVariable *gv) {
  for (auto &des : LoadedSILSections)
    if (des->invalidateGlobalVariable(gv))
      return true;
  return false;
}

bool SerializedSILLoader::invalidateVTable(SILVTable *vt) {
  for (auto &des : LoadedSILSections)
    if (des->invalidateVTable(vt))
      return true;
  return false;
}

bool SerializedSILLoader::invalidateWitnessTable(SILWitnessTable *wt) {
  for (auto &des : LoadedSILSections)
    if (des->invalidateWitnessTable(wt))
      return true;
  return false;
}

bool SerializedSILLoader::invalidateDefaultWitnessTable(
    SILDefaultWitnessTable *wt) {
  for (auto &des : LoadedSILSections)
    if (des->invalidateDefaultWitnessTable(wt))
      return true;
  return false;
}

bool SerializedSILLoader::invalidateProperty(SILProperty *p) {
  for (auto &des : LoadedSILSections)
    if (des->invalidateProperty(p))
      return true;
  return false;
}

bool SerializedSILLoader::invalidateDifferentiabilityWitness(
    SILDifferentiabilityWitness *w) {
  for (auto &des : LoadedSILSections)
    if (des->invalidateDifferentiabilityWitness(w))
      return true;
  return false;
}

// FIXME: Not the best interface. We know exactly which FileUnits may have SIL
// those in the main module.
void SerializedSILLoader::getAllForModule(Identifier Mod,
                                          FileUnit *PrimaryFile) {
  for (auto &Des : LoadedSILSections) {
    if (Des->getModuleIdentifier() == Mod) {
      Des->getAll(PrimaryFile ?
                  Des->getFile() != PrimaryFile : false);
    }
  }
}

void SerializedSILLoader::getAllSILFunctions() {
  for (auto &Des : LoadedSILSections)
    Des->getAllSILFunctions();
}

/// Deserialize all VTables in all SILModules.
void SerializedSILLoader::getAllVTables() {
  for (auto &Des : LoadedSILSections)
    Des->getAllVTables();
}

/// Deserialize all WitnessTables in all SILModules.
void SerializedSILLoader::getAllWitnessTables() {
  for (auto &Des : LoadedSILSections)
    Des->getAllWitnessTables();
}

/// Deserialize all DefaultWitnessTables in all SILModules.
void SerializedSILLoader::getAllDefaultWitnessTables() {
  for (auto &Des : LoadedSILSections)
    Des->getAllDefaultWitnessTables();
}

/// Deserialize all Properties in all SILModules.
void SerializedSILLoader::getAllProperties() {
  for (auto &Des : LoadedSILSections)
    Des->getAllProperties();
}

/// Deserialize all DifferentiabilityWitnesses in all SILModules.
void SerializedSILLoader::getAllDifferentiabilityWitnesses() {
  for (auto &Des : LoadedSILSections)
    Des->getAllDifferentiabilityWitnesses();
}
