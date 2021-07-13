//===------------- TypeLayout.h ---------------  Type layouts ---*- C++ -*-===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2019 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//

#ifndef SWIFT_IRGEN_TYPE_LAYOUT_H
#define SWIFT_IRGEN_TYPE_LAYOUT_H

#include "TypeInfo.h"
#include "FixedTypeInfo.h"
#include "swift/SIL/SILType.h"
#include "llvm/ADT/FoldingSet.h"
#include "llvm/Support/Debug.h"

namespace swift {
namespace irgen {

class EnumTypeLayoutEntry;

enum class TypeLayoutEntryKind : uint8_t {
  Empty,
  Scalar,
  Archetype,
  AlignedGroup,
  Resilient,
  Enum,
  TypeInfoBased,
};

enum class ScalarKind : uint8_t {
  POD,
  Immovable,
  ErrorReference,
  NativeStrongReference,
  NativeUnownedReference,
  NativeWeakReference,
  UnknownReference,
  UnknownUnownedReference,
  UnknownWeakReference,
  BlockReference,
  BridgeReference,
  ObjCReference,
  BlockStorage,
  ThickFunc,
  ExistentialReference,
};

class TypeLayoutEntry {
public:
  TypeLayoutEntryKind kind;
  uint8_t hasArchetypeField : 1;
  uint8_t hasResilientField : 1;
  uint8_t hasDependentResilientField : 1;

  TypeLayoutEntry()
      : kind(TypeLayoutEntryKind::Empty), hasArchetypeField(false),
        hasResilientField(false), hasDependentResilientField(false) {}

  TypeLayoutEntry(TypeLayoutEntryKind kind)
      : kind(kind), hasArchetypeField(false), hasResilientField(false),
        hasDependentResilientField(false) {}

  virtual ~TypeLayoutEntry();

  virtual void computeProperties();

  bool containsResilientField() const;
  bool containsArchetypeField() const;
  bool containsDependentResilientField() const;


  bool isEmpty() const { return kind == TypeLayoutEntryKind::Empty; }

  TypeLayoutEntryKind getKind() const { return kind; }

  virtual llvm::Value *alignmentMask(IRGenFunction &IGF) const;
  virtual llvm::Value *size(IRGenFunction &IGF) const;

  /// Return the size of the type if known statically
  virtual llvm::Optional<Size> fixedSize(IRGenModule& IGM) const;

  /// Return if the size of the type if known statically
  virtual bool isFixedSize(IRGenModule& IGM) const;

  /// Return the alignment of the type if known statically
  virtual llvm::Optional<Alignment> fixedAlignment(IRGenModule& IGM) const;

  /// Return the number of extra inhabitants if known statically
  virtual llvm::Optional<uint32_t> fixedXICount(IRGenModule& IGM) const;
  virtual llvm::Value *extraInhabitantCount(IRGenFunction &IGF) const;
  virtual llvm::Value *isBitwiseTakable(IRGenFunction &IGF) const;

  virtual void destroy(IRGenFunction &IGF, Address addr) const;

  void assign(IRGenFunction &IGF, Address dest, Address src,
              IsTake_t isTake) const;
  void initialize(IRGenFunction &IGF, Address dest, Address src,
                  IsTake_t isTake) const;

  virtual void assignWithCopy(IRGenFunction &IGF, Address dest,
                              Address src) const;
  virtual void assignWithTake(IRGenFunction &IGF, Address dest,
                              Address src) const;

  virtual void initWithCopy(IRGenFunction &IGF, Address dest,
                              Address src) const;
  virtual void initWithTake(IRGenFunction &IGF, Address dest,
                            Address src) const;

  /// Returns a pointer to the object (T*) inside of the buffer.
  virtual llvm::Value *initBufferWithCopyOfBuffer(IRGenFunction &IGF,
                                                  Address dest,
                                                  Address src) const;

  virtual llvm::Value *getEnumTagSinglePayload(IRGenFunction &IGF,
                                               llvm::Value *numEmptyCases,
                                               Address addr) const;

  virtual void storeEnumTagSinglePayload(IRGenFunction &IGF, llvm::Value *tag,
                                         llvm::Value *numEmptyCases,
                                         Address enumAddr) const;

  const EnumTypeLayoutEntry *getAsEnum() const;

#if !defined(NDEBUG) || defined(LLVM_ENABLE_DUMP)
  LLVM_DUMP_METHOD virtual void dump() const {
    assert(isEmpty() && "Missing subclass implementation?");
    llvm::dbgs() << "{empty}";
  }
#endif
protected:
  llvm::Value *
  getEnumTagSinglePayloadGeneric(IRGenFunction &IGF, Address addr,
                                 llvm::Value *numEmptyCases,
                                 llvm::function_ref<llvm::Value *(Address addr)>
                                     getExtraInhabitantIndexFun) const;

  void storeEnumTagSinglePayloadGeneric(
      IRGenFunction &IGF, llvm::Value *tag, llvm::Value *numEmptyCases,
      Address addr,
      llvm::function_ref<void(Address addr, llvm::Value *tag)>
          storeExtraInhabitantIndexFun) const;

  void gatherProperties(TypeLayoutEntry *fromEntry);
};

class ScalarTypeLayoutEntry : public TypeLayoutEntry,
                              public llvm::FoldingSetNode {
public:
  const FixedTypeInfo &typeInfo;
  SILType representative;

  ScalarKind scalarKind;

  ScalarTypeLayoutEntry(const FixedTypeInfo &ti, SILType representative,
                        ScalarKind scalarKind)
      : TypeLayoutEntry(TypeLayoutEntryKind::Scalar), typeInfo(ti),
        representative(representative), scalarKind(scalarKind) {}

  ~ScalarTypeLayoutEntry();

  void computeProperties() override;

  // Support for FoldingSet.
  void Profile(llvm::FoldingSetNodeID &id) const;
  static void Profile(llvm::FoldingSetNodeID &ID, const TypeInfo &ti,
                      SILType ty);

  llvm::Value *alignmentMask(IRGenFunction &IGF) const override;
  llvm::Value *size(IRGenFunction &IGF) const override;
  llvm::Optional<Size> fixedSize(IRGenModule& IGM) const override;
  bool isFixedSize(IRGenModule& IGM) const override;
  llvm::Optional<Alignment> fixedAlignment(IRGenModule& IGM) const override;
  llvm::Optional<uint32_t> fixedXICount(IRGenModule& IGM) const override;
  llvm::Value *extraInhabitantCount(IRGenFunction &IGF) const override;
  llvm::Value *isBitwiseTakable(IRGenFunction &IGF) const override;
  llvm::Type *getStorageType(IRGenFunction &IGF) const;

  void destroy(IRGenFunction &IGF, Address addr) const override;

  void assignWithCopy(IRGenFunction &IGF, Address dest,
                      Address src) const override;
  void assignWithTake(IRGenFunction &IGF, Address dest,
                      Address src) const override;

  void initWithCopy(IRGenFunction &IGF, Address dest,
                    Address src) const override;
  void initWithTake(IRGenFunction &IGF, Address dest,
                    Address src) const override;

  llvm::Value *getEnumTagSinglePayload(IRGenFunction &IGF,
                                       llvm::Value *numEmptyCases,
                                       Address addr) const override;

  void storeEnumTagSinglePayload(IRGenFunction &IGF, llvm::Value *tag,
                                 llvm::Value *numEmptyCases,
                                 Address enumAddr) const override;

  static bool classof(const TypeLayoutEntry *entry);

#if !defined(NDEBUG) || defined(LLVM_ENABLE_DUMP)
  void dump() const override;
#endif

};

class ArchetypeLayoutEntry : public TypeLayoutEntry,
                             public llvm::FoldingSetNode {
  SILType archetype;

public:
  ArchetypeLayoutEntry(SILType archetype)
      : TypeLayoutEntry(TypeLayoutEntryKind::Archetype), archetype(archetype) {}

  ~ArchetypeLayoutEntry();

  void computeProperties() override;

  // Support for FoldingSet.
  void Profile(llvm::FoldingSetNodeID &id) const;
  static void Profile(llvm::FoldingSetNodeID &ID, SILType archetype);

  llvm::Value *alignmentMask(IRGenFunction &IGF) const override;
  llvm::Value *size(IRGenFunction &IGF) const override;
  llvm::Optional<Size> fixedSize(IRGenModule& IGM) const override;
  bool isFixedSize(IRGenModule& IGM) const override;
  llvm::Optional<Alignment> fixedAlignment(IRGenModule& IGM) const override;
  llvm::Optional<uint32_t> fixedXICount(IRGenModule& IGM) const override;
  llvm::Value *extraInhabitantCount(IRGenFunction &IGF) const override;
  llvm::Value *isBitwiseTakable(IRGenFunction &IGF) const override;

  void destroy(IRGenFunction &IGF, Address addr) const override;

  void assignWithCopy(IRGenFunction &IGF, Address dest,
                      Address src) const override;
  void assignWithTake(IRGenFunction &IGF, Address dest,
                      Address src) const override;

  void initWithCopy(IRGenFunction &IGF, Address dest,
                    Address src) const override;
  void initWithTake(IRGenFunction &IGF, Address dest,
                    Address src) const override;

  llvm::Value *getEnumTagSinglePayload(IRGenFunction &IGF,
                                       llvm::Value *numEmptyCases,
                                       Address addr) const override;

  void storeEnumTagSinglePayload(IRGenFunction &IGF, llvm::Value *tag,
                                 llvm::Value *numEmptyCases,
                                 Address enumAddr) const override;

  static bool classof(const TypeLayoutEntry *entry);

#if !defined(NDEBUG) || defined(LLVM_ENABLE_DUMP)
  void dump() const override;
#endif
};

class ResilientTypeLayoutEntry : public TypeLayoutEntry,
                                 public llvm::FoldingSetNode {
  SILType ty;

public:
  ResilientTypeLayoutEntry(SILType ty)
      : TypeLayoutEntry(TypeLayoutEntryKind::Resilient), ty(ty) {}

  ~ResilientTypeLayoutEntry();

  void computeProperties() override;

  // Support for FoldingSet.
  void Profile(llvm::FoldingSetNodeID &id) const;
  static void Profile(llvm::FoldingSetNodeID &ID, SILType ty);

  llvm::Value *alignmentMask(IRGenFunction &IGF) const override;
  llvm::Value *size(IRGenFunction &IGF) const override;
  llvm::Optional<Size> fixedSize(IRGenModule& IGM) const override;
  bool isFixedSize(IRGenModule& IGM) const override;
  llvm::Optional<Alignment> fixedAlignment(IRGenModule& IGM) const override;
  llvm::Optional<uint32_t> fixedXICount(IRGenModule& IGM) const override;
  llvm::Value *extraInhabitantCount(IRGenFunction &IGF) const override;
  llvm::Value *isBitwiseTakable(IRGenFunction &IGF) const override;

  void destroy(IRGenFunction &IGF, Address addr) const override;

  void assignWithCopy(IRGenFunction &IGF, Address dest,
                      Address src) const override;
  void assignWithTake(IRGenFunction &IGF, Address dest,
                      Address src) const override;

  void initWithCopy(IRGenFunction &IGF, Address dest,
                    Address src) const override;
  void initWithTake(IRGenFunction &IGF, Address dest,
                    Address src) const override;

  llvm::Value *getEnumTagSinglePayload(IRGenFunction &IGF,
                                       llvm::Value *numEmptyCases,
                                       Address addr) const override;

  void storeEnumTagSinglePayload(IRGenFunction &IGF, llvm::Value *tag,
                                 llvm::Value *numEmptyCases,
                                 Address enumAddr) const override;

  static bool classof(const TypeLayoutEntry *entry);

#if !defined(NDEBUG) || defined(LLVM_ENABLE_DUMP)
  void dump() const override;
#endif
};

class AlignedGroupEntry : public TypeLayoutEntry, public llvm::FoldingSetNode {
  std::vector<TypeLayoutEntry *> entries;
  Alignment::int_type minimumAlignment;

public:
  AlignedGroupEntry(const std::vector<TypeLayoutEntry *> &entries,
                    Alignment::int_type minimumAlignment)
      : TypeLayoutEntry(TypeLayoutEntryKind::AlignedGroup), entries(entries),
        minimumAlignment(minimumAlignment) {}

  ~AlignedGroupEntry();

  void computeProperties() override;

  // Support for FoldingSet.
  void Profile(llvm::FoldingSetNodeID &id) const;
  static void Profile(llvm::FoldingSetNodeID &ID,
                      const std::vector<TypeLayoutEntry *> &entries,
                      Alignment::int_type minimumAlignment);

  llvm::Value *alignmentMask(IRGenFunction &IGF) const override;
  llvm::Value *size(IRGenFunction &IGF) const override;
  llvm::Optional<Size> fixedSize(IRGenModule& IGM) const override;
  bool isFixedSize(IRGenModule& IGM) const override;
  llvm::Optional<Alignment> fixedAlignment(IRGenModule& IGM) const override;
  llvm::Optional<uint32_t> fixedXICount(IRGenModule& IGM) const override;
  llvm::Value *extraInhabitantCount(IRGenFunction &IGF) const override;
  llvm::Value *isBitwiseTakable(IRGenFunction &IGF) const override;

  void destroy(IRGenFunction &IGF, Address addr) const override;

  void assignWithCopy(IRGenFunction &IGF, Address dest,
                      Address src) const override;
  void assignWithTake(IRGenFunction &IGF, Address dest,
                      Address src) const override;

  void initWithCopy(IRGenFunction &IGF, Address dest,
                    Address src) const override;
  void initWithTake(IRGenFunction &IGF, Address dest,
                    Address src) const override;

  llvm::Value *getEnumTagSinglePayload(IRGenFunction &IGF,
                                       llvm::Value *numEmptyCases,
                                       Address addr) const override;

  void storeEnumTagSinglePayload(IRGenFunction &IGF, llvm::Value *tag,
                                 llvm::Value *numEmptyCases,
                                 Address enumAddr) const override;

  static bool classof(const TypeLayoutEntry *entry);

#if !defined(NDEBUG) || defined(LLVM_ENABLE_DUMP)
  void dump() const override;
#endif

private:
  llvm::Value *withExtraInhabitantProvidingEntry(
      IRGenFunction &IGF, Address addr, llvm::Type *returnType,
      llvm::function_ref<llvm::Value *(TypeLayoutEntry *,
                                       Address /*entry addr*/,
                                       llvm::Value * /*entry xi count*/)>
          entryFun) const;
  void
  withEachEntry(IRGenFunction &IGF, Address dest, Address src,
                llvm::function_ref<void(TypeLayoutEntry *entry,
                                        Address entryDest, Address entrySrc)>
                    entryFun) const;
};

class EnumTypeLayoutEntry : public TypeLayoutEntry,
                            public llvm::FoldingSetNode {
public:
  unsigned numEmptyCases;
  unsigned minimumAlignment;
  std::vector<TypeLayoutEntry *> cases;

  EnumTypeLayoutEntry(unsigned numEmptyCases,
                      const std::vector<TypeLayoutEntry *> &cases)
      : TypeLayoutEntry(TypeLayoutEntryKind::Enum),
        numEmptyCases(numEmptyCases), minimumAlignment(1), cases(cases) {}

  ~EnumTypeLayoutEntry();

  void computeProperties() override;

  // Support for FoldingSet.
  void Profile(llvm::FoldingSetNodeID &id) const;
  static void Profile(llvm::FoldingSetNodeID &ID, unsigned numEmptyCases,
                      const std::vector<TypeLayoutEntry *> &cases);

  llvm::Value *alignmentMask(IRGenFunction &IGF) const override;
  llvm::Value *size(IRGenFunction &IGF) const override;
  llvm::Optional<Size> fixedSize(IRGenModule& IGM) const override;
  bool isFixedSize(IRGenModule& IGM) const override;
  llvm::Optional<Alignment> fixedAlignment(IRGenModule& IGM) const override;
  llvm::Optional<uint32_t> fixedXICount(IRGenModule& IGM) const override;
  llvm::Value *extraInhabitantCount(IRGenFunction &IGF) const override;
  llvm::Value *isBitwiseTakable(IRGenFunction &IGF) const override;

  void destroy(IRGenFunction &IGF, Address addr) const override;

  void assignWithCopy(IRGenFunction &IGF, Address dest,
                      Address src) const override;
  void assignWithTake(IRGenFunction &IGF, Address dest,
                      Address src) const override;

  void initWithCopy(IRGenFunction &IGF, Address dest,
                    Address src) const override;
  void initWithTake(IRGenFunction &IGF, Address dest,
                    Address src) const override;

  llvm::Value *getEnumTagSinglePayload(IRGenFunction &IGF,
                                       llvm::Value *numEmptyCases,
                                       Address addr) const override;

  void storeEnumTagSinglePayload(IRGenFunction &IGF, llvm::Value *tag,
                                 llvm::Value *numEmptyCases,
                                 Address enumAddr) const override;

  llvm::Value *getEnumTag(IRGenFunction &IGF, Address enumAddr) const;

  void destructiveProjectEnumData(IRGenFunction &IGF, Address enumAddr) const;

  void destructiveInjectEnumTag(IRGenFunction &IGF, llvm::Value *tag,
                                Address enumAddr) const;

  bool isMultiPayloadEnum() const;

#if !defined(NDEBUG) || defined(LLVM_ENABLE_DUMP)
  void dump() const override;
#endif

private:
  llvm::Value *maxPayloadSize(IRGenFunction &IGF) const;
  llvm::BasicBlock *testSinglePayloadEnumContainsPayload(IRGenFunction &IGF,
                                                         Address addr) const;

  void initializeSinglePayloadEnum(IRGenFunction &IGF, Address dest,
                                   Address src, IsTake_t isTake) const;
  void assignSinglePayloadEnum(IRGenFunction &IGF, Address dest, Address src,
                               IsTake_t isTake) const;

  void initializeMultiPayloadEnum(IRGenFunction &IGF, Address dest,
                                   Address src, IsTake_t isTake) const;
  void assignMultiPayloadEnum(IRGenFunction &IGF, Address dest, Address src,
                              IsTake_t isTake) const;

  std::pair<Address, llvm::Value *>
  getMultiPalyloadEnumTagByteAddrAndNumBytes(IRGenFunction &IGF,
                                             Address addr) const;

  llvm::Value *
  getEnumTagSinglePayloadForSinglePayloadEnum(IRGenFunction &IGF, Address addr,
                                              llvm::Value *numEmptyCases) const;
  void storeEnumTagSinglePayloadForSinglePayloadEnum(IRGenFunction &IGF,
                                                     llvm::Value *tag,
                                                     llvm::Value *numEmptyCases,
                                                     Address enumAddr) const;
  llvm::Value *
  getEnumTagSinglePayloadForMultiPayloadEnum(IRGenFunction &IGF, Address addr,
                                             llvm::Value *numEmptyCases) const;
  void storeEnumTagSinglePayloadForMultiPayloadEnum(IRGenFunction &IGF,
                                                    llvm::Value *tag,
                                                    llvm::Value *numEmptyCases,
                                                    Address enumAddr) const;
  llvm::Value *getEnumTagMultipayload(IRGenFunction &IGF,
                                      Address enumAddr) const;

  void storeEnumTagMultipayload(IRGenFunction &IGF, llvm::Value *tag,
                                Address enumAddr) const;

  /// Store a value to the enum's tag bytes.
  void storeMultiPayloadTag(IRGenFunction &IGF, llvm::Value *value,
                            Address enumAddr) const;
  /// Store a value to the enum's payload bytes.
  void storeMultiPayloadValue(IRGenFunction &IGF, llvm::Value *value,
                              Address enumAddr) const;

  void destroyMultiPayloadEnum(IRGenFunction &IGF, Address enumAddr) const;
  void destroySinglePayloadEnum(IRGenFunction &IGF, Address enumAddr) const;

  void multiPayloadEnumForPayloadAndEmptyCases(
      IRGenFunction &IGF, Address addr,
      llvm::function_ref<void(TypeLayoutEntry *payload, llvm::Value *tagIndex)>
          payloadFunction,
      llvm::function_ref<void()> noPayloadFunction) const;

  static bool classof(const TypeLayoutEntry *entry);
};

/// TypeLayouts that defer to the existing typeinfo infrastructure in cases that
/// type layouts don't have the functionality implemented yet (e.g. multi enum
/// extra inhabitants).
class TypeInfoBasedTypeLayoutEntry : public TypeLayoutEntry,
                                     public llvm::FoldingSetNode {
public:
  const FixedTypeInfo &typeInfo;
  SILType representative;

  TypeInfoBasedTypeLayoutEntry(const FixedTypeInfo &ti, SILType representative)
      : TypeLayoutEntry(TypeLayoutEntryKind::TypeInfoBased), typeInfo(ti),
        representative(representative) {}

  ~TypeInfoBasedTypeLayoutEntry();

  void computeProperties() override;

  // Support for FoldingSet.
  void Profile(llvm::FoldingSetNodeID &id) const;
  static void Profile(llvm::FoldingSetNodeID &ID, const TypeInfo &ti,
                      SILType ty);

  llvm::Value *alignmentMask(IRGenFunction &IGF) const override;
  llvm::Value *size(IRGenFunction &IGF) const override;
  llvm::Value *extraInhabitantCount(IRGenFunction &IGF) const override;
  llvm::Optional<Size> fixedSize(IRGenModule& IGM) const override;
  bool isFixedSize(IRGenModule& IGM) const override;
  llvm::Optional<Alignment> fixedAlignment(IRGenModule& IGM) const override;
  llvm::Optional<uint32_t> fixedXICount(IRGenModule& IGM) const override;
  llvm::Value *isBitwiseTakable(IRGenFunction &IGF) const override;
  llvm::Type *getStorageType(IRGenFunction &IGF) const;

  void destroy(IRGenFunction &IGF, Address addr) const override;

  void assignWithCopy(IRGenFunction &IGF, Address dest,
                      Address src) const override;
  void assignWithTake(IRGenFunction &IGF, Address dest,
                      Address src) const override;

  void initWithCopy(IRGenFunction &IGF, Address dest,
                    Address src) const override;
  void initWithTake(IRGenFunction &IGF, Address dest,
                    Address src) const override;

  llvm::Value *getEnumTagSinglePayload(IRGenFunction &IGF,
                                       llvm::Value *numEmptyCases,
                                       Address addr) const override;

  void storeEnumTagSinglePayload(IRGenFunction &IGF, llvm::Value *tag,
                                 llvm::Value *numEmptyCases,
                                 Address enumAddr) const override;

#if !defined(NDEBUG) || defined(LLVM_ENABLE_DUMP)
  void dump() const override;
#endif
};

class TypeLayoutCache {
  llvm::BumpPtrAllocator bumpAllocator;

  llvm::FoldingSet<ScalarTypeLayoutEntry> scalarEntries;
  llvm::FoldingSet<ArchetypeLayoutEntry> archetypeEntries;
  llvm::FoldingSet<AlignedGroupEntry> alignedGroupEntries;
  llvm::FoldingSet<EnumTypeLayoutEntry> enumEntries;
  llvm::FoldingSet<ResilientTypeLayoutEntry> resilientEntries;
  llvm::FoldingSet<TypeInfoBasedTypeLayoutEntry> typeInfoBasedEntries;

  TypeLayoutEntry emptyEntry;
public:
  ~TypeLayoutCache();
  ScalarTypeLayoutEntry *getOrCreateScalarEntry(const TypeInfo &ti,
                                                SILType representative,
                                                ScalarKind kind);

  ArchetypeLayoutEntry *getOrCreateArchetypeEntry(SILType archetype);

  AlignedGroupEntry *
  getOrCreateAlignedGroupEntry(const std::vector<TypeLayoutEntry *> &entries,
                               Alignment::int_type minimumAlignment);

  EnumTypeLayoutEntry *
  getOrCreateEnumEntry(unsigned numEmptyCase,
                       const std::vector<TypeLayoutEntry *> &nonEmptyCases);

  TypeInfoBasedTypeLayoutEntry *
  getOrCreateTypeInfoBasedEntry(const TypeInfo &ti, SILType representative);

  ResilientTypeLayoutEntry *getOrCreateResilientEntry(SILType ty);

  TypeLayoutEntry *getEmptyEntry();
};

} // namespace irgen
} // namespace swift
#endif
