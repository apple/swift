//===--- GenEnum.h - Swift IR Generation For 'enum' Types -------*- C++ -*-===//
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

#ifndef SWIFT_IRGEN_GENENUM_H
#define SWIFT_IRGEN_GENENUM_H

#include "TypeInfo.h"
#include "LoadableTypeInfo.h"

namespace llvm {
  class BasicBlock;
  class ConstantInt;
  class StructType;
  class Type;
  class Value;
}

namespace clang {
namespace CodeGen {
namespace swiftcall {
  class SwiftAggLowering;
}
}
}

namespace swift {
namespace irgen {
  class IRBuilder;
}
}

namespace swift {
  class EnumElementDecl;
  enum IsInitialization_t : bool;
  enum IsTake_t : bool;

namespace irgen {
  class EnumPayload;
  class EnumPayloadSchema;
  class IRGenFunction;
  class MetadataDependencyCollector;
  class TypeConverter;
  using clang::CodeGen::swiftcall::SwiftAggLowering;

/// Emit the dispatch branch(es) for an address-only enum.
void emitSwitchAddressOnlyEnumDispatch(IRGenFunction &IGF,
                                        SILType enumTy,
                                        Address enumAddr,
                                        ArrayRef<std::pair<EnumElementDecl*,
                                                           llvm::BasicBlock*>> dests,
                                        llvm::BasicBlock *defaultDest);

/// Injects a case and its associated data, if any, into a loadable enum
/// value.
void emitInjectLoadableEnum(IRGenFunction &IGF,
                             SILType enumTy,
                             EnumElementDecl *theCase,
                             Explosion &data,
                             Explosion &out);
  
/// Extracts the associated data for an enum case. This is an unchecked
/// operation; the input enum value must be of the given case.
void emitProjectLoadableEnum(IRGenFunction &IGF,
                              SILType enumTy,
                              Explosion &inData,
                              EnumElementDecl *theCase,
                              Explosion &out);

/// Projects the address of the associated data for a case inside a
/// enum, to which a new data value can be stored.
Address emitProjectEnumAddressForStore(IRGenFunction &IGF,
                                        SILType enumTy,
                                        Address enumAddr,
                                        EnumElementDecl *theCase);

/// Projects the address of the associated data for a case inside a
/// enum, clearing any tag bits interleaved into the data area, so that the
/// value inside can be loaded. Does not check that the enum has a value of the
/// given case.
Address emitDestructiveProjectEnumAddressForLoad(IRGenFunction &IGF,
                                                  SILType enumTy,
                                                  Address enumAddr,
                                                  EnumElementDecl *theCase);

/// Stores the tag bits for an enum case to the given address, overlaying
/// the data (if any) stored there.
void emitStoreEnumTagToAddress(IRGenFunction &IGF,
                                SILType enumTy,
                                Address enumAddr,
                                EnumElementDecl *theCase);

/// Pack masked bits into the low bits of an integer value.
/// Equivalent to a parallel bit extract instruction (PEXT),
/// although we don't currently emit PEXT directly.
llvm::Value *emitGatherBits(IRGenFunction &IGF,
                            llvm::APInt mask,
                            llvm::Value *source,
                            unsigned resultLowBit,
                            unsigned resultBitWidth);

/// Pack masked bits into the low bits of an integer value.
llvm::APInt gatherBits(const llvm::APInt &mask,
                       const llvm::APInt &value);

/// Unpack bits from the low bits of an integer value and
/// move them to the bit positions indicated by the mask.
/// Equivalent to a parallel bit deposit instruction (PDEP),
/// although we don't currently emit PDEP directly.
llvm::Value *emitScatterBits(IRGenModule &IGM,
                             IRBuilder &builder,
                             llvm::APInt mask,
                             llvm::Value *packedBits,
                             unsigned packedLowBit);

/// Unpack bits from the low bits of an integer value and
/// move them to the bit positions indicated by the mask.
llvm::APInt scatterBits(const llvm::APInt &mask, unsigned value);

/// An implementation strategy for an enum, which handles how the enum is
/// laid out and how to perform TypeInfo operations on values of the enum.
class EnumImplStrategy {
public:
  struct Element {
    EnumElementDecl *decl;
    const TypeInfo *ti;
    const TypeInfo *origTI;
  };
  
  enum TypeInfoKind {
    Opaque,   ///< The enum has a NonFixedTypeInfo.
    Fixed,    ///< The enum has a FixedTypeInfo.
    Loadable, ///< The enum has a LoadableTypeInfo.
  };
  
protected:
  std::vector<Element> ElementsWithPayload;
  std::vector<Element> ElementsWithNoPayload;
  IRGenModule &IGM;
  const TypeInfo *TI = nullptr;
  TypeInfoKind TIK;
  IsFixedSize_t AlwaysFixedSize;
  IsABIAccessible_t ElementsAreABIAccessible;
  unsigned NumElements;
  
  EnumImplStrategy(IRGenModule &IGM,
                   TypeInfoKind tik,
                   IsFixedSize_t alwaysFixedSize,
                   unsigned NumElements,
                   std::vector<Element> &&ElementsWithPayload,
                   std::vector<Element> &&ElementsWithNoPayload);
  
  /// Save the TypeInfo created for the enum.
  TypeInfo *registerEnumTypeInfo(TypeInfo *mutableTI) {
    TI = mutableTI;
    return mutableTI;
  }
  
  /// Constructs a TypeInfo for an enum of the best possible kind for its
  /// layout, FixedEnumTypeInfo or LoadableEnumTypeInfo.
  TypeInfo *getFixedEnumTypeInfo(llvm::StructType *T, Size S, SpareBitVector SB,
                                 Alignment A,
                                 IsTriviallyDestroyable_t isTriviallyDestroyable,
                                 IsBitwiseTakable_t isBT,
                                 IsCopyable_t isCopyable);
  
public:
  virtual ~EnumImplStrategy() { }
  
  /// Construct a layout strategy appropriate to the enum type.
  static std::unique_ptr<EnumImplStrategy> get(TypeConverter &TC,
                                               SILType Type,
                                               EnumDecl *theEnum);
  
  /// Given an incomplete StructType for the enum, completes layout of the
  /// storage type, calculates its size and alignment, and produces the
  /// TypeInfo for the enum.
  virtual TypeInfo *completeEnumTypeLayout(TypeConverter &TC,
                                           SILType Type,
                                           EnumDecl *theEnum,
                                           llvm::StructType *enumTy) = 0;

  const TypeInfo &getTypeInfo() const {
    assert(TI);
    return *TI;
  }
  
  llvm::StructType *getStorageType() const {
    return cast<llvm::StructType>(getTypeInfo().getStorageType());
  }
  
  IsTriviallyDestroyable_t isTriviallyDestroyable(ResilienceExpansion expansion) const {
    return getTypeInfo().isTriviallyDestroyable(expansion);
  }
  
  /// \group Query enum layout
  ///
  /// These APIs assume a fixed layout; they will not work on generic
  /// enum types that have not been fully instantiated.
  
  /// Return the list of cases determined to have a payload.
  /// The payloads are discriminated using the bits in the mask returned by
  /// getTagBits().
  ArrayRef<Element> getElementsWithPayload() const {
    return ElementsWithPayload;
  }
  
  /// Return the list of cases determined to have no payload.
  /// Each no-payload case is represented by a specific bit pattern which can
  /// be queried using getBitPatternForNoPayloadElement.
  ArrayRef<Element> getElementsWithNoPayload() const {
    return ElementsWithNoPayload;
  }

  /// Return a tag index in the range [0..NumElements].
  unsigned getTagIndex(EnumElementDecl *Case) const;

  /// Map the given element to the appropriate index in the
  /// discriminator type.
  /// Returns -1 if this is not supported by the enum implementation.
  virtual int64_t getDiscriminatorIndex(EnumElementDecl *target) const {
    return -1;
  }

  /// Emit field names for enum reflection.
  virtual bool isReflectable() const;

  /// Return the bits used for discriminators for payload cases.
  ///
  /// These bits are populated in increasing value according to the order of
  /// the getElementsWithPayload() array, starting from zero for the first
  /// element with payload.
  virtual ClusteredBitVector getTagBitsForPayloads() const = 0;
  
  /// Return the bit pattern used for the given no-payload case.
  virtual ClusteredBitVector
  getBitPatternForNoPayloadElement(EnumElementDecl *theCase) const = 0;

  /// Return the bit mask used to test for no-payload cases.
  virtual ClusteredBitVector
  getBitMaskForNoPayloadElements() const = 0;

  /// \group Indirect enum operations
  
  /// Return the enum case tag for the given value. Payload cases come first,
  /// followed by non-payload cases. Used for the getEnumTag value witness.
  virtual llvm::Value *emitGetEnumTag(IRGenFunction &IGF,
                                      SILType T,
                                      Address enumAddr) const = 0;

  /// Project the address of the data for a case. Does not check or modify
  /// the referenced enum value.
  /// Corresponds to the SIL 'init_enum_data_addr' instruction.
  Address projectDataForStore(IRGenFunction &IGF,
                              EnumElementDecl *elt,
                              Address enumAddr) const;

  /// Overlay the tag value for a case onto a data value in memory.
  /// Corresponds to the SIL 'inject_enum_addr' instruction.
  virtual void storeTag(IRGenFunction &IGF,
                        SILType T,
                        Address enumAddr,
                        EnumElementDecl *elt) const = 0;
  
  /// Overlay a dynamic case tag onto a data value in memory. Used when
  /// generating the destructiveInjectEnumTag value witness.
  virtual void emitStoreTag(IRGenFunction &IGF,
                            SILType T,
                            Address enumAddr,
                            llvm::Value *tag) const = 0;
  
  /// Clears tag bits from within the payload of an enum in memory and
  /// projects the address of the data for a case. Does not check
  /// the referenced enum value.
  /// Performs the block argument binding for a SIL
  /// 'switch_enum_addr' instruction.
  /// Also used when generating the destructiveProjectEnumData value
  /// witness.
  Address destructiveProjectDataForLoad(IRGenFunction &IGF,
                                        SILType T,
                                        Address enumAddr,
                                        EnumElementDecl *Case) const;

  /// Clears tag bits from within the payload of an enum in memory and
  /// projects the address of the data for a case. Does not check
  /// the referenced enum value.
  virtual void destructiveProjectDataForLoad(IRGenFunction &IGF,
                                             SILType T,
                                             Address enumAddr) const = 0;

  /// Return an i1 value that indicates whether the specified indirect enum
  /// value holds the specified case.  This is a light-weight form of a switch.
  virtual llvm::Value *emitIndirectCaseTest(IRGenFunction &IGF,
                                            SILType T,
                                            Address enumAddr,
                                            EnumElementDecl *Case) const = 0;
  
  /// Emit a branch on the case contained by an enum explosion.
  /// Performs the branching for a SIL 'switch_enum_addr'
  /// instruction.
  virtual void emitIndirectSwitch(IRGenFunction &IGF,
                                  SILType T,
                                  Address enumAddr,
                                  ArrayRef<std::pair<EnumElementDecl*,
                                                     llvm::BasicBlock*>> dests,
                                  llvm::BasicBlock *defaultDest) const = 0;

  /// Emit code to extract the discriminator as an integer value.
  /// Returns null if this is not supported by the enum implementation.
  ///
  /// FIXME: is this the same as emitGetEnumTag()? Seems like it could be,
  /// except that CCompatibleEnumImplStrategy maps cases to their raw
  /// values rather than indices.
  virtual llvm::Value *emitExtractDiscriminator(IRGenFunction &IGF,
                                                Explosion &value) const {
    return nullptr;
  }

  /// \group Loadable enum operations
  
  /// Emit the construction sequence for an enum case into an explosion.
  /// Corresponds to the SIL 'enum' instruction.
  virtual void emitValueInjection(IRGenModule &IGM,
                                  IRBuilder &builder,
                                  EnumElementDecl *elt,
                                  Explosion &params,
                                  Explosion &out) const = 0;
  
  /// Return an i1 value that indicates whether the specified loadable enum
  /// value holds the specified case.  This is a light-weight form of a switch.
  virtual llvm::Value *emitValueCaseTest(IRGenFunction &IGF,
                                         Explosion &value,
                                         EnumElementDecl *Case) const = 0;

  /// Emit a branch on the case contained by an enum explosion.
  /// Performs the branching for a SIL 'switch_enum' instruction.
  virtual void emitValueSwitch(IRGenFunction &IGF,
                               Explosion &value,
                               ArrayRef<std::pair<EnumElementDecl*,
                                                  llvm::BasicBlock*>> dests,
                               llvm::BasicBlock *defaultDest) const = 0;
  
  /// Project a case value out of an enum explosion. This does not check that
  /// the explosion actually contains a value of the given case.
  /// Performs the block argument binding for a SIL 'switch_enum'
  /// instruction.
  virtual void emitValueProject(IRGenFunction &IGF,
                                Explosion &inEnum,
                                EnumElementDecl *theCase,
                                Explosion &out) const = 0;
  
  /// \group Delegated TypeInfo operations
  
  virtual void addToAggLowering(IRGenModule &IGM, SwiftAggLowering &lowering,
                                Size offset) const = 0;
  virtual void getSchema(ExplosionSchema &schema) const = 0;
  virtual void destroy(IRGenFunction &IGF, Address addr, SILType T,
                       bool isOutlined) const = 0;

  virtual void initializeFromParams(IRGenFunction &IGF, Explosion &params,
                                    Address dest, SILType T,
                                    bool isOutlined) const;

  virtual void assignWithCopy(IRGenFunction &IGF, Address dest, Address src,
                              SILType T, bool isOutlined) const = 0;
  virtual void assignWithTake(IRGenFunction &IGF, Address dest, Address src,
                              SILType T, bool isOutlined) const = 0;
  virtual void initializeWithCopy(IRGenFunction &IGF, Address dest, Address src,
                                  SILType T, bool isOutlined) const = 0;
  virtual void initializeWithTake(IRGenFunction &IGF, Address dest, Address src,
                                  SILType T, bool isOutlined) const = 0;

  virtual void initializeMetadata(IRGenFunction &IGF,
                                  llvm::Value *metadata,
                                  bool isVWTMutable,
                                  SILType T,
                              MetadataDependencyCollector *collector) const = 0;

  virtual bool mayHaveExtraInhabitants(IRGenModule &IGM) const = 0;

  // Only ever called for fixed types.
  virtual llvm::Value *getExtraInhabitantIndex(IRGenFunction &IGF,
                                               Address src,
                                               SILType T,
                                               bool isOutlined) const = 0;
  // Only ever called for fixed types.
  virtual void storeExtraInhabitant(IRGenFunction &IGF,
                                    llvm::Value *index,
                                    Address dest,
                                    SILType T,
                                    bool isOutlined) const = 0;
  virtual llvm::Value *getEnumTagSinglePayload(IRGenFunction &IGF,
                                               llvm::Value *numEmptyCases,
                                               Address enumAddr,
                                               SILType T,
                                               bool isOutlined) const = 0;
  virtual void storeEnumTagSinglePayload(IRGenFunction &IGF,
                                         llvm::Value *whichCase,
                                         llvm::Value *numEmptyCases,
                                         Address enumAddr,
                                         SILType T,
                                         bool isOutlined) const = 0;
  
  /// \group Delegated FixedTypeInfo operations
  
  virtual unsigned getFixedExtraInhabitantCount(IRGenModule &IGM) const = 0;
  
  virtual APInt
  getFixedExtraInhabitantValue(IRGenModule &IGM,
                               unsigned bits,
                               unsigned index) const = 0;
  
  virtual APInt
  getFixedExtraInhabitantMask(IRGenModule &IGM) const = 0;
  
  /// \group Delegated LoadableTypeInfo operations
  
  virtual unsigned getExplosionSize() const = 0;
  virtual void loadAsCopy(IRGenFunction &IGF, Address addr,
                          Explosion &e) const = 0;
  virtual void loadAsTake(IRGenFunction &IGF, Address addr,
                          Explosion &e) const = 0;
  virtual void assign(IRGenFunction &IGF, Explosion &e, Address addr,
                      bool isOutlined, SILType T) const = 0;
  virtual void initialize(IRGenFunction &IGF, Explosion &e, Address addr,
                          bool isOutlined) const = 0;
  virtual void reexplode(Explosion &src,
                         Explosion &dest) const = 0;
  virtual void copy(IRGenFunction &IGF, Explosion &src,
                    Explosion &dest, Atomicity atomicity) const = 0;
  virtual void consume(IRGenFunction &IGF, Explosion &src,
                       Atomicity atomicity, SILType T) const = 0;
  virtual void fixLifetime(IRGenFunction &IGF, Explosion &src) const = 0;
  virtual void packIntoEnumPayload(IRGenModule &IGM,
                                   IRBuilder &builder,
                                   EnumPayload &payload,
                                   Explosion &in,
                                   unsigned offset) const = 0;
  
  virtual void unpackFromEnumPayload(IRGenFunction &IGF,
                                     const EnumPayload &payload,
                                     Explosion &dest,
                                     unsigned offset) const = 0;
  
  virtual bool needsPayloadSizeInMetadata() const = 0;
  virtual unsigned getPayloadSizeForMetadata() const;
  
  virtual LoadedRef loadRefcountedPtr(IRGenFunction &IGF, SourceLoc loc,
                                      Address addr) const;

  void callOutlinedCopy(IRGenFunction &IGF, Address dest, Address src,
                        SILType T, IsInitialization_t isInit,
                        IsTake_t isTake) const {
    TI->callOutlinedCopy(IGF, dest, src, T, isInit, isTake);
  }

  void callOutlinedDestroy(IRGenFunction &IGF, Address addr, SILType T) const {
    TI->callOutlinedDestroy(IGF, addr, T);
  }

  virtual void collectMetadataForOutlining(OutliningMetadataCollector &collector,
                                           SILType T) const = 0;

  virtual TypeLayoutEntry
  *buildTypeLayoutEntry(IRGenModule &IGM,
                        SILType T,
                        bool useStructLayouts) const = 0;

  virtual bool isSingleRetainablePointer(ResilienceExpansion expansion,
                                         ReferenceCounting *rc) const {
    return false;
  }

  void emitResilientTagIndices(IRGenModule &IGM) const;

  virtual bool canValueWitnessExtraInhabitantsUpTo(IRGenModule &IGM,
                                                   unsigned index) const {
    return false;
  }

private:
  EnumImplStrategy(const EnumImplStrategy &) = delete;
  EnumImplStrategy &operator=(const EnumImplStrategy &) = delete;
};
  
/// Get the EnumImplStrategy for an enum type.
const EnumImplStrategy &getEnumImplStrategy(IRGenModule &IGM, CanType ty);
const EnumImplStrategy &getEnumImplStrategy(IRGenModule &IGM, SILType ty);

}
}

#endif
