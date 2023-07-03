//===--- DeserializeSIL.h - Read SIL ----------------------------*- C++ -*-===//
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

#include "ModuleFile.h"
#include "SILFormat.h"
#include "swift/AST/Types.h"
#include "swift/SIL/SILModule.h"
#include "swift/SIL/SILMoveOnlyDeinit.h"
#include "swift/Serialization/SerializedSILLoader.h"

#include "llvm/ADT/DenseMap.h"
#include "llvm/Support/SaveAndRestore.h"

namespace llvm {
  template <typename Info> class OnDiskIterableChainedHashTable;
}

namespace swift {
  class SILDeserializer {
    using TypeID = serialization::TypeID;
    
    ModuleFile *MF;
    SILModule &SILMod;
    DeserializationNotificationHandlerSet *Callback;

    /// The cursor used to lazily load SILFunctions.
    llvm::BitstreamCursor SILCursor;
    llvm::BitstreamCursor SILIndexCursor;

    class FuncTableInfo;
    using SerializedFuncTable =
      llvm::OnDiskIterableChainedHashTable<FuncTableInfo>;

    //-----
    // Deserialization Caches
    //
    // NOTE: When adding more serialized tables to the deserializer,
    // always add invalidate methods and make sure SILModule always
    // invalidates the deserializer appropriately when it erases a
    // value that we deserialized here. Otherwise, memory corruption
    // may result.

    std::unique_ptr<SerializedFuncTable> FuncTable;
    MutableArrayRef<ModuleFile::PartiallySerialized<SILFunction*>> Funcs;

    std::unique_ptr<SerializedFuncTable> GlobalVarList;
    MutableArrayRef<ModuleFile::PartiallySerialized<SILGlobalVariable *>>
        GlobalVars;

    std::unique_ptr<SerializedFuncTable> VTableList;
    MutableArrayRef<ModuleFile::PartiallySerialized<SILVTable *>> VTables;

    std::unique_ptr<SerializedFuncTable> MoveOnlyDeinitList;
    MutableArrayRef<ModuleFile::PartiallySerialized<SILMoveOnlyDeinit *>>
        MoveOnlyDeinits;

    std::unique_ptr<SerializedFuncTable> WitnessTableList;
    MutableArrayRef<ModuleFile::PartiallySerialized<SILWitnessTable *>>
    WitnessTables;

    std::unique_ptr<SerializedFuncTable> DefaultWitnessTableList;
    MutableArrayRef<ModuleFile::PartiallySerialized<SILDefaultWitnessTable *>>
    DefaultWitnessTables;

    MutableArrayRef<ModuleFile::PartiallySerialized<SILProperty *>>
    Properties;

    std::unique_ptr<SerializedFuncTable> DifferentiabilityWitnessList;
    MutableArrayRef<
        ModuleFile::PartiallySerialized<SILDifferentiabilityWitness *>>
        DifferentiabilityWitnesses;

    //-----
    // End Deserialization Caches
    //
    // Before adding a new cache here, please read the comment at the
    // beginning of the deserialization cache section.

    /// A declaration will only
    llvm::DenseMap<NormalProtocolConformance *, SILWitnessTable *>
    ConformanceToWitnessTableMap;

    /// Data structures used to perform name lookup for local values.
    llvm::DenseMap<uint32_t, ValueBase*> LocalValues;

    /// The first two local values are reserved for SILUndef.
    serialization::ValueID LastValueID = 1;

    /// Data structures used to perform lookup of basic blocks.
    llvm::DenseMap<unsigned, SILBasicBlock*> BlocksByID;
    llvm::DenseMap<SILBasicBlock*, unsigned> UndefinedBlocks;
    unsigned BasicBlockID = 0;

    /// Return the SILBasicBlock of a given ID.
    SILBasicBlock *getBBForReference(SILFunction *Fn, unsigned ID);
    SILBasicBlock *getBBForDefinition(SILFunction *Fn, SILBasicBlock *Prev,
                                      unsigned ID);

    /// Read a SIL function.
    SILFunction *readSILFunction(serialization::DeclID, SILFunction *InFunc,
                                 StringRef Name, bool declarationOnly,
                                 bool errorIfEmptyBody = true);
    /// Read a SIL function.
    llvm::Expected<SILFunction *>
    readSILFunctionChecked(serialization::DeclID, SILFunction *InFunc,
                           StringRef Name, bool declarationOnly,
                           bool errorIfEmptyBody = true);

    /// Read a SIL basic block within a given SIL function.
    SILBasicBlock *readSILBasicBlock(SILFunction *Fn,
                                     SILBasicBlock *Prev,
                                     SmallVectorImpl<uint64_t> &scratch);
    /// Read a SIL instruction.
    bool readSILInstruction(SILFunction *Fn,
                            SILBuilder &Builder,
                            unsigned RecordKind,
                            SmallVectorImpl<uint64_t> &scratch);

    /// Read the SIL function table.
    std::unique_ptr<SerializedFuncTable>
    readFuncTable(ArrayRef<uint64_t> fields, StringRef blobData);

    /// When an instruction or block argument is defined, this method is used to
    /// register it and update our symbol table.
    void setLocalValue(ValueBase *Value, serialization::ValueID Id);
    /// Get a reference to a local value with the specified ID and type.
    SILValue getLocalValue(serialization::ValueID Id,
                           SILType Type);

    SILType getSILType(Type ty, SILValueCategory category,
                       SILFunction *inContext);

    SILDifferentiabilityWitness *
    getSILDifferentiabilityWitnessForReference(StringRef mangledKey);

    SILFunction *getFuncForReference(StringRef Name, SILType Ty);
    SILFunction *getFuncForReference(StringRef Name);
    SILVTable *readVTable(serialization::DeclID);
    SILMoveOnlyDeinit *readMoveOnlyDeinit(serialization::DeclID);
    SILGlobalVariable *getGlobalForReference(StringRef Name);
    SILGlobalVariable *readGlobalVar(StringRef Name);

    /// Read and return the witness table identified with \p WId.
    SILWitnessTable *readWitnessTable(serialization::DeclID WId,
                                      SILWitnessTable *existingWt);

    /// Read the witness table identified with \p WId, return the table or
    /// the first error if any.
    llvm::Expected<SILWitnessTable *>
      readWitnessTableChecked(serialization::DeclID WId,
                              SILWitnessTable *existingWt);

    void readWitnessTableEntries(
           llvm::BitstreamEntry &entry,
           std::vector<SILWitnessTable::Entry> &witnessEntries,
           std::vector<SILWitnessTable::ConditionalConformance>
             &conditionalConformances);
    SILProperty *readProperty(serialization::DeclID);
    SILDefaultWitnessTable *
    readDefaultWitnessTable(serialization::DeclID,
                            SILDefaultWitnessTable *existingWt);
    SILDifferentiabilityWitness *
        readDifferentiabilityWitness(serialization::DeclID);

    llvm::Optional<KeyPathPatternComponent>
    readKeyPathComponent(ArrayRef<uint64_t> ListOfValues, unsigned &nextValue);

  public:
    Identifier getModuleIdentifier() const {
      return MF->getAssociatedModule()->getName();
    }
    FileUnit *getFile() const {
      return MF->getFile();
    }
    SILFunction *lookupSILFunction(SILFunction *InFunc, bool onlyUpdateLinkage);
    SILFunction *lookupSILFunction(StringRef Name,
                                   bool declarationOnly = false);
    bool hasSILFunction(StringRef Name,
                        llvm::Optional<SILLinkage> Linkage = llvm::None);
    SILVTable *lookupVTable(StringRef MangledClassName);
    SILMoveOnlyDeinit *lookupMoveOnlyDeinit(StringRef mangledNominalTypeName);
    SILWitnessTable *lookupWitnessTable(SILWitnessTable *wt);
    SILDefaultWitnessTable *
    lookupDefaultWitnessTable(SILDefaultWitnessTable *wt);
    SILDifferentiabilityWitness *
    lookupDifferentiabilityWitness(StringRef mangledDiffWitnessKey);

    /// Invalidate all cached SILFunctions.
    void invalidateFunctionCache();

    /// Invalidate a specific cached SILFunction.
    bool invalidateFunction(SILFunction *F);

    /// Invalidate all cached SILGlobalVariable.
    void invalidateGlobalVariableCache();

    /// Invalidate a specific cached GlobalVariable.
    bool invalidateGlobalVariable(SILGlobalVariable *gv);

    /// Invalidate all cached SILVTable.
    void invalidateVTableCache();

    /// Invalidate a specific cached SILVTable.
    bool invalidateVTable(SILVTable *v);

    /// Invalidate all cached SILWitnessTable.
    void invalidateWitnessTableCache();

    /// Invalidate a specific cached SILWitnessTable.
    bool invalidateWitnessTable(SILWitnessTable *v);

    /// Invalidate all cached SILDefaultWitnessTable.
    void invalidateDefaultWitnessTableCache();

    /// Invalidate a specific cached SILDefaultWitnessTable.
    bool invalidateDefaultWitnessTable(SILDefaultWitnessTable *v);

    /// Invalidate all cached SILProperty.
    void invalidatePropertyCache();

    /// Invalidate a specific cached SILProperty.
    bool invalidateProperty(SILProperty *v);

    /// Invalidate all cached SILDifferentiabilityWitness.
    void invalidateDifferentiabilityWitnessCache();

    /// Invalidate a specific cached SILDifferentiabilityWitness.
    bool invalidateDifferentiabilityWitness(SILDifferentiabilityWitness *v);

    /// Invalidate all caches in this deserializer.
    void invalidateAllCaches() {
      invalidateFunctionCache();
      invalidateGlobalVariableCache();
      invalidateVTableCache();
      invalidateWitnessTableCache();
      invalidateDefaultWitnessTableCache();
      invalidatePropertyCache();
      invalidateDifferentiabilityWitnessCache();
    }

    /// Deserialize all SILFunctions, VTables, WitnessTables, and
    /// DefaultWitnessTables inside the module, and add them to SILMod.
    ///
    /// TODO: Globals.
    void getAll(bool UseCallback = true) {
      llvm::SaveAndRestore<DeserializationNotificationHandlerSet *> SaveCB(
          Callback);

      if (!UseCallback)
        Callback = nullptr;

      getAllSILFunctions();
      getAllSILGlobalVariables();
      getAllVTables();
      getAllWitnessTables();
      getAllDefaultWitnessTables();
      getAllProperties();
      getAllDifferentiabilityWitnesses();
      getAllMoveOnlyDeinits();
    }

    /// Deserialize all SILFunctions inside the module and add them to SILMod.
    void getAllSILFunctions();

    /// Deserialize all SILGlobalVariables inside the module and add them to
    /// SILMod.
    void getAllSILGlobalVariables();

    /// Deserialize all VTables inside the module and add them to SILMod.
    void getAllVTables();

    /// Deserialize all move only deinit tables inside the module and add them
    /// to SILMod.
    void getAllMoveOnlyDeinits();

    /// Deserialize all WitnessTables inside the module and add them to SILMod.
    void getAllWitnessTables();

    /// Deserialize all DefaultWitnessTables inside the module and add them
    /// to SILMod.
    void getAllDefaultWitnessTables();

    /// Deserialize all Property descriptors inside the module and add them
    /// to SILMod.
    void getAllProperties();

    /// Deserialize all DifferentiabilityWitnesses inside the module and add
    /// them to SILMod.
    void getAllDifferentiabilityWitnesses();

    SILDeserializer(ModuleFile *MF, SILModule &M,
                    DeserializationNotificationHandlerSet *callback);

    // Out of line to avoid instantiation OnDiskChainedHashTable here.
    ~SILDeserializer();
  };
} // end namespace swift
