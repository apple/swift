//===--- SerializedSILLoader.h - Handle SIL section in modules --*- C++ -*-===//
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

#ifndef SWIFT_SERIALIZATION_SILLOADER_H
#define SWIFT_SERIALIZATION_SILLOADER_H

#include "swift/AST/AutoDiff.h"
#include "swift/AST/Decl.h"
#include "swift/AST/Identifier.h"
#include "swift/SIL/Notifications.h"
#include "swift/SIL/SILDeclRef.h"
#include "swift/SIL/SILLinkage.h"
#include "swift/SIL/SILMoveOnlyDeinit.h"
#include <memory>
#include <vector>

namespace swift {
class ASTContext;
class FileUnit;
class ModuleDecl;
class SILDeserializer;
class SILFunction;
class SILGlobalVariable;
class SILProperty;
class SILModule;
class SILVTable;
class SILWitnessTable;
class SILDefaultWitnessTable;
class SILDifferentiabilityWitness;

/// Maintains a list of SILDeserializer, one for each serialized modules
/// in ASTContext. It provides lookupSILFunction that will perform lookup
/// on each SILDeserializer.
class SerializedSILLoader {
private:
  std::vector<std::unique_ptr<SILDeserializer>> LoadedSILSections;

  explicit SerializedSILLoader(
      ASTContext &ctx, SILModule *SILMod,
      DeserializationNotificationHandlerSet *callbacks);

public:
  /// Create a new loader.
  ///
  /// \param callbacks - not owned by the loader
  static std::unique_ptr<SerializedSILLoader>
  create(ASTContext &ctx, SILModule *SILMod,
         DeserializationNotificationHandlerSet *callbacks) {
    return std::unique_ptr<SerializedSILLoader>(
        new SerializedSILLoader(ctx, SILMod, callbacks));
  }
  ~SerializedSILLoader();

  SILFunction *lookupSILFunction(SILFunction *Callee, bool onlyUpdateLinkage);
  SILFunction *lookupSILFunction(StringRef Name,
                                 llvm::Optional<SILLinkage> linkage);
  bool hasSILFunction(StringRef Name,
                      llvm::Optional<SILLinkage> linkage = llvm::None);
  SILVTable *lookupVTable(const ClassDecl *C);
  SILMoveOnlyDeinit *lookupMoveOnlyDeinit(const NominalTypeDecl *nomDecl);
  SILWitnessTable *lookupWitnessTable(SILWitnessTable *C);
  SILDefaultWitnessTable *lookupDefaultWitnessTable(SILDefaultWitnessTable *C);
  SILDifferentiabilityWitness *
  lookupDifferentiabilityWitness(SILDifferentiabilityWitnessKey key);

  /// Invalidate the cached entries for deserialized state. Must be
  /// called when erasing deserialized state in the SILModule.
  void invalidateAllCaches();
  bool invalidateFunction(SILFunction *f);
  bool invalidateGlobalVariable(SILGlobalVariable *gv);
  bool invalidateVTable(SILVTable *vt);
  bool invalidateWitnessTable(SILWitnessTable *wt);
  bool invalidateDefaultWitnessTable(SILDefaultWitnessTable *wt);
  bool invalidateProperty(SILProperty *p);
  bool invalidateDifferentiabilityWitness(SILDifferentiabilityWitness *w);

  /// Deserialize all SILFunctions, VTables, and WitnessTables for
  /// a given Module.
  ///
  /// If PrimaryFile is nullptr, all definitions are brought in with
  /// definition linkage.
  ///
  /// Otherwise, definitions not in the primary file are brought in
  /// with external linkage.
  void getAllForModule(Identifier Mod, FileUnit *PrimaryFile);

  /// Deserialize all SILFunctions in all SILModules.
  void getAllSILFunctions();

  /// Deserialize all VTables in all SILModules.
  void getAllVTables();

  /// Deserialize all WitnessTables in all SILModules.
  void getAllWitnessTables();

  /// Deserialize all DefaultWitnessTables in all SILModules.
  void getAllDefaultWitnessTables();

  /// Deserialize all Properties in all SILModules.
  void getAllProperties();

  /// Deserialize all DifferentiabilityWitnesses in all SILModules.
  void getAllDifferentiabilityWitnesses();

  SerializedSILLoader(const SerializedSILLoader &) = delete;
  SerializedSILLoader(SerializedSILLoader &&) = delete;
  SerializedSILLoader &operator=(const SerializedSILLoader &) = delete;
  SerializedSILLoader &operator=(SerializedSILLoader &&) = delete;
};

} // end namespace swift

#endif
