//===--- AccessEnforcementWMO.cpp - Whole-module access enforcement opt ---===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2018 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//
///
/// This module pass removes dynamic access enforcement based on whole module
/// information.
///
/// This maps each access of identified storage onto a disjoint access
/// location. Local accesses (Box and Stack) already have unique AccessStorage
/// and should be removed by an earlier function pass if possible. This pass
/// handles Class and Global access by partitioning their non-unique
/// AccessStorage objects into unique DisjointAccessLocations. These disjoint
/// access locations may be accessed across multiple functions, so a module pass
/// is required to identify and optimize them.
///
/// Class accesses are partitioned by their fully qualified property
/// name. Global accesses are partitioned by the global variable name. Argument
/// accesses are ignored because they are considered an access in the caller,
/// while Class and Global access always occurs in the callee. Note that a SIL
/// function argument value may be the source of a either a Class-kind
/// AccessStorage or an Argument-kind AccessStorage. When the argument is
/// used to access a class, it will always be identified as a Class-kind
/// AccessStorage even though its source is an argument.
///
/// For each function, discover the disjointly accessed locations. Each location
/// is optimistically mapped to a `noNestedConflict` flag (if a location has not
/// been mapped, then it is assumed not to have nested conflict). Each location
/// without any nested conflict maps to a set of accesses that the optimization
/// may be able to remove.
///
/// After analyzing all functions in the module, disable any class property and
/// global variable access that still have not seen any nested conflicts by
/// giving them static enforcement.
///
/// Warning: This is only sound when unidentified accesses can never alias with
/// Class/Global access. To enforce this, the SILVerifier calls
/// identifyFormalAccess() for every access, which asserts that any Unidentified
/// access belongs to a know pattern that cannot originate from Class or Global
/// accesses.
///
/// Note: This optimization must be aware of all possible access to a Class or
/// Global address. This includes unpaired access instructions and keypath
/// instructions. Ignoring any access pattern would weaken enforcement. For
/// example, AccessStorageAnalysis cannot be used here because that analysis
/// may conservatively summarize some functions.
//===----------------------------------------------------------------------===//

#define DEBUG_TYPE "access-enforcement-wmo"

#include "swift/Basic/Assertions.h"
#include "swift/Basic/SmallPtrSetVector.h"
#include "swift/SIL/DebugUtils.h"
#include "swift/SIL/MemAccessUtils.h"
#include "swift/SIL/SILFunction.h"
#include "swift/SILOptimizer/PassManager/Transforms.h"
#include "swift/SILOptimizer/Utils/InstOptUtils.h"

using namespace swift;

using llvm::DenseMap;
using llvm::SmallDenseSet;

using DisjointAccessLocationKey =
    llvm::PointerUnion<const VarDecl *, const SILGlobalVariable *>;

// Get the VarDecl that represents the DisjointAccessLocation for the given
// storage and access base. Returns nullptr for any storage that can't be
// partitioned into a disjoint location.
//
// Global storage is expected to be disjoint because identifyFormalAccess may
// only return Unidentified storage for a global variable access if the global
// is defined in a different module.
static DisjointAccessLocationKey
getDisjointAccessLocation(AccessStorageWithBase storageAndBase) {
  auto storage = storageAndBase.storage;
  switch (storage.getKind()) {
  case AccessStorage::Class: {
    auto *varDecl = cast<VarDecl>(storageAndBase.getDecl());
    // For class properties, a VarDecl can always be derived from AccessBase.
    assert(varDecl && "no VarDecl for class property");
    return varDecl;
  }
  case AccessStorage::Global:
    return storageAndBase.getAccessBase().getGlobal();
  case AccessStorage::Box:
  case AccessStorage::Stack:
  case AccessStorage::Tail:
  case AccessStorage::Argument:
  case AccessStorage::Yield:
  case AccessStorage::Unidentified:
    return nullptr;
  case AccessStorage::Nested:
    llvm_unreachable("Unexpected Nested access.");
  }
  llvm_unreachable("unhandled kind");
}

static bool isVisibleExternally(DisjointAccessLocationKey key, SILModule *mod) {
  if (auto *decl = key.dyn_cast<const VarDecl *>())
    return mod->isVisibleExternally(decl);

  auto *global = key.get<const SILGlobalVariable *>();
  return isPossiblyUsedExternally(global->getLinkage(), mod->isWholeModule());
}

static StringRef getName(DisjointAccessLocationKey key) {
  if (auto *decl = key.dyn_cast<const VarDecl *>())
    return decl->getNameStr();

  return key.get<const SILGlobalVariable *>()->getName();
}

namespace {
// Implements an optimization to remove access markers on disjoint memory
// locations that are never reentrantly accessed. For a given memory location,
// if there are no potential nested conflicts, then enforcement must succeed for
// any access to that location.
//
// The existence of unidentified access complicates this problem. For this
// optimization to be valid, Global and Class property access must always be
// identifiable. identifyFormalAccess() in MemAccessUtils enforces a short list
// of unidentified producers (non-address PhiArgument, PointerToAddress, Undef,
// & local-init). We cannot allow the address of a global variable or class
// property to be exposed via one of these instructions, unless the declaration
// is considered "visible externally".
//
// Note: This assumes that PointerToAddress is never used to access a global or
// class property unless it's done via an Unsafe API that doesn't need
// enforcement.
//
// FIXME: Tail-allocated heap storage is currently categorized as
// Unidentified. This is conservatively safe because it can't also be accessed
// as a class property. However, it is inconsistent with the general rule that
// only local storage is unidentified, and we can probably remove a lot of
// accesses by being more precise here (OTOH, if this is known CoW storage, we
// should eventually add an even stronger optimization to remove exclusivity
// checks).
class GlobalAccessRemoval {
  SILModule &module;
  SmallPtrSetVector<SILFunction *, 8> changedFunctions;

  using BeginAccessSet = SmallDenseSet<BeginAccessInst *, 8>;

  /// Information for an access location that, if it is valid, must be disjoint
  /// from all other access locations.
  struct DisjointAccessLocationInfo {
    AccessStorage::Kind accessKind = AccessStorage::Unidentified;
    // False if any nested conflict has been seen at this location.
    bool noNestedConflict = true;
    BeginAccessSet beginAccessSet;
  };

  DenseMap<DisjointAccessLocationKey, DisjointAccessLocationInfo>
      disjointAccessMap;

public:
  GlobalAccessRemoval(SILModule &module) : module(module) {}

  void perform();
  
  void invalidateAnalysis(SILModuleTransform *pass);

protected:
  bool visitInstruction(SILInstruction *I);
  void recordAccess(SILInstruction *beginAccess, DisjointAccessLocationKey key,
                    AccessStorage::Kind storageKind, bool hasNoNestedConflict);
  void removeNonreentrantAccess();
};
} // namespace

/// Process all begin_access instructions in the module, recording their
/// DisjointAccessLocation. Ignore accesses at call sites because Arguments and
/// Unidentified access can only local storage.
void GlobalAccessRemoval::perform() {
  for (auto &F : module) {
    if (F.empty())
      continue;

    for (auto &BB : F) {
      for (auto &I : BB) {
        if (!visitInstruction(&I))
          return;
      }
    }
  }
  removeNonreentrantAccess();
}

void GlobalAccessRemoval::invalidateAnalysis(SILModuleTransform *pass) {
  for (SILFunction *changedFunction : changedFunctions) {
    pass->invalidateAnalysis(changedFunction,
                             SILAnalysis::InvalidationKind::Instructions);
  }
}

bool GlobalAccessRemoval::visitInstruction(SILInstruction *I) {
  if (auto *BAI = dyn_cast<BeginAccessInst>(I)) {
    auto storageAndBase = AccessStorageWithBase::compute(BAI->getSource());
    if (!storageAndBase.base)
      return false;
    auto key = getDisjointAccessLocation(storageAndBase);
    recordAccess(BAI, key, storageAndBase.storage.getKind(),
                 BAI->hasNoNestedConflict());
    return true;
  }
  if (auto *BUAI = dyn_cast<BeginUnpairedAccessInst>(I)) {
    auto storageAndBase = AccessStorageWithBase::compute(BUAI->getSource());
    if (!storageAndBase.base)
      return false;
    auto key = getDisjointAccessLocation(storageAndBase);
    recordAccess(BUAI, key, storageAndBase.storage.getKind(),
                 BUAI->hasNoNestedConflict());
    return true;
  }
  if (auto *KPI = dyn_cast<KeyPathInst>(I)) {
    for (const KeyPathPatternComponent &component :
         KPI->getPattern()->getComponents()) {
      switch (component.getKind()) {
      case KeyPathPatternComponent::Kind::StoredProperty:
        recordAccess(KPI, component.getStoredPropertyDecl(),
                     AccessStorage::Class, /*hasNoNestedConflict=*/false);
        break;
      case KeyPathPatternComponent::Kind::GettableProperty:
      case KeyPathPatternComponent::Kind::SettableProperty:
      case KeyPathPatternComponent::Kind::OptionalChain:
      case KeyPathPatternComponent::Kind::OptionalForce:
      case KeyPathPatternComponent::Kind::OptionalWrap:
      case KeyPathPatternComponent::Kind::TupleElement:
        break;
      }
    }
  }
  return true;
}

// Record an access in the disjointAccessMap.
//
// `beginAccess` is any instruction that represents a potential access,
// including key_path. One of the following conditions must be true for every
// begin_[unpaired_]access instructions:
// - it is guaranteed to access local memory, not a class property or global.
// - it has an identifiable source.
// - is is generated from a Builtin, which is assumed to have an associated
// key_path instruction somewhere else in the same module (or it must be dead
// code, or only access public properties).
//
// `key` may be nullptr if the variable's identity cannot be determined from the
// access. This is only legal when the access is known to be a local access, not
// a class property or global.
void GlobalAccessRemoval::recordAccess(SILInstruction *beginAccess,
                                       DisjointAccessLocationKey key,
                                       AccessStorage::Kind storageKind,
                                       bool hasNoNestedConflict) {
  if (key.isNull() || isVisibleExternally(key, &module))
    return;

  LLVM_DEBUG(if (!hasNoNestedConflict) llvm::dbgs()
             << "Nested conflict on " << getName(key) << " at" << *beginAccess
             << "\n");

  auto accessLocIter = disjointAccessMap.find(key);
  if (accessLocIter != disjointAccessMap.end()) {
    // Add this begin_access to an existing DisjointAccessLocationInfo.
    DisjointAccessLocationInfo &info = accessLocIter->second;
    assert(info.accessKind == storageKind);
    info.noNestedConflict &= hasNoNestedConflict;
    // Don't currently optimize unpaired access.
    if (auto *BAI = dyn_cast<BeginAccessInst>(beginAccess))
      info.beginAccessSet.insert(BAI);
    return;
  }

  // Create the first DisjointAccessLocationInfo for this begin_access.
  DisjointAccessLocationInfo info;
  info.accessKind = storageKind;
  info.noNestedConflict = hasNoNestedConflict;
  if (auto *BA = dyn_cast<BeginAccessInst>(beginAccess))
    info.beginAccessSet.insert(BA);
  disjointAccessMap.insert(std::make_pair(key, info));
}

// For each unique storage within this function that is never reentrantly
// accessed, promote all access checks for that storage to static enforcement.
void GlobalAccessRemoval::removeNonreentrantAccess() {
  for (auto &keyAndInfo : disjointAccessMap) {
    const DisjointAccessLocationInfo &info = keyAndInfo.second;
    if (!info.noNestedConflict)
      continue;

    auto key = keyAndInfo.first;
    LLVM_DEBUG(llvm::dbgs()
               << "Eliminating all formal access on " << getName(key) << "\n");
    assert(!isVisibleExternally(key, &module));

    // Non-deterministic iteration, only used to set a flag.
    for (BeginAccessInst *beginAccess : info.beginAccessSet) {
      LLVM_DEBUG(llvm::dbgs() << "  Disabling access marker " << *beginAccess);
      beginAccess->setEnforcement(SILAccessEnforcement::Static);
      changedFunctions.insert(beginAccess->getFunction());
    }
  }
}

namespace {
struct AccessEnforcementWMO : public SILModuleTransform {
  void run() override {
    GlobalAccessRemoval eliminationPass(*getModule());
    eliminationPass.perform();
    eliminationPass.invalidateAnalysis(this);
  }
};
}

SILTransform *swift::createAccessEnforcementWMO() {
  return new AccessEnforcementWMO();
}
