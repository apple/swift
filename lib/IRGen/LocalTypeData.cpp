//===--- LocalTypeData.cpp - Local type data search -----------------------===//
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
//  This file implements routines for finding and caching local type data
//  for a search.
//
//===----------------------------------------------------------------------===//

#include "LocalTypeData.h"
#include "Fulfillment.h"
#include "GenMeta.h"
#include "GenOpaque.h"
#include "GenPack.h"
#include "GenProto.h"
#include "IRGenDebugInfo.h"
#include "IRGenFunction.h"
#include "IRGenModule.h"
#include "MetadataRequest.h"
#include "swift/AST/IRGenOptions.h"
#include "swift/AST/PackConformance.h"
#include "swift/AST/ProtocolConformance.h"
#include "swift/SIL/SILModule.h"

using namespace swift;
using namespace irgen;

LocalTypeDataKey LocalTypeDataKey::getCachingKey() const {
  return { Type, Kind.getCachingKind() };
}

LocalTypeDataKind LocalTypeDataKind::getCachingKind() const {
  // Most local type data kinds are already canonical.
  if (!isConcreteProtocolConformance()) return *this;

  // Map protocol conformances to their root normal conformance.
  auto conformance = getConcreteProtocolConformance();
  return forConcreteProtocolWitnessTable(conformance->getRootConformance());
}

LocalTypeDataCache &IRGenFunction::getOrCreateLocalTypeData() {
  // Lazily allocate it.
  if (LocalTypeData) return *LocalTypeData;
  LocalTypeData = new LocalTypeDataCache();
  return *LocalTypeData;
}

void IRGenFunction::destroyLocalTypeData() {
  delete LocalTypeData;
}

OperationCost LocalTypeDataCache::CacheEntry::cost() const {
  switch (getKind()) {
  case Kind::Concrete:
    return cast<ConcreteCacheEntry>(this)->cost();
  case Kind::Abstract:
    return cast<AbstractCacheEntry>(this)->cost();
  }
  llvm_unreachable("bad cache entry kind");
}

OperationCost
LocalTypeDataCache::CacheEntry::costForRequest(LocalTypeDataKey key,
                                        DynamicMetadataRequest request) const {
  switch (getKind()) {
  case Kind::Concrete:
    return cast<ConcreteCacheEntry>(this)->costForRequest(key, request);
  case Kind::Abstract:
    return cast<AbstractCacheEntry>(this)->costForRequest(key, request);
  }
  llvm_unreachable("bad cache entry kind");
}

OperationCost
LocalTypeDataCache::ConcreteCacheEntry::costForRequest(LocalTypeDataKey key,
                                        DynamicMetadataRequest request) const {
  auto totalCost = cost();
  if (!immediatelySatisfies(key, request)) {
    // Use a lower cost for requests where emitCheckTypeMetadataState can just
    // branch on the existing response's returned dynamic state.
    totalCost += getCheckTypeMetadataStateCost(request, Value);
  }
  return totalCost;
}

OperationCost
LocalTypeDataCache::AbstractCacheEntry::costForRequest(LocalTypeDataKey key,
                                        DynamicMetadataRequest request) const {
  auto totalCost = cost();
  if (!immediatelySatisfies(key, request)) {
    totalCost += OperationCost::Call;
  }
  return totalCost;
}

void LocalTypeDataCache::CacheEntry::erase() const {
  switch (getKind()) {
  case Kind::Concrete:
    delete cast<ConcreteCacheEntry>(this);
    return;
  case Kind::Abstract:
    delete cast<AbstractCacheEntry>(this);
    return;
  }
  llvm_unreachable("bad cache entry kind");
}

static bool immediatelySatisfies(LocalTypeDataKey key,
                                 MetadataState storedState,
                                 DynamicMetadataRequest request) {
  assert((storedState == MetadataState::Complete ||
          key.Kind.isAnyTypeMetadata()) &&
         "non-metadata entry stored with incomplete state");

  return request.isSatisfiedBy(storedState);
}

bool LocalTypeDataCache::CacheEntry::immediatelySatisfies(
                                         LocalTypeDataKey key,
                                         DynamicMetadataRequest request) const {
  switch (getKind()) {
  case Kind::Concrete:
    return cast<ConcreteCacheEntry>(this)->immediatelySatisfies(key, request);
  case Kind::Abstract:
    return cast<AbstractCacheEntry>(this)->immediatelySatisfies(key, request);
  }
  llvm_unreachable("bad cache entry kind");
}

bool LocalTypeDataCache::ConcreteCacheEntry::immediatelySatisfies(
                                         LocalTypeDataKey key,
                                         DynamicMetadataRequest request) const {
  return ::immediatelySatisfies(key, Value.getStaticLowerBoundOnState(),
                                request);
}

bool LocalTypeDataCache::AbstractCacheEntry::immediatelySatisfies(
                                         LocalTypeDataKey key,
                                         DynamicMetadataRequest request) const {
  return ::immediatelySatisfies(key, getState(), request);
}

MetadataResponse
IRGenFunction::tryGetLocalTypeMetadataForLayout(SILType layoutType,
                                                DynamicMetadataRequest request){
  auto type = layoutType.getASTType();

  // Check under the formal type first.
  if (type->isLegalFormalType()) {
    if (auto response = tryGetLocalTypeMetadata(type, request))
      return response;
  }

  auto key = LocalTypeDataKey(type,
                            LocalTypeDataKind::forRepresentationTypeMetadata());
  return tryGetLocalTypeMetadata(key, request);
}

MetadataResponse
IRGenFunction::tryGetLocalTypeMetadata(CanType type,
                                       DynamicMetadataRequest request) {
  auto key = LocalTypeDataKey(type, LocalTypeDataKind::forFormalTypeMetadata());
  return tryGetLocalTypeMetadata(key, request);
}

MetadataResponse
IRGenFunction::tryGetLocalTypeMetadata(LocalTypeDataKey key,
                                       DynamicMetadataRequest request) {
  assert(key.Kind.isAnyTypeMetadata());
  if (!LocalTypeData) return MetadataResponse();
  return LocalTypeData->tryGet(*this, key, /*allow abstract*/ true, request);
}

/// Get local type data if it's possible to do so without emitting code.
/// Specifically, it doesn't call MetadataPath::follow, and therefore
/// it's safe to call from MetadataPath::follow.
///
/// It's okay to call this with any kind of key.
MetadataResponse
IRGenFunction::tryGetConcreteLocalTypeData(LocalTypeDataKey key,
                                           DynamicMetadataRequest request) {
  if (!LocalTypeData) return MetadataResponse();
  return LocalTypeData->tryGet(*this, key, /*allow abstract*/ false, request);
}

llvm::Value *IRGenFunction::tryGetLocalTypeDataForLayout(SILType type,
                                                       LocalTypeDataKind kind) {
  return tryGetLocalTypeData(LocalTypeDataKey(type.getASTType(), kind));
}

llvm::Value *IRGenFunction::tryGetLocalTypeData(CanType type,
                                                LocalTypeDataKind kind) {
  return tryGetLocalTypeData(LocalTypeDataKey(type, kind));
}

llvm::Value *IRGenFunction::tryGetLocalTypeData(LocalTypeDataKey key) {
  assert(!key.Kind.isAnyTypeMetadata());
  if (!LocalTypeData) return nullptr;
  if (auto response = LocalTypeData->tryGet(*this, key, /*allow abstract*/ true,
                                            MetadataState::Complete))
    return response.getMetadata();
  return nullptr;
}

MetadataResponse
LocalTypeDataCache::tryGet(IRGenFunction &IGF, LocalTypeDataKey key,
                           bool allowAbstract, DynamicMetadataRequest request) {
  // Use the caching key.
  key = key.getCachingKey();

  auto it = Map.find(key);
  if (it == Map.end()) return MetadataResponse();
  auto &chain = it->second;

  CacheEntry *best = nullptr;
  llvm::Optional<OperationCost> bestCost;

  CacheEntry *next = chain.Root;
  while (next) {
    CacheEntry *cur = next;
    next = cur->getNext();

    // Ignore abstract entries if so requested.
    if (!allowAbstract && !isa<ConcreteCacheEntry>(cur))
      continue;

    // Ignore unacceptable entries.
    if (!IGF.isActiveDominancePointDominatedBy(cur->DefinitionPoint))
      continue;

    // If there's a collision, compare by cost, ignoring higher-cost entries.
    if (best) {
      // Compute the cost of the best entry if we haven't done so already.
      // If that's zero, go ahead and short-circuit out.
      if (!bestCost) {
        bestCost = best->costForRequest(key, request);
        if (*bestCost == OperationCost::Free) break;
      }

      auto curCost = cur->costForRequest(key, request);
      if (curCost >= *bestCost) continue;

      // Replace the best cost and fall through.
      bestCost = curCost;
    }
    best = cur;
  }

  // If we didn't find anything, we're done.
  if (!best) return MetadataResponse();

  // Okay, we've found the best entry available.
  switch (best->getKind()) {

  // For concrete caches, this is easy.
  case CacheEntry::Kind::Concrete: {
    auto entry = cast<ConcreteCacheEntry>(best);

    if (entry->immediatelySatisfies(key, request))
      return entry->Value;

    assert(key.Kind.isAnyTypeMetadata());

    // Emit a dynamic check that the type metadata matches the request.
    // TODO: we could potentially end up calling this redundantly with a
    //   dynamic request.  Fortunately, those are used only in very narrow
    //   circumstances.
    auto response = emitCheckTypeMetadataState(IGF, request, entry->Value);

    // Add a concrete entry for the checked result.
    IGF.setScopedLocalTypeData(key, response);

    return response;
  }

  // For abstract caches, we need to follow a path.
  case CacheEntry::Kind::Abstract: {
    auto entry = cast<AbstractCacheEntry>(best);

    // Follow the path.
    auto &source = AbstractSources[entry->SourceIndex];
    auto response = entry->follow(IGF, source, request);

    // Following the path automatically caches at every point along it,
    // including the end.  If you hit the second assertion here, it's
    // probably because MetadataPath::followComponent isn't updating
    // sourceKey correctly to lead back to the same key you originally
    // looked up.
    assert(chain.Root->DefinitionPoint == IGF.getActiveDominancePoint());
    assert(isa<ConcreteCacheEntry>(chain.Root));

    return response;
  }

  }
  llvm_unreachable("bad cache entry kind");
}

MetadataResponse
LocalTypeDataCache::AbstractCacheEntry::follow(IRGenFunction &IGF,
                                               AbstractSource &source,
                                        DynamicMetadataRequest request) const {
  switch (source.getKind()) {
  case AbstractSource::Kind::TypeMetadata:
    return Path.followFromTypeMetadata(IGF, source.getType(),
                                       source.getValue(), request, nullptr);

  case AbstractSource::Kind::ProtocolWitnessTable:
    return Path.followFromWitnessTable(IGF, source.getType(),
                                       source.getProtocolConformance(),
                                       source.getValue(), request, nullptr);
  }
  llvm_unreachable("bad source kind");
}

static void maybeEmitDebugInfoForLocalTypeData(IRGenFunction &IGF,
                                               LocalTypeDataKey key,
                                               MetadataResponse value) {
  // FIXME: This check doesn't entirely behave correctly for non-transparent
  // functions that were inlined into transparent functions. Correct would be to
  // check which instruction requests the type metadata and see whether its
  // inlined function is transparent.
  auto * DS = IGF.getDebugScope();
  if (DS && DS->getInlinedFunction() &&
      DS->getInlinedFunction()->isTransparent())
    return;
  
  // Only for formal type metadata.
  if (key.Kind != LocalTypeDataKind::forFormalTypeMetadata())
    return;

  // Only for archetypes, and not for opened/opaque archetypes.
  auto type = dyn_cast<ArchetypeType>(key.Type);
  if (!type)
    return;
  if (!type->isRoot())
    return;
  if (!isa<PrimaryArchetypeType>(type) && !isa<PackArchetypeType>(type))
    return;

  auto *typeParam = type->getInterfaceType()->castTo<GenericTypeParamType>();
  auto name = typeParam->getName().str();

  llvm::Value *data = value.getMetadata();

  if (key.Type->is<PackArchetypeType>())
    data = maskMetadataPackPointer(IGF, data);

  // At -O0, create an alloca to keep the type alive. Not for async functions
  // though; see the comment in IRGenFunctionSIL::emitShadowCopyIfNeeded().
  if (!IGF.IGM.IRGen.Opts.shouldOptimize() && !IGF.isAsync()) {
    auto alloca =
        IGF.createAlloca(data->getType(), IGF.IGM.getPointerAlignment(), name);
    IGF.Builder.CreateStore(data, alloca);
    data = alloca.getAddress();
  }

  // Only if debug info is enabled.
  if (!IGF.IGM.DebugInfo)
    return;

  IGF.IGM.DebugInfo->emitTypeMetadata(IGF, data,
                                      typeParam->getDepth(),
                                      typeParam->getIndex(),
                                      name);
}

void
IRGenFunction::setScopedLocalTypeMetadataForLayout(SILType type,
                                                   MetadataResponse response) {
  auto key = LocalTypeDataKey(type.getASTType(),
                         LocalTypeDataKind::forRepresentationTypeMetadata());
  setScopedLocalTypeData(key, response);
}

void IRGenFunction::setScopedLocalTypeMetadata(CanType type,
                                               MetadataResponse response) {
  auto key = LocalTypeDataKey(type, LocalTypeDataKind::forFormalTypeMetadata());
  setScopedLocalTypeData(key, response);
}

void IRGenFunction::setScopedLocalTypeData(CanType type,
                                           LocalTypeDataKind kind,
                                           llvm::Value *data) {
  assert(!kind.isAnyTypeMetadata());
  setScopedLocalTypeData(LocalTypeDataKey(type, kind),
                         MetadataResponse::forComplete(data));
}

void IRGenFunction::setScopedLocalTypeDataForLayout(SILType type,
                                                    LocalTypeDataKind kind,
                                                    llvm::Value *data) {
  assert(!kind.isAnyTypeMetadata());
  setScopedLocalTypeData(LocalTypeDataKey(type.getASTType(), kind),
                         MetadataResponse::forComplete(data));
}

void IRGenFunction::setScopedLocalTypeData(LocalTypeDataKey key,
                                           MetadataResponse value) {
  maybeEmitDebugInfoForLocalTypeData(*this, key, value);

  // Register with the active ConditionalDominanceScope if necessary.
  bool isConditional = isConditionalDominancePoint();
  if (isConditional) {
    registerConditionalLocalTypeDataKey(key);
  }

  getOrCreateLocalTypeData().addConcrete(getActiveDominancePoint(),
                                         isConditional, key, value);
}

void IRGenFunction::setUnscopedLocalTypeMetadata(CanType type,
                                                 MetadataResponse response) {
  LocalTypeDataKey key(type, LocalTypeDataKind::forFormalTypeMetadata());
  setUnscopedLocalTypeData(key, response);
}

void IRGenFunction::setUnscopedLocalTypeData(CanType type,
                                             LocalTypeDataKind kind,
                                             llvm::Value *data) {
  assert(!kind.isAnyTypeMetadata());
  setUnscopedLocalTypeData(LocalTypeDataKey(type, kind),
                           MetadataResponse::forComplete(data));
}

void IRGenFunction::setUnscopedLocalTypeData(LocalTypeDataKey key,
                                             MetadataResponse value) {
  maybeEmitDebugInfoForLocalTypeData(*this, key, value);

  // This is supportable, but it would require ensuring that we add the
  // entry after any conditional entries; otherwise the stack discipline
  // will get messed up.
  assert(!isConditionalDominancePoint() &&
         "adding unscoped local type data while in conditional scope");

  getOrCreateLocalTypeData().addConcrete(DominancePoint::universal(),
                                         /*conditional*/ false, key, value);
}

void IRGenFunction::bindLocalTypeDataFromTypeMetadata(CanType type,
                                                      IsExact_t isExact,
                                                      llvm::Value *metadata,
                                                      MetadataState state) {
  auto response = MetadataResponse::forBounded(metadata, state);

  // Remember that we have this type metadata concretely.
  if (isExact) {
    if (!metadata->hasName()) setTypeMetadataName(IGM, metadata, type);
    setScopedLocalTypeMetadata(type, response);
  }

  // Don't bother adding abstract fulfillments at a conditional dominance
  // point; we're too likely to throw them all away.
  if (isConditionalDominancePoint())
    return;

  getOrCreateLocalTypeData()
    .addAbstractForTypeMetadata(*this, type, isExact, response);
}

void IRGenFunction::bindLocalTypeDataFromSelfWitnessTable(
                const ProtocolConformance *conformance,
                llvm::Value *selfTable,
                llvm::function_ref<CanType (CanType)> getTypeInContext) {
  SILWitnessTable::enumerateWitnessTableConditionalConformances(
      conformance,
      [&](unsigned index, CanType type, ProtocolDecl *proto) {
        if (auto packType = dyn_cast<PackType>(type)) {
          if (auto expansion = packType.unwrapSingletonPackExpansion())
            type = expansion.getPatternType();
        }

        type = getTypeInContext(type);

        if (isa<ArchetypeType>(type)) {
          WitnessIndex wIndex(privateWitnessTableIndexToTableOffset(index),
                              /*prefix*/ false);

          auto table = loadConditionalConformance(*this ,selfTable,
                                                  wIndex.forProtocolWitnessTable());
          table = Builder.CreateBitCast(table, IGM.WitnessTablePtrTy);
          setProtocolWitnessTableName(IGM, table, type, proto);

          setUnscopedLocalTypeData(
              type,
              LocalTypeDataKind::forAbstractProtocolWitnessTable(proto),
              table);
        }

        return /*finished?*/ false;
      });
}

void LocalTypeDataCache::addAbstractForTypeMetadata(IRGenFunction &IGF,
                                                    CanType type,
                                                    IsExact_t isExact,
                                                    MetadataResponse metadata) {
  struct Callback : FulfillmentMap::InterestingKeysCallback {
    bool isInterestingType(CanType type) const override {
      return true;
    }
    bool hasInterestingType(CanType type) const override {
      return true;
    }
    bool isInterestingPackExpansion(CanPackExpansionType type) const override {
      return isa<PackArchetypeType>(type.getPatternType());
    }
    bool hasLimitedInterestingConformances(CanType type) const override {
      return false;
    }
    GenericSignature::RequiredProtocols
    getInterestingConformances(CanType type) const override {
      llvm_unreachable("no limits");
    }
    CanType getSuperclassBound(CanType type) const override {
      if (auto arch = dyn_cast<ArchetypeType>(type))
        if (auto superclassTy = arch->getSuperclass())
          return superclassTy->getCanonicalType();
      return CanType();
    }
  } callbacks;

  // Look for anything at all that's fulfilled by this.  If we don't find
  // anything, stop.
  FulfillmentMap fulfillments;
  if (!fulfillments.searchTypeMetadata(IGF.IGM, type, isExact,
                                       metadata.getStaticLowerBoundOnState(),
                                       /*source*/ 0, MetadataPath(),
                                       callbacks)) {
    return;
  }

  addAbstractForFulfillments(IGF, std::move(fulfillments),
                             [&]() -> AbstractSource {
    return AbstractSource(type, metadata);
  });
}

void LocalTypeDataCache::
addAbstractForFulfillments(IRGenFunction &IGF, FulfillmentMap &&fulfillments,
                           llvm::function_ref<AbstractSource()> createSource) {
  // Add the source lazily.
  llvm::Optional<unsigned> sourceIndex;
  auto getSourceIndex = [&]() -> unsigned {
    if (!sourceIndex) {
      AbstractSources.emplace_back(createSource());
      sourceIndex = AbstractSources.size() - 1;
    }
    return *sourceIndex;
  };

  for (auto &fulfillment : fulfillments) {
    CanType type = fulfillment.first.getTypeParameter();
    LocalTypeDataKind localDataKind;

    switch (fulfillment.first.getKind()) {
    case GenericRequirement::Kind::Shape: {
      localDataKind = LocalTypeDataKind::forPackShapeExpression();
      break;
    }
    case GenericRequirement::Kind::Metadata:
    case GenericRequirement::Kind::MetadataPack: {
      // Ignore type metadata fulfillments for non-dependent types that
      // we can produce very cheaply.  We don't want to end up emitting
      // the type metadata for Int by chasing through N layers of metadata
      // just because that path happens to be in the cache.
      if (!type->hasArchetype() &&
          !shouldCacheTypeMetadataAccess(IGF.IGM, type)) {
        continue;
      }

      localDataKind = LocalTypeDataKind::forFormalTypeMetadata();
      break;
    }
    case GenericRequirement::Kind::WitnessTable:
    case GenericRequirement::Kind::WitnessTablePack: {
      // For now, ignore witness-table fulfillments when they're not for
      // archetypes.
      ProtocolDecl *protocol = fulfillment.first.getProtocol();
      if (auto archetype = dyn_cast<ArchetypeType>(type)) {
        auto conformsTo = archetype->getConformsTo();
        auto it = std::find(conformsTo.begin(), conformsTo.end(), protocol);
        if (it == conformsTo.end()) continue;
        localDataKind = LocalTypeDataKind::forAbstractProtocolWitnessTable(*it);
      } else {
        continue;
      }

      break;
    }
    }

    // Find the chain for the key.
    auto key = getKey(type, localDataKind).getCachingKey();
    auto &chain = Map[key];

    // Check whether there's already an entry that's at least as good as the
    // fulfillment.
    llvm::Optional<OperationCost> fulfillmentCost;
    auto getFulfillmentCost = [&]() -> OperationCost {
      if (!fulfillmentCost)
        fulfillmentCost = fulfillment.second.Path.cost();
      return *fulfillmentCost;
    };

    bool isConditional = IGF.isConditionalDominancePoint();

    bool foundBetter = false;
    for (CacheEntry *cur = chain.Root, *last = nullptr; cur;
         last = cur, cur = cur->getNext()) {
      // Ensure the entry is acceptable.
      if (!IGF.isActiveDominancePointDominatedBy(cur->DefinitionPoint))
        continue;

      // Ensure that the entry isn't better than the fulfillment.
      auto curCost = cur->cost();
      if (curCost == OperationCost::Free || curCost <= getFulfillmentCost()) {
        foundBetter = true;
        break;
      }

      // If the entry is defined at the current point, (1) we know there
      // won't be a better entry and (2) we should remove it.
      if (cur->DefinitionPoint == IGF.getActiveDominancePoint() &&
          !isConditional) {
        // Splice it out of the chain.
        assert(!cur->isConditional());
        chain.eraseEntry(last, cur);
        break;
      }
    }
    if (foundBetter) continue;

    // Okay, make a new entry.

    // Register with the conditional dominance scope if necessary.
    if (isConditional) {
      IGF.registerConditionalLocalTypeDataKey(key);
    }

    // Allocate the new entry.
    auto newEntry = new AbstractCacheEntry(IGF.getActiveDominancePoint(),
                                           isConditional,
                                           getSourceIndex(),
                                           std::move(fulfillment.second.Path),
                                           fulfillment.second.getState());

    // Add it to the front of the chain.
    chain.push_front(newEntry);
  }
}
#if !defined(NDEBUG) || defined(LLVM_ENABLE_DUMP)
LLVM_DUMP_METHOD void LocalTypeDataCache::dump() const {
  auto &out = llvm::errs();

  if (Map.empty()) {
    out << "(empty)\n";
    return;
  }

  for (auto &mapEntry : Map) {
    mapEntry.first.print(out);
    out << " => [";

    if (mapEntry.second.Root) out << "\n";
    for (auto cur = mapEntry.second.Root; cur; cur = cur->getNext()) {
      out << "  (";
      if (cur->DefinitionPoint.isUniversal()) out << "universal";
      else out << cur->DefinitionPoint.as<void>();
      out << ") ";

      if (cur->isConditional()) out << "conditional ";

      switch (cur->getKind()) {
      case CacheEntry::Kind::Concrete: {
        auto entry = cast<ConcreteCacheEntry>(cur);
        auto value = entry->Value.getMetadata();
        out << "concrete: " << value << "\n  ";
        if (!isa<llvm::Instruction>(value)) out << "  ";
        value->dump();
        break;
      }

      case CacheEntry::Kind::Abstract: {
        auto entry = cast<AbstractCacheEntry>(cur);
        out << "abstract: source=" << entry->SourceIndex << "\n";
        break;
      }
      }
    }
    out << "]\n";
  }
}
#endif

#if !defined(NDEBUG) || defined(LLVM_ENABLE_DUMP)
LLVM_DUMP_METHOD void LocalTypeDataKey::dump() const {
  print(llvm::errs());
  llvm::errs() << "\n";
}
#endif

void LocalTypeDataKey::print(llvm::raw_ostream &out) const {
  out << "(" << Type.getPointer()
      << " (" << Type << "), ";
  Kind.print(out);
  out << ")";
}

#if !defined(NDEBUG) || defined(LLVM_ENABLE_DUMP)
LLVM_DUMP_METHOD void LocalTypeDataKind::dump() const {
  print(llvm::errs());
  llvm::errs() << "\n";
}
#endif

void LocalTypeDataKind::print(llvm::raw_ostream &out) const {
  if (isConcreteProtocolConformance()) {
    out << "ConcreteConformance(";
    getConcreteProtocolConformance()->printName(out);
    out << ")";
  } else if (isAbstractProtocolConformance()) {
    out << "AbstractConformance("
        << getAbstractProtocolConformance()->getName()
        << ")";
  } else if (isPackProtocolConformance()) {
    out << "PackConformance("
        << getPackProtocolConformance()->getType()
        << ":"
        << getPackProtocolConformance()->getProtocol()->getName()
        << ")";
  } else if (Value == FormalTypeMetadata) {
    out << "FormalTypeMetadata";
  } else if (Value == RepresentationTypeMetadata) {
    out << "RepresentationTypeMetadata";
  } else if (Value == ValueWitnessTable) {
    out << "ValueWitnessTable";
  } else if (Value == Shape) {
    out << "Shape";
  } else {
    assert(isSingletonKind());
    if (Value >= ValueWitnessDiscriminatorBase) {
      auto witness = ValueWitness(Value - ValueWitnessDiscriminatorBase);
      out << "Discriminator(" << getValueWitnessName(witness) << ")";
      return;
    }
    ValueWitness witness = ValueWitness(Value - ValueWitnessBase);
    out << getValueWitnessName(witness);
  }
}

IRGenFunction::ConditionalDominanceScope::~ConditionalDominanceScope() {
  IGF.ConditionalDominance = OldScope;

  // Remove any conditional entries from the chains that were added in this
  // scope.
  for (auto &key : RegisteredKeys) {
    IGF.LocalTypeData->eraseConditional(key);
  }
}

void LocalTypeDataCache::eraseConditional(ArrayRef<LocalTypeDataKey> keys) {
  for (auto &key : keys) {
    auto &chain = Map[key.getCachingKey()];

    // Our ability to simply delete the front of the chain relies on an
    // assumption that (1) conditional additions always go to the front of
    // the chain and (2) we never add something unconditionally while in
    // an unconditional scope.
    assert(chain.Root);
    assert(chain.Root->isConditional());
    chain.eraseEntry(nullptr, chain.Root);
  }
}
