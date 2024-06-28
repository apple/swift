//===--- MemoryLocations.cpp ----------------------------------------------===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2021 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//

#define DEBUG_TYPE "sil-memory-locations"
#include "swift/Basic/Assertions.h"
#include "swift/SIL/MemoryLocations.h"
#include "swift/Basic/SmallBitVector.h"
#include "swift/SIL/ApplySite.h"
#include "swift/SIL/SILBasicBlock.h"
#include "swift/SIL/SILFunction.h"
#include "swift/SIL/SILModule.h"
#include "llvm/Support/raw_ostream.h"

using namespace swift;

namespace swift {
namespace {

//===----------------------------------------------------------------------===//
//                            Utility functions
//===----------------------------------------------------------------------===//

/// Enlarge the bitset if needed to set the bit with \p idx.
static void setBitAndResize(SmallBitVector &bits, unsigned idx) {
  if (bits.size() <= idx)
    bits.resize(idx + 1);
  bits.set(idx);
}

static bool allUsesInSameBlock(AllocStackInst *ASI) {
  SILBasicBlock *BB = ASI->getParent();
  int numDeallocStacks = 0;
  for (Operand *use : ASI->getUses()) {
    SILInstruction *user = use->getUser();
    if (isa<DeallocStackInst>(user)) {
      ++numDeallocStacks;
      if (user->getParent() != BB)
        return false;
    }
  }
  // In case of an unreachable, the dealloc_stack can be missing. In this
  // case we don't treat it as a single-block location.
  assert(numDeallocStacks <= 1 &&
    "A single-block stack location cannot have multiple deallocations");
  return numDeallocStacks == 1;
}

} // anonymous namespace
} // namespace swift


//===----------------------------------------------------------------------===//
//                     MemoryLocations members
//===----------------------------------------------------------------------===//

MemoryLocations::Location::Location(SILValue val, unsigned index, int parentIdx) :
      representativeValue(val),
      parentIdx(parentIdx) {
  assert(((parentIdx >= 0) ==
    (isa<StructElementAddrInst>(val) || isa<TupleElementAddrInst>(val) ||
     isa<InitEnumDataAddrInst>(val) || isa<UncheckedTakeEnumDataAddrInst>(val) ||
     isa<InitExistentialAddrInst>(val) || isa<OpenExistentialAddrInst>(val)))
    && "sub-locations can only be introduced with struct/tuple/enum projections");
  setBitAndResize(subLocations, index);
  setBitAndResize(selfAndParents, index);
}

void MemoryLocations::Location::updateFieldCounters(SILType ty, int increment) {
  SILFunction *function = representativeValue->getFunction();
  if (!ty.isEmpty(*function)) {
    numFieldsNotCoveredBySubfields += increment;
    if (!ty.isTrivial(*function))
      numNonTrivialFieldsNotCovered += increment;
  }
}

static SILValue getBaseValue(SILValue addr) {
  while (true) {
    switch (addr->getKind()) {
      case ValueKind::BeginAccessInst:
        addr = cast<BeginAccessInst>(addr)->getOperand();
        break;
      default:
        return addr;
    }
  }
}

int MemoryLocations::getLocationIdx(SILValue addr) const {
  auto iter = addr2LocIdx.find(getBaseValue(addr));
  if (iter == addr2LocIdx.end())
    return -1;
  return iter->second;
}

const MemoryLocations::Location *
MemoryLocations::getRootLocation(unsigned index) const {
  while (true) {
    const Location &loc = locations[index];
    if (loc.parentIdx < 0)
      return &loc;
    index = loc.parentIdx;
  }
}

static bool canHandleAllocStack(AllocStackInst *asi) {
  assert(asi);

  // An alloc_stack with dynamic lifetime set has a lifetime that relies on
  // unrelated conditional control flow for correctness. This means that we may
  // statically leak along paths that were known by the emitter to never be
  // taken if the value is live. So bail since we can't verify this.
  if (asi->hasDynamicLifetime())
    return false;

  // Otherwise we can optimize!
  return true;
}

void MemoryLocations::analyzeLocations(SILFunction *function) {
  // As we have to limit the set of handled locations to memory, which is
  // guaranteed to be not aliased, we currently only handle indirect function
  // arguments and alloc_stack locations.
  for (SILArgument *arg : function->getArguments()) {
    SILFunctionArgument *funcArg = cast<SILFunctionArgument>(arg);
    switch (funcArg->getArgumentConvention()) {
    case SILArgumentConvention::Indirect_In:
    case SILArgumentConvention::Indirect_In_Guaranteed:
    case SILArgumentConvention::Indirect_Out:
      // These are not SIL addresses under -enable-sil-opaque-values
      if (!function->getConventions().useLoweredAddresses())
        break;

      LLVM_FALLTHROUGH;
    case SILArgumentConvention::Indirect_Inout:
      analyzeLocation(funcArg);
      break;
    default:
      break;
    }
  }
  for (SILBasicBlock &BB : *function) {
    for (SILInstruction &I : BB) {
      if (auto *ASI = dyn_cast<AllocStackInst>(&I)) {
        if (canHandleAllocStack(ASI)) {
          if (allUsesInSameBlock(ASI)) {
            singleBlockLocations.push_back(ASI);
          } else {
            analyzeLocation(ASI);
          }
        }
      }
      if (auto *BAI = dyn_cast<BeginApplyInst>(&I)) {
        auto convention = BAI->getSubstCalleeConv();
        auto yields = convention.getYields();
        auto yieldedValues = BAI->getYieldedValues();
        for (auto index : indices(yields)) {
          if (convention.isSILIndirect(yields[index])) {
            analyzeLocation(yieldedValues[index]);
          }
        }
      }
    }
  }
}

void MemoryLocations::analyzeLocation(SILValue loc) {
  SILFunction *function = loc->getFunction();
  assert(function && "cannot analyze a SILValue which is not in a function");

  // Ignore trivial types to keep the number of locations small. Trivial types
  // are not interesting anyway, because such memory locations are not
  // destroyed.
  if (!handleTrivialLocations && loc->getType().isTrivial(*function))
    return;

  /// We don't handle empty tuples and empty structs.
  ///
  /// Locations with empty types don't even need a store to count as
  /// "initialized". We don't handle such cases.
  if (loc->getType().isEmpty(*function))
    return;

  unsigned currentLocIdx = locations.size();
  locations.push_back(Location(loc, currentLocIdx));
  SmallVector<SILValue, 8> collectedVals;
  SubLocationMap subLocationMap;
  if (!analyzeLocationUsesRecursively(loc, currentLocIdx, collectedVals,
                                      subLocationMap)) {
    locations.truncate(currentLocIdx);
    for (SILValue V : collectedVals) {
      addr2LocIdx.erase(V);
    }
    return;
  }
  addr2LocIdx[loc] = currentLocIdx;
}

void MemoryLocations::handleSingleBlockLocations(
                       std::function<void (SILBasicBlock *block)> handlerFunc) {
  SILBasicBlock *currentBlock = nullptr;
  clear();

  // Walk over all collected single-block locations.
  for (SingleValueInstruction *SVI : singleBlockLocations) {
    // Whenever the parent block changes, process the block's locations.
    if (currentBlock && SVI->getParent() != currentBlock) {
      handlerFunc(currentBlock);
      clear();
    }
    currentBlock = SVI->getParent();
    analyzeLocation(SVI);
  }
  // Process the last block's locations.
  if (currentBlock)
    handlerFunc(currentBlock);
  clear();
}

const MemoryLocations::Bits &MemoryLocations::getNonTrivialLocations() {
  if (nonTrivialLocations.empty()) {
    // Compute the bitset lazily.
    nonTrivialLocations.resize(getNumLocations());
    nonTrivialLocations.reset();
    unsigned idx = 0;
    for (Location &loc : locations) {
      initFieldsCounter(loc);
      if (loc.numNonTrivialFieldsNotCovered != 0)
        nonTrivialLocations.set(idx);
      ++idx;
    }
  }
  return nonTrivialLocations;
}

void MemoryLocations::dump() const {
  unsigned idx = 0;
  for (const Location &loc : locations) {
    llvm::dbgs() << "location #" << idx << ": sublocs=" << loc.subLocations
                 << ", parent=" << loc.parentIdx
                 << ", parentbits=" << loc.selfAndParents
                 << ", #f=" << loc.numFieldsNotCoveredBySubfields
                 << ", #ntf=" << loc.numNonTrivialFieldsNotCovered
                 << ": " << loc.representativeValue;
    ++idx;
  }
}

void MemoryLocations::clear() {
  locations.clear();
  addr2LocIdx.clear();
  nonTrivialLocations.clear();
}

static bool hasInoutArgument(ApplySite AS) {
  for (Operand &op : AS.getArgumentOperands()) {
    switch (AS.getArgumentConvention(op)) {
    case SILArgumentConvention::Indirect_Inout:
    case SILArgumentConvention::Indirect_InoutAliasable:
      return true;
    default:
      break;
    }
  }
  return false;
}

bool MemoryLocations::analyzeLocationUsesRecursively(SILValue V, unsigned locIdx,
                                    SmallVectorImpl<SILValue> &collectedVals,
                                    SubLocationMap &subLocationMap) {
  for (Operand *use : V->getUses()) {
    // We can safely ignore type dependent operands, because the lifetime of a
    // type is decoupled from the lifetime of its value. For example, even if
    // the result of an open_existential_addr is destroyed its type is still
    // valid.
    if (use->isTypeDependent())
      continue;
  
    SILInstruction *user = use->getUser();

    // We only handle addr-instructions which are planned to be used with
    // opaque values. We can still consider to support other addr-instructions
    // like addr-cast instructions. This somehow depends how opaque values will
    // look like.
    switch (user->getKind()) {
      case SILInstructionKind::StructElementAddrInst: {
        auto SEAI = cast<StructElementAddrInst>(user);
        if (!analyzeAddrProjection(SEAI, locIdx, SEAI->getFieldIndex(),
                                collectedVals, subLocationMap))
          return false;
        break;
      }
      case SILInstructionKind::TupleElementAddrInst: {
        auto *TEAI = cast<TupleElementAddrInst>(user);
        if (!analyzeAddrProjection(TEAI, locIdx, TEAI->getFieldIndex(),
                                collectedVals, subLocationMap))
          return false;
        break;
      }
      case SILInstructionKind::BeginAccessInst:
        if (!analyzeLocationUsesRecursively(cast<BeginAccessInst>(user), locIdx,
                                            collectedVals, subLocationMap))
          return false;
        break;
      case SILInstructionKind::InitExistentialAddrInst:
      case SILInstructionKind::OpenExistentialAddrInst:
      case SILInstructionKind::InitEnumDataAddrInst:
      case SILInstructionKind::UncheckedTakeEnumDataAddrInst:
        if (!handleNonTrivialProjections)
          return false;
        // The payload is represented as a single sub-location of the enum.
        if (!analyzeAddrProjection(cast<SingleValueInstruction>(user), locIdx,
                                  /*fieldNr*/ 0, collectedVals, subLocationMap))
          return false;
        break;
      case SILInstructionKind::PartialApplyInst:
        // inout/inout_aliasable conventions means that the argument "escapes".
        // This is okay for memory verification, but cannot handled by other
        // optimizations, like DestroyHoisting.
        if (!handleNonTrivialProjections && hasInoutArgument(ApplySite(user)))
          return false;
        break;
      case SILInstructionKind::LoadBorrowInst:
        // Reborrows are not handled
        if (!cast<LoadBorrowInst>(user)->getUsersOfType<BranchInst>().empty())
          return false;
        break;
      case SILInstructionKind::DebugValueInst:
        if (cast<DebugValueInst>(user)->hasAddrVal())
          break;
        return false;
      case SILInstructionKind::InjectEnumAddrInst:
      case SILInstructionKind::SelectEnumAddrInst:
      case SILInstructionKind::ExistentialMetatypeInst:
      case SILInstructionKind::ValueMetatypeInst:
      case SILInstructionKind::IsUniqueInst:
      case SILInstructionKind::FixLifetimeInst:
      case SILInstructionKind::LoadInst:
      case SILInstructionKind::StoreInst:
      case SILInstructionKind::StoreBorrowInst:
      case SILInstructionKind::EndAccessInst:
      case SILInstructionKind::DestroyAddrInst:
      case SILInstructionKind::CheckedCastAddrBranchInst:
      case SILInstructionKind::UncheckedRefCastAddrInst:
      case SILInstructionKind::UnconditionalCheckedCastAddrInst:
      case SILInstructionKind::ApplyInst:
      case SILInstructionKind::TryApplyInst:
      case SILInstructionKind::BeginApplyInst:
      case SILInstructionKind::CopyAddrInst:
      case SILInstructionKind::YieldInst:
      case SILInstructionKind::DeallocStackInst:
      case SILInstructionKind::SwitchEnumAddrInst:
      case SILInstructionKind::WitnessMethodInst:
        break;
      case SILInstructionKind::MarkUnresolvedMoveAddrInst:
        // We do not want the memory lifetime verifier to verify move_addr inst
        // since the MarkUnresolvedMoveAddrChecker will validate that its uses
        // are correct.
        return false;
      default:
        return false;
    }
  }
  return true;
}

bool MemoryLocations::analyzeAddrProjection(
    SingleValueInstruction *projection, unsigned parentLocIdx,unsigned fieldNr,
    SmallVectorImpl<SILValue> &collectedVals, SubLocationMap &subLocationMap) {

  if (projection->getType().isEmpty(*projection->getFunction()))
    return false;

  auto key = std::make_pair(parentLocIdx, fieldNr);
  unsigned subLocIdx = subLocationMap[key];
  if (subLocIdx == 0) {
    subLocIdx = locations.size();
    assert(subLocIdx > 0);
    subLocationMap[key] = subLocIdx;
    locations.push_back(Location(projection, subLocIdx, parentLocIdx));

    Location &parentLoc = locations[parentLocIdx];
    locations.back().selfAndParents |= parentLoc.selfAndParents;

    int idx = (int)parentLocIdx;
    do {
      Location &loc = locations[idx];
      setBitAndResize(loc.subLocations, subLocIdx);
      idx = loc.parentIdx;
    } while (idx >= 0);

    initFieldsCounter(parentLoc);
    assert(parentLoc.numFieldsNotCoveredBySubfields >= 1);
    parentLoc.updateFieldCounters(projection->getType(), -1);

    if (parentLoc.numFieldsNotCoveredBySubfields == 0) {
      int idx = (int)parentLocIdx;
      do {
        Location &loc = locations[idx];
        loc.subLocations.reset(parentLocIdx);
        idx = loc.parentIdx;
      } while (idx >= 0);
    }
  } else if (!isa<OpenExistentialAddrInst>(projection)) {
    Location *loc = &locations[subLocIdx];
    if (loc->representativeValue->getType() != projection->getType()) {
      assert(isa<InitEnumDataAddrInst>(projection) ||
             isa<UncheckedTakeEnumDataAddrInst>(projection) ||
             isa<InitExistentialAddrInst>(projection));
             
      // We can only handle a single enum payload type for a location or or a
      // single concrete existential type. Mismatching types can have a different
      // number of (non-trivial) sub-locations and we cannot handle this.
      // But we ignore opened existential types, because those cannot have
      // sub-locations (there cannot be an address projection on an
      // open_existential_addr).
      if (!isa<OpenExistentialAddrInst>(loc->representativeValue))
        return false;
      assert(loc->representativeValue->getType().isOpenedExistential());
      loc->representativeValue = projection;
    }
  }

  if (!analyzeLocationUsesRecursively(projection, subLocIdx, collectedVals,
                                      subLocationMap)) {
    return false;
  }
  registerProjection(projection, subLocIdx);
  collectedVals.push_back(projection);
  return true;
}

void MemoryLocations::initFieldsCounter(Location &loc) {
  if (loc.numFieldsNotCoveredBySubfields >= 0)
    return;

  assert(loc.numNonTrivialFieldsNotCovered < 0);

  loc.numFieldsNotCoveredBySubfields = 0;
  loc.numNonTrivialFieldsNotCovered = 0;
  SILFunction *function = loc.representativeValue->getFunction();
  SILType ty = loc.representativeValue->getType();
  if (StructDecl *decl = ty.getStructOrBoundGenericStruct()) {
    if (decl->isResilient(function->getModule().getSwiftModule(),
                          function->getResilienceExpansion())) {
      loc.numFieldsNotCoveredBySubfields = INT_MAX;
      return;
    }
    SILModule &module = function->getModule();
    for (VarDecl *field : decl->getStoredProperties()) {
      loc.updateFieldCounters(
          ty.getFieldType(field, module, TypeExpansionContext(*function)), +1);
    }
    return;
  }
  if (auto tupleTy = ty.getAs<TupleType>()) {
    for (unsigned idx = 0, end = tupleTy->getNumElements(); idx < end; ++idx) {
      loc.updateFieldCounters(ty.getTupleElementType(idx), +1);
    }
    return;
  }
  loc.updateFieldCounters(ty, +1);
}
