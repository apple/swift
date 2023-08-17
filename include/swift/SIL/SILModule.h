//===--- SILModule.h - Defines the SILModule class --------------*- C++ -*-===//
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
// This file defines the SILModule class.
//
//===----------------------------------------------------------------------===//

#ifndef SWIFT_SIL_SILMODULE_H
#define SWIFT_SIL_SILMODULE_H

#include "swift/AST/ASTContext.h"
#include "swift/AST/Builtins.h"
#include "swift/AST/Decl.h"
#include "swift/AST/SILLayout.h"
#include "swift/AST/SILOptions.h"
#include "swift/Basic/IndexTrie.h"
#include "swift/Basic/LangOptions.h"
#include "swift/Basic/ProfileCounter.h"
#include "swift/Basic/Range.h"
#include "swift/SIL/Notifications.h"
#include "swift/SIL/SILCoverageMap.h"
#include "swift/SIL/SILDeclRef.h"
#include "swift/SIL/SILDefaultWitnessTable.h"
#include "swift/SIL/SILDifferentiabilityWitness.h"
#include "swift/SIL/SILFunction.h"
#include "swift/SIL/SILGlobalVariable.h"
#include "swift/SIL/SILMoveOnlyDeinit.h"
#include "swift/SIL/SILPrintContext.h"
#include "swift/SIL/SILProperty.h"
#include "swift/SIL/SILType.h"
#include "swift/SIL/SILVTable.h"
#include "swift/SIL/SILWitnessTable.h"
#include "swift/SIL/TypeLowering.h"
#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/FoldingSet.h"
#include "llvm/ADT/MapVector.h"
#include "llvm/ADT/PointerIntPair.h"
#include "llvm/ADT/SetVector.h"
#include "llvm/ADT/ilist.h"
#include "llvm/ProfileData/InstrProfReader.h"
#include "llvm/Support/Allocator.h"
#include "llvm/Support/raw_ostream.h"
#include <functional>

namespace llvm {
namespace yaml {
class Output;
} // end namespace yaml
} // end namespace llvm

namespace swift {

/// The payload for the FixedSizeSlab.
/// This is a super-class rather than a member of FixedSizeSlab to make swift
/// bridging easier.
class FixedSizeSlabPayload {
public:
  /// The capacity of the payload.
  static constexpr size_t capacity = 64 * sizeof(uintptr_t);

private:
  friend class SILModule;

  /// The magic number which is stored in overflowGuard.
  static constexpr uintptr_t magicNumber = (uintptr_t)0xdeadbeafdeadbeafull;

  /// The payload.
  char data[capacity];
  
  /// Used for a cheap buffer overflow check - in the spirit of libgmalloc.
  uintptr_t overflowGuard = magicNumber;

public:
  void operator=(const FixedSizeSlabPayload &) = delete;
  void operator delete(void *Ptr, size_t) = delete;

  /// Returns the payload pointing to \p T.
  template<typename T> T *dataFor() { return (T *)(&data[0]); }

  /// Returns the payload pointing to const \p T
  template<typename T> const T *dataFor() const { return (const T *)(&data[0]); }
};

/// A fixed size slab of memory, which can be allocated and freed by the
/// SILModule at (basically) zero cost.
/// See SILModule::allocSlab().
class FixedSizeSlab : public llvm::ilist_node<FixedSizeSlab>,
                      public SILAllocated<FixedSizeSlab>,
                      public FixedSizeSlabPayload {
public:
  void operator=(const FixedSizeSlab &) = delete;
  void operator delete(void *Ptr, size_t) = delete;
};

class AnyFunctionType;
class ASTContext;
class FileUnit;
class FuncDecl;
class IRGenOptions;
class KeyPathPattern;
class ModuleDecl;
class SILUndef;
class SourceFile;
class SerializedSILLoader;
class SILFunctionBuilder;
class SILOptFunctionBuilder;
class SILRemarkStreamer;

namespace Lowering {
class SILGenModule;
} // namespace Lowering

/// A stage of SIL processing.
enum class SILStage {
  /// "Raw" SIL, emitted by SILGen, but not yet run through guaranteed
  /// optimization and diagnostic passes.
  ///
  /// Raw SIL does not have fully-constructed SSA and may contain undiagnosed
  /// dataflow errors.
  Raw,

  /// Canonical SIL, which has been run through at least the guaranteed
  /// optimization and diagnostic passes.
  ///
  /// Canonical SIL has stricter invariants than raw SIL. It must not contain
  /// dataflow errors, and some instructions must be canonicalized to simpler
  /// forms.
  Canonical,

  /// Lowered SIL, which has been prepared for IRGen and will no longer
  /// be passed to canonical SIL transform passes.
  ///
  /// In lowered SIL, the SILType of all SILValues is its SIL storage
  /// type. Explicit storage is required for all address-only and resilient
  /// types.
  ///
  /// Generating the initial Raw SIL is typically referred to as lowering (from
  /// the AST). To disambiguate, refer to the process of generating the lowered
  /// stage of SIL as "address lowering".
  Lowered,
};

/// A SIL module. The SIL module owns all of the SILFunctions generated
/// when a Swift compilation context is lowered to SIL.
class SILModule {
  friend class SILFunctionBuilder;

public:
  using FunctionListType = llvm::ilist<SILFunction>;
  using GlobalListType = llvm::ilist<SILGlobalVariable>;
  using VTableListType = llvm::ArrayRef<SILVTable*>;
  using PropertyListType = llvm::ilist<SILProperty>;
  using WitnessTableListType = llvm::ilist<SILWitnessTable>;
  using DefaultWitnessTableListType = llvm::ilist<SILDefaultWitnessTable>;
  using DifferentiabilityWitnessListType =
      llvm::ilist<SILDifferentiabilityWitness>;
  using SILMoveOnlyDeinitListType = llvm::ArrayRef<SILMoveOnlyDeinit *>;
  using CoverageMapCollectionType =
      llvm::MapVector<StringRef, SILCoverageMap *>;
  using BasicBlockNameMapType =
      llvm::DenseMap<const SILBasicBlock *, std::string>;

  enum class LinkingMode : uint8_t {
    /// Link functions with shared linkage. Used by the mandatory pipeline.
    LinkNormal,

    /// Link all functions. Used by the performance pipeline.
    LinkAll
  };

  using ActionCallback = std::function<void()>;
  using SlabList = llvm::simple_ilist<FixedSizeSlab>;

private:
  friend KeyPathPattern;
  friend SILBasicBlock;
  friend SILCoverageMap;
  friend SILDefaultWitnessTable;
  friend SILDifferentiabilityWitness;
  friend SILFunction;
  friend SILGlobalVariable;
  friend SILLayout;
  friend SILType;
  friend SILVTable;
  friend SILProperty;
  friend SILUndef;
  friend SILWitnessTable;
  friend SILMoveOnlyDeinit;
  friend Lowering::SILGenModule;
  friend Lowering::TypeConverter;
  class SerializationCallback;

  /// Allocator that manages the memory of all the pieces of the SILModule.
  mutable llvm::BumpPtrAllocator BPA;

  /// The list of freed slabs, which can be reused.
  SlabList freeSlabs;
  
  /// For consistency checking.
  size_t numAllocatedSlabs = 0;

  /// When an instruction is "deleted" from the SIL, it is put into this list.
  /// The instructions in this list are eventually deleted for real in
  /// flushDeletedInsts(), which is called by the pass manager after each pass
  /// run.
  /// In other words: instruction deletion is deferred to the end of a pass.
  ///
  /// This avoids dangling instruction pointers within the run of a pass and in
  /// analysis caches. Note that the analysis invalidation mechanism ensures
  /// that analysis caches are invalidated before flushDeletedInsts().
  std::vector<SILInstruction*> scheduledForDeletion;

  /// The swift Module associated with this SILModule.
  ModuleDecl *TheSwiftModule;

  /// A specific context for AST-level declarations associated with this SIL
  /// module.
  ///
  /// \sa getAssociatedContext
  const DeclContext *AssociatedDeclContext;

  /// Lookup table for SIL functions. This needs to be declared before \p
  /// functions so that the destructor of \p functions is called first.
  llvm::StringMap<SILFunction *> FunctionTable;
  llvm::StringMap<SILFunction *> ZombieFunctionTable;

  /// The list of SILFunctions in the module.
  FunctionListType functions;

  /// Functions, which are dead (and not in the functions list anymore),
  /// but kept alive for debug info generation.
  FunctionListType zombieFunctions;

  /// Lookup table for SIL vtables from class decls.
  llvm::DenseMap<const ClassDecl *, SILVTable *> VTableMap;

  /// The list of SILVTables in the module.
  std::vector<SILVTable*> vtables;

  /// This is a cache of vtable entries for quick look-up
  llvm::DenseMap<std::pair<const SILVTable *, SILDeclRef>, SILVTable::Entry>
      VTableEntryCache;

  /// Lookup table for SIL witness tables from conformances.
  llvm::DenseMap<const RootProtocolConformance *, SILWitnessTable *>
  WitnessTableMap;

  /// The list of SILWitnessTables in the module.
  WitnessTableListType witnessTables;

  /// Lookup table for SIL default witness tables from protocols.
  llvm::DenseMap<const ProtocolDecl *, SILDefaultWitnessTable *>
  DefaultWitnessTableMap;

  /// The list of SILDefaultWitnessTables in the module.
  DefaultWitnessTableListType defaultWitnessTables;

  /// Lookup table for SIL differentiability witnesses, keyed by mangled name.
  llvm::StringMap<SILDifferentiabilityWitness *> DifferentiabilityWitnessMap;

  /// Lookup table for SILDifferentiabilityWitnesses, keyed by original
  /// function name.
  llvm::StringMap<llvm::SmallVector<SILDifferentiabilityWitness *, 1>>
      DifferentiabilityWitnessesByFunction;

  /// The list of SILDifferentiabilityWitnesses in the module.
  DifferentiabilityWitnessListType differentiabilityWitnesses;

  /// Lookup table for SIL vtables from class decls.
  llvm::DenseMap<const NominalTypeDecl *, SILMoveOnlyDeinit *>
      MoveOnlyDeinitMap;

  /// The list of move only deinits in the module.
  std::vector<SILMoveOnlyDeinit *> moveOnlyDeinits;

  /// Declarations which are externally visible.
  ///
  /// These are method declarations which are referenced from inlinable
  /// functions due to cross-module-optimization. Those declarations don't have
  /// any attributes or linkage which mark them as externally visible by
  /// default.
  /// Currently this table is not serialized.
  llvm::SetVector<ValueDecl *> externallyVisible;

  /// Lookup table for SIL Global Variables.
  llvm::StringMap<SILGlobalVariable *> GlobalVariableMap;

  /// The list of SILGlobalVariables in the module.
  GlobalListType silGlobals;

  // The map of SILCoverageMaps in the module.
  CoverageMapCollectionType coverageMaps;

  // The list of SILProperties in the module.
  PropertyListType properties;

  /// The remark streamer used to serialize SIL remarks to a file.
  std::unique_ptr<swift::SILRemarkStreamer> silRemarkStreamer;

  /// This is a cache of intrinsic Function declarations to numeric ID mappings.
  llvm::DenseMap<Identifier, IntrinsicInfo> IntrinsicIDCache;

  /// This is a cache of builtin Function declarations to numeric ID mappings.
  llvm::DenseMap<Identifier, BuiltinInfo> BuiltinIDCache;

  /// This is the set of undef values we've created, for uniquing purposes.
  llvm::DenseMap<SILType, SILUndef *> UndefValues;

  llvm::DenseMap<std::pair<Decl *, VarDecl *>, unsigned> fieldIndices;
  llvm::DenseMap<EnumElementDecl *, unsigned> enumCaseIndices;

  /// The stage of processing this module is at.
  SILStage Stage;

  /// True if SIL conventions force address-only to be passed by address.
  ///
  /// Used for bootstrapping the AddressLowering pass. This should eventually
  /// be inferred from the SIL stage to be true only when Stage == Lowered.
  bool loweredAddresses;

  /// The set of deserialization notification handlers.
  DeserializationNotificationHandlerSet deserializationNotificationHandlers;

  /// The SILLoader used when linking functions into this module.
  ///
  /// This is lazily initialized the first time we attempt to
  /// deserialize. Previously this was created when the SILModule was
  /// constructed. In certain cases this was before all Modules had been loaded
  /// causing us to not
  std::unique_ptr<SerializedSILLoader> SILLoader;

  /// The indexed profile data to be used for PGO, or nullptr.
  std::unique_ptr<llvm::IndexedInstrProfReader> PGOReader;

  /// A trie of integer indices that gives pointer identity to a path of
  /// projections, shared between all functions in the module.
  std::unique_ptr<IndexTrieNode> indexTrieRoot;

  /// A mapping from root local archetypes to the instructions which define
  /// them.
  ///
  /// The value is either a SingleValueInstruction or a PlaceholderValue,
  /// in case a local archetype definition is looked up during parsing or
  /// deserializing SIL, where local archetypes can be forward referenced.
  ///
  /// In theory we wouldn't need to have the SILFunction in the key, because
  /// local archetypes \em should be unique across the module. But currently
  /// in some rare cases SILGen re-uses the same local archetype for multiple
  /// functions.
  using LocalArchetypeKey = std::pair<LocalArchetypeType *, SILFunction *>;
  llvm::DenseMap<LocalArchetypeKey, SILValue> RootLocalArchetypeDefs;

  /// The number of PlaceholderValues in RootLocalArchetypeDefs.
  int numUnresolvedLocalArchetypes = 0;

  /// The options passed into this SILModule.
  const SILOptions &Options;

  /// IRGen options to be used by target specific SIL optimization passes.
  ///
  /// Not null, if the module is created by the compiler itself (and not
  /// e.g. by lldb).
  const IRGenOptions *irgenOptions;

  /// The number of functions created in this module, which will be the index of
  /// the next function.
  unsigned nextFunctionIndex = 0;

  /// Set if the SILModule was serialized already. It is used
  /// to ensure that the module is serialized only once.
  bool serialized;

  bool parsedAsSerializedSIL;

  /// Set if we have registered a deserialization notification handler for
  /// lowering ownership in non transparent functions.
  /// This gets set in NonTransparent OwnershipModelEliminator pass.
  bool regDeserializationNotificationHandlerForNonTransparentFuncOME;
  /// Set if we have registered a deserialization notification handler for
  /// lowering ownership in transparent functions.
  /// This gets set in OwnershipModelEliminator pass.
  bool regDeserializationNotificationHandlerForAllFuncOME;

  bool prespecializedFunctionDeclsImported;

  /// Action to be executed for serializing the SILModule.
  ActionCallback SerializeSILAction;

  BasicBlockNameMapType basicBlockNames;

  SILModule(llvm::PointerUnion<FileUnit *, ModuleDecl *> context,
            Lowering::TypeConverter &TC, const SILOptions &Options,
            const IRGenOptions *irgenOptions = nullptr);

  SILModule(const SILModule&) = delete;
  void operator=(const SILModule&) = delete;

  /// Folding set for key path patterns.
  llvm::FoldingSet<KeyPathPattern> KeyPathPatterns;

public:
  ~SILModule();

  /// Method which returns the SerializedSILLoader, creating the loader if it
  /// has not been created yet.
  SerializedSILLoader *getSILLoader();

  /// Add a callback for each newly deserialized SIL function body.
  void registerDeserializationNotificationHandler(
      std::unique_ptr<DeserializationNotificationHandler> &&handler);

  /// Return the set of registered deserialization callbacks.
  DeserializationNotificationHandlerSet::range
  getDeserializationHandlers() const {
    return deserializationNotificationHandlers.getRange();
  }

  void removeDeserializationNotificationHandler(
      DeserializationNotificationHandler *handler) {
    deserializationNotificationHandlers.erase(handler);
  }

  bool hasRegisteredDeserializationNotificationHandlerForNonTransparentFuncOME() {
    return regDeserializationNotificationHandlerForNonTransparentFuncOME;
  }
  bool hasRegisteredDeserializationNotificationHandlerForAllFuncOME() {
    return regDeserializationNotificationHandlerForAllFuncOME;
  }
  void setRegisteredDeserializationNotificationHandlerForNonTransparentFuncOME() {
    regDeserializationNotificationHandlerForNonTransparentFuncOME = true;
  }
  void setRegisteredDeserializationNotificationHandlerForAllFuncOME() {
    regDeserializationNotificationHandlerForAllFuncOME = true;
  }

  /// Returns the instruction which defines the given root local archetype,
  /// e.g. an open_existential_addr.
  ///
  /// In case the local archetype is not defined yet (e.g. during parsing or
  /// deserialization), a PlaceholderValue is returned. This should not be the
  /// case outside of parsing or deserialization.
  SILValue getRootLocalArchetypeDef(CanLocalArchetypeType archetype,
                                    SILFunction *inFunction);

  /// Returns the instruction which defines the given root local archetype,
  /// e.g. an open_existential_addr.
  ///
  /// In contrast to getLocalArchetypeDef, it is required that all local
  /// archetypes are resolved.
  SingleValueInstruction *
  getRootLocalArchetypeDefInst(CanLocalArchetypeType archetype,
                               SILFunction *inFunction) {
    return cast<SingleValueInstruction>(
        getRootLocalArchetypeDef(archetype, inFunction));
  }

  /// Returns true if there are unresolved local archetypes in the module.
  ///
  /// This should only be the case during parsing or deserialization.
  bool hasUnresolvedLocalArchetypeDefinitions();

  /// Get a unique index for a struct or class field in layout order.
  ///
  /// Precondition: \p decl must be a non-resilient struct or class.
  ///
  /// Precondition: \p field must be a stored property declared in \p decl,
  ///               not in a superclass.
  ///
  /// Postcondition: The returned index is unique across all properties in the
  ///                object, including properties declared in a superclass.
  unsigned getFieldIndex(NominalTypeDecl *decl, VarDecl *property);

  unsigned getCaseIndex(EnumElementDecl *enumElement);

  /// Called by SILBuilder whenever a new instruction is created and inserted.
  void notifyAddedInstruction(SILInstruction *inst);

  /// Called after an instruction is moved from one function to another.
  void notifyMovedInstruction(SILInstruction *inst, SILFunction *fromFunction);

  unsigned getNewFunctionIndex() { return nextFunctionIndex++; }

  // This may be larger that the number of live functions in the 'functions'
  // linked list because it includes the indices of zombie functions.
  unsigned getNumFunctionIndices() const { return nextFunctionIndex; }

  /// Set a serialization action.
  void setSerializeSILAction(ActionCallback SerializeSILAction);
  ActionCallback getSerializeSILAction() const;

  /// Set a flag indicating that this module is serialized already.
  void setSerialized() { serialized = true; }
  bool isSerialized() const { return serialized; }

  void setParsedAsSerializedSIL() {
    serialized = true;
    parsedAsSerializedSIL = true;
  }
  bool isParsedAsSerializedSIL() const { return parsedAsSerializedSIL; }

  void setBasicBlockName(const SILBasicBlock *block, StringRef name) {
#ifndef NDEBUG
    basicBlockNames[block] = name.str();
#endif
  }
  llvm::Optional<StringRef> getBasicBlockName(const SILBasicBlock *block) {
#ifndef NDEBUG
    auto Known = basicBlockNames.find(block);
    if (Known == basicBlockNames.end())
      return llvm::None;

    return StringRef(Known->second);
#else
    return llvm::None;
#endif
  }

  /// Serialize a SIL module using the configured SerializeSILAction.
  void serialize();

  /// This converts Swift types to SILTypes.
  Lowering::TypeConverter &Types;

  /// Invalidate cached entries in SIL Loader.
  void invalidateSILLoaderCaches();

  /// Erase a function from the module.
  void eraseFunction(SILFunction *F);

  /// Invalidate a function in SILLoader cache.
  void invalidateFunctionInSILCache(SILFunction *F);

  /// Specialization can cause a function that was erased before by dead function
  /// elimination to become alive again. If this happens we need to remove it
  /// from the list of zombies.
  SILFunction *removeFromZombieList(StringRef Name);

  /// Erase a global SIL variable from the module.
  void eraseGlobalVariable(SILGlobalVariable *G);

  /// Create and return an empty SIL module suitable for generating or parsing
  /// SIL into.
  ///
  /// \param context The associated decl context. This should be a FileUnit in
  /// single-file mode, and a ModuleDecl in whole-module mode.
  static std::unique_ptr<SILModule>
  createEmptyModule(llvm::PointerUnion<FileUnit *, ModuleDecl *> context,
                    Lowering::TypeConverter &TC, const SILOptions &Options,
                    const IRGenOptions *irgenOptions = nullptr);

  /// Get the Swift module associated with this SIL module.
  ModuleDecl *getSwiftModule() const { return TheSwiftModule; }
  /// Get the AST context used for type uniquing etc. by this SIL module.
  ASTContext &getASTContext() const;
  SourceManager &getSourceManager() const { return getASTContext().SourceMgr; }

  /// Get the Swift DeclContext associated with this SIL module. This is never
  /// null.
  ///
  /// All AST declarations within this context are assumed to have been fully
  /// processed as part of generating this module. This allows certain passes
  /// to make additional assumptions about these declarations.
  ///
  /// If this is the same as TheSwiftModule, the entire module is being
  /// compiled as a single unit.
  const DeclContext *getAssociatedContext() const {
    return AssociatedDeclContext;
  }

  /// Returns true if this SILModule really contains the whole module, i.e.
  /// optimizations can assume that they see the whole module.
  bool isWholeModule() const {
    return isa<ModuleDecl>(AssociatedDeclContext);
  }

  bool isStdlibModule() const;

  /// Returns true if it is the optimized OnoneSupport module.
  bool isOptimizedOnoneSupportModule() const;

  const SILOptions &getOptions() const { return Options; }
  const IRGenOptions *getIRGenOptionsOrNull() const {
    // This exposes target specific information, therefore serialized SIL
    // is also target specific.
    return irgenOptions;
  }

  using iterator = FunctionListType::iterator;
  using const_iterator = FunctionListType::const_iterator;
  FunctionListType &getFunctionList() { return functions; }
  const FunctionListType &getFunctionList() const { return functions; }
  iterator begin() { return functions.begin(); }
  iterator end() { return functions.end(); }
  const_iterator begin() const { return functions.begin(); }
  const_iterator end() const { return functions.end(); }
  iterator_range<iterator> getFunctions() {
    return {functions.begin(), functions.end()};
  }
  iterator_range<const_iterator> getFunctions() const {
    return {functions.begin(), functions.end()};
  }

  const_iterator zombies_begin() const { return zombieFunctions.begin(); }
  const_iterator zombies_end() const { return zombieFunctions.end(); }

  llvm::ArrayRef<SILVTable*> getVTables() const {
    return llvm::ArrayRef<SILVTable*>(vtables);
  }
  using vtable_iterator = VTableListType::iterator;
  using vtable_const_iterator = VTableListType::const_iterator;
  vtable_iterator vtable_begin() { return getVTables().begin(); }
  vtable_iterator vtable_end() { return getVTables().end(); }
  vtable_const_iterator vtable_begin() const { return getVTables().begin(); }
  vtable_const_iterator vtable_end() const { return getVTables().end(); }

  ArrayRef<SILMoveOnlyDeinit *> getMoveOnlyDeinits() const {
    return ArrayRef<SILMoveOnlyDeinit *>(moveOnlyDeinits);
  }
  using moveonlydeinit_iterator = SILMoveOnlyDeinitListType::iterator;
  using moveonlydeinit_const_iterator =
      SILMoveOnlyDeinitListType::const_iterator;
  moveonlydeinit_iterator moveonlydeinit_begin() {
    return getMoveOnlyDeinits().begin();
  }
  moveonlydeinit_iterator moveonlydeinit_end() {
    return getMoveOnlyDeinits().end();
  }
  moveonlydeinit_const_iterator moveonlydeinit_begin() const {
    return getMoveOnlyDeinits().begin();
  }
  moveonlydeinit_const_iterator moveonlydeinit_end() const {
    return getMoveOnlyDeinits().end();
  }

  using witness_table_iterator = WitnessTableListType::iterator;
  using witness_table_const_iterator = WitnessTableListType::const_iterator;
  WitnessTableListType &getWitnessTableList() { return witnessTables; }
  const WitnessTableListType &getWitnessTableList() const { return witnessTables; }
  witness_table_iterator witness_table_begin() { return witnessTables.begin(); }
  witness_table_iterator witness_table_end() { return witnessTables.end(); }
  witness_table_const_iterator witness_table_begin() const { return witnessTables.begin(); }
  witness_table_const_iterator witness_table_end() const { return witnessTables.end(); }
  iterator_range<witness_table_iterator> getWitnessTables() {
    return {witnessTables.begin(), witnessTables.end()};
  }
  iterator_range<witness_table_const_iterator> getWitnessTables() const {
    return {witnessTables.begin(), witnessTables.end()};
  }

  using default_witness_table_iterator = DefaultWitnessTableListType::iterator;
  using default_witness_table_const_iterator = DefaultWitnessTableListType::const_iterator;
  DefaultWitnessTableListType &getDefaultWitnessTableList() { return defaultWitnessTables; }
  const DefaultWitnessTableListType &getDefaultWitnessTableList() const { return defaultWitnessTables; }
  default_witness_table_iterator default_witness_table_begin() { return defaultWitnessTables.begin(); }
  default_witness_table_iterator default_witness_table_end() { return defaultWitnessTables.end(); }
  default_witness_table_const_iterator default_witness_table_begin() const { return defaultWitnessTables.begin(); }
  default_witness_table_const_iterator default_witness_table_end() const { return defaultWitnessTables.end(); }
  iterator_range<default_witness_table_iterator> getDefaultWitnessTables() {
    return {defaultWitnessTables.begin(), defaultWitnessTables.end()};
  }
  iterator_range<default_witness_table_const_iterator> getDefaultWitnessTables() const {
    return {defaultWitnessTables.begin(), defaultWitnessTables.end()};
  }

  using differentiability_witness_iterator = DifferentiabilityWitnessListType::iterator;
  using differentiability_witness_const_iterator = DifferentiabilityWitnessListType::const_iterator;
  DifferentiabilityWitnessListType &getDifferentiabilityWitnessList() { return differentiabilityWitnesses; }
  const DifferentiabilityWitnessListType &getDifferentiabilityWitnessList() const { return differentiabilityWitnesses; }
  differentiability_witness_iterator differentiability_witness_begin() { return differentiabilityWitnesses.begin(); }
  differentiability_witness_iterator differentiability_witness_end() { return differentiabilityWitnesses.end(); }
  differentiability_witness_const_iterator differentiability_witness_begin() const { return differentiabilityWitnesses.begin(); }
  differentiability_witness_const_iterator differentiability_witness_end() const { return differentiabilityWitnesses.end(); }
  iterator_range<differentiability_witness_iterator>
  getDifferentiabilityWitnesses() {
    return {differentiabilityWitnesses.begin(),
            differentiabilityWitnesses.end()};
  }
  iterator_range<differentiability_witness_const_iterator>
  getDifferentiabilityWitnesses() const {
    return {differentiabilityWitnesses.begin(),
            differentiabilityWitnesses.end()};
  }

  void addExternallyVisibleDecl(ValueDecl *decl) {
    externallyVisible.insert(decl);
  }
  bool isExternallyVisibleDecl(ValueDecl *decl) {
    return externallyVisible.count(decl) != 0;
  }

  using sil_global_iterator = GlobalListType::iterator;
  using sil_global_const_iterator = GlobalListType::const_iterator;
  GlobalListType &getSILGlobalList() { return silGlobals; }
  const GlobalListType &getSILGlobalList() const { return silGlobals; }
  sil_global_iterator sil_global_begin() { return silGlobals.begin(); }
  sil_global_iterator sil_global_end() { return silGlobals.end(); }
  sil_global_const_iterator sil_global_begin() const {
    return silGlobals.begin();
  }
  sil_global_const_iterator sil_global_end() const {
    return silGlobals.end();
  }
  iterator_range<sil_global_iterator> getSILGlobals() {
    return {silGlobals.begin(), silGlobals.end()};
  }
  iterator_range<sil_global_const_iterator> getSILGlobals() const {
    return {silGlobals.begin(), silGlobals.end()};
  }

  using coverage_map_iterator = CoverageMapCollectionType::iterator;
  using coverage_map_const_iterator = CoverageMapCollectionType::const_iterator;
  CoverageMapCollectionType &getCoverageMaps() { return coverageMaps; }
  const CoverageMapCollectionType &getCoverageMaps() const {
    return coverageMaps;
  }

  swift::SILRemarkStreamer *getSILRemarkStreamer() {
    return silRemarkStreamer.get();
  }

  void installSILRemarkStreamer();

  // This is currently limited to VarDecl because the visibility of global
  // variables and class properties is straightforward, while the visibility of
  // class methods (ValueDecls) depends on the subclass scope. "Visibility" has
  // a different meaning when vtable layout is at stake.
  bool isVisibleExternally(const VarDecl *decl) {
    return isPossiblyUsedExternally(getDeclSILLinkage(decl), isWholeModule());
  }

  PropertyListType &getPropertyList() { return properties; }
  const PropertyListType &getPropertyList() const { return properties; }

  /// Look for a global variable by name.
  ///
  /// \return null if this module has no such global variable
  SILGlobalVariable *lookUpGlobalVariable(StringRef name) const {
    return GlobalVariableMap.lookup(name);
  }

  /// Look for a function by name.
  ///
  /// \return null if this module has no such function
  SILFunction *lookUpFunction(StringRef name) const {
    return FunctionTable.lookup(name);
  }

  /// Look for a function by declaration.
  ///
  /// \return null if this module has no such function
  SILFunction *lookUpFunction(SILDeclRef fnRef);

  /// Attempt to deserialize function \p F and all functions which are referenced
  /// from \p F (according to the \p LinkMode).
  ///
  /// Returns true if deserialization succeeded, false otherwise.
  bool loadFunction(SILFunction *F, LinkingMode LinkMode);

  /// Attempt to deserialize a function with \p name and all functions which are
  /// referenced from that function (according to the \p LinkMode).
  ///
  /// If \p linkage is provided, the deserialized function is required to have
  /// that linkage. Returns null, if this is not the case.
  SILFunction *loadFunction(StringRef name, LinkingMode LinkMode,
                            llvm::Optional<SILLinkage> linkage = llvm::None);

  /// Update the linkage of the SILFunction with the linkage of the serialized
  /// function.
  ///
  /// The serialized SILLinkage can differ from the linkage derived from the
  /// AST, e.g. cross-module-optimization can change the SIL linkages.
  void updateFunctionLinkage(SILFunction *F);

  /// Attempt to deserialize function \p F and all required referenced functions.
  ///
  /// Returns true if linking succeeded, false otherwise.
  bool linkFunction(SILFunction *F, LinkingMode LinkMode);

  /// Check if a given function exists in any of the modules.
  /// i.e. it can be linked by linkFunction.
  bool hasFunction(StringRef Name);

  /// Look up the SILWitnessTable representing the lowering of a protocol
  /// conformance, and collect the substitutions to apply to the referenced
  /// witnesses, if any.
  ///
  /// \arg C The protocol conformance mapped key to use to lookup the witness
  ///        table.
  /// \arg deserializeLazily If we cannot find the witness table should we
  ///                        attempt to lazily deserialize it.
  SILWitnessTable *lookUpWitnessTable(const ProtocolConformance *C);

  /// Attempt to lookup \p Member in the witness table for \p C.
  ///
  /// Also, deserialize all referenced functions according to the \p linkgingMode.
  std::pair<SILFunction *, SILWitnessTable *>
  lookUpFunctionInWitnessTable(ProtocolConformanceRef C,
                               SILDeclRef Requirement,
                               SILModule::LinkingMode linkingMode);

  /// Look up the SILDefaultWitnessTable representing the default witnesses
  /// of a resilient protocol, if any.
  SILDefaultWitnessTable *lookUpDefaultWitnessTable(const ProtocolDecl *Protocol,
                                                    bool deserializeLazily=true);

  /// Attempt to lookup \p Member in the default witness table for \p Protocol.
  std::pair<SILFunction *, SILDefaultWitnessTable *>
  lookUpFunctionInDefaultWitnessTable(const ProtocolDecl *Protocol,
                                      SILDeclRef Requirement,
                                      bool deserializeLazily=true);

  /// Look up the VTable mapped to the given ClassDecl. Returns null on failure.
  SILVTable *lookUpVTable(const ClassDecl *C, bool deserializeLazily = true);

  /// Attempt to lookup the function corresponding to \p Member in the class
  /// hierarchy of \p Class.
  SILFunction *lookUpFunctionInVTable(ClassDecl *Class, SILDeclRef Member);

  /// Look up the deinit mapped to the given move only nominal type decl.
  /// Returns null on failure.
  SILMoveOnlyDeinit *lookUpMoveOnlyDeinit(const NominalTypeDecl *nomDecl,
                                          bool deserializeLazily = true);

  /// Look up the function mapped to the given move only nominal type decl.
  /// Returns null on failure.
  SILFunction *lookUpMoveOnlyDeinitFunction(const NominalTypeDecl *nomDecl);

  /// Look up the differentiability witness with the given name.
  SILDifferentiabilityWitness *lookUpDifferentiabilityWitness(StringRef name);

  /// Look up the differentiability witness corresponding to the given key.
  SILDifferentiabilityWitness *
  lookUpDifferentiabilityWitness(SILDifferentiabilityWitnessKey key);

  /// Look up the differentiability witness corresponding to the given function.
  llvm::ArrayRef<SILDifferentiabilityWitness *>
  lookUpDifferentiabilityWitnessesForFunction(StringRef name);

  /// Attempt to deserialize the SILDifferentiabilityWitness. Returns true if
  /// deserialization succeeded, false otherwise.
  bool loadDifferentiabilityWitness(SILDifferentiabilityWitness *dw);

  // Given a protocol, attempt to create a default witness table declaration
  // for it.
  SILDefaultWitnessTable *
  createDefaultWitnessTableDeclaration(const ProtocolDecl *Protocol,
                                       SILLinkage Linkage);

  /// Deletes a dead witness table.
  void deleteWitnessTable(SILWitnessTable *Wt);

  /// Return the stage of processing this module is at.
  SILStage getStage() const { return Stage; }

  /// Advance the module to a further stage of processing.
  void setStage(SILStage s) {
    assert(s >= Stage && "regressing stage?!");
    Stage = s;
  }

  /// True if SIL conventions force address-only to be passed by address.
  bool useLoweredAddresses() const { return loweredAddresses; }

  void setLoweredAddresses(bool val) {
    loweredAddresses = val;
    if (val) {
      Types.setLoweredAddresses();
    }
  }

  llvm::IndexedInstrProfReader *getPGOReader() const { return PGOReader.get(); }

  void setPGOReader(std::unique_ptr<llvm::IndexedInstrProfReader> IPR) {
    PGOReader = std::move(IPR);
  }

  IndexTrieNode *getIndexTrieRoot() { return indexTrieRoot.get(); }

  /// Can value operations (copies and destroys) on the given lowered type
  /// be performed in this module?
  bool isTypeABIAccessible(SILType type,
                           TypeExpansionContext forExpansion);

  /// Can type metadata for the given formal type be fetched in
  /// the given module?
  bool isTypeMetadataAccessible(CanType type);

  /// Can type metadata necessary for value operations for the given sil type be
  /// fetched in the given module?
  bool isTypeMetadataForLayoutAccessible(SILType type);

  void verify(bool isCompleteOSSA = true,
              bool checkLinearLifetime = true) const;

  /// Run the SIL verifier to make sure that all Functions follow
  /// invariants.
  void verify(SILPassManager *passManager,
              bool isCompleteOSSA = true,
              bool checkLinearLifetime = true) const;

  /// Run the SIL verifier without assuming OSSA lifetimes end at dead end
  /// blocks.
  void verifyIncompleteOSSA() const {
    verify(/*isCompleteOSSA=*/false);
  }

  /// Check linear OSSA lifetimes, assuming complete OSSA.
  void verifyOwnership() const;

  /// Check if there are any leaking instructions.
  ///
  /// Aborts with an error if more instructions are allocated than contained in
  /// the module.
  void checkForLeaks() const;

  /// Check if there are any leaking instructions after the SILModule is
  /// destructed.
  ///
  /// The SILModule destructor already calls checkForLeaks(). This function is
  /// useful to check if the destructor itself destroys all data structures.
  static void checkForLeaksAfterDestruction();

  /// Pretty-print the module.
  void dump(bool Verbose = false) const;

  /// Pretty-print the module to a file.
  /// Useful for dumping the module when running in a debugger.
  /// Warning: no error handling is done. Fails with an assert if the file
  /// cannot be opened.
  void dump(const char *FileName, bool Verbose = false,
            bool PrintASTDecls = false) const;

  /// Pretty-print the module to the designated stream.
  ///
  /// \param M If present, the types and declarations from this module will be
  ///        printed. The module would usually contain the types and Decls that
  ///        the SIL module depends on.
  /// \param Opts The SIL options, used to determine printing verbosity and
  ///        and sorting.
  /// \param PrintASTDecls If set to true print AST decls.
  void print(raw_ostream &OS, ModuleDecl *M = nullptr,
             const SILOptions &Opts = SILOptions(),
             bool PrintASTDecls = true) const {
    SILPrintContext PrintCtx(OS, Opts);
    print(PrintCtx, M, PrintASTDecls);
  }

  /// Pretty-print the module with the context \p PrintCtx.
  ///
  /// \param M If present, the types and declarations from this module will be
  ///        printed. The module would usually contain the types and Decls that
  ///        the SIL module depends on.
  /// \param PrintASTDecls If set to true print AST decls.
  void print(SILPrintContext &PrintCtx, ModuleDecl *M = nullptr,
             bool PrintASTDecls = true) const;

  /// Allocate memory using the module's internal allocator.
  void *allocate(unsigned Size, unsigned Align) const;

  template <typename T> T *allocate(unsigned Count) const {
    return static_cast<T *>(allocate(sizeof(T) * Count, alignof(T)));
  }

  /// Allocates a slab of memory.
  ///
  /// This has (almost) zero cost, because for the first time, the allocation is
  /// done with the BPA.
  /// Subsequent allocations are reusing the already freed slabs.
  FixedSizeSlab *allocSlab();
  
  /// Frees a slab.
  ///
  /// This has (almost) zero cost, because the slab is just put into the
  /// freeSlabs list.
  void freeSlab(FixedSizeSlab *slab);
  
  /// Frees all slabs of a list.
  void freeAllSlabs(SlabList &slabs);

  template <typename T>
  MutableArrayRef<T> allocateCopy(ArrayRef<T> Array) const {
    MutableArrayRef<T> result(allocate<T>(Array.size()), Array.size());
    std::uninitialized_copy(Array.begin(), Array.end(), result.begin());
    return result;
  }

  StringRef allocateCopy(StringRef Str) const {
    auto result = allocateCopy<char>({Str.data(), Str.size()});
    return {result.data(), result.size()};
  }

  /// Allocate memory for an instruction using the module's internal allocator.
  void *allocateInst(unsigned Size, unsigned Align) const;

  /// Called before \p I is removed from its basic block and scheduled for
  /// deletion.
  void willDeleteInstruction(SILInstruction *I);

  /// Schedules the (already removed) instruction \p I for deletion.
  /// See scheduledForDeletion for details.
  void scheduleForDeletion(SILInstruction *I);

  /// Deletes all scheduled instructions for real.
  /// See scheduledForDeletion for details.
  void flushDeletedInsts();

  /// Looks up the llvm intrinsic ID and type for the builtin function.
  ///
  /// \returns Returns llvm::Intrinsic::not_intrinsic if the function is not an
  /// intrinsic. The particular intrinsic functions which correspond to the
  /// returned value are defined in llvm/Intrinsics.h.
  const IntrinsicInfo &getIntrinsicInfo(Identifier ID);

  /// Looks up the lazily cached identification for the builtin function.
  ///
  /// \returns Returns builtin info of BuiltinValueKind::None kind if the
  /// declaration is not a builtin.
  const BuiltinInfo &getBuiltinInfo(Identifier ID);

  /// Returns true if the builtin or intrinsic is no-return.
  bool isNoReturnBuiltinOrIntrinsic(Identifier Name);

  /// Returns true if the default atomicity of the module is Atomic.
  bool isDefaultAtomic() const {
    return !getOptions().AssumeSingleThreaded;
  }

  /// Returns true if SIL entities associated with declarations in the given
  /// declaration context ought to be serialized as part of this module.
  bool
  shouldSerializeEntitiesAssociatedWithDeclContext(const DeclContext *DC) const;

  /// Gather prespecialized from extensions.
  void performOnceForPrespecializedImportedExtensions(
      llvm::function_ref<void(AbstractFunctionDecl *)> action);
};

inline llvm::raw_ostream &operator<<(llvm::raw_ostream &OS, const SILModule &M){
  M.print(OS);
  return OS;
}

inline bool SILOptions::supportsLexicalLifetimes(const SILModule &mod) const {
  switch (mod.getStage()) {
  case SILStage::Raw:
    // In raw SIL, lexical markers are used for diagnostics.  These markers are
    // present as long as the lexical lifetimes feature is not disabled
    // entirely.
    return LexicalLifetimes != LexicalLifetimesOption::Off;
  case SILStage::Canonical:
  case SILStage::Lowered:
    // In Canonical SIL, lexical markers are used to ensure that object
    // lifetimes do not get observably shortened from the end of a lexical
    // scope.  That behavior only occurs when lexical lifetimes is (fully)
    // enabled.  (When only diagnostic markers are enabled, the markers are
    // stripped as part of lowering from raw to canonical SIL.)
    return LexicalLifetimes == LexicalLifetimesOption::On;
  }
}

/// Print a simple description of a SILModule for the request evaluator.
void simple_display(llvm::raw_ostream &out, const SILModule *M);

/// Retrieve a SourceLoc for a SILModule that the request evaluator can use for
/// diagnostics.
SourceLoc extractNearestSourceLoc(const SILModule *SM);

namespace Lowering {
/// Determine whether the given class will be allocated/deallocated using the
/// Objective-C runtime, i.e., +alloc and -dealloc.
LLVM_LIBRARY_VISIBILITY bool usesObjCAllocator(ClassDecl *theClass);
} // namespace Lowering
} // namespace swift

#endif
