//===--- MemAccessUtils.cpp - Utilities for SIL memory access. ------------===//
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

#define DEBUG_TYPE "sil-access-utils"

#include "swift/SIL/MemAccessUtils.h"
#include "swift/SIL/SILModule.h"
#include "swift/SIL/SILUndef.h"
#include "swift/SIL/DynamicCasts.h"
#include "swift/SIL/Consumption.h"
#include "swift/SIL/SILInstruction.h"
#include "llvm/Support/Debug.h"

using namespace swift;

//===----------------------------------------------------------------------===//
//                          MARK: FindAccessVisitor
//===----------------------------------------------------------------------===//

namespace {

enum StorageCastTy { StopAtStorageCast, IgnoreStorageCast };

// Handle a single phi-web within an access use-def chain.
//
// Recursively calls the useDefVisitor on any operations that aren't recognized
// as storage casts or projections. If the useDefVisitor finds a consistent
// result for all operands, then it's result will remain valid. If the
// useDefVisitor has an invalid result after processing the phi web, then it's
// original result is restored, then the phi reported to the useDefVisitor as a
// NonAccess.
//
// Phi-web's are only allowed to contain casts and projections that do not
// affect the access path. If AccessPhiVisitor reaches an unhandled projection,
// it remembers that as the commonDefinition. If after processing the entire
// web, the commonDefinition is unique, then it calls the original useDefVisitor
// to update its result. Note that visitAccessProjection and setDefinition are
// only used by visitors that process access projections; once the accessed
// address is reached, they are no longer relevant.
template <typename UseDefVisitor>
class AccessPhiVisitor
    : public AccessUseDefChainVisitor<AccessPhiVisitor<UseDefVisitor>> {

  UseDefVisitor &useDefVisitor;
  StorageCastTy storageCastTy;

  Optional<SILValue> commonDefinition;
  SmallVector<SILValue, 8> pointerWorklist;
  SmallPtrSet<SILPhiArgument *, 4> nestedPhis;

public:
  AccessPhiVisitor(UseDefVisitor &useDefVisitor, StorageCastTy storageCastTy)
    : useDefVisitor(useDefVisitor), storageCastTy(storageCastTy) {}

  // Main entry point.
  void findPhiAccess(SILPhiArgument *phiArg) && {
    auto savedResult = useDefVisitor.saveResult();
    visitPhi(phiArg);
    while (!pointerWorklist.empty()) {
      this->visit(pointerWorklist.pop_back_val());
    }
    // If a common path component was found, recursively look for the result.
    if (commonDefinition) {
      if (commonDefinition.getValue()) {
        useDefVisitor.reenterUseDef(commonDefinition.getValue());
      } else {
        // Divergent paths were found; invalidate any previously discovered
        // storage.
        useDefVisitor.invalidateResult();
      }
    }
    // If the result is now invalid, reset it and process the current phi as an
    // unrecgonized access instead.
    if (!useDefVisitor.isResultValid()) {
      useDefVisitor.restoreResult(savedResult);
      visitNonAccess(phiArg);
    }
  }

  // Visitor helper.
  void setDefinition(SILValue def) {
    if (!commonDefinition) {
      commonDefinition = def;
      return;
    }
    if (commonDefinition.getValue() != def)
      commonDefinition = SILValue();
  }

  void checkVisitorResult(SILValue result) {
    assert(!result && "must override any visitor that returns a result");
  }

  // MARK: Visitor implementation.

  // Recursively call the original storageVisitor for each base. We can't simply
  // look for a common definition on all phi inputs, because the base may be
  // cloned on each path. For example, two global_addr instructions may refer to
  // the same global storage. Those global_addr instructions may each be
  // converted to a RawPointer before being passed into the non-address phi.
  void visitBase(SILValue base, AccessedStorage::Kind kind) {
    checkVisitorResult(useDefVisitor.visitBase(base, kind));
  }

  void visitNonAccess(SILValue value) {
    checkVisitorResult(useDefVisitor.visitNonAccess(value));
  }

  void visitNestedAccess(BeginAccessInst *access) {
    checkVisitorResult(useDefVisitor.visitNestedAccess(access));
  }

  void visitPhi(SILPhiArgument *phiArg) {
    if (nestedPhis.insert(phiArg).second)
      phiArg->getIncomingPhiValues(pointerWorklist);
  }

  void visitStorageCast(SingleValueInstruction *cast, Operand *sourceOper) {
    // Allow conversions to/from pointers and addresses on disjoint phi paths
    // only if the underlying useDefVisitor allows it.
    if (storageCastTy == IgnoreStorageCast)
      pointerWorklist.push_back(sourceOper->get());
    else
      visitNonAccess(cast);
  }

  void visitAccessProjection(SingleValueInstruction *projectedAddr,
                             Operand *sourceOper) {
    // An offset index on a phi path is always conservatively considered an
    // unknown offset.
    if (isa<IndexAddrInst>(projectedAddr) || isa<TailAddrInst>(projectedAddr)) {
      useDefVisitor.addUnknownOffset();
      pointerWorklist.push_back(sourceOper->get());
      return;
    }
    // No other access projections are expected to occur on disjoint phi
    // paths. Stop searching at this projection.
    setDefinition(projectedAddr);
  }
};

// Find the origin of an access while skipping projections and casts and
// handling phis.
template <typename Impl>
class FindAccessVisitorImpl : public AccessUseDefChainVisitor<Impl, SILValue> {
  using SuperTy = AccessUseDefChainVisitor<Impl, SILValue>;

protected:
  NestedAccessType nestedAccessTy;
  StorageCastTy storageCastTy;

  SmallPtrSet<SILPhiArgument *, 4> visitedPhis;
  bool hasUnknownOffset = false;

public:
  FindAccessVisitorImpl(NestedAccessType nestedAccessTy,
                        StorageCastTy storageCastTy)
      : nestedAccessTy(nestedAccessTy), storageCastTy(storageCastTy) {}

  // MARK: AccessPhiVisitor::UseDefVisitor implementation.
  //
  // Subclasses must implement:
  //   isResultValid()
  //   invalidateResult()
  //   saveResult()
  //   restoreResult(Result)
  //   addUnknownOffset()

  void reenterUseDef(SILValue sourceAddr) {
    SILValue nextAddr = this->visit(sourceAddr);
    while (nextAddr) {
      checkNextAddressType(nextAddr, sourceAddr);
      nextAddr = this->visit(nextAddr);
    }
  }

  // MARK: visitor implementation.

  // Override AccessUseDefChainVisitor to ignore access markers and find the
  // outer access base.
  SILValue visitNestedAccess(BeginAccessInst *access) {
    if (nestedAccessTy == NestedAccessType::IgnoreAccessBegin)
      return access->getSource();

    return SuperTy::visitNestedAccess(access);
  }

  SILValue visitPhi(SILPhiArgument *phiArg) {
    // Cycles involving phis are only handled within AccessPhiVisitor.
    // Path components are not allowed in phi cycles.
    if (visitedPhis.insert(phiArg).second) {
      AccessPhiVisitor<Impl>(this->asImpl(), storageCastTy)
          .findPhiAccess(phiArg);
      // Each phi operand was now reentrantly processed. Stop visiting.
      return SILValue();
    }
    // Cannot treat unresolved phis as "unidentified" because they may alias
    // with global or class access.
    return this->asImpl().visitNonAccess(phiArg);
  }

  SILValue visitStorageCast(SingleValueInstruction *, Operand *sourceAddr) {
    assert(storageCastTy == IgnoreStorageCast);
    return sourceAddr->get();
  }

  SILValue visitAccessProjection(SingleValueInstruction *projectedAddr,
                                 Operand *sourceAddr) {
    if (auto *indexAddr = dyn_cast<IndexAddrInst>(projectedAddr)) {
      if (!Projection(indexAddr).isValid())
        this->asImpl().addUnknownOffset();
    } else if (isa<TailAddrInst>(projectedAddr)) {
      this->asImpl().addUnknownOffset();
    }
    return sourceAddr->get();
  }

protected:
  // Helper for reenterUseDef
  void checkNextAddressType(SILValue nextAddr, SILValue sourceAddr) {
#ifdef NDEBUG
    return;
#endif
    SILType type = nextAddr->getType();
    // FIXME: This relatively expensive pointer getAnyPointerElementType check
    // is only needed because keypath generation incorrectly produces
    // pointer_to_address directly from stdlib Pointer types without a
    // struct_extract (as is correctly done in emitAddressorAccessor), and
    // the PointerToAddressInst operand type is never verified.
    if (type.getASTType()->getAnyPointerElementType())
      return;

    if (type.isAddress() || isa<SILBoxType>(type.getASTType())
        || isa<BuiltinRawPointerType>(type.getASTType())) {
      return;
    }
    llvm::errs() << "Visiting ";
    sourceAddr->print(llvm::errs());
    llvm::errs() << "  not an address ";
    nextAddr->print(llvm::errs());
    nextAddr->getFunction()->print(llvm::errs());
    assert(false);
  }
};

// Implement getAccessAddress, getAccessBegin, and getAccessBase.
class FindAccessBaseVisitor
    : public FindAccessVisitorImpl<FindAccessBaseVisitor> {
  using SuperTy = FindAccessVisitorImpl<FindAccessBaseVisitor>;

protected:
  Optional<SILValue> base;

public:
  FindAccessBaseVisitor(NestedAccessType nestedAccessTy,
                        StorageCastTy storageCastTy)
      : FindAccessVisitorImpl(nestedAccessTy, storageCastTy) {}

  // Returns the accessed address or an invalid SILValue.
  SILValue findBase(SILValue sourceAddr) && {
    reenterUseDef(sourceAddr);
    return base.getValueOr(SILValue());
  }

  void setResult(SILValue foundBase) {
    if (!base)
      base = foundBase;
    else if (base.getValue() != foundBase)
      base = SILValue();
  }

  // MARK: AccessPhiVisitor::UseDefVisitor implementation.

  bool isResultValid() const { return base && bool(base.getValue()); }

  void invalidateResult() { base = SILValue(); }

  Optional<SILValue> saveResult() const { return base; }

  void restoreResult(Optional<SILValue> result) { base = result; }

  void addUnknownOffset() { return; }

  // MARK: visitor implementation.

  SILValue visitBase(SILValue base, AccessedStorage::Kind kind) {
    setResult(base);
    return SILValue();
  }

  SILValue visitNonAccess(SILValue value) {
    setResult(value);
    return SILValue();
  }

  // Override visitStorageCast to avoid seeing through arbitrary address casts.
  SILValue visitStorageCast(SingleValueInstruction *cast, Operand *sourceAddr) {
    if (storageCastTy == StopAtStorageCast)
      return visitNonAccess(cast);

    return SuperTy::visitStorageCast(cast, sourceAddr);
  }
};

} // end anonymous namespace

//===----------------------------------------------------------------------===//
//                            MARK: Standalone API
//===----------------------------------------------------------------------===//

SILValue swift::getTypedAccessAddress(SILValue address) {
  assert(address->getType().isAddress());
  SILValue accessAddress =
      FindAccessBaseVisitor(NestedAccessType::StopAtAccessBegin,
                            StopAtStorageCast)
          .findBase(address);
  assert(accessAddress->getType().isAddress());
  return accessAddress;
}

// TODO: When the optimizer stops stripping begin_access markers and SILGen
// protects all memory operations with at least an "unsafe" access scope, then
// we should be able to assert that this returns a BeginAccessInst.
SILValue swift::getAccessScope(SILValue address) {
  assert(address->getType().isAddress());
  return FindAccessBaseVisitor(NestedAccessType::StopAtAccessBegin,
                               IgnoreStorageCast)
      .findBase(address);
}

// This is allowed to be called on a non-address pointer type.
SILValue swift::getAccessBase(SILValue address) {
  return FindAccessBaseVisitor(NestedAccessType::IgnoreAccessBegin,
                               IgnoreStorageCast)
      .findBase(address);
}

bool swift::isLetAddress(SILValue address) {
  SILValue base = getAccessBase(address);
  if (!base)
    return false;

  // Is this an address of a "let" class member?
  if (auto *rea = dyn_cast<RefElementAddrInst>(base))
    return rea->getField()->isLet();

  // Is this an address of a global "let"?
  if (auto *gai = dyn_cast<GlobalAddrInst>(base)) {
    auto *globalDecl = gai->getReferencedGlobal()->getDecl();
    return globalDecl && globalDecl->isLet();
  }
  return false;
}

//===----------------------------------------------------------------------===//
//                          MARK: FindReferenceRoot
//===----------------------------------------------------------------------===//

// On some platforms, casting from a metatype to a reference type dynamically
// allocates a ref-counted box for the metatype. Naturally that is the place
// where RC-identity begins. Considering the source of such a casts to be
// RC-identical would confuse ARC optimization, which might eliminate a retain
// of such an object completely.
//
// The SILVerifier checks that none of these operations cast a trivial value to
// a reference except unconditional_checked_cast[_value], which is checked By
// SILDynamicCastInst::isRCIdentityPreserving().
bool swift::isRCIdentityPreservingCast(SingleValueInstruction *svi) {
  switch (svi->getKind()) {
  default:
    return false;
  // Ignore ownership casts
  case SILInstructionKind::CopyValueInst:
  case SILInstructionKind::BeginBorrowInst:
  // Ignore class type casts
  case SILInstructionKind::UpcastInst:
  case SILInstructionKind::UncheckedRefCastInst:
  case SILInstructionKind::RefToBridgeObjectInst:
  case SILInstructionKind::BridgeObjectToRefInst:
    return true;
  case SILInstructionKind::UnconditionalCheckedCastInst:
  case SILInstructionKind::UnconditionalCheckedCastValueInst:
    return SILDynamicCastInst(svi).isRCIdentityPreserving();
  }
}

namespace {

// Essentially RC identity where the starting point is already a reference.
class FindReferenceRoot {
  SmallPtrSet<SILPhiArgument *, 4> visitedPhis;

public:
  SILValue findRoot(SILValue ref) && {
    SILValue root = recursiveFindRoot(ref);
    assert(root && "all phi inputs must be reachable");
    return root;
  }

protected:
  // Return an invalid value for a phi with no resolved inputs.
  SILValue recursiveFindRoot(SILValue ref) {
    while (auto *svi = dyn_cast<SingleValueInstruction>(ref)) {
      if (!isRCIdentityPreservingCast(svi)) {
        break;
      }
      ref = svi->getOperand(0);
    };
    auto *phi = dyn_cast<SILPhiArgument>(ref);
    if (!phi || !phi->isPhiArgument()) {
      return ref;
    }
    // Handle phis...
    if (!visitedPhis.insert(phi).second) {
      return SILValue();
    }
    SILValue commonInput;
    phi->visitIncomingPhiOperands([&](Operand *operand) {
      SILValue input = recursiveFindRoot(operand->get());
      // Ignore "back/cross edges" to previously visited phis.
      if (!input)
        return true;

      if (!commonInput) {
        commonInput = input;
        return true;
      }
      if (commonInput == input)
        return true;

      commonInput = phi;
      return false;
    });
    return commonInput;
  }
};

} // end anonymous namespace

static SILValue findReferenceRoot(SILValue ref) {
  return FindReferenceRoot().findRoot(ref);
}

//===----------------------------------------------------------------------===//
//                            MARK: AccessedStorage
//===----------------------------------------------------------------------===//

SILGlobalVariable *getReferencedGlobal(SILInstruction *inst) {
  if (auto *gai = dyn_cast<GlobalAddrInst>(inst)) {
    return gai->getReferencedGlobal();
  }
  if (auto apply = FullApplySite::isa(inst)) {
    if (auto *funcRef = apply.getReferencedFunctionOrNull()) {
      return getVariableOfGlobalInit(funcRef);
    }
  }
  return nullptr;
}

constexpr unsigned AccessedStorage::TailIndex;

AccessedStorage::AccessedStorage(SILValue base, Kind kind) {
  // For kind==Unidentified, base may be an invalid empty or tombstone value.
  assert(base && "invalid storage base");
  initKind(kind);
  switch (kind) {
  case Box:
    assert(isa<AllocBoxInst>(base));
    value = base;
    break;
  case Stack:
    assert(isa<AllocStackInst>(base));
    value = base;
    break;
  case Nested:
    assert(isa<BeginAccessInst>(base));
    value = base;
    break;
  case Yield:
    assert(isa<BeginApplyInst>(
             cast<MultipleValueInstructionResult>(base)->getParent()));
    value = base;
    break;
  case Unidentified:
    value = base;
    break;
  case Argument:
    value = base;
    setElementIndex(cast<SILFunctionArgument>(base)->getIndex());
    break;
  case Global:
    global = getReferencedGlobal(cast<SingleValueInstruction>(base));
    // Require a decl for all formally accessed globals defined in this
    // module. AccessEnforcementWMO requires this. Swift globals defined in
    // another module either use an addressor, which has Unidentified
    // storage. Imported non-Swift globals are accessed via global_addr but have
    // no declaration.
    assert(global->getDecl() || isa<GlobalAddrInst>(base));
    break;
  case Class: {
    // Do a best-effort to find the identity of the object being projected
    // from. It is OK to be unsound here (i.e. miss when two ref_element_addrs
    // actually refer the same address) because, when the effort fails, static
    // analysis will be sufficiently conservative given that classes are not
    // "uniquely identified", and these addresses will be dynamically checked.
    auto *REA = cast<RefElementAddrInst>(base);
    value = findReferenceRoot(REA->getOperand());
    setElementIndex(REA->getFieldIndex());
    break;
  }
  case Tail: {
    auto *RTA = cast<RefTailAddrInst>(base);
    value = findReferenceRoot(RTA->getOperand());
    break;
  }
  }
  setLetAccess(base);
}

void AccessedStorage::visitRoots(
    SILFunction *function,
    llvm::function_ref<bool(SILValue)> visitor) const {
  if (SILValue root = getRoot()) {
    visitor(root);
    return;
  }
  if (getKind() == Unidentified) {
    return;
  }
  assert(getKind() == Global && function);
  SILGlobalVariable *global = getGlobal();
  for (auto &block : *function) {
    for (auto &instruction : block) {
      if (global == getReferencedGlobal(&instruction)) {
        visitor(cast<SingleValueInstruction>(&instruction));
      }
    }
  }
}

// Set 'isLet' to true if this storage can be determined to be a 'let' variable.
//
// \p base must be the access base for this storage, as passed to the
// AccessedStorage constructor.
void AccessedStorage::setLetAccess(SILValue base) {
  // It's unclear whether a global will ever be missing it's varDecl, but
  // technically we only preserve it for debug info. So if we don't have a decl,
  // check the flag on SILGlobalVariable, which is guaranteed valid,
  if (getKind() == AccessedStorage::Global) {
    Bits.AccessedStorage.isLet = getGlobal()->isLet();
    return;
  }
  if (auto *decl = dyn_cast_or_null<VarDecl>(getDecl(base))) {
    Bits.AccessedStorage.isLet = decl->isLet();
  }
}

const ValueDecl *AccessedStorage::getDecl(SILValue base) const {
  switch (getKind()) {
  case Box:
    return cast<AllocBoxInst>(value)->getLoc().getAsASTNode<VarDecl>();

  case Stack:
    return cast<AllocStackInst>(value)->getDecl();

  case Global:
    return global->getDecl();

  case Class: {
    // The property index is relative to the VarDecl in ref_element_addr, and
    // can only be reliably determined when the base is avaiable. Otherwise, we
    // can only make a best effort to extract it from the object type, which
    // might not even be a class in the case of bridge objects.
    if (ClassDecl *classDecl =
            base ? cast<RefElementAddrInst>(base)->getClassDecl()
                 : getObject()->getType().getClassOrBoundGenericClass()) {
      return getIndexedField(classDecl, getPropertyIndex());
    }
    return nullptr;
  }
  case Tail:
    return nullptr;

  case Argument:
    return getArgument()->getDecl();

  case Yield:
    return nullptr;

  case Nested:
    return nullptr;

  case Unidentified:
    return nullptr;
  }
  llvm_unreachable("unhandled kind");
}

const char *AccessedStorage::getKindName(AccessedStorage::Kind k) {
  switch (k) {
  case Box:
    return "Box";
  case Stack:
    return "Stack";
  case Nested:
    return "Nested";
  case Unidentified:
    return "Unidentified";
  case Argument:
    return "Argument";
  case Yield:
    return "Yield";
  case Global:
    return "Global";
  case Class:
    return "Class";
  case Tail:
    return "Tail";
  }
  llvm_unreachable("unhandled kind");
}

void AccessedStorage::print(raw_ostream &os) const {
  if (!*this) {
    os << "INVALID\n";
    return;
  }
  os << getKindName(getKind()) << " ";
  switch (getKind()) {
  case Box:
  case Stack:
  case Nested:
  case Yield:
  case Unidentified:
    os << value;
    break;
  case Argument:
    os << value;
    break;
  case Global:
    os << *global;
    break;
  case Class:
    os << getObject();
    if (auto *decl = getDecl()) {
      os << "  Field: ";
      decl->print(os);
    }
    os << " Index: " << getPropertyIndex() << "\n";
    break;
  case Tail:
    os << getObject();
  }
}

LLVM_ATTRIBUTE_USED void AccessedStorage::dump() const { print(llvm::dbgs()); }

namespace {

// Implementation of AccessUseDefChainVisitor that looks for a single common
// AccessedStorage object for all projection paths.
class FindAccessedStorageVisitor
    : public FindAccessVisitorImpl<FindAccessedStorageVisitor> {

public:
  struct Result {
    Optional<AccessedStorage> storage;
    SILValue base;
  };

private:
  Result result;

  void setResult(AccessedStorage foundStorage, SILValue foundBase) {
    if (!result.storage) {
      result.storage = foundStorage;
      assert(!result.base);
      result.base = foundBase;
    } else {
      // `storage` may still be invalid. If both `storage` and `foundStorage`
      // are invalid, this check passes, but we return an invalid storage
      // below.
      if (!result.storage->hasIdenticalBase(foundStorage))
        result.storage = AccessedStorage();
      if (result.base != foundBase)
        result.base = SILValue();
    }
  }

public:
  FindAccessedStorageVisitor(NestedAccessType nestedAccessTy)
      : FindAccessVisitorImpl(nestedAccessTy, IgnoreStorageCast) {}

  // Main entry point
  void findStorage(SILValue sourceAddr) { this->reenterUseDef(sourceAddr); }

  AccessedStorage getStorage() const {
    return result.storage.getValueOr(AccessedStorage());
  }
  // getBase may return an invalid value for valid Global storage because there
  // may be multiple global_addr bases for identical storage.
  SILValue getBase() const { return result.base; }

  // MARK: AccessPhiVisitor::UseDefVisitor implementation.

  // A valid result requires valid storage, but not a valid base.
  bool isResultValid() const {
    return result.storage && bool(result.storage.getValue());
  }

  void invalidateResult() { setResult(AccessedStorage(), SILValue()); }

  Result saveResult() const { return result; }

  void restoreResult(Result savedResult) { result = savedResult; }

  void addUnknownOffset() { return; }

  // MARK: visitor implementation.

  SILValue visitBase(SILValue base, AccessedStorage::Kind kind) {
    setResult(AccessedStorage(base, kind), base);
    return SILValue();
  }

  SILValue visitNonAccess(SILValue value) {
    invalidateResult();
    return SILValue();
  }
};

} // end anonymous namespace

AccessedStorageWithBase
AccessedStorageWithBase::compute(SILValue sourceAddress) {
  FindAccessedStorageVisitor visitor(NestedAccessType::IgnoreAccessBegin);
  visitor.findStorage(sourceAddress);
  return {visitor.getStorage(), visitor.getBase()};
}

AccessedStorageWithBase
AccessedStorageWithBase::computeInScope(SILValue sourceAddress) {
  FindAccessedStorageVisitor visitor(NestedAccessType::StopAtAccessBegin);
  visitor.findStorage(sourceAddress);
  return {visitor.getStorage(), visitor.getBase()};
}

AccessedStorage AccessedStorage::compute(SILValue sourceAddress) {
  return AccessedStorageWithBase::compute(sourceAddress).storage;
}

AccessedStorage AccessedStorage::computeInScope(SILValue sourceAddress) {
  return AccessedStorageWithBase::computeInScope(sourceAddress).storage;
}

//===----------------------------------------------------------------------===//
//                              MARK: AccessPath
//===----------------------------------------------------------------------===//

AccessPath AccessPath::forTailStorage(SILValue rootReference) {
  return AccessPath(
    AccessedStorage::forClass(rootReference, AccessedStorage::TailIndex),
    PathNode(rootReference->getModule()->getIndexTrieRoot()),
    /*offset*/ 0);
}

bool AccessPath::contains(AccessPath subPath) const {
  if (!isValid() || !subPath.isValid()) {
    return false;
  }
  if (!storage.hasIdenticalBase(subPath.storage)) {
    return false;
  }
  // Does the offset index match?
  if (offset != subPath.offset || offset == UnknownOffset) {
    return false;
  }
  return pathNode.node->isPrefixOf(subPath.pathNode.node);
}

bool AccessPath::mayOverlap(AccessPath otherPath) const {
  if (!isValid() || !otherPath.isValid())
    return true;

  if (storage.isDistinctFrom(otherPath.storage)) {
    return false;
  }
  // If subpaths are disjoint, they do not overlap regardless of offset.
  if (!pathNode.node->isPrefixOf(otherPath.pathNode.node)
      && !otherPath.pathNode.node->isPrefixOf(pathNode.node)) {
    return true;
  }
  return offset == otherPath.offset || offset == UnknownOffset
         || otherPath.offset == UnknownOffset;
}

namespace {

// Implementation of AccessUseDefChainVisitor that builds an AccessPath.
class AccessPathVisitor : public FindAccessVisitorImpl<AccessPathVisitor> {
  using SuperTy = FindAccessVisitorImpl<AccessPathVisitor>;

  SILModule *module;

  // This nested visitor holds the AccessedStorage and base results.
  FindAccessedStorageVisitor storageVisitor;

  // Save just enough information for to checkpoint before processing phis. Phis
  // can add path components and add an unknown offset.
  struct Result {
    FindAccessedStorageVisitor::Result storageResult;
    int savedOffset;
    unsigned pathLength;

    Result(FindAccessedStorageVisitor::Result storageResult, int offset,
           unsigned pathLength)
        : storageResult(storageResult), savedOffset(offset),
          pathLength(pathLength) {}
  };

  // Only access projections affect this path. Since they are are not allowed
  // beyond phis, this path is not part of AccessPathVisitor::Result.
  llvm::SmallVector<AccessPath::Index, 8> reversePath;
  // Holds a non-zero value if an index_addr has been processed without yet
  // creating a path index for it.
  int pendingOffset = 0;

public:
  AccessPathVisitor(SILModule *module, NestedAccessType nestedAccessTy)
      : FindAccessVisitorImpl(nestedAccessTy, IgnoreStorageCast),
        module(module), storageVisitor(NestedAccessType::IgnoreAccessBegin) {}

  // Main entry point.
  AccessPathWithBase findAccessPath(SILValue sourceAddr) && {
    this->reenterUseDef(sourceAddr);
    if (auto storage = storageVisitor.getStorage()) {
      return AccessPathWithBase(
          AccessPath(storage, computeForwardPath(), pendingOffset),
          storageVisitor.getBase());
    }
    return AccessPathWithBase(AccessPath(), SILValue());
  }

protected:
  void addPathOffset(int offset) {
    if (pendingOffset == AccessPath::UnknownOffset)
      return;

    if (offset == AccessPath::UnknownOffset) {
      pendingOffset = offset;
      return;
    }
    // Accumulate static offsets
    pendingOffset = pendingOffset + offset;
  }

  // Return the trie node corresponding to the current state of reversePath.
  AccessPath::PathNode computeForwardPath() {
    IndexTrieNode *forwardPath = module->getIndexTrieRoot();
    for (AccessPath::Index nextIndex : llvm::reverse(reversePath)) {
      forwardPath = forwardPath->getChild(nextIndex.getEncoding());
    }
    return AccessPath::PathNode(forwardPath);
  }

public:
  // MARK: AccessPhiVisitor::UseDefVisitor implementation.

  bool isResultValid() const { return storageVisitor.isResultValid(); }

  void invalidateResult() {
    storageVisitor.invalidateResult();
    // Don't clear reversePath. We my call restoreResult later.
    pendingOffset = 0;
  }

  Result saveResult() const {
    return Result(storageVisitor.saveResult(), pendingOffset,
                  reversePath.size());
  }

  void restoreResult(Result result) {
    storageVisitor.restoreResult(result.storageResult);
    pendingOffset = result.savedOffset;
    assert(result.pathLength <= reversePath.size()
           && "a phi should only add to the path");
    reversePath.erase(reversePath.begin() + result.pathLength,
                      reversePath.end());
  }

  void addUnknownOffset() { pendingOffset = AccessPath::UnknownOffset; }

  // MARK: visitor implementation. Return the address source as the next use-def
  // value to process. An invalid SILValue stops def-use traversal.

  SILValue visitBase(SILValue base, AccessedStorage::Kind kind) {
    return storageVisitor.visitBase(base, kind);
  }

  SILValue visitNonAccess(SILValue value) {
    invalidateResult();
    return SILValue();
  }

  // Override FindAccessVisitorImpl to record path components.
  SILValue visitAccessProjection(SingleValueInstruction *projectedAddr,
                                 Operand *sourceAddr) {
    auto projIdx = ProjectionIndex(projectedAddr);
    if (auto *indexAddr = dyn_cast<IndexAddrInst>(projectedAddr)) {
      addPathOffset(projIdx.isValid() ? projIdx.Index
                                      : AccessPath::UnknownOffset);
    } else if (isa<TailAddrInst>(projectedAddr)) {
      addPathOffset(AccessPath::UnknownOffset);
    } else if (projIdx.isValid()) {
      if (pendingOffset) {
        LLVM_DEBUG(llvm::dbgs() << "Subobject projection with offset index: "
                                << *projectedAddr);
        // Return an invalid result even though findAccessedStorage() may be
        // able to find valid storage, because an offset from a subobject is an
        // invalid access path.
        return visitNonAccess(projectedAddr);
      }
      reversePath.push_back(
          AccessPath::Index::forSubObjectProjection(projIdx.Index));
    } else {
      // Ignore everything in getAccessProjectionOperand that is an access
      // projection with no affect on the access path.
      assert(isa<OpenExistentialAddrInst>(projectedAddr)
             || isa<InitEnumDataAddrInst>(projectedAddr)
             || isa<UncheckedTakeEnumDataAddrInst>(projectedAddr)
             || isa<ProjectBoxInst>(projectedAddr));
    }
    return sourceAddr->get();
  }
};

} // end anonymous namespace

AccessPathWithBase AccessPathWithBase::compute(SILValue address) {
  return AccessPathVisitor(address->getModule(),
                           NestedAccessType::IgnoreAccessBegin)
      .findAccessPath(address);
}

AccessPathWithBase AccessPathWithBase::computeInScope(SILValue address) {
  return AccessPathVisitor(address->getModule(),
                           NestedAccessType::StopAtAccessBegin)
      .findAccessPath(address);
}

void AccessPath::Index::print(raw_ostream &os) const {
  if (isSubObjectProjection())
    os << '#' << getSubObjectIndex();
  else {
    os << '@';
    if (isUnknownOffset())
      os << "Unknown";
    else
      os << getOffset();
  }
}

LLVM_ATTRIBUTE_USED void AccessPath::Index::dump() const {
  print(llvm::dbgs());
}

static void recursivelyPrintPath(AccessPath::PathNode node, raw_ostream &os) {
  AccessPath::PathNode parent = node.getParent();
  if (!parent.isRoot()) {
    recursivelyPrintPath(parent, os);
    os << ",";
  }
  node.getIndex().print(os);
}

void AccessPath::printPath(raw_ostream &os) const {
  os << "Path: ";
  if (!isValid()) {
    os << "INVALID\n";
    return;
  }
  os << "(";
  PathNode node = getPathNode();
  if (offset != 0) {
    Index::forOffset(offset).print(os);
    if (!node.isRoot())
      os << ",";
  }
  if (!node.isRoot())
    recursivelyPrintPath(node, os);
  os << ")\n";
}

void AccessPath::print(raw_ostream &os) const {
  if (!isValid()) {
    os << "INVALID\n";
    return;
  }
  os << "Storage: ";
  getStorage().print(os);
  printPath(os);
}

LLVM_ATTRIBUTE_USED void AccessPath::dump() const { print(llvm::dbgs()); }

void AccessPathWithBase::print(raw_ostream &os) const {
  if (base)
    os << "Base: " << base;

  accessPath.print(os);
}

LLVM_ATTRIBUTE_USED void AccessPathWithBase::dump() const {
  print(llvm::dbgs());
}

//===----------------------------------------------------------------------===//
//                      MARK: AccessPathDefUseTraversal
//===----------------------------------------------------------------------===//

namespace {

// Perform def-use DFS traversal along a given AccessPath. DFS terminates at
// each discovered use.
//
// For useTy == Exact, the collected uses all have the same AccessPath.
// Subobject projections within that access path and their transitive uses are
// not included.
//
// For useTy == Inner, the collected uses to subobjects contained by the
// current access path.
//
// For useTy == Overlapping, the collected uses also include uses that
// access an object that contains the given AccessPath as well as uses at
// an unknown offset relative to the current path.
//
// Example, where AccessPath == (#2):
//   %base = ...                            // access base
//   load %base                             // containing use
//   %elt1 = struct_element_addr %base, #1  // non-use (ignored)
//   load %elt1                             // non-use (unseen)
//   %elt2 = struct_element_addr %base, #2  // outer projection (followed)
//   load %elt2                             // exact use
//   %sub = struct_element_addr %elt2,  #i  // inner projection (followed)
//   load %sub                              // inner use
//
// A use may be a BranchInst if the corresponding phi does not have common
// AccessedStorage.
//
// For class storage, the def-use traversal starts at the reference
// root. Eventually, traversal reach the base address of the formal access:
//
//   %ref = ...                        // reference root
//   %base = ref_element_addr %refRoot // formal access address
//   load %base                        // use
class AccessPathDefUseTraversal {
  AccessUseVisitor &visitor;

  // The origin of the def-use traversal.
  AccessedStorage storage;

  // Remaining access path indices from the most recently visited def to any
  // exact use in def-use order.
  SmallVector<AccessPath::Index, 4> pathIndices;

  // A point in the def-use traversal. isRef() is true only for object access
  // prior to reaching the base address.
  struct DFSEntry {
    // Next potential use to visit and flag indicating whether traversal has
    // reachaed the access base yet.
    llvm::PointerIntPair<Operand *, 1, bool> useAndIsRef;
    int pathCursor; // position within pathIndices
    int offset;     // index_addr offsets seen prior to this use

    DFSEntry(Operand *use, bool isRef, int pathCursor, int offset)
        : useAndIsRef(use, isRef), pathCursor(pathCursor), offset(offset) {}

    Operand *getUse() const { return useAndIsRef.getPointer(); }
    // Is this pointer a reference?
    bool isRef() const { return useAndIsRef.getInt(); }
  };
  SmallVector<DFSEntry, 16> dfsStack;

  SmallPtrSet<const SILPhiArgument *, 4> visitedPhis;

  // Transient traversal data should not be copied.
  AccessPathDefUseTraversal(const AccessPathDefUseTraversal &) = delete;
  AccessPathDefUseTraversal &
  operator=(const AccessPathDefUseTraversal &) = delete;

public:
  AccessPathDefUseTraversal(AccessUseVisitor &visitor, AccessPath accessPath,
                            SILFunction *function)
    : visitor(visitor), storage(accessPath.getStorage()) {
    assert(accessPath.isValid());

    initializePathIndices(accessPath);

    storage.visitRoots(function, [this](SILValue root) {
      initializeDFS(root);
      return true;
    });
  }

  // Return true is all uses have been visited.
  bool visitUses() {
    // Return false if initialization failed.
    if (!storage) {
      return false;
    }
    while (!dfsStack.empty()) {
      if (!visitUser(dfsStack.pop_back_val()))
        return false;
    }
    return true;
  }

protected:
  void initializeDFS(SILValue root) {
    // If root is a phi, record it so that its uses aren't visited twice.
    if (auto *phi = dyn_cast<SILPhiArgument>(root)) {
      if (phi->isPhiArgument())
        visitedPhis.insert(phi);
    }
    pushUsers(root,
              DFSEntry(nullptr, storage.isReference(), pathIndices.size(), 0));
  }

  void pushUsers(SILValue def, const DFSEntry &dfs) {
    for (auto *use : def->getUses())
      pushUser(DFSEntry(use, dfs.isRef(), dfs.pathCursor, dfs.offset));
  }

  void pushUser(DFSEntry dfs) {
    Operand *use = dfs.getUse();
    if (auto *bi = dyn_cast<BranchInst>(use->getUser())) {
      if (pushPhiUses(bi->getArgForOperand(use), dfs))
        return;
    }
    // If we didn't find and process a phi, continue DFS.
    dfsStack.emplace_back(dfs);
  }

  bool pushPhiUses(const SILPhiArgument *phi, DFSEntry dfs);

  void initializePathIndices(AccessPath accessPath);

  // Return the offset at the current DFS path cursor, or zero.
  int getPathOffset(const DFSEntry &dfs) const;

  // Return true if the accumulated offset matches the current path index.
  // Update the DFSEntry and pathCursor to skip remaining offsets.
  bool checkAndUpdateOffset(DFSEntry &dfs);

  // Handle non-index_addr projections.
  void followProjection(SingleValueInstruction *svi, DFSEntry dfs);

  enum UseKind { LeafUse, IgnoredUse };
  UseKind visitSingleValueUser(SingleValueInstruction *svi, DFSEntry dfs);

  // Returns true as long as the visitor returns true.
  bool visitUser(DFSEntry dfs);
};

} // end anonymous namespace

// Initialize the array of remaining path indices.
void AccessPathDefUseTraversal::initializePathIndices(AccessPath accessPath) {
  for (AccessPath::PathNode currentNode = accessPath.getPathNode();
       !currentNode.isRoot(); currentNode = currentNode.getParent()) {
    assert(currentNode.getIndex().isSubObjectProjection()
           && "a valid AccessPath does not contain any intermediate offsets");
    pathIndices.push_back(currentNode.getIndex());
  }
  if (int offset = accessPath.getOffset()) {
    pathIndices.push_back(AccessPath::Index::forOffset(offset));
  }
  // The search will start from the object root, not the formal access base,
  // so add the class index to the front.
  if (storage.getKind() == AccessedStorage::Class) {
    pathIndices.push_back(
        AccessPath::Index::forSubObjectProjection(storage.getPropertyIndex()));
  }
  if (storage.getKind() == AccessedStorage::Tail) {
    pathIndices.push_back(
        AccessPath::Index::forSubObjectProjection(ProjectionIndex::TailIndex));
  }
  // If the expected path has an unknown offset, then none of the uses are
  // exact.
  if (!visitor.findOverlappingUses() && !pathIndices.empty()
      && pathIndices.back().isUnknownOffset()) {
    return;
  }
}

// Return true if this phi has been processed and does not need to be
// considered as a separate use.
bool AccessPathDefUseTraversal::pushPhiUses(const SILPhiArgument *phi,
                                            DFSEntry dfs) {
  if (!visitedPhis.insert(phi).second)
    return true;

  // If this phi has a common base, continue to follow the access path. This
  // check is different for reference types vs pointer types.
  if (dfs.isRef()) {
    assert(!dfs.offset && "index_addr not allowed on reference roots");
    // When isRef is true, the address access hasn't been seen yet and
    // we're still following the reference root's users. Check if all phi
    // inputs have the same reference root before looking through it.
    if (findReferenceRoot(phi) == storage.getObject()) {
      pushUsers(phi, dfs);
      return true;
    }
    // The branch will be pushed onto the normal user list.
    return false;
  }
  // Check if all phi inputs have the same accessed storage before
  // looking through it. If the phi input differ the its storage is invalid.
  auto phiPath = AccessPath::compute(phi);
  if (phiPath.isValid()) {
    assert(phiPath.getStorage().hasIdenticalBase(storage)
           && "inconsistent phi storage");
    // If the phi paths have different offsets, its path has unknown offset.
    if (phiPath.getOffset() == AccessPath::UnknownOffset) {
      if (!visitor.findOverlappingUses())
        return true;
      dfs.offset = AccessPath::UnknownOffset;
    }
    pushUsers(phi, dfs);
    return true;
  }
  // The branch will be pushed onto the normal user list.
  return false;
}

// Return the offset at the current DFS path cursor, or zero.
int AccessPathDefUseTraversal::getPathOffset(const DFSEntry &dfs) const {
  if (dfs.pathCursor <= 0
      || pathIndices[dfs.pathCursor - 1].isSubObjectProjection()) {
    return 0;
  }
  return pathIndices[dfs.pathCursor - 1].getOffset();
}

// Return true if the accumulated offset matches the current path index.
// Update the DFSEntry and pathCursor to skip remaining offsets.
bool AccessPathDefUseTraversal::checkAndUpdateOffset(DFSEntry &dfs) {
  int pathOffset = getPathOffset(dfs);
  if (dfs.offset == AccessPath::UnknownOffset) {
    if (pathOffset > 0) {
      // Pop the offset from the expected path; there should only be
      // one. Continue matching subobject indices even after seeing an unknown
      // offset. A subsequent mismatching subobject index is still considered
      // non-overlapping. This is valid for aliasing since an offset from a
      // subobject is considered an invalid access path.
      --dfs.pathCursor;
      assert(getPathOffset(dfs) == 0 && "only one offset index allowed");
    }
    // Continue searching only if we need to find overlapping uses. Preserve the
    // unknown dfs offset so we don't consider any dependent operations to be
    // exact or inner uses.
    return visitor.findOverlappingUses();
  }
  if (pathOffset == 0) {
    return dfs.offset == 0;
  }
  // pop the offset from the expected path; there should only be one.
  --dfs.pathCursor;
  assert(getPathOffset(dfs) == 0 && "only one offset index allowed");

  // Ignore all uses on this path unless we're collecting containing uses.
  // UnknownOffset appears to overlap with all offsets and subobject uses.
  if (pathOffset == AccessPath::UnknownOffset) {
    // Set the dfs offset to unknown to avoid considering any dependent
    // operations as exact or inner uses.
    dfs.offset = AccessPath::UnknownOffset;
    return visitor.findOverlappingUses();
  }
  int useOffset = dfs.offset;
  dfs.offset = 0;
  // A known offset must match regardless of findOverlappingUses.
  return pathOffset == useOffset;
}

// Handle non-index_addr projections.
void AccessPathDefUseTraversal::followProjection(SingleValueInstruction *svi,
                                                 DFSEntry dfs) {
  if (!checkAndUpdateOffset(dfs)) {
    return;
  }
  if (dfs.pathCursor <= 0) {
    if (visitor.useTy == AccessUseType::Exact) {
      assert(dfs.pathCursor == 0);
      return;
    }
    --dfs.pathCursor;
    pushUsers(svi, dfs);
    return;
  }
  AccessPath::Index pathIndex = pathIndices[dfs.pathCursor - 1];
  auto projIdx = ProjectionIndex(svi);
  assert(projIdx.isValid());
  // Only subobjects indices are expected because offsets are handled above.
  if (projIdx.Index == pathIndex.getSubObjectIndex()) {
    --dfs.pathCursor;
    pushUsers(svi, dfs);
  }
  return;
}

// During the def-use traversal, visit a single-value instruction in which the
// used address is at operand zero.
//
// This must handle the def-use side of all operations that
// AccessUseDefChainVisitor::visit can handle.
//
// Return IgnoredUse if the def-use traversal either continues past \p
// svi or ignores this use.
//
// FIXME: Reuse getAccessProjectionOperand() instead of using special cases once
// the unchecked_take_enum_data_addr -> load -> project_box pattern is fixed.
AccessPathDefUseTraversal::UseKind
AccessPathDefUseTraversal::visitSingleValueUser(SingleValueInstruction *svi,
                                                DFSEntry dfs) {
  if (dfs.isRef()) {
    if (isRCIdentityPreservingCast(svi)) {
      pushUsers(svi, dfs);
      return IgnoredUse;
    }
    // 'svi' will be processed below as either RefElementAddrInst,
    // RefTailAddrInst, or some unknown LeafUse.
  } else if (isAccessedStorageCast(svi)) {
    pushUsers(svi, dfs);
    return IgnoredUse;
  }
  switch (svi->getKind()) {
  default:
    return LeafUse;

  case SILInstructionKind::BeginAccessInst:
    if (visitor.nestedAccessTy == NestedAccessType::StopAtAccessBegin) {
      return LeafUse;
    }
    pushUsers(svi, dfs);
    return IgnoredUse;

  // Handle ref_element_addr since we start at the object root instead of
  // the access base.
  case SILInstructionKind::RefElementAddrInst:
    assert(dfs.isRef());
    assert(dfs.pathCursor > 0 && "ref_element_addr cannot occur within access");
    dfs.useAndIsRef.setInt(false);
    followProjection(svi, dfs);
    return IgnoredUse;

  case SILInstructionKind::RefTailAddrInst: {
    assert(dfs.isRef());
    assert(dfs.pathCursor > 0 && "ref_tail_addr cannot occur within an access");
    dfs.useAndIsRef.setInt(false);
    --dfs.pathCursor;
    AccessPath::Index pathIndex = pathIndices[dfs.pathCursor];
    assert(pathIndex.isSubObjectProjection());
    if (pathIndex.getSubObjectIndex() == AccessedStorage::TailIndex)
      pushUsers(svi, dfs);

    return IgnoredUse;
  }

  // MARK: Access projections

  case SILInstructionKind::StructElementAddrInst:
  case SILInstructionKind::TupleElementAddrInst:
    followProjection(svi, dfs);
    return IgnoredUse;

  case SILInstructionKind::IndexAddrInst:
  case SILInstructionKind::TailAddrInst: {
    auto projIdx = ProjectionIndex(svi);
    if (projIdx.isValid()) {
      if (dfs.offset != AccessPath::UnknownOffset)
        dfs.offset += projIdx.Index;
      else
        assert(visitor.findOverlappingUses());
    } else if (visitor.findOverlappingUses()) {
      dfs.offset = AccessPath::UnknownOffset;
    } else {
      return IgnoredUse;
    }
    pushUsers(svi, dfs);
    return IgnoredUse;
  }

  case SILInstructionKind::InitEnumDataAddrInst:
    pushUsers(svi, dfs);
    return IgnoredUse;

  // open_existential_addr and unchecked_take_enum_data_addr are classified as
  // access projections, but they also modify memory. Both see through them and
  // also report them as uses.
  case SILInstructionKind::OpenExistentialAddrInst:
  case SILInstructionKind::UncheckedTakeEnumDataAddrInst:
    pushUsers(svi, dfs);
    return LeafUse;

  case SILInstructionKind::StructExtractInst:
    // Handle nested access to a KeyPath projection. The projection itself
    // uses a Builtin. However, the returned UnsafeMutablePointer may be
    // converted to an address and accessed via an inout argument.
    if (isUnsafePointerExtraction(cast<StructExtractInst>(svi))) {
      pushUsers(svi, dfs);
      return IgnoredUse;
    }
    return LeafUse;

  case SILInstructionKind::LoadInst:
    // Load a box from an indirect payload of an opaque enum. See comments
    // in AccessUseDefChainVisitor::visit. Record this load as a leaf-use even
    // when we look through its project_box because anyone inspecting the load
    // itself will see the same AccessPath.
    // FIXME: if this doesn't go away with opaque values, add a new instruction
    // for load+project_box.
    if (svi->getType().is<SILBoxType>()) {
      Operand *addrOper = &cast<LoadInst>(svi)->getOperandRef();
      assert(isa<UncheckedTakeEnumDataAddrInst>(addrOper->get()));
      // Push the project_box uses
      for (auto *use : svi->getUses()) {
        if (isa<ProjectBoxInst>(use->getUser()))
          pushUser(DFSEntry(use, dfs.isRef(), dfs.pathCursor, dfs.offset));
      }
    }
    return LeafUse;
  }
}

bool AccessPathDefUseTraversal::visitUser(DFSEntry dfs) {
  Operand *use = dfs.getUse();
  assert(!(dfs.isRef() && use->get()->getType().isAddress()));
  if (auto *svi = dyn_cast<SingleValueInstruction>(use->getUser())) {
    if (use->getOperandNumber() == 0
        && visitSingleValueUser(svi, dfs) == IgnoredUse) {
      return true;
    }
  }
  if (isa<EndBorrowInst>(use->getUser())) {
    return true;
  }
  // We weren't able to "see through" any more address conversions; so
  // record this as a use.

  // Do the path offsets match?
  if (!checkAndUpdateOffset(dfs))
    return true;

  // Is this a partial path match?
  if (dfs.pathCursor > 0 || dfs.offset == AccessPath::UnknownOffset) {
    return visitor.visitOverlappingUse(use);
  }
  if (dfs.pathCursor < 0) {
    return visitor.visitInnerUse(use);
  }
  return visitor.visitExactUse(use);
}

bool swift::visitAccessPathUses(AccessUseVisitor &visitor,
                                AccessPath accessPath, SILFunction *function) {
  return AccessPathDefUseTraversal(visitor, accessPath, function).visitUses();
}

bool swift::visitAccessedStorageUses(AccessUseVisitor &visitor,
                                     AccessedStorage storage,
                                     SILFunction *function) {
  IndexTrieNode *emptyPath = function->getModule().getIndexTrieRoot();
  return visitAccessPathUses(visitor, AccessPath(storage, emptyPath, 0),
                             function);
}

class CollectAccessPathUses : public AccessUseVisitor {
  // Result: Exact uses, projection uses, and containing uses.
  SmallVectorImpl<Operand *> &uses;

  unsigned useLimit;

public:
  CollectAccessPathUses(SmallVectorImpl<Operand *> &uses, AccessUseType useTy,
                        unsigned useLimit)
    : AccessUseVisitor(useTy, NestedAccessType::IgnoreAccessBegin), uses(uses),
      useLimit(useLimit) {}

  bool visitUse(Operand *use, AccessUseType useTy) override {
    if (uses.size() == useLimit) {
      return false;
    }
    uses.push_back(use);
    return true;
  }
};

bool AccessPath::collectUses(SmallVectorImpl<Operand *> &uses,
                             AccessUseType useTy, SILFunction *function,
                             unsigned useLimit) const {
  CollectAccessPathUses collector(uses, useTy, useLimit);
  return visitAccessPathUses(collector, *this, function);
}

//===----------------------------------------------------------------------===//
//             MARK: Helper API for specific formal access patterns
//===----------------------------------------------------------------------===//

static bool isScratchBuffer(SILValue value) {
  // Special case unsafe value buffer access.
  return value->getType().is<BuiltinUnsafeValueBufferType>();
}

bool swift::memInstMustInitialize(Operand *memOper) {
  SILValue address = memOper->get();
  SILInstruction *memInst = memOper->getUser();

  switch (memInst->getKind()) {
  default:
    return false;

  case SILInstructionKind::CopyAddrInst: {
    auto *CAI = cast<CopyAddrInst>(memInst);
    return CAI->getDest() == address && CAI->isInitializationOfDest();
  }
  case SILInstructionKind::InitExistentialAddrInst:
  case SILInstructionKind::InitEnumDataAddrInst:
  case SILInstructionKind::InjectEnumAddrInst:
    return true;

  case SILInstructionKind::StoreInst:
    return cast<StoreInst>(memInst)->getOwnershipQualifier()
           == StoreOwnershipQualifier::Init;

#define NEVER_OR_SOMETIMES_LOADABLE_CHECKED_REF_STORAGE(Name, ...) \
  case SILInstructionKind::Store##Name##Inst: \
    return cast<Store##Name##Inst>(memInst)->isInitializationOfDest();
#include "swift/AST/ReferenceStorage.def"
  }
}

Operand *
swift::getSingleInitAllocStackUse(AllocStackInst *asi,
                                  SmallVectorImpl<Operand *> *destroyingUses) {
  // For now, we just look through projections and rely on memInstMustInitialize
  // to classify all other uses as init or not.
  SmallVector<Operand *, 32> worklist(asi->getUses());
  Operand *singleInit = nullptr;

  while (!worklist.empty()) {
    auto *use = worklist.pop_back_val();
    auto *user = use->getUser();

    if (Projection::isAddressProjection(user)
        || isa<OpenExistentialAddrInst>(user)) {
      // Look through address projections.
      for (SILValue r : user->getResults()) {
        llvm::copy(r->getUses(), std::back_inserter(worklist));
      }
      continue;
    }

    if (auto *li = dyn_cast<LoadInst>(user)) {
      // If we are not taking,
      if (li->getOwnershipQualifier() != LoadOwnershipQualifier::Take) {
        continue;
      }
      // Treat load [take] as a write.
      return nullptr;
    }

    switch (user->getKind()) {
    default:
      break;
    case SILInstructionKind::UnconditionalCheckedCastAddrInst: {
      auto *uccai = cast<UnconditionalCheckedCastAddrInst>(user);
      // Only handle the case where we are doing a take of our alloc_stack as a
      // source value. If we are the dest, then something else is happening!
      // Break!
      if (use->get() == uccai->getDest())
        break;
      // Ok, we are the Src and are performing a take. Treat it as a destroy!
      if (destroyingUses)
        destroyingUses->push_back(use);
      continue;
    }
    case SILInstructionKind::CheckedCastAddrBranchInst: {
      auto *ccabi = cast<CheckedCastAddrBranchInst>(user);
      // We only handle the case where we are doing a take of our alloc_stack as
      // a source.
      //
      // TODO: Can we expand this?
      if (use->get() == ccabi->getDest())
        break;
      if (ccabi->getConsumptionKind() != CastConsumptionKind::TakeAlways)
        break;
      // Ok, we are the Src and are performing a take. Treat it as a destroy!
      if (destroyingUses)
        destroyingUses->push_back(use);
      continue;
    }
    case SILInstructionKind::DestroyAddrInst:
      if (destroyingUses)
        destroyingUses->push_back(use);
      continue;
    case SILInstructionKind::DeallocStackInst:
    case SILInstructionKind::LoadBorrowInst:
    case SILInstructionKind::DebugValueAddrInst:
      continue;
    }

    // See if we have an initializer and that such initializer is in the same
    // block.
    if (memInstMustInitialize(use)) {
      if (user->getParent() != asi->getParent() || singleInit) {
        return nullptr;
      }

      singleInit = use;
      continue;
    }

    // Otherwise, if we have found something not in our allowlist, return false.
    return nullptr;
  }

  // We did not find any users that we did not understand. So we can
  // conservatively return the single initializing write that we found.
  return singleInit;
}

/// Return true if the given address value is produced by a special address
/// producer that is only used for local initialization, not formal access.
bool swift::isAddressForLocalInitOnly(SILValue sourceAddr) {
  switch (sourceAddr->getKind()) {
  default:
    return false;

  // Value to address conversions: the operand is the non-address source
  // value. These allow local mutation of the value but should never be used
  // for formal access of an lvalue.
  case ValueKind::OpenExistentialBoxInst:
  case ValueKind::ProjectExistentialBoxInst:
    return true;

  // Self-evident local initialization.
  case ValueKind::InitEnumDataAddrInst:
  case ValueKind::InitExistentialAddrInst:
  case ValueKind::AllocExistentialBoxInst:
  case ValueKind::AllocValueBufferInst:
  case ValueKind::ProjectValueBufferInst:
    return true;
  }
}

// Return true if the given apply invokes a global addressor defined in another
// module.
bool swift::isExternalGlobalAddressor(ApplyInst *AI) {
  FullApplySite apply(AI);
  auto *funcRef = apply.getReferencedFunctionOrNull();
  if (!funcRef)
    return false;

  return funcRef->isGlobalInit() && funcRef->isExternalDeclaration();
}

// Return true if the given StructExtractInst extracts the RawPointer from
// Unsafe[Mutable]Pointer.
bool swift::isUnsafePointerExtraction(StructExtractInst *SEI) {
  if (!isa<BuiltinRawPointerType>(SEI->getType().getASTType()))
    return false;

  auto &C = SEI->getModule().getASTContext();
  auto *decl = SEI->getStructDecl();
  return decl == C.getUnsafeMutablePointerDecl() ||
         decl == C.getUnsafePointerDecl();
}

// Given a block argument address base, check if it is actually a box projected
// from a switch_enum. This is a valid pattern at any SIL stage resulting in a
// block-type phi. In later SIL stages, the optimizer may form address-type
// phis, causing this assert if called on those cases.
void swift::checkSwitchEnumBlockArg(SILPhiArgument *arg) {
  assert(!arg->getType().isAddress());
  SILBasicBlock *Pred = arg->getParent()->getSinglePredecessorBlock();
  if (!Pred || !isa<SwitchEnumInst>(Pred->getTerminator())) {
    arg->dump();
    llvm_unreachable("unexpected box source.");
  }
}

bool swift::isPossibleFormalAccessBase(const AccessedStorage &storage,
                                       SILFunction *F) {
  switch (storage.getKind()) {
  case AccessedStorage::Nested:
    assert(false && "don't pass nested storage to this helper");
    return false;

  case AccessedStorage::Box:
  case AccessedStorage::Stack:
    if (isScratchBuffer(storage.getValue()))
      return false;
    break;
  case AccessedStorage::Global:
    break;
  case AccessedStorage::Class:
    break;
  case AccessedStorage::Tail:
    return false;

  case AccessedStorage::Yield:
    // Yields are accessed by the caller.
    return false;
  case AccessedStorage::Argument:
    // Function arguments are accessed by the caller.
    return false;

  case AccessedStorage::Unidentified:
    if (isAddressForLocalInitOnly(storage.getValue()))
      return false;

    if (isa<SILPhiArgument>(storage.getValue())) {
      checkSwitchEnumBlockArg(cast<SILPhiArgument>(storage.getValue()));
      return false;
    }
    // Pointer-to-address exclusivity cannot be enforced. `baseAddress` may be
    // pointing anywhere within an object.
    if (isa<PointerToAddressInst>(storage.getValue()))
      return false;

    if (isa<SILUndef>(storage.getValue()))
      return false;

    if (isScratchBuffer(storage.getValue()))
      return false;

    break;
  }
  // Additional checks that apply to anything that may fall through.

  // Immutable values are only accessed for initialization.
  if (storage.isLetAccess())
    return false;

  return true;
}

SILBasicBlock::iterator swift::removeBeginAccess(BeginAccessInst *beginAccess) {
  while (!beginAccess->use_empty()) {
    Operand *op = *beginAccess->use_begin();

    // Delete any associated end_access instructions.
    if (auto endAccess = dyn_cast<EndAccessInst>(op->getUser())) {
      endAccess->eraseFromParent();

      // Forward all other uses to the original address.
    } else {
      op->set(beginAccess->getSource());
    }
  }
  auto nextIter = std::next(beginAccess->getIterator());
  beginAccess->getParent()->erase(beginAccess);
  return nextIter;
}

//===----------------------------------------------------------------------===//
//                             MARK: Verification
//===----------------------------------------------------------------------===//

// Helper for visitApplyAccesses that visits address-type call arguments,
// including arguments to @noescape functions that are passed as closures to
// the current call.
static void visitApplyAccesses(ApplySite apply,
                               llvm::function_ref<void(Operand *)> visitor) {
  for (Operand &oper : apply.getArgumentOperands()) {
    // Consider any address-type operand an access. Whether it is read or modify
    // depends on the argument convention.
    if (oper.get()->getType().isAddress()) {
      visitor(&oper);
      continue;
    }
    auto fnType = oper.get()->getType().getAs<SILFunctionType>();
    if (!fnType || !fnType->isNoEscape())
      continue;

    // When @noescape function closures are passed as arguments, their
    // arguments are considered accessed at the call site.
    TinyPtrVector<PartialApplyInst *> partialApplies;
    findClosuresForFunctionValue(oper.get(), partialApplies);
    // Recursively visit @noescape function closure arguments.
    for (auto *PAI : partialApplies)
      visitApplyAccesses(PAI, visitor);
  }
}

static void visitBuiltinAddress(BuiltinInst *builtin,
                                llvm::function_ref<void(Operand *)> visitor) {
  if (auto kind = builtin->getBuiltinKind()) {
    switch (kind.getValue()) {
    default:
      builtin->dump();
      llvm_unreachable("unexpected builtin memory access.");

    // Handle builtin "generic_add"<V>($*V, $*V, $*V) and the like.
#define BUILTIN(Id, Name, Attrs)
#define BUILTIN_BINARY_OPERATION_POLYMORPHIC(Id, Name)        \
    case BuiltinValueKind::Id:

#include "swift/AST/Builtins.def"

      visitor(&builtin->getAllOperands()[1]);
      visitor(&builtin->getAllOperands()[2]);
      return;

    // These consume values out of their second operand.
    case BuiltinValueKind::ResumeNonThrowingContinuationReturning:
    case BuiltinValueKind::ResumeThrowingContinuationReturning:
    case BuiltinValueKind::ResumeThrowingContinuationThrowing:
      visitor(&builtin->getAllOperands()[1]);
      return;

    // WillThrow exists for the debugger, does nothing.
    case BuiltinValueKind::WillThrow:
      return;

    // Buitins that affect memory but can't be formal accesses.
    case BuiltinValueKind::AssumeTrue:
    case BuiltinValueKind::UnexpectedError:
    case BuiltinValueKind::ErrorInMain:
    case BuiltinValueKind::IsOptionalType:
    case BuiltinValueKind::CondFailMessage:
    case BuiltinValueKind::AllocRaw:
    case BuiltinValueKind::DeallocRaw:
    case BuiltinValueKind::Fence:
    case BuiltinValueKind::StaticReport:
    case BuiltinValueKind::Once:
    case BuiltinValueKind::OnceWithContext:
    case BuiltinValueKind::Unreachable:
    case BuiltinValueKind::CondUnreachable:
    case BuiltinValueKind::DestroyArray:
    case BuiltinValueKind::UnsafeGuaranteed:
    case BuiltinValueKind::UnsafeGuaranteedEnd:
    case BuiltinValueKind::Swift3ImplicitObjCEntrypoint:
    case BuiltinValueKind::PoundAssert:
    case BuiltinValueKind::IntInstrprofIncrement:
    case BuiltinValueKind::TSanInoutAccess:
    case BuiltinValueKind::CancelAsyncTask:
    case BuiltinValueKind::CreateAsyncTask:
    case BuiltinValueKind::CreateAsyncTaskInGroup:
    case BuiltinValueKind::AutoDiffCreateLinearMapContext:
    case BuiltinValueKind::AutoDiffAllocateSubcontext:
    case BuiltinValueKind::InitializeDefaultActor:
    case BuiltinValueKind::DestroyDefaultActor:
    case BuiltinValueKind::GetCurrentExecutor:
    case BuiltinValueKind::StartAsyncLet:
    case BuiltinValueKind::StartAsyncLetWithLocalBuffer:
    case BuiltinValueKind::EndAsyncLet:
    case BuiltinValueKind::EndAsyncLetLifetime:
    case BuiltinValueKind::CreateTaskGroup:
    case BuiltinValueKind::DestroyTaskGroup:
      return;

    // General memory access to a pointer in first operand position.
    case BuiltinValueKind::CmpXChg:
    case BuiltinValueKind::AtomicLoad:
    case BuiltinValueKind::AtomicStore:
    case BuiltinValueKind::AtomicRMW:
      // Currently ignored because the access is on a RawPointer, not a
      // SIL address.
      // visitor(&builtin->getAllOperands()[0]);
      return;

    // Arrays: (T.Type, Builtin.RawPointer, Builtin.RawPointer,
    // Builtin.Word)
    case BuiltinValueKind::CopyArray:
    case BuiltinValueKind::TakeArrayNoAlias:
    case BuiltinValueKind::TakeArrayFrontToBack:
    case BuiltinValueKind::TakeArrayBackToFront:
    case BuiltinValueKind::AssignCopyArrayNoAlias:
    case BuiltinValueKind::AssignCopyArrayFrontToBack:
    case BuiltinValueKind::AssignCopyArrayBackToFront:
    case BuiltinValueKind::AssignTakeArray:
      // Currently ignored because the access is on a RawPointer.
      // visitor(&builtin->getAllOperands()[1]);
      // visitor(&builtin->getAllOperands()[2]);
      return;
    }
  }
  if (auto ID = builtin->getIntrinsicID()) {
    switch (ID.getValue()) {
      // Exhaustively verifying all LLVM instrinsics that access memory is
      // impractical. Instead, we call out the few common cases and return in
      // the default case.
    default:
      return;
    case llvm::Intrinsic::memcpy:
    case llvm::Intrinsic::memmove:
      // Currently ignored because the access is on a RawPointer.
      // visitor(&builtin->getAllOperands()[0]);
      // visitor(&builtin->getAllOperands()[1]);
      return;
    case llvm::Intrinsic::memset:
      // Currently ignored because the access is on a RawPointer.
      // visitor(&builtin->getAllOperands()[0]);
      return;
    }
  }
  llvm_unreachable("Must be either a builtin or intrinsic.");
}

void swift::visitAccessedAddress(SILInstruction *I,
                                 llvm::function_ref<void(Operand *)> visitor) {
  assert(I->mayReadOrWriteMemory());

  // Reference counting instructions do not access user visible memory.
  if (isa<RefCountingInst>(I))
    return;

  if (isa<DeallocationInst>(I))
    return;

  if (auto apply = FullApplySite::isa(I)) {
    visitApplyAccesses(apply, visitor);
    return;
  }

  if (auto builtin = dyn_cast<BuiltinInst>(I)) {
    visitBuiltinAddress(builtin, visitor);
    return;
  }

  switch (I->getKind()) {
  default:
    I->dump();
    llvm_unreachable("unexpected memory access.");

  case SILInstructionKind::AssignInst:
  case SILInstructionKind::AssignByWrapperInst:
    visitor(&I->getAllOperands()[AssignInst::Dest]);
    return;

  case SILInstructionKind::CheckedCastAddrBranchInst:
    visitor(&I->getAllOperands()[CheckedCastAddrBranchInst::Src]);
    visitor(&I->getAllOperands()[CheckedCastAddrBranchInst::Dest]);
    return;

  case SILInstructionKind::CopyAddrInst:
    visitor(&I->getAllOperands()[CopyAddrInst::Src]);
    visitor(&I->getAllOperands()[CopyAddrInst::Dest]);
    return;

#define NEVER_OR_SOMETIMES_LOADABLE_CHECKED_REF_STORAGE(Name, ...) \
  case SILInstructionKind::Store##Name##Inst:
#include "swift/AST/ReferenceStorage.def"
  case SILInstructionKind::StoreInst:
  case SILInstructionKind::StoreBorrowInst:
    visitor(&I->getAllOperands()[StoreInst::Dest]);
    return;

  case SILInstructionKind::SelectEnumAddrInst:
    visitor(&I->getAllOperands()[0]);
    return;

  case SILInstructionKind::InitExistentialAddrInst:
  case SILInstructionKind::InjectEnumAddrInst:
#define NEVER_OR_SOMETIMES_LOADABLE_CHECKED_REF_STORAGE(Name, ...) \
  case SILInstructionKind::Load##Name##Inst:
#include "swift/AST/ReferenceStorage.def"
  case SILInstructionKind::LoadInst:
  case SILInstructionKind::LoadBorrowInst:
  case SILInstructionKind::OpenExistentialAddrInst:
  case SILInstructionKind::SwitchEnumAddrInst:
  case SILInstructionKind::UncheckedTakeEnumDataAddrInst:
  case SILInstructionKind::UnconditionalCheckedCastInst: {
    // Assuming all the above have only a single address operand.
    assert(I->getNumOperands() - I->getNumTypeDependentOperands() == 1);
    Operand *singleOperand = &I->getAllOperands()[0];
    // Check the operand type because UnconditionalCheckedCastInst may operate
    // on a non-address.
    if (singleOperand->get()->getType().isAddress())
      visitor(singleOperand);
    return;
  }
  // Non-access cases: these are marked with memory side effects, but, by
  // themselves, do not access formal memory.
#define UNCHECKED_REF_STORAGE(Name, ...)                                       \
  case SILInstructionKind::StrongCopy##Name##ValueInst:
#define ALWAYS_OR_SOMETIMES_LOADABLE_CHECKED_REF_STORAGE(Name, ...)            \
  case SILInstructionKind::StrongCopy##Name##ValueInst:
#include "swift/AST/ReferenceStorage.def"
  case SILInstructionKind::AbortApplyInst:
  case SILInstructionKind::AllocBoxInst:
  case SILInstructionKind::AllocExistentialBoxInst:
  case SILInstructionKind::AllocGlobalInst:
  case SILInstructionKind::BeginAccessInst:
  case SILInstructionKind::BeginApplyInst:
  case SILInstructionKind::BeginBorrowInst:
  case SILInstructionKind::BeginCOWMutationInst:
  case SILInstructionKind::EndCOWMutationInst:
  case SILInstructionKind::BeginUnpairedAccessInst:
  case SILInstructionKind::BindMemoryInst:
  case SILInstructionKind::CheckedCastValueBranchInst:
  case SILInstructionKind::CondFailInst:
  case SILInstructionKind::CopyBlockInst:
  case SILInstructionKind::CopyBlockWithoutEscapingInst:
  case SILInstructionKind::CopyValueInst:
  case SILInstructionKind::DeinitExistentialAddrInst:
  case SILInstructionKind::DeinitExistentialValueInst:
  case SILInstructionKind::DestroyAddrInst:
  case SILInstructionKind::DestroyValueInst:
  case SILInstructionKind::EndAccessInst:
  case SILInstructionKind::EndApplyInst:
  case SILInstructionKind::EndBorrowInst:
  case SILInstructionKind::EndUnpairedAccessInst:
  case SILInstructionKind::EndLifetimeInst:
  case SILInstructionKind::ExistentialMetatypeInst:
  case SILInstructionKind::FixLifetimeInst:
  case SILInstructionKind::GlobalAddrInst:
  case SILInstructionKind::HopToExecutorInst:
  case SILInstructionKind::ExtractExecutorInst:
  case SILInstructionKind::InitExistentialValueInst:
  case SILInstructionKind::IsUniqueInst:
  case SILInstructionKind::IsEscapingClosureInst:
  case SILInstructionKind::KeyPathInst:
  case SILInstructionKind::OpenExistentialBoxInst:
  case SILInstructionKind::OpenExistentialBoxValueInst:
  case SILInstructionKind::OpenExistentialValueInst:
  case SILInstructionKind::PartialApplyInst:
  case SILInstructionKind::ProjectValueBufferInst:
  case SILInstructionKind::YieldInst:
  case SILInstructionKind::UnwindInst:
  case SILInstructionKind::UncheckedOwnershipConversionInst:
  case SILInstructionKind::UncheckedRefCastAddrInst:
  case SILInstructionKind::UnconditionalCheckedCastAddrInst:
  case SILInstructionKind::UnconditionalCheckedCastValueInst:
  case SILInstructionKind::ValueMetatypeInst:
  // TODO: Is this correct?
  case SILInstructionKind::GetAsyncContinuationInst:
  case SILInstructionKind::GetAsyncContinuationAddrInst:
  case SILInstructionKind::AwaitAsyncContinuationInst:
    return;
  }
}
