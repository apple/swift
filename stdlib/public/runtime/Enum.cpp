//===--- Enum.cpp - Runtime declarations for enums ------------------------===//
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
// Swift runtime functions in support of enums.
//
//===----------------------------------------------------------------------===//

#include "llvm/Support/ErrorHandling.h"
#include "llvm/Support/MathExtras.h"
#include "swift/Runtime/Metadata.h"
#include "swift/Runtime/Enum.h"
#include "swift/Runtime/Debug.h"
#include "Private.h"
#include "EnumImpl.h"
#include <cstring>
#include <algorithm>

using namespace swift;

static EnumValueWitnessTable *getMutableVWTableForInit(EnumMetadata *self,
                                                       EnumLayoutFlags flags) {
  auto oldTable =
    static_cast<const EnumValueWitnessTable *>(self->getValueWitnesses());

  // If we can alter the existing table in-place, do so.
  if (isValueWitnessTableMutable(flags))
    return const_cast<EnumValueWitnessTable*>(oldTable);

  // Otherwise, allocate permanent memory for it and copy the existing table.
  void *memory = allocateMetadata(sizeof(EnumValueWitnessTable),
                                  alignof(EnumValueWitnessTable));
  auto newTable = new (memory) EnumValueWitnessTable(*oldTable);
  self->setValueWitnesses(newTable);

  return newTable;
}

void
swift::swift_initEnumMetadataSingleCase(EnumMetadata *self,
                                        EnumLayoutFlags layoutFlags,
                                        const TypeLayout *payloadLayout) {
  auto vwtable = getMutableVWTableForInit(self, layoutFlags);

  TypeLayout layout;
  layout.size = payloadLayout->size;
  layout.stride = payloadLayout->stride;
  layout.flags = payloadLayout->flags.withEnumWitnesses(true);
  layout.extraInhabitantCount = payloadLayout->getNumExtraInhabitants();

  vwtable->publishLayout(layout);
}

void
swift::swift_initEnumMetadataSinglePayload(EnumMetadata *self,
                                           EnumLayoutFlags layoutFlags,
                                           const TypeLayout *payloadLayout,
                                           unsigned emptyCases) {
  size_t payloadSize = payloadLayout->size;
  unsigned payloadNumExtraInhabitants
    = payloadLayout->getNumExtraInhabitants();
  
  unsigned unusedExtraInhabitants = 0;
  
  // If there are enough extra inhabitants for all of the cases, then the size
  // of the enum is the same as its payload.
  size_t size;
  if (payloadNumExtraInhabitants >= emptyCases) {
    size = payloadSize;
    unusedExtraInhabitants = payloadNumExtraInhabitants - emptyCases;
  } else {
    size = payloadSize + getEnumTagCounts(payloadSize,
                                      emptyCases - payloadNumExtraInhabitants,
                                        1 /*payload case*/).numTagBytes; ;
  }

  auto vwtable = getMutableVWTableForInit(self, layoutFlags);
  
  size_t align = payloadLayout->flags.getAlignment();
  bool isBT = payloadLayout->flags.isBitwiseTakable();
  TypeLayout layout;
  layout.size = size;
  layout.flags =
      payloadLayout->flags
          .withEnumWitnesses(true)
          .withInlineStorage(
              ValueWitnessTable::isValueInline(isBT, size, align));
  layout.extraInhabitantCount = unusedExtraInhabitants;
  auto rawStride = llvm::alignTo(size, align);
  layout.stride = rawStride == 0 ? 1 : rawStride;
  
  // Substitute in better common value witnesses if we have them.
  // If the payload type is a single-refcounted pointer, and the enum has
  // a single empty case, then we can borrow the witnesses of the single
  // refcounted pointer type, since swift_retain and objc_retain are both
  // nil-aware. Most single-refcounted types will use the standard
  // value witness tables for NativeObject or AnyObject. This isn't
  // foolproof but should catch the common case of optional class types.
#if OPTIONAL_OBJECT_OPTIMIZATION
  auto payloadVWT = payload->getValueWitnesses();
  if (emptyCases == 1
      && (payloadVWT == &VALUE_WITNESS_SYM(Bo)
#if SWIFT_OBJC_INTEROP
          || payloadVWT == &VALUE_WITNESS_SYM(BO)
#endif
          )) {
#define WANT_ONLY_REQUIRED_VALUE_WITNESSES
#define VALUE_WITNESS(LOWER_ID, UPPER_ID) \
    vwtable->LOWER_ID = payloadVWT->LOWER_ID;
#define DATA_VALUE_WITNESS(LOWER_ID, UPPER_ID, TYPE)
#include "swift/ABI/ValueWitness.def"
  } else {
#endif
    installCommonValueWitnesses(layout, vwtable);
#if OPTIONAL_OBJECT_OPTIMIZATION
  }
#endif

  vwtable->publishLayout(layout);
}

unsigned
swift::swift_getEnumTagSinglePayloadGeneric(const OpaqueValue *value,
                                            unsigned emptyCases,
                                            const Metadata *payloadType,
                               getExtraInhabitantTag_t *getExtraInhabitantTag) {
  auto size = payloadType->vw_size();
  auto numExtraInhabitants = payloadType->vw_getNumExtraInhabitants();
  return getEnumTagSinglePayloadImpl(value, emptyCases, payloadType, size,
                                     numExtraInhabitants,
                                     getExtraInhabitantTag);
}

void swift::swift_storeEnumTagSinglePayloadGeneric(OpaqueValue *value,
                                                   unsigned whichCase,
                                                   unsigned emptyCases,
                                                   const Metadata *payloadType,
                           storeExtraInhabitantTag_t *storeExtraInhabitantTag) {
  auto size = payloadType->vw_size();
  auto numExtraInhabitants = payloadType->vw_getNumExtraInhabitants();
  storeEnumTagSinglePayloadImpl(value, whichCase, emptyCases, payloadType, size,
                                numExtraInhabitants, storeExtraInhabitantTag);
}

static uint32_t getMultiPayloadEnumTagSinglePayload(const OpaqueValue *value,
                                                    uint32_t numExtraCases,
                                                    const Metadata *enumType);
static void storeMultiPayloadEnumTagSinglePayload(OpaqueValue *value,
                                                  uint32_t index,
                                                  uint32_t numExtraCases,
                                                  const Metadata *enumType);

void
swift::swift_initEnumMetadataMultiPayload(EnumMetadata *enumType,
                                     EnumLayoutFlags layoutFlags,
                                     unsigned numPayloads,
                                     const TypeLayout * const *payloadLayouts) {
  // Accumulate the layout requirements of the payloads.
  size_t payloadSize = 0, alignMask = 0;
  bool isPOD = true, isBT = true;
  for (unsigned i = 0; i < numPayloads; ++i) {
    const TypeLayout *payloadLayout = payloadLayouts[i];
    payloadSize
      = std::max(payloadSize, (size_t)payloadLayout->size);
    alignMask |= payloadLayout->flags.getAlignmentMask();
    isPOD &= payloadLayout->flags.isPOD();
    isBT &= payloadLayout->flags.isBitwiseTakable();
  }
  
  // Store the max payload size in the metadata.
  assignUnlessEqual(enumType->getPayloadSize(), payloadSize);
  
  // The total size includes space for the tag.
  auto tagCounts = getEnumTagCounts(payloadSize,
                                enumType->getDescription()->getNumEmptyCases(),
                                numPayloads);
  unsigned totalSize = payloadSize + tagCounts.numTagBytes;
  
  // See whether there are extra inhabitants in the tag.
  unsigned numExtraInhabitants = tagCounts.numTagBytes == 4
    ? INT_MAX
    : (1 << (tagCounts.numTagBytes * 8)) - tagCounts.numTags;
  numExtraInhabitants = std::min(numExtraInhabitants,
                          unsigned(ValueWitnessFlags::MaxNumExtraInhabitants));

  auto vwtable = getMutableVWTableForInit(enumType, layoutFlags);

  // Set up the layout info in the vwtable.
  auto rawStride = (totalSize + alignMask) & ~alignMask;
  TypeLayout layout{totalSize,
                    rawStride == 0 ? 1 : rawStride,
                    ValueWitnessFlags()
                     .withAlignmentMask(alignMask)
                     .withPOD(isPOD)
                     .withBitwiseTakable(isBT)
                     .withEnumWitnesses(true)
                     .withInlineStorage(ValueWitnessTable::isValueInline(
                         isBT, totalSize, alignMask + 1)),
                    numExtraInhabitants};

  installCommonValueWitnesses(layout, vwtable);

  // Unconditionally overwrite the enum-tag witnesses.
  // The compiler does not generate meaningful enum-tag witnesses for
  // enums in this state.
  vwtable->getEnumTagSinglePayload = getMultiPayloadEnumTagSinglePayload;
  vwtable->storeEnumTagSinglePayload = storeMultiPayloadEnumTagSinglePayload;

  vwtable->publishLayout(layout);
}

namespace {
struct MultiPayloadLayout {
  size_t payloadSize;
  size_t numTagBytes;
};
} // end anonymous namespace

static MultiPayloadLayout getMultiPayloadLayout(const EnumMetadata *enumType) {
  size_t payloadSize = enumType->getPayloadSize();
  size_t totalSize = enumType->getValueWitnesses()->size;
  return {payloadSize, totalSize - payloadSize};
}

static void storeMultiPayloadTag(OpaqueValue *value,
                                 MultiPayloadLayout layout,
                                 unsigned tag) {
  auto tagBytes = reinterpret_cast<uint8_t *>(value) + layout.payloadSize;
  storeEnumElement(tagBytes, tag, layout.numTagBytes);
}

static void storeMultiPayloadValue(OpaqueValue *value,
                                   MultiPayloadLayout layout,
                                   unsigned payloadValue) {
  auto bytes = reinterpret_cast<uint8_t *>(value);
  storeEnumElement(bytes, payloadValue, layout.payloadSize);
}

static unsigned loadMultiPayloadTag(const OpaqueValue *value,
                                    MultiPayloadLayout layout,
                                    unsigned baseValue = 0) {
  auto tagBytes = reinterpret_cast<const uint8_t *>(value) +
                  layout.payloadSize;
  auto tag = loadEnumElement(tagBytes, layout.numTagBytes);

  // The maximum number of extra tag bytes is 4.
  // Note: return early to avoid shifting baseValue by 32 which is
  // undefined behaviour.
  if (layout.numTagBytes == 4) {
    return tag;
  }

  // Replace out-of-range bytes with the base value.
  return tag | (baseValue & (~0u << (layout.numTagBytes * 8)));
}

static unsigned loadMultiPayloadValue(const OpaqueValue *value,
                                      MultiPayloadLayout layout) {
  auto bytes = reinterpret_cast<const uint8_t *>(value);
  return loadEnumElement(bytes, layout.payloadSize);
}

SWIFT_CC
static unsigned getMultiPayloadExtraInhabitantTag(const OpaqueValue *value,
                                                  unsigned enumNumXI,
                                                  const Metadata *enumType) {
  auto layout = getMultiPayloadLayout(cast<EnumMetadata>(enumType));
  unsigned index = ~loadMultiPayloadTag(value, layout, ~0u);
  
  if (index >= enumType->getValueWitnesses()->getNumExtraInhabitants())
    return 0;
  return index + 1;
}

SWIFT_CC
static void storeMultiPayloadExtraInhabitantTag(OpaqueValue *value,
                                                unsigned tag,
                                                unsigned enumNumXI,
                                                const Metadata *enumType) {
  auto layout = getMultiPayloadLayout(cast<EnumMetadata>(enumType));
  storeMultiPayloadTag(value, layout, ~(tag - 1));
}

static uint32_t getMultiPayloadEnumTagSinglePayload(const OpaqueValue *value,
                                                    uint32_t numExtraCases,
                                                    const Metadata *enumType) {
  return getEnumTagSinglePayloadImpl(value, numExtraCases, enumType,
                                     enumType->vw_size(),
                                     enumType->vw_getNumExtraInhabitants(),
                                     getMultiPayloadExtraInhabitantTag);
}

static void storeMultiPayloadEnumTagSinglePayload(OpaqueValue *value,
                                                  uint32_t index,
                                                  uint32_t numExtraCases,
                                                  const Metadata *enumType) {
  storeEnumTagSinglePayloadImpl(value, index, numExtraCases, enumType,
                                enumType->vw_size(),
                                enumType->vw_getNumExtraInhabitants(),
                                storeMultiPayloadExtraInhabitantTag);
}

void
swift::swift_storeEnumTagMultiPayload(OpaqueValue *value,
                                      const EnumMetadata *enumType,
                                      unsigned whichCase) {
  auto layout = getMultiPayloadLayout(enumType);
  unsigned numPayloads = enumType->getDescription()->getNumPayloadCases();
  if (whichCase < numPayloads) {
    // For a payload case, store the tag after the payload area.
    storeMultiPayloadTag(value, layout, whichCase);
  } else {
    // For an empty case, factor out the parts that go in the payload and
    // tag areas.
    unsigned whichEmptyCase = whichCase - numPayloads;
    unsigned whichTag, whichPayloadValue;
    if (layout.payloadSize >= 4) {
      whichTag = numPayloads;
      whichPayloadValue = whichEmptyCase;
    } else {
      unsigned numPayloadBits = layout.payloadSize * CHAR_BIT;
      whichTag = numPayloads + (whichEmptyCase >> numPayloadBits);
      whichPayloadValue = whichEmptyCase & ((1U << numPayloads) - 1U);
    }
    storeMultiPayloadTag(value, layout, whichTag);
    storeMultiPayloadValue(value, layout, whichPayloadValue);
  }
}

unsigned
swift::swift_getEnumCaseMultiPayload(const OpaqueValue *value,
                                     const EnumMetadata *enumType) {
  auto layout = getMultiPayloadLayout(enumType);
  unsigned numPayloads = enumType->getDescription()->getNumPayloadCases();

  unsigned tag = loadMultiPayloadTag(value, layout);
  if (tag < numPayloads) {
    // If the tag indicates a payload, then we're done.
    return tag;
  } else {
    // Otherwise, the other part of the discriminator is in the payload.
    unsigned payloadValue = loadMultiPayloadValue(value, layout);
    
    if (layout.payloadSize >= 4) {
      return numPayloads + payloadValue;
    } else {
      unsigned numPayloadBits = layout.payloadSize * CHAR_BIT;
      return (payloadValue | (tag - numPayloads) << numPayloadBits)
             + numPayloads;
    }
  }
}
