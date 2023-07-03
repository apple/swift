//===--- Serialization.h - Read and write Swift modules ---------*- C++ -*-===//
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
//  This file defines the Serializer interface.
//
//===----------------------------------------------------------------------===//

#ifndef SWIFT_SERIALIZATION_SERIALIZATION_H
#define SWIFT_SERIALIZATION_SERIALIZATION_H

#include "ModuleFormat.h"
#include "swift/Serialization/SerializationOptions.h"
#include "swift/Subsystems.h"
#include "swift/AST/Identifier.h"
#include "swift/AST/RequirementSignature.h"
#include "swift/Basic/LLVM.h"
#include "llvm/ADT/MapVector.h"
#include <array>
#include <queue>
#include <tuple>

namespace clang {
  class Type;
}

namespace swift {
  class SILModule;

  namespace fine_grained_dependencies {
    class SourceFileDepGraph;
  }

namespace serialization {

using FilenamesTy = ArrayRef<std::string>;

class SerializerBase {
protected:
  SmallVector<char, 0> Buffer;
  llvm::BitstreamWriter Out{Buffer};

  /// A reusable buffer for emitting records.
  SmallVector<uint64_t, 64> ScratchRecord;

  /// The module currently being serialized.
  const ModuleDecl *M = nullptr;

  /// The SourceFile currently being serialized, if any.
  ///
  /// If this is non-null, only decls actually from this SourceFile will be
  /// serialized. Any other decls will be cross-referenced instead.
  const SourceFile *SF = nullptr;

  /// Record the name of a block.
  void emitBlockID(unsigned ID, StringRef name,
                   SmallVectorImpl<unsigned char> &nameBuffer);

  /// Record the name of a record within a block.
  void emitRecordID(unsigned ID, StringRef name,
                    SmallVectorImpl<unsigned char> &nameBuffer,
                    SmallVectorImpl<unsigned> *wideNameBuffer = nullptr);

  void writeToStream(raw_ostream &os);

public:
  SerializerBase(ArrayRef<unsigned char> signature, ModuleOrSourceFile DC);

  ASTContext &getASTContext() const;
};

class Serializer : public SerializerBase {
  class DeclSerializer;
  friend class DeclSerializer;
  class TypeSerializer;
  friend class TypeSerializer;

  /// A map from non-identifier uniqued strings to their serialized IDs.
  ///
  /// Since we never remove items from this map, we can use a BumpPtrAllocator
  /// to back the entries.
  llvm::StringMap<IdentifierID, llvm::BumpPtrAllocator> UniquedStringIDs;

  /// A map from Identifiers to their serialized IDs.
  ///
  /// This is stored separately from \p UniquedStringIDs because it's faster
  /// to do lookups in, even though that may lead to some duplication between
  /// identifier and non-identifier strings.
  llvm::DenseMap<Identifier, IdentifierID> IdentifierIDs;

  /// All uniqued strings that need to be serialized (identifiers and
  /// non-identifiers).
  std::vector<StringRef> StringsToWrite;

  /// The last assigned IdentifierID for uniqued strings from this module.
  ///
  /// Note that special module IDs and IDs of special names must not be valid
  /// IdentifierIDs, except that 0 will always represent the empty identifier.
  uint32_t /*IdentifierID*/ LastUniquedStringID =
      serialization::NUM_SPECIAL_IDS - 1;

  SmallVector<DeclID, 16> exportedPrespecializationDecls;

  /// Helper for serializing entities in the AST block object graph.
  ///
  /// Keeps track of assigning IDs to newly-seen entities, and collecting
  /// offsets when those entities are ready to be serialized.
  ///
  /// \tparam T The AST type that's being serialized
  /// \tparam ID The ID type for encoding into the bitstream
  /// \tparam RecordCode_ The code for the offsets record in the Index block for
  /// referring to these entities.
  template <typename T, typename ID, unsigned RecordCode_>
  class ASTBlockRecordKeeper {
    /// Map of entities to assigned ID numbers.
    llvm::DenseMap<T, ID> IDs;

    /// All entities that still need to be written.
    ///
    /// This is a queue and not simply a vector because serializing one
    /// entity might result in another one getting queued up.
    std::queue<T> EntitiesToWrite;

    /// The offsets of entities that have already been written.
    ///
    /// `Offsets[entityID - 1]` gives the offset for a particular entity. (0 is
    /// reserved for "empty" entities.)
    std::vector<BitOffset> Offsets;
  public:
    /// The code for the offsets record in the Index block for referring to
    /// these entities.
    static const unsigned RecordCode = RecordCode_;

    /// Assigns \p entity an ID and schedules it for writing.
    ///
    /// If \p entity has been seen before, returns the existing ID.
    ///
    /// 0 is used for "empty" entities (those that coerce to \c false ).
    ID addRef(T entity) {
      assert(ID() == 0 && "bad default constructor for ID");
      if (!entity)
        return 0;

      auto &entityID = IDs[entity];
      if (entityID == ID()) {
        EntitiesToWrite.push(entity);
        entityID = IDs.size();
      }
      return entityID;
    }

    /// Returns true if \p entity has been seen before (i.e. ever passed to
    /// #addRef).
    bool hasRef(T entity) const {
      return IDs.find(entity) != IDs.end();
    }

    bool hasMoreToSerialize() const {
      return !EntitiesToWrite.empty();
    }

    /// Returns the next entity to be written.
    ///
    /// If there is nothing left to serialize, returns None.
    llvm::Optional<T> peekNext() const {
      if (!hasMoreToSerialize())
        return llvm::None;
      return EntitiesToWrite.front();
    }

    /// Records that the next entity will be written at \p offset, and returns
    /// it so it can be written.
    ///
    /// If there is nothing left to serialize, returns None.
    llvm::Optional<T> popNext(BitOffset offset) {
      if (!hasMoreToSerialize())
        return llvm::None;
      T result = EntitiesToWrite.front();
      EntitiesToWrite.pop();
      Offsets.push_back(offset);
      assert(IDs.lookup(result) == Offsets.size());
      return result;
    }

    /// Returns the offsets for all entities that have been written.
    ///
    /// Should only be called after all entities \e have been written.
    ArrayRef<BitOffset> getOffsets() const {
      assert(!hasMoreToSerialize() && "not all entities were serialized");
      return Offsets;
    }
  };

  ASTBlockRecordKeeper<const Decl *, DeclID,
                       index_block::DECL_OFFSETS>
  DeclsToSerialize;

  ASTBlockRecordKeeper<Type, TypeID,
                       index_block::TYPE_OFFSETS>
  TypesToSerialize;

  ASTBlockRecordKeeper<const clang::Type *, ClangTypeID,
                       index_block::CLANG_TYPE_OFFSETS>
  ClangTypesToSerialize;

  ASTBlockRecordKeeper<const DeclContext *, LocalDeclContextID,
                       index_block::LOCAL_DECL_CONTEXT_OFFSETS>
  LocalDeclContextsToSerialize;

  ASTBlockRecordKeeper<GenericSignature, GenericSignatureID,
                       index_block::GENERIC_SIGNATURE_OFFSETS>
  GenericSignaturesToSerialize;

  ASTBlockRecordKeeper<const GenericEnvironment *, GenericEnvironmentID,
                       index_block::GENERIC_ENVIRONMENT_OFFSETS>
  GenericEnvironmentsToSerialize;

  ASTBlockRecordKeeper<SubstitutionMap, SubstitutionMapID,
                       index_block::SUBSTITUTION_MAP_OFFSETS>
  SubstitutionMapsToSerialize;

  ASTBlockRecordKeeper<ProtocolConformance *, ProtocolConformanceID,
                       index_block::PROTOCOL_CONFORMANCE_OFFSETS>
  ConformancesToSerialize;

  ASTBlockRecordKeeper<PackConformance *, ProtocolConformanceID,
                       index_block::PACK_CONFORMANCE_OFFSETS>
  PackConformancesToSerialize;

  ASTBlockRecordKeeper<const SILLayout *, SILLayoutID,
                       index_block::SIL_LAYOUT_OFFSETS>
  SILLayoutsToSerialize;

public:
  using DeclTableData = SmallVector<std::pair<uint8_t, DeclID>, 4>;
  /// The in-memory representation of what will eventually be an on-disk hash
  /// table.
  using DeclTable = llvm::MapVector<DeclBaseName, DeclTableData>;

  using ObjCMethodTableData =
    SmallVector<std::tuple<std::string, bool, DeclID>, 4>;

  // In-memory representation of what will eventually be an on-disk
  // hash table of all defined Objective-C methods.
  using ObjCMethodTable = llvm::MapVector<ObjCSelector, ObjCMethodTableData>;

  using NestedTypeDeclsData = SmallVector<std::pair<DeclID, DeclID>, 4>;
  // In-memory representation of what will eventually be an on-disk
  // hash table of all defined Objective-C methods.
  using NestedTypeDeclsTable = llvm::MapVector<Identifier, NestedTypeDeclsData>;

  using DeclMembersData = SmallVector<DeclID, 2>;
  // In-memory representation of what will eventually be an on-disk
  // hash table of all ValueDecl-members of a particular DeclBaseName.
  using DeclMembersTable = llvm::MapVector<uint32_t, DeclMembersData>;

  using DeclMemberNamesData = std::pair<serialization::BitOffset,
                                        std::unique_ptr<DeclMembersTable>>;
  // In-memory representation of what will eventually be an on-disk
  // hash table mapping DeclBaseNames to DeclMembersData tables.
  using DeclMemberNamesTable = llvm::MapVector<DeclBaseName, DeclMemberNamesData>;

  using ExtensionTableData =
      SmallVector<std::pair<const NominalTypeDecl *, DeclID>, 4>;
  using ExtensionTable = llvm::MapVector<Identifier, ExtensionTableData>;

  using DerivativeFunctionConfigTableData =
      llvm::SmallVector<std::pair<std::string, GenericSignatureID>, 4>;
  // In-memory representation of what will eventually be an on-disk hash table
  // mapping original declaration USRs to derivative function configurations.
  using DerivativeFunctionConfigTable =
      llvm::MapVector<Identifier, DerivativeFunctionConfigTableData>;
  // Uniqued mapping from original declarations USRs to derivative function
  // configurations.
  // Note: this exists because `GenericSignature` can be used as a `DenseMap`
  // key, while `GenericSignatureID` cannot
  // (`DenseMapInfo<GenericSignatureID>::getEmptyKey()` crashes). To work
  // around this, a `UniquedDerivativeFunctionConfigTable` is first
  // constructed, and then converted to a `DerivativeFunctionConfigTableData`.
  using UniquedDerivativeFunctionConfigTable = llvm::MapVector<
      Identifier,
      llvm::SmallSetVector<std::pair<Identifier, GenericSignature>, 4>>;

  // In-memory representation of what will eventually be an on-disk
  // hash table of the fingerprint associated with a serialized
  // iterable decl context. It is keyed by that context's decl ID.
  using DeclFingerprintsTable = llvm::MapVector<uint32_t, Fingerprint>;

private:
  /// A map from identifiers to methods and properties with the given name.
  ///
  /// This is used for id-style lookup.
  DeclTable ClassMembersForDynamicLookup;

  /// A map from DeclBaseNames of members to Decl->members sub-tables.
  ///
  /// This is for Named Lazy Member Loading.
  DeclMemberNamesTable DeclMemberNames;

  enum {NumDeclTypeAbbrCodes = 512};

  /// The abbreviation code for each record in the "decls-and-types" block.
  ///
  /// These are registered up front when entering the block, so they can be
  /// reused.
  std::array<unsigned, NumDeclTypeAbbrCodes> DeclTypeAbbrCodes;

  /// The decls that adopt compiler-known protocols.
  SmallVector<DeclID, 2> KnownProtocolAdopters[NumKnownProtocols];

  /// Writes the BLOCKINFO block for the serialized module file.
  void writeBlockInfoBlock();

  /// Writes the Swift module file header and name, plus metadata determining
  /// if the module can be loaded.
  void writeHeader(const SerializationOptions &options = {});

  /// Writes the dependencies used to build this module: its imported
  /// modules and its source files.
  void writeInputBlock(const SerializationOptions &options);

  /// Check if a decl is cross-referenced.
  bool isDeclXRef(const Decl *D) const;

  /// Writes a reference to a decl in another module.
  void writeCrossReference(const DeclContext *DC, uint32_t pathLen = 1);

  /// Writes a reference to a decl in another module.
  void writeCrossReference(const Decl *D);

  /// Writes the given decl.
  void writeASTBlockEntity(const Decl *D);

  /// Write a DeclContext as a local DeclContext at the current offset.
  void writeASTBlockEntity(const DeclContext *DC);

  /// Write the components of a PatternBindingInitializer as a local context.
  void writePatternBindingInitializer(PatternBindingDecl *binding,
                                      unsigned bindingIndex);

  /// Write the components of a DefaultArgumentInitializer as a local context.
  void writeDefaultArgumentInitializer(const DeclContext *parentContext, unsigned index);

  /// Write the components of an AbstractClosureExpr as a local context.
  void writeAbstractClosureExpr(const DeclContext *parentContext, Type Ty, bool isImplicit, unsigned discriminator);

  /// Writes the given type.
  void writeASTBlockEntity(Type ty);

  /// Writes the given Clang type.
  void writeASTBlockEntity(const clang::Type *ty);

  /// Writes a generic signature.
  void writeASTBlockEntity(GenericSignature sig);

  /// Writes a generic environment.
  void writeASTBlockEntity(const GenericEnvironment *env);

  /// Writes a substitution map.
  void writeASTBlockEntity(const SubstitutionMap substitutions);

  /// Writes a protocol conformance.
  void writeASTBlockEntity(ProtocolConformance *conformance);

  void writeLocalNormalProtocolConformance(NormalProtocolConformance *);

  /// Writes a pack conformance.
  void writeASTBlockEntity(PackConformance *conformance);

  /// Registers the abbreviation for the given decl or type layout.
  template <typename Layout>
  void registerDeclTypeAbbr() {
    using AbbrArrayTy = decltype(DeclTypeAbbrCodes);
    static_assert(Layout::Code <= std::tuple_size<AbbrArrayTy>::value,
                  "layout has invalid record code");
    DeclTypeAbbrCodes[Layout::Code] = Layout::emitAbbrev(Out);
  }

  /// Writes all queued \p entities until there are no more.
  ///
  /// \returns true if any entities were written
  template <typename SpecificASTBlockRecordKeeper>
  bool writeASTBlockEntitiesIfNeeded(SpecificASTBlockRecordKeeper &entities);

  /// Writes all decls and types in the DeclsToWrite queue.
  ///
  /// This will continue until the queue is empty, even if the items currently
  /// in the queue trigger the serialization of additional decls and/or types.
  void writeAllDeclsAndTypes();

  /// Writes all identifiers in the IdentifiersToWrite queue.
  ///
  /// This must be called after writeAllDeclsAndTypes(), since that may add
  /// additional identifiers to the pool.
  std::vector<CharOffset> writeAllIdentifiers();

  /// Writes the offsets for a serialized entity kind.
  ///
  /// \see ASTBlockRecordKeeper
  template <typename SpecificASTBlockRecordKeeper>
  void writeOffsets(const index_block::OffsetsLayout &Offsets,
                    const SpecificASTBlockRecordKeeper &entities);

  /// Serializes all transparent SIL functions in the SILModule.
  void writeSIL(const SILModule *M, bool serializeAllSIL);

  /// Top-level entry point for serializing a module.
  void writeAST(ModuleOrSourceFile DC);

  /// Serializes the given dependency graph into the incremental information
  /// section of this swift module.
  void writeIncrementalInfo(
      const fine_grained_dependencies::SourceFileDepGraph *DepGraph);

  using SerializerBase::SerializerBase;
  using SerializerBase::writeToStream;

public:
  /// Serialize a module to the given stream.
  static void
  writeToStream(raw_ostream &os, ModuleOrSourceFile DC,
                const SILModule *M,
                const SerializationOptions &options,
                const fine_grained_dependencies::SourceFileDepGraph *DepGraph);

  /// Records the use of the given Type.
  ///
  /// The Type will be scheduled for serialization if necessary.
  ///
  /// \returns The ID for the given Type in this module.
  TypeID addTypeRef(Type ty);

  /// Records the use of the given C type.
  ///
  /// The type will be scheduled for serialization if necessary.,
  ///
  /// \returns The ID for the given type in this module.
  ClangTypeID addClangTypeRef(const clang::Type *ty);

  /// Records the use of the given DeclBaseName.
  ///
  /// The Identifier will be scheduled for serialization if necessary.
  ///
  /// \returns The ID for the given DeclBaseName in this module.
  IdentifierID addDeclBaseNameRef(DeclBaseName ident);

  /// Records the use of the given string, which will only be stored once in
  /// the resulting module file.
  ///
  /// \returns A pair containing the copy of the string now owned by the
  /// Serializer and the ID for the string in this module.
  /// \sa addUniquedStringRef
  std::pair<StringRef, IdentifierID> addUniquedString(StringRef str);

  /// Records the use of the given string, which will only be stored once in
  /// the resulting module file.
  ///
  /// \returns The ID for the given string in this module.
  /// \sa addUniquedString
  IdentifierID addUniquedStringRef(StringRef str) {
    return addUniquedString(str).second;
  }

  /// Records the use of the given file name.
  ///
  /// The Identifier will be scheduled for serialization if necessary.
  ///
  /// \returns The ID for the given file name in this module.
  IdentifierID addFilename(StringRef filename);

  /// Records the use of the given Decl.
  ///
  /// The Decl will be scheduled for serialization if necessary.
  ///
  /// \returns The ID for the given Decl in this module.
  DeclID addDeclRef(const Decl *D, bool allowTypeAliasXRef = false);

  /// Records the use of the given DeclContext.
  ///
  /// The DeclContext will be scheduled for serialization if necessary.
  DeclContextID addDeclContextRef(const DeclContext *DC);

  /// Records the use of the given local DeclContext.
  ///
  /// The DeclContext will be scheduled for serialization if necessary.
  LocalDeclContextID addLocalDeclContextRef(const DeclContext *DC);

  /// Records the use of the given generic signature.
  ///
  /// The GenericSignature will be scheduled for serialization if necessary.
  GenericSignatureID addGenericSignatureRef(GenericSignature sig);

  /// Records the use of the given opened generic environment.
  GenericEnvironmentID addGenericEnvironmentRef(GenericEnvironment *env);

  /// Records the use of the given substitution map.
  ///
  /// The SubstitutionMap will be scheduled for serialization if necessary.
  SubstitutionMapID addSubstitutionMapRef(SubstitutionMap substitutions);

  /// Records the use of the given protocol conformance.
  ///
  /// The protocol conformance will be scheduled for serialization
  /// if necessary.
  ///
  /// \returns The ID for the given conformance in this module.
  ProtocolConformanceID addConformanceRef(ProtocolConformance *conformance);
  ProtocolConformanceID addConformanceRef(PackConformance *conformance);
  ProtocolConformanceID addConformanceRef(ProtocolConformanceRef conformance);

  SmallVector<ProtocolConformanceID, 4>
  addConformanceRefs(ArrayRef<ProtocolConformanceRef> conformances);
  SmallVector<ProtocolConformanceID, 4>
  addConformanceRefs(ArrayRef<ProtocolConformance *> conformances);

  /// Records the use of the given SILLayout.
  SILLayoutID addSILLayoutRef(const SILLayout *layout);

  /// Records the module containing \p DC.
  ///
  /// The module's name will be scheduled for serialization if necessary. This
  /// may not be exactly the same as the name of the module containing DC;
  /// instead, it will match the containing file's "exported module name".
  ///
  /// \param ignoreExport When true, register the real module name,
  /// ignoring exported_as definitions.
  /// \returns The ID for the identifier for the module's name, or one of the
  /// special module codes defined above.
  /// \see FileUnit::getExportedModuleName
  IdentifierID addContainingModuleRef(const DeclContext *DC,
                                      bool ignoreExport);

  /// Records the module \m.
  IdentifierID addModuleRef(const ModuleDecl *m);

  /// Write a SILLayout.
  void writeASTBlockEntity(const SILLayout *layout);

  /// Adds an encoding of the given list of generic requirements to
  /// the given list of values.
  void serializeGenericRequirements(ArrayRef<Requirement> requirements,
                                    SmallVectorImpl<uint64_t> &scratch);

  /// Writes a protocol's requirement signature, consisting of a list of
  /// generic requirements and a list of protocol typealias records.
  void writeRequirementSignature(const RequirementSignature &requirementSig);

  /// Writes a protocol's associated type table.
  void writeAssociatedTypes(ArrayRef<AssociatedTypeDecl *> assocTypes);

  /// Writes a protocol's primary associated type table.
  void writePrimaryAssociatedTypes(ArrayRef<AssociatedTypeDecl *> assocTypes);

  bool allowCompilerErrors() const;
};

} // end namespace serialization
} // end namespace swift
#endif
