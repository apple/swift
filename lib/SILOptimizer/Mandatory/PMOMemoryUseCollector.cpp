//===--- PMOMemoryUseCollector.cpp - Memory use analysis for PMO ----------===//
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

#define DEBUG_TYPE "definite-init"
#include "PMOMemoryUseCollector.h"
#include "swift/AST/Expr.h"
#include "swift/Basic/Assertions.h"
#include "swift/SIL/InstructionUtils.h"
#include "swift/SIL/SILArgument.h"
#include "swift/SIL/SILBuilder.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/SaveAndRestore.h"

using namespace swift;

//===----------------------------------------------------------------------===//
//                     PMOMemoryObjectInfo Implementation
//===----------------------------------------------------------------------===//

PMOMemoryObjectInfo::PMOMemoryObjectInfo(AllocationInst *allocation)
    : MemoryInst(allocation) {
  auto &module = MemoryInst->getModule();

  // Compute the type of the memory object.
  if (auto *abi = dyn_cast<AllocBoxInst>(MemoryInst)) {
    assert(abi->getBoxType()->getLayout()->getFields().size() == 1 &&
           "analyzing multi-field boxes not implemented");
    MemorySILType =
        getSILBoxFieldType(TypeExpansionContext(*abi->getFunction()),
                           abi->getBoxType(), module.Types, 0);
  } else {
    MemorySILType = cast<AllocStackInst>(MemoryInst)->getElementType();
  }
}

SILInstruction *PMOMemoryObjectInfo::getFunctionEntryPoint() const {
  return &*getFunction().begin()->begin();
}

//===----------------------------------------------------------------------===//
//                          Scalarization Logic
//===----------------------------------------------------------------------===//

/// Given a pointer to a tuple type, compute the addresses of each element and
/// add them to the ElementAddrs vector.
static void
getScalarizedElementAddresses(SILValue Pointer, SILBuilder &B, SILLocation Loc,
                              SmallVectorImpl<SILValue> &ElementAddrs) {
  TupleType *TT = Pointer->getType().castTo<TupleType>();
  for (auto Index : indices(TT->getElements())) {
    ElementAddrs.push_back(B.createTupleElementAddr(Loc, Pointer, Index));
  }
}

/// Scalarize a load down to its subelements.  If NewLoads is specified, this
/// can return the newly generated sub-element loads.
static SILValue scalarizeLoad(LoadInst *LI,
                              SmallVectorImpl<SILValue> &ElementAddrs) {
  SILBuilderWithScope B(LI);
  SmallVector<SILValue, 4> ElementTmps;

  for (unsigned i = 0, e = ElementAddrs.size(); i != e; ++i) {
    auto *SubLI = B.createTrivialLoadOr(LI->getLoc(), ElementAddrs[i],
                                        LI->getOwnershipQualifier(),
                                        true /*supports unqualified*/);
    ElementTmps.push_back(SubLI);
  }

  if (LI->getType().is<TupleType>())
    return B.createTuple(LI->getLoc(), LI->getType(), ElementTmps);
  return B.createStruct(LI->getLoc(), LI->getType(), ElementTmps);
}

/// Scalarize a load_borrow down to its subelements. It will scalarize each of
/// the end_borrows of the load_borrow as well.
static void scalarizeLoadBorrow(LoadBorrowInst *lbi,
                                SmallVectorImpl<SILValue> &elementAddrs) {
  // First gather all of our end_borrows. We are going to scalarize them as
  // well.
  SmallVector<EndBorrowInst *, 8> endBorrows;
  for (auto *op : lbi->getUses()) {
    if (auto *ebi = dyn_cast<EndBorrowInst>(op->getUser())) {
      endBorrows.push_back(ebi);
    }
  }

  SILBuilderWithScope b(lbi);
  SmallVector<SILValue, 4> elementTmps;

  for (unsigned i : indices(elementAddrs)) {
    if (elementAddrs[i]->getType().isTrivial(*lbi->getFunction())) {
      elementTmps.push_back(b.createLoad(lbi->getLoc(), elementAddrs[i],
                                         LoadOwnershipQualifier::Trivial));
      continue;
    }

    SILValue v = b.createLoadBorrow(lbi->getLoc(), elementAddrs[i]);
    for (auto *ebi : endBorrows) {
      SILBuilderWithScope(ebi).createEndBorrow(lbi->getLoc(), v);
    }
    elementTmps.push_back(v);
  }

  // Inline constructor.
  auto result = ([&]() -> SILValue {
    if (lbi->getType().is<TupleType>())
      return b.createTuple(lbi->getLoc(), lbi->getType(), elementTmps);
    return b.createStruct(lbi->getLoc(), lbi->getType(), elementTmps);
  })();

  // Delete all of the end borrows, rauw, and we are done!
  for (auto *ebi : endBorrows) {
    ebi->eraseFromParent();
  }
  lbi->replaceAllUsesWith(result);
  lbi->eraseFromParent();
}

//===----------------------------------------------------------------------===//
//                     ElementUseCollector Implementation
//===----------------------------------------------------------------------===//

namespace {

class ElementUseCollector {
  SILModule &Module;
  const PMOMemoryObjectInfo &TheMemory;
  SmallVectorImpl<PMOMemoryUse> &Uses;
  SmallVectorImpl<SILInstruction *> &Releases;

  /// When walking the use list, if we index into a struct element, keep track
  /// of this, so that any indexes into tuple subelements don't affect the
  /// element we attribute an access to.
  bool InStructSubElement = false;

public:
  ElementUseCollector(const PMOMemoryObjectInfo &TheMemory,
                      SmallVectorImpl<PMOMemoryUse> &Uses,
                      SmallVectorImpl<SILInstruction *> &Releases)
      : Module(TheMemory.MemoryInst->getModule()), TheMemory(TheMemory),
        Uses(Uses), Releases(Releases) {}

  /// This is the main entry point for the use walker.  It collects uses from
  /// the address and the refcount result of the allocation.
  [[nodiscard]] bool collectFrom();

private:
  [[nodiscard]] bool collectUses(SILValue Pointer);
  [[nodiscard]] bool collectContainerUses(SILValue boxValue);
};
} // end anonymous namespace

bool ElementUseCollector::collectFrom() {
  if (auto *abi = TheMemory.getContainer()) {
    return collectContainerUses(abi);
  }

  return collectUses(TheMemory.getAddress());
}

bool ElementUseCollector::collectContainerUses(SILValue boxValue) {
  assert(isa<AllocBoxInst>(boxValue) || isa<CopyValueInst>(boxValue));

  for (auto *ui : boxValue->getUses()) {
    auto *user = ui->getUser();

    // dealloc_box deallocated a box containing uninitialized memory. This can
    // not effect any value stored into the box.
    if (isa<DeallocBoxInst>(user))
      continue;

    // Retaining the box doesn't effect the value inside the box.
    if (isa<StrongRetainInst>(user) || isa<RetainValueInst>(user))
      continue;

    // Like retaining, copies do not effect the underlying value. We do need to
    // recursively visit the copies users though.
    if (auto *cvi = dyn_cast<CopyValueInst>(user)) {
      if (!collectContainerUses(cvi))
        return false;
      continue;
    }

    // Since we are trying to promote loads/stores, any releases of the box are
    // not considered uses of the underlying value due to:
    //
    // 1. If this is not the last release of the box, then the underlying value
    // is not effected implying we do not add this value.
    //
    // 2. If this is the last release of the box, then the box's destruction
    // will result in a release of the underlying value. If there are any
    // loads/stores after this point, the behavior would be undefined so we can
    // ignore this possibility.
    //
    // That being said, if we want to eliminate the box completely we need to
    // know where the releases are so that we can release the value that would
    // have been at +1 in the box at that time. So we add these to the Releases
    // array.
    //
    // FIXME: Since we do not support promoting strong_release or release_value
    // today this will cause the underlying allocation to never be
    // eliminated. That should be implemented and fixed.
    if (isa<StrongReleaseInst>(user) || isa<ReleaseValueInst>(user) ||
        isa<DestroyValueInst>(user)) {
      Releases.push_back(user);
      continue;
    }

    if (auto *p = dyn_cast<ProjectBoxInst>(user)) {
      if (!collectUses(p))
        return false;
      continue;
    }

    // Other uses of the container are considered escapes of the underlying
    // value.
    //
    // This will cause the dataflow to stop propagating any information at the
    // use block.
    Uses.emplace_back(user, PMOUseKind::Escape);
  }

  return true;
}

bool ElementUseCollector::collectUses(SILValue Pointer) {
  assert(Pointer->getType().isAddress() &&
         "Walked through the pointer to the value?");
  SILType PointeeType = Pointer->getType().getObjectType();

  /// This keeps track of instructions in the use list that touch multiple tuple
  /// elements and should be scalarized.  This is done as a second phase to
  /// avoid invalidating the use iterator.
  ///
  SmallVector<SILInstruction *, 4> UsesToScalarize;

  for (auto *UI : Pointer->getUses()) {
    auto *User = UI->getUser();

    // struct_element_addr P, #field indexes into the current element.
    if (auto *seai = dyn_cast<StructElementAddrInst>(User)) {
      // Generally, we set the "InStructSubElement" flag and recursively process
      // the uses so that we know that we're looking at something within the
      // current element.
      llvm::SaveAndRestore<bool> X(InStructSubElement, true);
      if (!collectUses(seai))
        return false;
      continue;
    }

    // Instructions that compute a subelement are handled by a helper.
    if (auto *teai = dyn_cast<TupleElementAddrInst>(User)) {
      if (!collectUses(teai))
        return false;
      continue;
    }

    // Look through begin_access.
    if (auto *bai = dyn_cast<BeginAccessInst>(User)) {
      if (!collectUses(bai))
        return false;
      continue;
    }

    // Ignore end_access.
    if (isa<EndAccessInst>(User)) {
      continue;
    }

    // Loads are a use of the value.
    if (isa<LoadInst>(User) || isa<LoadBorrowInst>(User)) {
      if (PointeeType.is<TupleType>())
        UsesToScalarize.push_back(User);
      else
        Uses.emplace_back(User, PMOUseKind::Load);
      continue;
    }

    // Stores *to* the allocation are writes.
    if (auto *si = dyn_cast<StoreInst>(User)) {
      if (UI->getOperandNumber() == StoreInst::Dest) {
        if (auto tupleType = PointeeType.getAs<TupleType>()) {
          if (!tupleType->isEqual(Module.getASTContext().TheEmptyTupleType) &&
              !tupleType->containsPackExpansionType()) {
            UsesToScalarize.push_back(User);
            continue;
          }
        }

        auto kind = ([&]() -> PMOUseKind {
          switch (si->getOwnershipQualifier()) {
          // Coming out of SILGen, we assume that raw stores are
          // initializations, unless they have trivial type (which we classify
          // as InitOrAssign).
          case StoreOwnershipQualifier::Unqualified:
            if (PointeeType.isTrivial(*User->getFunction()))
              return PMOUseKind::InitOrAssign;
            return PMOUseKind::Initialization;

          case StoreOwnershipQualifier::Init:
            return PMOUseKind::Initialization;

          case StoreOwnershipQualifier::Assign:
            return PMOUseKind::Assign;

          case StoreOwnershipQualifier::Trivial:
            return PMOUseKind::InitOrAssign;
          }
          llvm_unreachable("covered switch");
        })();
        Uses.emplace_back(si, kind);
        continue;
      }
    }

    if (auto *CAI = dyn_cast<CopyAddrInst>(User)) {
      // If this is a copy of a tuple, we should scalarize it so that we don't
      // have an access that crosses elements.
      if (auto tupleType = PointeeType.getAs<TupleType>()) {
        if (!tupleType->isEqual(Module.getASTContext().TheEmptyTupleType) &&
            !tupleType->containsPackExpansionType()) {
          UsesToScalarize.push_back(CAI);
          continue;
        }
      }

      // If this is the source of the copy_addr, then this is a load.  If it is
      // the destination, then this is an unknown assignment.  Note that we'll
      // revisit this instruction and add it to Uses twice if it is both a load
      // and store to the same aggregate.
      //
      // Inline constructor.
      auto Kind = ([&]() -> PMOUseKind {
        if (UI->getOperandNumber() == CopyAddrInst::Src)
          return PMOUseKind::Load;
        if (PointeeType.isTrivial(*CAI->getFunction()))
          return PMOUseKind::InitOrAssign;
        if (CAI->isInitializationOfDest())
          return PMOUseKind::Initialization;
        return PMOUseKind::Assign;
      })();
      Uses.emplace_back(User, Kind);
      continue;
    }

    // The apply instruction does not capture the pointer when it is passed
    // through 'inout' arguments or for indirect returns.  InOut arguments are
    // treated as uses and may-store's, but an indirect return is treated as a
    // full store.
    //
    // Note that partial_apply instructions always close over their argument.
    //
    if (auto *Apply = dyn_cast<ApplyInst>(User)) {
      auto substConv = Apply->getSubstCalleeConv();
      unsigned ArgumentNumber = UI->getOperandNumber() - 1;

      // If this is an out-parameter, it is like a store.
      unsigned NumIndirectResults = substConv.getNumIndirectSILResults() +
                                    substConv.getNumIndirectSILErrorResults();
      if (ArgumentNumber < NumIndirectResults) {
        // We do not support initializing sub members. This is an old
        // restriction from when this code was used by Definite
        // Initialization. With proper code review, we can remove this, but for
        // now, lets be conservative.
        if (InStructSubElement) {
          return false;
        }
        Uses.emplace_back(User, PMOUseKind::Initialization);
        continue;

        // Otherwise, adjust the argument index.
      } else {
        ArgumentNumber -= NumIndirectResults;
      }

      auto ParamConvention =
          substConv.getParameters()[ArgumentNumber].getConvention();

      switch (ParamConvention) {
      case ParameterConvention::Direct_Owned:
      case ParameterConvention::Direct_Unowned:
      case ParameterConvention::Direct_Guaranteed:
      case ParameterConvention::Pack_Owned:
      case ParameterConvention::Pack_Guaranteed:
      case ParameterConvention::Pack_Inout:
        llvm_unreachable("address value passed to indirect parameter");

      // If this is an in-parameter, it is like a load.
      case ParameterConvention::Indirect_In_CXX:
      case ParameterConvention::Indirect_In:
      case ParameterConvention::Indirect_In_Guaranteed:
        Uses.emplace_back(User, PMOUseKind::IndirectIn);
        continue;

      // If this is an @inout parameter, it is like both a load and store.
      case ParameterConvention::Indirect_Inout:
      case ParameterConvention::Indirect_InoutAliasable: {
        // If we're in the initializer for a struct, and this is a call to a
        // mutating method, we model that as an escape of self.  If an
        // individual sub-member is passed as inout, then we model that as an
        // inout use.
        Uses.emplace_back(User, PMOUseKind::InOutUse);
        continue;
      }
      }
      llvm_unreachable("bad parameter convention");
    }

    // init_existential_addr is modeled as an initialization store.
    if (isa<InitExistentialAddrInst>(User)) {
      // init_existential_addr should not apply to struct subelements.
      if (InStructSubElement) {
        return false;
      }
      Uses.emplace_back(User, PMOUseKind::Initialization);
      continue;
    }

    // open_existential_addr is a use of the protocol value,
    // so it is modeled as a load.
    if (isa<OpenExistentialAddrInst>(User)) {
      Uses.emplace_back(User, PMOUseKind::Load);
      // TODO: Is it safe to ignore all uses of the open_existential_addr?
      continue;
    }

    // We model destroy_addr as a release of the entire value.
    if (isa<DestroyAddrInst>(User)) {
      Releases.push_back(User);
      continue;
    }

    if (isa<DeallocStackInst>(User)) {
      continue;
    }

    // Sanitizer instrumentation is not user visible, so it should not
    // count as a use and must not affect compile-time diagnostics.
    if (isSanitizerInstrumentation(User))
      continue;

    // We don't care about debug instructions.
    if (User->isDebugInstruction())
      continue;

    // Otherwise, the use is something complicated, it escapes.
    Uses.emplace_back(User, PMOUseKind::Escape);
  }

  // Now that we've walked all of the immediate uses, scalarize any operations
  // working on tuples if we need to for canonicalization or analysis reasons.
  if (!UsesToScalarize.empty()) {
    SILInstruction *PointerInst = Pointer->getDefiningInstruction();
    SmallVector<SILValue, 4> ElementAddrs;
    SILBuilderWithScope AddrBuilder(++SILBasicBlock::iterator(PointerInst),
                                    PointerInst);
    getScalarizedElementAddresses(Pointer, AddrBuilder, PointerInst->getLoc(),
                                  ElementAddrs);

    SmallVector<SILValue, 4> ElementTmps;
    for (auto *User : UsesToScalarize) {
      ElementTmps.clear();

      LLVM_DEBUG(llvm::errs() << "  *** Scalarizing: " << *User << "\n");

      // Scalarize LoadInst
      if (auto *LI = dyn_cast<LoadInst>(User)) {
        SILValue Result = scalarizeLoad(LI, ElementAddrs);
        LI->replaceAllUsesWith(Result);
        LI->eraseFromParent();
        continue;
      }

      // Scalarize LoadBorrowInst
      if (auto *LBI = dyn_cast<LoadBorrowInst>(User)) {
        scalarizeLoadBorrow(LBI, ElementAddrs);
        continue;
      }

      // Scalarize StoreInst
      if (auto *SI = dyn_cast<StoreInst>(User)) {
        SILBuilderWithScope B(User, SI);
        B.emitDestructureValueOperation(
            SI->getLoc(), SI->getSrc(),
            [&](unsigned index, SILValue v) { ElementTmps.push_back(v); });
        for (unsigned i = 0, e = ElementAddrs.size(); i != e; ++i)
          B.createTrivialStoreOr(SI->getLoc(), ElementTmps[i], ElementAddrs[i],
                                 SI->getOwnershipQualifier(),
                                 true /*supports unqualified*/);
        SI->eraseFromParent();
        continue;
      }

      // Scalarize CopyAddrInst.
      auto *CAI = cast<CopyAddrInst>(User);
      SILBuilderWithScope B(User, CAI);

      // Determine if this is a copy *from* or *to* "Pointer".
      if (CAI->getSrc() == Pointer) {
        // Copy from pointer.
        getScalarizedElementAddresses(CAI->getDest(), B, CAI->getLoc(),
                                      ElementTmps);
        for (unsigned i = 0, e = ElementAddrs.size(); i != e; ++i)
          B.createCopyAddr(CAI->getLoc(), ElementAddrs[i], ElementTmps[i],
                           CAI->isTakeOfSrc(), CAI->isInitializationOfDest());

      } else {
        getScalarizedElementAddresses(CAI->getSrc(), B, CAI->getLoc(),
                                      ElementTmps);
        for (unsigned i = 0, e = ElementAddrs.size(); i != e; ++i)
          B.createCopyAddr(CAI->getLoc(), ElementTmps[i], ElementAddrs[i],
                           CAI->isTakeOfSrc(), CAI->isInitializationOfDest());
      }
      CAI->eraseFromParent();
    }

    // Now that we've scalarized some stuff, recurse down into the newly created
    // element address computations to recursively process it.  This can cause
    // further scalarization.
    if (llvm::any_of(ElementAddrs, [&](SILValue v) {
          return !collectUses(cast<TupleElementAddrInst>(v));
        })) {
      return false;
    }
  }

  return true;
}

/// collectPMOElementUsesFrom - Analyze all uses of the specified allocation
/// instruction (alloc_box, alloc_stack or mark_uninitialized), classifying them
/// and storing the information found into the Uses and Releases lists.
bool swift::collectPMOElementUsesFrom(
    const PMOMemoryObjectInfo &MemoryInfo, SmallVectorImpl<PMOMemoryUse> &Uses,
    SmallVectorImpl<SILInstruction *> &Releases) {
  return ElementUseCollector(MemoryInfo, Uses, Releases).collectFrom();
}
